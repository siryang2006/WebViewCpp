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
    bool init(const std::string& title, const std::string& url = "",
              int width = 800, int height = 600, bool resizable = true);

    // 主循环（阻塞直到 terminate）
    void run();
    void terminate();

    // 导航和渲染
    void navigate(const std::string& url);
    void set_html(const std::string& html);
    void set_title(const std::string& title);
    void eval(const std::string& js);

    // 对象绑定/解绑
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

    // C++ 调用 JS（带回调）
    void call_js(const std::string& func_name, const json& args,
                 std::function<void(const json&)> on_result = nullptr,
                 std::function<void(const std::string&)> on_error = nullptr,
                 int timeout_ms = 10000);

    // 调用已注册的 JS 回调
    void call_registered_js(const std::string& name, const json& args);

    // 异步任务派发（替代 detach，确保对象销毁时能取消）
    void dispatch_task(std::function<void()> fn);

    // 获取原生指针（仅 WebViewWrapper 内部使用）
    void* native() { return m_webview; }
    bool is_ready() const { return m_webview != nullptr && !m_terminated.load(); }

private:
    void setup_js_bridge();
    void inject_all_objects();
    void inject_single_object(std::shared_ptr<CppObject> obj, const std::string& instance_id = "");

    void post_eval(const std::string& js);
    void process_eval_queue();
    void start_dispatch_loop();
    static void dispatch_loop_static(WebViewWrapper* self);

    void start_dispatch_loop_internal();

    struct webview* m_webview = nullptr;
    std::atomic<bool> m_terminated{false};

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
    struct PendingCallback {
        std::function<void(const json&)> on_result;
        std::function<void(const std::string&)> on_error;
    };
    std::mutex m_callback_mutex;
    int64_t m_next_callback_id{0};
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
