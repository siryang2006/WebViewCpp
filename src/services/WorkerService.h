#pragma once

#include "binding/CppObject.h"
#include "WebViewWrapper.h"
#include <iostream>
#include <thread>
#include <chrono>

// ============================================================
// Worker 示例（支持 JS new 创建，演示 bind_factory + 实例生命周期）
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
