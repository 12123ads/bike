"""Service：把 broker、落库、派生、下行队列串起来。

一个类持有全部运行时状态。刻意不做依赖注入框架 —— 组件只有五个，
显式传参比容器清楚。

下行发布走 `broker.internal_message_broadcast()`（`amqtt/broker.py:433`），
即服务端以「broker 自己」的身份发，不占一个 MQTT 客户端连接，
也就绕开了「服务端要不要给自己配账号」这个问题。
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from typing import Any

from amqtt.broker import Broker

from . import contract as ct
from .broker import IngestPlugin, build_broker_config
from .config import ServerConfig
from .derive import build_state, state_changed
from .store import Store

log = logging.getLogger("ebike.service")

_QOS1 = 1


class Service:
    def __init__(self, cfg: ServerConfig) -> None:
        self.cfg = cfg
        self.store = Store(cfg.db_path)
        self.broker: Broker | None = None
        #: 下行 id 的序号。**不在内存里递增** —— 见 Store.reserve_dn_seq：
        #: 计数器只活在进程里的话，重启后第一条命令必然撞 pending_downlink 主键。
        self._tasks: list[asyncio.Task[Any]] = []
        #: 每设备一把锁：ingest 是并发的（broker 为每条消息起 task），
        #: 而「算 state → 比对 → 发布」不是原子的，两条报文同时到会发出乱序的 state。
        self._locks: dict[str, asyncio.Lock] = {}

    def _lock(self, dev: str) -> asyncio.Lock:
        return self._locks.setdefault(dev, asyncio.Lock())

    # --- 生命周期 -------------------------------------------------------------

    async def start(self) -> None:
        await self.store.open()
        # 旧库升级：meta 表刚建出来，但 pending_downlink 里可能已经有 c-7 这种行。
        # 不抬高水位的话第一条命令就撞主键。
        await self.store.sync_dn_seq_to_existing()
        # 上报周期可能被 `interval` 指令改过（契约 §6.1），从库里恢复 ——
        # 否则离线阈值会退回配置文件的值，而设备用的是被改过的周期（契约 §4.2）。
        await self._restore_intervals()
        IngestPlugin.service = self
        self.broker = Broker(build_broker_config(self.cfg))
        await self.broker.start()
        log.info("broker 已启动: %s", list(self.broker.listeners_config))

        # 启动时把每台设备的 state 重新发一遍 retain。
        # 必要性：amqtt 的 retain 表是**纯内存的**（`_retained_messages` dict，
        # `broker.start()` 里还会 `.clear()`），进程重启后 retain 全丢。
        # 「重启 HA 立即有位置」那条验收（DESIGN.md §9.4）因此必须由我们自己恢复。
        for d in self.cfg.devices:
            await self.publish_state(d.id, force=True)

        self._tasks.append(asyncio.create_task(self._tick(), name="ebike-tick"))

    async def stop(self) -> None:
        for t in self._tasks:
            t.cancel()
        for t in self._tasks:
            try:
                await t
            except asyncio.CancelledError:
                pass
        self._tasks.clear()
        if self.broker is not None:
            await self.broker.shutdown()
            self.broker = None
        IngestPlugin.service = None
        await self.store.close()

    async def _tick(self) -> None:
        """周期任务：重算在线状态、清理过期轨迹。

        在线是**算出来的**（契约 §4.2），没有任何报文会告诉我们「设备离线了」，
        所以必须有人周期性地重算，否则 HA 上的车会永远停在最后一次上报的「在线」。
        """
        last_prune = 0.0
        while True:
            try:
                await asyncio.sleep(30)
                for d in self.cfg.devices:
                    await self.publish_state(d.id)
                now = time.time()
                if now - last_prune > 3600:
                    last_prune = now
                    n = await self.store.prune(self.cfg.track_retention_days)
                    n += await self.store.prune_downlinks(
                        self.cfg.downlink_retention_days)
                    if n:
                        log.info("清理过期数据 %d 行", n)
            except asyncio.CancelledError:
                raise
            except Exception:
                log.exception("周期任务出错")

    # --- 上行 ----------------------------------------------------------------

    async def ingest(self, username: str | None, topic: str, payload: bytes) -> None:
        """处理一条上行。**所有校验失败都只记日志不抛** —— 畸形报文是常态
        （固件调试期尤其），不能让它影响别的设备或弄死 broker 的 task。

        ``username`` 是 **broker 认证过的用户名**（password 模式 = 口令文件里的
        账号；cert 模式 = 证书 SAN 里的设备 id）。它才是身份 —— MQTT 的
        client_id 是 CONNECT 包里客户端自报的，与凭据无关，不能拿来判权。
        """
        try:
            parsed = ct.parse_topic(topic)
        except ct.ContractError:
            log.warning("丢弃：topic 不属于契约 %r (user=%s)", topic, username)
            return

        dev, suffix = parsed.device_id, parsed.suffix

        # ⚠ MESSAGE_RECEIVED 在 broker 的 ACL 检查**之前**触发
        # （amqtt/broker.py:753 的注释明说了），所以这里自己再判一次身份。
        # 判据是「认证用户名 == topic 里的设备 id」—— 设备账号的用户名就是
        # 设备 id（契约 §4）。`ha` 只读账号在这里被挡住，无论它把 client_id
        # 报成什么；2026-09-03 审计 H1 之前的实现按 client_id 判且白名单含
        # "svc"，等于允许任何持凭据者冒充任意设备。
        if username != dev:
            log.warning("丢弃：user %r 无权发 %s", username, topic)
            return

        # 未配置的设备一律丢。允许它们落库会让 /devices、/state 和 HA 看不到的
        # 数据堆在库里，且 build_state 拿不到 volt_curve/geofence。
        # （到这里 username == dev，所以等价于「这个账号没配设备」。）
        if self.cfg.device(dev) is None:
            log.warning("丢弃：%s 不是已配置的设备（topic=%s）", dev, topic)
            return

        if suffix not in ct.UP_SUFFIXES and suffix != ct.LWT:
            # dn/* 是我们自己发的，state 也是；回到这里说明配置有问题
            return

        async with self._lock(dev):
            t_srv = int(time.time())
            if suffix == ct.LWT:
                await self._on_lwt(dev, payload)
            else:
                try:
                    await self._on_up(dev, suffix, payload, t_srv)
                except ct.ContractError as e:
                    log.warning("丢弃：%s 不合契约: %s", topic, e)
                    return
            # 已持锁 —— 用 _locked 变体（`asyncio.Lock` 不可重入，审计 M11）
            await self._publish_state_locked(dev)

    async def _on_up(self, dev: str, suffix: str, payload: bytes, t_srv: int) -> None:
        parser = ct.UP_PARSERS[suffix]
        data = parser(payload)

        # 任何上行都刷新 last_seen —— 在线判定完全建立在它上面（契约 §4.2）
        await self.store.touch(dev, t_srv)

        if suffix == ct.UP_LOC:
            assert isinstance(data, list)
            added = await self.store.add_loc(dev, data, t_srv)
            if added != len(data):
                # 差额是被 UNIQUE(dev,q) 去掉的重复点，补发时正常
                log.info("%s 收 %d 点，新增 %d（其余为重复）", dev, len(data), added)
        elif suffix == ct.UP_TELE:
            assert isinstance(data, dict)
            await self.store.add_tele(dev, data, t_srv)
        elif suffix == ct.UP_EVENT:
            assert isinstance(data, dict)
            await self.store.add_event(dev, data, t_srv)
            log.info("%s 事件 %s %s", dev, data["kind"], data.get("detail") or "")
        elif suffix == ct.UP_HELLO:
            assert isinstance(data, dict)
            await self.store.set_dev_fields(
                dev, fw=data.get("fw"), kid=data.get("kid") or 0)
            # 设备主动说话了 = 它是活的，把「上次非优雅断连」的标记清掉
            await self.store.set_dev_fields(dev, lwt=0)
            log.info("%s hello fw=%s kid=%s rst=%s",
                     dev, data.get("fw"), data.get("kid"), data.get("rst"))
            # 已持锁（ingest 里的 `async with self._lock(dev)`）—— 必须用
            # _locked 变体，`asyncio.Lock` 不可重入，调公开版会死锁（审计 M11）
            await self._flush_downlinks_locked(dev)
        elif suffix == ct.UP_ACK:
            assert isinstance(data, dict)
            row = await self.store.downlink(data["dn_id"])
            # 审计 M10：dn_id 是全局主键，必须校验这条下行确实属于本设备。
            # 否则（多车时）bike02 一条 ack 能把 bike01 的密钥轮换标记为已送达；
            # 单车时配合身份冒充也能销账别人的下行。
            if row is not None and row["dev"] != dev:
                log.warning("丢弃：%s ack 了 %s 的下行 %s",
                            dev, row["dev"], data["dn_id"])
                return
            found = await self.store.mark_acked(
                data["dn_id"], data["ok"], data.get("err"))
            if not found:
                # 可能是重发的 ack，也可能是伪造的。两种都只需记录。
                log.info("%s ack 了一个不在队列里的 id=%s", dev, data["dn_id"])
            elif not data["ok"]:
                log.warning("%s 拒绝了下行 %s: %s",
                            dev, data["dn_id"], data.get("err"))
            else:
                await self._apply_acked_cmd(dev, row)

    async def _apply_acked_cmd(self, dev: str, row: dict[str, Any]) -> None:
        """设备确认了一条指令之后，把服务端这边的状态跟上。

        目前只有 `interval` 需要：离线阈值 = 上报周期 × factor + grace（契约 §4.2），
        而周期是设备侧的事实。不跟着改的话，把周期调大之后健康的车会被判离线；
        调小则要等三倍新周期才发现真离线。**只在 ack 成功后写** —— 指令排在队列里
        还没送到时，设备用的仍是旧周期。
        """
        if row["suffix"] != ct.DN_CMD:
            return
        try:
            body = json.loads(row["payload"])
        except (json.JSONDecodeError, TypeError):
            return
        if not isinstance(body, dict) or body.get("c") != "interval":
            return
        secs = (body.get("a") or {}).get("s")
        if not isinstance(secs, int) or secs <= 0:
            return
        dcfg = self.cfg.device(dev)
        if dcfg is None or dcfg.report_interval == secs:
            return
        old = dcfg.report_interval
        dcfg.report_interval = secs
        await self.store.set_meta(f"interval:{dev}", str(secs))
        log.info("%s 上报周期 %d → %d s，离线阈值改为 %d s",
                 dev, old, secs, self.cfg.offline_after(dev))

    async def _restore_intervals(self) -> None:
        """把 `interval` 指令改过的周期从库里读回来（见 _apply_acked_cmd）。"""
        for d in self.cfg.devices:
            raw = await self.store.get_meta(f"interval:{d.id}")
            if raw is None:
                continue
            try:
                secs = int(raw)
            except ValueError:
                continue
            if secs > 0 and secs != d.report_interval:
                log.info("%s 恢复上报周期 %d s（配置文件里是 %d）",
                         d.id, secs, d.report_interval)
                d.report_interval = secs

    async def _on_lwt(self, dev: str, payload: bytes) -> None:
        """契约 §4.2：LWT **不是**离线判据，只是「上次断连是否非优雅」的痕迹。

        因为没有备份电池（DESIGN.md §6），剪线的瞬间设备来不及说话，
        所以 lwt=1 区分不了「正常关机档」和「被剪线」。
        """
        flag = 1
        try:
            o = json.loads(payload)
            if isinstance(o, dict):
                flag = int(o.get("lwt", 1))
        except (json.JSONDecodeError, ValueError, TypeError):
            pass
        await self.store.set_dev_fields(dev, lwt=flag)

    async def ingest_lwt_will(self, topic: str, payload: bytes) -> None:
        """broker 代发遗嘱的落库入口（审计 M9 / 契约 §4.2）。

        amqtt 非优雅断连的遗嘱走 `_broadcast_message`，不触发
        MESSAGE_RECEIVED，`IngestPlugin` 的常规路径看不到它；
        `IngestPlugin.on_broker_retained_message` 从遗嘱 retain
        （will_retain=1）那条路把它转到这里。身份在这里重新判：
        topic 必须落在已配置设备上（遗嘱的 topic 是设备 CONNECT 时
        自己配的，broker 原样广播，服务端只信已配置设备的 lwt topic）。
        """
        try:
            parsed = ct.parse_topic(topic)
        except ct.ContractError:
            return
        dev, suffix = parsed.device_id, parsed.suffix
        if suffix != ct.LWT or self.cfg.device(dev) is None:
            return
        async with self._lock(dev):
            await self._on_lwt(dev, payload)
            # 已持锁 —— _locked 变体（审计 M11）
            await self._publish_state_locked(dev)

    async def on_device_online(self, client_id: str) -> None:
        """CLIENT_CONNECTED 时冲刷下行队列（契约 §4.1）。"""
        if self.cfg.device(client_id) is None:
            return
        # flush_downlinks 自己拿锁（审计 M11），这里不再包一层 —— 包了会死锁。
        await self.flush_downlinks(client_id)

    # --- 下行 ----------------------------------------------------------------

    async def next_dn_id(self, prefix: str) -> str:
        """取一个全局唯一的下行 id。序号存在库里，跨重启不回退。"""
        seq = await self.store.reserve_dn_seq()
        return f"{prefix}-{seq}"

    async def enqueue_cmd(self, dev: str, cmd: str,
                          args: dict[str, Any] | None = None) -> str:
        dn_id = await self.next_dn_id("c")
        payload = ct.build_cmd(dn_id, cmd, args)
        # 审计 M11：入队和冲刷必须在同一把锁里。两个并发 HTTP 请求各自
        # 「入队 → 冲刷」交错时，后入队的可能先发出去 —— 而 flush_downlinks
        # 的文档说顺序是硬要求（连续两次密钥轮换必须按序到达）。
        async with self._lock(dev):
            await self.store.enqueue_downlink(dn_id, dev, ct.DN_CMD, payload)
            await self._flush_downlinks_locked(dev)
        return dn_id

    async def enqueue_secret(self, dev: str, op: str, **kw: Any) -> str:
        dn_id = await self.next_dn_id("s")
        payload = ct.build_secret(dn_id, op, **kw)
        async with self._lock(dev):
            await self.store.enqueue_downlink(dn_id, dev, ct.DN_SECRET, payload)
            await self._flush_downlinks_locked(dev)
        return dn_id

    async def flush_downlinks(self, dev: str) -> int:
        """把未确认的下行按创建顺序发出去。返回发了几条。

        **自己拿锁**。已经持锁的调用方（ingest 路径）用 `_flush_downlinks_locked`
        —— `asyncio.Lock` 不可重入，在持锁时调这个会死锁。
        """
        async with self._lock(dev):
            return await self._flush_downlinks_locked(dev)

    async def _flush_downlinks_locked(self, dev: str) -> int:
        """`flush_downlinks` 的实现。**调用方必须已持 `_lock(dev)`。**

        **顺序是硬要求**：连续两次密钥轮换必须按序到达，否则设备会停在旧密钥上。
        每次都重发全部未确认的（而不是只发新的），因为设备可能漏掉了中间某条。
        锁保证「读队列 → 逐条发 → 标记已发」不与另一个冲刷交错 ——
        否则两个协程会各读到同一批 pending 行并把它们发两遍、且顺序交错。
        """
        if self.broker is None:
            return 0
        rows = await self.store.pending_downlinks(dev)
        for row in rows:
            topic = ct.topic(dev, row["suffix"])
            # retain 恒为 False（契约 §4.1）：internal_message_broadcast 不设
            # retain flag，正好是我们要的行为 —— 密钥不会留在 broker 的 retain 表里。
            await self.broker.internal_message_broadcast(
                topic, row["payload"], _QOS1)
            await self.store.mark_sent(row["id"])
            log.info("发下行 %s → %s（第 %d 次尝试）",
                     row["id"], topic, row["tries"] + 1)
        return len(rows)

    # --- state ---------------------------------------------------------------

    async def publish_state(self, dev: str, *, force: bool = False) -> dict[str, Any]:
        """算 state 并 retain 发布。`force=True` 用于进程启动时恢复 retain。

        **自己拿锁**（审计 M11）。已经持锁的调用方（ingest 路径）用
        `_publish_state_locked` —— `asyncio.Lock` 不可重入。

        以前这个方法不拿锁，而 ingest 路径持锁调它：`/state/{id}`、`/devices`、
        `_tick` 三条不持锁的路径可以和 ingest 交错，让**旧 state 晚于新 state**
        写进 retain 表（HA 上显示陈旧值，下一轮 tick 才自愈）。
        """
        async with self._lock(dev):
            return await self._publish_state_locked(dev, force=force)

    async def _publish_state_locked(self, dev: str, *,
                                    force: bool = False) -> dict[str, Any]:
        """`publish_state` 的实现。**调用方必须已持 `_lock(dev)`。**

        锁保护的不变量：「算 state → 比对旧值 → 发布 → 写回 state_json」
        整体原子。少了它，两个协程算出的 state 可以按相反顺序写 retain。
        """
        state = await build_state(self.store, dev, self.cfg)
        ds = await self.store.dev_state(dev)
        if not force and not state_changed(ds.get("state_json"), state):
            return state

        payload = ct.dumps(state)
        if self.broker is not None:
            topic = ct.topic(dev, ct.STATE)
            await self.broker.internal_message_broadcast(topic, payload, _QOS1)
            # internal_message_broadcast 不会设 retain，要自己调 retain_message，
            # 否则「重启 HA 立即有位置」不成立（DESIGN.md §9.4）
            await self.broker.retain_message(None, topic, payload, _QOS1)
        await self.store.set_dev_fields(
            dev, state_json=json.dumps(state, separators=(",", ":")),
            mode=state.get("mo"))
        return state
