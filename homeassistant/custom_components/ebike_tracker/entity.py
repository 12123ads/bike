"""实体基类：设备信息和公共可用性逻辑。

单独一个文件是因为四个平台（device_tracker / sensor / binary_sensor）都要它，
放进任何一个平台文件都会造成循环 import。
"""

from __future__ import annotations

from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity import Entity

from .const import (
    ATTR_LAST_SEEN,
    ATTR_UNGRACEFUL,
    DOMAIN,
    F_FW,
    F_LAST_SEEN,
    F_LWT,
)
from .coordinator import EbikeCoordinator


class EbikeEntity(Entity):
    """所有实体的基类。"""

    _attr_has_entity_name = True
    _attr_should_poll = False   # local_push：数据是推过来的

    def __init__(self, coordinator: EbikeCoordinator, entry: ConfigEntry,
                 key: str) -> None:
        self.coordinator = coordinator
        self._entry = entry
        self._key = key
        # ⚠ **用 device_id 而不是 entry_id。** entry_id 是 ULID，删掉集成再加
        # 同一辆车会得到新的 entry_id → 新的 unique_id → 实体 id 带 _2 后缀，
        # 历史曲线、自定义名字、dashboard 引用全断。device_id 是设备出厂烧的，
        # 而且 entry 的 unique_id 本来就是它（config_flow.py），天然唯一。
        self._attr_unique_id = f"{coordinator.device_id}_{key}"

    @property
    def device_info(self) -> DeviceInfo:
        """把所有实体归到同一个「设备」下，HA 界面里就是一张卡片。"""
        return DeviceInfo(
            identifiers={(DOMAIN, self.coordinator.device_id)},
            name=f"电瓶车 {self.coordinator.device_id}",
            manufacturer="自制",
            model="nRF52840 + Air780EP",
            # 固件版本走 state 的 fw 字段（契约 §5.1 的 up/hello → §7 的 state）。
            # 以前这里读 entry.data["fw_version"]，而**没有任何代码往那里写**，
            # 所以设备卡片上永远是空的。
            sw_version=self.coordinator.get(F_FW),
        )

    @property
    def available(self) -> bool:
        """两个条件：MQTT 连着，且收到过至少一条 state。

        ⚠ **「车离线」不在这两个条件里。** 车离线时我们**仍然收得到** state
        （服务端算出 `on=false` 并 retain 发布），那时候实体是**可用**的，
        只是 `binary_sensor.在线` 显示 off。
        混在一起的后果：车一离线所有实体变 unavailable、历史图断掉、
        **看不到最后一次在哪** —— 那恰恰是车被偷时最需要的信息。

        ⚠ **MQTT 断开必须自己判。** HA 的自动 unavailable 只作用于
        `MqttAvailabilityMixin` 的实体（`mqtt/entity.py`），而本集成的实体
        属于自己 domain 的三个平台，不继承那个 mixin。不判的话 broker 挂掉后
        9 个实体会继续显示最后一次的值并标记为可用 —— 位置、在线、锁状态
        全部静默陈旧，而使用者分不清是真值还是过期值。
        """
        return self.coordinator.mqtt_connected and self.coordinator.has_data

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        attrs: dict[str, Any] = {}
        if (ls := self.coordinator.get(F_LAST_SEEN)) is not None:
            attrs[ATTR_LAST_SEEN] = ls
        # lwt=1 表示上次断连是非优雅的。因为没有备份电池（DESIGN.md §6），
        # 它区分不了「正常关机档」和「被剪线」，所以只作为属性暴露，不做告警。
        if (lwt := self.coordinator.get(F_LWT)) is not None:
            attrs[ATTR_UNGRACEFUL] = bool(lwt)
        return attrs

    async def async_added_to_hass(self) -> None:
        self.async_on_remove(
            self.coordinator.async_add_listener(self.async_write_ha_state)
        )
