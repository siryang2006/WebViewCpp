/* ================================================================
   service-panel.js — 侧边栏本地服务状态 + 资源监控
   监听 'model:started' / 'model:stopped' 更新状态栏；
   轮询 chatService.getMetrics 显示 CPU/内存/显存；
   维护 AppState.apiPort（chat.js 读取它判断能否对话）。
   ================================================================ */
(function() {
  function setStatus(running, modelName, apiPort) {
    var dot = $('serviceStatusDot');
    var statusText = $('serviceStatusText');
    var metrics = $('serviceMetrics');
    var modelInfo = $('modelRunningInfo');
    var modelNameEl = $('modelRunningName');
    var apiUrl = $('modelApiUrl');
    var apiRow = $('modelApiRow');

    if (running) {
      dot.className = 'status-dot';
      statusText.textContent = '运行中';
      metrics.style.display = '';
      if (modelName) {
        modelInfo.style.display = '';
        modelNameEl.textContent = modelName;
      }
      if (apiPort) {
        window.AppState.apiPort = apiPort;
        apiUrl.textContent = 'http://127.0.0.1:' + apiPort;
        apiRow.style.display = '';
      }
    } else {
      dot.className = 'status-dot stopped';
      statusText.textContent = '已停止';
      metrics.style.display = 'none';
      modelInfo.style.display = 'none';
      window.AppState.apiPort = 0;
    }
  }

  // 资源监控（真实数据，每 2 秒轮询）。
  // 后端 getMetrics() 无参返回 { status, models: [{modelId, memoryMB, ...}, ...] }。
  // 侧边栏展示首个运行模型的指标；同时把每个模型的指标写回 AppState.models[*].metrics，
  // 供模型列表行渲染（models.js 读取）。兼容旧的单对象结构。
  function applyMetricsToBars(d) {
    var cpu = Math.min(d.cpuPercent || 0, 100);
    var memMB = d.memoryMB || 0;
    var gpuMB = d.gpuMemoryMB || 0;
    var memPct = Math.min(memMB / (16 * 1024) * 100, 100);  // 假设系统 16GB

    $('cpuBar').style.width = cpu + '%';
    $('cpuBar').className = 'metric-bar-fill' + (cpu > 80 ? ' critical' : cpu > 60 ? ' high' : '');
    $('cpuVal').textContent = cpu.toFixed(1) + '%';

    $('memBar').style.width = memPct + '%';
    $('memBar').className = 'metric-bar-fill' + (memPct > 85 ? ' critical' : memPct > 70 ? ' high' : '');
    $('memVal').textContent = (memMB / 1024).toFixed(1) + ' GB';

    $('gpuBar').style.width = Math.min(gpuMB / 8192 * 100, 100) + '%';
    $('gpuBar').className = 'metric-bar-fill' + (gpuMB > 6000 ? ' critical' : gpuMB > 4000 ? ' high' : '');
    $('gpuVal').textContent = (gpuMB / 1024).toFixed(1) + ' GB';
  }

  function updateMetrics() {
    if (!window.chatService) return;
    window.chatService.getMetrics().then(function(r) {
      if (!r || !r.ok || !r.data) return;
      var d = r.data;

      // 多模型结构：把指标分发给 AppState.models，并用首个模型刷新侧边栏。
      if (Array.isArray(d.models)) {
        var byId = {};
        d.models.forEach(function(m) { byId[m.modelId] = m; });

        var changed = false;
        (window.AppState.models || []).forEach(function(m) {
          var mt = byId[m.id];
          if (mt && mt.status === 'ok') {
            m.metrics = { memoryMB: mt.memoryMB, cpuPercent: mt.cpuPercent,
                          gpuMemoryMB: mt.gpuMemoryMB, threads: mt.threads,
                          handles: mt.handles, pid: mt.pid, port: mt.port };
            changed = true;
          } else if (m.metrics) {
            delete m.metrics;
            changed = true;
          }
        });
        // 列表行指标刷新（仅当模型页可见时由 renderModels 输出）
        if (changed && window.renderModels) window.renderModels();
        // 详情页若打开了运行中的模型，刷新其资源面板
        if (window.refreshDetailMetrics) window.refreshDetailMetrics(byId);

        if (d.models.length > 0) {
          applyMetricsToBars(d.models[0]);
        }
        return;
      }

      // 兼容旧的单对象结构
      if (d.status === 'ok') applyMetricsToBars(d);
    }).catch(function() {}); // 2s 轮询，无模型运行时失败是常态，静默忽略避免噪音
  }
  setInterval(updateMetrics, 2000);

  // API 地址复制
  $('modelApiCopy').addEventListener('click', function() {
    var url = $('modelApiUrl').textContent;
    navigator.clipboard.writeText(url).then(function() {
      $('modelApiCopy').textContent = '✓';
      setTimeout(function() { $('modelApiCopy').textContent = '📋'; }, 1500);
    });
  });
  $('modelApiUrl').addEventListener('click', function() {
    $('modelApiCopy').click();
  });

  // 监听模型生命周期事件
  AppBus.on('model:started', function(d) { setStatus(true, d.name, d.port); });
  AppBus.on('model:stopped', function() {
    // 检查是否还有其他运行中的模型
    var running = (window.AppState.models || []).filter(function(m) { return m.status === 'running'; });
    if (running.length > 0) {
      setStatus(true, running[0].name || running[0].id, running[0].port || 0);
    } else {
      setStatus(false);
    }
  });

  // 启动时同步侧边栏状态（可能已有运行中的模型）
  if (window.AppState.models) {
    var running = window.AppState.models.filter(function(m) { return m.status === 'running'; });
    if (running.length > 0) {
      setStatus(true, running[0].name || running[0].id, running[0].port || 0);
    }
  }
})();
