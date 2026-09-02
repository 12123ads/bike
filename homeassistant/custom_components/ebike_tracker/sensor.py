"""传感器：电压、电量、定位方式、最后上报时间。

电量百分比**不用 `SensorDeviceClass.BATTERY`**：那个 device class 在 HA 里
语义是「这个设备自己的电池」，而这里是**车的动力电池**。用它会让 HA 在
「设备电量低」之类的自动化里把这辆车和手机、传感器混在一起。
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorEntityDescription,
    SensorStateClass,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import PERCENTAGE, UnitOfElectricPotential
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import (
    DOMAIN,
    F_ACCURACY,
    F_LAST_SEEN,
    F_PERCENT,
    F_SRC,
    F_VOLT,
    SRC_LABELS,
)
from .coordinator import EbikeCoordinator
from .entity import EbikeEntity


@dataclass(frozen=True, kw_only=True)
class EbikeSensorDescription(SensorEntityDescription):
    """加一个取值函数 —— 每个传感器只有取值逻辑不同，别的都一样。"""

    value_fn: Callable[[EbikeCoordinator], Any]


def _last_seen(coord: EbikeCoordinator) -> datetime | None:
    """服务端时钟的 Unix 秒 → 带时区的 datetime。

    HA 的 TIMESTAMP device class 要求 tz-aware，naive 会被拒。
    契约 §5.6：这里用的是 `ls`（服务端时钟），不是设备的 `t` ——
    设备时钟在拿到 NITZ 之前是错的。
    """
    ts = coord.get(F_LAST_SEEN)
    if not isinstance(ts, (int, float)) or ts <= 0:
        return None
    return datetime.fromtimestamp(ts, tz=timezone.utc)


SENSORS: tuple[EbikeSensorDescription, ...] = (
    EbikeSensorDescription(
        key="voltage",
        translation_key="voltage",
        device_class=SensorDeviceClass.VOLTAGE,
        state_class=SensorStateClass.MEASUREMENT,
        native_unit_of_measurement=UnitOfElectricPotential.VOLT,
        suggested_display_precision=1,
        value_fn=lambda c: c.get(F_VOLT),
    ),
    EbikeSensorDescription(
        key="battery_percent",
        translation_key="battery_percent",
        # 刻意不用 SensorDeviceClass.BATTERY，理由见模块 docstring
        state_class=SensorStateClass.MEASUREMENT,
        native_unit_of_measurement=PERCENTAGE,
        icon="mdi:battery-50",
        value_fn=lambda c: c.get(F_PERCENT),
    ),
    EbikeSensorDescription(
        key="accuracy",
        translation_key="accuracy",
        state_class=SensorStateClass.MEASUREMENT,
        native_unit_of_measurement="m",
        icon="mdi:circle-outline",
        suggested_display_precision=0,
        value_fn=lambda c: c.get(F_ACCURACY),
    ),
    EbikeSensorDescription(
        key="fix_source",
        translation_key="fix_source",
        icon="mdi:satellite-variant",
        value_fn=lambda c: SRC_LABELS.get(c.get(F_SRC), c.get(F_SRC)),
    ),
    EbikeSensorDescription(
        key="last_seen",
        translation_key="last_seen",
        device_class=SensorDeviceClass.TIMESTAMP,
        icon="mdi:clock-outline",
        value_fn=_last_seen,
    ),
)


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    coordinator: EbikeCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities(
        EbikeSensor(coordinator, entry, desc) for desc in SENSORS
    )


class EbikeSensor(EbikeEntity, SensorEntity):
    entity_description: EbikeSensorDescription

    def __init__(self, coordinator: EbikeCoordinator, entry: ConfigEntry,
                 description: EbikeSensorDescription) -> None:
        super().__init__(coordinator, entry, description.key)
        self.entity_description = description

    @property
    def native_value(self) -> Any:
        return self.entity_description.value_fn(self.coordinator)
