#include "ChatService.h"
#include "WebViewWrapper.h"
#include <curl/curl.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

ChatService::ChatService() {
    // curl 全局初始化由 main/WinMain 统一调用一次（libcurl 文档要求 call-once，
    // 且非线程安全），此处不再重复 init。

    bind_async("startModel", [this](const std::string& id, const json& args, WebViewWrapper* wv) {
        startModel(id, args, wv);
    });

    bind_sync("stopModel", [this](const json& args) -> json {
        return stopModel(args);
    });

    bind_async("chat", [this](const std::string& id, const json& args, WebViewWrapper* wv) {
        chat(id, args, wv);
    });

    bind_sync("getStatus", [this](const json& args) -> json {
        return getStatus(args);
    });

    bind_sync("getMetrics", [this](const json& args) -> json {
        return getMetrics(args);
    });

    bind_async("downloadServer", [this](const std::string& id, const json& args, WebViewWrapper* wv) {
        downloadServer(id, args, wv);
    });
}

ChatService::~ChatService() {
    stopServers("");  // 停止全部
    // curl_global_cleanup 由 main/WinMain 统一在进程退出前调用。
}

void ChatService::on_created() {
    std::cout << "[ChatService] created\n";
}

void ChatService::on_destroyed() {
    std::cout << "[ChatService] destroyed\n";
    stopServers("");
}

// 异步桥接以 JS arguments 数组形式传参，foo({...}) 到达时为 [{...}]。
const json& ChatService::unwrapArgs(const json& args) {
    if (args.is_array() && !args.empty() && args[0].is_object()) return args[0];
    return args;
}

// 按进程名杀死未托管的进程（解决残留 llama-server 占端口，但放过已在 m_models 中的）
static void killOrphanedByName(const std::string& name, const std::vector<int>& excludePids) {
#ifdef _WIN32
    std::wstring wname(name.begin(), name.end());
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = {sizeof(pe)};
    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, wname.c_str()) == 0 &&
                std::find(excludePids.begin(), excludePids.end(), (int)pe.th32ProcessID) == excludePids.end()) {
                HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProcess) {
                    TerminateProcess(hProcess, 1);
                    CloseHandle(hProcess);
                }
            }
        } while (Process32NextW(snapshot, &pe));
    }
    CloseHandle(snapshot);
#endif
}

void ChatService::startModel(const std::string& id, const json& args, WebViewWrapper* wv) {
    // wv由WebViewWrapper保证非空，直接存储
    m_wv = wv;

    // 启动前先清理残留的 llama-server 进程，但放过已在 m_models 中的
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<int> managedPids;
        for (auto& [mid, rm] : m_models) {
            if (rm->server) managedPids.push_back(rm->server->pid());
        }
        killOrphanedByName("llama-server.exe", managedPids);
    }

    const json& p = unwrapArgs(args);

    std::string model_id = p.value("modelId", "");
    if (model_id.empty()) {
        wv->resolve(id, CppObject::error_result(ErrorCode::INVALID_ARGUMENTS, "modelId is required"));
        return;
    }

    int ctx = p.value("ctx", 4096);
    bool thinking = p.value("thinking", false);
    std::string gguf_path = p.value("ggufPath", "");

    LlamaParams params;
    params.ctx = ctx;
    params.thinking = thinking;
    params.n_gpu_layers = p.value("ngl", -1);
    params.threads = p.value("threads", 4);
    params.flash_attention = p.value("flashAttn", true) ? 1 : 0;

    // 该模型已在运行 → 幂等返回 running。
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_models.find(model_id);
        if (it != m_models.end() && it->second->running.load()) {
            int port = it->second->port;
            json d = json::object();
            d["status"] = "running";
            d["modelId"] = model_id;
            d["port"] = port;
            wv->resolve(id, CppObject::ok_result(d));
            return;
        }
    }

    // 检查 llama-server 是否存在
    std::string server_path = getServerPath();
    if (!std::ifstream(server_path).good()) {
        json d = json::object();
        d["status"] = "need_download";
        d["message"] = "llama-server not found";
        wv->resolve(id, CppObject::ok_result(d));
        return;
    }

    // 收集已占用端口，避免与已运行模型冲突。
    std::vector<int> usedPorts;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [mid, rm] : m_models) {
            if (rm->running.load()) usedPorts.push_back(rm->port);
        }
    }

    int port = 0;
    std::unique_ptr<Subprocess> server;
    std::string start_err;
    if (startServer(gguf_path, params, usedPorts, port, server, start_err)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto rm = std::make_unique<RunningModel>();
        rm->model_id = model_id;
        rm->gguf_path = gguf_path;
        rm->params = params;
        rm->port = port;
        rm->server = std::move(server);
        rm->running = true;
        m_models[model_id] = std::move(rm);

        json d = json::object();
        d["status"] = "running";
        d["modelId"] = model_id;
        d["port"] = port;
        wv->resolve(id, CppObject::ok_result(d));
    } else {
        wv->resolve(id, CppObject::error_result(ErrorCode::INTERNAL_ERROR,
            start_err.empty() ? "Failed to start llama-server" : ("Failed to start llama-server: " + start_err).c_str()));
    }
}

json ChatService::stopModel(const json& args) {
    const json& p = unwrapArgs(args);
    std::string model_id = p.is_object() ? p.value("modelId", "") : "";

    stopServers(model_id);

    json d = json::object();
    d["status"] = "stopped";
    if (model_id.empty()) {
        d["scope"] = "all";
    } else {
        d["modelId"] = model_id;
    }
    return CppObject::ok_result(d);
}

void ChatService::chat(const std::string& id, const json& args, WebViewWrapper* wv) {
    const json& p = unwrapArgs(args);

    std::string prompt = p.value("prompt", "");
    std::string callback = p.value("callback", "");
    std::string model_id = p.value("modelId", "");

    if (prompt.empty()) {
        wv->resolve(id, CppObject::error_result(ErrorCode::INVALID_ARGUMENTS, "prompt is required"));
        return;
    }

    // 解析目标端口：带 modelId 用指定模型；否则用唯一运行模型。
    int port = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!model_id.empty()) {
            auto it = m_models.find(model_id);
            if (it != m_models.end() && it->second->running.load()) port = it->second->port;
        } else {
            // 无 modelId：若仅一个模型在运行则用它。
            RunningModel* only = nullptr;
            int count = 0;
            for (auto& [mid, rm] : m_models) {
                if (rm->running.load()) { only = rm.get(); count++; }
            }
            if (count == 1 && only) port = only->port;
        }
    }

    if (port == 0) {
        wv->resolve(id, CppObject::error_result(ErrorCode::INTERNAL_ERROR, "No model is running"));
        return;
    }

    json messages = json::array();
    messages.push_back({{"role", "user"}, {"content", prompt}});

    if (!streamingRequest(port, messages, callback, wv)) {
        wv->resolve(id, CppObject::error_result(ErrorCode::INTERNAL_ERROR, "Request failed"));
    } else {
        wv->resolve(id, CppObject::ok_result({{"status", "completed"}}));
    }
}

json ChatService::getStatus(const json& args) {
    const json& p = unwrapArgs(args);
    std::string model_id = p.is_object() ? p.value("modelId", "") : "";

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!model_id.empty()) {
        auto it = m_models.find(model_id);
        if (it != m_models.end() && it->second->running.load()) {
            json d = json::object();
            d["status"] = "running";
            d["modelId"] = model_id;
            d["port"] = it->second->port;
            d["pid"] = it->second->server ? it->second->server->pid() : 0;
            return CppObject::ok_result(d);
        }
        json d = json::object();
        d["status"] = "stopped";
        d["modelId"] = model_id;
        return CppObject::ok_result(d);
    }

    // 无 modelId：返回所有运行中模型列表。
    json models = json::array();
    for (auto& [mid, rm] : m_models) {
        if (rm->running.load()) {
            json m = json::object();
            m["modelId"] = mid;
            m["port"] = rm->port;
            m["pid"] = rm->server ? rm->server->pid() : 0;
            m["status"] = "running";
            models.push_back(std::move(m));
        }
    }
    json d = json::object();
    d["status"] = models.empty() ? "stopped" : "running";
    d["models"] = std::move(models);
    return CppObject::ok_result(d);
}

// 采集单模型指标。须在锁外调用：Subprocess::getMetrics 可能采样 CPU。
json ChatService::metricsFor(RunningModel* rm) {
    ProcessMetrics metrics = {};
    bool ok = false;
    if (rm && rm->server && rm->server->isRunning()) {
        ok = rm->server->getMetrics(metrics);
    }
    json d = json::object();
    if (ok) {
        d["modelId"] = rm->model_id;
        d["port"] = rm->port;
        d["pid"] = rm->server ? rm->server->pid() : 0;
        d["status"] = "ok";
        d["memoryMB"] = metrics.memoryMB;
        d["cpuPercent"] = metrics.cpuPercent;
        d["gpuMemoryMB"] = metrics.gpuMemoryMB;
        d["threads"] = metrics.threads;
        d["handles"] = metrics.handleCount;
    } else {
        d["modelId"] = rm ? rm->model_id : "";
        d["status"] = "stopped";
        d["memoryMB"] = 0;
        d["cpuPercent"] = 0.0;
        d["gpuMemoryMB"] = 0;
    }
    return d;
}

json ChatService::getMetrics(const json& args) {
    const json& p = unwrapArgs(args);
    std::string model_id = p.is_object() ? p.value("modelId", "") : "";

    if (!model_id.empty()) {
        // 锁内取指针引用，锁内采样单个模型，避免与 stop 竞争删除。
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_models.find(model_id);
        if (it != m_models.end() && it->second->running.load()) {
            return CppObject::ok_result(metricsFor(it->second.get()));
        }
        json d = json::object();
        d["status"] = "stopped";
        d["modelId"] = model_id;
        d["memoryMB"] = 0;
        d["cpuPercent"] = 0.0;
        d["gpuMemoryMB"] = 0;
        return CppObject::ok_result(d);
    }

    // 无 modelId：返回每个运行模型一份指标。
    json models = json::array();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [mid, rm] : m_models) {
            if (rm->running.load()) {
                models.push_back(metricsFor(rm.get()));
            }
        }
    }
    json d = json::object();
    d["status"] = models.empty() ? "stopped" : "ok";
    d["models"] = std::move(models);
    return CppObject::ok_result(d);
}

void ChatService::downloadServer(const std::string& id, const json& args, WebViewWrapper* wv) {
    wv->resolve(id, CppObject::ok_result({{
        {"status", "not_implemented"},
        {"message", "Download llama-server from: https://github.com/ggml-org/llama.cpp/releases"}
    }}));
}

std::string ChatService::getExeDir() {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string exe_dir(exe_path);
    auto sep = exe_dir.find_last_of("/\\");
    return (sep == std::string::npos) ? "." : exe_dir.substr(0, sep);
}

std::string ChatService::getServerPath() {
    std::string exe_dir = getExeDir();

    // 优先从 llama-bin 子目录（CMake 复制位置）查找
    std::string server_path = exe_dir + "/llama-bin/llama-server.exe";
    if (GetFileAttributesA(server_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return server_path;
    }

    // 回退：exe 同目录
    return exe_dir + "/llama-server.exe";
}

int ChatService::getAvailablePort(const std::vector<int>& exclude) {
    for (int port = 8080; port <= 9090; ++port) {
        if (std::find(exclude.begin(), exclude.end(), port) != exclude.end()) continue;

        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) continue;

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(static_cast<u_short>(port));

        if (bind(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
            closesocket(s);
            return port;
        }
        closesocket(s);
    }
    return 0;  // 无可用端口
}

bool ChatService::startServer(const std::string& gguf_path, const LlamaParams& params,
                              const std::vector<int>& excludePorts,
                              int& out_port, std::unique_ptr<Subprocess>& out_server,
                              std::string& error_detail) {
    int port = getAvailablePort(excludePorts);
    if (port == 0) {
        error_detail = "No available port (8080-9090 all in use)";
        std::cerr << "[ChatService] " << error_detail << "\n";
        return false;
    }

    std::string exe_dir = getExeDir();

    // gguf_path 多为相对路径（如 downloads/xxx/model.gguf，来自 models.json）。
    // 子进程工作目录设为 exe 目录后，相对路径即以 exe 目录为基准解析；
    // 绝对路径（含盘符或 UNC）原样传递。
    std::string model_arg = gguf_path;
    bool is_absolute = (gguf_path.size() >= 2 && gguf_path[1] == ':') ||
                       (gguf_path.size() >= 2 && (gguf_path[0] == '\\' || gguf_path[0] == '/') &&
                        (gguf_path[1] == '\\' || gguf_path[1] == '/'));
    if (!is_absolute && !gguf_path.empty()) {
        model_arg = exe_dir + "/" + gguf_path;
    }

    // 构建命令行参数
    std::ostringstream args;
    args << "-m \"" << model_arg << "\"";
    args << " -c " << params.ctx;
    args << " -ngl " << params.n_gpu_layers;
    args << " -t " << params.threads;
    if (params.flash_attention) args << " --flash-attn 1";
    if (params.thinking) args << " --reasoning-format auto";
    args << " --host 127.0.0.1";
    args << " --port " << port;

    // 健康检查：轮询 /health 端点
    auto healthCheck = [port]() -> bool {
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        std::string url = "http://127.0.0.1:" + std::to_string(port) + "/health";
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 500L);
        CURLcode res = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(curl);
        return res == CURLE_OK && code == 200;
    };

    // 工作目录设为 exe 目录：保证 gguf 相对路径、DLL 依赖（llama-bin 下的 ggml*.dll）
    // 等都能正确解析。健康检查放宽到 30s —— 大模型加载 + warmup 可能较慢。
    auto server = std::make_unique<Subprocess>();
    bool ok = server->start(getServerPath(), args.str(), exe_dir, healthCheck, 30000, nullptr);

    if (!ok) {
        std::string sub_err = server->lastError();
        if (!sub_err.empty()) {
            if (error_detail.empty()) error_detail = sub_err;
            else error_detail += "; " + sub_err;
        }
    }

    if (ok) {
        out_port = port;
        out_server = std::move(server);
    }
    return ok;
}

void ChatService::stopServers(const std::string& modelId) {
    // 把要停止的 Subprocess 移出锁外再 stop()/析构 —— stop() 优雅终止会阻塞最多 5s，
    // 不能在持锁时做（stopModel 是 GUI 线程 bind_sync）。
    std::vector<std::unique_ptr<Subprocess>> toStop;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (modelId.empty()) {
            for (auto& [mid, rm] : m_models) {
                rm->running = false;
                if (rm->server) toStop.push_back(std::move(rm->server));
            }
            m_models.clear();
        } else {
            auto it = m_models.find(modelId);
            if (it != m_models.end()) {
                it->second->running = false;
                if (it->second->server) toStop.push_back(std::move(it->second->server));
                m_models.erase(it);
            }
        }
    }
    for (auto& server : toStop) {
        server->stop(true);  // 优雅终止；析构进一步兜底
    }
}

// SSE 流式解析上下文：在 write callback 内按行解析，token 即时回调到 JS
namespace {
struct StreamContext {
    std::string buffer;           // 未处理完的残留数据
    std::string callback_id;
    WebViewWrapper* wv = nullptr;

    // 处理一行 SSE 数据
    void processLine(const std::string& line) {
        if (line.rfind("data: ", 0) != 0) return;
        std::string json_str = line.substr(6);
        if (json_str == "[DONE]") return;
        try {
            auto chunk = json::parse(json_str);
            if (chunk.contains("choices") && !chunk["choices"].empty()) {
                auto& choice = chunk["choices"][0];
                if (choice.contains("delta") && choice["delta"].contains("content")) {
                    std::string token = choice["delta"]["content"];
                    if (!token.empty() && !callback_id.empty() && wv) {
                        wv->call_registered_js(callback_id, {{"token", token}, {"done", false}});
                    }
                }
            }
        } catch (...) {}
    }
};

// curl 写回调：累积数据，按 \n 切分出完整行立即处理
size_t streamWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* ctx = static_cast<StreamContext*>(userdata);
    ctx->buffer.append(ptr, ptr + total);

    // 逐行处理已到达的完整行
    size_t pos;
    while ((pos = ctx->buffer.find('\n')) != std::string::npos) {
        std::string line = ctx->buffer.substr(0, pos);
        // 去掉行尾 \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ctx->buffer.erase(0, pos + 1);
        if (!line.empty()) ctx->processLine(line);
    }
    return total;
}
} // namespace

bool ChatService::streamingRequest(int port, const json& messages,
                                   const std::string& callback_id, WebViewWrapper* wv) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    json body = {
        {"messages", messages},
        {"stream", true}
    };
    std::string body_str = body.dump();
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // 流式解析上下文：token 在 write callback 内即时回调到 JS
    StreamContext ctx;
    ctx.callback_id = callback_id;
    ctx.wv = wv;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    CURLcode res = curl_easy_perform(curl);

    // 处理可能残留的最后一行（无结尾换行）
    if (!ctx.buffer.empty()) {
        std::string line = ctx.buffer;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ctx.processLine(line);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (!callback_id.empty()) {
        wv->call_registered_js(callback_id, {{"token", ""}, {"done", true}});
    }

    return res == CURLE_OK;
}
