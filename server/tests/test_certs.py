"""证书与 TLS listener 测试。

这一层要挡住的是「TLS 配好了但设备连不上」——那种错误在设备侧只表现为一个
`ERROR`，隔着 9600 baud 的 AT 链路极难诊断（DESIGN.md §8.7）。
所以在这里用真的 TLS 客户端连一次真的 TLS listener。
"""

from __future__ import annotations

import pytest
from amqtt.client import MQTTClient
from cryptography import x509

from ebike_server import certs
from ebike_server.broker import build_broker_config
from ebike_server.config import DeviceConfig, MqttConfig, ServerConfig
from ebike_server.service import Service

TLS_PORT = 21884


@pytest.fixture
def prepared(tmp_path):
    """生成 CA + 服务端证书 + 口令，返回一份配好 TLS 的 ServerConfig。"""
    certs_dir = tmp_path / "certs"
    certs.ensure_ca(certs_dir)
    # CN 必须是设备实际连接用的名字 —— 这里是 127.0.0.1
    certs.ensure_server_cert(certs_dir, "127.0.0.1")
    passwd = tmp_path / "passwd"
    dev_pw = certs.make_password(passwd, "bike01")

    cfg = ServerConfig(
        db_path=str(tmp_path / "t.db"),
        api_token="t",
        mqtt=MqttConfig(
            plain_bind="",                       # 只开 TLS
            tls_bind=f"127.0.0.1:{TLS_PORT}",
            certfile=str(certs_dir / "server.crt"),
            keyfile=str(certs_dir / "server.key"),
            cafile=str(certs_dir / "ca.crt"),
            password_file=str(passwd),
        ),
        devices=[DeviceConfig(id="bike01")],
    )
    return cfg, certs_dir, dev_pw


def test_ca_is_reused_not_regenerated(tmp_path):
    """重复跑 init 不能换掉 CA —— 换了之后已经烧进模组 FS 的 ca.crt 就废了。"""
    d = tmp_path / "certs"
    k1, c1 = certs.ensure_ca(d)
    b1 = c1.read_bytes()
    k2, c2 = certs.ensure_ca(d)
    assert c2.read_bytes() == b1


def test_server_cert_cn_matches_hostname(tmp_path):
    """设备侧 AT+SSLCFG="hostname" 校验这个值，填错就握手失败。"""
    d = tmp_path / "certs"
    certs.ensure_server_cert(d, "mqtt.example.com")
    crt = x509.load_pem_x509_certificate((d / "server.crt").read_bytes())
    cn = crt.subject.get_attributes_for_oid(x509.NameOID.COMMON_NAME)[0].value
    assert cn == "mqtt.example.com"


def test_private_keys_are_not_world_readable(tmp_path):
    d = tmp_path / "certs"
    certs.ensure_ca(d)
    certs.ensure_server_cert(d, "127.0.0.1")
    for name in ("ca.key", "server.key"):
        mode = (d / name).stat().st_mode & 0o777
        assert mode == 0o600, f"{name} 权限是 {oct(mode)}"


def test_password_file_is_argon2_and_private(tmp_path):
    """amqtt 的 FileAuthPlugin 只认 argon2，不是 mosquitto_passwd 格式。"""
    p = tmp_path / "passwd"
    certs.make_password(p, "bike01")
    line = p.read_text(encoding="utf-8").strip()
    assert line.startswith("bike01:$argon2")
    assert (p.stat().st_mode & 0o777) == 0o600


def test_rewriting_password_replaces_not_duplicates(tmp_path):
    p = tmp_path / "passwd"
    certs.make_password(p, "bike01")
    certs.make_password(p, "ha")
    pw3 = certs.make_password(p, "bike01")     # 轮换
    lines = [ln for ln in p.read_text(encoding="utf-8").splitlines() if ln]
    assert len(lines) == 2
    assert sum(1 for ln in lines if ln.startswith("bike01:")) == 1


def test_device_cert_san_matches_amqtt_expectation(tmp_path):
    """UserAuthCertPlugin 用正则匹配 spiffe://<domain>/device/<id>，
    格式差一个字符就认不出来。"""
    d = tmp_path / "certs"
    certs.ensure_ca(d)
    certs.make_device_cert(d, "bike01", "ebike.local")
    crt = x509.load_pem_x509_certificate((d / "bike01.crt").read_bytes())
    san = crt.extensions.get_extension_for_class(x509.SubjectAlternativeName)
    uris = san.value.get_values_for_type(x509.UniformResourceIdentifier)
    assert uris == ["spiffe://ebike.local/device/bike01"]


def test_missing_cert_file_fails_loudly(prepared):
    """amqtt 的 ListenerConfig.__post_init__ 会检查文件存在，
    所以配错路径是启动失败而不是「起来了但没有 TLS」。"""
    cfg, certs_dir, _ = prepared
    cfg.mqtt.certfile = str(certs_dir / "nope.crt")
    with pytest.raises(Exception):
        build_broker_config(cfg)
        # 实际抛在 Broker(config) 里，这里先确认配置能构造出来
        from amqtt.broker import Broker
        Broker(build_broker_config(cfg))


async def test_device_connects_over_tls(prepared):
    """端到端：真的 TLS 握手 + 口令认证 + 发一条位置。"""
    cfg, certs_dir, dev_pw = prepared
    svc = Service(cfg)
    await svc.start()
    try:
        from ebike_server import contract as ct
        c = MQTTClient(client_id="bike01",
                       config={"auto_reconnect": False, "keep_alive": 5,
                               "check_hostname": True})
        await c.connect(f"mqtts://bike01:{dev_pw}@127.0.0.1:{TLS_PORT}/",
                        cafile=str(certs_dir / "ca.crt"))
        await c.publish(ct.topic("bike01", ct.UP_LOC),
                        ct.dumps({"t": 1, "q": 1, "s": "g",
                                  "la": 31.230416, "lo": 121.473701}), qos=1)
        import asyncio
        for _ in range(60):
            if await svc.store.last_loc("bike01"):
                break
            await asyncio.sleep(0.05)
        row = await svc.store.last_loc("bike01")
        assert row is not None and row["lat"] == 31.230416
        await c.disconnect()
    finally:
        await svc.stop()


async def test_wrong_ca_is_rejected(prepared, tmp_path):
    """客户端拿着别的 CA 连不上 —— 否则中间人可以冒充服务端收走位置。"""
    cfg, _, dev_pw = prepared
    other = tmp_path / "other"
    certs.ensure_ca(other)

    svc = Service(cfg)
    await svc.start()
    try:
        c = MQTTClient(client_id="bike01",
                       config={"auto_reconnect": False})
        with pytest.raises(Exception):
            await c.connect(f"mqtts://bike01:{dev_pw}@127.0.0.1:{TLS_PORT}/",
                            cafile=str(other / "ca.crt"))
    finally:
        await svc.stop()


# --- mode="cert"（mTLS） -----------------------------------------------------

CERT_PORT = 21886


@pytest.fixture
def cert_mode(tmp_path):
    """mode="cert" 的完整环境：CA + 服务端证书 + 设备证书。"""
    certs_dir = tmp_path / "certs"
    certs.ensure_ca(certs_dir)
    certs.ensure_server_cert(certs_dir, "127.0.0.1")
    dev_key, dev_crt = certs.make_device_cert(certs_dir, "bike01", "ebike.local")

    cfg = ServerConfig(
        db_path=str(tmp_path / "t.db"),
        api_token="t",
        mqtt=MqttConfig(
            plain_bind="",
            tls_bind=f"127.0.0.1:{CERT_PORT}",
            certfile=str(certs_dir / "server.crt"),
            keyfile=str(certs_dir / "server.key"),
            cafile=str(certs_dir / "ca.crt"),
            mode="cert",
            cert_uri_domain="ebike.local",
            password_file=str(tmp_path / "passwd"),
        ),
        devices=[DeviceConfig(id="bike01")],
    )
    return cfg, certs_dir, str(dev_crt), str(dev_key)


async def test_cert_mode_device_can_publish_and_subscribe(cert_mode):
    """mode="cert" 必须端到端可用，不只是「CONNECT 通过」。

    **这条防的是一个真实缺陷**：上游的 `UserAuthCertPlugin` 只比对证书 SAN
    与 client_id，**从不设 `session.username`**（`amqtt/contrib/cert.py:57-83`），
    而 ACL 插件按 username 查表，None 时退化成 `"anonymous"` → 空列表 → 一律拒。
    表现是 CONNECT 成功但 **SUBSCRIBE 返回 0x80**，设备永远收不到
    `dn/cmd` 和 `dn/secret`（远程指令和密钥轮换全废）。
    `DeviceCertAuthPlugin` 补上 username 之后才真正可用。
    """
    import asyncio

    from ebike_server import contract as ct

    cfg, certs_dir, dev_crt, dev_key = cert_mode
    svc = Service(cfg)
    await svc.start()
    try:
        c = MQTTClient(client_id="bike01", config={
            "auto_reconnect": False, "keep_alive": 5, "check_hostname": True,
            "connection": {"cafile": str(certs_dir / "ca.crt"),
                           "certfile": dev_crt, "keyfile": dev_key},
        })
        await c.connect(f"mqtts://127.0.0.1:{CERT_PORT}/",
                        cafile=str(certs_dir / "ca.crt"))

        granted = await c.subscribe([(f"{ct.PREFIX}/bike01/dn/#", 1)])
        assert granted == [1], f"订阅被拒（{granted}）—— ACL 拿不到身份"

        # 下行真的能到设备
        dn_id = await svc.enqueue_cmd("bike01", "locate")
        msg = await asyncio.wait_for(c.deliver_message(), timeout=5)
        assert dn_id in msg.data.decode()

        # 上行也能落库
        await c.publish(ct.topic("bike01", ct.UP_LOC),
                        ct.dumps({"t": 1, "q": 1, "s": "g",
                                  "la": 31.230416, "lo": 121.473701}), qos=1)
        for _ in range(60):
            if await svc.store.last_loc("bike01"):
                break
            await asyncio.sleep(0.05)
        assert await svc.store.last_loc("bike01") is not None
        await c.disconnect()
    finally:
        await svc.stop()


async def test_cert_mode_acl_still_scoped_to_own_device(cert_mode):
    """补上 username 不能顺手把 ACL 放开：订别的车仍然要被拒。"""
    from ebike_server import contract as ct

    cfg, certs_dir, dev_crt, dev_key = cert_mode
    svc = Service(cfg)
    await svc.start()
    try:
        c = MQTTClient(client_id="bike01", config={
            "auto_reconnect": False, "keep_alive": 5, "check_hostname": True,
            "connection": {"cafile": str(certs_dir / "ca.crt"),
                           "certfile": dev_crt, "keyfile": dev_key},
        })
        await c.connect(f"mqtts://127.0.0.1:{CERT_PORT}/",
                        cafile=str(certs_dir / "ca.crt"))
        refused = await c.subscribe([(f"{ct.PREFIX}/bike99/dn/#", 1)])
        assert refused == [128], f"订到了别的车的下行（{refused}）"
        await c.disconnect()
    finally:
        await svc.stop()


def test_publish_acl_assertion_covers_every_device():
    """原来的断言是「publish_acl 非空」，而 build_acl 里塞了 svc 键 ——
    那条 raise 永远不会触发。改成逐设备检查之后才真的是一道保险。"""
    cfg = ServerConfig(devices=[DeviceConfig(id="bike01")])
    cfg.mqtt.plain_bind = "127.0.0.1:1883"
    cfg.mqtt.tls_bind = ""

    import ebike_server.broker as bmod
    real = bmod.build_acl

    def drop_device(c):
        pub, sub = real(c)
        pub.pop("bike01", None)          # 模拟漏配
        return pub, sub

    bmod.build_acl = drop_device
    try:
        with pytest.raises(RuntimeError, match="publish ACL"):
            build_broker_config(cfg)
    finally:
        bmod.build_acl = real


def test_no_svc_account_in_acl():
    """审计 R7：ACL 里不能有 `svc` 账号。

    服务端自己发下行走 `internal_message_broadcast`（broker 身份、不过 ACL），
    从来不需要账号 —— `init` 也没为它建口令。但**口令文件是用户可编辑的**：
    留着这条 ACL，谁手加一行 `svc` 就拿到整棵 topic 树的读写权，
    包括伪造 `state`（正是「设备不能发 state」那条规则要防的）。
    """
    from ebike_server import contract as ct
    from ebike_server.broker import build_acl

    pub, sub = build_acl(ServerConfig(devices=[DeviceConfig(id="bike01")]))
    assert "svc" not in pub, "publish ACL 里又出现了 svc 全权限账号"
    assert "svc" not in sub, "subscribe ACL 里又出现了 svc 全权限账号"
    # 该有的还在
    assert set(pub) == {"bike01"}, f"publish ACL 的账号集变了：{sorted(pub)}"
    assert set(sub) == {"ha", "bike01"}, f"subscribe ACL 的账号集变了：{sorted(sub)}"
    # 没有任何条目是整棵树
    for user, topics in {**pub, **sub}.items():
        for t in topics:
            assert t != f"{ct.PREFIX}/#", f"{user} 拿到了整棵 topic 树：{t}"


def test_empty_publish_acl_is_refused_not_silently_open():
    """审计 R7 的连带风险：删掉 `svc` 之后「publish_acl 非空」不再恒真。

    amqtt 的 `publish_acl` **空字典 = 发布全放行**
    （`topic_checking.py:69-71` 的 hbmqtt 兼容分支）。设备列表为空时
    `build_acl` 真的会返回 `{}` —— 必须启动失败，不能静默全放行。
    """
    cfg = ServerConfig(devices=[])
    cfg.mqtt.plain_bind = "127.0.0.1:1883"
    cfg.mqtt.tls_bind = ""
    with pytest.raises(RuntimeError, match="publish ACL"):
        build_broker_config(cfg)


def test_every_listener_has_a_connection_cap():
    """审计 M12：8883 是公网口，amqtt 默认 `max_connections=-1`（无限），
    且它读 CONNECT 包**没有超时** —— 只连不发的 socket 会永久占一个任务 +
    一份 TLS 会话内存。上限是唯一兜底。

    必须**每个** listener 都显式写：`ListenerConfig.apply` 只在字段等于默认值
    时才从 default listener 继承，而 `max_connections` 的默认是 0（= 不限），
    所以「只给 tls 配」并不会让 plain 继承到限制。
    """
    cfg = ServerConfig(devices=[DeviceConfig(id="bike01")])
    cfg.mqtt.plain_bind = "127.0.0.1:1883"
    cfg.mqtt.tls_bind = "0.0.0.0:8883"
    conf = build_broker_config(cfg)

    assert len(conf["listeners"]) == 2
    for name, listener in conf["listeners"].items():
        cap = listener.get("max_connections")
        assert isinstance(cap, int) and cap > 0, \
            f"listener {name!r} 没有连接数上限：{listener}"


async def test_connection_cap_reaches_the_running_broker(tmp_path):
    """上一条测的是配置字典；这条确认 amqtt 真的把它读进了 `Server.semaphore`
    —— 配置键名拼错会静默退回无限制（dacite 对 listener 段不是 strict）。"""
    from ebike_server.config import MqttConfig

    passwd = tmp_path / "passwd"
    certs.make_password(passwd, "bike01")
    cfg = ServerConfig(
        db_path=str(tmp_path / "t.db"),
        mqtt=MqttConfig(plain_bind="127.0.0.1:21993", tls_bind="",
                        password_file=str(passwd), max_connections=4),
        devices=[DeviceConfig(id="bike01")],
    )
    svc = Service(cfg)
    await svc.start()
    try:
        assert svc.broker is not None
        servers = svc.broker._servers            # amqtt 内部；没有公开访问器
        assert servers, "broker 没起 listener"
        for name, server in servers.items():
            assert server.max_connections == 4, \
                f"listener {name!r} 的上限没传到 broker：{server.max_connections}"
            assert server.semaphore is not None, \
                f"listener {name!r} 没建 semaphore —— 等于不限制"
    finally:
        await svc.stop()
