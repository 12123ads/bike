/**
 * 用假 DOM + 假 fetch 跑一遍前端的渲染逻辑。
 *
 * **这不是浏览器测试** —— 没有真 DOM、没有 CSS、没有地图。
 * 它能抓住的是渲染分支的错误，比如 `lk=null` 被显示成「未上锁」
 * （那会让人以为车没锁好而白跑一趟）。抓不住的是布局和地图交互。
 *
 * 由 tests/test_web_frontend.py 调用。
 * argv: <前端 JS 文件> <state fixture> <events fixture> <输出>
 */
const fs = require('fs');

const [, , jsPath, statePath, eventsPath, outPath] = process.argv;

let js = fs.readFileSync(jsPath, 'utf8');
// 去掉自动调用，改成手动驱动
js = js.replace(/main\(\)\.catch[\s\S]*$/, '');

const els = {};
function el(id) {
  if (!els[id]) {
    els[id] = {
      id, textContent: '', innerHTML: '', className: '', value: '',
      disabled: false, style: {}, dataset: {}, addEventListener() {},
    };
  }
  return els[id];
}

global.document = {
  getElementById: el,
  querySelectorAll: () => [],
  createElement: () => ({ set onload(f) {}, set onerror(f) {} }),
  head: { appendChild() {} },
  body: { innerHTML: '' },
};
global.location = { href: '' };
global.window = {};
global.setInterval = () => 0;
global.setTimeout = () => 0;
global.clearTimeout = () => {};
global.confirm = () => true;

const STATE = JSON.parse(fs.readFileSync(statePath, 'utf8'));
const EVENTS = JSON.parse(fs.readFileSync(eventsPath, 'utf8'));

global.fetch = async (path) => {
  let body = {};
  if (path === '/api/config') {
    body = {
      devices: [{ id: 'bike01', report_interval: 900 }],
      gaode_key: '', gaode_security_code: '',
      allow_remote_unlock: false, commands: [],
    };
  } else if (path.startsWith('/api/state')) {
    body = STATE;
  } else if (path.startsWith('/api/events')) {
    body = { events: EVENTS };
  } else if (path.startsWith('/api/pending')) {
    body = { pending: [] };
  } else if (path.startsWith('/api/track')) {
    body = { points: [], count: 0 };
  }
  return { ok: true, status: 200, json: async () => body };
};

const api = new Function(
  js + '\n return {main, renderState, renderEvents, renderPending};'
)();

(async () => {
  await api.main();
  // main() 里的 refreshSlow() 是不 await 的，等一轮微任务让它跑完
  await new Promise((r) => setImmediate(r));
  api.renderEvents(EVENTS);

  const ids = ['online', 'moving', 'lock', 'pos', 'acc', 'src', 'volt',
               'pct', 'gf', 'ls', 'devlabel', 'cmdnote', 'trackinfo'];
  const out = {};
  for (const id of ids) {
    out[id] = { text: els[id] ? els[id].textContent : null,
                cls: els[id] ? els[id].className : null };
  }
  out.events_html = els.events ? els.events.innerHTML : null;
  out.unlock_disabled = els['btn-unlock'] ? els['btn-unlock'].disabled : null;
  fs.writeFileSync(outPath, JSON.stringify(out, null, 1));
})();
