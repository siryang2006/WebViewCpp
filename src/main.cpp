#include "WebViewWrapper.h"
#include "DownloadService.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdlib>
#include <sys/stat.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
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
// 配置服务：从磁盘读取 JSON 配置（演示 C++ 读文件能力）
// 前端通过 file:// 协议无法 fetch 本地文件，改由 C++ 读取返回
// ============================================================
class ConfigService : public CppObject {
public:
    ConfigService(const std::string& base_dir) : m_base_dir(base_dir) {
        bind_async("deleteFile", [this](const std::string& id, const json& args, WebViewWrapper* wv) {
            std::string path = args.is_array() && args.size() > 0 ? args[0].get<std::string>() : "";
            std::string dir = m_base_dir;
            wv->dispatch_task([id, path, dir, wv]() {
                if (!wv->is_ready()) { wv->reject(id, "WebView terminated"); return; }
                if (path.empty()) { wv->reject(id, "path is required"); return; }
                // path comes from JS and is joined onto the app dir; reject '..' so
                // it can't escape the directory and delete arbitrary files.
                if (path.find("..") != std::string::npos) { wv->reject(id, "Invalid path"); return; }
                std::string fullPath = dir + "/" + path;
                int ret = remove(fullPath.c_str());
                if (ret == 0) {
                    wv->resolve(id, {{"ok", true}, {"path", path}});
                } else {
                    wv->reject(id, "Failed to delete: " + path);
                }
            });
        });

        bind_async("addModel", [this](const std::string& id, const json& args, WebViewWrapper* wv) {
            json model = args.is_array() && args.size() > 0 ? args[0] : args;
            std::string dir = m_base_dir;
            wv->dispatch_task([id, model, dir, wv]() mutable {
                if (!wv->is_ready()) { wv->reject(id, "WebView terminated"); return; }
                std::string configPath = dir + "/models.json";
                std::ifstream f(configPath);
                if (!f) { wv->reject(id, "models.json not found"); return; }
                std::stringstream ss;
                ss << f.rdbuf();
                f.close();
                try {
                    json data = json::parse(ss.str());
                    if (!data.contains("models") || !data["models"].is_array()) {
                        data["models"] = json::array();
                    }
                    // Check for duplicate id
                    std::string newId = model.value("id", "");
                    for (auto& m : data["models"]) {
                        if (m.value("id", "") == newId) {
                            wv->reject(id, "Model id already exists: " + newId);
                            return;
                        }
                    }
                    // Set defaults
                    if (!model.contains("status")) model["status"] = "available";
                    if (!model.contains("size")) model["size"] = "Unknown";
                    if (!model.contains("size_bytes")) model["size_bytes"] = 0;
                    if (!model.contains("param")) model["param"] = 0;
                    if (!model.contains("type")) model["type"] = "Other";
                    if (!model.contains("desc")) model["desc"] = "";
                    if (!model.contains("ctx")) model["ctx"] = "32K";
                    if (!model.contains("gguf_path")) {
                        std::string filename = model.value("download_url", "");
                        auto pos = filename.find_last_of("/");
                        if (pos != std::string::npos) filename = filename.substr(pos + 1);
                        model["gguf_path"] = "downloads/" + newId + "/" + filename;
                    }
                    data["models"].push_back(model);
                    std::ofstream of(configPath);
                    of << data.dump(2);
                    of.close();
                    wv->resolve(id, data);
                } catch (const std::exception& e) {
                    wv->reject(id, std::string("failed: ") + e.what());
                }
            });
        });

        bind_async("deleteModel", [this](const std::string& id, const json& args, WebViewWrapper* wv) {
            std::string modelId = args.is_array() && args.size() > 0 ? args[0].get<std::string>() : "";
            std::string dir = m_base_dir;
            wv->dispatch_task([id, modelId, dir, wv]() {
                if (!wv->is_ready()) { wv->reject(id, "WebView terminated"); return; }
                std::string configPath = dir + "/models.json";
                std::ifstream f(configPath);
                if (!f) { wv->reject(id, "models.json not found"); return; }
                std::stringstream ss;
                ss << f.rdbuf();
                f.close();
                try {
                    json data = json::parse(ss.str());
                    if (!data.contains("models") || !data["models"].is_array()) {
                        wv->reject(id, "No models array");
                        return;
                    }
                    auto& models = data["models"];
                    bool found = false;
                    for (auto it = models.begin(); it != models.end(); ++it) {
                        if (it->value("id", "") == modelId) {
                            models.erase(it);
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        wv->reject(id, "Model not found: " + modelId);
                        return;
                    }
                    std::ofstream of(configPath);
                    of << data.dump(2);
                    of.close();
                    wv->resolve(id, {{"ok", true}});
                } catch (const std::exception& e) {
                    wv->reject(id, std::string("failed: ") + e.what());
                }
            });
        });

        bind_async("read", [this](const std::string& id, const json& args, WebViewWrapper* wv) {
            std::string name = args.empty() ? "" : args[0].get<std::string>();
            std::string dir = m_base_dir;
            wv->dispatch_task([id, name, dir, this, wv]() {
                if (!wv->is_ready()) { wv->reject(id, "WebView terminated"); return; }
                std::ifstream f(dir + "/" + name);
                if (!f) { wv->reject(id, "config not found: " + name); return; }
                std::stringstream ss;
                ss << f.rdbuf();
                try {
                    json data = json::parse(ss.str());
                    // 动态检测已下载的模型：检查 gguf_path 文件是否存在
                    if (data.contains("models") && data["models"].is_array()) {
                        for (auto& m : data["models"]) {
                            if (m.contains("gguf_path") && m["gguf_path"].is_string()) {
                                std::string savePath = dir + "/" + m["gguf_path"].get<std::string>();
                                struct stat st;
                                bool exists = (stat(savePath.c_str(), &st) == 0 && st.st_size > 0);
                                m["status"] = exists ? "downloaded" : "available";
                            } else {
                                m["status"] = "available";
                            }
                        }
                    }
                    wv->resolve(id, data);
                } catch (const std::exception& e) {
                    wv->reject(id, std::string("invalid json: ") + e.what());
                }
            });
        });
    }
    std::string object_name() const override { return "config"; }

private:
    std::string m_base_dir;
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

#ifdef _WIN32
// ============================================================
// 测试用最小 HTTP 服务器（仅用于 DownloadService 集成测试）
// 在本地回环端口上提供一段已知内容，可选支持 HTTP Range（断点续传）。
// 无需外网，全部在测试进程内运行。
// ============================================================
class TestHttpServer {
public:
    // body: 要提供的内容；supportRange: 是否honor Range 请求(206)；
    // throttleMs: 每个数据块之间的延迟(ms)，用于给暂停/取消留出时间窗口。
    TestHttpServer(std::string body, bool supportRange, int throttleMs = 0)
        : m_body(std::move(body)), m_supportRange(supportRange), m_throttleMs(throttleMs) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // 让系统分配空闲端口
        bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        listen(m_listen, 4);
        int len = sizeof(addr);
        getsockname(m_listen, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);
        m_thread = std::thread([this] { serve(); });
    }

    ~TestHttpServer() {
        m_stop.store(true);
        closesocket(m_listen);
        if (m_thread.joinable()) m_thread.join();
        WSACleanup();
    }

    int port() const { return m_port; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(m_port) + "/file"; }

private:
    void serve() {
        while (!m_stop.load()) {
            SOCKET client = accept(m_listen, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;  // listen socket closed -> shutdown
            handle(client);
            closesocket(client);
        }
    }

    void handle(SOCKET client) {
        // 读取请求头（直到 \r\n\r\n）
        std::string req;
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos) {
            int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) return;
            req.append(buf, n);
        }

        long long start = 0;
        bool hasRange = false;
        if (m_supportRange) {
            auto pos = req.find("Range: bytes=");
            if (pos != std::string::npos) {
                start = strtoll(req.c_str() + pos + 13, nullptr, 10);
                hasRange = true;
            }
        }

        long long total = static_cast<long long>(m_body.size());
        if (start < 0 || start > total) start = 0;
        std::string slice = m_body.substr(static_cast<size_t>(start));

        std::ostringstream hdr;
        if (hasRange) {
            hdr << "HTTP/1.1 206 Partial Content\r\n";
            hdr << "Content-Range: bytes " << start << "-" << (total - 1) << "/" << total << "\r\n";
        } else {
            hdr << "HTTP/1.1 200 OK\r\n";
        }
        hdr << "Content-Length: " << slice.size() << "\r\n";
        hdr << "Accept-Ranges: bytes\r\n";
        hdr << "Connection: close\r\n\r\n";
        std::string h = hdr.str();
        send(client, h.c_str(), static_cast<int>(h.size()), 0);

        // 分块发送 body，可选限速
        size_t off = 0;
        const size_t chunk = 4096;
        while (off < slice.size()) {
            size_t n = (std::min)(chunk, slice.size() - off);
            int sent = send(client, slice.data() + off, static_cast<int>(n), 0);
            if (sent <= 0) break;
            off += sent;
            if (m_throttleMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(m_throttleMs));
        }
    }

    std::string m_body;
    bool m_supportRange;
    int m_throttleMs;
    SOCKET m_listen = INVALID_SOCKET;
    int m_port = 0;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
};
#endif // _WIN32

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

    // ============================================================
    // DownloadService 测试
    // ============================================================
    auto download = std::make_shared<DownloadService>(".");

    try {
        json r = download->object_name();
        if (r == "download") { report("download.object_name", true, "got " + r.dump()); passed++; }
        else { report("download.object_name", false, "Expected \"download\", got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("download.object_name", false, e.what()); failed++; }

    try { download->invoke_sync("startDownload", {}); report("download.startDownload_is_async", false, "no exception"); failed++; }
    catch (const std::exception& e) { report("download.startDownload_is_async", true, std::string("caught: ") + e.what()); passed++; }

    try {
        json r = download->invoke_sync("pauseDownload", {{"modelId", "nonexistent"}});
        if (r["ok"] == false) { report("download.pauseDownload_no_task", true, "got error: " + r.dump()); passed++; }
        else { report("download.pauseDownload_no_task", false, "Expected error, got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("download.pauseDownload_no_task", false, e.what()); failed++; }

    try {
        json r = download->invoke_sync("resumeDownload", {{"modelId", "nonexistent"}});
        if (r["ok"] == false) { report("download.resumeDownload_no_task", true, "got error: " + r.dump()); passed++; }
        else { report("download.resumeDownload_no_task", false, "Expected error, got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("download.resumeDownload_no_task", false, e.what()); failed++; }

    try {
        json r = download->invoke_sync("cancelDownload", {{"modelId", "nonexistent"}});
        if (r["ok"] == false) { report("download.cancelDownload_no_task", true, "got error: " + r.dump()); passed++; }
        else { report("download.cancelDownload_no_task", false, "Expected error, got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("download.cancelDownload_no_task", false, e.what()); failed++; }

    try {
        json r = download->invoke_sync("getProgress", {{"modelId", "nonexistent"}});
        if (r["ok"] == false) { report("download.getProgress_no_task", true, "got error: " + r.dump()); passed++; }
        else { report("download.getProgress_no_task", false, "Expected error, got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("download.getProgress_no_task", false, e.what()); failed++; }

    try {
        json r = download->invoke_sync("getSpeed", {{"modelId", "nonexistent"}});
        if (r["ok"] == false) { report("download.getSpeed_no_task", true, "got error: " + r.dump()); passed++; }
        else { report("download.getSpeed_no_task", false, "Expected error, got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("download.getSpeed_no_task", false, e.what()); failed++; }

    // startDownload 现在校验必填参数（url/savePath/modelId）。invoke_sync 走异步分支会抛异常，
    // 故这里直接调用公共方法、传 wv=nullptr 来观察拒绝行为对路径穿越的防护。
    try {
        json r = download->invoke_sync("pauseDownload", {{"modelId", ""}});
        if (r["ok"] == false && r["code"] == static_cast<int>(ErrorCode::INVALID_ARGUMENTS)) {
            report("download.pause_empty_modelId", true, "got " + r.dump()); passed++;
        } else { report("download.pause_empty_modelId", false, "Expected INVALID_ARGUMENTS, got " + r.dump()); failed++; }
    } catch (const std::exception& e) { report("download.pause_empty_modelId", false, e.what()); failed++; }

#ifdef _WIN32
    // ============================================================
    // DownloadService 集成测试：本地 HTTP 服务器，无需外网/WebView
    // 直接调用 startDownload(id, args, nullptr) 驱动下载线程，轮询 getProgress 观察状态。
    // ============================================================
    auto makeBody = [](size_t n) {
        std::string s; s.reserve(n);
        for (size_t i = 0; i < n; ++i) s.push_back(static_cast<char>('A' + (i % 26)));
        return s;
    };
    auto readFile = [](const std::string& path) -> std::string {
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf(); return ss.str();
    };
    // 轮询直到状态命中 target 之一或超时；返回最终 status（""=超时）。
    auto pollStatus = [&](std::shared_ptr<DownloadService> svc, const std::string& mid,
                          std::initializer_list<const char*> targets, int timeoutMs) -> std::string {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            json r = svc->invoke_sync("getProgress", {{"modelId", mid}});
            if (r.contains("data") && r["data"].contains("status")) {
                std::string st = r["data"]["status"].get<std::string>();
                for (auto* t : targets) if (st == t) return st;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return "";
    };

    auto startArgs = [](const std::string& url, const std::string& path,
                        const std::string& mid, long long total) -> json {
        return json::array({ json{{"url", url}, {"savePath", path}, {"modelId", mid}, {"totalSize", total}} });
    };

    // --- 测试 1: 完整下载（服务器无 Range 支持，200）---
    try {
        std::string body = makeBody(64 * 1024);
        TestHttpServer srv(body, /*range*/false);
        std::string path = "test_dl_full.bin";
        remove(path.c_str());
        auto svc = std::make_shared<DownloadService>(".");
        svc->startDownload("t1", startArgs(srv.url(), path, "m_full", (long long)body.size()), nullptr);
        std::string st = pollStatus(svc, "m_full", {"completed", "cancelled"}, 10000);
        std::string got = readFile(path);
        if (st == "completed" && got == body) { report("download.full_download", true, std::to_string(got.size()) + " bytes"); passed++; }
        else { report("download.full_download", false, "status=" + st + " size=" + std::to_string(got.size()) + "/" + std::to_string(body.size())); failed++; }
        remove(path.c_str());
    } catch (const std::exception& e) { report("download.full_download", false, e.what()); failed++; }

    // --- 测试 2: 断点续传（服务器支持 Range，206 → 追加）---
    try {
        std::string body = makeBody(64 * 1024);
        std::string path = "test_dl_resume.bin";
        remove(path.c_str());
        // 预置前半部分（必须与 body 前缀一致，否则续传会得到错误内容）
        size_t partial = 20 * 1024;
        { std::ofstream of(path, std::ios::binary); of.write(body.data(), partial); }
        TestHttpServer srv(body, /*range*/true);
        auto svc = std::make_shared<DownloadService>(".");
        svc->startDownload("t2", startArgs(srv.url(), path, "m_resume", (long long)body.size()), nullptr);
        std::string st = pollStatus(svc, "m_resume", {"completed", "cancelled"}, 10000);
        std::string got = readFile(path);
        if (st == "completed" && got == body) { report("download.resume_206_append", true, "resumed from " + std::to_string(partial)); passed++; }
        else { report("download.resume_206_append", false, "status=" + st + " size=" + std::to_string(got.size()) + "/" + std::to_string(body.size())); failed++; }
        remove(path.c_str());
    } catch (const std::exception& e) { report("download.resume_206_append", false, e.what()); failed++; }

    // --- 测试 3: 有残留文件但服务器忽略 Range（返回 200 完整 body）→ 必须截断重下，不能损坏 ---
    try {
        std::string body = makeBody(64 * 1024);
        std::string path = "test_dl_norange.bin";
        remove(path.c_str());
        // 预置一段“垃圾”数据；若错误地追加完整 body，文件会比 body 更大且内容错位
        { std::ofstream of(path, std::ios::binary); std::string junk(20 * 1024, 'Z'); of.write(junk.data(), junk.size()); }
        TestHttpServer srv(body, /*range*/false);  // 忽略 Range，始终 200
        auto svc = std::make_shared<DownloadService>(".");
        svc->startDownload("t3", startArgs(srv.url(), path, "m_norange", (long long)body.size()), nullptr);
        std::string st = pollStatus(svc, "m_norange", {"completed", "cancelled"}, 10000);
        std::string got = readFile(path);
        if (st == "completed" && got == body) { report("download.no_range_truncates", true, "clean " + std::to_string(got.size()) + " bytes"); passed++; }
        else { report("download.no_range_truncates", false, "status=" + st + " size=" + std::to_string(got.size()) + "/" + std::to_string(body.size())); failed++; }
        remove(path.c_str());
    } catch (const std::exception& e) { report("download.no_range_truncates", false, e.what()); failed++; }

    // --- 测试 4: 暂停 → 恢复 → 完成 ---
    try {
        std::string body = makeBody(128 * 1024);
        std::string path = "test_dl_pause.bin";
        remove(path.c_str());
        TestHttpServer srv(body, /*range*/true, /*throttleMs*/15);  // 限速制造暂停窗口
        auto svc = std::make_shared<DownloadService>(".");
        svc->startDownload("t4", startArgs(srv.url(), path, "m_pause", (long long)body.size()), nullptr);
        // 等到下载真正开始
        pollStatus(svc, "m_pause", {"downloading"}, 5000);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        json pr = svc->invoke_sync("pauseDownload", {{"modelId", "m_pause"}});
        std::string pst = pollStatus(svc, "m_pause", {"paused"}, 3000);
        json rr = svc->invoke_sync("resumeDownload", {{"modelId", "m_pause"}});
        std::string st = pollStatus(svc, "m_pause", {"completed", "cancelled"}, 15000);
        std::string got = readFile(path);
        bool ok = (pr["ok"] == true) && (pst == "paused") && (rr["ok"] == true) && (st == "completed") && (got == body);
        if (ok) { report("download.pause_resume", true, "paused then completed"); passed++; }
        else { report("download.pause_resume", false, "pauseOk=" + std::string(pr.value("ok",false)?"1":"0") + " pst=" + pst + " st=" + st + " size=" + std::to_string(got.size()) + "/" + std::to_string(body.size())); failed++; }
        remove(path.c_str());
    } catch (const std::exception& e) { report("download.pause_resume", false, e.what()); failed++; }

    // --- 测试 5: 取消（任务应被移除）---
    try {
        std::string body = makeBody(256 * 1024);
        std::string path = "test_dl_cancel.bin";
        remove(path.c_str());
        TestHttpServer srv(body, /*range*/true, /*throttleMs*/15);
        auto svc = std::make_shared<DownloadService>(".");
        svc->startDownload("t5", startArgs(srv.url(), path, "m_cancel", (long long)body.size()), nullptr);
        pollStatus(svc, "m_cancel", {"downloading"}, 5000);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        json cr = svc->invoke_sync("cancelDownload", {{"modelId", "m_cancel"}});
        // 取消后任务应已从 map 移除，getProgress 返回错误
        json gp = svc->invoke_sync("getProgress", {{"modelId", "m_cancel"}});
        bool ok = (cr["ok"] == true) && (gp["ok"] == false);
        if (ok) { report("download.cancel_removes_task", true, "cancelled + removed"); passed++; }
        else { report("download.cancel_removes_task", false, "cancelOk=" + std::string(cr.value("ok",false)?"1":"0") + " getProgress=" + gp.dump()); failed++; }
        remove(path.c_str());
    } catch (const std::exception& e) { report("download.cancel_removes_task", false, e.what()); failed++; }
#endif // _WIN32

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

    // 计算 exe 所在目录（配置文件与前端资源均在此）
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string exe_dir(exe_path);
    auto sep = exe_dir.find_last_of("/\\");
    exe_dir = (sep == std::string::npos) ? "." : exe_dir.substr(0, sep);

    wv.bind_object(std::make_shared<MathService>());
    wv.bind_object(std::make_shared<FileService>());
    wv.bind_object(std::make_shared<ConfigService>(exe_dir));

    auto downloadSvc = std::make_shared<DownloadService>(exe_dir);
    wv.bind_object(downloadSvc);
    downloadSvc->setWebView(&wv);

    wv.bind_factory("Worker", WorkerService::create, WebViewWrapper::FactoryMode::Instance);

    // 加载独立的 demo.html（CSS/JS 分离为 demo.css / demo.js）
    // 用 file:// URL 导航，相对路径才能解析到同目录的 css/js
    std::string html_path = exe_dir + "/demo.html";
    std::ifstream f(html_path);
    if (!f) {
        std::cerr << "Error: demo.html not found at " << html_path << std::endl;
        return 1;
    }
    f.close();
    // 反斜杠转正斜杠，组装 file:// URL
    std::string url_path = html_path;
    for (auto& c : url_path) if (c == '\\') c = '/';
    wv.navigate("file:///" + url_path);
    wv.dispatch_task([&wv]() {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        wv.call_registered_js("onCppEvent", {{"event", "startup"}, {"time", "2024-01-01"}});
    });

    wv.run();
    return 0;
}
