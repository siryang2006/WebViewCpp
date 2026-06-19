#include "Subprocess.h"
#include <iostream>
#include <sstream>
#include <psapi.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <tlhelp32.h>
#pragma comment(lib, "psapi.lib")
#include <mutex>
#endif

Subprocess::Subprocess() {}

Subprocess::~Subprocess() {
    stop(false);  // stop() 内部已 join monitor
#ifdef _WIN32
    if (m_job) CloseHandle(m_job);
    if (m_process) CloseHandle(m_process);
#endif
}

bool Subprocess::start(const std::string& exePath,
                       const std::string& args,
                       const std::string& workDir,
                       HealthCheckFn healthCheck,
                       int timeoutMs,
                       OnExitFn onExit) {
    // 先停止任何已有进程（在锁外，stop 内部会 join 旧 monitor 线程）。
    stop(false);

    std::lock_guard<std::mutex> lock(m_mutex);

    m_lastError.clear();
    m_onExit = onExit;
    m_exitCode = 0;
    m_intentionalStop = false;

#ifdef _WIN32
    // 旧 monitor 线程必须在重新赋值前 join，否则 std::thread::operator= 会 terminate。
    if (m_monitor.joinable()) m_monitor.join();
    if (m_process) { CloseHandle(m_process); m_process = nullptr; }

    std::string cmd = "\"" + exePath + "\" " + args;
    const char* cwd = workDir.empty() ? nullptr : workDir.c_str();

    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    BOOL success = CreateProcessA(
        nullptr, (LPSTR)cmd.c_str(),
        nullptr, nullptr,
        FALSE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
        nullptr, cwd,
        &si, &pi
    );

    if (!success) {
        DWORD err = GetLastError();
        m_lastError = "CreateProcessA failed: error " + std::to_string(err);
        std::cerr << "[Subprocess] Failed to start " << exePath << ": " << err << "\n";
        return false;
    }

    m_process = pi.hProcess;
    m_pid = pi.dwProcessId;
    m_running = true;
    m_cpuTimeLast = 0;
    m_wallTimeLast = 0;
    CloseHandle(pi.hThread);

    m_monitor = std::thread(&Subprocess::monitorWorker, this);

    if (healthCheck && timeoutMs > 0) {
        int retries = timeoutMs / 200;
        while (retries-- > 0) {
            Sleep(200);

            DWORD exitCode = 0;
            if (GetExitCodeProcess(m_process, &exitCode) && exitCode != STILL_ACTIVE) {
                m_exitCode = (int)exitCode;
                m_running = false;
                m_lastError = "Process exited early with code " + std::to_string(exitCode);
                std::cerr << "[Subprocess] " << exePath << " exited early (code=" << exitCode << ")\n";
                return false;
            }

            if (healthCheck()) {
                std::cout << "[Subprocess] " << exePath << " started (pid=" << m_pid << ")\n";
                return true;
            }
        }
        // 健康检查超时：标记主动停止并杀进程（仍持有锁，用 *Locked 版本）。
        m_intentionalStop = true;
        m_lastError = "Health check timed out after " + std::to_string(timeoutMs) + "ms";
        std::cerr << "[Subprocess] " << exePath << " health check timed out after " << timeoutMs << "ms\n";
        killProcessLocked(false);
        return false;
    }

    return true;
#else
    return false;
#endif
}

// 终止子进程；调用方必须已持有 m_mutex。不 join monitor（由 stop 在锁外做）。
void Subprocess::killProcessLocked(bool graceful) {
    if (!m_running.load()) return;
#ifdef _WIN32
    if (m_process) {
        if (graceful) {
            // 信号必须发往子进程的进程组（创建时用了 CREATE_NEW_PROCESS_GROUP，
            // 组 id == 子进程 pid）。传 0 会发给调用方自己的进程组。
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, m_pid);
            if (WaitForSingleObject(m_process, 5000) == WAIT_TIMEOUT) {
                TerminateProcess(m_process, -1);
            }
        } else {
            TerminateProcess(m_process, -1);
        }
    }
#endif
    m_running = false;
}

void Subprocess::stop(bool graceful) {
    std::thread monitorToJoin;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 标记主动停止：monitor 线程醒来后会跳过 onExit，避免回调里再抢 m_mutex
        // 与本函数随后 join monitor 形成死锁。
        m_intentionalStop = true;
        killProcessLocked(graceful);
        // 把 monitor 线程移出，锁释放后再 join（join 期间不持锁）。
        monitorToJoin = std::move(m_monitor);
    }
    if (monitorToJoin.joinable()) {
        monitorToJoin.join();
    }
}

void Subprocess::monitorWorker() {
#ifdef _WIN32
    if (m_process) {
        WaitForSingleObject(m_process, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(m_process, &exitCode);
        m_exitCode = (int)exitCode;
        m_running = false;

        // 仅在「非主动停止」（即进程自行崩溃/退出）时回调，避免与 stop() 的
        // join 形成死锁，也避免主动停止时误报异常退出。
        if (!m_intentionalStop.load() && m_onExit) {
            m_onExit(exitCode);
        }
    }
#endif
}

bool Subprocess::getMetrics(ProcessMetrics& out) {
    out = {};

#ifdef _WIN32
    if (!m_process || !m_running.load()) return false;

    // 内存
    PROCESS_MEMORY_COUNTERS mem = {};
    if (GetProcessMemoryInfo(m_process, &mem, sizeof(mem))) {
        out.memoryMB = mem.WorkingSetSize / (1024 * 1024);
    }

    // 线程数、句柄数
    out.threads = 0;
    out.handleCount = 0;
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te = {};
            te.dwSize = sizeof(te);
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == m_pid) out.threads++;
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
    }
    {
        DWORD handles = 0;
        if (GetProcessHandleCount(m_process, &handles)) {
            out.handleCount = handles;
        }
    }

    // CPU 使用率（采样两次计算差值）
    FILETIME ftKernel, ftUser, ftNow;
    GetSystemTimeAsFileTime(&ftNow);

    FILETIME ftCreation, ftExit;
    if (GetProcessTimes(m_process, &ftCreation, &ftExit, &ftKernel, &ftUser)) {
        int64_t cpuTime = ((int64_t)ftKernel.dwHighDateTime << 32 | ftKernel.dwLowDateTime)
                        + ((int64_t)ftUser.dwHighDateTime << 32 | ftUser.dwLowDateTime);

        int64_t wallTime = ((int64_t)ftNow.dwHighDateTime << 32 | ftNow.dwLowDateTime);

        if (m_cpuTimeLast > 0 && m_wallTimeLast > 0) {
            int64_t cpuDelta = cpuTime - m_cpuTimeLast;
            int64_t wallDelta = wallTime - m_wallTimeLast;
            if (wallDelta > 0) {
                // 乘 100 得到百分比，乘以 CPU 核心数补偿
                SYSTEM_INFO si = {};
                GetSystemInfo(&si);
                out.cpuPercent = (double)cpuDelta / wallDelta * 100.0 * si.dwNumberOfProcessors;
                if (out.cpuPercent > 100.0) out.cpuPercent = 100.0;
            }
        }

        m_cpuTimeLast = cpuTime;
        m_wallTimeLast = wallTime;
    }

    // GPU 显存（NVIDIA via nvidia-smi）
    {
        static bool nvidiaChecked = false;
        static bool nvidiaAvailable = false;
        static std::mutex nvidiaMutex;
        std::lock_guard<std::mutex> lock(nvidiaMutex);
        if (!nvidiaChecked) {
            nvidiaChecked = true;
            // 检查 NVIDIA 驱动路径
            const char* nvidiaPath = "C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvidia-smi.exe";
            if (GetFileAttributesA(nvidiaPath) != INVALID_FILE_ATTRIBUTES) {
                nvidiaAvailable = true;
            }
        }
        if (nvidiaAvailable && m_pid > 0) {
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "nvidia-smi.exe --query-compute-apps=pid,used_memory --format=csv,noheader,nounits -c %d", m_pid);
            FILE* fp = _popen(cmd, "r");
            if (fp) {
                char buf[128] = {};
                if (fgets(buf, sizeof(buf), fp)) {
                    unsigned int mb = 0;
                    if (sscanf(buf, "%*d, %u", &mb) == 1) {
                        out.gpuMemoryMB = mb;
                    }
                }
                _pclose(fp);
            }
        }
    }

    return true;
#else
    return false;
#endif
}

uint64_t Subprocess::getMemoryMB() {
#ifdef _WIN32
    if (!m_process || !m_running.load()) return 0;

    PROCESS_MEMORY_COUNTERS mem = {};
    if (GetProcessMemoryInfo(m_process, &mem, sizeof(mem))) {
        return mem.WorkingSetSize / (1024 * 1024);
    }
#endif
    return 0;
}
