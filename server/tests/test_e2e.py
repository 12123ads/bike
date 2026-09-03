"""端到端：真的起 broker，用真的 MQTT 客户端连上去发报文。

这一层测的是**别的层测不到的东西**：ACL 是否真的拦得住、retain 是否真的
发给了新订阅者、下行队列是否真的在设备上线时冲刷。这些都只在真实的
broker 行为下才成立，单元测试全绿也可能这里全红。

只用明文 listener（127.0.0.1）—— TLS 握手在这里没有额外信息，而它会把测试
变慢并引入证书生成的时序依赖。TLS 配置的正确性由 test_certs 那几条覆盖。
"""

from __future__ import annotations

import asyncio
import base64
import json

import pytest
from amqtt.client import MQTTClient

from ebike_server import certs, contract as ct
from ebike_server.config import DeviceConfig, MqttConfig, ServerConfig
from ebike_server.service import Service

PORT = 21883  # 挑一个不常用的，避免和真实 broker 撞
#: 合法的 §6.2 密钥：base64 的恰好 32 字节（契约层强校验，见审计 M3）。
KEY32 = base64.b64encode(bytes(range(32))).decode()


@pytest.fixture
async def running(tmp_path):
    """起一个只监听明文本地端口的服务端。"""
    passwd = tmp_path / "passwd"
    dev_pw = certs.make_password(passwd, "bike01")
    ha_pw = certs.make_password(passwd, "ha")

    cfg = ServerConfig(
        db_path=str(tmp_path / "t.db"),
        api_token="testtoken",
        mqtt=MqttConfig(
            plain_bind=f"127.0.0.1:{PORT}",
            tls_bind="",                      # 测试里不开 TLS
            password_file=str(passwd),
        ),
        devices=[DeviceConfig(id="bike01", report_interval=900)],
    )
    svc = Service(cfg)
    await svc.start()
    try:
        yield svc, cfg, dev_pw, ha_pw
    finally:
        await svc.stop()


async def connect_as(user: str, pw: str, client_id: str | None = None) -> MQTTClient:
    c = MQTTClient(client_id=client_id or user,
                   config={"auto_reconnect": False, "keep_alive": 5})
    await c.connect(f"mqtt://{user}:{pw}@127.0.0.1:{PORT}/")
    return c


async def deliver(c: MQTTClient, timeout: float = 3.0):
    """收一条消息，超时返回 None。

    `MQTTClient.deliver_message` 超时是**抛 TimeoutError** 而不是返回 None
    （amqtt/client.py:412），而这些测试里「什么都没收到」恰恰是被断言的正常结果，
    所以必须在这里转成 None。
    """
    try:
        return await c.deliver_message(timeout_duration=timeout)
    except asyncio.TimeoutError:
        return None


async def wait_for(fn, timeout=3.0):
    """轮询等条件成立。broker 的投递是异步的，直接断言会偶发失败。"""
    deadline = asyncio.get_running_loop().time() + timeout
    while asyncio.get_running_loop().time() < deadline:
        if result := await fn():
            return result
        await asyncio.sleep(0.05)
    return None


# --- 认证 -------------------------------------------------------------------


async def test_wrong_password_rejected(running):
    _, _, _, _ = running
    c = MQTTClient(client_id="bike01", config={"auto_reconnect": False})
    with pytest.raises(Exception):
        await c.connect(f"mqtt://bike01:wrongpassword@127.0.0.1:{PORT}/")


async def test_anonymous_rejected(running):
    c = MQTTClient(client_id="anon", config={"auto_reconnect": False})
    with pytest.raises(Exception):
        await c.connect(f"mqtt://127.0.0.1:{PORT}/")


# --- 上行落库 ---------------------------------------------------------------


async def test_loc_ingested_and_stored(running):
    svc, _, dev_pw, _ = running
    c = await connect_as("bike01", dev_pw)
    await c.publish(ct.topic("bike01", ct.UP_LOC),
                    ct.dumps({"t": 1788105895, "q": 1, "s": "g",
                              "la": 31.230416, "lo": 121.473701, "a": 8.0}),
                    qos=1)
    rows = await wait_for(lambda: svc.store.track("bike01", 0, 2**31, 10, 0))
    assert rows and rows[0]["lat"] == 31.230416
    await c.disconnect()


async def test_malformed_payload_dropped_not_fatal(running):
    """畸形报文是固件调试期的常态，不能弄死服务。"""
    svc, _, dev_pw, _ = running
    c = await connect_as("bike01", dev_pw)
    await c.publish(ct.topic("bike01", ct.UP_LOC), b"not json at all", qos=1)
    await asyncio.sleep(0.3)
    # 服务还活着，后面的好报文照样收
    await c.publish(ct.topic("bike01", ct.UP_LOC),
                    ct.dumps({"t": 1, "q": 9, "s": "g", "la": 31.2, "lo": 121.4}),
                    qos=1)
    rows = await wait_for(lambda: svc.store.track("bike01", 0, 2**31, 10, 0))
    assert rows and len(rows) == 1
    await c.disconnect()


async def test_batch_ingest_dedups(running):
    svc, _, dev_pw, _ = running
    c = await connect_as("bike01", dev_pw)
    pts = [{"t": 1000 + i, "q": i, "s": "g", "la": 31.2, "lo": 121.4}
           for i in range(5)]
    await c.publish(ct.topic("bike01", ct.UP_LOC), ct.dumps(pts), qos=1)
    await wait_for(lambda: svc.store.track("bike01", 0, 2**31, 10, 0))
    # 重发同一批，条数不应增加
    await c.publish(ct.topic("bike01", ct.UP_LOC), ct.dumps(pts), qos=1)
    await asyncio.sleep(0.4)
    rows = await svc.store.track("bike01", 0, 2**31, 20, 0)
    assert len(rows) == 5
    await c.disconnect()


# --- ACL --------------------------------------------------------------------


async def test_device_cannot_publish_state(running):
    """设备被撬开后不能伪造「一切正常」覆盖服务端的判断（契约 §3）。"""
    svc, _, dev_pw, ha_pw = running
    ha = await connect_as("ha", ha_pw)
    await ha.subscribe([(ct.sub_all_state(), 1)])
    # 先把启动时 retain 的那条读掉
    await deliver(ha, 2)

    dev = await connect_as("bike01", dev_pw)
    forged = ct.dumps({"t": 1, "on": True, "mo": "parked", "la": 0, "lo": 0})
    await dev.publish(ct.topic("bike01", ct.STATE), forged, qos=1)

    got = await deliver(ha, 1.5)
    assert got is None, "设备伪造的 state 被广播出去了"
    await dev.disconnect()
    await ha.disconnect()


async def test_ha_cannot_publish_commands(running):
    """HA 账号只能订阅 state，不能发下行（契约 §3）。"""
    svc, _, dev_pw, ha_pw = running
    dev = await connect_as("bike01", dev_pw)
    await dev.subscribe([(f"{ct.PREFIX}/bike01/dn/#", 1)])

    ha = await connect_as("ha", ha_pw)
    await ha.publish(ct.topic("bike01", ct.DN_CMD),
                     ct.dumps({"id": "x-1", "c": "unlock"}), qos=1)

    got = await deliver(dev, 1.5)
    assert got is None, "HA 发的指令到达了设备"
    await dev.disconnect()
    await ha.disconnect()


async def test_ha_cannot_subscribe_raw_uplink(running):
    """HA 看不到原始报文 —— DESIGN.md §9.4 的「跟 HA 解耦」。"""
    svc, _, dev_pw, ha_pw = running
    ha = await connect_as("ha", ha_pw)
    await ha.subscribe([(f"{ct.PREFIX}/bike01/up/#", 1)])

    dev = await connect_as("bike01", dev_pw)
    await dev.publish(ct.topic("bike01", ct.UP_LOC),
                      ct.dumps({"t": 1, "q": 1, "s": "g", "la": 31.2, "lo": 121.4}),
                      qos=1)
    got = await deliver(ha, 1.5)
    assert got is None, "HA 订到了原始上行"
    await dev.disconnect()
    await ha.disconnect()


async def test_device_cannot_write_under_another_id(running):
    """**用自己的合法凭据往别的设备 id 下灌报文必须被丢掉。**

    这条针对的是 `Service.ingest` 里那层自己做的身份校验。amqtt 的
    MESSAGE_RECEIVED 在 ACL 之前触发（`amqtt/broker.py:753`），所以 broker 的
    publish ACL 拦不住这条路径上的落库 —— 校验必须在 ingest 里做，
    而且**不能挂在「dev 已配置」的条件上**：那样未配置的 id 反而成了后门。
    """
    svc, _, dev_pw, _ = running
    dev = await connect_as("bike01", dev_pw)

    # bike99 没配置；bike01 的凭据不该能在它名下写东西
    await dev.publish(f"{ct.PREFIX}/bike99/up/loc",
                      ct.dumps({"t": 1, "q": 1, "s": "g",
                                "la": 31.2, "lo": 121.4}), qos=1)
    await asyncio.sleep(0.4)
    assert await svc.store.track("bike99", 0, 2**31, 10, 0) == []

    # 自己名下照样写得进去 —— 别把正常路径一起挡了
    await dev.publish(ct.topic("bike01", ct.UP_LOC),
                      ct.dumps({"t": 1, "q": 1, "s": "g",
                                "la": 31.2, "lo": 121.4}), qos=1)
    rows = await wait_for(lambda: svc.store.track("bike01", 0, 2**31, 10, 0))
    assert rows and len(rows) == 1
    await dev.disconnect()


# --- state retain -----------------------------------------------------------


async def test_state_published_to_ha(running):
    svc, _, dev_pw, ha_pw = running
    ha = await connect_as("ha", ha_pw)
    await ha.subscribe([(ct.sub_all_state(), 1)])
    await deliver(ha, 2)   # 启动时的 retain

    dev = await connect_as("bike01", dev_pw)
    await dev.publish(ct.topic("bike01", ct.UP_LOC),
                      ct.dumps({"t": 1, "q": 1, "s": "g",
                                "la": 31.230416, "lo": 121.473701, "a": 8.0}),
                      qos=1)
    msg = await deliver(ha, 3)
    assert msg is not None
    state = json.loads(msg.data)
    assert state["on"] is True
    assert state["la"] == 31.230416
    assert "gla" in state, "缺 GCJ-02 字段"
    await dev.disconnect()
    await ha.disconnect()


async def test_retain_delivers_to_late_subscriber(running):
    """「重启 HA 立即有位置」这条验收（DESIGN.md §9.4）。"""
    svc, _, dev_pw, ha_pw = running
    dev = await connect_as("bike01", dev_pw)
    await dev.publish(ct.topic("bike01", ct.UP_LOC),
                      ct.dumps({"t": 1, "q": 1, "s": "g",
                                "la": 31.230416, "lo": 121.473701}),
                      qos=1)
    await wait_for(lambda: svc.store.last_loc("bike01"))
    await asyncio.sleep(0.3)
    await dev.disconnect()

    # 现在才连上的订阅者应该立刻拿到 state
    ha = await connect_as("ha", ha_pw, client_id="ha-late")
    await ha.subscribe([(ct.sub_all_state(), 1)])
    msg = await deliver(ha, 3)
    assert msg is not None, "retain 没有发给后连上的订阅者"
    assert json.loads(msg.data)["la"] == 31.230416
    await ha.disconnect()


# --- 下行队列（契约 §4.1） ---------------------------------------------------


async def test_downlink_queued_while_offline_then_flushed(running):
    """省电档下设备不在线时下发，等它上线才送到 —— 不靠 retain。"""
    svc, _, dev_pw, _ = running
    dn_id = await svc.enqueue_cmd("bike01", "locate", {"to": 60})
    assert len(await svc.store.pending_downlinks("bike01")) == 1

    dev = await connect_as("bike01", dev_pw)
    await dev.subscribe([(f"{ct.PREFIX}/bike01/dn/#", 1)])
    # 订阅之后触发一次冲刷（真实设备会发 up/hello）
    await dev.publish(ct.topic("bike01", ct.UP_HELLO),
                      ct.dumps({"t": 1, "q": 1, "fw": "0.1.0", "kid": 0}), qos=1)

    msg = await deliver(dev, 3)
    assert msg is not None, "下行没送到"
    body = json.loads(msg.data)
    assert body["c"] == "locate" and body["id"] == dn_id

    # 回 ack，队列应该清空
    await dev.publish(ct.topic("bike01", ct.UP_ACK),
                      ct.dumps({"t": 1, "q": 2, "id": dn_id, "ok": 1}), qos=1)
    drained = await wait_for(lambda: _is_empty(svc, "bike01"), timeout=3)
    assert drained is True, "ack 之后队列没清空"
    await dev.disconnect()


async def _is_empty(svc, dev):
    """wait_for 用真值判断，所以队列非空时必须返回 falsy 而不是 False 之外的东西。"""
    rows = await svc.store.pending_downlinks(dev)
    return True if not rows else None


async def test_secret_never_retained(running):
    """契约 §6.2：密钥不能留在 broker 的 retain 表里。"""
    svc, _, dev_pw, _ = running
    await svc.enqueue_secret("bike01", "set", uid=1, kid=8, key_b64=KEY32)
    dev = await connect_as("bike01", dev_pw)
    await dev.subscribe([(f"{ct.PREFIX}/bike01/dn/#", 1)])
    await dev.publish(ct.topic("bike01", ct.UP_HELLO),
                      ct.dumps({"t": 1, "q": 1, "kid": 0}), qos=1)
    msg = await deliver(dev, 3)
    assert msg is not None and json.loads(msg.data)["k"] == KEY32

    # broker 的 retain 表里不能有这个 topic
    assert svc.broker is not None
    retained = svc.broker.retained_messages
    assert ct.topic("bike01", ct.DN_SECRET) not in retained
    await dev.disconnect()


# --- amqtt 的一个行为，必须知道 ----------------------------------------------


async def test_retained_state_leaks_to_device_on_connect(running):
    """⚠ 这条测试**断言一个缺陷存在**，不是断言正确行为。

    amqtt 0.12.0 在客户端连上时会遍历**全局**订阅过滤器列表，把匹配的 retain
    消息推给这个新连上的会话，而不管它自己订了什么：

        for topic in self._subscriptions:                    # broker.py:600
            await self._publish_retained_messages_for_subscription(
                (topic, QOS_0), client_session)

    而 `_publish_retained_messages_for_subscription`（broker.py:1134）**完全不调
    `_topic_filtering`** —— 订阅 ACL 和接收 ACL 在这条路径上都不生效。

    所以只要 HA 订过 `ebike/v1/+/state`，之后任何通过认证的客户端一连上都会收到
    那条 retain 的 state，包括设备自己。

    **为什么当前不构成实际危害**：
    - 唯一 retain 的 topic 是 `state`（契约 §4.1 决定下行一律不 retain），
      而 state 里的位置就是设备自己刚报上来的。
    - 单车场景只有一个设备账号，没有「A 车看到 B 车位置」的问题。

    **什么情况下会变成危害**：加第二辆车之后，`ebike/v1/+/state` 匹配所有车，
    bike02 连上就会收到 bike01 的位置。**多车前必须解决**，办法是要么不让
    HA 用通配符订阅（改成每车一条精确订阅），要么打补丁/换 broker。

    契约 §4.1 那个「下行绝不 retain」的决定在这里额外挡住了最坏的情况：
    如果密钥下发用了 retain，这条路径会把密钥发给任何连上来的客户端。
    """
    svc, _, dev_pw, ha_pw = running

    # HA 先用通配符订阅，让那个 filter 进 broker 的全局表
    ha = await connect_as("ha", ha_pw)
    await ha.subscribe([(ct.sub_all_state(), 1)])
    await deliver(ha, 2)
    await ha.disconnect()

    # 设备只订 dn/#，但连上之后会收到 state
    dev = await connect_as("bike01", dev_pw, client_id="bike01-probe")
    await dev.subscribe([(f"{ct.PREFIX}/bike01/dn/#", 1)])
    got = await deliver(dev, 2)

    assert got is not None and got.topic.endswith("/state"), (
        "amqtt 的 retain 泄漏行为变了 —— 如果是被修好了，"
        "删掉这条测试并同步更新 docs/MQTT-CONTRACT.md §4.3")
    await dev.disconnect()


async def test_secret_is_not_retained_so_it_cannot_leak(running):
    """上一条那个泄漏路径推的是 retain 表里的东西。
    密钥不进 retain 表（契约 §4.1），所以泄漏不了 —— 这是那个决定的额外收益。"""
    svc, _, dev_pw, ha_pw = running
    await svc.enqueue_secret("bike01", "set", uid=1, kid=1, key_b64=KEY32)

    ha = await connect_as("ha", ha_pw)
    await ha.subscribe([(f"{ct.PREFIX}/#", 1)])   # ACL 会拒，但 filter 会进表
    await ha.disconnect()

    assert svc.broker is not None
    for topic in svc.broker.retained_messages:
        assert "/dn/" not in topic, f"下行进了 retain 表：{topic}"


# --- 2026-09-03 审计修复的回归测试 ------------------------------------------


async def test_ha_credentials_cannot_ingest_even_with_spoofed_client_id(running):
    """审计 H1：只读的 ha 账号把 client_id 报成 "svc"（或设备 id），
    也不能往 ingest 里灌数据。

    amqtt 的 MESSAGE_RECEIVED 在 ACL 之前触发（broker.py:753），
    所以 broker 的 publish ACL 拦不住落库 —— 身份必须由 ingest 自己
    按认证用户名判。旧实现按自报 client_id 判且白名单含 "svc"，
    实测 ha 凭据 + client_id="svc" 能把假遥测写进库。
    """
    svc, _, dev_pw, ha_pw = running
    # 连接层：ha 凭据 + client_id="svc" 能通过认证（amqtt 不管 client_id）
    c = await connect_as("ha", ha_pw, client_id="svc")
    # 灌一条假遥测 —— 修复后必须被 ingest 的身份校验拒掉
    await c.publish(ct.topic("bike01", ct.UP_TELE),
                    ct.dumps({"t": 1, "q": 777, "v": 66.6}), qos=1)
    await asyncio.sleep(0.4)
    import aiosqlite
    db = await aiosqlite.connect(svc.cfg.db_path)
    try:
        cur = await db.execute("SELECT q FROM tele WHERE dev='bike01'")
        rows = await cur.fetchall()
        assert rows == [], f"ha 凭据经 client_id=svc 注入了遥测: {rows}"
    finally:
        await db.close()
    await c.disconnect()


async def test_device_cannot_ingest_under_another_configured_device(running):
    """审计 H1 的对照组：身份跟**用户名**走，client_id 无关。

    bike01 的合法凭据把 client_id 报成 "bike99"（随便报），发**自己**的
    topic —— 必须正常落库（不能因为 client_id 对不上就拦掉正常设备；
    amqtt 不管 client_id 与 username 的关系）。身份冒充的拒绝路径由
    test_ha_credentials_cannot_ingest_even_with_spoofed_client_id 覆盖
    （ha 用户名 ≠ 设备 id → 拒）。
    """
    svc, _, dev_pw, _ = running
    c = await connect_as("bike01", dev_pw, client_id="bike99")
    await c.publish(ct.topic("bike01", ct.UP_LOC),
                    ct.dumps({"t": 1, "q": 555, "s": "g",
                              "la": 31.2, "lo": 121.4}), qos=1)
    await asyncio.sleep(0.4)
    # track() 的 SELECT 列表没有 q，直接查库
    import aiosqlite
    db = await aiosqlite.connect(svc.cfg.db_path)
    try:
        cur = await db.execute(
            "SELECT q FROM loc WHERE dev='bike01' AND q=555")
        rows = await cur.fetchall()
        assert rows == [(555,)], \
            "合法设备换了个 client_id 就发不上自己的 topic —— 修过头了"
    finally:
        await db.close()
    await c.disconnect()


async def test_abnormal_disconnect_lwt_lands_in_db(running):
    """审计 M9：broker 代发的遗嘱必须落库（lwt=1）。

    amqtt 非优雅断连走 `_broadcast_message`，不触发 MESSAGE_RECEIVED；
    修复后 IngestPlugin 挂了 RETAINED_MESSAGE 事件（will_retain=1 的
    遗嘱会走 retain_message → 事件带 source_session），把它转进
    `ingest_lwt_will`。这里直接调 `ingest_lwt_will` 验证落库路径本身；
    完整的 broker 链路由下一条测试覆盖。
    """
    svc, _, dev_pw, _ = running
    # 先清掉：hello 路径会置 lwt=0
    await svc.store.set_dev_fields("bike01", lwt=0)
    await svc.ingest_lwt_will(ct.topic("bike01", ct.LWT), b'{"lwt":1}')
    ds = await svc.store.dev_state("bike01")
    assert ds["lwt"] == 1, f"遗嘱没落库: {ds}"
    # 垃圾 payload 也能容忍（保持 1，_on_lwt 的解析语义）
    await svc.ingest_lwt_will(ct.topic("bike01", ct.LWT), b'garbage')
    ds = await svc.store.dev_state("bike01")
    assert ds["lwt"] == 1
    # 非契约 topic 直接忽略
    await svc.ingest_lwt_will("wrong/topic", b'{"lwt":1}')
    # 未配置设备的 lwt topic 忽略
    await svc.ingest_lwt_will(f"{ct.PREFIX}/bike99/lwt", b'{"lwt":1}')


async def test_will_retained_event_reaches_ingest_via_plugin(running):
    """审计 M9 的 broker 侧：带 will（retain=1）的客户端非优雅断连后，
    RETAINED_MESSAGE 事件链路把 lwt=1 落库。

    amqtt 的 MQTTClient 原生支持 will 配置（client.py:614-619 读
    config["will"]）。连上后直接关底层 transport（不发 DISCONNECT），
    broker 判非优雅 → 广播遗嘱 + retain → RETAINED_MESSAGE 事件 →
    IngestPlugin.on_broker_retained_message → ingest_lwt_will。
    """
    svc, _, dev_pw, _ = running
    c = MQTTClient(client_id="bike01-will", config={
        "auto_reconnect": False, "keep_alive": 5,
        "will": {"retain": True, "qos": 1,
                 "topic": ct.topic("bike01", ct.LWT),
                 "message": '{"lwt":1}'},
    })
    await c.connect(f"mqtt://bike01:{dev_pw}@127.0.0.1:{PORT}/")
    await asyncio.sleep(0.3)
    # 非优雅断连：绕过 disconnect()，直接关 transport。
    # handler.writer 是 StreamWriterAdapter，里面的 _writer 才是真 StreamWriter。
    assert c._handler is not None and c._handler.writer is not None
    c._handler.writer._writer.transport.abort()

    async def lwt_is_set():
        ds = await svc.store.dev_state("bike01")
        return True if ds["lwt"] == 1 else None

    got = await wait_for(lwt_is_set, timeout=5)
    assert got is True, "非优雅断连后 lwt 没有置 1 —— 遗嘱链路断了"


async def test_ack_for_another_devices_downlink_rejected(running):
    """审计 M10：设备不能 ack 别的设备的下行（dn_id 是全局主键）。

    单车下最直接的触发面是身份冒充；这里直接构造跨设备场景：
    给 bike01 排一条下行，然后以 bike01 的身份 ack 一条属于「未来
    bike02」的 id —— 通过手工插库模拟。"""
    svc, _, dev_pw, _ = running
    # 手工插一条 dev='other' 的下行（绕过配置限制，模拟多车库）
    await svc.store.enqueue_downlink("s-999", "other", ct.DN_SECRET, b"{}")
    c = await connect_as("bike01", dev_pw)
    await c.publish(ct.topic("bike01", ct.UP_ACK),
                    ct.dumps({"t": 1, "q": 1, "id": "s-999", "ok": 1}), qos=1)
    await asyncio.sleep(0.4)
    row = await svc.store.downlink("s-999")
    assert row["acked"] == 0, "bike01 销账了 other 的下行 —— dn_id 归属没校验"
    await c.disconnect()


async def test_publish_state_calls_are_serialized(running):
    """审计 M11：`publish_state` 自己拿设备锁。

    「算 state → 比对旧值 → 发布 → 写回 state_json」必须整体原子，否则两个
    并发调用（`/state/{id}`、`/devices`、`_tick` 三条 HTTP/定时路径 + ingest）
    算出的 state 可以按**相反顺序**写进 retain 表 —— HA 上显示陈旧值。

    检测手段：只有 publish_state 会调 `retain_message`，在它里面人为拉长
    窗口并数重入次数。锁在时重入恒为 0。

    第二段用**不带锁的** `_publish_state_locked` 跑同样的并发 —— 它必须
    交错。否则这条测试就是个空壳（探针本身不灵敏，加锁与否都是 0）。
    """
    svc, _, _, _ = running
    assert svc.broker is not None
    inflight = 0
    overlaps = 0
    orig = svc.broker.retain_message

    async def spy(*a, **kw):
        nonlocal inflight, overlaps
        inflight += 1
        try:
            if inflight > 1:
                overlaps += 1
            await asyncio.sleep(0.05)   # 把交错窗口拉大
            return await orig(*a, **kw)
        finally:
            inflight -= 1

    svc.broker.retain_message = spy          # type: ignore[method-assign]
    try:
        await asyncio.gather(*(svc.publish_state("bike01", force=True)
                               for _ in range(4)))
        assert overlaps == 0, f"{overlaps} 次 publish_state 交错 —— 锁没生效"

        # 探针自检：绕过锁必须能观测到交错
        overlaps = 0
        await asyncio.gather(*(svc._publish_state_locked("bike01", force=True)
                               for _ in range(4)))
        assert overlaps > 0, \
            "绕过锁也没观测到交错 —— 这个探针测不出 M11，别信上面那条断言"
    finally:
        svc.broker.retain_message = orig     # type: ignore[method-assign]


async def test_concurrent_enqueue_preserves_downlink_order(running):
    """审计 M11：入队 + 冲刷在同一把锁里，下行按 id 顺序到达。

    `flush_downlinks` 的文档说顺序是硬要求（连续两次密钥轮换必须按序到达，
    否则设备停在旧密钥上）。两个并发 HTTP 请求各自「入队 → 冲刷」时，
    没有锁的话第二条可以先发出去。
    """
    svc, _, dev_pw, _ = running
    dev = await connect_as("bike01", dev_pw)
    await dev.subscribe([(f"{ct.PREFIX}/bike01/dn/#", 1)])

    ids = await asyncio.gather(*(svc.enqueue_cmd("bike01", "ping")
                                 for _ in range(3)))
    seen: list[str] = []
    while len(seen) < 3:
        msg = await deliver(dev, 3)
        if msg is None:
            break
        dn_id = json.loads(msg.data)["id"]
        if dn_id not in seen:
            seen.append(dn_id)
    await dev.disconnect()

    # 每次冲刷都重发全部未确认的，所以同一个 id 可能重复到达；
    # 要断言的是**首次出现的顺序**和入队顺序一致。
    assert seen == sorted(ids, key=lambda s: int(s.split("-")[1])), \
        f"下行乱序：入队 {ids}，到达 {seen}"


async def test_ingest_and_http_paths_do_not_deadlock(running):
    """审计 M11 的反向风险：`asyncio.Lock` 不可重入。

    ingest 持锁后必须走 `_locked` 变体；`on_device_online` 反过来不能自己
    包锁再调公开版。任一处搞错就是**永久死锁**（整个设备的上行全停）。
    这条测试把三条路径并发跑一遍，超时即失败。
    """
    svc, _, dev_pw, _ = running
    dev = await connect_as("bike01", dev_pw)

    async def exercise() -> None:
        await asyncio.gather(
            svc.ingest("bike01", ct.topic("bike01", ct.UP_HELLO),
                       ct.dumps({"t": 1, "q": 1, "kid": 0})),
            svc.ingest("bike01", ct.topic("bike01", ct.UP_TELE),
                       ct.dumps({"t": 2, "q": 2, "v": 48.1})),
            svc.ingest_lwt_will(ct.topic("bike01", ct.LWT), b'{"lwt":1}'),
            svc.on_device_online("bike01"),
            svc.publish_state("bike01"),
            svc.enqueue_cmd("bike01", "ping"),
            svc.flush_downlinks("bike01"),
        )

    await asyncio.wait_for(exercise(), timeout=10)
    await dev.disconnect()
