/* ================================================================
   主题切换
   ================================================================ */
var $ = function(id) { return document.getElementById(id); };
var themeToggle = $('themeToggle');
var currentTheme = 'dark'; // 内存状态，WebView2 set_html 无 origin 不支持 localStorage
document.documentElement.setAttribute('data-theme', currentTheme);
themeToggle.textContent = '☀';

themeToggle.addEventListener('click', function() {
  currentTheme = currentTheme === 'dark' ? 'light' : 'dark';
  document.documentElement.setAttribute('data-theme', currentTheme);
  themeToggle.textContent = currentTheme === 'dark' ? '☀' : '🌙';
});

/* ================================================================
   功能卡片切换
   ================================================================ */
document.querySelectorAll('.feature-card').forEach(function(card) {
  card.addEventListener('click', function() {
    document.querySelectorAll('.feature-card').forEach(function(c) { c.classList.remove('active'); });
    card.classList.add('active');
  });
});

$('cardsPrev').addEventListener('click', function() { $('cardsTrack').scrollBy({ left: -280, behavior: 'smooth' }); });
$('cardsNext').addEventListener('click', function() { $('cardsTrack').scrollBy({ left: 280, behavior: 'smooth' }); });

/* ================================================================
   推荐问题
   ================================================================ */
document.querySelectorAll('.chip').forEach(chip => {
  chip.addEventListener('click', () => {
    $('inputBox').value = chip.dataset.q;
    $('inputBox').dispatchEvent(new Event('input'));
    $('inputBox').focus();
  });
});

/* ================================================================
   模式选择
   ================================================================ */
document.querySelectorAll('.mode-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.mode-btn').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
  });
});

/* ================================================================
   聊天核心逻辑
   ================================================================ */
const chatArea = $('chatArea');
const welcomeCard = $('welcomeCard');
const inputBox = $('inputBox');
const sendBtn = $('sendBtn');

let hasMessages = false;
let isTyping = false;

function addMessage(text, isUser = false) {
  if (!hasMessages) {
    welcomeCard.style.display = 'none';
    chatArea.style.display = 'flex';
    hasMessages = true;
  }

  const time = new Date().toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit' });
  const msg = document.createElement('div');
  msg.className = 'msg ' + (isUser ? 'user' : 'bot');
  msg.innerHTML = `
    <div class="msg-avatar">${isUser ? 'JK' : '🤖'}</div>
    <div class="msg-content">
      <div class="msg-bubble">${escapeHtml(text)}</div>
      <div class="msg-time">${time}</div>
    </div>`;
  chatArea.appendChild(msg);
  chatArea.scrollTop = chatArea.scrollHeight;
}

function addTyping() {
  isTyping = true;
  const t = document.createElement('div');
  t.className = 'msg bot';
  t.id = 'typingEl';
  t.innerHTML = `
    <div class="msg-avatar">🤖</div>
    <div class="msg-content">
      <div class="typing-indicator">
        <div class="typing-dot"></div>
        <div class="typing-dot"></div>
        <div class="typing-dot"></div>
      </div>
    </div>`;
  chatArea.appendChild(t);
  chatArea.scrollTop = chatArea.scrollHeight;
}

function removeTyping() {
  isTyping = false;
  const t = document.getElementById('typingEl');
  if (t) t.remove();
}

function escapeHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')
          .replace(/"/g,'&quot;').replace(/'/g,'&#039;').replace(/\n/g,'<br>');
}

function getAiReply(userText) {
  // 假数据回复
  const q = userText.trim().toLowerCase();
  if (q.includes('时间') || q.includes('安排')) {
    return '好的！根据你今天的日程，我建议如下安排：\n\n08:30 - 09:00  处理邮件和消息\n09:00 - 11:30  深度工作（最重要的事）\n11:30 - 13:00  午餐休息\n13:00 - 15:00  会议和协作\n15:00 - 17:00  学习和成长\n17:00 - 18:00  整理和计划明天\n\n记住：先做最重要的事！💪';
  }
  if (q.includes('利弊') || q.includes('分析')) {
    return '好的，我来帮你分析这个决策的利弊：\n\n✅ 优势\n• 提高效率，节省时间\n• 减少重复劳动\n• 标准化流程\n\n⚠️ 风险\n• 需要初期投入\n• 可能存在学习曲线\n• 过渡期的不确定性\n\n建议：先做一个小范围试点，再决定是否全面推广。';
  }
  if (q.includes('代码') || q.includes('重构')) {
    return '代码重构建议：\n\n1. 先写单元测试，覆盖现有逻辑\n2. 遵循「童子军规则」：离开时比来时更干净\n3. 分阶段进行，每次只重构一个模块\n4. 使用 IDE 的重构工具\n5. 保持 commit 粒度小而专注\n\n记住：重构不是重写，要尽量保留原有的行为。';
  }
  return `这是一个演示回复。你说的是：「${userText}」\n\n作为 FlowyAIPC AI 助手，我目前正在连接本地大模型服务（Herdsman）。正式上线后将基于你选择的本地模型提供智能问答服务。\n\n你可以尝试：\n• 问一个技术问题\n• 请求代码生成\n• 总结一段文字\n• 翻译一段内容`;
}

async function sendMessage() {
  const text = inputBox.value.trim();
  if (!text || isTyping) return;

  addMessage(text, true);
  inputBox.value = '';
  inputBox.style.height = 'auto';
  addTyping();

  // 模拟网络延迟
  await new Promise(r => setTimeout(r, 800 + Math.random() * 600));
  removeTyping();
  addMessage(getAiReply(text));
}

sendBtn.addEventListener('click', sendMessage);
inputBox.addEventListener('keydown', e => {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    sendMessage();
  }
});

// 自动高度
inputBox.addEventListener('input', () => {
  inputBox.style.height = 'auto';
  inputBox.style.height = Math.min(inputBox.scrollHeight, 120) + 'px';
});

/* ================================================================
   新建对话
   ================================================================ */
$('newChatBtn').addEventListener('click', () => {
  hasMessages = false;
  chatArea.innerHTML = '';
  welcomeCard.style.display = 'flex';
  inputBox.value = '';
  inputBox.style.height = 'auto';
});

/* ================================================================
   资源监控动画（假数据）
   ================================================================ */
function updateMetrics() {
  const cpu = 20 + Math.random() * 40;
  const mem = 40 + Math.random() * 40;
  const gpu = 10 + Math.random() * 50;

  const cpuBar = $('cpuBar');
  const memBar = $('memBar');
  const gpuBar = $('gpuBar');

  cpuBar.style.width = cpu + '%';
  cpuBar.className = 'metric-bar-fill' + (cpu > 80 ? ' critical' : cpu > 60 ? ' high' : '');

  memBar.style.width = mem + '%';
  memBar.className = 'metric-bar-fill' + (mem > 85 ? ' critical' : mem > 70 ? ' high' : '');

  gpuBar.style.width = gpu + '%';
  gpuBar.className = 'metric-bar-fill' + (gpu > 80 ? ' critical' : gpu > 60 ? ' high' : '');

  $('cpuVal').textContent = Math.round(cpu) + '%';
  $('memVal').textContent = (8 + Math.random() * 4).toFixed(1) + ' GB';
  $('gpuVal').textContent = Math.round(gpu) + '%';
}
setInterval(updateMetrics, 3000);

/* ================================================================
   侧边栏对话点击
   ================================================================ */
document.querySelectorAll('.chat-item').forEach(item => {
  item.addEventListener('click', e => {
    if (e.target.classList.contains('chat-item-delete')) {
      item.remove();
      return;
    }
    document.querySelectorAll('.chat-item').forEach(i => i.classList.remove('active'));
    item.classList.add('active');
  });
});

/* ================================================================
   模型详情页
   ================================================================ */
let currentDetailId = null;

function showDetail(id) {
  const m = allModels.find(x => x.id === id);
  if (!m) return;
  currentDetailId = id;

  const isRunning = m.status === 'running';
  const isDownloaded = m.status === 'downloaded';
  const isDownloading = m.status === 'downloading';
  const typeLabel = m.type.includes('Gemma') ? 'Gemma' : m.type.includes('Qwen') ? 'Qwen' : m.type.includes('Llama') ? 'Llama' : m.type;

  $('detailTitle').textContent = m.name;
  $('detailName').textContent = m.name;
  $('detailDesc').textContent = m.desc;
  $('detailParam').textContent = m.param + 'B';
  $('detailSize').textContent = m.size;
  $('detailType').textContent = m.type;
  $('detailCtx').textContent = m.ctx || '8K';

  const heroIcon = $('detailHeroIcon');
  heroIcon.textContent = isRunning ? '🚀' : isDownloaded ? '✅' : '📦';
  heroIcon.className = 'detail-hero-icon' + (isRunning ? ' running' : '');

  $('detailTags').innerHTML = `
    <span class="detail-tag gb">${m.size}</span>
    <span class="detail-tag type">${typeLabel}</span>
    <span class="detail-tag ctx">上下文 ${m.ctx || '8K'}</span>
    <span class="detail-tag">${m.param}B 参数</span>`;

  const statusRow = $('detailStatusRow');
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

  $('detailPath').textContent = m.gguf_path || `models/${m.id}`;

  // 上下文滑块
  const ctxSection = $('detailCtxSection');
  const ctxSlider = $('detailCtxSlider');
  const ctxVal = $('detailCtxVal');
  if (isDownloaded || isRunning) {
    ctxSection.style.display = '';
    var ctx = m.ctx || '8K';
    const pct = ctx === '8K' ? 25 : ctx === '32K' ? 50 : ctx === '64K' ? 75 : ctx === '128K' ? 90 : 50;
    ctxSlider.value = pct;
    ctxVal.textContent = pct + '%';
  } else {
    ctxSection.style.display = 'none';
  }

  // 底部操作按钮
  const actionBtn = $('detailActionBtn');
  if (isRunning) {
    actionBtn.className = 'btn btn-red';
    actionBtn.textContent = '■ 停止';
    actionBtn.onclick = function() { stopModel(id); closeDetail(); };
  } else if (isDownloaded) {
    actionBtn.className = 'btn btn-green';
    actionBtn.textContent = '▶ 启动';
    actionBtn.onclick = function() { m.status = 'running'; renderModels(); updateServiceStatus(true); closeDetail(); };
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

  $('detailPage').classList.add('open');
}

function closeDetail() {
  $('detailPage').classList.remove('open');
  currentDetailId = null;
}

$('detailBackBtn').addEventListener('click', closeDetail);
$('detailCloseBtn2').addEventListener('click', closeDetail);
$('detailCtxSlider').addEventListener('input', e => {
  $('detailCtxVal').textContent = e.target.value + '%';
});

/* ================================================================
   模型页 Model Page
   ================================================================ */
const modelList = $('modelList');
const modelSearchInput = $('modelSearchInput');
const expandFilterBtn = $('expandFilterBtn');
const filterPanel = $('filterPanel');

// 模型数据从 models.json 配置加载（异步），加载前为空数组
let allModels = [];

// 筛选状态
const filterState = {
  param: 'all',   // all | small | medium | large
  type: 'all',    // all | gemma | qwen | llama | other
};
// 渲染列表
function renderModels() {
  const q = modelSearchInput.value.trim().toLowerCase();
  const filtered = allModels.filter(m => {
    if (q && !m.name.toLowerCase().includes(q) && !m.desc.toLowerCase().includes(q)) return false;
    if (filterState.param === 'small' && m.param > 10) return false;
    if (filterState.param === 'medium' && (m.param <= 10 || m.param > 30)) return false;
    if (filterState.param === 'large' && m.param <= 30) return false;
    if (filterState.type === 'gemma' && !m.type.includes('Gemma')) return false;
    if (filterState.type === 'qwen' && !m.type.includes('Qwen')) return false;
    if (filterState.type === 'llama' && !m.type.includes('Llama')) return false;
    if (filterState.type === 'other' && ['Gemma', 'Qwen', 'Llama'].some(t => m.type.includes(t))) return false;
    return true;
  });

  if (!filtered.length) {
    modelList.innerHTML = `<div style="text-align:center;padding:48px;color:var(--text-muted);font-size:13px;">🔍 没有找到符合条件的模型</div>`;
    return;
  }

  modelList.innerHTML = filtered.map(m => {
    const isRunning = m.status === 'running';
    const isDownloaded = m.status === 'downloaded';
    const isDownloading = m.status === 'downloading';
    const isAvailable = m.status === 'available';
    const typeLabel = m.type.includes('Gemma') ? 'Gemma' : m.type.includes('Qwen') ? 'Qwen' : m.type.includes('Llama') ? 'Llama' : m.type;
    return `<div class="model-row ${isRunning ? 'selected' : ''}">
      <div class="model-check ${isRunning ? 'on' : ''}" id="ck-${m.id}">${isRunning ? '✓' : ''}</div>
      <div class="model-row-icon ${isRunning ? 'running' : ''}">${isRunning ? '🚀' : isDownloaded ? '✅' : '📦'}</div>
      <div class="model-row-info">
        <div class="model-row-name">${m.name}</div>
        <div class="model-row-meta">${m.desc}</div>
        <div class="model-row-tags">
          <span class="model-row-tag gb">${m.size}</span>
          <span class="model-row-tag type">${typeLabel}</span>
        </div>
      </div>
      ${isRunning ? `<div class="model-row-status"><span class="status-dot-sm"></span>运行中</div>` : ''}
      <div class="model-row-actions">
        ${isRunning ? `<button class="btn btn-red" onclick="stopModel('${m.id}')">停止</button>` : ''}
        ${isDownloaded ? `<button class="btn btn-green" onclick="showConfig('${m.id}')">▶ 立即启动</button>
          <button class="btn btn-ghost" onclick="deleteModel('${m.id}')" title="删除">🗑</button>` : ''}
        ${isDownloading ? `
          <div class="model-dl-info">
            <div class="model-dl-bar"><div class="model-dl-fill" id="dlbar-${m.id}" style="width:${m.progress}%"></div></div>
            <div class="model-dl-speed">${formatSpeed(m.speed || 0)}</div>
          </div>
          <button class="btn btn-ghost" onclick="pauseDownload('${m.id}')" title="暂停">⏸</button>
          <button class="btn btn-ghost" onclick="cancelDownload('${m.id}')" title="取消">✕</button>` : ''}
        ${m.status === 'paused' ? `
          <div class="model-dl-info">
            <div class="model-dl-bar"><div class="model-dl-fill" id="dlbar-${m.id}" style="width:${m.progress}%"></div></div>
            <div class="model-dl-speed">已暂停</div>
          </div>
          <button class="btn btn-ghost" onclick="resumeDownload('${m.id}')" title="继续">▶</button>
          <button class="btn btn-ghost" onclick="cancelDownload('${m.id}')" title="取消">✕</button>` : ''}
        ${isAvailable ? `<button class="btn btn-blue" onclick="startDownload('${m.id}')">⬇ 下载模型</button>` : ''}
        <button class="btn btn-ghost" onclick="showDetail('${m.id}')" title="详情">⋯</button>
      </div>
    </div>`;
  }).join('');
}

// 下载服务
var dl = window.downloadService;

function formatSpeed(bytesPerSec) {
  if (bytesPerSec >= 1073741824) return (bytesPerSec / 1073741824).toFixed(1) + ' GB/s';
  if (bytesPerSec >= 1048576) return (bytesPerSec / 1048576).toFixed(1) + ' MB/s';
  if (bytesPerSec >= 1024) return (bytesPerSec / 1024).toFixed(1) + ' KB/s';
  return bytesPerSec + ' B/s';
}

function startDownload(id) {
  var m = allModels.find(function(x) { return x.id === id; });
  if (!m || !m.download_url) return;

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
}

function pauseDownload(id) {
  var m = allModels.find(function(x) { return x.id === id; });
  if (!m) return;
  m.status = 'paused';
  m._pausing = true;
  renderModels();
  dl.pauseDownload(id).then(function(r) {
    m._pausing = false;
    if (r && r.ok === false) {
      m.status = 'downloading';
      renderModels();
    }
  }).catch(function() {
    m._pausing = false;
    m.status = 'downloading';
    renderModels();
  });
}

function resumeDownload(id) {
  var m = allModels.find(function(x) { return x.id === id; });
  if (!m) return;
  dl.resumeDownload(id).then(function(r) {
    if (r && r.ok === false) {
      m.status = 'paused';
    } else {
      m.status = 'downloading';
    }
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
  var m = allModels.find(function(x) { return x.id === id; });
  if (m) {
    m.status = 'available';
    m.progress = 0;
    m.downloaded = 0;
    m.speed = 0;
  }
  renderModels();
}

function showAddModel() {
  $('addModelId').value = '';
  $('addModelName').value = '';
  $('addModelUrl').value = '';
  $('addModelDesc').value = '';
  $('addModelSize').value = '';
  $('addModelSizeBytes').value = '';
  $('addModelParam').value = '';
  $('addModelType').value = 'Other';
  $('addModelCtx').value = '32K';
  $('addModelOverlay').classList.add('show');
}

$('addModelBtn').addEventListener('click', showAddModel);
$('addModelCancel').addEventListener('click', function() {
  $('addModelOverlay').classList.remove('show');
});
$('addModelConfirm').addEventListener('click', function() {
  var id = $('addModelId').value.trim();
  var name = $('addModelName').value.trim();
  var url = $('addModelUrl').value.trim();
  if (!id || !url) { alert('模型 ID 和下载 URL 为必填项'); return; }
  var model = {
    id: id,
    name: name || id,
    download_url: url,
    desc: $('addModelDesc').value.trim() || '',
    size: $('addModelSize').value.trim() || 'Unknown',
    size_bytes: parseInt($('addModelSizeBytes').value) || 0,
    param: parseFloat($('addModelParam').value) || 0,
    type: $('addModelType').value,
    ctx: $('addModelCtx').value
  };
  window.__cpp__.config.addModel(model).then(function(data) {
    $('addModelOverlay').classList.remove('show');
    allModels = (data.models || []).map(function(m) {
      m.progress = (m.status === 'downloaded' || m.status === 'running') ? 100 : 0;
      return m;
    });
    renderModels();
  }).catch(function(e) {
    alert('添加模型失败: ' + e);
  });
});

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
  var m = allModels.find(function(x) { return x.id === id; });
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

function stopModel(id) {
  var m = allModels.find(function(x) { return x.id === id; });
  if (m) m.status = 'downloaded';
  renderModels();
}

// 展开筛选
expandFilterBtn.addEventListener('click', () => {
  const open = filterPanel.classList.toggle('open');
  expandFilterBtn.querySelector('span').textContent = open ? '‹' : '›';
  expandFilterBtn.style.borderColor = open ? 'var(--accent)' : '';
});

// 筛选复选框
document.querySelectorAll('.filter-check[data-param]').forEach(el => {
  el.addEventListener('click', () => {
    const val = el.dataset.param;
    document.querySelectorAll('.filter-check[data-param]').forEach(c => c.classList.remove('checked'));
    el.classList.add('checked');
    filterState.param = val;
    renderModels();
  });
});
document.querySelectorAll('.filter-check[data-type]').forEach(el => {
  el.addEventListener('click', () => {
    const val = el.dataset.type;
    document.querySelectorAll('.filter-check[data-type]').forEach(c => c.classList.remove('checked'));
    el.classList.add('checked');
    filterState.type = val;
    renderModels();
  });
});

modelSearchInput.addEventListener('input', renderModels);

// 从 models.json 加载模型配置（经 C++ config bridge，file:// 下无法直接 fetch）
function loadModels() {
  window.__cpp__.config.read('models.json')
    .then(function(data) {
      allModels = (data.models || []).map(function(m) {
        m.progress = (m.status === 'downloaded' || m.status === 'running') ? 100 : 0;
        return m;
      });
      renderModels();
    })
    .catch(function(e) {
      modelList.innerHTML = '<div style="text-align:center;padding:48px;color:var(--red);font-size:13px;">⚠ 模型配置加载失败：' + e + '</div>';
    });
}
loadModels();

/* ================================================================
   启动配置弹窗
   ================================================================ */
let configModelId = null;
let configCtx = 50;
let configThinking = false;

function showConfig(id) {
  const m = allModels.find(x => x.id === id);
  if (!m) return;
  configModelId = id;
  configCtx = 50;
  configThinking = false;
  $('configModelName').textContent = m.name;
  $('configCtx').textContent = configCtx + '%';
  $('configCtxSlider').value = configCtx;
  $('configThinking').className = 'config-toggle';
  $('configOverlay').classList.add('show');
}

$('configCtxSlider').addEventListener('input', e => {
  configCtx = parseInt(e.target.value);
  $('configCtx').textContent = configCtx + '%';
});
$('configThinking').addEventListener('click', () => {
  configThinking = !configThinking;
  $('configThinking').className = 'config-toggle' + (configThinking ? ' on' : '');
});
$('configCancel').addEventListener('click', () => $('configOverlay').classList.remove('show'));
$('configConfirm').addEventListener('click', () => {
  const m = allModels.find(x => x.id === configModelId);
  if (m) m.status = 'running';
  $('configOverlay').classList.remove('show');
  renderModels();
  updateServiceStatus(true);
});

/* ================================================================
   Tab 切换（应用 / 模型）
   ================================================================ */
let currentTab = 'app';
const modelPage = $('modelPage');

document.querySelectorAll('.tab-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    currentTab = btn.dataset.tab;
    if (currentTab === 'model') {
      modelPage.classList.add('active');
      document.querySelector('.feature-cards-section').style.display = 'none';
      document.querySelector('.content-area').style.display = 'none';
    } else {
      modelPage.classList.remove('active');
      document.querySelector('.feature-cards-section').style.display = '';
      document.querySelector('.content-area').style.display = '';
    }
  });
});

/* ================================================================
   侧边栏本地服务状态
   ================================================================ */
function updateServiceStatus(running) {
  const dot = document.querySelector('.status-dot');
  const statusText = document.querySelector('.service-status span:last-child');
  if (running) {
    dot.className = 'status-dot';
    statusText.textContent = '运行中';
  } else {
    dot.className = 'status-dot stopped';
    statusText.textContent = '已停止';
  }
}
