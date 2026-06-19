#include "ChatService.h"
#include "WebViewWrapper.h"
#include <curl/curl.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

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
    stopServer();
    // curl_global_cleanup 由 main/WinMain 统一在进程退出前调用。
}

void ChatService::on_created() {
    std::cout << "[ChatService] created\n";
}

void ChatService::on_destroyed() {
    std::cout << "[ChatService] destroyed\n";
    stopServer();
}

void ChatService::startModel(const std::string& id, const json& args, WebViewWrapper* wv) {
    // wv由WebViewWrapper保证非空，直接存储
    m_wv = wv;

    std::string model_id = args.value("modelId", "");
    if (model_id.empty()) {
        wv->resolve(id, CppObject::error_result(ErrorCode::INVALID_ARGUMENTS, "modelId is required"));
        return;
    }

    int ctx = args.value("ctx", 4096);
    bool thinking = args.value("thinking", false);
    std::string gguf_path = args.value("ggufPath", "");

    LlamaParams params;
    params.ctx = ctx;
    params.thinking = thinking;
    params.n_gpu_layers = args.value("ngl", -1);
    params.threads = args.value("threads", 4);
    params.flash_attention = args.value("flashAttn", true) ? 1 : 0;

    wv->dispatch_task([this, id, model_id, gguf_path, params, wv]() {
        // 检查 llama-server 是否存在
        std::string server_path = getServerPath();
        if (!std::ifstream(server_path).good()) {
            wv->resolve(id, CppObject::ok_result({{
                {"status", "need_download"},
                {"message", "llama-server not found"}
            }}));
            return;
        }

        // 停止之前的
        stopServer();

        if (startServer(gguf_path, params)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_current = std::make_unique<ModelInstance>();
            m_current->model_id = model_id;
            m_current->gguf_path = gguf_path;
            m_current->params = params;
            m_current->running = true;

            wv->resolve(id, CppObject::ok_result({{
                {"status", "running"},
                {"modelId", model_id},
                {"port", m_port}
            }}));
        } else {
            wv->resolve(id, CppObject::error_result(ErrorCode::INTERNAL_ERROR, "Failed to start llama-server"));
        }
    });
}

json ChatService::stopModel(const json& args) {
    stopServer();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_current.reset();
    return CppObject::ok_result({{"status", "stopped"}});
}

void ChatService::chat(const std::string& id, const json& args, WebViewWrapper* wv) {
    std::string prompt = args.value("prompt", "");
    std::string callback = args.value("callback", "");

    if (prompt.empty()) {
        wv->resolve(id, CppObject::error_result(ErrorCode::INVALID_ARGUMENTS, "prompt is required"));
        return;
    }

    bool running = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        running = m_current && m_current->running;
    }

    if (!running) {
        wv->resolve(id, CppObject::error_result(ErrorCode::INTERNAL_ERROR, "No model is running"));
        return;
    }

    json messages = json::array();
    messages.push_back({{"role", "user"}, {"content", prompt}});

    if (!streamingRequest(messages, callback, wv)) {
        wv->resolve(id, CppObject::error_result(ErrorCode::INTERNAL_ERROR, "Request failed"));
    } else {
        wv->resolve(id, CppObject::ok_result({{"status", "completed"}}));
    }
}

json ChatService::getStatus(const json& args) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_current && m_current->running) {
        return CppObject::ok_result({{
            {"status", "running"},
            {"modelId", m_current->model_id},
            {"port", m_port}
        }});
    }
    return CppObject::ok_result({{"status", "stopped"}});
}

json ChatService::getMetrics(const json& args) {
    ProcessMetrics metrics = {};
    bool ok = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_server && m_server->isRunning()) {
            ok = m_server->getMetrics(metrics);
        }
    }

    if (ok) {
        return CppObject::ok_result({{
            {"status", "ok"},
            {"memoryMB", metrics.memoryMB},
            {"cpuPercent", metrics.cpuPercent},
            {"gpuMemoryMB", metrics.gpuMemoryMB},
            {"threads", metrics.threads},
            {"handles", metrics.handleCount}
        }});
    }
    return CppObject::ok_result({{
        {"status", "stopped"},
        {"memoryMB", 0},
        {"cpuPercent", 0.0},
        {"gpuMemoryMB", 0}
    }});
}

void ChatService::downloadServer(const std::string& id, const json& args, WebViewWrapper* wv) {
    wv->resolve(id, CppObject::ok_result({{
        {"status", "not_implemented"},
        {"message", "Download llama-server from: https://github.com/ggml-org/llama.cpp/releases"}
    }}));
}

std::string ChatService::getServerPath() {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string exe_dir(exe_path);
    auto sep = exe_dir.find_last_of("/\\");
    exe_dir = (sep == std::string::npos) ? "." : exe_dir.substr(0, sep);

    // 优先从 llama-bin 子目录（CMake 复制位置）查找
    std::string server_path = exe_dir + "/llama-bin/llama-server.exe";
    if (GetFileAttributesA(server_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return server_path;
    }

    // 回退：exe 同目录
    return exe_dir + "/llama-server.exe";
}

int ChatService::getAvailablePort() {
    for (int port = 8080; port <= 9090; ++port) {
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
    return 8080;
}

bool ChatService::startServer(const std::string& gguf_path, const LlamaParams& params) {
    // 端口与参数都用局部变量，启动阶段不持 m_mutex —— start() 内部健康检查会
    // 阻塞最多 15s，若持锁会冻结 GUI 线程的 getStatus/getMetrics/stopModel。
    int port = getAvailablePort();

    // 构建命令行参数
    std::ostringstream args;
    args << "-m \"" << gguf_path << "\"";
    args << " -c " << params.ctx;
    args << " -ngl " << params.n_gpu_layers;
    args << " -t " << params.threads;
    if (params.flash_attention) args << " -fa";
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

    // 进程退出回调（仅真实崩溃时由 monitor 线程调用；主动 stop 会跳过）。
    auto onExit = [this](int exitCode) {
        std::cerr << "[ChatService] llama-server exited with code " << exitCode << "\n";
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_current) m_current->running = false;
    };

    auto server = std::make_unique<Subprocess>();
    bool ok = server->start(getServerPath(), args.str(), "", healthCheck, 15000, onExit);

    // 启动成功后才在锁内发布 m_server / m_port。
    if (ok) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_port = port;
        m_server = std::move(server);
        m_serverPid = m_server->pid();
    }
    return ok;
}

void ChatService::stopServer() {
    // 把 m_server 移出锁外再 stop()/析构 —— stop() 优雅终止会阻塞最多 5s，
    // 不能在持锁时做（stopModel 是 GUI 线程 bind_sync）。
    std::unique_ptr<Subprocess> server;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        server = std::move(m_server);
        if (m_current) m_current->running = false;
    }
    if (server) {
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

bool ChatService::streamingRequest(const json& messages, const std::string& callback_id, WebViewWrapper* wv) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    json body = {
        {"messages", messages},
        {"stream", true}
    };
    std::string body_str = body.dump();
    std::string url = getServerUrl() + "/v1/chat/completions";

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
