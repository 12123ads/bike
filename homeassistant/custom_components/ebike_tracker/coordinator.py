"""协调器：订一个 MQTT topic，把 state 分发给各实体。

为什么不用 `DataUpdateCoordinator`：那个是给**轮询**设计的（它的核心是
`update_interval` + `_async_update_data`）。这里是 `local_push` —— 数据由 broker
推过来，没有「去拉一次」的动作。硬套那个基类会引入一个永远不用的轮询路径。

所以这里是一个最小的「订阅 + 回调分发」，用 HA 的 `mqtt.async_subscribe`
（复用 HA 自己配好的 MQTT 连接，包括 TLS 和凭据 —— 我们不自己开连接）。
"""

from __future__ import annotations

import json
import logging
from collections.abc import Callable
from typing import Any

from homeassistant.components import mqtt
from homeassistant.core import HomeAssistant, callback

from .const import F_TIME, state_topic

_LOGGER = logging.getLogger(__name__)


class EbikeCoordinator:
    """持有一辆车的最新 state，并在更新时通知实体。"""

    def __init__(self, hass: HomeAssistant, device_id: str) -> None:
        self.hass = hass
        self.device_id = device_id
        self.data: dict[str, Any] = {}
        #: 收到过至少一条 state 吗。实体的 available 靠它 ——
        #: 空 dict 和「设备离线」是两件事：前者是我们还不知道，后者是我们知道它离线。
        self.has_data = False
        self._listeners: list[Callable[[], None]] = []
        self._unsub: Callable[[], None] | None = None

    @property
    def topic(self) -> str:
        return state_topic(self.device_id)

    async def async_start(self) -> None:
        """订阅。QoS 1 —— 和契约 §4 的 state 一致。"""
        self._unsub = await mqtt.async_subscribe(
            self.hass, self.topic, self._message_received, qos=1
        )
        _LOGGER.debug("已订阅 %s", self.topic)

    @callback
    def async_stop(self) -> None:
        if self._unsub is not None:
            self._unsub()
            self._unsub = None

    @callback
    def _message_received(self, msg: mqtt.ReceiveMessage) -> None:
        """收到一条 state。

        **畸形报文只记日志不抛** —— 抛出去会让 HA 的 MQTT 分发链路记一堆
        堆栈，而且下一条好报文照样能恢复。这和服务端 ingest 的处理一致。
        """
        try:
            payload = json.loads(msg.payload)
        except (json.JSONDecodeError, TypeError, ValueError):
            _LOGGER.warning("%s 收到非 JSON 报文，丢弃", self.topic)
            return

        if not isinstance(payload, dict):
            _LOGGER.warning("%s 的 state 不是对象，丢弃", self.topic)
            return

        # 乱序保护：MQTT 不保证跨消息顺序，而服务端每次状态变化都发一条。
        # 收到一条比当前更旧的就丢掉，否则地图上的车会往回跳。
        old_t = self.data.get(F_TIME)
        new_t = payload.get(F_TIME)
        if isinstance(old_t, int) and isinstance(new_t, int) and new_t < old_t:
            _LOGGER.debug("丢弃过期 state（t=%s < %s）", new_t, old_t)
            return

        self.data = payload
        self.has_data = True
        _LOGGER.debug(
            "state: on=%s mo=%s la=%s gla=%s a=%s v=%s pct=%s lk=%s",
            payload.get("on"), payload.get("mo"), payload.get("la"),
            payload.get("gla"), payload.get("a"), payload.get("v"),
            payload.get("pct"), payload.get("lk"),
        )
        for update_callback in self._listeners:
            update_callback()

    @callback
    def async_add_listener(self, update_callback: Callable[[], None]) -> Callable[[], None]:
        """注册实体的刷新回调，返回取消函数。"""
        self._listeners.append(update_callback)

        @callback
        def remove_listener() -> None:
            if update_callback in self._listeners:
                self._listeners.remove(update_callback)

        return remove_listener

    def get(self, key: str, default: Any = None) -> Any:
        return self.data.get(key, default)
