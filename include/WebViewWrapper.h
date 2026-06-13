#pragma once

#include "binding/CppObject.h"
#include <string>
#include <map>
#include <memory>
#include <functional>
#include <mutex>
#include <queue>
#include <atomic>
#include <condition_variable>
#include <thread>

// Forward declaration - actual webview struct defined in webview/webview.h
// Only included in WebViewWrapper.cpp to avoid ODR violations

class WebViewWrapper {
public:
    WebViewWrapper();
    ~WebViewWrapper();

    WebViewWrapper(const WebViewWrapper&) = delete;
    WebViewWrapper& operator=(const WebViewWrapper&) = delete;

    // 初始化
    // debug_port: 启用 CDP 远程调试端口 (0=禁用, >0=端口号, 仅 Windows 有效)
    bool init(const std::string& title, const std::string& url = "",
              int width = 800, int height = 600, bool resizable = true,
              int debug_port = 0);

    // 主循环（阻塞直到 terminate）
    void run();
    void terminate();

    // 导航和渲染
    void navigate(const std::string& url);
    void set_html(const std::string& html);
    void set_title(const std::string& title);
    void eval(const std::string& js);

    // 对象绑定/解绑
    // 注意：JS 端访问绑定对象的方法和属性均为异步（返回 Promise），需 await。
    //   例如 await window.__cpp__.math.add(1,2)、await window.__cpp__.math.version。
    //   因此 JSON.stringify(obj) 不会序列化属性值（属性 getter 返回 Promise）。
    // 线程：bind_object / unbind_object / bind_factory 可在任意线程调用——
    //   内部会把需要 GUI 线程的注入操作（webview_init）自动调度到 GUI 线程。
    void bind_object(std::shared_ptr<CppObject> obj);
    void unbind_object(const std::string& name);

    // 对象工厂（JS 动态创建 C++ 对象）
    enum class FactoryMode {
        Instance,  // JS new 创建实例，C++ 对应 new shared_ptr
        Global     // JS 直接调用，C++ 使用全局单例
    };
    using ObjectFactory = std::function<std::shared_ptr<CppObject>(const json& args)>;
    void bind_factory(const std::string& type_name, ObjectFactory factory, FactoryMode mode = FactoryMode::Instance);

    // 线程安全回调
    void resolve(const std::string& id, const json& result);
    void reject(const std::string& id, const std::string& error);

    // C++ 调用已注册的 JS 回调（由 JS 端的函数参数 ID 触发）
    // fn_id: JS 端传入的回调 ID；args: 传给 JS 回调的参数
    void call_js_fn(const std::string& fn_id, const json& args,
                    std::function<void(const json&)> on_result = nullptr,
                    std::function<void(const std::string&)> on_error = nullptr,
                    int timeout_ms = 10000);

    // C++ 调用 JS（带回调）
    void call_js(const std::string& func_name, const json& args,
                 std::function<void(const json&)> on_result = nullptr,
                 std::function<void(const std::string&)> on_error = nullptr,
                 int timeout_ms = 10000);

    // 调用已注册的 JS 回调
    void call_registered_js(const std::string& name, const json& args);

    // 异步任务派发（替代 detach，确保对象销毁时能取消）
    void dispatch_task(std::function<void()> fn);

    bool is_ready() const { return m_webview != nullptr && !m_terminated.load(); }

private:
    bool setup_js_bridge();
    void inject_single_object(std::shared_ptr<CppObject> obj, const std::string& instance_id = "");

    void post_eval(const std::string& js);
    // 持久注入：webview_init 保证脚本在每次文档创建时重新执行（跨导航存活），
    // post_eval 保证对已加载的当前页也立即生效（运行时绑定场景）。
    void inject_persistent(const std::string& js);
    void process_eval_queue();
    void start_dispatch_loop();
    static void dispatch_loop_static(WebViewWrapper* self);

    void start_dispatch_loop_internal();

    void* m_webview = nullptr;
    std::atomic<bool> m_terminated{false};

    // GUI（消息循环）线程 id：webview_init/webview_create 等 API 只能在此线程调用。
    // inject_persistent 据此判断是否需要 dispatch 到 GUI 线程。
    std::thread::id m_gui_thread_id;

    std::mutex m_objects_mutex;
    std::map<std::string, std::shared_ptr<CppObject>> m_objects;

    // 工厂和动态实例管理
    struct FactoryInfo {
        ObjectFactory factory;
        FactoryMode mode;
    };
    std::map<std::string, FactoryInfo> m_factories;
    std::map<std::string, std::shared_ptr<CppObject>> m_dynamic_instances;

    // 线程安全的 eval 队列
    std::mutex m_eval_mutex;
    std::queue<std::string> m_eval_queue;
    std::atomic<bool> m_eval_pending{false};

    // 异步回调
    // is_js_callback=true: 直接调 JS 函数（cb_fn），on_result/on_error 忽略
    // is_js_callback=false: 走 Promise 机制
    struct PendingCallback {
        std::function<void(const json&)> on_result;
        std::function<void(const std::string&)> on_error;
        bool is_js_callback = false;
    };
    std::mutex m_callback_mutex;
    std::map<std::string, PendingCallback> m_pending_callbacks;

    // 任务派发线程
    std::thread m_dispatch_thread;
    std::mutex m_task_mutex;
    std::condition_variable m_task_cv;
    std::queue<std::function<void()>> m_task_queue;
    std::atomic<bool> m_task_running{false};

    // 全局唯一 ID
    int64_t next_unique_id() {
        static std::atomic<int64_t> id{0};
        return ++id;
    }

    friend class JsCallback;
};
