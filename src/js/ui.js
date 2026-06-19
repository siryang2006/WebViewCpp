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

/* ---- 功能卡片切换 ---- */
document.querySelectorAll('.feature-card').forEach(function(card) {
  card.addEventListener('click', function() {
    document.querySelectorAll('.feature-card').forEach(function(c) { c.classList.remove('active'); });
    card.classList.add('active');
  });
});

$('cardsPrev').addEventListener('click', function() { $('cardsTrack').scrollBy({ left: -240, behavior: 'smooth' }); });
$('cardsNext').addEventListener('click', function() { $('cardsTrack').scrollBy({ left: 240, behavior: 'smooth' }); });

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
    }
  });
});

// 初始显示模型页
(function() {
  document.querySelector('.tab-btn[data-tab="model"]').click();
})();

/* ---- 侧边栏对话项点击 ---- */
document.querySelectorAll('.chat-item').forEach(function(item) {
  item.addEventListener('click', function(e) {
    if (e.target.classList.contains('chat-item-delete')) {
      item.remove();
      return;
    }
    document.querySelectorAll('.chat-item').forEach(function(i) { i.classList.remove('active'); });
    item.classList.add('active');
  });
});
