#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include "binding/CppObject.h"
#include "Subprocess.h"
#include <string>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>

// llama-server HTTP 推理服务
// 子进程由 Subprocess 管理，Job Object 由 WebViewWrapper 统一处理，
// 父进程退出时 llama-server 自动被终止。
class ChatService : public CppObject {
public:
    struct LlamaParams {
        int ctx = 4096;           // context window size
        int n_gpu_layers = -1;    // -1 = auto (all to GPU)
        int threads = 4;          // CPU threads
        int flash_attention = 1;   // enable flash attention
        bool thinking = false;     // enable thinking mode
    };

    ChatService();
    ~ChatService();

    std::string object_name() const override { return "chat"; }
    void on_created() override;
    void on_destroyed() override;

    // 启动模型（异步）
    void startModel(const std::string& id, const json& args, WebViewWrapper* wv);
    // 停止模型
    json stopModel(const json& args);
    // 聊天（流式，token 通过回调 ID 逐个推送到 JS）
    void chat(const std::string& id, const json& args, WebViewWrapper* wv);
    // 获取当前运行状态
    json getStatus(const json& args);
    // 获取资源占用
    json getMetrics(const json& args);
    // 下载 llama-server 二进制
    void downloadServer(const std::string& id, const json& args, WebViewWrapper* wv);

private:
    struct ModelInstance {
        std::string model_id;
        std::string gguf_path;
        LlamaParams params;
        std::atomic<bool> running{false};
    };

    std::string getServerPath();
    std::string getExeDir();
    std::string getServerUrl() const { return "http://127.0.0.1:" + std::to_string(m_port); }
    int getAvailablePort();

    bool startServer(const std::string& gguf_path, const LlamaParams& params);
    void stopServer();

    // 流式 HTTP 请求
    bool streamingRequest(const json& messages, const std::string& callback_id, WebViewWrapper* wv);

    WebViewWrapper* m_wv = nullptr;
    std::mutex m_mutex;

    // 当前运行中的模型
    std::unique_ptr<ModelInstance> m_current;

    // llama-server 子进程
    std::unique_ptr<Subprocess> m_server;
    int m_port = 0;
    int m_serverPid = 0;
};
