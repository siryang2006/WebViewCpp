/**
 * ChatService - 大模型推理服务 JS 封装
 * 与 C++ ChatService 一一对应。
 *
 * 分工原则：C++ 提供基础能力（进程管理、HTTP 流式推理、资源采样），
 * JS 类负责业务逻辑（流式 token 回调的注册/清理、参数组装）。
 */
class ChatService {
    constructor() {
        this._nextId = 1;
        this._streamCallbacks = {};
    }

    // 启动模型：参数透传到 llama-server 命令行
    // params: { modelId, ggufPath, ctx, ngl, threads, flashAttn, thinking }
    startModel(params) {
        return window.__cpp__.chat.startModel({
            modelId: params.modelId,
            ggufPath: params.ggufPath,
            ctx: params.ctx || 4096,
            ngl: (params.ngl === undefined ? -1 : params.ngl),
            threads: params.threads || 4,
            flashAttn: params.flashAttn !== false,
            thinking: !!params.thinking
        });
    }

    // 停止模型。modelId 可选：传入停止指定模型，不传停止全部。
    stopModel(modelId) {
        return window.__cpp__.chat.stopModel(modelId ? { modelId: modelId } : {});
    }

    // 流式对话：onToken(token) 逐 token 回调，返回 Promise 在完成时 resolve
    // C++ 通过 callback id 回调 { token, done }，本方法负责注册/清理回调。
    // modelId 可选：指定向哪个运行中的模型推理；不传时若只有一个模型在运行则用它。
    chat(prompt, onToken, modelId) {
        var cbId = '__chat_cb_' + this._nextId++;
        var self = this;

        return new Promise(function(resolve, reject) {
            var cleanup = function() {
                delete self._streamCallbacks[cbId];
                if (window.__registered_cbs__) delete window.__registered_cbs__[cbId];
            };

            // 注册流式 token 回调
            window.__register_cb__(cbId, function(data) {
                if (data.done) {
                    cleanup();
                    resolve();
                    return;
                }
                if (data.token && onToken) {
                    onToken(data.token);
                }
            });

            // 发起推理。正常完成由 {done:true} 回调驱动 resolve；但兜底处理 RPC
            // 直接 resolve（C++ 已返回但未发 done 回调）的情况，避免 Promise 永挂、
            // 回调泄漏。cleanup/resolve 均幂等。
            var req = { prompt: prompt, callback: cbId };
            if (modelId) req.modelId = modelId;
            window.__cpp__.chat.chat(req)
                .then(function() {
                    cleanup();
                    resolve();
                })
                .catch(function(e) {
                    cleanup();
                    reject(e);
                });
        });
    }

    // 获取运行状态。modelId 可选：
    //   带 modelId → { status, modelId, port }
    //   不带      → { status, models: [{modelId, port, status}, ...] }
    getStatus(modelId) {
        return window.__cpp__.chat.getStatus(modelId ? { modelId: modelId } : {});
    }

    // 获取资源占用。modelId 可选：
    //   带 modelId → { status, modelId, port, memoryMB, cpuPercent, gpuMemoryMB, threads, handles }
    //   不带      → { status, models: [{modelId, memoryMB, cpuPercent, ...}, ...] }
    getMetrics(modelId) {
        return window.__cpp__.chat.getMetrics(modelId ? { modelId: modelId } : {});
    }

    // 获取所有运行中模型的指标，返回数组（封装无参 getMetrics）。
    getAllMetrics() {
        return window.__cpp__.chat.getMetrics({}).then(function(r) {
            if (r && r.ok && r.data && Array.isArray(r.data.models)) return r.data.models;
            return [];
        });
    }
}

window.chatService = new ChatService();
