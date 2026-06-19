#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

// 资源占用指标
struct ProcessMetrics {
    uint64_t memoryMB = 0;      // 内存占用 (MB)
    double cpuPercent = 0.0;      // CPU 使用率 (%)
    uint64_t gpuMemoryMB = 0;   // GPU 显存占用 (MB, NVIDIA only)
    uint64_t threads = 0;       // 线程数
    uint64_t handleCount = 0;    // 句柄数
};

// 子进程管理器：启动、停止、监控
// Job Object 由 WebViewWrapper 在进程级别统一管理，
// 父进程退出时内核自动终止所有子进程。
class Subprocess {
public:
    using HealthCheckFn = std::function<bool()>;
    using OnExitFn = std::function<void(int exitCode)>;

    Subprocess();
    ~Subprocess();

    // 不可拷贝/移动
    Subprocess(const Subprocess&) = delete;
    Subprocess& operator=(const Subprocess&) = delete;

    // 启动进程
    bool start(const std::string& exePath,
               const std::string& args,
               const std::string& workDir = "",
               HealthCheckFn healthCheck = nullptr,
               int timeoutMs = 10000,
               OnExitFn onExit = nullptr);

    // 停止进程
    void stop(bool graceful = true);

    // 进程是否在运行
    bool isRunning() const { return m_running.load(); }

    // 获取进程 PID
    int pid() const { return m_pid; }
    // 获取退出码
    int exitCode() const { return m_exitCode; }

    // 获取最近一次失败的详细错误描述
    std::string lastError() const { return m_lastError; }


    // 获取资源占用（自动采样 CPU 百分比）
    bool getMetrics(ProcessMetrics& out);

    // 快速获取内存（不需要 CPU 采样）
    uint64_t getMemoryMB();

private:
    void monitorWorker();
    // 终止子进程；调用方必须已持有 m_mutex。
    void killProcessLocked(bool graceful);

#ifdef _WIN32
    HANDLE m_process = nullptr;
    HANDLE m_job = nullptr;
    DWORD m_pid = 0;

    // CPU 采样
    int64_t m_cpuTimeLast = 0;
    int64_t m_wallTimeLast = 0;
#endif
    std::atomic<bool> m_running{false};
    // 主动停止标志：为 true 时 monitor 线程跳过 onExit 回调，
    // 避免「停止时 join monitor」与「monitor 回调里再加锁」形成死锁。
    std::atomic<bool> m_intentionalStop{false};
    std::atomic<int> m_exitCode{0};
    std::thread m_monitor;
    OnExitFn m_onExit;
    std::string m_lastError;
    std::mutex m_mutex;
};
