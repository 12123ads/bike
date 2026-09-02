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
        self._attr_unique_id = f"{entry.entry_id}_{key}"

    @property
    def device_info(self) -> DeviceInfo:
        """把所有实体归到同一个「设备」下，HA 界面里就是一张卡片。"""
        return DeviceInfo(
            identifiers={(DOMAIN, self.coordinator.device_id)},
            name=f"电瓶车 {self.coordinator.device_id}",
            manufacturer="自制",
            model="nRF52840 + Air780EP",
            sw_version=self._entry.data.get("fw_version"),
        )

    @property
    def available(self) -> bool:
        """还没收到过任何 state = 不可用。

        ⚠ **注意这和「车离线」是两件事**：车离线时我们**仍然收得到** state
        （服务端算出 `on=false` 并 retain 发布），那时候实体是**可用**的，
        只是 `binary_sensor` 显示离线。真正不可用只有「HA 刚启动还没收到 retain」
        或「MQTT 断了」——后者 HA 自己会处理。

        把两者混在一起的后果：车一离线所有实体变成 unavailable，
        历史图断掉，而且看不到「最后一次在哪」——那恰恰是车被偷时最需要的信息。
        """
        return self.coordinator.has_data

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
