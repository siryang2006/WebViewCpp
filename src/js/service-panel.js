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

  // 资源监控（真实数据，每 2 秒轮询）
  function updateMetrics() {
    if (!window.chatService) return;
    window.chatService.getMetrics().then(function(r) {
      if (r && r.ok && r.data && r.data.status === 'ok') {
        var d = r.data;
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
    }).catch(function() {});
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
  AppBus.on('model:stopped', function() { setStatus(false); });
})();
