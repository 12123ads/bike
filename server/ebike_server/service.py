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
        self._dn_seq = 0
        self._tasks: list[asyncio.Task[Any]] = []
        #: 每设备一把锁：ingest 是并发的（broker 为每条消息起 task），
        #: 而「算 state → 比对 → 发布」不是原子的，两条报文同时到会发出乱序的 state。
        self._locks: dict[str, asyncio.Lock] = {}

    def _lock(self, dev: str) -> asyncio.Lock:
        return self._locks.setdefault(dev, asyncio.Lock())

    # --- 生命周期 -------------------------------------------------------------

    async def start(self) -> None:
        await self.store.open()
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
                    if n:
                        log.info("清理过期数据 %d 行", n)
            except asyncio.CancelledError:
                raise
            except Exception:
                log.exception("周期任务出错")

    # --- 上行 ----------------------------------------------------------------

    async def ingest(self, client_id: str | None, topic: str, payload: bytes) -> None:
        """处理一条上行。**所有校验失败都只记日志不抛** —— 畸形报文是常态
        （固件调试期尤其），不能让它影响别的设备或弄死 broker 的 task。
        """
        try:
            parsed = ct.parse_topic(topic)
        except ct.ContractError:
            log.warning("丢弃：topic 不属于契约 %r (client=%s)", topic, client_id)
            return

        dev, suffix = parsed.device_id, parsed.suffix

        # ⚠ MESSAGE_RECEIVED 在 broker 的 ACL 检查**之前**触发
        # （amqtt/broker.py:753 的注释明说了），所以这里自己再判一次身份。
        # 没有这一层，一个能登录的客户端可以往别的设备 id 下灌报文。
        if client_id is not None and self.cfg.device(dev) is not None:
            expected = {dev, "svc"}
            if client_id not in expected:
                log.warning("丢弃：client %r 无权发 %s", client_id, topic)
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
            await self.publish_state(dev)

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
            await self.flush_downlinks(dev)
        elif suffix == ct.UP_ACK:
            assert isinstance(data, dict)
            found = await self.store.mark_acked(
                data["dn_id"], data["ok"], data.get("err"))
            if not found:
                # 可能是重发的 ack，也可能是伪造的。两种都只需记录。
                log.info("%s ack 了一个不在队列里的 id=%s", dev, data["dn_id"])
            elif not data["ok"]:
                log.warning("%s 拒绝了下行 %s: %s",
                            dev, data["dn_id"], data.get("err"))

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

    async def on_device_online(self, client_id: str) -> None:
        """CLIENT_CONNECTED 时冲刷下行队列（契约 §4.1）。"""
        if self.cfg.device(client_id) is None:
            return
        async with self._lock(client_id):
            await self.flush_downlinks(client_id)

    # --- 下行 ----------------------------------------------------------------

    def next_dn_id(self, prefix: str) -> str:
        self._dn_seq += 1
        return f"{prefix}-{self._dn_seq}"

    async def enqueue_cmd(self, dev: str, cmd: str,
                          args: dict[str, Any] | None = None) -> str:
        dn_id = self.next_dn_id("c")
        payload = ct.build_cmd(dn_id, cmd, args)
        await self.store.enqueue_downlink(dn_id, dev, ct.DN_CMD, payload)
        await self.flush_downlinks(dev)
        return dn_id

    async def enqueue_secret(self, dev: str, op: str, **kw: Any) -> str:
        dn_id = self.next_dn_id("s")
        payload = ct.build_secret(dn_id, op, **kw)
        await self.store.enqueue_downlink(dn_id, dev, ct.DN_SECRET, payload)
        await self.flush_downlinks(dev)
        return dn_id

    async def flush_downlinks(self, dev: str) -> int:
        """把未确认的下行按创建顺序发出去。返回发了几条。

        **顺序是硬要求**：连续两次密钥轮换必须按序到达，否则设备会停在旧密钥上。
        每次都重发全部未确认的（而不是只发新的），因为设备可能漏掉了中间某条。
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
        """算 state 并 retain 发布。`force=True` 用于进程启动时恢复 retain。"""
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
