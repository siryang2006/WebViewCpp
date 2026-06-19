/* ================================================================
   chat.js — 聊天 UI + 流式渲染 + 对话历史管理
   推理能力由 window.chatService 提供（C++ ChatService）。
   本模块负责消息气泡渲染、输入框交互、运行模型选择器、
   侧边栏对话列表 CRUD；
   选中的模型 id 存于 AppState.selectedModelId，推理时按 id 路由。
   ================================================================ */
(function() {
  var chatArea = $('chatArea');
  var welcomeCard = $('welcomeCard');
  var inputBox = $('inputBox');
  var sendBtn = $('sendBtn');
  var modelSelect = $('chatModelSelect');
  var chatHistory = $('chatHistory');

  var isTyping = false;
  var convIdCounter = 0;

  // 取当前运行中的模型列表（status === 'running'）
  function runningModels() {
    return (window.AppState.models || []).filter(function(m) { return m.status === 'running'; });
  }

  // 重建模型下拉：列出运行中的模型；无则置灰提示。
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

  AppBus.on('model:started', refreshModelSelect);
  AppBus.on('model:stopped', refreshModelSelect);
  AppBus.on('models:changed', refreshModelSelect);

  /* ---- 对话管理 ---- */

  function genConvId() {
    return 'conv_' + (++convIdCounter);
  }

  function nowTime() {
    return new Date().toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit' });
  }

  function nowLabel() {
    var d = new Date();
    var today = new Date(d.getFullYear(), d.getMonth(), d.getDate());
    var yesterday = new Date(today - 86400000);
    if (d >= today) return '今天';
    if (d >= yesterday) return '昨天';
    return '更早';
  }

  function findConv(id) {
    var cs = window.AppState.conversations;
    for (var i = 0; i < cs.length; i++) {
      if (cs[i].id === id) return cs[i];
    }
    return null;
  }

  function saveCurrentMessages() {
    var conv = findConv(window.AppState.currentConvId);
    if (!conv) return;
    var msgs = chatArea.querySelectorAll('.msg');
    conv.messages = [];
    for (var i = 0; i < msgs.length; i++) {
      var bubble = msgs[i].querySelector('.msg-bubble');
      if (bubble) {
        conv.messages.push({
          text: bubble.textContent,
          isUser: msgs[i].classList.contains('user')
        });
      }
    }
    // 以第一条用户消息为标题
    for (var j = 0; j < conv.messages.length; j++) {
      if (conv.messages[j].isUser) {
        conv.title = conv.messages[j].text.slice(0, 30);
        break;
      }
    }
    conv.time = nowLabel();
  }

  function switchConversation(id) {
    saveCurrentMessages();

    var conv = findConv(id);
    if (!conv) return;

    window.AppState.currentConvId = id;
    chatArea.innerHTML = '';
    welcomeCard.style.display = 'none';
    chatArea.style.display = 'flex';

    for (var i = 0; i < conv.messages.length; i++) {
      var m = conv.messages[i];
      var msg = document.createElement('div');
      msg.className = 'msg ' + (m.isUser ? 'user' : 'bot');
      msg.innerHTML =
        '<div class="msg-avatar">' + (m.isUser ? 'JK' : '🤖') + '</div>' +
        '<div class="msg-content">' +
        '<div class="msg-bubble">' + escapeHtml(m.text) + '</div>' +
        '</div>';
      chatArea.appendChild(msg);
    }
    chatArea.scrollTop = chatArea.scrollHeight;
    renderSidebar();
  }

  function newConversation() {
    saveCurrentMessages();
    var id = genConvId();
    var conv = { id: id, title: '新对话', messages: [], time: nowLabel() };
    window.AppState.conversations.push(conv);
    window.AppState.currentConvId = id;
    chatArea.innerHTML = '';
    welcomeCard.style.display = 'flex';
    chatArea.style.display = 'none';
    inputBox.value = '';
    inputBox.style.height = 'auto';
    renderSidebar();
  }

  function deleteConversation(id) {
    var cs = window.AppState.conversations;
    for (var i = 0; i < cs.length; i++) {
      if (cs[i].id === id) {
        cs.splice(i, 1);
        break;
      }
    }
    if (window.AppState.currentConvId === id) {
      if (cs.length > 0) {
        switchConversation(cs[cs.length - 1].id);
      } else {
        window.AppState.currentConvId = null;
        chatArea.innerHTML = '';
        welcomeCard.style.display = 'flex';
        chatArea.style.display = 'none';
      }
    }
    renderSidebar();
  }

  function renderSidebar() {
    if (!chatHistory) return;
    var convs = window.AppState.conversations;
    if (!convs.length) {
      chatHistory.innerHTML = '<div class="history-section-label">暂无对话</div>';
      return;
    }
    var html = '';
    var groups = {};
    for (var i = 0; i < convs.length; i++) {
      var label = convs[i].time || '更早';
      if (!groups[label]) groups[label] = [];
      groups[label].push(convs[i]);
    }
    var order = ['今天', '昨天', '更早'];
    for (var oi = 0; oi < order.length; oi++) {
      var lab = order[oi];
      var items = groups[lab];
      if (!items) continue;
      html += '<div class="history-section-label">' + lab + '</div>';
      for (var j = 0; j < items.length; j++) {
        var c = items[j];
        var active = c.id === window.AppState.currentConvId ? ' active' : '';
        html += '<div class="chat-item' + active + '" onclick="window.switchConversation(\'' + c.id + '\')">' +
          '<span class="chat-item-icon">💬</span>' +
          '<span class="chat-item-text">' + escapeHtml(c.title) + '</span>' +
          '<span class="chat-item-time"></span>' +
          '<button class="chat-item-delete" onclick="event.stopPropagation();window.deleteConversation(\'' + c.id + '\')">✕</button>' +
          '</div>';
      }
    }
    chatHistory.innerHTML = html;
  }

  /* ---- 发消息 ---- */

  function ensureChatVisible() {
    var conv = findConv(window.AppState.currentConvId);
    if (!conv) {
      newConversation();
    }
    if (welcomeCard.style.display !== 'none') {
      welcomeCard.style.display = 'none';
      chatArea.style.display = 'flex';
    }
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
    saveCurrentMessages();
    renderSidebar();
  }

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

  /* ---- 事件绑定 ---- */

  sendBtn.addEventListener('click', sendMessage);
  inputBox.addEventListener('keydown', function(e) {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      sendMessage();
    }
  });

  inputBox.addEventListener('input', function() {
    inputBox.style.height = 'auto';
    inputBox.style.height = Math.min(inputBox.scrollHeight, 120) + 'px';
  });

  $('newChatBtn').addEventListener('click', newConversation);

  // 暴露全局接口给侧边栏 onclick
  window.switchConversation = switchConversation;
  window.deleteConversation = deleteConversation;

  // 初始：建一个默认对话
  newConversation();
  refreshModelSelect();
})();
