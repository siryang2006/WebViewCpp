#include "WebViewWrapper.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <cmath>

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

        // 反向调用：JS 调用此方法后，C++ 主动回调已注册的 JS 回调（onCppEvent）。
        bind_async("fire_event", [](const std::string& id, const json& args, WebViewWrapper* wv) {
            std::string event = args.empty() ? "manual" : args[0].get<std::string>();
            wv->dispatch_task([id, event, wv]() {
                if (!wv->is_ready()) {
                    wv->reject(id, "WebView terminated");
                    return;
                }
                wv->call_registered_js("onCppEvent", {{"event", event}, {"source", "fire_event"}});
                wv->resolve(id, {{"fired", true}, {"event", event}});
            });
        });
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
// C++ 服务测试（纯 C++ 单元测试，无需 WebView）
// ============================================================
static bool run_cpp_tests() {
    auto report = [](const std::string& name, bool ok, const std::string& detail = "") {
        std::cout << "  [" << (ok ? "PASS" : "FAIL") << "] " << name;
        if (!detail.empty()) std::cout << " - " << detail;
        std::cout << std::endl;
        return ok;
    };

    std::cout << "\n=== C++ Service Tests ===" << std::endl;
    int passed = 0, failed = 0;

    auto math = std::make_shared<MathService>();

    try {
        json r = math->invoke_sync("add", {10, 20});
        if (r == 30) { report("math.add(10,20)", true, "got " + r.dump()); passed++; }
        else { report("math.add(10,20)", false, "Expected 30, got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("math.add(10,20)", false, e.what()); failed++; }

    try {
        json r = math->invoke_sync("multiply", {6, 7});
        if (r == 42) { report("math.multiply(6,7)", true, "got " + r.dump()); passed++; }
        else { report("math.multiply(6,7)", false, "Expected 42, got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("math.multiply(6,7)", false, e.what()); failed++; }

    try {
        json r = math->get_property("version");
        if (r == "1.0.0") { report("math.version", true, "got " + r.dump()); passed++; }
        else { report("math.version", false, "Expected \"1.0.0\", got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("math.version", false, e.what()); failed++; }

    try {
        json r = math->get_property("pi");
        double v = r;
        if (std::abs(v - 3.14159) < 0.001) { report("math.pi", true, "got " + r.dump()); passed++; }
        else { report("math.pi", false, "Expected ~3.14159, got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("math.pi", false, e.what()); failed++; }

    try { math->invoke_sync("nonexistent", {}); report("math.error_method_not_found", false, "no exception"); failed++; }
    catch (const std::exception& e) { report("math.error_method_not_found", true, std::string("caught: ") + e.what()); passed++; }

    try { math->get_property("nonexistent"); report("math.error_property_not_found", false, "no exception"); failed++; }
    catch (const std::exception& e) { report("math.error_property_not_found", true, std::string("caught: ") + e.what()); passed++; }

    try { math->set_property("version", "2.0"); report("math.error_set_readonly", false, "no exception"); failed++; }
    catch (const std::exception& e) { report("math.error_set_readonly", true, std::string("caught: ") + e.what()); passed++; }

    try { math->invoke_sync("add", {"hello", 20}); report("math.add_wrong_arg_type", false, "no exception"); failed++; }
    catch (const std::exception& e) { report("math.add_wrong_arg_type", true, std::string("caught: ") + e.what()); passed++; }

    auto file = std::make_shared<FileService>();

    try {
        json r = file->object_name();
        if (r == "file") { report("file.object_name", true, "got " + r.dump()); passed++; }
        else { report("file.object_name", false, "Expected \"file\", got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("file.object_name", false, e.what()); failed++; }

    try { file->invoke_sync("read", {"test.txt"}); report("file.read_is_async", false, "no exception"); failed++; }
    catch (const std::exception& e) { report("file.read_is_async", true, std::string("caught: ") + e.what()); passed++; }

    try { file->invoke_sync("write", {"test.txt", "content"}); report("file.write_is_async", false, "no exception"); failed++; }
    catch (const std::exception& e) { report("file.write_is_async", true, std::string("caught: ") + e.what()); passed++; }

    auto worker = WorkerService::create({"Alice", 5});

    try {
        json r = worker->invoke_sync("getName", {});
        if (r == "Alice") { report("Worker.getName", true, "got " + r.dump()); passed++; }
        else { report("Worker.getName", false, "Expected \"Alice\", got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("Worker.getName", false, e.what()); failed++; }

    try {
        json r = worker->invoke_sync("getPriority", {});
        if (r == 5) { report("Worker.getPriority", true, "got " + r.dump()); passed++; }
        else { report("Worker.getPriority", false, "Expected 5, got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("Worker.getPriority", false, e.what()); failed++; }

    try {
        json r = worker->invoke_sync("setPriority", {8});
        if (r == 8) { report("Worker.setPriority(8)", true, "got " + r.dump()); passed++; }
        else { report("Worker.setPriority(8)", false, "Expected 8, got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("Worker.setPriority(8)", false, e.what()); failed++; }

    try {
        json r = worker->invoke_sync("getPriority", {});
        if (r == 8) { report("Worker.getPriority_after_set", true, "got " + r.dump()); passed++; }
        else { report("Worker.getPriority_after_set", false, "Expected 8, got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("Worker.getPriority_after_set", false, e.what()); failed++; }

    try { worker->invoke_sync("nonexistent", {}); report("Worker.error_method_not_found", false, "no exception"); failed++; }
    catch (const std::exception& e) { report("Worker.error_method_not_found", true, std::string("caught: ") + e.what()); passed++; }

    try { worker->get_property("nonexistent"); report("Worker.error_property_not_found", false, "no exception"); failed++; }
    catch (const std::exception& e) { report("Worker.error_property_not_found", true, std::string("caught: ") + e.what()); passed++; }

    try { worker->set_property("getName", "Bob"); report("Worker.error_set_non_existent", false, "no exception"); failed++; }
    catch (const std::exception& e) { report("Worker.error_set_non_existent", true, std::string("caught: ") + e.what()); passed++; }

    try {
        auto w = WorkerService::create({});
        json r = w->invoke_sync("getName", {});
        if (r == "default") { report("Worker.default_name", true, "got " + r.dump()); passed++; }
        else { report("Worker.default_name", false, "Expected \"default\", got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("Worker.default_name", false, e.what()); failed++; }

    try {
        auto w = WorkerService::create({});
        json r = w->invoke_sync("getPriority", {});
        if (r == 0) { report("Worker.default_priority", true, "got " + r.dump()); passed++; }
        else { report("Worker.default_priority", false, "Expected 0, got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("Worker.default_priority", false, e.what()); failed++; }

    try {
        auto w = WorkerService::create({"Bob"});
        json r = w->invoke_sync("getName", {});
        if (r == "Bob") { report("Worker.partial_arg_name", true, "got " + r.dump()); passed++; }
        else { report("Worker.partial_arg_name", false, "Expected \"Bob\", got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("Worker.partial_arg_name", false, e.what()); failed++; }

    try {
        auto w = WorkerService::create({"Bob"});
        json r = w->invoke_sync("getPriority", {});
        if (r == 0) { report("Worker.partial_arg_priority", true, "got " + r.dump()); passed++; }
        else { report("Worker.partial_arg_priority", false, "Expected 0, got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("Worker.partial_arg_priority", false, e.what()); failed++; }

    try {
        auto a = WorkerService::create({"Alice", 5});
        auto b = WorkerService::create({"Bob", 10});
        a->invoke_sync("setPriority", {99});
        json ra = a->invoke_sync("getPriority", {});
        json rb = b->invoke_sync("getPriority", {});
        bool ids = a->instance_id() != b->instance_id();
        bool ok = true;
        if (ra != 99) { report("Worker.multi_A_priority", false, "Expected 99, got " + ra.dump()); ok = false; }
        if (rb != 10) { report("Worker.multi_B_priority", false, "Expected 10, got " + rb.dump()); ok = false; }
        if (!ids) { report("Worker.unique_instance_ids", false, "IDs should differ"); ok = false; }
        if (ok) {
            report("Worker.multi_A_priority", true, "got " + ra.dump()); passed++;
            report("Worker.multi_B_priority", true, "got " + rb.dump()); passed++;
            report("Worker.unique_instance_ids", true, "A=" + std::to_string(a->instance_id()) + " B=" + std::to_string(b->instance_id())); passed++;
        } else { failed += 3; }
    } catch (const std::exception& e) {
        report("Worker.multi tests", false, e.what()); failed += 3;
    }

    try { WorkerService::create({"Test", 1})->invoke_sync("doWork", {"task"}); report("Worker.doWork_is_async", false, "no exception"); failed++; }
    catch (const std::exception& e) { report("Worker.doWork_is_async", true, std::string("caught: ") + e.what()); passed++; }

    try {
        json r = CppObject::ok_result(42);
        if (r["ok"] == true && r["data"] == 42) { report("CppObject.ok_result", true, "got " + r.dump()); passed++; }
        else { report("CppObject.ok_result", false, "Unexpected: " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("CppObject.ok_result", false, e.what()); failed++; }

    try {
        json r = CppObject::error_result(ErrorCode::INVALID_ARGUMENTS, "bad arg");
        if (r["ok"] == false && r["code"] == -4 && r["message"] == "bad arg") { report("CppObject.error_result", true, "got " + r.dump()); passed++; }
        else { report("CppObject.error_result", false, "Unexpected: " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("CppObject.error_result", false, e.what()); failed++; }

    std::cout << "Passed: " << passed << ", Failed: " << failed << ", Total: " << (passed + failed) << std::endl;
    std::cout << "=== End C++ Tests ===" << std::endl;
    return failed == 0;
}

// ============================================================
// 主函数
// ============================================================
#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    int cdp_port = 0;
    bool test_mode = false;
    if (lpCmdLine && *lpCmdLine) {
        std::string cmd(lpCmdLine);
        auto port_pos = cmd.find("--cdp-port=");
        if (port_pos != std::string::npos) {
            cdp_port = std::stoi(cmd.substr(port_pos + 11));
        }
        if (cmd.find("--test") != std::string::npos) {
            test_mode = true;
        }
    }
    if (test_mode) {
        bool ok = run_cpp_tests();
        return ok ? 0 : 1;
    }
#else
int main(int argc, char* argv[]) {
    int cdp_port = 0;
    bool test_mode = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        auto port_pos = arg.find("--cdp-port=");
        if (port_pos != std::string::npos) {
            cdp_port = std::stoi(arg.substr(port_pos + 11));
        }
        if (arg == "--test") {
            test_mode = true;
        }
    }
    if (test_mode) {
        bool ok = run_cpp_tests();
        return ok ? 0 : 1;
    }
#endif
    WebViewWrapper wv;

    if (!wv.init("WebView C++ Binding Demo", "", 1024, 768, true, cdp_port)) {
        std::cerr << "Failed to create webview" << std::endl;
        return 1;
    }

    wv.bind_object(std::make_shared<MathService>());
    wv.bind_object(std::make_shared<FileService>());

    wv.bind_factory("Worker", WorkerService::create, WebViewWrapper::FactoryMode::Instance);

    // 加载独立 HTML 文件（demo.html），无需在 C++ 中拼接 HTML/JS/CSS
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string exe_dir(exe_path);
    auto sep = exe_dir.find_last_of("/\\");
    exe_dir = (sep == std::string::npos) ? "." : exe_dir.substr(0, sep);
    std::string html_path = exe_dir + "/demo.html";
    std::ifstream f(html_path);
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        wv.set_html(ss.str());
    } else {
        std::cerr << "Error: demo.html not found at " << html_path << std::endl;
        return 1;
    }
    wv.dispatch_task([&wv]() {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        wv.call_registered_js("onCppEvent", {{"event", "startup"}, {"time", "2024-01-01"}});
    });

    wv.run();
    return 0;
}
