#include "WebViewWrapper.h"
#include <iostream>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

// ============================================================
// 数学服务示例
// ============================================================
class MathService : public CppObject {
public:
    MathService() {
        bind_sync("add", [](const json& args) -> json {
            return args[0].get<int>() + args[1].get<int>();
        });

        bind_sync("multiply", [](const json& args) -> json {
            return args[0].get<int>() * args[1].get<int>();
        });

        // 异步方法：使用 wv->dispatch_task 替代 detach
        bind_async("slow_add", [](const std::string& id, const json& args, WebViewWrapper* wv) {
            int a = args[0].get<int>();
            int b = args[1].get<int>();
            wv->dispatch_task([id, a, b, wv]() {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (!wv->is_ready()) {
                    wv->reject(id, "WebView terminated");
                    return;
                }
                wv->resolve(id, a + b);
            });
        });

        bind_async("fetch_data", [](const std::string& id, const json& args, WebViewWrapper* wv) {
            std::string query = args[0].get<std::string>();
            wv->dispatch_task([id, query, wv]() {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!wv->is_ready()) {
                    wv->reject(id, "WebView terminated");
                    return;
                }
                wv->resolve(id, {{"query", query}, {"status", "ok"}, {"count", 42}});
            });
        });

        bind_property("version", []() -> std::string { return "1.0.0"; });
        bind_property("pi", []() -> double { return 3.14159; });
    }

    std::string object_name() const override { return "math"; }

    void on_created() override { std::cout << "[MathService] created\n"; }
    void on_destroyed() override { std::cout << "[MathService] destroyed\n"; }
};

// ============================================================
// 文件服务示例
// ============================================================
class FileService : public CppObject {
public:
    FileService() {
        bind_async("read", [](const std::string& id, const json& args, WebViewWrapper* wv) {
            std::string path = args[0].get<std::string>();
            wv->dispatch_task([id, path, wv]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (!wv->is_ready()) {
                    wv->reject(id, "WebView terminated");
                    return;
                }
                wv->resolve(id, {{"content", "File content of: " + path}, {"size", 1024}});
            });
        });

        bind_async("write", [](const std::string& id, const json& args, WebViewWrapper* wv) {
            std::string path = args[0].get<std::string>();
            std::string content = args[1].get<std::string>();
            wv->dispatch_task([id, path, content, wv]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                if (!wv->is_ready()) {
                    wv->reject(id, "WebView terminated");
                    return;
                }
                wv->resolve(id, {{"success", true}, {"bytes", content.size()}});
            });
        });
    }

    std::string object_name() const override { return "file"; }
};

// ============================================================
// Worker 示例（支持 JS new 创建）
// ============================================================
class WorkerService : public CppObject {
public:
    WorkerService(const std::string& name, int priority)
        : m_name(name), m_priority(priority) {
        bind_sync("getName", [this](const json&) -> json { return m_name; });
        bind_sync("getPriority", [this](const json&) -> json { return m_priority; });
        bind_sync("setPriority", [this](const json& args) -> json {
            m_priority = args[0].get<int>();
            return m_priority;
        });
        bind_async("doWork", [this](const std::string& id, const json& args, WebViewWrapper* wv) {
            std::string task = args[0].get<std::string>();
            wv->dispatch_task([id, task, this, wv]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (!wv->is_ready()) {
                    wv->reject(id, "WebView terminated");
                    return;
                }
                wv->resolve(id, {
                    {"worker", m_name},
                    {"task", task},
                    {"priority", m_priority},
                    {"status", "done"}
                });
            });
        });
    }

    // 工厂构造函数
    static std::shared_ptr<WorkerService> create(const json& args) {
        std::string name = args.size() > 0 ? args[0].get<std::string>() : "default";
        int priority = args.size() > 1 ? args[1].get<int>() : 0;
        return std::make_shared<WorkerService>(name, priority);
    }

    std::string object_name() const override { return "worker_" + m_name; }

    void on_created() override { std::cout << "[Worker] created: " << m_name << " (pri=" << m_priority << ")\n"; }
    void on_destroyed() override { std::cout << "[Worker] destroyed: " << m_name << "\n"; }

private:
    std::string m_name;
    int m_priority;
};

// ============================================================
// 主函数
// ============================================================
#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#else
int main() {
#endif
    WebViewWrapper wv;

    if (!wv.init("WebView C++ Binding Demo", "", 1024, 768)) {
        std::cerr << "Failed to create webview" << std::endl;
        return 1;
    }

    // 1. 全局单例绑定
    wv.bind_object(std::make_shared<MathService>());
    wv.bind_object(std::make_shared<FileService>());

    // 2. 实例工厂：JS new <-> C++ new，JS GC 自动触发 C++ 销毁
    wv.bind_factory("Worker", WorkerService::create, WebViewWrapper::FactoryMode::Instance);

    // 3. 全局工厂：JS 直接调用，C++ 使用全局单例
    // wv.bind_factory("System", SystemService::create, WebViewWrapper::FactoryMode::Global);

    wv.set_html(R"HTML(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>C++ / JS Binding Demo</title>
    <style>
        body { font-family: 'Segoe UI', sans-serif; margin: 20px; background: #1a1a2e; color: #e0e0e0; }
        h1 { color: #00d4ff; }
        .section { background: #16213e; padding: 15px; border-radius: 8px; margin: 10px 0; }
        button { background: #0f3460; color: white; border: none; padding: 8px 16px;
                 border-radius: 4px; cursor: pointer; margin: 4px; }
        button:hover { background: #533483; }
        .result { background: #0a0a1a; padding: 10px; border-radius: 4px; margin-top: 10px;
                  font-family: monospace; white-space: pre-wrap; }
        .loading { color: #ffcc00; } .success { color: #00ff88; } .error { color: #ff4444; }
    </style>
</head>
<body>
    <h1>C++ / JS Object Binding Demo</h1>

    <div class="section">
        <h2>1. 全局单例</h2>
        <button onclick="testSyncAdd()">math.add(10, 20)</button>
        <button onclick="testProperties()">math.version & math.pi</button>
        <div id="sync-result" class="result"></div>
    </div>

    <div class="section">
        <h2>2. JS new 创建 C++ 实例</h2>
        <button onclick="testNewWorker()">new Worker("Alice", 5)</button>
        <button onclick="testNewWorker2()">new Worker("Bob", 10)</button>
        <button onclick="testWorkerDestroy()">destroy worker1</button>
        <div id="worker-result" class="result"></div>
    </div>

    <div class="section">
        <h2>3. 异步方法</h2>
        <button onclick="testAsync()">file.read("config.json")</button>
        <div id="async-result" class="result"></div>
    </div>

    <div class="section">
        <h2>4. C++ calls JS</h2>
        <button onclick="registerCallback()">Register JS callback</button>
        <div id="cpp2js-result" class="result"></div>
    </div>

    <script>
        var worker1, worker2;

        function log(id, msg, cls) {
            var el = document.getElementById(id);
            el.className = 'result ' + (cls || '');
            el.textContent += msg + '\n';
        }

        async function testSyncAdd() {
            log('sync-result', 'math.add(10, 20) = ' + window.__cpp__.math.add(10, 20), 'success');
        }
        async function testProperties() {
            log('sync-result', 'math.version = "' + window.__cpp__.math.version + '"', 'success');
            log('sync-result', 'math.pi = ' + window.__cpp__.math.pi, 'success');
        }

        async function testNewWorker() {
            worker1 = new window.__cpp__.Worker("Alice", 5);
            log('worker-result', 'Created worker1: ' + JSON.stringify(worker1), 'success');
            log('worker-result', 'getName() = ' + worker1.getName(), 'success');
            log('worker-result', 'getPriority() = ' + worker1.getPriority(), 'success');
            log('worker-result', 'setPriority(8) = ' + worker1.setPriority(8), 'success');

            var result = await worker1.doWork("Build project");
            log('worker-result', 'doWork() = ' + JSON.stringify(result, null, 2), 'success');
        }

        async function testNewWorker2() {
            worker2 = new window.__cpp__.Worker("Bob", 10);
            log('worker-result', 'Created worker2: ' + JSON.stringify(worker2), 'success');
            var result = await worker2.doWork("Run tests");
            log('worker-result', 'doWork() = ' + JSON.stringify(result, null, 2), 'success');
        }

        function testWorkerDestroy() {
            if (worker1) {
                worker1.__destroy__();
                log('worker-result', 'worker1 destroyed manually', 'success');
                worker1 = null;
            }
        }

        async function testAsync() {
            log('async-result', 'Calling file.read("config.json")...', 'loading');
            try {
                var r = await window.__cpp__.file.read("config.json");
                log('async-result', 'file.read() = ' + JSON.stringify(r, null, 2), 'success');
            } catch(e) { log('async-result', 'Error: ' + e.message, 'error'); }
        }

        function registerCallback() {
            window.__register_cb__("onCppEvent", function(args) {
                log('cpp2js-result', 'C++ called JS: ' + JSON.stringify(args), 'success');
            });
            log('cpp2js-result', 'Callback registered.', 'loading');
        }
    </script>
</body>
</html>
    )HTML");

    // C++ 主动调用 JS
    wv.dispatch_task([&wv]() {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        wv.call_registered_js("onCppEvent", {{"event", "startup"}, {"time", "2024-01-01"}});
    });

    wv.run();
    return 0;
}
