/* ================================================================
   chat.js — 聊天 UI + 流式渲染
   推理能力由 window.chatService 提供（C++ ChatService）。
   本模块负责消息气泡渲染、输入框交互、运行模型选择器；
   选中的模型 id 存于 AppState.selectedModelId，推理时按 id 路由。
   ================================================================ */
(function() {
  var chatArea = $('chatArea');
  var welcomeCard = $('welcomeCard');
  var inputBox = $('inputBox');
  var sendBtn = $('sendBtn');
  var modelSelect = $('chatModelSelect');

  var hasMessages = false;
  var isTyping = false;

  // 取当前运行中的模型列表（status === 'running'）
  function runningModels() {
    return (window.AppState.models || []).filter(function(m) { return m.status === 'running'; });
  }

  // 重建模型下拉：列出运行中的模型；无则置灰提示。
  // 尽量保留当前选择；选中项失效时回退到第一个运行模型。
  function refreshModelSelect() {
    if (!modelSelect) return;
    var running = runningModels();
    var prev = window.AppState.selectedModelId;

    if (!running.length) {
      modelSelect.innerHTML = '<option value="">无运行模型</option>';
      modelSelect.disabled = true;
      window.AppState.selectedModelId = '';
      return;
    }

    modelSelect.disabled = false;
    modelSelect.innerHTML = running.map(function(m) {
      return '<option value="' + escapeHtml(m.id) + '">' + escapeHtml(m.name || m.id) + '</option>';
    }).join('');

    // 保留原选择；失效则用第一个
    var stillValid = running.some(function(m) { return m.id === prev; });
    var sel = stillValid ? prev : running[0].id;
    modelSelect.value = sel;
    window.AppState.selectedModelId = sel;
  }

  if (modelSelect) {
    modelSelect.addEventListener('change', function() {
      window.AppState.selectedModelId = modelSelect.value;
    });
  }

  // 模型启动/停止/列表变化时刷新选择器
  AppBus.on('model:started', refreshModelSelect);
  AppBus.on('model:stopped', refreshModelSelect);
  AppBus.on('models:changed', refreshModelSelect);

  function ensureChatVisible() {
    if (!hasMessages) {
      welcomeCard.style.display = 'none';
      chatArea.style.display = 'flex';
      hasMessages = true;
    }
  }

  function nowTime() {
    return new Date().toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit' });
  }

  function addMessage(text, isUser) {
    ensureChatVisible();
    var msg = document.createElement('div');
    msg.className = 'msg ' + (isUser ? 'user' : 'bot');
    msg.innerHTML =
      '<div class="msg-avatar">' + (isUser ? 'JK' : '🤖') + '</div>' +
      '<div class="msg-content">' +
      '<div class="msg-bubble">' + escapeHtml(text) + '</div>' +
      '<div class="msg-time">' + nowTime() + '</div>' +
      '</div>';
    chatArea.appendChild(msg);
    chatArea.scrollTop = chatArea.scrollHeight;
  }

  function addTyping() {
    isTyping = true;
    var t = document.createElement('div');
    t.className = 'msg bot';
    t.id = 'typingEl';
    t.innerHTML =
      '<div class="msg-avatar">🤖</div>' +
      '<div class="msg-content"><div class="typing-indicator">' +
      '<div class="typing-dot"></div><div class="typing-dot"></div><div class="typing-dot"></div>' +
      '</div></div>';
    chatArea.appendChild(t);
    chatArea.scrollTop = chatArea.scrollHeight;
  }

  function removeTyping() {
    isTyping = false;
    var t = $('typingEl');
    if (t) t.remove();
  }

  async function sendMessage() {
    var text = inputBox.value.trim();
    if (!text || isTyping) return;

    addMessage(text, true);
    inputBox.value = '';
    inputBox.style.height = 'auto';

    // 选中的运行模型才走真实推理
    var modelId = window.AppState.selectedModelId;
    var hasRunning = runningModels().some(function(m) { return m.id === modelId; });
    if (modelId && hasRunning && window.chatService) {
      await streamChat(text, modelId);
    } else {
      addTyping();
      await new Promise(function(r) { setTimeout(r, 400); });
      removeTyping();
      addMessage('当前没有运行中的模型。请到「模型」页启动一个模型后再开始对话。');
    }
  }

  // 流式对话：UI 渲染层。token 回调注册/清理由 chatService 负责。
  function streamChat(prompt, modelId) {
    isTyping = true;
    ensureChatVisible();

    var msg = document.createElement('div');
    msg.className = 'msg bot';
    msg.innerHTML =
      '<div class="msg-avatar">🤖</div>' +
      '<div class="msg-content">' +
      '<div class="msg-bubble"><span class="stream-cursor">▋</span></div>' +
      '<div class="msg-time">' + nowTime() + '</div>' +
      '</div>';
    chatArea.appendChild(msg);
    var bubble = msg.querySelector('.msg-bubble');
    chatArea.scrollTop = chatArea.scrollHeight;

    var fullText = '';

    return window.chatService.chat(prompt, function(token) {
      fullText += token;
      bubble.innerHTML = escapeHtml(fullText) + '<span class="stream-cursor">▋</span>';
      chatArea.scrollTop = chatArea.scrollHeight;
    }, modelId).then(function() {
      bubble.innerHTML = escapeHtml(fullText);
      isTyping = false;
    }).catch(function(e) {
      bubble.innerHTML = escapeHtml('推理出错: ' + e);
      isTyping = false;
    });
  }

  sendBtn.addEventListener('click', sendMessage);
  inputBox.addEventListener('keydown', function(e) {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      sendMessage();
    }
  });

  // 输入框自动高度
  inputBox.addEventListener('input', function() {
    inputBox.style.height = 'auto';
    inputBox.style.height = Math.min(inputBox.scrollHeight, 120) + 'px';
  });

  // 新建对话
  $('newChatBtn').addEventListener('click', function() {
    hasMessages = false;
    chatArea.innerHTML = '';
    welcomeCard.style.display = 'flex';
    inputBox.value = '';
    inputBox.style.height = 'auto';
  });

  // 初始构建一次（此时通常无运行模型，显示置灰提示）
  refreshModelSelect();
})();
