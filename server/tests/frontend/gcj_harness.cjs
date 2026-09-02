/**
 * 把前端的坐标转换函数抠出来，和服务端 geo.py 逐点比对。
 *
 * 为什么必须比对：轨迹点是**前端**转的（/track 返回 WGS84），
 * 实时点是**服务端**转的（契约 §7 的 gla/glo）。两边算法不一致的话，
 * 轨迹会和当前位置错开几百米 —— 而两者各自看起来都正常。
 *
 * 由 tests/test_web_frontend.py 调用，直接 `node` 跑也行。
 * 输入 /tmp 里的点集，输出 GCJ-02 结果，由 Python 侧断言。
 */
const fs = require('fs');

const [, , htmlJsPath, inPath, outPath] = process.argv;
const js = fs.readFileSync(htmlJsPath, 'utf8');

// 只取坐标转换那一段。用 new Function 而不是 eval：
// eval 在模块作用域里声明的函数拿不到，new Function 可以显式 return。
const block = js.slice(js.indexOf('const GCJ_A'), js.indexOf('function loadMap'));
if (!block) {
  console.error('在前端 JS 里找不到坐标转换段');
  process.exit(2);
}
const wgs84ToGcj02 = new Function(block + '\n return wgs84ToGcj02;')();

const pts = JSON.parse(fs.readFileSync(inPath, 'utf8'));
// 前端返回的是 [经度, 纬度]（高德的顺序）
fs.writeFileSync(outPath, JSON.stringify(pts.map(([lat, lon]) => wgs84ToGcj02(lat, lon))));
