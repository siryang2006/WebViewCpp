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
#include <vector>

// llama-server HTTP 推理服务
// 支持多模型并发：每个模型一个 llama-server 子进程，独立端口、独立指标。
// 子进程由 Subprocess 管理，Job Object 由 WebViewWrapper 统一处理，
// 父进程退出时所有 llama-server 自动被终止。
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

    // 启动模型（异步）。args: {modelId, ggufPath, ctx, ngl, threads, flashAttn, thinking}
    void startModel(const std::string& id, const json& args, WebViewWrapper* wv);
    // 停止模型。args: {modelId} 停止指定模型；无 modelId 时停止全部。
    json stopModel(const json& args);
    // 聊天（流式，token 通过回调 ID 逐个推送到 JS）。args: {modelId?, prompt, callback}
    void chat(const std::string& id, const json& args, WebViewWrapper* wv);
    // 获取运行状态。args: {modelId?}。带 modelId 返回单模型；不带返回 {models:[...]}。
    json getStatus(const json& args);
    // 获取资源占用。args: {modelId?}。带 modelId 返回单模型；不带返回 {models:[...]}。
    json getMetrics(const json& args);
    // 下载 llama-server 二进制
    void downloadServer(const std::string& id, const json& args, WebViewWrapper* wv);

private:
    // 一个运行中的模型：独立子进程 + 端口。
    struct RunningModel {
        std::string model_id;
        std::string gguf_path;
        LlamaParams params;
        int port = 0;
        std::unique_ptr<Subprocess> server;
        std::atomic<bool> running{false};
    };

    std::string getServerPath();
    std::string getExeDir();
    int getAvailablePort(const std::vector<int>& exclude);

    // 启动一个 llama-server 子进程，成功时通过 out_port 返回端口、out_server 返回进程。
    bool startServer(const std::string& gguf_path, const LlamaParams& params,
                     const std::vector<int>& excludePorts,
                     int& out_port, std::unique_ptr<Subprocess>& out_server);
    // 停止指定 modelId；modelId 为空停止全部。
    void stopServers(const std::string& modelId);

    // 解析参数对象中的 modelId（兼容 [{...}] 与 {...} 两种到达形式）。
    static const json& unwrapArgs(const json& args);

    // 单模型指标 JSON（含 modelId/port/资源占用）。须在锁外调用（CPU 采样）。
    json metricsFor(RunningModel* rm);

    // 流式 HTTP 请求（向指定端口的 llama-server 转发）
    bool streamingRequest(int port, const json& messages,
                          const std::string& callback_id, WebViewWrapper* wv);

    WebViewWrapper* m_wv = nullptr;
    std::mutex m_mutex;

    // 运行中的模型，按 modelId 索引
    std::map<std::string, std::unique_ptr<RunningModel>> m_models;
};
