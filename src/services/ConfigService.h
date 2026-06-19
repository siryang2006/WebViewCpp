#pragma once

#include "binding/CppObject.h"
#include "WebViewWrapper.h"
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#endif

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
                // 安全检查：禁止路径穿越
                if (path.find("..") != std::string::npos) { wv->reject(id, "Invalid path"); return; }

                // 构建完整路径并规范化（统一用反斜杠）
                std::string fullPath = dir + "\\" + path;
                std::replace(fullPath.begin(), fullPath.end(), '/', '\\');

                // 使用 Windows API 验证路径是否在 base_dir 内
                char baseBuf[MAX_PATH];
                GetFullPathNameA(dir.c_str(), MAX_PATH, baseBuf, nullptr);
                char fullBuf[MAX_PATH];
                GetFullPathNameA(fullPath.c_str(), MAX_PATH, fullBuf, nullptr);

                std::string baseNorm = baseBuf;
                std::string fullNorm = fullBuf;
                // 确保 baseNorm 以反斜杠结尾用于前缀匹配
                if (!baseNorm.empty() && baseNorm.back() != '\\') baseNorm += '\\';

                // 检查完整路径是否以 baseDir 为前缀（防止 a/..b 类型的绕过）
                if (fullNorm.find(baseNorm) != 0) {
                    wv->reject(id, "Invalid path"); return;
                }

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
