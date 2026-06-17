#include "DownloadService.h"
#include "WebViewWrapper.h"
#include <curl/curl.h>
#include <fstream>
#include <iostream>
#include <direct.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#endif

DownloadService::DownloadService(const std::string& base_dir)
    : m_baseDir(base_dir) {
    curl_global_init(CURL_GLOBAL_ALL);

    bind_async("startDownload", [this](const std::string& id, const json& args, WebViewWrapper* wv) {
        startDownload(id, args, wv);
    });

    bind_sync("pauseDownload", [this](const json& args) -> json {
        return pauseDownload(args);
    });

    bind_sync("resumeDownload", [this](const json& args) -> json {
        return resumeDownload(args);
    });

    bind_sync("cancelDownload", [this](const json& args) -> json {
        return cancelDownload(args);
    });

    bind_sync("getProgress", [this](const json& args) -> json {
        return getProgress(args);
    });

    bind_sync("getSpeed", [this](const json& args) -> json {
        return getSpeed(args);
    });
}

DownloadService::~DownloadService() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [id, task] : m_tasks) {
            task->cancelled.store(true);
            {
                std::lock_guard<std::mutex> plock(task->pauseMutex);
                task->pausedFlag = false;
            }
            task->pauseCv.notify_all();
            if (task->thread.joinable()) {
                task->thread.join();
            }
        }
        m_tasks.clear();
    }
    curl_global_cleanup();
}

void DownloadService::on_created() {
    std::cout << "[DownloadService] created\n";
}

void DownloadService::on_destroyed() {
    std::cout << "[DownloadService] destroyed\n";
}

void DownloadService::setWebView(WebViewWrapper* wv) {
    m_wv = wv;
}

bool DownloadService::createDirectoryRecursive(const std::string& path) {
    if (path.empty()) return true;
    
    size_t start = 0;
    if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') {
        start = 2;
    } else if (path.size() >= 2 && path[1] == ':') {
        start = 2;
    }
    
    for (size_t i = start; i < path.size(); ++i) {
        if (path[i] == '\\' || path[i] == '/') {
            std::string subdir = path.substr(0, i);
            if (!subdir.empty()) {
                _mkdir(subdir.c_str());
            }
        }
    }
    
    _mkdir(path.c_str());
    
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

void DownloadService::startDownload(const std::string& id, const json& args, WebViewWrapper* wv) {
    if (!args.is_array() || args.empty()) {
        if (wv) wv->reject(id, "startDownload requires an object argument");
        return;
    }
    const json& params = args[0];
    if (!params.is_object()) {
        if (wv) wv->reject(id, "startDownload requires an object argument");
        return;
    }
    std::string url = params.value("url", "");
    std::string savePath = params.value("savePath", "");
    std::string modelId = params.value("modelId", "");
    long long totalSize = params.value("totalSize", 0LL);
    std::string callbackFn = params.value("callback", "");

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_tasks.count(modelId)) {
            auto& existing = m_tasks[modelId];
            if (!existing->cancelled.load() && !existing->completed.load()) {
                if (wv) wv->reject(id, "Download already in progress for model: " + modelId);
                return;
            }
            if (existing->thread.joinable()) {
                existing->thread.join();
            }
        }
    }

    std::string dir;
    size_t lastSep = savePath.find_last_of("/\\");
    if (lastSep != std::string::npos) {
        dir = savePath.substr(0, lastSep);
    }
    if (!dir.empty()) {
        createDirectoryRecursive(dir);
    }

    auto task = std::make_shared<DownloadTask>();
    task->modelId = modelId;
    task->url = url;
    task->savePath = savePath;
    task->callbackFn = callbackFn;
    task->totalSize = totalSize;
    task->wv = wv;
    task->service = this;
    task->lastSpeedTime = std::chrono::steady_clock::now();

    struct stat st;
    if (stat(savePath.c_str(), &st) == 0) {
        task->downloaded.store(st.st_size);
        task->lastDownloaded = st.st_size;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks[modelId] = task;
    }

    task->thread = std::thread(&DownloadService::downloadWorker, this, task);

    if (wv) wv->resolve(id, {{"ok", true}, {"modelId", modelId}});
}

json DownloadService::getModelIdFromArgs(const json& args) {
    if (args.is_array() && !args.empty() && args[0].is_object()) {
        return args[0].value("modelId", "");
    }
    if (args.is_object()) {
        return args.value("modelId", "");
    }
    return "";
}

json DownloadService::pauseDownload(const json& args) {
    std::string modelId = getModelIdFromArgs(args).get<std::string>();
    if (modelId.empty()) {
        return error_result(ErrorCode::INVALID_ARGUMENTS, "modelId is required");
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_tasks.count(modelId)) {
        return error_result(ErrorCode::INVALID_ARGUMENTS, "No download task found for: " + modelId);
    }

    auto& task = m_tasks[modelId];
    if (task->paused.load()) {
        return ok_result({{"status", "paused"}, {"message", "Already paused"}});
    }

    task->paused.store(true);
    {
        std::lock_guard<std::mutex> plock(task->pauseMutex);
        task->pausedFlag = true;
    }

    reportProgress(task);
    return ok_result({{"status", "paused"}, {"modelId", modelId}});
}

json DownloadService::resumeDownload(const json& args) {
    std::string modelId = getModelIdFromArgs(args).get<std::string>();
    if (modelId.empty()) {
        return error_result(ErrorCode::INVALID_ARGUMENTS, "modelId is required");
    }

    std::shared_ptr<DownloadTask> task;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_tasks.count(modelId)) {
            return error_result(ErrorCode::INVALID_ARGUMENTS, "No download task found for: " + modelId);
        }
        task = m_tasks[modelId];
    }

    if (!task->paused.load()) {
        return ok_result({{"status", "resumed"}, {"message", "Already running"}});
    }

    bool threadAlive = task->thread.joinable();

    task->paused.store(false);
    {
        std::lock_guard<std::mutex> plock(task->pauseMutex);
        task->pausedFlag = false;
    }
    task->pauseCv.notify_all();

    if (!threadAlive && !task->completed.load() && !task->cancelled.load()) {
        task->thread = std::thread(&DownloadService::downloadWorker, this, task);
    }

    reportProgress(task);
    return ok_result({{"status", "resumed"}, {"modelId", modelId}});
}

json DownloadService::cancelDownload(const json& args) {
    std::string modelId = getModelIdFromArgs(args).get<std::string>();
    if (modelId.empty()) {
        return error_result(ErrorCode::INVALID_ARGUMENTS, "modelId is required");
    }

    std::shared_ptr<DownloadTask> task;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_tasks.count(modelId)) {
            return error_result(ErrorCode::INVALID_ARGUMENTS, "No download task found for: " + modelId);
        }
        task = m_tasks[modelId];
    }

    task->cancelled.store(true);
    task->paused.store(false);
    {
        std::lock_guard<std::mutex> plock(task->pauseMutex);
        task->pausedFlag = false;
    }
    task->pauseCv.notify_all();

    if (task->curlHandle) {
        curl_easy_pause(static_cast<CURL*>(task->curlHandle), CURLPAUSE_CONT);
    }

    if (task->thread.joinable()) {
        task->thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.erase(modelId);
    }

    return ok_result({{"status", "cancelled"}, {"modelId", modelId}});
}

json DownloadService::getProgress(const json& args) {
    std::string modelId = getModelIdFromArgs(args).get<std::string>();
    if (modelId.empty()) {
        return error_result(ErrorCode::INVALID_ARGUMENTS, "modelId is required");
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_tasks.count(modelId)) {
        return error_result(ErrorCode::INVALID_ARGUMENTS, "No download task found for: " + modelId);
    }

    auto& task = m_tasks[modelId];
    long long downloaded = task->downloaded.load();
    long long total = task->totalSize;
    double percentage = (total > 0) ? (static_cast<double>(downloaded) / total * 100.0) : 0.0;

    std::string status = "downloading";
    if (task->cancelled.load()) status = "cancelled";
    else if (task->completed.load()) status = "completed";
    else if (task->paused.load()) status = "paused";

    return ok_result({
        {"downloaded", downloaded},
        {"total", total},
        {"percentage", percentage},
        {"status", status}
    });
}

json DownloadService::getSpeed(const json& args) {
    std::string modelId = getModelIdFromArgs(args).get<std::string>();
    if (modelId.empty()) {
        return error_result(ErrorCode::INVALID_ARGUMENTS, "modelId is required");
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_tasks.count(modelId)) {
        return error_result(ErrorCode::INVALID_ARGUMENTS, "No download task found for: " + modelId);
    }

    return ok_result({{"speed", m_tasks[modelId]->speed.load()}});
}

void DownloadService::downloadWorker(std::shared_ptr<DownloadTask> task) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        task->cancelled.store(true);
        reportProgress(task);
        return;
    }

    task->curlHandle = curl;

    long long existingSize = 0;
    struct stat st;
    if (stat(task->savePath.c_str(), &st) == 0) {
        existingSize = st.st_size;
    }

    FILE* fp = fopen(task->savePath.c_str(), existingSize > 0 ? "ab" : "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        task->curlHandle = nullptr;
        task->cancelled.store(true);
        reportProgress(task);
        return;
    }
    task->fileHandle = fp;

    curl_easy_setopt(curl, CURLOPT_URL, task->url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, task.get());
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, task.get());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FILETIME, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");

    if (existingSize > 0) {
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(existingSize));
    }

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    fclose(fp);
    task->fileHandle = nullptr;
    curl_easy_cleanup(curl);
    task->curlHandle = nullptr;

    if (res == CURLE_OK && httpCode == 200) {
        task->completed.store(true);
    } else if (task->cancelled.load()) {
        // cancelled
    } else if (task->paused.load()) {
        // paused
    } else {
        task->cancelled.store(true);
    }

    if (task->completed.load()) {
        std::cout << "[DownloadService] completed: " << task->modelId
                  << " (" << task->downloaded.load() << " bytes)" << std::endl;
    } else if (task->cancelled.load()) {
        std::cout << "[DownloadService] cancelled/failed: " << task->modelId
                  << " curl=" << res << " http=" << httpCode << std::endl;
    }

    reportProgress(task);
}

void DownloadService::reportProgress(std::shared_ptr<DownloadTask> task) {
    if (!m_wv || !task->wv) return;
    if (task->callbackFn.empty()) return;

    long long downloaded = task->downloaded.load();
    long long total = task->totalSize;
    double percentage = (total > 0) ? (static_cast<double>(downloaded) / total * 100.0) : 0.0;

    std::string status = "downloading";
    if (task->cancelled.load()) status = "cancelled";
    else if (task->completed.load()) status = "completed";
    else if (task->paused.load()) status = "paused";

    json progress = {
        {"modelId", task->modelId},
        {"downloaded", downloaded},
        {"total", total},
        {"percentage", percentage},
        {"speed", task->speed.load()},
        {"status", status}
    };

    m_wv->call_registered_js(task->callbackFn, progress);
}

size_t DownloadService::writeCallback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* task = static_cast<DownloadTask*>(userdata);

    if (task->cancelled.load()) {
        return 0;
    }

    while (task->paused.load() && !task->cancelled.load()) {
        std::unique_lock<std::mutex> lock(task->pauseMutex);
        task->pauseCv.wait_for(lock, std::chrono::milliseconds(100), [&task] {
            return !task->pausedFlag || task->cancelled.load();
        });
    }

    if (task->cancelled.load()) {
        return 0;
    }

    size_t bytes = size * nmemb;
    if (task->fileHandle) {
        size_t written = fwrite(ptr, size, nmemb, task->fileHandle);
        task->downloaded.fetch_add(written * size);
        return written * size;
    }
    return 0;
}

int DownloadService::progressCallback(void* clientp, long long dltotal, long long dlnow, long long ultotal, long long ulnow) {
    auto* task = static_cast<DownloadTask*>(clientp);

    if (task->cancelled.load()) {
        return 1;
    }

    while (task->paused.load() && !task->cancelled.load()) {
        std::unique_lock<std::mutex> lock(task->pauseMutex);
        task->pauseCv.wait_for(lock, std::chrono::milliseconds(100), [&task] {
            return !task->pausedFlag || task->cancelled.load();
        });
    }

    if (task->cancelled.load()) {
        return 1;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - task->lastSpeedTime).count();
    if (elapsed >= 1000) {
        long long currentDownloaded = task->downloaded.load();
        long long delta = currentDownloaded - task->lastDownloaded;
        if (delta > 0) {
            task->speed.store(delta * 1000 / elapsed);
        }
        task->lastDownloaded = currentDownloaded;
        task->lastSpeedTime = now;
        
        std::shared_ptr<DownloadTask> sharedTask;
        {
            std::lock_guard<std::mutex> lock(task->service->m_mutex);
            auto it = task->service->m_tasks.find(task->modelId);
            if (it != task->service->m_tasks.end()) {
                sharedTask = it->second;
            }
        }
        if (sharedTask) {
            task->service->reportProgress(sharedTask);
        }
    }

    return 0;
}
