#include "DownloadService.h"
#include "WebViewWrapper.h"
#include <curl/curl.h>
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <direct.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#endif

DownloadService::DownloadService(const std::string& base_dir)
    : m_baseDir(base_dir) {
    // curl 全局初始化由 main/WinMain 统一调用一次，此处不再重复。

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
    // Copy task pointers out under the lock, then signal + join WITHOUT holding
    // m_mutex — progressCallback (on the worker thread) also locks m_mutex, so
    // joining while holding it would deadlock.
    std::vector<std::shared_ptr<DownloadTask>> tasks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [id, task] : m_tasks) {
            tasks.push_back(task);
        }
        m_tasks.clear();
    }
    for (auto& task : tasks) {
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
    // curl_global_cleanup 由 main/WinMain 统一调用。
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

    if (url.empty() || savePath.empty() || modelId.empty()) {
        if (wv) wv->reject(id, "startDownload requires url, savePath and modelId");
        return;
    }
    // Reject path traversal: savePath comes straight from JS and is joined onto
    // the app directory, so '..' segments could escape it and overwrite arbitrary files.
    if (savePath.find("..") != std::string::npos) {
        if (wv) wv->reject(id, "Invalid savePath");
        return;
    }

    // Retire any finished/cancelled task for this model first. Move the stale
    // thread out and join it AFTER releasing m_mutex (the worker's progressCallback
    // also locks m_mutex, so joining under the lock would deadlock).
    std::thread staleThread;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_tasks.find(modelId);
        if (it != m_tasks.end()) {
            auto& existing = it->second;
            if (!existing->cancelled.load() && !existing->completed.load()) {
                if (wv) wv->reject(id, "Download already in progress for model: " + modelId);
                return;
            }
            staleThread = std::move(existing->thread);
            m_tasks.erase(it);
        }
    }
    if (staleThread.joinable()) {
        staleThread.join();
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

    task->threadRunning.store(true);
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

    task->paused.store(false);
    {
        std::lock_guard<std::mutex> plock(task->pauseMutex);
        task->pausedFlag = false;
    }
    task->pauseCv.notify_all();

    // If the worker is no longer running (e.g. the connection dropped while paused),
    // restart it. threadRunning — not thread.joinable() — is the live signal: a
    // finished std::thread stays joinable until it's actually joined.
    if (!task->threadRunning.load() && !task->completed.load() && !task->cancelled.load()) {
        if (task->thread.joinable()) {
            task->thread.join();
        }
        task->threadRunning.store(true);
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

    // The write/progress callbacks both abort once `cancelled` is set, which ends
    // curl_easy_perform on its own. We deliberately do NOT touch task->curlHandle
    // here: the worker thread may be tearing it down concurrently, so reading it
    // from this thread would be a use-after-free race.
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
        task->threadRunning.store(false);
        reportProgress(task);
        return;
    }

    task->curlHandle.store(curl);

    // How many bytes are already on disk. We request a resume from here, but the
    // file is NOT opened yet — writeCallback opens it once it knows from the status
    // code whether the server actually honored the range (206) or is sending the
    // whole body from scratch (200). Appending a full body onto a partial file
    // would silently corrupt the download.
    long long existingSize = 0;
    struct stat st;
    if (stat(task->savePath.c_str(), &st) == 0) {
        existingSize = st.st_size;
    }

    // Run one curl transfer, optionally resuming from `resumeAt` bytes. Returns the
    // CURLcode; httpCode is written out. Factored out so we can retry from scratch.
    auto perform = [&](long long resumeAt) -> CURLcode {
        task->resumeFrom = resumeAt;
        task->httpStatus = 0;
        task->fileOpened = false;
        task->fileHandle = nullptr;

        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_URL, task->url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, task.get());
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, task.get());
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, task.get());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FILETIME, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
        if (resumeAt > 0) {
            curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(resumeAt));
        }

        CURLcode rc = curl_easy_perform(curl);
        if (task->fileHandle) {
            fclose(task->fileHandle);
            task->fileHandle = nullptr;
        }
        return rc;
    };

    CURLcode res = perform(existingSize);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    // CURLE_RANGE_ERROR (33): we asked to resume but the server doesn't support
    // ranges, so curl aborted before delivering any body. Reset the on-disk bytes
    // and retry once from offset 0 (full re-download).
    if (res == CURLE_RANGE_ERROR && existingSize > 0 &&
        !task->cancelled.load() && !task->paused.load()) {
        task->downloaded.store(0);
        task->lastDownloaded = 0;
        res = perform(0);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    }

    // Detach the handle BEFORE cleanup so nothing else can read a dangling pointer.
    task->curlHandle.store(nullptr);
    curl_easy_cleanup(curl);

    // 200 = full body, 206 = partial content (successful resume). Both, with a
    // clean curl result, mean the transfer finished. curl already enforces
    // Content-Length (CURLE_PARTIAL_FILE otherwise), so we don't double-check size.
    if (res == CURLE_OK && (httpCode == 200 || httpCode == 206)) {
        task->completed.store(true);
    } else if (task->cancelled.load()) {
        // cancelled by user
    } else if (task->paused.load()) {
        // paused — thread exits, resumeDownload() will respawn it
    } else {
        // network/server failure: leave the partial file on disk so a later
        // resume can continue, but do NOT mark completed.
        task->cancelled.store(true);
    }

    if (task->completed.load()) {
        std::cout << "[DownloadService] completed: " << task->modelId
                  << " (" << task->downloaded.load() << " bytes)" << std::endl;
    } else if (task->cancelled.load()) {
        std::cout << "[DownloadService] cancelled/failed: " << task->modelId
                  << " curl=" << res << " (" << curl_easy_strerror(res) << ") http=" << httpCode << std::endl;
    }

    task->threadRunning.store(false);
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

    // Open the file lazily on the first body byte, now that headerCallback has
    // recorded the status code. 206 => server honored our Range, so append to the
    // partial file. Anything else (200, redirected body, range ignored) => start
    // fresh by truncating, otherwise we'd corrupt the file by appending a full body.
    if (!task->fileOpened) {
        const char* mode = (task->httpStatus == 206 && task->resumeFrom > 0) ? "ab" : "wb";
        if (mode[0] == 'w') {
            task->downloaded.store(0);   // restarting from scratch
            task->lastDownloaded = 0;
        }
        task->fileHandle = fopen(task->savePath.c_str(), mode);
        task->fileOpened = true;
        if (!task->fileHandle) {
            return 0;  // abort the transfer; worker will mark it failed
        }
    }

    size_t bytes = size * nmemb;
    if (task->fileHandle) {
        size_t written = fwrite(ptr, 1, bytes, task->fileHandle);
        task->downloaded.fetch_add(static_cast<long long>(written));
        return written;
    }
    return 0;
}

size_t DownloadService::headerCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* task = static_cast<DownloadTask*>(userdata);
    size_t total = size * nitems;
    // Status line, e.g. "HTTP/1.1 206 Partial Content". On a redirect chain curl
    // delivers a status line per response; the last one wins, which is what we want.
    if (total >= 12 && buffer[0] == 'H' && buffer[1] == 'T' && buffer[2] == 'T' && buffer[3] == 'P') {
        const char* sp = static_cast<const char*>(memchr(buffer, ' ', total));
        if (sp && sp + 1 < buffer + total) {
            task->httpStatus = strtol(sp + 1, nullptr, 10);
        }
    }
    return total;
}

int DownloadService::progressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
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
        // Decay to 0 on a stalled interval instead of holding the last value.
        task->speed.store(delta > 0 ? (delta * 1000 / elapsed) : 0);
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
