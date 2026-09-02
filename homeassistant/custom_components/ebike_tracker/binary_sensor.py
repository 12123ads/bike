"""二值传感器：在线、移动中、锁着。

锁状态做成 `binary_sensor` 而不是 `lock` 实体，是有意的：`lock` 实体带
「上锁/开锁」按钮，而远程开锁绕过 NFC 挑战应答、两边默认都是关的（契约 §6.1）。
给一个按不动或者按了会失败的按钮比不给按钮更糟。要远程开锁就在
`docs/HA.md` 里按说明配一个 `rest_command`，那样至少是显式的。
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
    BinarySensorEntityDescription,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import (
    DOMAIN,
    F_LOCKED,
    F_MODE,
    F_ONLINE,
    MODE_MOVING,
)
from .coordinator import EbikeCoordinator
from .entity import EbikeEntity


@dataclass(frozen=True, kw_only=True)
class EbikeBinaryDescription(BinarySensorEntityDescription):
    #: 返回 None 表示「不知道」—— HA 会显示 unknown 而不是 off
    value_fn: Callable[[EbikeCoordinator], bool | None]


def _online(coord: EbikeCoordinator) -> bool | None:
    v = coord.get(F_ONLINE)
    return bool(v) if isinstance(v, bool) else None


def _moving(coord: EbikeCoordinator) -> bool | None:
    mode = coord.get(F_MODE)
    if mode is None:
        return None
    return mode == MODE_MOVING


def _locked(coord: EbikeCoordinator) -> bool | None:
    """锁着吗。

    契约 §7：`lk` 为 null 是**常态** —— 位置反馈微动开关是选配的，
    没接就永远拿不到 `lock_state` 事件。
    **必须返回 None 而不是 False**：显示成「没锁」会让人以为车没锁好而白跑一趟，
    显示成「未知」才是事实。
    """
    v = coord.get(F_LOCKED)
    return bool(v) if isinstance(v, bool) else None


BINARY_SENSORS: tuple[EbikeBinaryDescription, ...] = (
    EbikeBinaryDescription(
        key="online",
        translation_key="online",
        device_class=BinarySensorDeviceClass.CONNECTIVITY,
        value_fn=_online,
    ),
    EbikeBinaryDescription(
        key="moving",
        translation_key="moving",
        device_class=BinarySensorDeviceClass.MOVING,
        value_fn=_moving,
    ),
    EbikeBinaryDescription(
        key="locked",
        translation_key="locked",
        device_class=BinarySensorDeviceClass.LOCK,
        # ⚠ HA 的 LOCK device class 语义是**反的**：on = 未锁（unlocked）。
        # 所以取值函数返回「锁着」，这里要取反。
        value_fn=lambda c: (None if _locked(c) is None else not _locked(c)),
    ),
)


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    coordinator: EbikeCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities(
        EbikeBinarySensor(coordinator, entry, desc) for desc in BINARY_SENSORS
    )


class EbikeBinarySensor(EbikeEntity, BinarySensorEntity):
    entity_description: EbikeBinaryDescription

    def __init__(self, coordinator: EbikeCoordinator, entry: ConfigEntry,
                 description: EbikeBinaryDescription) -> None:
        super().__init__(coordinator, entry, description.key)
        self.entity_description = description

    @property
    def is_on(self) -> bool | None:
        return self.entity_description.value_fn(self.coordinator)
