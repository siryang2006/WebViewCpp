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
  var systemPromptVisible = false;

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
    loadSystemPrompt(sel);
  }

  if (modelSelect) {
    modelSelect.addEventListener('change', function() {
      window.AppState.selectedModelId = modelSelect.value;
      loadSystemPrompt(modelSelect.value);
    });
  }

  // 加载当前选中模型的系统提示词
  function loadSystemPrompt(modelId) {
    var spInput = $('systemPromptInput');
    if (!spInput) return;
    if (!modelId) { spInput.value = ''; return; }
    var models = window.AppState.models || [];
    for (var i = 0; i < models.length; i++) {
      if (models[i].id === modelId && models[i].systemPrompt) {
        spInput.value = models[i].systemPrompt;
        return;
      }
    }
    spInput.value = '';
  }

  // 保存系统提示词到当前模型
  function saveSystemPrompt() {
    var spInput = $('systemPromptInput');
    var modelId = window.AppState.selectedModelId;
    if (!spInput || !modelId) return;
    var models = window.AppState.models || [];
    for (var i = 0; i < models.length; i++) {
      if (models[i].id === modelId) {
        models[i].systemPrompt = spInput.value;
        break;
      }
    }
  }

  // 系统提示词自动保存
  var spTimer = null;
  var spInput = $('systemPromptInput');
  if (spInput) {
    spInput.addEventListener('input', function() {
      if (spTimer) clearTimeout(spTimer);
      spTimer = setTimeout(saveSystemPrompt, 500);
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
      // 取当前模型的系统提示词
      var sp = '';
      var models = window.AppState.models || [];
      for (var i = 0; i < models.length; i++) {
        if (models[i].id === modelId && models[i].systemPrompt) {
          sp = models[i].systemPrompt;
          break;
        }
      }
      await streamChat(text, modelId, sp);
    } else {
      addTyping();
      await new Promise(function(r) { setTimeout(r, 400); });
      removeTyping();
      addMessage('当前没有运行中的模型。请到「模型」页启动一个模型后再开始对话。');
    }
    saveCurrentMessages();
    renderSidebar();
  }

  function streamChat(prompt, modelId, systemPrompt) {
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
    }, modelId, systemPrompt).then(function() {
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

  /* ---- 系统提示词 ---- */
  $('systemPromptToggle').addEventListener('click', function() {
    systemPromptVisible = !systemPromptVisible;
    var area = $('systemPromptArea');
    area.style.display = systemPromptVisible ? '' : 'none';
    if (systemPromptVisible && window.AppState.selectedModelId) {
      loadSystemPrompt(window.AppState.selectedModelId);
      $('systemPromptInput').focus();
    }
  });

  $('newChatBtn').addEventListener('click', newConversation);

  // 暴露全局接口给侧边栏 onclick
  window.switchConversation = switchConversation;
  window.deleteConversation = deleteConversation;

  /* ---- 图片生成 ---- */
  $('imageGenBtn').addEventListener('click', function() {
    var prompt = $('imagePrompt').value.trim();
    if (!prompt || isTyping) return;
    var area = $('imagePreviewArea');
    area.innerHTML = '<div class="image-preview-placeholder"><span class="image-preview-placeholder-icon">⏳</span><span class="image-preview-placeholder-text">正在生成图片…</span></div>';

    // 尝试调用运行中的 FLUX 模型
    if (window.chatService && window.chatService.generateImage) {
      window.chatService.generateImage(prompt, function(b64) {
        if (b64) {
          area.innerHTML = '<img src="data:image/png;base64,' + b64 + '" alt="' + escapeHtml(prompt) + '">';
        }
      }).then(function() {
        // done
      }).catch(function(e) {
        // 模型未运行或生成失败 → 显示占位符
        area.innerHTML = '<div class="image-preview-placeholder"><span class="image-preview-placeholder-icon">🎨</span><span class="image-preview-placeholder-text">「' + escapeHtml(prompt) + '」<br><span style="font-size:12px;color:var(--text-muted)">' + escapeHtml(e.message || '请先启动 FLUX 模型') + '</span></span></div>';
      });
    } else {
      // 兜底占位
      setTimeout(function() {
        area.innerHTML = '<div class="image-preview-placeholder"><span class="image-preview-placeholder-icon">🎨</span><span class="image-preview-placeholder-text">「' + escapeHtml(prompt) + '」<br><span style="font-size:12px;color:var(--text-muted)">（图片生成功能接入中）</span></span></div>';
      }, 1000);
    }
  });
  $('imagePrompt').addEventListener('keydown', function(e) {
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); $('imageGenBtn').click(); }
  });

  /* ---- 翻译 ---- */
  // 翻译 prompt 模板，保存到 localStorage
  var TRANSLATE_TEMPLATE_KEY = 'flowy_translate_template';
  var translateTemplateTimer = null;

  function loadTranslateTemplate() {
    var saved = localStorage.getItem(TRANSLATE_TEMPLATE_KEY);
    if (saved) {
      $('translateTemplateInput').value = saved;
    }
  }

  function saveTranslateTemplate() {
    var val = $('translateTemplateInput').value;
    localStorage.setItem(TRANSLATE_TEMPLATE_KEY, val);
  }

  // 模板切换
  $('translateTemplateToggle').addEventListener('click', function() {
    var area = $('translateTemplateArea');
    if (area.style.display === 'none') {
      area.style.display = '';
      loadTranslateTemplate();
    } else {
      area.style.display = 'none';
    }
  });

  // 自动保存模板（500ms 防抖）
  $('translateTemplateInput').addEventListener('input', function() {
    if (translateTemplateTimer) clearTimeout(translateTemplateTimer);
    translateTemplateTimer = setTimeout(saveTranslateTemplate, 500);
  });

  $('translateBtn').addEventListener('click', function() {
    var text = $('translateSrcText').value.trim();
    if (!text || isTyping) return;
    var src = $('translateSrc').value;
    var dst = $('translateDst').value;
    var output = $('translateOutput');

    // 读取模板并替换占位符
    var template = $('translateTemplateInput').value.trim();
    if (!template) {
      template = '将以下文本翻译为 {target_lang}，注意只需要输出翻译后的结果，不要额外解释：\n\n{source_text}';
    }
    var prompt = template
      .replace(/\{target_lang\}/g, dst === 'zh' ? '中文' : dst === 'en' ? 'English' : dst === 'ja' ? '日本語' : dst === 'ko' ? '한국어' : dst === 'fr' ? 'Français' : dst === 'de' ? 'Deutsch' : dst === 'es' ? 'Español' : dst === 'pt' ? 'Português' : dst === 'ru' ? 'Русский' : dst)
      .replace(/\{source_text\}/g, text);

    // 翻译优先使用 HY-MT2（翻译专用模型），没有则用用户选择的模型
    var models = window.AppState.models || [];
    var modelId = null;
    var hyMt2 = models.find(function(m) {
      return (m.id === 'HY-MT2-1.7B' || m.id === 'hy-mt2-1.7b' || m.id.toUpperCase().includes('HY-MT2')) && m.status === 'running';
    });
    if (hyMt2) {
      modelId = hyMt2.id;
    } else {
      modelId = window.AppState.selectedModelId;
      var selectedRunning = models.some(function(m) { return m.id === modelId && m.status === 'running'; });
      if (!modelId || !selectedRunning) {
        output.innerHTML = '<span style="color:var(--warn-color)">⚠️ 没有可用的翻译模型（HY-MT2 或选中的模型未在运行）</span>';
        return;
      }
    }

    isTyping = true;
    output.innerHTML = '<span style="color:var(--text-muted)">⏳ 翻译中…</span>';

    // 添加 source 语言信息到 prompt（非 auto 时）
    if (src !== 'auto') {
      prompt = '源语言为' + (src === 'zh' ? '中文' : src === 'en' ? '英语' : src === 'ja' ? '日语' : src === 'ko' ? '韩语' : src === 'fr' ? '法语' : src === 'de' ? '德语' : src === 'es' ? '西班牙语' : src === 'pt' ? '葡萄牙语' : src === 'ru' ? '俄语' : src) + '。' + prompt;
    }

    var fullText = '';
    var started = false;
    window.chatService.chat(prompt, function(token) {
      // 收到首个 token 时清空 loading 占位
      if (!started) {
        output.innerHTML = '';
        started = true;
      }
      fullText += token;
      output.innerHTML = escapeHtml(fullText);
    }, modelId).then(function() {
      // 流结束：重置标志位（chatService 完成时 resolve，不发 __DONE__ token）
      isTyping = false;
      if (!fullText) output.innerHTML = '<span style="color:var(--text-muted)">（无输出）</span>';
    }).catch(function(e) {
      isTyping = false;
      output.innerHTML = '<span style="color:var(--warn-color)">翻译失败: ' + escapeHtml(String(e)) + '</span>';
    });
  });

  // 翻译原文字数统计
  $('translateSrcText').addEventListener('input', function() {
    var len = this.value.length;
    $('translateSrcCount').textContent = len + ' 字';
  });

  // 回车执行翻译（Shift+Enter 换行）
  $('translateSrcText').addEventListener('keydown', function(e) {
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); $('translateBtn').click(); }
  });


  // 初始：建一个默认对话
  newConversation();
  refreshModelSelect();
})();
