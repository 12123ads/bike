"""网页的 HTML/CSS/JS，作为字符串内联。

为什么不放静态文件目录：整个前端不到 20 KB，内联之后
**镜像里少一个目录、`StaticFiles` 少一个挂载点、也不用担心
`.dockerignore` 把它排除掉**（`*.md` 那条已经差点误伤过文档）。
代价是这个文件长，且没有语法高亮。

地图用高德 JS API 2.0。**坐标直接用服务端算好的 GCJ-02**（契约 §7 的
`gla`/`glo`）——高德的底图就是 GCJ-02，前端不做任何坐标转换。
"""

from __future__ import annotations

LOGIN_HTML = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>电瓶车定位 · 登录</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body {
    margin: 0; min-height: 100vh; display: grid; place-items: center;
    font: 15px/1.5 -apple-system, "PingFang SC", "Microsoft YaHei", sans-serif;
    background: #14161a; color: #e6e6e6;
  }
  .card {
    width: min(380px, 92vw); padding: 28px;
    background: #1d2025; border: 1px solid #2c3038; border-radius: 12px;
  }
  h1 { margin: 0 0 4px; font-size: 19px; }
  p.hint { margin: 0 0 20px; color: #8b93a1; font-size: 13px; }
  label { display: block; margin-bottom: 6px; font-size: 13px; color: #b8c0cc; }
  input {
    width: 100%; padding: 10px 12px; font-size: 14px; font-family: ui-monospace, monospace;
    background: #14161a; color: #e6e6e6;
    border: 1px solid #343a44; border-radius: 8px;
  }
  input:focus { outline: none; border-color: #4a90d9; }
  button {
    width: 100%; margin-top: 16px; padding: 11px; font-size: 15px; font-weight: 500;
    background: #2f6fb3; color: #fff; border: 0; border-radius: 8px; cursor: pointer;
  }
  button:hover { background: #3a7fc8; }
  button:disabled { background: #3a4048; cursor: default; }
  .err {
    margin-top: 14px; padding: 9px 12px; font-size: 13px; border-radius: 8px;
    background: #3a2226; border: 1px solid #5c2b32; color: #ffb3b8; display: none;
  }
</style>
</head>
<body>
<div class="card">
  <h1>电瓶车定位</h1>
  <p class="hint">用服务端的 API token 登录。<code>ebike-server init</code> 打印过它，
    也在配置文件的 <code>api_token</code> 里。</p>
  <label for="tok">API token</label>
  <input id="tok" type="password" autocomplete="current-password" spellcheck="false"
         placeholder="粘贴 token">
  <button id="go">登录</button>
  <div class="err" id="err"></div>
</div>
<script>
const tok = document.getElementById('tok');
const go = document.getElementById('go');
const err = document.getElementById('err');

function fail(msg) { err.textContent = msg; err.style.display = 'block'; }

async function login() {
  const v = tok.value.trim();
  if (!v) { fail('token 不能为空'); return; }
  go.disabled = true; err.style.display = 'none';
  try {
    const r = await fetch('/ui/login', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({token: v}),
    });
    if (r.ok) { location.href = '/'; return; }
    const d = await r.json().catch(() => ({}));
    fail(d.detail || ('登录失败（HTTP ' + r.status + '）'));
  } catch (e) {
    fail('连不上服务端：' + e.message);
  }
  go.disabled = false;
}

go.addEventListener('click', login);
tok.addEventListener('keydown', e => { if (e.key === 'Enter') login(); });
tok.focus();
</script>
</body>
</html>
"""

# <!--INDEX_PLACEHOLDER-->

INDEX_HTML = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>电瓶车定位</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  html, body { margin: 0; height: 100%; }
  body {
    font: 14px/1.5 -apple-system, "PingFang SC", "Microsoft YaHei", sans-serif;
    background: #14161a; color: #e6e6e6;
    display: grid; grid-template-columns: 320px 1fr; grid-template-rows: 100%;
  }
  @media (max-width: 780px) {
    body { grid-template-columns: 100%; grid-template-rows: auto 1fr; }
    #side { max-height: 45vh; }
  }

  #side {
    overflow-y: auto; padding: 16px;
    background: #1a1d22; border-right: 1px solid #2c3038;
  }
  #map { width: 100%; height: 100%; background: #22252b; }

  header { display: flex; align-items: baseline; gap: 8px; margin-bottom: 14px; }
  header h1 { margin: 0; font-size: 17px; }
  header .dev { font-family: ui-monospace, monospace; color: #8b93a1; font-size: 12px; }

  .badge {
    display: inline-block; padding: 2px 8px; border-radius: 20px;
    font-size: 12px; font-weight: 500;
  }
  .on  { background: #1e3a28; color: #7ee2a8; border: 1px solid #2d5a3d; }
  .off { background: #3a2226; color: #ffb3b8; border: 1px solid #5c2b32; }
  .unk { background: #2c3038; color: #9aa3b0; border: 1px solid #3a4048; }

  .grid { display: grid; grid-template-columns: auto 1fr; gap: 7px 12px; margin: 12px 0 18px; }
  .grid dt { color: #8b93a1; font-size: 13px; }
  .grid dd { margin: 0; font-variant-numeric: tabular-nums; }

  h2 {
    margin: 18px 0 8px; font-size: 12px; font-weight: 600;
    text-transform: uppercase; letter-spacing: .06em; color: #7d8595;
  }

  .row { display: flex; gap: 8px; flex-wrap: wrap; }
  button {
    padding: 7px 12px; font-size: 13px; cursor: pointer;
    background: #262a31; color: #dbe1ea;
    border: 1px solid #363c46; border-radius: 7px;
  }
  button:hover:not(:disabled) { background: #2f343d; border-color: #454c58; }
  button:disabled { opacity: .45; cursor: not-allowed; }
  button.danger { background: #3a2226; border-color: #5c2b32; color: #ffb3b8; }
  button.danger:hover:not(:disabled) { background: #4a2a2f; }

  select {
    padding: 6px 10px; font-size: 13px; background: #262a31; color: #dbe1ea;
    border: 1px solid #363c46; border-radius: 7px;
  }

  ul.events { list-style: none; margin: 0; padding: 0; font-size: 13px; }
  ul.events li {
    padding: 6px 0; border-bottom: 1px solid #24282f;
    display: flex; justify-content: space-between; gap: 10px;
  }
  ul.events li:last-child { border-bottom: 0; }
  ul.events .k { font-weight: 500; }
  ul.events .t { color: #7d8595; font-size: 12px; white-space: nowrap; }
  .ev-unlock_deny .k, .ev-lowbatt .k { color: #ffb3b8; }
  .ev-unlock_ok .k { color: #7ee2a8; }
  .ev-motion .k { color: #ffd479; }

  #toast {
    position: fixed; left: 50%; bottom: 24px; transform: translateX(-50%);
    padding: 10px 18px; border-radius: 8px; font-size: 13px;
    background: #262a31; border: 1px solid #454c58; color: #e6e6e6;
    opacity: 0; transition: opacity .2s; pointer-events: none; z-index: 999;
    max-width: 80vw;
  }
  #toast.show { opacity: 1; }
  #toast.bad { background: #3a2226; border-color: #5c2b32; color: #ffb3b8; }

  .note {
    margin-top: 6px; font-size: 12px; color: #7d8595;
  }
  .maperr {
    padding: 20px; margin: 20px; border-radius: 10px;
    background: #24282f; border: 1px solid #363c46; color: #b8c0cc;
    font-size: 13px; line-height: 1.7;
  }
  .maperr code { color: #ffd479; }
  footer { margin-top: 20px; font-size: 12px; color: #6b7280; }
  footer a { color: #7d8595; }
</style>
</head>
<body>
<aside id="side">
  <header>
    <h1>电瓶车</h1>
    <span class="dev" id="devlabel"></span>
  </header>

  <div class="row" id="devrow" style="display:none">
    <select id="devsel"></select>
  </div>

  <div style="margin-top:12px">
    <span class="badge unk" id="online">…</span>
    <span class="badge unk" id="moving">…</span>
    <span class="badge unk" id="lock">…</span>
  </div>

  <dl class="grid">
    <dt>位置</dt><dd id="pos">—</dd>
    <dt>精度</dt><dd id="acc">—</dd>
    <dt>定位方式</dt><dd id="src">—</dd>
    <dt>电压</dt><dd id="volt">—</dd>
    <dt>电量</dt><dd id="pct">—</dd>
    <dt>围栏</dt><dd id="gf">—</dd>
    <dt>最后上报</dt><dd id="ls">—</dd>
  </dl>

  <h2>轨迹</h2>
  <div class="row">
    <select id="span">
      <option value="3600">最近 1 小时</option>
      <option value="21600">最近 6 小时</option>
      <option value="86400" selected>最近 24 小时</option>
      <option value="604800">最近 7 天</option>
      <option value="2592000">最近 30 天</option>
    </select>
    <button id="fit">缩放到轨迹</button>
  </div>
  <div class="note" id="trackinfo">—</div>

  <h2>指令</h2>
  <div class="row">
    <button data-cmd="locate">立刻定位</button>
    <button data-cmd="ping">要遥测</button>
    <button data-cmd="lock">上锁</button>
    <button data-cmd="unlock" class="danger" id="btn-unlock">远程开锁</button>
  </div>
  <div class="note" id="cmdnote"></div>

  <h2>待确认下行</h2>
  <div class="note" id="pending">—</div>

  <h2>最近事件</h2>
  <ul class="events" id="events"><li><span class="k">—</span></li></ul>

  <footer>
    <button id="logout" style="padding:5px 10px;font-size:12px">退出登录</button>
  </footer>
</aside>

<div id="map"></div>
<div id="toast"></div>

<script>
// ---- 状态 ----------------------------------------------------------------
let CFG = null, DEV = null, MAP = null, AMapNS = null;
let marker = null, circle = null, polyline = null;
let lastState = null, followed = false;

const $ = id => document.getElementById(id);

function toast(msg, bad) {
  const t = $('toast');
  t.textContent = msg;
  t.className = 'show' + (bad ? ' bad' : '');
  clearTimeout(toast._h);
  toast._h = setTimeout(() => { t.className = ''; }, 3200);
}

async function api(path, opts) {
  const r = await fetch(path, opts);
  if (r.status === 401) { location.href = '/ui/login'; throw new Error('未登录'); }
  const body = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(body.detail || ('HTTP ' + r.status));
  return body;
}

// ---- 时间显示 -----------------------------------------------------------
function fmtTime(unix) {
  if (!unix) return '—';
  const d = new Date(unix * 1000);
  return d.toLocaleString('zh-CN', {hour12: false});
}

function fmtAgo(unix) {
  if (!unix) return '从未上报';
  const s = Math.max(0, Math.floor(Date.now() / 1000 - unix));
  if (s < 60) return s + ' 秒前';
  if (s < 3600) return Math.floor(s / 60) + ' 分钟前';
  if (s < 86400) return Math.floor(s / 3600) + ' 小时前';
  return Math.floor(s / 86400) + ' 天前';
}

// ---- 侧栏渲染 -----------------------------------------------------------
function badge(el, state, onText, offText) {
  if (state === null || state === undefined) {
    el.className = 'badge unk'; el.textContent = '未知'; return;
  }
  el.className = 'badge ' + (state ? 'on' : 'off');
  el.textContent = state ? onText : offText;
}

function renderState(s) {
  lastState = s;
  badge($('online'), s.on, '在线', '离线');
  badge($('moving'), s.mo === undefined ? null : (s.mo === 'moving'), '移动中', '静止');
  // lk 为 null 是常态（没接位置反馈开关），必须显示「未知」不能显示「未上锁」
  badge($('lock'), s.lk === null || s.lk === undefined ? null : s.lk, '已上锁', '未上锁');

  $('pos').textContent = (s.la !== undefined && s.la !== null)
    ? s.la.toFixed(6) + ', ' + s.lo.toFixed(6) : '—';
  $('acc').textContent = s.a !== undefined && s.a !== null
    ? Math.round(s.a) + ' 米' : '—';
  $('src').textContent = s.s === 'g' ? '卫星定位' : (s.s === 'l' ? '基站定位' : '—');
  $('volt').textContent = s.v !== undefined && s.v !== null ? s.v.toFixed(1) + ' V' : '—';
  $('pct').textContent = s.pct !== undefined && s.pct !== null ? s.pct + ' %' : '—';
  $('gf').textContent = s.gf === 'in' ? '围栏内' : (s.gf === 'out' ? '围栏外' : '未设置');
  $('ls').textContent = s.ls ? fmtAgo(s.ls) + '（' + fmtTime(s.ls) + '）' : '从未上报';

  drawMarker(s);
}

function renderEvents(list) {
  const ul = $('events');
  if (!list.length) { ul.innerHTML = '<li><span class="k">暂无事件</span></li>'; return; }
  const LABEL = {
    boot: '开机', motion: '检测到移动', still: '静止',
    unlock_ok: '开锁成功', unlock_deny: '开锁被拒',
    lock_state: '锁状态变化', lowbatt: '电量低', nfc_err: 'NFC 异常',
  };
  ul.innerHTML = list.map(e => {
    let extra = '';
    if (e.detail) {
      if (e.detail.mg !== undefined) extra = ' ' + e.detail.mg + ' mg';
      else if (e.detail.locked !== undefined) extra = e.detail.locked ? '（锁上）' : '（打开）';
      else if (e.detail.lv !== undefined) extra = '（' + e.detail.lv + ' 级，' + e.detail.v + ' V）';
      else if (e.detail.uid !== undefined) extra = ' uid=' + e.detail.uid;
    }
    return '<li class="ev-' + e.kind + '"><span class="k">'
      + (LABEL[e.kind] || e.kind) + extra
      + '</span><span class="t">' + fmtAgo(e.t_srv) + '</span></li>';
  }).join('');
}

function renderPending(rows) {
  if (!rows.length) { $('pending').textContent = '无'; return; }
  $('pending').innerHTML = rows.map(r =>
    r.id + ' → ' + r.suffix + '，已尝试 ' + r.tries + ' 次'
  ).join('<br>') + '<br><span style="color:#7d8595">'
    + '省电档下设备可能几十分钟才上线，届时会自动送达。</span>';
}

// ---- 地图 ---------------------------------------------------------------
function drawMarker(s) {
  // 用服务端算好的 GCJ-02（契约 §7 的 gla/glo）—— 高德底图就是 GCJ-02，
  // 前端不做任何坐标转换。gla 缺失时不回落到 la，那会让车偏几百米。
  if (!MAP || s.gla === undefined || s.gla === null) return;
  const pos = [s.glo, s.gla];

  if (!marker) {
    marker = new AMapNS.Marker({position: pos, map: MAP, title: DEV,
                                anchor: 'bottom-center'});
    circle = new AMapNS.Circle({
      center: pos, radius: s.a || 0, map: MAP,
      strokeColor: '#4a90d9', strokeWeight: 1, strokeOpacity: 0.7,
      fillColor: '#4a90d9', fillOpacity: 0.15,
    });
    MAP.setZoomAndCenter(16, pos);
    followed = true;
  } else {
    marker.setPosition(pos);
    circle.setCenter(pos);
    circle.setRadius(s.a || 0);
  }
  marker.setLabel({direction: 'top', content:
    '<div style="padding:2px 6px;background:#1d2025;border:1px solid #363c46;'
    + 'border-radius:5px;color:#e6e6e6;font-size:12px">'
    + (s.on ? '在线' : '离线') + ' · ' + fmtAgo(s.ls) + '</div>'});
}

function drawTrack(points) {
  if (!MAP) return;
  // 服务端的 /track 返回 WGS84（那是落库的原始值）。高德底图是 GCJ-02，
  // 所以这里必须转一次 —— 不转的话轨迹会整体偏几百米，而实时点是准的，
  // 两者对不上会让人以为轨迹坏了。
  const path = points
    .filter(p => p.lat !== null && p.lon !== null)
    .map(p => wgs84ToGcj02(p.lat, p.lon));

  if (polyline) { polyline.setMap(null); polyline = null; }
  if (path.length < 2) {
    $('trackinfo').textContent = points.length + ' 个点（不足以画线）';
    return;
  }
  polyline = new AMapNS.Polyline({
    path: path, map: MAP,
    strokeColor: '#ffd479', strokeWeight: 4, strokeOpacity: 0.85,
    lineJoin: 'round', showDir: true,
  });
  $('trackinfo').textContent = points.length + ' 个点，'
    + fmtTime(points[0].t_srv) + ' 起';
}

// WGS84 → GCJ-02。和服务端 geo.py 的实现是同一个算法（同一套常数），
// 所以实时点（服务端转的）和轨迹点（这里转的）落在同一个坐标系里。
const GCJ_A = 6378245.0, GCJ_EE = 0.00669342162296594;
function outOfChina(lat, lon) {
  return !(lon > 72.004 && lon < 137.8347 && lat > 0.8293 && lat < 55.8271);
}
function tLat(x, y) {
  let r = -100 + 2*x + 3*y + 0.2*y*y + 0.1*x*y + 0.2*Math.sqrt(Math.abs(x));
  r += (20*Math.sin(6*x*Math.PI) + 20*Math.sin(2*x*Math.PI)) * 2/3;
  r += (20*Math.sin(y*Math.PI) + 40*Math.sin(y/3*Math.PI)) * 2/3;
  r += (160*Math.sin(y/12*Math.PI) + 320*Math.sin(y*Math.PI/30)) * 2/3;
  return r;
}
function tLon(x, y) {
  let r = 300 + x + 2*y + 0.1*x*x + 0.1*x*y + 0.1*Math.sqrt(Math.abs(x));
  r += (20*Math.sin(6*x*Math.PI) + 20*Math.sin(2*x*Math.PI)) * 2/3;
  r += (20*Math.sin(x*Math.PI) + 40*Math.sin(x/3*Math.PI)) * 2/3;
  r += (150*Math.sin(x/12*Math.PI) + 300*Math.sin(x/30*Math.PI)) * 2/3;
  return r;
}
function wgs84ToGcj02(lat, lon) {
  if (outOfChina(lat, lon)) return [lon, lat];
  let dLat = tLat(lon - 105, lat - 35), dLon = tLon(lon - 105, lat - 35);
  const rad = lat / 180 * Math.PI;
  let magic = Math.sin(rad);
  magic = 1 - GCJ_EE * magic * magic;
  const sq = Math.sqrt(magic);
  dLat = (dLat * 180) / ((GCJ_A * (1 - GCJ_EE)) / (magic * sq) * Math.PI);
  dLon = (dLon * 180) / (GCJ_A / sq * Math.cos(rad) * Math.PI);
  return [lon + dLon, lat + dLat];   // 高德要 [经度, 纬度]
}

function loadMap(key, security) {
  return new Promise((resolve, reject) => {
    if (!key) { reject(new Error('nokey')); return; }
    if (security) {
      window._AMapSecurityConfig = {securityJsCode: security};
    }
    const s = document.createElement('script');
    s.src = 'https://webapi.amap.com/loader.js';
    s.onerror = () => reject(new Error('loader 加载失败'));
    s.onload = () => {
      window.AMapLoader.load({
        key: key, version: '2.0', plugins: ['AMap.Scale', 'AMap.ToolBar'],
      }).then(resolve).catch(reject);
    };
    document.head.appendChild(s);
  });
}

function mapFailed(msg) {
  $('map').innerHTML = '<div class="maperr"><b>地图没加载出来：</b>' + msg
    + '<br><br>其余功能不受影响，位置在左侧以经纬度显示。'
    + '<br><br>检查：<br>'
    + '· <code>/root/gaode.key</code> 里是不是一行 32 位的 key<br>'
    + '· 高德控制台里这个 key 的类型要选 <b>Web端(JS API)</b><br>'
    + '· 如果配了域名白名单，要把访问这个页面用的域名加进去<br>'
    + '· 这台机器要能访问 <code>webapi.amap.com</code></div>';
}

// ---- 轮询 ---------------------------------------------------------------
async function refreshState() {
  try { renderState(await api('/api/state/' + DEV)); }
  catch (e) { console.warn('state 刷新失败', e); }
}

async function refreshSlow() {
  try {
    const [ev, pd] = await Promise.all([
      api('/api/events?dev=' + DEV + '&limit=30'),
      api('/api/pending?dev=' + DEV),
    ]);
    renderEvents(ev.events);
    renderPending(pd.pending);
  } catch (e) { console.warn('事件刷新失败', e); }
}

async function refreshTrack() {
  const span = parseInt($('span').value, 10);
  const since = Math.floor(Date.now() / 1000) - span;
  try {
    const t = await api('/api/track?dev=' + DEV + '&since=' + since);
    drawTrack(t.points);
  } catch (e) { $('trackinfo').textContent = '轨迹加载失败：' + e.message; }
}

// ---- 指令 ---------------------------------------------------------------
async function sendCmd(cmd, btn) {
  if (cmd === 'unlock' &&
      !confirm('远程开锁会绕过手机 NFC 的挑战应答。\\n\\n确定要开锁吗？')) return;
  btn.disabled = true;
  try {
    const r = await api('/api/cmd/' + DEV + '/' + cmd, {method: 'POST'});
    toast('已入队 ' + r.queued + '（设备下次上线时送达）');
    refreshSlow();
  } catch (e) {
    toast(e.message, true);
  }
  btn.disabled = false;
}

// ---- 启动 ---------------------------------------------------------------
async function main() {
  CFG = await api('/api/config');
  if (!CFG.devices.length) { toast('配置里没有设备', true); return; }

  DEV = CFG.devices[0].id;
  $('devlabel').textContent = DEV;
  if (CFG.devices.length > 1) {
    $('devrow').style.display = 'flex';
    $('devsel').innerHTML = CFG.devices
      .map(d => '<option value="' + d.id + '">' + d.id + '</option>').join('');
    $('devsel').addEventListener('change', e => {
      DEV = e.target.value;
      $('devlabel').textContent = DEV;
      if (marker) { marker.setMap(null); marker = null; }
      if (circle) { circle.setMap(null); circle = null; }
      refreshState(); refreshSlow(); refreshTrack();
    });
  }

  if (!CFG.allow_remote_unlock) {
    $('btn-unlock').disabled = true;
    $('cmdnote').textContent = '远程开锁已禁用（它绕过 NFC 挑战应答）。'
      + '要用需在服务端配置里设 allow_remote_unlock=true，固件侧也要打开。';
  }

  document.querySelectorAll('button[data-cmd]').forEach(b => {
    b.addEventListener('click', () => sendCmd(b.dataset.cmd, b));
  });
  $('span').addEventListener('change', refreshTrack);
  $('logout').addEventListener('click', async () => {
    await fetch('/ui/logout', {method: 'POST'});
    location.href = '/ui/login';
  });
  $('fit').addEventListener('click', () => {
    if (MAP) MAP.setFitView(null, false, [40, 40, 40, 40]);
  });

  // 先把数据放出来，地图慢一步不影响看状态
  await refreshState();
  refreshSlow();

  try {
    AMapNS = await loadMap(CFG.gaode_key, CFG.gaode_security_code);
    MAP = new AMapNS.Map('map', {zoom: 12, mapStyle: 'amap://styles/dark'});
    MAP.addControl(new AMapNS.Scale());
    MAP.addControl(new AMapNS.ToolBar({position: {top: '16px', right: '16px'}}));
    if (lastState) drawMarker(lastState);
    refreshTrack();
  } catch (e) {
    mapFailed(e.message === 'nokey'
      ? '服务端没读到高德 key（配置项 <code>web.gaode_key_file</code>）。'
      : e.message);
  }

  // state 5 秒一轮：服务端本身每 30 秒重算一次在线状态，
  // 但设备上报是随时的，5 秒能让「刚点了立刻定位」几乎马上看到结果。
  setInterval(refreshState, 5000);
  setInterval(refreshSlow, 15000);
}

main().catch(e => {
  document.body.innerHTML = '<div class="maperr">页面初始化失败：' + e.message
    + '<br><br><a href="/ui/login">重新登录</a></div>';
});
</script>
</body>
</html>
"""

