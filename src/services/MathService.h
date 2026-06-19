#pragma once

#include "binding/CppObject.h"
#include "WebViewWrapper.h"
#include <iostream>
#include <thread>
#include <chrono>

// ============================================================
// 数学服务示例：演示同步/异步方法、属性、反向回调
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
