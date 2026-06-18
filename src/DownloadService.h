#pragma once

#include "binding/CppObject.h"
#include <string>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <curl/curl.h>

struct curl_context_t;

class DownloadService : public CppObject {
public:
    DownloadService(const std::string& base_dir);
    ~DownloadService();

    std::string object_name() const override { return "download"; }
    void on_created() override;
    void on_destroyed() override;

    void setWebView(WebViewWrapper* wv);

    void startDownload(const std::string& id, const json& args, WebViewWrapper* wv);
    json pauseDownload(const json& args);
    json resumeDownload(const json& args);
    json cancelDownload(const json& args);
    json getProgress(const json& args);
    json getSpeed(const json& args);

private:
    struct DownloadTask {
        std::string modelId;
        std::string url;
        std::string savePath;
        std::string callbackFn;
        long long totalSize = 0;
        std::atomic<long long> downloaded{0};
        std::atomic<long long> speed{0};
        std::atomic<bool> paused{false};
        std::atomic<bool> cancelled{false};
        std::atomic<bool> completed{false};
        std::atomic<bool> threadRunning{false};
        std::thread thread;
        WebViewWrapper* wv = nullptr;
        DownloadService* service = nullptr;
        long long lastDownloaded = 0;
        std::chrono::steady_clock::time_point lastSpeedTime;
        std::mutex pauseMutex;
        std::condition_variable pauseCv;
        bool pausedFlag = false;
        std::atomic<CURL*> curlHandle{nullptr};
        FILE* fileHandle = nullptr;
        // Range handling: file is opened lazily inside writeCallback once we know
        // whether the server honored the resume request (HTTP 206) or not.
        long long resumeFrom = 0;       // bytes already on disk we asked to resume from
        long httpStatus = 0;            // status line code captured by headerCallback
        bool fileOpened = false;        // guards lazy open in writeCallback
    };

    void downloadWorker(std::shared_ptr<DownloadTask> task);
    void reportProgress(std::shared_ptr<DownloadTask> task);
    bool createDirectoryRecursive(const std::string& path);
    static json getModelIdFromArgs(const json& args);

    static size_t writeCallback(void* ptr, size_t size, size_t nmemb, void* userdata);
    static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata);
    static int progressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);

    std::string m_baseDir;
    WebViewWrapper* m_wv = nullptr;
    std::mutex m_mutex;
    std::map<std::string, std::shared_ptr<DownloadTask>> m_tasks;
};
