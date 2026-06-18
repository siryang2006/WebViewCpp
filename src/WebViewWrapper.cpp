#include "WebViewWrapper.h"
#include <webview/webview.h>

#include <sstream>
#include <chrono>

#ifdef _WIN32
#define UNICODE
#define _UNICODE
#include <windows.h>

// Win32 辅助：获取窗口 DPI。
static int get_window_dpi(HWND hwnd) {
    UINT dpi = GetDpiForWindow(hwnd);
    return dpi ? dpi : 96;
}

// Win32 辅助：计算 contentRect(不含边框) → 加上标题栏和边框后的 frame size。
// 与 webview 内部 make_window_frame_size 逻辑一致。
// hwnd 用于查询 DPI；style 是已知的窗口风格。
static SIZE content_to_frame(HWND hwnd, DWORD style, int cx, int cy) {
    RECT r{0, 0, cx, cy};
    using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static AdjustWindowRectExForDpiFn adjFn =
        (AdjustWindowRectExForDpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                    "AdjustWindowRectExForDpi");
    int dpi = hwnd ? get_window_dpi(hwnd) : 96;
    if (adjFn) {
        adjFn(&r, style, FALSE, 0, (UINT)dpi);
    } else {
        AdjustWindowRect(&r, style, FALSE);
    }
    return {r.right - r.left, r.bottom - r.top};
}

// Win32 窗口过程：只处理 WM_SIZE（把 widget 填满 client area）和 WM_CLOSE/WM_DESTROY。
// 所有其他消息走 DefWindowProc。
static LRESULT CALLBACK containing_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            // 窗口 client area 变化时，把 widget 子窗口填满它。
            // webview_get_native_handle(WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET) 在 embed()
            // 完成前不可用（返回 null），所以这里用 FindWindow 找已存在的 widget。
            HWND widget = ::FindWindowExW(hwnd, nullptr, L"webview_widget", nullptr);
            if (widget) {
                RECT r;
                GetClientRect(hwnd, &r);
                SetWindowPos(widget, nullptr, 0, 0, r.right, r.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
#endif // _WIN32

// ============================================================
// CppObject::invoke_async
// ============================================================
void JsCallback::resolve(const json& result) {
    if (m_wv) m_wv->resolve(m_id, result);
}
void JsCallback::reject(const std::string& error) {
    if (m_wv) m_wv->reject(m_id, error);
}

// ============================================================
// CppObject::invoke_async (forward declared in CppObject.h)
// ============================================================
void CppObject::invoke_async(const std::string& method, const std::string& id,
                              const json& args, WebViewWrapper* wv) {
    if (m_destroyed.load()) {
        if (wv) wv->reject(id, "Object destroyed");
        return;
    }
    auto it = m_async_methods.find(method);
    if (it == m_async_methods.end()) {
        if (wv) wv->reject(id, "Async method not found: " + method);
        return;
    }
    try {
        it->second(id, args, wv);
    } catch (const std::exception& e) {
        if (wv) wv->reject(id, e.what());
    }
}

// ============================================================
// WebViewWrapper 构造/析构
// ============================================================

WebViewWrapper::WebViewWrapper() {}

WebViewWrapper::~WebViewWrapper() {
    terminate();

    // 停止任务派发线程
    {
        std::lock_guard<std::mutex> lock(m_task_mutex);
        m_task_running.store(false);
    }
    m_task_cv.notify_all();
    if (m_dispatch_thread.joinable()) {
        m_dispatch_thread.join();
    }

    if (m_webview) {
        webview_destroy(m_webview);
        m_webview = nullptr;
    }

#ifdef _WIN32
    // 关闭 Job 句柄：正常退出时这是兜底，触发 KILL_ON_JOB_CLOSE 终止残余子进程。
    if (m_job) {
        ::CloseHandle(static_cast<HANDLE>(m_job));
        m_job = nullptr;
    }
#endif
}

// ============================================================
// 初始化
// ============================================================
bool WebViewWrapper::init(const std::string& title, const std::string& url,
                          int width, int height, bool resizable,
                          int debug_port) {
#ifdef _WIN32
    // 创建 Job Object 并把当前（宿主）进程纳入，设置 KILL_ON_JOB_CLOSE：
    // 宿主进程退出时，内核自动终止 Job 内所有进程——包括 webview_create
    // 之后由 WebView2 派生的 msedgewebview2.exe 子进程，杜绝孤儿残留。
    if (HANDLE job = ::CreateJobObjectW(nullptr, nullptr)) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                  &info, sizeof(info));
        ::AssignProcessToJobObject(job, ::GetCurrentProcess());
        m_job = job; // Job 句柄随进程生命周期持有，进程退出即关闭触发清理
    }

    // WebView2 基于 Chromium，架构上无法单进程（--single-process 官方不支持，会崩溃）。
    // 用命令行参数把子进程压到最少：关站点隔离 + 渲染进程限 1 + GPU 内联到 browser 进程。
    std::string browser_args =
        "--disable-features=site-per-process,IsolateOrigins,RendererCodeIntegrity"
        " --renderer-process-limit=1"
        " --in-process-gpu";
    if (debug_port > 0) {
        browser_args += " --remote-debugging-port=" + std::to_string(debug_port);
    }
    SetEnvironmentVariableA("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", browser_args.c_str());

    // 创建外层容器窗口，计算含边框的 frame size：
    // 这样 webview 嵌入后从第一帧起就是正确尺寸，不会有 CW_USEDEFAULT 小窗口跳变。
    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    if (!resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    SIZE frame = content_to_frame(nullptr, style, width, height);
    HWND hwnd = ::CreateWindowExW(0, L"STATIC", L"", style,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  frame.cx, frame.cy,
                                  nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return false;
    // 用我们的 wndproc 替换默认 Static proc。
    ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(containing_wndproc));
    ::SetWindowTextW(hwnd, L"Loading...");
    m_containing_window = hwnd;

    // 传入我们的窗口，让 webview 托管（m_owns_window=false）。
    // 这样 webview 不会走 CW_USEDEFAULT 自建路径，也不会在 embed() 期间 ShowWindow。
    m_webview = webview_create(1, hwnd);
    if (!m_webview) {
        ::DestroyWindow(hwnd);
        m_containing_window = nullptr;
        return false;
    }

    // embed() 阻塞期间窗口保持隐藏；返回后设置标题，再显示。
    // 注意：webview 不会帮我们设标题（因为 m_owns_window=false），
    // 所以标题要在这里单独设置。
    webview_set_title(m_webview, title.c_str());
#else
    m_webview = webview_create(1, nullptr);
    if (!m_webview) return false;
    webview_set_title(m_webview, title.c_str());
    webview_set_size(m_webview, width, height,
                     resizable ? WEBVIEW_HINT_NONE : WEBVIEW_HINT_FIXED);
#endif

    m_gui_thread_id = std::this_thread::get_id();

#ifdef _WIN32
    // 在显示窗口前，先把 widget 填满 client area。
    // embed() 返回后 widget 已经创建，直接填满然后才 ShowWindow。
    HWND widget = ::FindWindowExW(static_cast<HWND>(m_containing_window), nullptr, L"webview_widget", nullptr);
    if (widget) {
        RECT r;
        ::GetClientRect(static_cast<HWND>(m_containing_window), &r);
        ::SetWindowPos(widget, nullptr, 0, 0, r.right, r.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ::ShowWindow(static_cast<HWND>(m_containing_window), SW_SHOW);
    ::UpdateWindow(static_cast<HWND>(m_containing_window));
#endif

    if (!setup_js_bridge()) {
        webview_destroy(m_webview);
        m_webview = nullptr;
        return false;
    }
    start_dispatch_loop();

    if (!url.empty()) {
        webview_navigate(m_webview, url.c_str());
    }

    return true;
}

// ============================================================
// 主循环
// ============================================================
void WebViewWrapper::run() {
#ifdef _WIN32
    if (m_containing_window) {
        ::SetForegroundWindow(static_cast<HWND>(m_containing_window));
    }
#endif
    webview_run(m_webview);
}

void WebViewWrapper::terminate() {
    if (m_webview && !m_terminated.exchange(true)) {
        webview_terminate(m_webview);

        // 拒绝所有未决的 C++→JS 回调，避免调用方永久等待。
        // （JS 端的 __pending_callbacks__ 随页面销毁，无需处理）
        std::map<std::string, PendingCallback> pending;
        {
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            pending.swap(m_pending_callbacks);
        }
        for (auto& [id, cb] : pending) {
            if (cb.on_error) cb.on_error("WebView terminated");
        }

#ifdef _WIN32
        // 销毁我们自建的窗口（m_owns_window=false，所以 webview_destroy 不会碰它）。
        if (m_containing_window) {
            ::DestroyWindow(static_cast<HWND>(m_containing_window));
            m_containing_window = nullptr;
        }
#endif
    }
}

// ============================================================
// 导航和渲染
// ============================================================
void WebViewWrapper::navigate(const std::string& url) {
    webview_navigate(m_webview, url.c_str());
}

void WebViewWrapper::set_html(const std::string& html) {
    webview_set_html(m_webview, html.c_str());
}

void WebViewWrapper::set_title(const std::string& title) {
    webview_set_title(m_webview, title.c_str());
}

void WebViewWrapper::eval(const std::string& js) {
    webview_eval(m_webview, js.c_str());
}

// ============================================================
// 线程安全 eval
// ============================================================
void WebViewWrapper::post_eval(const std::string& js) {
    if (!m_webview || m_terminated.load()) return;

    {
        std::lock_guard<std::mutex> lock(m_eval_mutex);
        m_eval_queue.push(js);
    }

    // 防止重复 dispatch
    if (!m_eval_pending.exchange(true)) {
        webview_dispatch(m_webview, [](webview_t, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            self->process_eval_queue();
        }, this);
    }
}

// 持久化注入：脚本同时注册为 user script（跨导航存活，
// 解决"先 bind 后 set_html/navigate 导致注入被丢弃"的时序竞态），
// 并 post_eval 到当前页（覆盖运行时绑定时已加载的页面）。
//
// webview_init（底层 AddScriptToExecuteOnDocumentCreated）会 pump 消息循环且
// 无内部同步，必须在 GUI 线程调用。若调用方在其它线程（如派发线程上运行时
// bind_object），则把 webview_init 调度到 GUI 线程执行。post_eval 本身线程安全。
void WebViewWrapper::inject_persistent(const std::string& js) {
    if (!m_webview || m_terminated.load()) return;

    if (std::this_thread::get_id() == m_gui_thread_id) {
        webview_init(m_webview, js.c_str());
    } else {
        // 堆分配脚本副本，所有权转移给 GUI 线程回调，执行后释放。
        auto* payload = new std::string(js);
        webview_dispatch(m_webview, [](webview_t w, void* arg) {
            std::unique_ptr<std::string> script(static_cast<std::string*>(arg));
            webview_init(w, script->c_str());
        }, payload);
    }
    post_eval(js);
}

void WebViewWrapper::process_eval_queue() {
    // 标记 dispatch 已处理
    m_eval_pending.store(false);

    std::queue<std::string> batch;
    {
        std::lock_guard<std::mutex> lock(m_eval_mutex);
        std::swap(batch, m_eval_queue);
    }
    while (!batch.empty()) {
        webview_eval(m_webview, batch.front().c_str());
        batch.pop();
    }

    // 如果队列还有新项，继续 dispatch
    {
        std::lock_guard<std::mutex> lock(m_eval_mutex);
        if (!m_eval_queue.empty() && !m_eval_pending.exchange(true)) {
            webview_dispatch(m_webview, [](webview_t, void* arg) {
                auto* self = static_cast<WebViewWrapper*>(arg);
                self->process_eval_queue();
            }, this);
        }
    }
}

// ============================================================
// 任务派发线程（替代 detach）
// ============================================================
void WebViewWrapper::dispatch_task(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(m_task_mutex);
        if (!m_task_running.load()) return;
        m_task_queue.push(std::move(fn));
    }
    m_task_cv.notify_one();
}

void WebViewWrapper::start_dispatch_loop() {
    m_task_running.store(true);
    m_dispatch_thread = std::thread(dispatch_loop_static, this);
}

void WebViewWrapper::dispatch_loop_static(WebViewWrapper* self) {
    self->start_dispatch_loop_internal();
}

void WebViewWrapper::start_dispatch_loop_internal() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_task_mutex);
            m_task_cv.wait(lock, [this]() {
                return !m_task_queue.empty() || !m_task_running.load();
            });
            if (!m_task_running.load() && m_task_queue.empty()) break;
            task = std::move(m_task_queue.front());
            m_task_queue.pop();
        }
        if (task) {
            try {
                task();
            } catch (...) {
                // 捕获所有异常，防止线程退出崩溃
            }
        }
    }
}

// ============================================================
// 对象绑定/解绑
// ============================================================
void WebViewWrapper::bind_object(std::shared_ptr<CppObject> obj) {
    {
        std::lock_guard<std::mutex> lock(m_objects_mutex);
        m_objects[obj->object_name()] = obj;
    }
    obj->on_created();
    inject_single_object(obj, "");
}

void WebViewWrapper::unbind_object(const std::string& name) {
    std::shared_ptr<CppObject> obj;
    {
        std::lock_guard<std::mutex> lock(m_objects_mutex);
        auto it = m_objects.find(name);
        if (it == m_objects.end()) return;
        obj = it->second;
        m_objects.erase(it);
    }

    // 用持久脚本删除：bind_object 注册的 define 是持久 user script，跨导航会重放，
    // 仅 post_eval 删除当前页无法阻止下次导航重建对象。把 delete 也注册为持久脚本，
    // user script 按注册顺序执行，delete 晚于 define，因此每次文档加载后对象都被移除；
    // 若之后再次 bind 同名对象，新 define 注册更晚，会重新覆盖（last-writer-wins）。
    std::string js = "if (window.__cpp__ && ('" + name + "' in window.__cpp__)) { delete window.__cpp__." + name + "; }\n";
    inject_persistent(js);
    obj->mark_destroyed();
}

void WebViewWrapper::bind_factory(const std::string& type_name, ObjectFactory factory, FactoryMode mode) {
    {
        std::lock_guard<std::mutex> lock(m_objects_mutex);
        m_factories[type_name] = { std::move(factory), mode };
    }

    if (mode == FactoryMode::Instance) {
        // ===== 实例模式：JS 异步工厂，创建 C++ 实例并返回 Proxy =====
        // NOTE: webview_bind 返回 Promise，因此必须用 async/await
        std::ostringstream js;
        js << "(function() {\n"
           << "  window.__cpp__ = window.__cpp__ || {};\n"
           << "  var __gc_reg__ = new FinalizationRegistry(function(iid) {\n"
           << "    Promise.resolve(window.__webview_destroy__(iid)).catch(function(){});\n"
           << "  });\n"
           << "  var makeProxy = function(info) {\n"
           << "    var iid = info.id;\n"
           << "    var syncSet = {}; (info.sync||[]).forEach(function(n){ syncSet[n]=1; });\n"
           << "    var asyncSet = {}; (info.async||[]).forEach(function(n){ asyncSet[n]=1; });\n"
           << "    var propSet = {}; (info.props||[]).forEach(function(n){ propSet[n]=1; });\n"
           << "    var self = { __id__: iid, __type__: info.type, __destroyed__: false };\n"
           << "    var proxy = new Proxy(self, {\n"
           << "      get: function(target, prop) {\n"
           << "        if (prop === '__destroy__') {\n"
           << "          return function() {\n"
           << "            if (target.__destroyed__) return;\n"
           << "            target.__destroyed__ = true;\n"
           << "            __gc_reg__.unregister(target.__token__);\n"
           << "            Promise.resolve(window.__webview_destroy__(target.__id__)).catch(function(){});\n"
           << "          };\n"
           << "        }\n"
           << "        if (prop in target) return target[prop];\n"
           << "        // 关键：proxy 不能表现为 thenable，否则 async 工厂 return proxy\n"
           << "        // 或 await proxy 时，JS 会调用 proxy.then(resolve,reject) 来\"解包\"，\n"
           << "        // 而方法代理返回的函数不会回调 resolve/reject，导致 await 永久挂起。\n"
           << "        if (prop === 'then' || prop === 'catch' || prop === 'finally') return undefined;\n"
           << "        if (typeof prop === 'symbol') return undefined;\n"
           << "        // 属性：通过 get_property 读取（返回 Promise）\n"
           << "        if (propSet[prop]) return window.__webview_get_property__(iid, prop);\n"
           << "        // 异步方法：通过 async 桥接，匹配 __pending_callbacks__ 协议\n"
           << "        if (asyncSet[prop]) {\n"
           << "          return function() {\n"
           << "            var mArgs = Array.prototype.slice.call(arguments);\n"
           << "            var cb_fn = null;\n"
           << "            if (typeof mArgs[mArgs.length-1] === 'function') { cb_fn = mArgs.pop(); }\n"
           << "            return new Promise(function(resolve, reject) {\n"
           << "              var cid = window.__next_cb_id__();\n"
           << "              window.__pending_callbacks__ = window.__pending_callbacks__ || {};\n"
           << "              window.__pending_callbacks__[cid] = cb_fn ? null : { resolve: resolve, reject: reject };\n"
           << "              if (cb_fn) { window.__store_js_fn__(cid, cb_fn); }\n"
           << "              var req = JSON.stringify({ args: mArgs, id: cid, has_cb: !!cb_fn });\n"
           << "              window.__webview_async_call__(iid, prop, req);\n"
           << "            });\n"
           << "          };\n"
           << "        }\n"
           << "        // 其余按同步方法处理（含未知名字，让 C++ 端报 Method not found）\n"
            << "        return async function() {\n"
            << "          var mArgs = Array.prototype.slice.call(arguments);\n"
            << "          var mReq = JSON.stringify(mArgs);\n"
            << "          return await window.__webview_sync_call__(iid, prop, mReq);\n"
            << "        };\n"
           << "      }\n"
           << "    });\n"
           << "    var token = {};\n"
           << "    self.__token__ = token;\n"
           << "    __gc_reg__.register(proxy, iid, token);\n"
           << "    return proxy;\n"
           << "  };\n"
           << "  window.__cpp__." << type_name << " = async function() {\n"
           << "    var args = Array.prototype.slice.call(arguments);\n"
            << "    var info = await window.__webview_create__(\"" << type_name << "\", JSON.stringify(args));\n"
           << "    if (!info.id) throw new Error(info.error || 'Create failed');\n"
           << "    return makeProxy(info);\n"
           << "  };\n"
           << "})();\n";
        inject_persistent(js.str());
    } else {
        // ===== 全局模式：JS 直接调用函数，C++ 使用全局单例 =====
        // factory 在锁外调用：用户工厂可能回调入 wrapper，持锁会死锁。
        ObjectFactory factory_copy;
        {
            std::lock_guard<std::mutex> lock(m_objects_mutex);
            factory_copy = m_factories[type_name].factory;
        }
        auto obj = factory_copy(json::array());
        {
            std::lock_guard<std::mutex> lock(m_objects_mutex);
            m_objects[type_name] = obj;
        }
        obj->on_created();
        inject_single_object(obj, "");
    }
}

void WebViewWrapper::inject_single_object(std::shared_ptr<CppObject> obj, const std::string& instance_id) {
    const std::string& js_name = instance_id.empty() ? obj->object_name() : instance_id;

    // 用 JSON.stringify 将标识符转成 JS 字符串字面量，避免特殊字符注入
    std::ostringstream js;
    js << "window.__cpp__ = window.__cpp__ || {};\n";
    js << "window.__cpp__[" << json(js_name).dump() << "] = {};\n";

    // 同步方法（webview bindings 返回 Promise，需 await）
    for (auto& [method_name, _] : obj->sync_methods()) {
        js << "window.__cpp__[" << json(js_name).dump() << "][" << json(method_name).dump() << "]"
           << " = async function() {\n"
           << "  var args = Array.prototype.slice.call(arguments);\n"
           << "  var req = JSON.stringify(args);\n"
           << "  return await window.__webview_sync_call__("
           << json(js_name).dump() << "," << json(method_name).dump() << ", req);\n"
           << "};\n";
    }

    // 异步方法（支持两种调用方式）：
    //   obj.asyncMethod(args)         -> 返回 Promise
    //   obj.asyncMethod(args, cb)    -> 检测末尾参数为函数，直接调用 cb(err, result)
    for (auto& [method_name, _] : obj->async_methods()) {
        js << "window.__cpp__[" << json(js_name).dump() << "][" << json(method_name).dump() << "]"
           << " = function() {\n"
           << "  var args = Array.prototype.slice.call(arguments);\n"
           << "  var cb_fn = null;\n"
           << "  if (typeof args[args.length-1] === 'function') { cb_fn = args.pop(); }\n"
           << "  return new Promise(function(resolve, reject) {\n"
           << "    var id = window.__next_cb_id__();\n"
           << "    window.__pending_callbacks__ = window.__pending_callbacks__ || {};\n"
           << "    window.__pending_callbacks__[id] = cb_fn ? null : { resolve: resolve, reject: reject };\n"
           << "    if (cb_fn) { window.__store_js_fn__(id, cb_fn); }\n"
           << "    var req = JSON.stringify({ args: args, id: id, has_cb: !!cb_fn });\n"
           << "    window.__webview_async_call__("
           << json(js_name).dump() << "," << json(method_name).dump() << ", req);\n"
           << "  });\n"
           << "};\n";
    }

    // 属性（webview bindings 返回 Promise，需 await）
    for (auto& prop_name : obj->property_names()) {
        js << "Object.defineProperty(window.__cpp__[" << json(js_name).dump() << "], " << json(prop_name).dump() << ", {\n"
           << "  get: async function() {\n"
           << "    return await window.__webview_get_property__("
           << json(js_name).dump() << "," << json(prop_name).dump() << ");\n"
           << "  },\n";
        if (obj->has_setter(prop_name)) {
            js << "  set: function(v) {\n"
               << "    window.__webview_set_property__("
               << json(js_name).dump() << "," << json(prop_name).dump() << ", JSON.stringify(v));\n"
               << "  },\n";
        } else {
            js << "  set: function(v) { throw new Error('Property is read-only'); },\n";
        }
        js << "  enumerable: true\n});\n";
    }

    // 注入销毁方法
    js << "window.__cpp__[" << json(js_name).dump() << "].__destroy__ = function() {\n"
       << "  return window.__webview_destroy__(" << json(js_name).dump() << ");\n"
       << "};\n";

    inject_persistent(js.str());
}

// ============================================================
// 回调机制（线程安全）
// ============================================================
void WebViewWrapper::resolve(const std::string& id, const json& result) {
    if (m_terminated.load() || !m_webview) return;

    // 锁内查找，然后决定路径
    bool is_js_cb = false;
    std::string js;
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        auto it = m_pending_callbacks.find(id);
        if (it == m_pending_callbacks.end()) return;
        is_js_cb = it->second.is_js_callback;
        m_pending_callbacks.erase(it);
    }

    if (is_js_cb) {
        // JS 函数回调：调用 JS 端存储的函数，Node.js convention: cb(null, result)
        std::string json_str = result.dump(-1, ' ', /*ensure_ascii=*/true);
        js = "(function(){"
            "var fn=window.__js_callbacks__['" + id + "'];"
            "if(typeof fn==='function'){fn(null," + json_str + ");delete window.__js_callbacks__['" + id + "'];}"
            "})();";
    } else {
        // Promise 回调：resolve Promise
        std::string json_str = result.dump(-1, ' ', /*ensure_ascii=*/true);
        js = "(function(){"
            "var cbs=window.__pending_callbacks__||{};"
            "var cb=cbs['" + id + "'];"
            "if(cb){cb.resolve(" + json_str + ");delete cbs['" + id + "'];}"
            "})();";
    }
    post_eval(js);
}

void WebViewWrapper::reject(const std::string& id, const std::string& error) {
    if (m_terminated.load() || !m_webview) return;

    bool is_js_cb = false;
    std::string js;
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        auto it = m_pending_callbacks.find(id);
        if (it == m_pending_callbacks.end()) return;
        is_js_cb = it->second.is_js_callback;
        m_pending_callbacks.erase(it);
    }

    if (is_js_cb) {
        // JS 函数回调：cb(error_message, null)
        js = "(function(){"
            "var fn=window.__js_callbacks__['" + id + "'];"
            "if(typeof fn==='function'){fn(" + json(error).dump() + ",null);delete window.__js_callbacks__['" + id + "'];}"
            "})();";
    } else {
        std::string escaped = CppObject::js_escape(error);
        js = "(function(){"
            "var cbs=window.__pending_callbacks__||{};"
            "var cb=cbs['" + id + "'];"
            "if(cb){cb.reject(new Error('" + escaped + "'));delete cbs['" + id + "'];}"
            "})();";
    }
    post_eval(js);
}

void WebViewWrapper::call_js(const std::string& func_name, const json& args,
                             std::function<void(const json&)> on_result,
                             std::function<void(const std::string&)> on_error,
                             int timeout_ms) {
    if (m_terminated.load() || !m_webview) {
        if (on_error) on_error("WebView not ready");
        return;
    }

    std::string id = std::to_string(next_unique_id());

    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_pending_callbacks[id] = { std::move(on_result), std::move(on_error), false };
    }

    // 超时清理
    if (timeout_ms > 0) {
        dispatch_task([this, id, timeout_ms]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            auto it = m_pending_callbacks.find(id);
            if (it != m_pending_callbacks.end()) {
                if (it->second.on_error)
                    it->second.on_error("Timeout");
                m_pending_callbacks.erase(it);
            }
        });
    }

    std::ostringstream js;
    js << "(function(){\n"
       << "  try {\n"
       << "    var result = window[" << json(func_name).dump() << "](" << args.dump(-1, ' ', true) << ");\n"
       << "    if (result && typeof result.then === 'function') {\n"
       << "      result.then(function(r) {\n"
       << "        window.__cpp_result__('" << id << "', true, JSON.stringify(r === undefined ? null : r));\n"
       << "      }).catch(function(e) {\n"
       << "        window.__cpp_result__('" << id << "', false, JSON.stringify(e.message || String(e)));\n"
       << "      });\n"
       << "    } else {\n"
       << "      window.__cpp_result__('" << id << "', true, JSON.stringify(result === undefined ? null : result));\n"
       << "    }\n"
       << "  } catch(e) {\n"
       << "    window.__cpp_result__('" << id << "', false, JSON.stringify(e.message || String(e)));\n"
       << "  }\n"
       << "})();";
    post_eval(js.str());
}

void WebViewWrapper::call_registered_js(const std::string& name, const json& args) {
    // 回调存于 __registered_cbs__（由 __register_cb__ 注册），而非 window 顶层。
    post_eval(
        "(function(){var fn=(window.__registered_cbs__||{})[" + json(name).dump() + "];"
        "if(typeof fn==='function'){fn(" + args.dump(-1, ' ', true) + ");}})();"
    );
}

// ============================================================
// JS Bridge
// ============================================================
//
// 根本原因（CDP 测试 7 个 binding undefined 的 bug）：
//   webview 库每次 webview_bind 都调用 create_bind_script()，生成一段遍历所有 binding
//   名调用 window.__webview__.onBind(name) 的脚本。onBind 在 window[name] 已存在时会
//   throw。因 bindings 是 std::map（按名字字典序遍历），当循环执行到一个名字已被
//   webview_init 脚本预先定义的 binding 时，onBind 抛异常并中止整个 forEach 循环，
//   导致字典序在其后的所有 binding 的 JS 包装函数都未注入 window。
//
//   触发点：__store_js_fn__ 同时在 webview_init 脚本和 webview_bind 中定义。字典序中
//   __store_js_fn__ 在 __webview_* 之前，故 __webview_sync_call__ 等全部失效，而
//   __cpp_result__（字典序更靠前）幸存。
//
// 修复：__store_js_fn__ 只保留 webview_init 中的纯 JS 版本（直接存函数对象到
//   __js_callbacks__）。原 webview_bind 版本试图把 JS 函数 JSON 序列化再 eval 回来，
//   本就不可行，删除它同时消除了名字冲突。其余 binding 名均不与 init 脚本冲突。
//
bool WebViewWrapper::setup_js_bridge() {
    // webview_init 注册基础 JS 对象（user script，跨导航存活）
    webview_error_t init_err = webview_init(m_webview,
        "window.__pending_callbacks__ = {};\n"
        "window.__registered_cbs__ = {};\n"
        "window.__js_callbacks__ = {};\n"
        "window.__cb_seq__ = 0;\n"
        "window.__next_cb_id__ = function() {\n"
        "  return '__cb_' + (++window.__cb_seq__) + '_' + Math.random().toString(36).slice(2, 11);\n"
        "};\n"
        "window.__register_cb__ = function(name, fn) { window.__registered_cbs__[name] = fn; };\n"
        "window.__store_js_fn__ = function(id, fn) { window.__js_callbacks__[id] = fn; };\n"
        "window.__call_js_fn__ = function(id, args) {\n"
        "  var fn = window.__js_callbacks__[id];\n"
        "  if (typeof fn !== 'function') throw new Error('JS callback not found: ' + id);\n"
        "  var result = fn.apply(null, args);\n"
        "  return result;\n"
        "};\n"
        "window.__delete_js_fn__ = function(id) { delete window.__js_callbacks__[id]; };\n"
    );
    if (init_err != WEBVIEW_ERROR_OK) {
        return false;
    }

    // C++ 调用 JS 结果回调（JS→C++ 方向）
    webview_bind(m_webview, "__cpp_result__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            try {
                auto data = json::parse(req);
                std::string id = data[0].get<std::string>();
                bool success = data[1].get<bool>();
                std::string value = data[2].get<std::string>();

                // 锁内取出回调并 erase，解锁后再调用 —— 用户回调可能回调进
                // WebViewWrapper（如再次 call_js），持锁调用会死锁。
                PendingCallback cb;
                bool found = false;
                {
                    std::lock_guard<std::mutex> lock(self->m_callback_mutex);
                    auto it = self->m_pending_callbacks.find(id);
                    if (it != self->m_pending_callbacks.end()) {
                        cb = std::move(it->second);
                        self->m_pending_callbacks.erase(it);
                        found = true;
                    }
                }
                if (found) {
                    if (success) {
                        if (cb.on_result) cb.on_result(json::parse(value));
                    } else {
                        if (cb.on_error) cb.on_error(value);
                    }
                }
            } catch (...) {
                // 吞掉所有异常，避免穿透到 webview C 回调栈
            }
            webview_return(self->m_webview, seq, 0, "");
        }, this);

    // 注意：__store_js_fn__ 不在此 webview_bind。它是纯 JS 操作（直接把函数对象存进
    // __js_callbacks__），已在上方 webview_init 脚本中定义。JS 函数无法 JSON 序列化跨
    // 边界传给 C++，故不存在对应的 C++ 回调。

    // 同步调用
    webview_bind(m_webview, "__webview_sync_call__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            try {
                auto data = json::parse(req);
                std::string obj_name = data[0].get<std::string>();
                std::string method = data[1].get<std::string>();
                json args;
                if (data[2].is_string()) {
                    args = json::parse(data[2].get<std::string>());
                } else {
                    args = data[2];
                }

                // 锁内查找，解锁后调用 — 避免用户 invoke_sync 回调持同一把锁导致死锁
                std::shared_ptr<CppObject> obj;
                {
                    std::lock_guard<std::mutex> lock(self->m_objects_mutex);
                    auto it = self->m_objects.find(obj_name);
                    if (it == self->m_objects.end()) {
                        it = self->m_dynamic_instances.find(obj_name);
                        if (it == self->m_dynamic_instances.end())
                            throw BindingException(ErrorCode::OBJECT_NOT_FOUND, "Object not found: " + obj_name);
                    }
                    obj = it->second;
                }
                json result = obj->invoke_sync(method, args);
                webview_return(self->m_webview, seq, 0, result.dump().c_str());
            } catch (const std::exception& e) {
                webview_return(self->m_webview, seq, 1,
                    BindingException::from(e).to_json().dump().c_str());
            }
        }, this);

    // 异步调用
    webview_bind(m_webview, "__webview_async_call__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            try {
                auto data = json::parse(req);
                std::string obj_name = data[0].get<std::string>();
                std::string method = data[1].get<std::string>();
                auto call_data = json::parse(data[2].get<std::string>());
                std::string id = call_data["id"].get<std::string>();
                json args = call_data["args"];
                bool has_cb = call_data.value("has_cb", false);

                std::shared_ptr<CppObject> obj;
                {
                    std::lock_guard<std::mutex> lock(self->m_objects_mutex);
                    auto it = self->m_objects.find(obj_name);
                    if (it != self->m_objects.end()) {
                        obj = it->second;
                    } else {
                        it = self->m_dynamic_instances.find(obj_name);
                        if (it != self->m_dynamic_instances.end())
                            obj = it->second;
                    }
                }

                // 先注册 pending callback —— reject()/resolve() 都依赖 m_pending_callbacks
                // 里存在该 id 才能定位 is_js_cb 并向 JS 端发出回调。若对象未找到时直接
                // reject 而 id 未注册，reject 会 find 失败静默返回，JS 端 Promise 永久挂起。
                {
                    std::lock_guard<std::mutex> lock(self->m_callback_mutex);
                    self->m_pending_callbacks[id] = { nullptr, nullptr, has_cb };
                }

                if (!obj) {
                    self->reject(id, BindingException(ErrorCode::OBJECT_NOT_FOUND,
                        "Object not found: " + obj_name).to_json().dump());
                } else {
                    self->dispatch_task([obj, method, id, args, self]() {
                        obj->invoke_async(method, id, args, self);
                    });
                }
                webview_return(self->m_webview, seq, 0, "");
            } catch (const std::exception& e) {
                webview_return(self->m_webview, seq, 1,
                    BindingException::from(e).to_json().dump().c_str());
            }
        }, this);

    // 属性读取
    webview_bind(m_webview, "__webview_get_property__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            try {
                auto data = json::parse(req);
                std::string obj_name = data[0].get<std::string>();
                std::string prop = data[1].get<std::string>();

                std::lock_guard<std::mutex> lock(self->m_objects_mutex);
                auto it = self->m_objects.find(obj_name);
                if (it != self->m_objects.end()) {
                    // found
                } else {
                    it = self->m_dynamic_instances.find(obj_name);
                    if (it == self->m_dynamic_instances.end())
                        throw BindingException(ErrorCode::OBJECT_NOT_FOUND, "Object not found: " + obj_name);
                }
                json val = it->second->get_property(prop);
                webview_return(self->m_webview, seq, 0, val.dump().c_str());
            } catch (const std::exception& e) {
                webview_return(self->m_webview, seq, 1,
                    BindingException::from(e).to_json().dump().c_str());
            }
        }, this);

    // 属性设置
    webview_bind(m_webview, "__webview_set_property__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            try {
                auto data = json::parse(req);
                std::string obj_name = data[0].get<std::string>();
                std::string prop = data[1].get<std::string>();
                json val = json::parse(data[2].get<std::string>());

                std::lock_guard<std::mutex> lock(self->m_objects_mutex);
                auto it = self->m_objects.find(obj_name);
                if (it != self->m_objects.end()) {
                    // found
                } else {
                    it = self->m_dynamic_instances.find(obj_name);
                    if (it == self->m_dynamic_instances.end())
                        throw BindingException(ErrorCode::OBJECT_NOT_FOUND, "Object not found: " + obj_name);
                }
                it->second->set_property(prop, val);
                webview_return(self->m_webview, seq, 0, "");
            } catch (const std::exception& e) {
                webview_return(self->m_webview, seq, 1,
                    BindingException::from(e).to_json().dump().c_str());
            }
        }, this);

    // 创建动态实例
    webview_bind(m_webview, "__webview_create__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            try {
                auto data = json::parse(req);
                std::string type_name = data[0].get<std::string>();
                json args = json::parse(data[1].get<std::string>());

                std::shared_ptr<CppObject> obj;
                std::string instance_id;
                {
                    std::lock_guard<std::mutex> lock(self->m_objects_mutex);
                    auto fit = self->m_factories.find(type_name);
                    if (fit == self->m_factories.end()) {
                        webview_return(self->m_webview, seq, 1, json("Unknown type: " + type_name).dump().c_str());
                        return;
                    }
                    obj = fit->second.factory(args);
                    instance_id = type_name + "_" + std::to_string(obj->instance_id());
                    self->m_dynamic_instances[instance_id] = obj;
                }

                obj->on_created();
                json sync_names = json::array();
                for (auto& [n, _] : obj->sync_methods()) sync_names.push_back(n);
                json async_names = json::array();
                for (auto& [n, _] : obj->async_methods()) async_names.push_back(n);
                json prop_names = json::array();
                for (auto& n : obj->property_names()) prop_names.push_back(n);

                json result = {
                    {{"id", instance_id}},
                    {{"type", type_name}},
                    {{"sync", std::move(sync_names)}},
                    {{"async", std::move(async_names)}},
                    {{"props", std::move(prop_names)}}
                };
                webview_return(self->m_webview, seq, 0, result.dump().c_str());
            } catch (const std::exception& e) {
                webview_return(self->m_webview, seq, 1,
                    BindingException::from(e).to_json().dump().c_str());
            }
        }, this);

    // 销毁动态实例
    webview_bind(m_webview, "__webview_destroy__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            try {
                auto destroy_data = json::parse(req);
                std::string instance_id = destroy_data[0].get<std::string>();

                std::shared_ptr<CppObject> obj;
                {
                    std::lock_guard<std::mutex> lock(self->m_objects_mutex);
                    auto it = self->m_dynamic_instances.find(instance_id);
                    if (it != self->m_dynamic_instances.end()) {
                        obj = it->second;
                        self->m_dynamic_instances.erase(it);
                    } else {
                        auto sit = self->m_objects.find(instance_id);
                        if (sit != self->m_objects.end()) {
                            obj = sit->second;
                            self->m_objects.erase(sit);
                        }
                    }
                    if (!obj) {
                        webview_return(self->m_webview, seq, 0, "");
                        return;
                    }
                }

                std::string js = "if (window.__cpp__ && window.__cpp__." + instance_id + ") { delete window.__cpp__." + instance_id + "; }\n";
                self->post_eval(js);
                obj->mark_destroyed();

                webview_return(self->m_webview, seq, 0, "");
            } catch (const std::exception& e) {
                webview_return(self->m_webview, seq, 1,
                    BindingException::from(e).to_json().dump().c_str());
            }
        }, this);

    // 获取已注册的类型列表
    webview_bind(m_webview, "__webview_list_types__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            try {
                std::lock_guard<std::mutex> lock(self->m_objects_mutex);
                json types = json::array();
                for (auto& [name, _] : self->m_factories) {
                    types.push_back(name);
                }
                webview_return(self->m_webview, seq, 0, types.dump().c_str());
            } catch (const std::exception& e) {
                webview_return(self->m_webview, seq, 1,
                    BindingException::from(e).to_json().dump().c_str());
            }
        }, this);

    return true;
}

// C++ 调用已注册的 JS 回调（通过 __store_js_fn__ 存储）。
// 底层通过 post_eval 触发 JS 函数，执行时机在消息循环中。
void WebViewWrapper::call_js_fn(const std::string& fn_id, const json& args,
                                std::function<void(const json&)> on_result,
                                std::function<void(const std::string&)> on_error,
                                int timeout_ms) {
    if (m_terminated.load() || !m_webview) {
        if (on_error) on_error("WebView not ready");
        return;
    }

    std::string id = std::to_string(next_unique_id());
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_pending_callbacks[id] = { std::move(on_result), std::move(on_error), false };
    }

    // 超时清理
    if (timeout_ms > 0) {
        dispatch_task([this, id, timeout_ms]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            std::lock_guard<std::mutex> lock(m_callback_mutex);
            auto it = m_pending_callbacks.find(id);
            if (it != m_pending_callbacks.end()) {
                if (it->second.on_error)
                    it->second.on_error("Timeout");
                m_pending_callbacks.erase(it);
            }
        });
    }

    // 注入 JS：触发已存储的 JS 函数，结果通过 __cpp_result__ 返回
    std::string js = "(function(){"
        "try{"
        "  var r=window.__call_js_fn__('" + fn_id + "'," + args.dump(-1,' ',true) + ");"
        "  window.__cpp_result__('" + id + "',true,JSON.stringify(r === undefined ? null : r));"
        "}catch(e){"
        "  window.__cpp_result__('" + id + "',false,JSON.stringify({message:e.message||String(e)}));"
        "}"
    "})();";
    post_eval(js);
}
