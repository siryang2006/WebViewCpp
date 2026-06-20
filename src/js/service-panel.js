(function() {
  var curModelId = null;
  var modelPorts = {};

  function getRunningModels() {
    return (window.AppState.models || []).filter(function(m) { return m.status === 'running'; });
  }

  function populateDropdown() {
    var sel = $('modelRunningSelect');
    var running = getRunningModels();
    if (running.length === 0) {
      sel.innerHTML = '<option value="">— 无运行模型 —</option>';
      curModelId = null;
      $('stopModelBtn').style.display = 'none';
      return;
    }
    var cur = sel.value;
    sel.innerHTML = '';
    for (var i = 0; i < running.length; i++) {
      var opt = document.createElement('option');
      opt.value = running[i].id;
      var port = running[i].port || modelPorts[running[i].id] || '';
      opt.textContent = running[i].name + (port ? ' :' + port : '');
      sel.appendChild(opt);
    }
    if (cur && running.some(function(m) { return m.id === cur; })) {
      sel.value = cur;
    }
    curModelId = sel.value;
    showModelDetail(curModelId);
  }

  function showModelDetail(modelId) {
    if (!modelId) {
      $('modelApiUrl').textContent = '-';
      $('stopModelBtn').style.display = 'none';
      $('serviceMetrics').style.display = 'none';
      return;
    }
    var m = (window.AppState.models || []).find(function(x) { return x.id === modelId; });
    if (!m) return;
    var port = m.port || modelPorts[modelId] || 0;
    window.AppState.apiPort = port;
    $('modelApiUrl').textContent = port ? 'http://127.0.0.1:' + port : '-';
    $('stopModelBtn').style.display = '';
  }

  function applyMetricsToBars(d) {
    var cpu = Math.min(d.cpuPercent || 0, 100);
    var memMB = d.memoryMB || 0;
    var gpuMB = d.gpuMemoryMB || 0;
    var memPct = Math.min(memMB / (16 * 1024) * 100, 100);

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

  function refreshPanel() {
    var running = getRunningModels();
    if (running.length === 0) {
      $('serviceStatusDot').className = 'status-dot stopped';
      $('serviceStatusText').textContent = '已停止';
      $('serviceMetrics').style.display = 'none';
      $('modelRunningInfo').style.display = 'none';
      window.AppState.apiPort = 0;
    } else {
      $('serviceStatusDot').className = 'status-dot';
      $('serviceStatusText').textContent = '运行中 (' + running.length + ')';
      $('serviceMetrics').style.display = '';
      $('modelRunningInfo').style.display = '';
      populateDropdown();
    }
  }

  function updateMetrics() {
    if (!window.chatService) return;
    window.chatService.getMetrics().then(function(r) {
      if (!r || !r.ok || !r.data) return;
      var d = r.data;

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
        if (changed && window.renderModels) window.renderModels();
        if (window.refreshDetailMetrics) window.refreshDetailMetrics(byId);

        if (curModelId && byId[curModelId] && byId[curModelId].status === 'ok') {
          applyMetricsToBars(byId[curModelId]);
          $('serviceMetrics').style.display = '';
        } else if (d.models.length > 0) {
          applyMetricsToBars(d.models[0]);
          $('serviceMetrics').style.display = '';
        } else {
          $('serviceMetrics').style.display = 'none';
        }
        return;
      }

      if (d.status === 'ok') {
        applyMetricsToBars(d);
        $('serviceMetrics').style.display = '';
      }
    }).catch(function() {});
  }
  setInterval(updateMetrics, 1000);

  $('modelRunningSelect').addEventListener('change', function() {
    curModelId = this.value;
    showModelDetail(curModelId);
  });

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

  $('stopModelBtn').addEventListener('click', function() {
    if (!curModelId) return;
    if (!window.chatService) return;
    window.chatService.stopModel(curModelId).then(function() {
      var m = (window.AppState.models || []).find(function(x) { return x.id === curModelId; });
      if (m) { m.status = 'downloaded'; delete m.port; }
      AppBus.emit('model:stopped', { id: curModelId });
    }).catch(function(e) {
      alert('停止失败: ' + e);
    });
  });

  AppBus.on('model:started', function(d) {
    if (d && d.id && d.port) modelPorts[d.id] = d.port;
    refreshPanel();
  });
  AppBus.on('model:stopped', function(d) {
    if (d && d.id) delete modelPorts[d.id];
    refreshPanel();
  });

  refreshPanel();
})();
