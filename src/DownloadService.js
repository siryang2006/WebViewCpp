/**
 * DownloadService - 下载服务JS封装
 * 与C++ DownloadService 完全一一对应
 */
class DownloadService {
    constructor() {
        this._nextId = 1;
        this._callbacks = {};
    }

    startDownload(params, callback) {
        var cbId = '__dl_cb_' + this._nextId++;

        this._callbacks[cbId] = function(data) {
            if (callback) callback(data);
            if (data.status === 'completed' || data.status === 'cancelled' || data.status === 'error') {
                delete window.downloadService._callbacks[cbId];
                delete window.__registered_cbs__[cbId];
            }
        };

        window.__register_cb__(cbId, function(data) {
            if (window.downloadService._callbacks[cbId]) {
                window.downloadService._callbacks[cbId](data);
            }
        });

        return window.__cpp__.download.startDownload({
            url: params.url,
            savePath: params.savePath,
            modelId: params.modelId,
            totalSize: params.totalSize || 0,
            callback: cbId
        });
    }

    pauseDownload(modelId) {
        return window.__cpp__.download.pauseDownload({ modelId: modelId });
    }

    resumeDownload(modelId) {
        return window.__cpp__.download.resumeDownload({ modelId: modelId });
    }

    cancelDownload(modelId) {
        return window.__cpp__.download.cancelDownload({ modelId: modelId });
    }

    getProgress(modelId) {
        return window.__cpp__.download.getProgress({ modelId: modelId });
    }

    getSpeed(modelId) {
        return window.__cpp__.download.getSpeed({ modelId: modelId });
    }
}

window.downloadService = new DownloadService();
