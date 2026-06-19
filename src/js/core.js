/* ================================================================
   core.js — 共享基础层
   提供：DOM 工具、格式化、共享状态 AppState、事件总线 AppBus
   所有功能模块依赖此文件，必须最先加载。
   ================================================================ */

// DOM 查询快捷方式（全局，使用极频繁）
var $ = function(id) { return document.getElementById(id); };

// HTML 转义（聊天消息、模型名等渲染前必经）
function escapeHtml(s) {
  return String(s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#039;').replace(/\n/g, '<br>');
}

// 下载速度格式化
function formatSpeed(bytesPerSec) {
  if (bytesPerSec >= 1073741824) return (bytesPerSec / 1073741824).toFixed(1) + ' GB/s';
  if (bytesPerSec >= 1048576) return (bytesPerSec / 1048576).toFixed(1) + ' MB/s';
  if (bytesPerSec >= 1024) return (bytesPerSec / 1024).toFixed(1) + ' KB/s';
  return bytesPerSec + ' B/s';
}

// 上下文长度档位（滑块 0-6 映射到 token 数）
var CTX_STEPS = [2048, 4096, 8192, 16384, 32768, 65536, 131072];

// "8K"/"32K"/"128K" -> token 数
function parseCtxToTokens(ctx) {
  if (!ctx) return 8192;
  var n = parseInt(ctx);
  if (String(ctx).toUpperCase().includes('K')) return n * 1024;
  return n;
}

// ================================================================
// 共享状态：跨模块只读/写这一个对象，避免散落的全局变量
// ================================================================
window.AppState = {
  models: [],     // 模型列表（models.js 维护）
  apiPort: 0       // 当前运行模型的 HTTP 端口（service-panel.js 维护，chat.js 读取）
};

// ================================================================
// 事件总线：跨模块生命周期通知，发布/订阅解耦
// 约定事件：
//   'model:started'  detail { id, name, port }  —— 模型启动成功
//   'model:stopped'  detail { id }              —— 模型停止
//   'models:changed'                            —— 模型列表数据变化，需重渲染
// ================================================================
window.AppBus = (function() {
  var target = new EventTarget();
  return {
    on: function(type, handler) {
      target.addEventListener(type, function(e) { handler(e.detail); });
    },
    emit: function(type, detail) {
      target.dispatchEvent(new CustomEvent(type, { detail: detail || {} }));
    }
  };
})();
