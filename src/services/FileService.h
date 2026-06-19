#pragma once

#include "binding/CppObject.h"
#include "WebViewWrapper.h"
#include <thread>
#include <chrono>

// ============================================================
// 文件服务示例：演示异步方法
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
