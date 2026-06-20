/* ================================================================
   models.js — 模型列表 / 下载 / 详情页
   数据存于 AppState.models；下载经 window.downloadService；
   配置/删除经 window.__cpp__.config。停止模型发 'model:stopped' 事件。
   行内 onclick 调用的函数挂在 window 上（startDownload 等）。
   ================================================================ */
(function() {
  var modelList = $('modelList');
  var modelSearchInput = $('modelSearchInput');
  var expandFilterBtn = $('expandFilterBtn');
  var filterPanel = $('filterPanel');
  var dl = window.downloadService;

  // 筛选状态
  var filterState = { param: 'all', type: 'all' };

  // 把内存(MB)格式化为 MB/GB
  function fmtMem(mb) {
    if (!mb) return '0 MB';
    return mb >= 1024 ? (mb / 1024).toFixed(1) + ' GB' : Math.round(mb) + ' MB';
  }

  // 运行中模型行的精简指标徽标：💾 内存 · ⚡ CPU · 🎮 显存
  function formatRowMetrics(mt) {
    if (!mt) return '';
    var parts = [
      '<span class="row-metric" title="内存">💾 ' + fmtMem(mt.memoryMB) + '</span>',
      '<span class="row-metric" title="CPU">⚡ ' + (mt.cpuPercent || 0).toFixed(0) + '%</span>'
    ];
    if (mt.gpuMemoryMB) parts.push('<span class="row-metric" title="显存">🎮 ' + fmtMem(mt.gpuMemoryMB) + '</span>');
    return parts.join('');
  }

  function renderModels() {
    var models = window.AppState.models;
    var q = modelSearchInput.value.trim().toLowerCase();
    var filtered = models.filter(function(m) {
      if (q && !m.name.toLowerCase().includes(q) && !m.desc.toLowerCase().includes(q)) return false;
      return true;
    });

    if (!filtered.length) {
      modelList.innerHTML = '<div style="text-align:center;padding:48px;color:var(--text-muted);font-size:13px;">🔍 没有找到符合条件的模型</div>';
      return;
    }

    modelList.innerHTML = filtered.map(function(m) {
      var isRunning = m.status === 'running';
      var isDownloaded = m.status === 'downloaded';
      var isDownloading = m.status === 'downloading';
      // 没有status字段或status为'available'表示可以下载
      var isAvailable = !m.status || m.status === 'available';
      // 转义用户可控字段（名称/描述/大小/类型来自「添加模型」表单，可能含 < ' " 等）。
      var name = escapeHtml(m.name);
      var desc = escapeHtml(m.desc);
      var size = escapeHtml(m.size);
      var idA = escapeHtml(m.id);                       // 用于 HTML 属性（id="..."）
      var idJs = escapeHtml(m.id).replace(/'/g, "\\'");  // 用于 onclick 单引号字符串
      return '<div class="model-row ' + (isRunning ? 'selected' : '') + '">' +
        '<div class="model-check ' + (isRunning ? 'on' : '') + '" id="ck-' + idA + '">' + (isRunning ? '✓' : '') + '</div>' +
        '<div class="model-row-icon ' + (isRunning ? 'running' : '') + '">' + (isRunning ? '🚀' : isDownloaded ? '✅' : '📦') + '</div>' +
        '<div class="model-row-info">' +
          '<div class="model-row-name">' + name + '</div>' +
          '<div class="model-row-meta">' + desc + '</div>' +
          '<div class="model-row-tags">' +
            '<span class="model-row-tag gb">' + size + '</span>' +
          '</div>' +
          (isRunning && m.metrics ?
            '<div class="model-row-metrics" id="rowmetrics-' + idA + '">' + formatRowMetrics(m.metrics) + '</div>' : '') +
        '</div>' +
        (isRunning ? '<div class="model-row-status"><span class="status-dot-sm"></span>运行中</div>' : '') +
        '<div class="model-row-actions">' +
          (isRunning ? '<button class="btn btn-red" onclick="stopModel(\'' + idJs + '\')">停止</button>' : '') +
          (isDownloaded ? '<button class="btn btn-green" onclick="showConfig(\'' + idJs + '\')">▶ 立即启动</button>' +
            '<button class="btn btn-ghost" onclick="deleteModel(\'' + idJs + '\')" title="删除">🗑</button>' : '') +
          (isDownloading ?
            '<div class="model-dl-info">' +
              '<div class="model-dl-bar"><div class="model-dl-fill" id="dlbar-' + idA + '" style="width:' + m.progress + '%"></div></div>' +
              '<div class="model-dl-speed">' + formatSpeed(m.speed || 0) + '</div>' +
            '</div>' +
            '<button class="btn btn-ghost" onclick="pauseDownload(\'' + idJs + '\')" title="暂停">⏸</button>' +
            '<button class="btn btn-ghost" onclick="cancelDownload(\'' + idJs + '\')" title="取消">✕</button>' : '') +
          (m.status === 'paused' ?
            '<div class="model-dl-info">' +
              '<div class="model-dl-bar"><div class="model-dl-fill" id="dlbar-' + idA + '" style="width:' + m.progress + '%"></div></div>' +
              '<div class="model-dl-speed">已暂停</div>' +
            '</div>' +
            '<button class="btn btn-ghost" onclick="resumeDownload(\'' + idJs + '\')" title="继续">▶</button>' +
            '<button class="btn btn-ghost" onclick="cancelDownload(\'' + idJs + '\')" title="取消">✕</button>' : '') +
          (isAvailable ? '<button class="btn btn-blue" onclick="startDownload(\'' + idJs + '\')">⬇ 下载模型</button>' : '') +
          '<button class="btn btn-ghost" onclick="showDetail(\'' + idJs + '\')" title="详情">⋯</button>' +
        '</div>' +
      '</div>';
    }).join('');
  }

  function findModel(id) {
    return window.AppState.models.find(function(x) { return x.id === id; });
  }

  /* ---- 下载操作 ---- */
  function startDownload(id) {
    var m = findModel(id);
    if (!m || !m.download_url) return;

    dl.getFileSize(m.download_url).then(function(r) {
      if (r && r.ok && r.data && r.data.size > 0) {
        m.size_bytes = r.data.size;
      }
    }).catch(function() {}).then(function() {
      m.status = 'downloading';
      m.progress = 0;
      m.downloaded = 0;
      m.speed = 0;
      renderModels();

      dl.startDownload({
        url: m.download_url,
        savePath: m.gguf_path || 'downloads/' + id + '/' + m.download_url.split('/').pop(),
        modelId: id,
        totalSize: m.size_bytes || 0
      }, function(data) {
        m.progress = data.percentage || 0;
        m.downloaded = data.downloaded || 0;
        m.total = data.total || 0;
        m.speed = data.speed || 0;

        if (data.status === 'completed') {
          m.status = 'downloaded';
          m.progress = 100;
        } else if ((data.status === 'cancelled' || data.status === 'error') && !m._pausing) {
          m.status = 'available';
          m.progress = 0;
          m.downloaded = 0;
          m.speed = 0;
        }
        renderModels();
      });
    });
  }

  function pauseDownload(id) {
    var m = findModel(id);
    if (!m) return;
    m.status = 'paused';
    m._pausing = true;
    renderModels();
    dl.pauseDownload(id).then(function(r) {
      m._pausing = false;
      if (r && r.ok === false) { m.status = 'downloading'; renderModels(); }
    }).catch(function() {
      m._pausing = false;
      m.status = 'downloading';
      renderModels();
    });
  }

  function resumeDownload(id) {
    var m = findModel(id);
    if (!m) return;
    dl.resumeDownload(id).then(function(r) {
      m.status = (r && r.ok === false) ? 'paused' : 'downloading';
      renderModels();
    }).catch(function() {
      m.status = 'paused';
      renderModels();
    });
    m.status = 'downloading';
    renderModels();
  }

  function cancelDownload(id) {
    dl.cancelDownload(id);
    var m = findModel(id);
    if (m) {
      m.status = 'available';
      m.progress = 0;
      m.downloaded = 0;
      m.speed = 0;
    }
    renderModels();
  }

  /* ---- 停止运行中的模型 ---- */
  function stopModel(id) {
    var m = findModel(id);
    if (m) { m.status = 'downloaded'; delete m.metrics; }
    renderModels();
    AppBus.emit('model:stopped', { id: id });
    if (window.chatService) {
      // 传入 id 仅停止该模型（支持多模型并发）
      window.chatService.stopModel(id).catch(function(e) {
        console.error('[models] stopModel failed:', id, e);
      });
    }
  }

  /* ---- 添加/编辑模型 ---- */
  var _editModeId = null;

  function showAddModel() {
    _editModeId = null;
    $('addModelTitle').textContent = '➕ 添加模型';
    $('addModelConfirm').textContent = '确认添加';
    $('addModelId').value = '';
    $('addModelName').value = '';
    $('addModelUrl').value = '';
    $('addModelDesc').value = '';
    $('addModelSize').value = '';
    $('addModelSizeBytes').value = '';
    $('addModelParam').value = '';
    $('addModelType').value = 'Other';
    $('addModelCtx').value = '32K';
    $('addModelBackend').value = 'llama-server';
    $('addModelOverlay').classList.add('show');
  }

  function showEditModel(id) {
    var m = findModel(id);
    if (!m) return;
    _editModeId = id;
    $('addModelTitle').textContent = '✏️ 编辑模型';
    $('addModelConfirm').textContent = '保存修改';
    $('addModelId').value = m.id;
    $('addModelId').disabled = true;
    $('addModelName').value = m.name || '';
    $('addModelUrl').value = m.download_url || '';
    $('addModelDesc').value = m.desc || '';
    $('addModelSize').value = m.size || '';
    $('addModelSizeBytes').value = m.size_bytes || '';
    $('addModelParam').value = m.param || '';
    $('addModelType').value = m.type || 'Other';
    $('addModelCtx').value = m.ctx || '32K';
    $('addModelBackend').value = m.backend || 'llama-server';
    $('addModelOverlay').classList.add('show');
  }

  $('addModelBtn').addEventListener('click', showAddModel);
  $('addModelCancel').addEventListener('click', function() {
    $('addModelOverlay').classList.remove('show');
    $('addModelId').disabled = false;
  });
  $('addModelConfirm').addEventListener('click', function() {
    var id = $('addModelId').value.trim();
    var url = $('addModelUrl').value.trim();
    if (!id || !url) { alert('模型 ID 和下载 URL 为必填项'); return; }
    if (_editModeId) {
      var updates = {
        id: id,
        name: $('addModelName').value.trim() || id,
        download_url: url,
        desc: $('addModelDesc').value.trim() || '',
        size: $('addModelSize').value.trim() || 'Unknown',
        size_bytes: parseInt($('addModelSizeBytes').value) || 0,
        param: parseFloat($('addModelParam').value) || 0,
        type: $('addModelType').value,
        ctx: $('addModelCtx').value,
        backend: $('addModelBackend').value
      };
      window.__cpp__.config.updateModel(updates).then(function(data) {
        $('addModelOverlay').classList.remove('show');
        $('addModelId').disabled = false;
        window.AppState.models = (data.models || []).map(function(m) {
          m.progress = (m.status === 'downloaded' || m.status === 'running') ? 100 : 0;
          return m;
        });
        renderModels();
        // 重新打开详情页显示更新后的数据
        var savedId = _editModeId;
        _editModeId = null;
        showDetail(savedId);
      }).catch(function(e) {
        alert('编辑模型失败: ' + e);
      });
    } else {
      var model = {
        id: id,
        name: $('addModelName').value.trim() || id,
        download_url: url,
        desc: $('addModelDesc').value.trim() || '',
        size: $('addModelSize').value.trim() || 'Unknown',
        size_bytes: parseInt($('addModelSizeBytes').value) || 0,
        param: parseFloat($('addModelParam').value) || 0,
        type: $('addModelType').value,
        ctx: $('addModelCtx').value,
        backend: $('addModelBackend').value
      };
      window.__cpp__.config.addModel(model).then(function(data) {
        $('addModelOverlay').classList.remove('show');
        window.AppState.models = (data.models || []).map(function(m) {
          m.progress = (m.status === 'downloaded' || m.status === 'running') ? 100 : 0;
          return m;
        });
        renderModels();
      }).catch(function(e) {
        alert('添加模型失败: ' + e);
      });
    }
  });

  /* ---- 删除模型（确认弹窗）---- */
  function confirmDialog(msg, onOk) {
    $('confirmMessage').textContent = msg;
    $('confirmOverlay').classList.add('show');
    var okHandler = function() {
      $('confirmOverlay').classList.remove('show');
      $('confirmOk').removeEventListener('click', okHandler);
      $('confirmCancel').removeEventListener('click', cancelHandler);
      onOk();
    };
    var cancelHandler = function() {
      $('confirmOverlay').classList.remove('show');
      $('confirmOk').removeEventListener('click', okHandler);
      $('confirmCancel').removeEventListener('click', cancelHandler);
    };
    $('confirmOk').addEventListener('click', okHandler);
    $('confirmCancel').addEventListener('click', cancelHandler);
  }

  function deleteModel(id) {
    var m = findModel(id);
    if (!m) return;
    dl.cancelDownload(id);
    confirmDialog('确定要删除「' + m.name + '」的本地文件吗？', function() {
      if (m.gguf_path) {
        window.__cpp__.config.deleteFile(m.gguf_path).then(function() {
          m.status = 'available';
          m.progress = 0;
          m.downloaded = 0;
          m.speed = 0;
          renderModels();
        }).catch(function(e) {
          alert('删除本地文件失败: ' + e);
        });
      }
    });
  }

  /* ---- 模型详情页 ---- */
  var currentDetailId = null;

  function showDetail(id) {
    var m = findModel(id);
    if (!m) return;
    currentDetailId = id;

    var isRunning = m.status === 'running';
    var isDownloaded = m.status === 'downloaded';
    var isDownloading = m.status === 'downloading';

    $('detailTitle').textContent = m.name;
    $('detailName').textContent = m.name;
    $('detailDesc').textContent = m.desc;
    $('detailParam').textContent = m.param + 'B';
    $('detailSize').textContent = m.size;
    $('detailType').textContent = m.type;
    $('detailCtx').textContent = m.ctx || '8K';
    $('detailBackend').textContent = m.backend || 'llama-server';

    var heroIcon = $('detailHeroIcon');
    heroIcon.textContent = isRunning ? '🚀' : isDownloaded ? '✅' : '📦';
    heroIcon.className = 'detail-hero-icon' + (isRunning ? ' running' : '');

    $('detailTags').innerHTML =
      '<span class="detail-tag gb">' + escapeHtml(m.size) + '</span>' +
      (m.ctx && m.ctx !== 'N/A' ? '<span class="detail-tag ctx">上下文 ' + escapeHtml(m.ctx) + '</span>' : '') +
      '<span class="detail-tag">' + escapeHtml(String(m.param)) + 'B 参数</span>' +
      '<span class="detail-tag" style="border-color:var(--accent);color:var(--accent);">' + escapeHtml(m.backend || 'llama-server') + '</span>';

    var statusRow = $('detailStatusRow');
    if (isRunning) {
      statusRow.className = 'detail-status-row running';
      statusRow.innerHTML = '<span class="status-dot-sm" style="width:8px;height:8px;"></span> <span>运行中</span>';
    } else if (isDownloaded) {
      statusRow.className = 'detail-status-row stopped';
      statusRow.innerHTML = '<span>已下载 · 未运行</span>';
    } else {
      statusRow.className = 'detail-status-row stopped';
      statusRow.innerHTML = '<span>未下载</span>';
    }

    // 资源占用区块：仅运行中显示，立即拉一次指标并由 2s 轮询持续刷新。
    var metricsSection = $('detailMetricsSection');
    if (isRunning) {
      metricsSection.style.display = '';
      renderDetailMetrics(m.metrics);
      // 主动拉一次，避免等到下个轮询周期才有数据
      if (window.chatService) {
        window.chatService.getMetrics(id).then(function(r) {
          if (currentDetailId === id && r && r.ok && r.data && r.data.status === 'ok') {
            renderDetailMetrics(r.data);
          }
        }).catch(function() {}); // 失败由 2s 轮询兜底刷新，无需提示
      }
    } else {
      metricsSection.style.display = 'none';
    }

    $('detailPath').textContent = m.gguf_path || ('models/' + m.id);

    // 底部操作按钮
    var actionBtn = $('detailActionBtn');
    if (isRunning) {
      actionBtn.className = 'btn btn-red';
      actionBtn.textContent = '■ 停止';
      actionBtn.onclick = function() { stopModel(id); closeDetail(); };
    } else if (isDownloaded) {
      actionBtn.className = 'btn btn-green';
      actionBtn.textContent = '▶ 启动';
      actionBtn.onclick = function() { closeDetail(); window.showConfig(id); };
    } else if (isDownloading) {
      actionBtn.className = 'btn btn-yellow';
      actionBtn.textContent = '⏸ 暂停';
      actionBtn.onclick = function() { pauseDownload(id); closeDetail(); };
    } else if (m.status === 'paused') {
      actionBtn.className = 'btn btn-blue';
      actionBtn.textContent = '▶ 继续';
      actionBtn.onclick = function() { resumeDownload(id); closeDetail(); };
    } else {
      actionBtn.className = 'btn btn-blue';
      actionBtn.textContent = '⬇ 下载';
      actionBtn.onclick = function() { startDownload(id); closeDetail(); };
    }

    // 编辑按钮
    $('detailEditBtn').onclick = function() { showEditModel(id); };

    $('detailPage').classList.add('open');
    $('detailOverlay').classList.add('open');
  }

  function closeDetail() {
    $('detailPage').classList.remove('open');
    $('detailOverlay').classList.remove('open');
    currentDetailId = null;
  }

  // 渲染详情页资源占用面板（mt: {memoryMB, cpuPercent, gpuMemoryMB, threads, handles}）
  function renderDetailMetrics(mt) {
    if (!mt) return;
    var cpu = Math.min(mt.cpuPercent || 0, 100);
    var memMB = mt.memoryMB || 0;
    var gpuMB = mt.gpuMemoryMB || 0;
    var memPct = Math.min(memMB / (16 * 1024) * 100, 100);

    $('detailCpuBar').style.width = cpu + '%';
    $('detailCpuBar').className = 'metric-bar-fill' + (cpu > 80 ? ' critical' : cpu > 60 ? ' high' : '');
    $('detailCpuVal').textContent = cpu.toFixed(1) + '%';

    $('detailMemBar').style.width = memPct + '%';
    $('detailMemBar').className = 'metric-bar-fill' + (memPct > 85 ? ' critical' : memPct > 70 ? ' high' : '');
    $('detailMemVal').textContent = fmtMem(memMB);

    $('detailGpuBar').style.width = Math.min(gpuMB / 8192 * 100, 100) + '%';
    $('detailGpuBar').className = 'metric-bar-fill' + (gpuMB > 6000 ? ' critical' : gpuMB > 4000 ? ' high' : '');
    $('detailGpuVal').textContent = fmtMem(gpuMB);

    var extra = [];
    if (mt.pid) extra.push('PID ' + mt.pid);
    if (mt.port) extra.push('端口 ' + mt.port);
    if (mt.threads) extra.push('线程 ' + mt.threads);
    if (mt.handles) extra.push('句柄 ' + mt.handles);
    $('detailMetricsExtra').textContent = extra.join(' · ');
  }

  // 由 service-panel 的 2s 轮询调用：若详情页正展示某个运行中的模型，刷新其资源面板。
  // byId: { modelId -> metrics对象 }
  function refreshDetailMetrics(byId) {
    if (!currentDetailId) return;
    var mt = byId[currentDetailId];
    if (mt && mt.status === 'ok') {
      $('detailMetricsSection').style.display = '';
      renderDetailMetrics(mt);
    }
  }

  $('detailBackBtn').addEventListener('click', closeDetail);
  $('detailCloseBtn2').addEventListener('click', closeDetail);
  $('detailOverlay').addEventListener('click', function(e) {
    if (e.target === this) closeDetail();
  });

  /* ---- 筛选 ---- */
  expandFilterBtn.addEventListener('click', function() {
    var open = filterPanel.classList.toggle('open');
    expandFilterBtn.querySelector('span').textContent = open ? '‹' : '›';
    expandFilterBtn.style.borderColor = open ? 'var(--accent)' : '';
  });

  modelSearchInput.addEventListener('input', renderModels);

  /* ---- 从 models.json 加载（经 C++ config bridge）---- */
  function loadModels() {
    modelList.innerHTML = '<div style="text-align:center;padding:48px;color:var(--text-muted);font-size:13px;">⏳ 正在加载模型列表…</div>';
    window.__cpp__.config.read('models.json')
      .then(function(data) {
        window.AppState.models = (data.models || []).map(function(m) {
          m.progress = (m.status === 'downloaded' || m.status === 'running') ? 100 : 0;
          return m;
        });
        renderModels();
      })
      .catch(function(e) {
        modelList.innerHTML = '<div style="text-align:center;padding:48px;color:var(--red);font-size:13px;">⚠ 模型配置加载失败：' + e + '</div>';
      });
  }

  // 配置弹窗启动成功后需要重渲染列表（config.js 发出）
  AppBus.on('models:changed', renderModels);

  // 监听模型生命周期事件，直接更新 AppState.models 中对应模型的状态，
  // 避免 models:changed 触发 loadModels() 从 config.json 读取旧状态覆盖。
  AppBus.on('model:started', function(d) {
    var models = window.AppState.models || [];
    for (var i = 0; i < models.length; i++) {
      if (models[i].id === d.id) {
        models[i].status = 'running';
        if (d.port) models[i].port = d.port;
        break;
      }
    }
    renderModels();
  });
  AppBus.on('model:stopped', function(d) {
    var models = window.AppState.models || [];
    for (var i = 0; i < models.length; i++) {
      if (!d || !d.id || models[i].id === d.id) {
        models[i].status = 'downloaded';
        delete models[i].port;
      }
    }
    renderModels();
  });

  // 行内 onclick 依赖的全局函数
  window.startDownload = startDownload;
  window.pauseDownload = pauseDownload;
  window.resumeDownload = resumeDownload;
  window.cancelDownload = cancelDownload;
  window.stopModel = stopModel;
  window.deleteModel = deleteModel;
  window.showDetail = showDetail;
  window.renderModels = renderModels;
  window.refreshDetailMetrics = refreshDetailMetrics;
  window.loadModels = loadModels;

  // 刷新按钮
  $('refreshModelBtn').addEventListener('click', loadModels);

  loadModels();
})();
