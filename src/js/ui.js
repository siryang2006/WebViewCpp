/* ================================================================
   ui.js — 纯 UI 交互（无业务逻辑、不碰 C++ 桥接）
   主题切换、功能卡片、推荐问题、模式选择、Tab 切换、侧边栏对话项
   ================================================================ */

/* ---- 主题切换 ---- */
(function() {
  var themeToggle = $('themeToggle');
  var currentTheme = 'dark'; // 内存状态，WebView2 set_html 无 origin 不支持 localStorage
  document.documentElement.setAttribute('data-theme', currentTheme);
  themeToggle.textContent = '☀';

  themeToggle.addEventListener('click', function() {
    currentTheme = currentTheme === 'dark' ? 'light' : 'dark';
    document.documentElement.setAttribute('data-theme', currentTheme);
    themeToggle.textContent = currentTheme === 'dark' ? '☀' : '🌙';
  });
})();

/* ---- 功能卡片切换 + 输入面板联动 ---- */
function switchFeature(feature) {
  var panels = document.querySelectorAll('.input-panel');
  var found = false;
  for (var i = 0; i < panels.length; i++) {
    var match = panels[i].dataset.feature === feature;
    panels[i].classList.toggle('active', match);
    if (match) found = true;
  }
  // 无对应面板时回退到聊天
  if (!found) {
    for (var j = 0; j < panels.length; j++) {
      panels[j].classList.toggle('active', panels[j].dataset.feature === 'chat');
    }
  }
  document.querySelectorAll('.feature-card').forEach(function(c) {
    c.classList.toggle('active', c.dataset.feature === feature);
  });
}

document.querySelectorAll('.feature-card').forEach(function(card) {
  card.addEventListener('click', function() {
    switchFeature(card.dataset.feature);
  });
});

$('cardsPrev').addEventListener('click', function() { $('cardsTrack').scrollBy({ left: -200, behavior: 'smooth' }); });
$('cardsNext').addEventListener('click', function() { $('cardsTrack').scrollBy({ left: 200, behavior: 'smooth' }); });

/* ---- 推荐问题 ---- */
document.querySelectorAll('.chip').forEach(function(chip) {
  chip.addEventListener('click', function() {
    $('inputBox').value = chip.dataset.q;
    $('inputBox').dispatchEvent(new Event('input'));
    $('inputBox').focus();
  });
});

/* ---- 模式选择 ---- */
document.querySelectorAll('.mode-btn').forEach(function(btn) {
  btn.addEventListener('click', function() {
    document.querySelectorAll('.mode-btn').forEach(function(b) { b.classList.remove('active'); });
    btn.classList.add('active');
  });
});

/* ---- Tab 切换（模型 / 应用）---- */
document.querySelectorAll('.tab-btn').forEach(function(btn) {
  btn.addEventListener('click', function() {
    document.querySelectorAll('.tab-btn').forEach(function(b) { b.classList.remove('active'); });
    btn.classList.add('active');
    var tab = btn.dataset.tab;
    if (tab === 'model') {
      $('modelPage').classList.add('active');
      document.querySelector('.feature-cards-section').style.display = 'none';
      document.querySelector('.content-area').style.display = 'none';
    } else {
      $('modelPage').classList.remove('active');
      document.querySelector('.feature-cards-section').style.display = '';
      document.querySelector('.content-area').style.display = '';
      // 切到应用页时刷新模型下拉（可能其他模型已启动/停止）
      if (window.refreshModelSelect) window.refreshModelSelect();
    }
  });
});

/* ---- 翻译：交换语言 ---- */
$('translateSwap').addEventListener('click', function() {
  var src = $('translateSrc');
  var dst = $('translateDst');
  var tmp = src.value;
  // 如果源是 auto，交换后目标保留原值（dst 无 auto 选项）
  if (tmp === 'auto') return;
  src.value = dst.value;
  dst.value = tmp;
});

/* ---- 翻译：复制结果 ---- */
$('translateCopy').addEventListener('click', function() {
  var output = $('translateOutput');
  var text = output.textContent;
  if (text && text !== '翻译结果将显示在这里') {
    navigator.clipboard.writeText(text).catch(function() {});
  }
});

/* ---- 图片上传 ---- */
$('imageUploadBtn').addEventListener('click', function() {
  $('imageUploadInput').click();
});
$('imageUploadInput').addEventListener('change', function() {
  var file = this.files[0];
  if (!file) return;
  var reader = new FileReader();
  reader.onload = function(e) {
    var area = $('imagePreviewArea');
    area.innerHTML = '<img src="' + e.target.result + '" alt="上传的参考图片">';
  };
  reader.readAsDataURL(file);
});

// 初始显示模型页
(function() {
  document.querySelector('.tab-btn[data-tab="model"]').click();
})();


