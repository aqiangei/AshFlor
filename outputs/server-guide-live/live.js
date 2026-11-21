(() => {
  const dialog = document.getElementById('serverNotice');
  if (!dialog || window.ashflorServerStatusLoaded) return;
  window.ashflorServerStatusLoaded = true;
  const style = document.createElement('style');
  style.textContent = [
    '#serverNotice [data-live-status]{flex-direction:column;align-items:flex-start;gap:4px}',
    '.server-live-status{display:block;font-size:13px;color:#e8f8ec;text-decoration:none;line-height:1.4}',
    '#serverNotice .server-topup-tips{display:flex;flex-wrap:wrap;gap:6px 10px;align-items:center;margin:8px 0;padding:0;list-style:none}',
    '#serverNotice .server-topup-tips li{margin:0;color:#122c48;font-size:13px;line-height:1.4}',
  ].join(' ');
  document.head.append(style);
  const qqHref = 'https://wpa.qq.com/msgrd?v=3&uin=3028753757&site=qq&menu=yes';
  const official = dialog.querySelector('.server-official-site');
  if (official) {
    official.innerHTML =
      '<strong>氪金联系QQ</strong>' +
      '<a href="' + qqHref + '" target="_blank" rel="noopener noreferrer">3028753757</a>';
  }
  for (const stale of dialog.querySelectorAll('[data-afdian],[data-server-renewal]')) {
    stale.remove();
  }
  if (!dialog.querySelector('[data-server-topup-tips]')) {
    const tips = document.createElement('ul');
    tips.className = 'server-topup-tips';
    tips.dataset.serverTopupTips = '';
    for (const line of [
      '25 元一个随机 Apex',
      '10 元一个永久超凡拟态（Mimic）',
      '10 元迁移服务器',
      '三个服务器已续租半年，共 2892 元',
    ]) {
      const item = document.createElement('li');
      item.textContent = line;
      tips.append(item);
    }
    if (official) official.after(tips);
    else dialog.querySelector('.server-notice-body')?.prepend(tips);
  }
  const fifth = Array.from(dialog.querySelectorAll('a[data-server]'))
    .find(link => link.textContent.trim().startsWith('五服'));
  if (fifth && !dialog.querySelector('[data-flowrix-private]')) {
    const link = document.createElement('a');
    link.href = 'http://103.91.210.16:6767/';
    link.dataset.flowrixPrivate = '';
    link.textContent = '原版 Flowrix.pro 私服 · 103.91.210.16:6767';
    fifth.after(link);
  }
  const entries = [
    ['一服', 'http://202.189.8.109:67/'],
    ['二服', 'http://202.189.23.253:67/'],
    ['三服', 'http://103.91.210.16:67/'],
  ];
  const rows = entries.map(([name, url]) => {
    const link = Array.from(dialog.querySelectorAll('a[data-server]'))
      .find(a => a.textContent.trim().startsWith(name));
    if (!link) return null;
    link.href = url;
    link.dataset.liveStatus = '';
    link.textContent = name + ' · ' + new URL(url).host;
    const label = document.createElement('small');
    label.className = 'server-live-status';
    label.textContent = '正在检测…';
    label.title = '游戏内人数约每 10 秒采样，延迟为当前浏览器到该服的 HTTP 往返时间';
    link.append(label);
    return {url, label};
  }).filter(Boolean);
  let generation = 0;
  let timer;
  const requests = new Set();
  const stop = () => {
    ++generation;
    clearTimeout(timer);
    for (const request of requests) request.abort();
    requests.clear();
  };
  const refresh = async () => {
    if (!dialog.open || document.hidden) return;
    const startedGeneration = generation;
    await Promise.all(rows.map(async ({url, label}) => {
      const controller = new AbortController();
      requests.add(controller);
      const timeout = setTimeout(() => controller.abort(), 3500);
      const started = performance.now();
      try {
        const response = await fetch(new URL('/api/server-status', url),
          {cache: 'no-store', credentials: 'omit', signal: controller.signal});
        const data = await response.json();
        if (response.status === 503 && data.error === 'status_unavailable') {
          if (startedGeneration === generation && dialog.open)
            label.textContent = '人数更新中 · ' + Math.round(performance.now() - started) + ' ms';
          return;
        }
        if (!response.ok) throw Error('unavailable');
        if (!Number.isSafeInteger(data.players) || data.players < 0) throw Error('invalid count');
        if (startedGeneration !== generation || !dialog.open) return;
        const latency = Math.round(performance.now() - started);
        const quality = latency < 150 ? '流畅' : latency < 350 ? '一般' : '较慢';
        label.textContent = data.players + ' 人在线 · ' + latency + ' ms · ' + quality;
      } catch (_) {
        if (startedGeneration === generation && dialog.open)
          label.textContent = '人数未知 · 暂时无法连接';
      } finally {
        clearTimeout(timeout);
        requests.delete(controller);
      }
    }));
    if (startedGeneration === generation && dialog.open && !document.hidden)
      timer = setTimeout(refresh, 5000);
  };
  const sync = () => {
    stop();
    if (dialog.open && !document.hidden) refresh();
  };
  new MutationObserver(sync).observe(dialog, {attributes: true, attributeFilter: ['open']});
  document.addEventListener('visibilitychange', sync);
  dialog.addEventListener('close', stop);
  sync();
})();
