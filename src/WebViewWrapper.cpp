#include "WebViewWrapper.h"
#include <webview/webview.h>

#include <sstream>
#include <chrono>

// ============================================================
// JsCallback
// ============================================================
void JsCallback::resolve(const json& result) {
    if (m_wv) m_wv->resolve(m_id, result);
}
void JsCallback::reject(const std::string& error) {
    if (m_wv) m_wv->reject(m_id, error);
}

// ============================================================
// CppObject::invoke_async (needs full WebViewWrapper definition)
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
}

// ============================================================
// 初始化
// ============================================================
bool WebViewWrapper::init(const std::string& title, const std::string& url,
                          int width, int height, bool resizable) {
    m_webview = static_cast<struct webview*>(webview_create(1, nullptr));
    if (!m_webview) return false;

    webview_set_title(m_webview, title.c_str());
    webview_set_size(m_webview, width, height,
                     resizable ? WEBVIEW_HINT_NONE : WEBVIEW_HINT_FIXED);

    setup_js_bridge();
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
    webview_run(m_webview);
}

void WebViewWrapper::terminate() {
    if (m_webview && !m_terminated.exchange(true)) {
        webview_terminate(m_webview);
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

    std::string js = "if (window.__cpp__ && window.__cpp__." + name + ") { delete window.__cpp__." + name + "; }\n";
    post_eval(js);
    obj->mark_destroyed();
}

void WebViewWrapper::bind_factory(const std::string& type_name, ObjectFactory factory, FactoryMode mode) {
    std::lock_guard<std::mutex> lock(m_objects_mutex);
    m_factories[type_name] = { std::move(factory), mode };

    if (mode == FactoryMode::Instance) {
        // ===== 实例模式：JS new <-> C++ new，JS GC 自动触发 C++ 销毁 =====
        std::ostringstream js;
        js << "(function() {\n"
           << "  window.__cpp__ = window.__cpp__ || {};\n"
           << "  var __gc_reg__ = new FinalizationRegistry(function(iid) {\n"
           << "    window.__webview_destroy__(iid);\n"
           << "  });\n"
           << "  var Ctor = function() {\n"
           << "    var args = Array.prototype.slice.call(arguments);\n"
           << "    var req = JSON.stringify([\"" << type_name << "\", args]);\n"
           << "    var raw = window.__webview_create__(req);\n"
           << "    var info = JSON.parse(raw);\n"
           << "    if (!info.id) throw new Error(info.error || 'Create failed');\n"
           << "    var iid = info.id;\n"
           << "    var self = { __id__: iid, __type__: \"" << type_name << "\", __destroyed__: false };\n"
           << "    var proxy = new Proxy(self, {\n"
           << "      get: function(target, prop) {\n"
           << "        if (prop in target) return target[prop];\n"
           << "        return function() {\n"
           << "          var mArgs = Array.prototype.slice.call(arguments);\n"
           << "          var mReq = JSON.stringify([iid, prop, mArgs]);\n"
           << "          var mRaw = window.__webview_sync_call__(iid, prop, mReq);\n"
           << "          return JSON.parse(mRaw);\n"
           << "        };\n"
           << "      }\n"
           << "    });\n"
           << "    __gc_reg__.register(proxy, iid, proxy);\n"
           << "    return proxy;\n"
           << "  };\n"
           << "  Ctor.prototype.__destroy__ = function() {\n"
           << "    if (this.__destroyed__) return;\n"
           << "    this.__destroyed__ = true;\n"
           << "    window.__webview_destroy__(this.__id__);\n"
           << "  };\n"
           << "  window.__cpp__." << type_name << " = Ctor;\n"
           << "})();\n";
        post_eval(js.str());
    } else {
        // ===== 全局模式：JS 直接调用函数，C++ 使用全局单例 =====
        auto obj = m_factories[type_name].factory(json::array());
        m_objects[type_name] = obj;
        obj->on_created();
        inject_single_object(obj, "");
    }
}

void WebViewWrapper::inject_all_objects() {
    std::lock_guard<std::mutex> lock(m_objects_mutex);
    for (auto& [_, obj] : m_objects) {
        inject_single_object(obj, "");
    }
}

void WebViewWrapper::inject_single_object(std::shared_ptr<CppObject> obj, const std::string& instance_id) {
    const std::string& js_name = instance_id.empty() ? obj->object_name() : instance_id;

    std::ostringstream js;
    js << "window.__cpp__ = window.__cpp__ || {};\n";
    js << "window.__cpp__." << js_name << " = {};\n";

    // 同步方法
    for (auto& [method_name, _] : obj->sync_methods()) {
        js << "window.__cpp__." << js_name << "." << method_name
           << " = function() {\n"
           << "  var args = Array.prototype.slice.call(arguments);\n"
           << "  var req = JSON.stringify(args);\n"
           << "  var raw = window.__webview_sync_call__('"
           << js_name << "','" << method_name << "', req);\n"
           << "  return JSON.parse(raw);\n"
           << "};\n";
    }

    // 异步方法
    for (auto& [method_name, _] : obj->async_methods()) {
        js << "window.__cpp__." << js_name << "." << method_name
           << " = function() {\n"
           << "  return new Promise(function(resolve, reject) {\n"
           << "    var args = Array.prototype.slice.call(arguments);\n"
           << "    var id = '__cb_' + Date.now() + '_' + Math.random().toString(36).substr(2,9);\n"
           << "    window.__pending_callbacks__ = window.__pending_callbacks__ || {};\n"
           << "    window.__pending_callbacks__[id] = { resolve: resolve, reject: reject };\n"
           << "    var req = JSON.stringify({ args: args, id: id });\n"
           << "    window.__webview_async_call__('"
           << js_name << "','" << method_name << "', req);\n"
           << "  });\n"
           << "};\n";
    }

    // 属性
    for (auto& prop_name : obj->property_names()) {
        js << "Object.defineProperty(window.__cpp__." << js_name << ", '" << prop_name << "', {\n"
           << "  get: function() {\n"
           << "    return JSON.parse(window.__webview_get_property__('"
           << js_name << "','" << prop_name << "'));\n"
           << "  },\n";
        if (obj->has_setter(prop_name)) {
            js << "  set: function(v) {\n"
               << "    window.__webview_set_property__('"
               << js_name << "','" << prop_name << "', JSON.stringify(v));\n"
               << "  },\n";
        } else {
            js << "  set: function(v) { throw new Error('Property is read-only'); },\n";
        }
        js << "  enumerable: true\n});\n";
    }

    // 注入销毁方法
    js << "window.__cpp__." << js_name << ".__destroy__ = function() {\n"
       << "  window.__webview_destroy__('" << js_name << "');\n"
       << "};\n";

    post_eval(js.str());
}

// ============================================================
// 回调机制（线程安全）
// ============================================================
void WebViewWrapper::resolve(const std::string& id, const json& result) {
    if (m_terminated.load() || !m_webview) return;

    std::string json_str = result.dump();
    std::ostringstream js;
    js << "(function(){\n"
       << "  var cbs = window.__pending_callbacks__ || {};\n"
       << "  var cb = cbs['" << id << "'];\n"
       << "  if (cb) { cb.resolve(" << json_str << "); delete cbs['" << id << "']; }\n"
       << "})();";
    post_eval(js.str());
}

void WebViewWrapper::reject(const std::string& id, const std::string& error) {
    if (m_terminated.load() || !m_webview) return;

    std::string escaped = CppObject::js_escape(error);
    std::ostringstream js;
    js << "(function(){\n"
       << "  var cbs = window.__pending_callbacks__ || {};\n"
       << "  var cb = cbs['" << id << "'];\n"
       << "  if (cb) { cb.reject(new Error('" << escaped << "')); delete cbs['" << id << "']; }\n"
       << "})();";
    post_eval(js.str());
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
        m_pending_callbacks[id] = { std::move(on_result), std::move(on_error) };
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
       << "    var result = window." << func_name << "(" << args.dump() << ");\n"
       << "    if (result && typeof result.then === 'function') {\n"
       << "      result.then(function(r) {\n"
       << "        window.__cpp_result__('" << id << "', true, JSON.stringify(r));\n"
       << "      }).catch(function(e) {\n"
       << "        window.__cpp_result__('" << id << "', false, e.message || String(e));\n"
       << "      });\n"
       << "    } else {\n"
       << "      window.__cpp_result__('" << id << "', true, JSON.stringify(result));\n"
       << "    }\n"
       << "  } catch(e) {\n"
       << "    window.__cpp_result__('" << id << "', false, e.message || String(e));\n"
       << "  }\n"
       << "})();";
    post_eval(js.str());
}

void WebViewWrapper::call_registered_js(const std::string& name, const json& args) {
    post_eval("window." + name + "(" + args.dump() + ")");
}

// ============================================================
// JS Bridge
// ============================================================
void WebViewWrapper::setup_js_bridge() {
    post_eval(
        "window.__pending_callbacks__ = {};\n"
        "window.__registered_cbs__ = {};\n"
        "window.__register_cb__ = function(name, fn) { window.__registered_cbs__[name] = fn; };\n"
    );

    // C++ 调用 JS 结果回调
    webview_bind(m_webview, "__cpp_result__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            auto data = json::parse(req);
            std::string id = data[0].get<std::string>();
            bool success = data[1].get<bool>();
            std::string value = data[2].get<std::string>();

            std::lock_guard<std::mutex> lock(self->m_callback_mutex);
            auto it = self->m_pending_callbacks.find(id);
            if (it != self->m_pending_callbacks.end()) {
                if (success) {
                    if (it->second.on_result)
                        it->second.on_result(json::parse(value));
                } else {
                    if (it->second.on_error)
                        it->second.on_error(value);
                }
                self->m_pending_callbacks.erase(it);
            }
            webview_return(self->m_webview, seq, 0, "");
        }, this);

    // 同步调用
    webview_bind(m_webview, "__webview_sync_call__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            auto data = json::parse(req);
            std::string obj_name = data[0].get<std::string>();
            std::string method = data[1].get<std::string>();
            json args;
            // 兼容两种格式：data[2] 是 JSON 值或 JSON 字符串
            if (data[2].is_string()) {
                args = json::parse(data[2].get<std::string>());
            } else {
                args = data[2];
            }

            try {
                std::lock_guard<std::mutex> lock(self->m_objects_mutex);
                // 先查静态对象，再查动态实例
                auto it = self->m_objects.find(obj_name);
                if (it == self->m_objects.end())
                    it = self->m_dynamic_instances.find(obj_name);
                if (it == self->m_dynamic_instances.end())
                    throw std::runtime_error("Object not found: " + obj_name);
                json result = it->second->invoke_sync(method, args);
                webview_return(self->m_webview, seq, 0, result.dump().c_str());
            } catch (const std::exception& e) {
                webview_return(self->m_webview, seq, 1, e.what());
            }
        }, this);

    // 异步调用
    webview_bind(m_webview, "__webview_async_call__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            auto data = json::parse(req);
            std::string obj_name = data[0].get<std::string>();
            std::string method = data[1].get<std::string>();
            auto call_data = json::parse(data[2].get<std::string>());
            std::string id = call_data["id"].get<std::string>();
            json args = call_data["args"];

            std::shared_ptr<CppObject> obj;
            {
                std::lock_guard<std::mutex> lock(self->m_objects_mutex);
                auto it = self->m_objects.find(obj_name);
                if (it == self->m_objects.end())
                    it = self->m_dynamic_instances.find(obj_name);
                if (it != self->m_objects.end() || it != self->m_dynamic_instances.end())
                    obj = it->second;
            }

            if (!obj) {
                self->reject(id, "Object not found: " + obj_name);
            } else {
                self->dispatch_task([obj, method, id, args, self]() {
                    obj->invoke_async(method, id, args, self);
                });
            }
            webview_return(self->m_webview, seq, 0, "");
        }, this);

    // 属性读取
    webview_bind(m_webview, "__webview_get_property__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            auto data = json::parse(req);
            std::string obj_name = data[0].get<std::string>();
            std::string prop = data[1].get<std::string>();

            try {
                std::lock_guard<std::mutex> lock(self->m_objects_mutex);
                auto it = self->m_objects.find(obj_name);
                if (it == self->m_objects.end())
                    it = self->m_dynamic_instances.find(obj_name);
                if (it == self->m_objects.end() && it == self->m_dynamic_instances.end())
                    throw std::runtime_error("Object not found: " + obj_name);
                json val = it->second->get_property(prop);
                webview_return(self->m_webview, seq, 0, val.dump().c_str());
            } catch (const std::exception& e) {
                webview_return(self->m_webview, seq, 1, e.what());
            }
        }, this);

    // 属性设置
    webview_bind(m_webview, "__webview_set_property__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            auto data = json::parse(req);
            std::string obj_name = data[0].get<std::string>();
            std::string prop = data[1].get<std::string>();
            json val = json::parse(data[2].get<std::string>());

            try {
                std::lock_guard<std::mutex> lock(self->m_objects_mutex);
                auto it = self->m_objects.find(obj_name);
                if (it == self->m_objects.end())
                    it = self->m_dynamic_instances.find(obj_name);
                if (it == self->m_objects.end() && it == self->m_dynamic_instances.end())
                    throw std::runtime_error("Object not found: " + obj_name);
                it->second->set_property(prop, val);
                webview_return(self->m_webview, seq, 0, "");
            } catch (const std::exception& e) {
                webview_return(self->m_webview, seq, 1, e.what());
            }
        }, this);

    // 创建动态实例
    webview_bind(m_webview, "__webview_create__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            auto data = json::parse(req);
            std::string type_name = data[0].get<std::string>();
            json args = data[1];

            std::shared_ptr<CppObject> obj;
            std::string instance_id;
            {
                std::lock_guard<std::mutex> lock(self->m_objects_mutex);
                auto fit = self->m_factories.find(type_name);
                if (fit == self->m_factories.end()) {
                    webview_return(self->m_webview, seq, 1, ("Unknown type: " + type_name).c_str());
                    return;
                }
                try {
                    obj = fit->second.factory(args);
                } catch (const std::exception& e) {
                    webview_return(self->m_webview, seq, 1, e.what());
                    return;
                }
                instance_id = type_name + "_" + std::to_string(obj->instance_id());
                self->m_dynamic_instances[instance_id] = obj;
            }

            obj->on_created();
            // 注意：不调用 inject_single_object，由 JS Proxy 统一处理方法分发

            json result = {
                {"id", instance_id},
                {"type", type_name}
            };
            webview_return(self->m_webview, seq, 0, result.dump().c_str());
        }, this);

    // 销毁动态实例
    webview_bind(m_webview, "__webview_destroy__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            std::string instance_id = req;

            std::shared_ptr<CppObject> obj;
            {
                std::lock_guard<std::mutex> lock(self->m_objects_mutex);
                auto it = self->m_dynamic_instances.find(instance_id);
                if (it == self->m_dynamic_instances.end()) {
                    webview_return(self->m_webview, seq, 1, "Instance not found");
                    return;
                }
                obj = it->second;
                self->m_dynamic_instances.erase(it);
            }

            // 清理 JS 端
            std::string js = "if (window.__cpp__ && window.__cpp__." + instance_id + ") { delete window.__cpp__." + instance_id + "; }\n";
            self->post_eval(js);
            obj->mark_destroyed();

            webview_return(self->m_webview, seq, 0, "");
        }, this);

    // 获取已注册的类型列表
    webview_bind(m_webview, "__webview_list_types__",
        [](const char* seq, const char* req, void* arg) {
            auto* self = static_cast<WebViewWrapper*>(arg);
            std::lock_guard<std::mutex> lock(self->m_objects_mutex);
            json types = json::array();
            for (auto& [name, _] : self->m_factories) {
                types.push_back(name);
            }
            webview_return(self->m_webview, seq, 0, types.dump().c_str());
        }, this);
}
