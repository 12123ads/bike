"""HTTP API 测试。

用 FastAPI 的 TestClient 直接打 app，不起 uvicorn —— 省掉端口和启动时序，
测的东西（鉴权、参数校验、远程开锁开关）一样覆盖到。
"""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from ebike_server import contract as ct
from ebike_server.api import build_app
from ebike_server.config import DeviceConfig, MqttConfig, ServerConfig
from ebike_server.service import Service

TOKEN = "testtoken"
AUTH = {"Authorization": f"Bearer {TOKEN}"}


@pytest.fixture
async def svc_and_client(tmp_path):
    """只起 Service 的 store 部分，不起 broker —— API 层不需要它。

    broker=None 时 flush_downlinks/publish_state 都会短路（service.py 里判了），
    所以入队仍然会落库，正好是这里要测的。
    """
    cfg = ServerConfig(
        db_path=str(tmp_path / "t.db"),
        api_token=TOKEN,
        mqtt=MqttConfig(plain_bind="", tls_bind=""),
        devices=[DeviceConfig(id="bike01")],
    )
    svc = Service(cfg)
    await svc.store.open()
    app = build_app(svc, cfg)
    with TestClient(app) as client:
        yield svc, cfg, client
    await svc.store.close()


# --- 鉴权 -------------------------------------------------------------------


def test_health_needs_no_auth(svc_and_client):
    _, _, c = svc_and_client
    r = c.get("/health")
    assert r.status_code == 200 and r.json()["ok"] is True


def test_everything_else_needs_auth(svc_and_client):
    _, _, c = svc_and_client
    for path in ("/devices", "/state/bike01", "/track?dev=bike01", "/events?dev=bike01"):
        assert c.get(path).status_code == 401, path


def test_wrong_token_rejected(svc_and_client):
    _, _, c = svc_and_client
    r = c.get("/devices", headers={"Authorization": "Bearer wrong"})
    assert r.status_code == 401


def test_unknown_device_404(svc_and_client):
    _, _, c = svc_and_client
    assert c.get("/state/nope", headers=AUTH).status_code == 404


# --- 读接口 -----------------------------------------------------------------


async def test_track_returns_points(svc_and_client):
    svc, _, c = svc_and_client
    await svc.store.add_loc("bike01", [
        {"q": 1, "t_dev": 1, "src": "g", "lat": 31.2, "lon": 121.4,
         "acc": 8.0, "speed": None, "heading": None, "sats": 9},
    ], 5000)
    r = c.get("/track?dev=bike01&since=0&until=9999", headers=AUTH)
    assert r.status_code == 200
    body = r.json()
    assert body["count"] == 1 and body["points"][0]["lat"] == 31.2


async def test_track_pagination(svc_and_client):
    svc, _, c = svc_and_client
    pts = [{"q": i, "t_dev": i, "src": "g", "lat": 31.2, "lon": 121.4,
            "acc": None, "speed": None, "heading": None, "sats": None}
           for i in range(10)]
    await svc.store.add_loc("bike01", pts, 5000)
    r1 = c.get("/track?dev=bike01&until=9999&limit=4&offset=0", headers=AUTH).json()
    r2 = c.get("/track?dev=bike01&until=9999&limit=4&offset=4", headers=AUTH).json()
    assert r1["count"] == 4 and r2["count"] == 4
    assert r1["points"] != r2["points"]


def test_track_limit_is_bounded(svc_and_client):
    _, _, c = svc_and_client
    # limit 上限 5000，超过应该 422 而不是拖垮数据库
    assert c.get("/track?dev=bike01&limit=99999", headers=AUTH).status_code == 422


async def test_state_endpoint(svc_and_client):
    svc, _, c = svc_and_client
    r = c.get("/state/bike01", headers=AUTH)
    assert r.status_code == 200
    # 从没上报过 → 离线
    assert r.json()["on"] is False


# --- 下行 -------------------------------------------------------------------


def test_cmd_queued(svc_and_client):
    _, _, c = svc_and_client
    r = c.post("/cmd/bike01/locate", headers=AUTH, json={"to": 60})
    assert r.status_code == 200
    assert r.json()["queued"].startswith("c-")


def test_unknown_cmd_rejected(svc_and_client):
    _, _, c = svc_and_client
    assert c.post("/cmd/bike01/selfdestruct", headers=AUTH).status_code == 400


def test_remote_unlock_disabled_by_default(svc_and_client):
    """契约 §6.1：远程开锁绕过 NFC 挑战应答，默认必须关着。"""
    _, cfg, c = svc_and_client
    assert cfg.allow_remote_unlock is False
    r = c.post("/cmd/bike01/unlock", headers=AUTH)
    assert r.status_code == 403
    assert "挑战应答" in r.json()["detail"]


def test_remote_unlock_works_when_enabled(svc_and_client):
    _, cfg, c = svc_and_client
    cfg.allow_remote_unlock = True
    assert c.post("/cmd/bike01/unlock", headers=AUTH).status_code == 200


def test_secret_response_does_not_echo_key(svc_and_client):
    """契约 §6.2：明文密钥不能回显，也不能进日志。"""
    _, _, c = svc_and_client
    r = c.post("/secret/bike01", headers=AUTH,
               json={"op": "set", "uid": 1, "kid": 8, "key_b64": "SUPERSECRET"})
    assert r.status_code == 200
    assert "SUPERSECRET" not in r.text


def test_secret_bad_op_rejected(svc_and_client):
    _, _, c = svc_and_client
    assert c.post("/secret/bike01", headers=AUTH,
                  json={"op": "nonsense"}).status_code == 400


def test_secret_missing_fields_rejected(svc_and_client):
    _, _, c = svc_and_client
    r = c.post("/secret/bike01", headers=AUTH, json={"op": "set", "uid": 1})
    assert r.status_code == 400


def test_pending_hides_payload(svc_and_client):
    """/pending 是诊断接口，但队列里可能有明文密钥。"""
    _, _, c = svc_and_client
    c.post("/secret/bike01", headers=AUTH,
           json={"op": "set", "uid": 1, "kid": 8, "key_b64": "SUPERSECRET"})
    r = c.get("/pending?dev=bike01", headers=AUTH)
    assert r.status_code == 200
    assert "SUPERSECRET" not in r.text
    rows = r.json()["pending"]
    assert len(rows) == 1 and rows[0]["suffix"] == ct.DN_SECRET
    assert rows[0]["bytes"] > 0


# --- 配置文件生成（docker 入口依赖它） --------------------------------------


def test_write_default_creates_readable_config(tmp_path):
    from ebike_server import config as cfgmod
    p = cfgmod.write_default(tmp_path / "config.json")
    assert p.exists()
    # 里面有 api_token，不能让同宿主其他用户读到
    assert (p.stat().st_mode & 0o777) == 0o600
    loaded = cfgmod.load(p)
    assert loaded.api_token and len(loaded.api_token) >= 20


def test_write_default_does_not_clobber(tmp_path):
    """init 可以重复跑 —— 第二次不能把 api_token 换掉，
    否则 HA 和脚本里配好的 token 会突然失效。"""
    from ebike_server import config as cfgmod
    p = cfgmod.write_default(tmp_path / "config.json")
    first = cfgmod.load(p).api_token
    cfgmod.write_default(p)
    assert cfgmod.load(p).api_token == first


def test_docker_defaults_bind_all_and_disable_plaintext(tmp_path):
    """容器里绑 127.0.0.1 等于谁都连不上；而开明文 MQTT 等于把开锁凭据
    交给同一 docker 网络里的任何容器。"""
    from ebike_server import config as cfgmod
    p = cfgmod.write_default(tmp_path / "config.json", docker=True)
    cfg = cfgmod.load(p)
    assert cfg.http_bind.startswith("0.0.0.0:")
    assert cfg.mqtt.tls_bind.startswith("0.0.0.0:")
    assert cfg.mqtt.plain_bind == "", "容器配置不该开明文 MQTT 口"


def test_missing_config_raises_not_silently_defaults(tmp_path):
    """路径打错却用默认配置跑起来，比直接失败危险得多。"""
    from ebike_server import config as cfgmod
    with pytest.raises(FileNotFoundError):
        cfgmod.load(tmp_path / "nope.json")


def test_unknown_config_key_rejected(tmp_path):
    """拼错的配置项要报错，不能静默忽略 —— 那会让人以为设置生效了。"""
    import json as _json
    from ebike_server import config as cfgmod
    p = tmp_path / "bad.json"
    p.write_text(_json.dumps({"allow_remote_unlok": True}), encoding="utf-8")
    with pytest.raises(ValueError, match="未知项"):
        cfgmod.load(p)
