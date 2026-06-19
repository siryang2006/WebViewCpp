/* ================================================================
   config.js — 启动配置弹窗
   组装 llama-server 参数，经 window.chatService.startModel 启动。
   启动成功发 'model:started' 事件（service-panel 监听）。
   showConfig 挂在 window 上供行内 onclick / models.js 调用。
   ================================================================ */
(function() {
  var configModelId = null;
  var configThinking = false;
  var configFlashAttn = true;

  function findModel(id) {
    return window.AppState.models.find(function(x) { return x.id === id; });
  }

  function showConfig(id) {
    var m = findModel(id);
    if (!m) return;
    configModelId = id;
    configThinking = false;
    configFlashAttn = true;

    $('configModelName').textContent = m.name;

    // 上下文长度默认取模型 ctx 对应档位
    var ctxIdx = CTX_STEPS.indexOf(parseCtxToTokens(m.ctx));
    if (ctxIdx < 0) ctxIdx = 2; // 8192
    $('configCtxSlider').value = ctxIdx;
    $('configCtx').textContent = CTX_STEPS[ctxIdx];

    $('configNglSlider').value = -1;
    $('configNgl').textContent = '全部';

    $('configThreadsSlider').value = 4;
    $('configThreads').textContent = '4';

    $('configFlashAttn').className = 'config-toggle on';
    $('configThinking').className = 'config-toggle';

    updateCmdPreview();
    $('configOverlay').classList.add('show');
  }

  function getConfigParams() {
    var ctxIdx = parseInt($('configCtxSlider').value);
    return {
      ctx: CTX_STEPS[ctxIdx],
      ngl: parseInt($('configNglSlider').value),
      threads: parseInt($('configThreadsSlider').value),
      flashAttn: configFlashAttn,
      thinking: configThinking
    };
  }

  // 实时预览 llama-server 命令行
  function updateCmdPreview() {
    var p = getConfigParams();
    var parts = ['llama-server', '-m <model>', '-c ' + p.ctx, '-ngl ' + p.ngl, '-t ' + p.threads];
    if (p.flashAttn) parts.push('-fa');
    if (p.thinking) parts.push('--reasoning-format auto');
    $('configCmdPreview').textContent = parts.join(' ');
  }

  $('configCtxSlider').addEventListener('input', function(e) {
    $('configCtx').textContent = CTX_STEPS[parseInt(e.target.value)];
    updateCmdPreview();
  });
  $('configNglSlider').addEventListener('input', function(e) {
    var v = parseInt(e.target.value);
    $('configNgl').textContent = v === -1 ? '全部' : (v === 0 ? '纯CPU' : v);
    updateCmdPreview();
  });
  $('configThreadsSlider').addEventListener('input', function(e) {
    $('configThreads').textContent = e.target.value;
    updateCmdPreview();
  });
  $('configFlashAttn').addEventListener('click', function() {
    configFlashAttn = !configFlashAttn;
    $('configFlashAttn').className = 'config-toggle' + (configFlashAttn ? ' on' : '');
    updateCmdPreview();
  });
  $('configThinking').addEventListener('click', function() {
    configThinking = !configThinking;
    $('configThinking').className = 'config-toggle' + (configThinking ? ' on' : '');
    updateCmdPreview();
  });
  $('configCancel').addEventListener('click', function() {
    $('configOverlay').classList.remove('show');
  });
  $('configConfirm').addEventListener('click', function() {
    var m = findModel(configModelId);
    if (!m) return;

    $('configOverlay').classList.remove('show');
    m.status = 'running';
    AppBus.emit('models:changed');

    var p = getConfigParams();

    if (!window.chatService) return;
    window.chatService.startModel({
      modelId: m.id,
      ggufPath: m.gguf_path,
      ctx: p.ctx,
      ngl: p.ngl,
      threads: p.threads,
      flashAttn: p.flashAttn,
      thinking: p.thinking
    }).then(function(r) {
      if (r && r.ok && r.data && r.data.status === 'need_download') {
        alert('请先下载 llama-server.exe 放到 exe 同目录\n下载地址: https://github.com/ggml-org/llama.cpp/releases');
        m.status = 'downloaded';
        AppBus.emit('models:changed');
      } else if (r && r.ok) {
        AppBus.emit('model:started', { id: m.id, name: m.name, port: r.data.port });
      } else {
        if (r && r.message) alert('启动失败: ' + r.message);
        m.status = 'downloaded';
        AppBus.emit('models:changed');
        AppBus.emit('model:stopped', { id: m.id });
      }
    }).catch(function(e) {
      alert('启动失败: ' + e);
      m.status = 'downloaded';
      AppBus.emit('models:changed');
      AppBus.emit('model:stopped', { id: m.id });
    });
  });

  // 行内 onclick / models.js 依赖
  window.showConfig = showConfig;
})();
