"""网页界面测试。

重点在**鉴权边界**：网页能下发指令（含远程开锁），所以「没登录能不能拿到数据」
和「cookie 是不是 HttpOnly」比页面长什么样重要得多。
"""

from __future__ import annotations

import re

import pytest
from fastapi.testclient import TestClient

from ebike_server.api import build_app
from ebike_server.config import DeviceConfig, MqttConfig, ServerConfig, WebConfig
from ebike_server.service import Service
from ebike_server.websession import COOKIE_NAME, LoginThrottle, SessionStore

TOKEN = "web-test-token"


@pytest.fixture
async def web(tmp_path):
    keyfile = tmp_path / "gaode.key"
    keyfile.write_text("0123456789abcdef0123456789abcdef\n", encoding="utf-8")

    cfg = ServerConfig(
        db_path=str(tmp_path / "t.db"),
        api_token=TOKEN,
        mqtt=MqttConfig(plain_bind="", tls_bind=""),
        web=WebConfig(gaode_key_file=str(keyfile)),
        devices=[DeviceConfig(id="bike01")],
    )
    svc = Service(cfg)
    await svc.store.open()
    app = build_app(svc, cfg)
    with TestClient(app) as client:
        yield svc, cfg, client
    await svc.store.close()


def login(client) -> None:
    r = client.post("/ui/login", json={"token": TOKEN})
    assert r.status_code == 200


# --- 鉴权 -------------------------------------------------------------------


def test_root_shows_login_when_anonymous(web):
    _, _, c = web
    r = c.get("/")
    assert r.status_code == 200
    # 不做 302 —— 302 之后地址栏变了，刷新会停在登录页而不是回地图
    assert "登录" in r.text
    assert "AMapLoader" not in r.text, "未登录却给出了地图页"


def test_all_web_api_needs_session(web):
    _, _, c = web
    for path in ("/api/config", "/api/state/bike01", "/api/track?dev=bike01",
                 "/api/events?dev=bike01", "/api/pending?dev=bike01"):
        assert c.get(path).status_code == 401, path
    assert c.post("/api/cmd/bike01/locate").status_code == 401


def test_wrong_token_rejected(web):
    _, _, c = web
    r = c.post("/ui/login", json={"token": "nope"})
    assert r.status_code == 401
    assert COOKIE_NAME not in c.cookies


def test_login_then_index_has_map(web):
    _, _, c = web
    login(c)
    r = c.get("/")
    assert "AMapLoader" in r.text
    assert "id=\"map\"" in r.text


def test_session_cookie_is_httponly_and_samesite(web):
    """HttpOnly：XSS 也偷不走会话。SameSite=strict：挡 CSRF。

    这两条是「网页不用 Bearer token」这个决定的全部意义 ——
    localStorage 里的 API token 会被任何一个 XSS 拿走且长期有效。
    """
    _, _, c = web
    r = c.post("/ui/login", json={"token": TOKEN})
    raw = r.headers.get("set-cookie", "")
    assert "httponly" in raw.lower(), raw
    assert "samesite=strict" in raw.lower(), raw
    # 默认部署是 http://127.0.0.1，加 Secure 会让 cookie 根本不回传
    assert "; secure" not in raw.lower(), raw


def test_cookie_secure_when_configured(tmp_path):
    """放到 HTTPS 反代后面时要能打开 Secure。"""
    cfg = ServerConfig(
        db_path=str(tmp_path / "t.db"), api_token=TOKEN,
        mqtt=MqttConfig(plain_bind="", tls_bind=""),
        web=WebConfig(cookie_secure=True, gaode_key_file=""),
        devices=[DeviceConfig(id="bike01")],
    )
    svc = Service(cfg)
    app = build_app(svc, cfg)
    with TestClient(app) as c:
        r = c.post("/ui/login", json={"token": TOKEN})
        assert "secure" in r.headers.get("set-cookie", "").lower()


def test_logout_invalidates_session(web):
    _, _, c = web
    login(c)
    assert c.get("/api/config").status_code == 200
    c.post("/ui/logout")
    assert c.get("/api/config").status_code == 401


def test_bearer_api_still_works_independently(web):
    """网页的会话和 API 的 Bearer 是两套，互不影响。"""
    _, _, c = web
    r = c.get("/state/bike01", headers={"Authorization": f"Bearer {TOKEN}"})
    assert r.status_code == 200
    # 反过来：会话 cookie 不能当 Bearer 用
    login(c)
    assert c.get("/state/bike01").status_code == 401


def test_malformed_login_body(web):
    _, _, c = web
    r = c.post("/ui/login", content=b"not json",
               headers={"Content-Type": "application/json"})
    assert r.status_code == 400


# --- 数据接口 ---------------------------------------------------------------


def test_config_exposes_key_only_after_login(web):
    """高德 JS key 天生要发给浏览器，但至少要求先登录。"""
    _, _, c = web
    assert c.get("/api/config").status_code == 401
    login(c)
    body = c.get("/api/config").json()
    assert re.fullmatch(r"[0-9a-f]{32}", body["gaode_key"])
    assert body["devices"][0]["id"] == "bike01"
    assert body["allow_remote_unlock"] is False


def test_config_without_keyfile_returns_empty(tmp_path):
    """key 读不到时前端要能退化成「只显示坐标」，不能 500。"""
    cfg = ServerConfig(
        db_path=str(tmp_path / "t.db"), api_token=TOKEN,
        mqtt=MqttConfig(plain_bind="", tls_bind=""),
        web=WebConfig(gaode_key_file=str(tmp_path / "missing.key")),
        devices=[DeviceConfig(id="bike01")],
    )
    svc = Service(cfg)
    app = build_app(svc, cfg)
    with TestClient(app) as c:
        c.post("/ui/login", json={"token": TOKEN})
        assert c.get("/api/config").json()["gaode_key"] == ""


async def test_track_and_events(web):
    svc, _, c = web
    login(c)
    await svc.store.add_loc("bike01", [{
        "q": 1, "t_dev": 1, "src": "g", "lat": 31.2, "lon": 121.4,
        "acc": 8.0, "speed": None, "heading": None, "sats": 9,
    }], 5000)
    await svc.store.add_event("bike01", {"q": 2, "t_dev": 1, "kind": "motion",
                                         "detail": {"mg": 180}}, 5000)
    t = c.get("/api/track?dev=bike01&since=0").json()
    assert t["count"] == 1 and t["points"][0]["lat"] == 31.2
    e = c.get("/api/events?dev=bike01").json()
    assert e["events"][0]["kind"] == "motion"


def test_unknown_device_404(web):
    _, _, c = web
    login(c)
    assert c.get("/api/state/nope").status_code == 404
    assert c.get("/api/track?dev=nope").status_code == 404


def test_track_limit_is_clamped_not_rejected(web):
    """网页传的 limit 是自己拼的，夹住比 422 好 —— 用户看到的是空白页而不是报错。"""
    _, _, c = web
    login(c)
    assert c.get("/api/track?dev=bike01&limit=999999").status_code == 200


def test_pending_hides_secret_payload(web):
    _, _, c = web
    login(c)
    c.post("/secret/bike01", headers={"Authorization": f"Bearer {TOKEN}"},
           json={"op": "set", "uid": 1, "kid": 1, "key_b64": "SUPERSECRET"})
    r = c.get("/api/pending?dev=bike01")
    assert r.status_code == 200
    assert "SUPERSECRET" not in r.text


# --- 指令 -------------------------------------------------------------------


def test_web_cmd_queues(web):
    _, _, c = web
    login(c)
    r = c.post("/api/cmd/bike01/locate")
    assert r.status_code == 200 and r.json()["queued"].startswith("c-")


def test_web_unlock_403_by_default(web):
    """契约 §6.1：网页和 API 走同一套检查，不能有一条路绕过去。"""
    _, cfg, c = web
    login(c)
    assert cfg.allow_remote_unlock is False
    r = c.post("/api/cmd/bike01/unlock")
    assert r.status_code == 403
    assert "挑战应答" in r.json()["detail"]


def test_web_unlock_works_when_enabled(web):
    _, cfg, c = web
    login(c)
    cfg.allow_remote_unlock = True
    assert c.post("/api/cmd/bike01/unlock").status_code == 200


def test_web_unknown_cmd_rejected(web):
    _, _, c = web
    login(c)
    assert c.post("/api/cmd/bike01/selfdestruct").status_code == 400


# --- 会话与限速 -------------------------------------------------------------


def test_session_store_expiry():
    s = SessionStore(ttl=0)
    tok, _ = s.create()
    assert s.valid(tok) is False        # ttl=0 立刻过期
    assert len(s) == 0


def test_session_store_basics():
    s = SessionStore(ttl=60)
    tok, age = s.create()
    assert age == 60 and s.valid(tok)
    assert not s.valid("garbage")
    assert not s.valid(None)
    s.revoke(tok)
    assert not s.valid(tok)


def test_login_throttle():
    """没有限速的登录接口是个 CPU 放大器：每次请求都做一次 compare_digest。"""
    t = LoginThrottle(max_attempts=3, window=300)
    assert not t.blocked("1.2.3.4")
    for _ in range(3):
        t.record_failure("1.2.3.4")
    assert t.blocked("1.2.3.4")
    assert not t.blocked("5.6.7.8")     # 按 IP 隔离
    t.reset("1.2.3.4")
    assert not t.blocked("1.2.3.4")


def test_throttle_returns_429(tmp_path):
    cfg = ServerConfig(
        db_path=str(tmp_path / "t.db"), api_token=TOKEN,
        mqtt=MqttConfig(plain_bind="", tls_bind=""),
        web=WebConfig(gaode_key_file=""),
        devices=[DeviceConfig(id="bike01")],
    )
    svc = Service(cfg)
    app = build_app(svc, cfg)
    with TestClient(app) as c:
        for _ in range(10):
            c.post("/ui/login", json={"token": "wrong"})
        r = c.post("/ui/login", json={"token": "wrong"})
        assert r.status_code == 429
        # 限速期间连**正确**的 token 也进不去 —— 否则限速可以被绕过
        assert c.post("/ui/login", json={"token": TOKEN}).status_code == 429


# --- 关掉网页 ---------------------------------------------------------------


def test_web_can_be_disabled(tmp_path):
    cfg = ServerConfig(
        db_path=str(tmp_path / "t.db"), api_token=TOKEN,
        mqtt=MqttConfig(plain_bind="", tls_bind=""),
        web=WebConfig(enabled=False),
        devices=[DeviceConfig(id="bike01")],
    )
    svc = Service(cfg)
    app = build_app(svc, cfg)
    with TestClient(app) as c:
        assert c.get("/").status_code == 404
        assert c.get("/api/config").status_code == 404
        # API 不受影响
        assert c.get("/health").status_code == 200


# --- 前端资源 ---------------------------------------------------------------


def test_frontend_uses_gcj_fields_not_wgs():
    """契约 §7：高德底图是 GCJ-02，实时点必须用服务端算好的 gla/glo。"""
    from ebike_server.web_assets import INDEX_HTML
    assert "s.gla" in INDEX_HTML and "s.glo" in INDEX_HTML
    # 不能回落到 WGS84 —— 静默回落会让车偏几百米而界面看不出异常
    assert "s.gla === undefined || s.gla === null) return" in INDEX_HTML


def test_frontend_converts_track_points():
    """/track 返回的是 WGS84（落库原值），画在高德底图上必须先转。

    不转的后果：轨迹整体偏几百米，而实时点是准的，两者对不上。
    """
    from ebike_server.web_assets import INDEX_HTML
    assert "wgs84ToGcj02" in INDEX_HTML
    assert "0.00669342162296594" in INDEX_HTML, "GCJ 偏心率常数和服务端不一致"


def test_frontend_lock_unknown_is_not_unlocked():
    """`lk` 为 null 是常态（没接反馈开关）。显示成「未上锁」会让人白跑一趟。"""
    from ebike_server.web_assets import INDEX_HTML
    assert "s.lk === null || s.lk === undefined ? null : s.lk" in INDEX_HTML


def test_docker_config_points_key_at_mounted_secret(tmp_path):
    """容器里 /root/gaode.key 不存在（那是宿主路径），
    compose 把它只读挂到 /run/secrets/gaode.key。"""
    from ebike_server import config as cfgmod
    p = cfgmod.write_default(tmp_path / "config.json", docker=True)
    cfg = cfgmod.load(p)
    assert cfg.web.gaode_key_file == "/run/secrets/gaode.key"
    assert cfg.web.enabled is True


def test_nested_web_section_hydrates(tmp_path):
    """`from __future__ import annotations` 让 dataclass 的 type 变成字符串，
    嵌套段靠名字映射填充。漏了映射的话 web 会是一个 dict 然后在使用处炸掉。"""
    import json as _json
    from ebike_server import config as cfgmod
    p = tmp_path / "c.json"
    p.write_text(_json.dumps({
        "web": {"enabled": False, "session_ttl": 60, "cookie_secure": True},
        "mqtt": {"plain_bind": ""},
    }), encoding="utf-8")
    cfg = cfgmod.load(p)
    assert isinstance(cfg.web, cfgmod.WebConfig)
    assert cfg.web.enabled is False and cfg.web.session_ttl == 60
    assert isinstance(cfg.mqtt, cfgmod.MqttConfig)


def test_unknown_web_key_rejected(tmp_path):
    import json as _json
    from ebike_server import config as cfgmod
    p = tmp_path / "c.json"
    p.write_text(_json.dumps({"web": {"gaode_ky": "x"}}), encoding="utf-8")
    with pytest.raises(ValueError, match="未知项"):
        cfgmod.load(p)


def test_unreadable_key_warns_not_silent(tmp_path, caplog):
    """读不到 key 必须留下日志。

    之前是静默返回空串 —— 结果容器里因为权限打不开文件（宿主 /root/gaode.key
    是 0600 root，容器以 uid 1000 跑），网页上只是地图空着，日志里一个字都没有。
    那种问题只能靠猜。
    """
    import logging
    from ebike_server.config import WebConfig
    p = tmp_path / "k"
    p.write_text("0" * 32, encoding="utf-8")
    p.chmod(0o000)
    w = WebConfig(gaode_key_file=str(p))
    with caplog.at_level(logging.WARNING, logger="ebike.config"):
        key = w.resolve_gaode_key()
    p.chmod(0o600)   # 让 tmp_path 能被清理
    if key:
        # root 跑测试时权限位挡不住，那这条断言没有意义
        pytest.skip("以 root 运行，chmod 000 挡不住读取")
    assert any("权限" in r.message or "PermissionError" in r.message
               for r in caplog.records), caplog.text


def test_missing_key_file_logs_info(tmp_path, caplog):
    import logging
    from ebike_server.config import WebConfig
    w = WebConfig(gaode_key_file=str(tmp_path / "nope"))
    with caplog.at_level(logging.INFO, logger="ebike.config"):
        assert w.resolve_gaode_key() == ""
    assert any("不存在" in r.message for r in caplog.records)


def test_env_var_overrides_key(tmp_path, monkeypatch):
    """容器里读不到挂载文件时的退路（compose 里注释掉的那个变量）。"""
    import json as _json
    from ebike_server import config as cfgmod
    p = tmp_path / "c.json"
    p.write_text(_json.dumps({
        "web": {"gaode_key_file": str(tmp_path / "absent")},
    }), encoding="utf-8")
    monkeypatch.setenv("EBIKE_GAODE_KEY", "f" * 32)
    cfg = cfgmod.load(p)
    assert cfg.web.resolve_gaode_key() == "f" * 32


def test_config_key_takes_precedence_over_empty_file(tmp_path):
    """文件存在但是空的 → 退到配置里的 gaode_key。"""
    from ebike_server.config import WebConfig
    p = tmp_path / "k"
    p.write_text("\n", encoding="utf-8")
    w = WebConfig(gaode_key_file=str(p), gaode_key="a" * 32)
    assert w.resolve_gaode_key() == "a" * 32
