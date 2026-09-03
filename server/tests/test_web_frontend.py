"""前端 JS 的测试。

浏览器自动化在这台机器上不可用（`browser_no_host`），所以这里用 `node`：

1. **语法检查** —— `node --check`。内联 JS 拼错了只会在浏览器控制台里报，
   服务端一切正常，从外面完全看不出来。
2. **坐标转换和服务端逐点比对** —— 轨迹点是前端转的、实时点是服务端转的，
   两边不一致会让轨迹和当前位置错开几百米，而各自看起来都正常。
3. **渲染分支** —— 用假 DOM 跑一遍，检查 `lk=null` 显示成「未知」而不是「未上锁」。

**测不到的**：布局、CSS、地图交互、高德 SDK 的真实行为。
那些要真浏览器，等有 browser host 时补。
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from ebike_server.geo import haversine_m, wgs84_to_gcj02
from ebike_server.web_assets import INDEX_HTML, LOGIN_HTML

HARNESS = Path(__file__).parent / "frontend"

pytestmark = pytest.mark.skipif(
    shutil.which("node") is None, reason="没有 node，跳过前端测试"
)


def extract_js(html: str) -> str:
    blocks = re.findall(r"<script>(.*?)</script>", html, re.S)
    assert len(blocks) == 1, f"期望 1 个 script 块，实际 {len(blocks)}"
    return blocks[0]


@pytest.fixture(scope="module")
def index_js(tmp_path_factory) -> Path:
    p = tmp_path_factory.mktemp("fe") / "index.js"
    p.write_text(extract_js(INDEX_HTML), encoding="utf-8")
    return p


@pytest.fixture(scope="module")
def login_js(tmp_path_factory) -> Path:
    p = tmp_path_factory.mktemp("fe") / "login.js"
    p.write_text(extract_js(LOGIN_HTML), encoding="utf-8")
    return p


# --- 语法 -------------------------------------------------------------------


def test_index_js_syntax(index_js):
    r = subprocess.run(["node", "--check", str(index_js)],
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, r.stderr


def test_login_js_syntax(login_js):
    r = subprocess.run(["node", "--check", str(login_js)],
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, r.stderr


# --- 坐标转换 ---------------------------------------------------------------

#: 覆盖南北东西 + 两个境外点
COORD_CASES = [
    (31.230416, 121.473701),   # 上海
    (39.908722, 116.397499),   # 北京
    (23.129163, 113.264435),   # 广州
    (45.803775, 126.534967),   # 哈尔滨
    (43.825592, 87.616848),    # 乌鲁木齐
    (51.507400, -0.127800),    # 伦敦（境外）
    (35.689500, 139.691700),   # 东京（境外）
]


def test_frontend_gcj_matches_server(index_js, tmp_path):
    """前端和服务端的 WGS84→GCJ-02 必须逐点一致（到厘米级）。

    不一致的表现：轨迹整体偏移，而实时位置是准的 —— 看起来像「轨迹坏了」。
    """
    in_p = tmp_path / "in.json"
    out_p = tmp_path / "out.json"
    in_p.write_text(json.dumps(COORD_CASES), encoding="utf-8")

    r = subprocess.run(
        ["node", str(HARNESS / "gcj_harness.cjs"), str(index_js),
         str(in_p), str(out_p)],
        capture_output=True, text=True, timeout=60,
    )
    assert r.returncode == 0, r.stderr

    js_out = json.loads(out_p.read_text(encoding="utf-8"))
    worst = 0.0
    for (lat, lon), (jlon, jlat) in zip(COORD_CASES, js_out):
        plat, plon = wgs84_to_gcj02(lat, lon)
        worst = max(worst, haversine_m(plat, plon, jlat, jlon))
    assert worst < 0.01, f"前后端坐标转换差 {worst:.4f} 米"


def test_frontend_returns_amap_order(index_js, tmp_path):
    """高德要 [经度, 纬度]。顺序搞反的话点会跑到地球另一边。"""
    in_p = tmp_path / "in.json"
    out_p = tmp_path / "out.json"
    in_p.write_text(json.dumps([[31.230416, 121.473701]]), encoding="utf-8")
    subprocess.run(["node", str(HARNESS / "gcj_harness.cjs"), str(index_js),
                    str(in_p), str(out_p)], check=True, timeout=60)
    lon, lat = json.loads(out_p.read_text(encoding="utf-8"))[0]
    assert 120 < lon < 122, f"第一个元素应该是经度，得到 {lon}"
    assert 30 < lat < 32, f"第二个元素应该是纬度，得到 {lat}"


# --- 渲染 -------------------------------------------------------------------


STATE_FIXTURE = {
    "t": 1788280563, "on": True, "mo": "parked", "ls": 1788280500,
    "lwt": 0, "lk": None,
    "la": 31.230416, "lo": 121.473701,
    "gla": 31.228474, "glo": 121.478224,
    "a": 8.0, "s": "g", "gf": "in", "v": 51.5, "pct": 68,
}

EVENTS_FIXTURE = [
    {"t_srv": 1788280500, "kind": "motion", "detail": {"mg": 260}},
    {"t_srv": 1788280400, "kind": "unlock_deny", "detail": {"uid": 1}},
    {"t_srv": 1788280300, "kind": "lock_state", "detail": {"locked": False}},
    {"t_srv": 1788280200, "kind": "lowbatt", "detail": {"lv": 1, "v": 46.2}},
]

#: 恶意 detail：契约层现在会拒掉这种值（`_check_event_detail`），
#: 但渲染层是第二道墙 —— 上游放松校验时它不能变成存储型 XSS（审计 M4）。
XSS_EVENTS_FIXTURE = [
    {"t_srv": 1788280500, "kind": "motion",
     "detail": {"mg": "<img src=x onerror=alert(1)>"}},
    {"t_srv": 1788280400, "kind": "<script>bad()</script>",
     "detail": {"uid": "\"><svg onload=alert(2)>"}},
]


@pytest.fixture(scope="module")
def rendered(index_js, tmp_path_factory) -> dict:
    d = tmp_path_factory.mktemp("render")
    (d / "state.json").write_text(json.dumps(STATE_FIXTURE), encoding="utf-8")
    (d / "events.json").write_text(json.dumps(EVENTS_FIXTURE), encoding="utf-8")
    out = d / "out.json"
    r = subprocess.run(
        ["node", str(HARNESS / "render_harness.cjs"), str(index_js),
         str(d / "state.json"), str(d / "events.json"), str(out)],
        capture_output=True, text=True, timeout=60,
    )
    assert r.returncode == 0, r.stderr
    return json.loads(out.read_text(encoding="utf-8"))


def test_lock_unknown_is_not_shown_as_unlocked(rendered):
    """**最重要的一条。** `lk=null` 是常态（没接位置反馈开关，契约 §7）。
    显示成「未上锁」会让人以为车没锁好而白跑一趟。"""
    assert rendered["lock"]["text"] == "未知"
    assert "unk" in rendered["lock"]["cls"]
    assert "未上锁" not in rendered["lock"]["text"]


def test_online_and_moving_badges(rendered):
    assert rendered["online"]["text"] == "在线"
    assert "on" in rendered["online"]["cls"].split()
    assert rendered["moving"]["text"] == "静止"


def test_position_and_accuracy_shown(rendered):
    assert "31.230416" in rendered["pos"]["text"]
    assert rendered["acc"]["text"] == "8 米"
    assert rendered["src"]["text"] == "卫星定位"


def test_voltage_and_percent(rendered):
    assert rendered["volt"]["text"] == "51.5 V"
    assert rendered["pct"]["text"] == "68 %"


def test_geofence_shown(rendered):
    assert rendered["gf"]["text"] == "围栏内"


def test_last_seen_is_relative_plus_absolute(rendered):
    """相对时间（「7 分钟前」）比绝对时间好读，但绝对时间也要有 ——
    排查问题时需要对时间戳。"""
    text = rendered["ls"]["text"]
    assert "前" in text and "2026" in text


def test_events_rendered_with_labels(rendered):
    html = rendered["events_html"] or ""
    assert "检测到移动" in html and "260 mg" in html
    assert "开锁被拒" in html
    assert "电量低" in html and "46.2" in html
    # 危险事件要有区分的 class，好上颜色
    assert "ev-unlock_deny" in html and "ev-lowbatt" in html


@pytest.fixture(scope="module")
def rendered_xss(index_js, tmp_path_factory) -> dict:
    d = tmp_path_factory.mktemp("render_xss")
    (d / "state.json").write_text(json.dumps(STATE_FIXTURE), encoding="utf-8")
    (d / "events.json").write_text(json.dumps(XSS_EVENTS_FIXTURE),
                                   encoding="utf-8")
    out = d / "out.json"
    r = subprocess.run(
        ["node", str(HARNESS / "render_harness.cjs"), str(index_js),
         str(d / "state.json"), str(d / "events.json"), str(out)],
        capture_output=True, text=True, timeout=60,
    )
    assert r.returncode == 0, r.stderr
    return json.loads(out.read_text(encoding="utf-8"))


def test_event_detail_is_escaped_before_innerhtml(rendered_xss):
    """审计 M4：事件面板用 innerHTML 拼动态文本，必须转义。

    契约层现在也会拒掉这种 detail（`_check_event_detail`），但渲染层是
    独立的第二道墙：上游放松一次校验就不该等于存储型 XSS —— 那条链是
    「MQTT 凭据 → 伪造 event → 浏览器执行 → 用会话 cookie 调
    /api/cmd/.../unlock」。

    判据是**结构**而不是关键字：注入内容里的 `onerror=alert(1)` 作为纯
    文本留在页面上无害（`<` 已被转义成 `&lt;`，形不成标签）。所以断言
    「HTML 里的每一个裸 `<` 都是我们自己生成的 li/span 标签」。
    """
    html = rendered_xss["events_html"] or ""
    raw_tags = re.findall(r"<(/?[a-zA-Z][^\s>]*)", html)
    allowed = {"li", "/li", "span", "/span"}
    assert set(raw_tags) <= allowed, \
        f"渲染出了我们没生成的标签（转义漏了）：{set(raw_tags) - allowed}"
    # 注入内容必须以转义形式存在（证明内容没被丢掉，只是失效了）
    assert "&lt;img" in html and "&lt;svg" in html and "&lt;script" in html
    # class 属性里也不能逃出引号
    assert 'class="ev-&lt;script&gt;bad()&lt;/script&gt;"' in html


def test_unlock_button_disabled_when_server_says_no(rendered):
    """契约 §6.1：服务端说不允许，按钮就该是禁用的 ——
    让用户点一下再吃 403 是糟糕的体验。"""
    assert rendered["unlock_disabled"] is True
    assert "挑战应答" in rendered["cmdnote"]["text"]


# --- 页面结构 ---------------------------------------------------------------


def test_no_inline_event_handlers():
    """内联 `onclick=` 会被严格的 CSP 挡掉，也更难审查。"""
    assert not re.search(r"\son\w+\s*=\s*[\"']", INDEX_HTML)
    assert not re.search(r"\son\w+\s*=\s*[\"']", LOGIN_HTML)


def test_login_page_has_no_map_or_data():
    """未登录时给的是登录页，不能泄露任何设备信息。"""
    assert "AMapLoader" not in LOGIN_HTML
    assert "/api/state" not in LOGIN_HTML
    assert "gaode" not in LOGIN_HTML.lower()


def test_only_external_resource_is_amap_loader():
    """页面只该引用高德的 loader，不引第三方 CDN ——
    多一个外部依赖就多一个「它挂了页面就白屏」的可能。"""
    urls = set(re.findall(r"https?://[a-zA-Z0-9./-]+", INDEX_HTML + LOGIN_HTML))
    assert urls == {"https://webapi.amap.com/loader.js"}, urls


def test_token_never_stored_in_browser():
    """API token 不进 localStorage/sessionStorage ——
    那里的东西任何一个 XSS 都能拿走，而它能下发指令。"""
    for html in (INDEX_HTML, LOGIN_HTML):
        assert "localStorage" not in html
        assert "sessionStorage" not in html
