"""device_tracker：地图上那辆车。

这是整个集成的主实体。两件事值得单独说：

1. **坐标系可选**（契约 §7 两套都发）。默认 GCJ-02，因为国内地图卡是 GCJ-02；
   用 OSM 底图就在选项里切 wgs84。**不做「自动判断」**——猜错了车会偏几百米
   而且没人知道为什么。
2. **`location_accuracy` 就是「误差圈可见」那条验收**（DESIGN.md §9.4）。
   基站定位降级时这个值会是 1000 米量级，HA 会画一个大圈，那正是想要的效果。
"""

from __future__ import annotations

from typing import Any

from homeassistant.components.device_tracker import SourceType, TrackerEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import (
    ATTR_COORD_SYSTEM,
    ATTR_GEOFENCE,
    ATTR_SOURCE,
    CONF_COORD_SYSTEM,
    COORD_GCJ02,
    DEFAULT_COORD_SYSTEM,
    DOMAIN,
    F_ACCURACY,
    F_GCJ_LAT,
    F_GCJ_LON,
    F_GEOFENCE,
    F_LAT,
    F_LON,
    F_SRC,
    SRC_LABELS,
)
from .coordinator import EbikeCoordinator
from .entity import EbikeEntity


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    coordinator: EbikeCoordinator = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([EbikeTracker(coordinator, entry)])


class EbikeTracker(EbikeEntity, TrackerEntity):
    _attr_name = None          # 用设备名，界面上就是「电瓶车 bike01」
    _attr_icon = "mdi:moped"
    # BaseTrackerEntity 把 _attr_entity_category 钉成 DIAGNOSTIC，
    # 不覆盖的话地图实体会被折叠进设备页的「诊断」区。它是主实体，要在主区。
    _attr_entity_category = None

    def __init__(self, coordinator: EbikeCoordinator, entry: ConfigEntry) -> None:
        super().__init__(coordinator, entry, "tracker")

    @property
    def source_type(self) -> SourceType:
        return SourceType.GPS

    @property
    def _use_gcj(self) -> bool:
        return self._entry.options.get(
            CONF_COORD_SYSTEM, DEFAULT_COORD_SYSTEM
        ) == COORD_GCJ02

    @property
    def latitude(self) -> float | None:
        if self._use_gcj:
            # GCJ 字段缺失时**不回落到 WGS84** —— 静默回落会让车偏几百米
            # 而界面上看不出任何异常。宁可显示「没有位置」。
            return self.coordinator.get(F_GCJ_LAT)
        return self.coordinator.get(F_LAT)

    @property
    def longitude(self) -> float | None:
        if self._use_gcj:
            return self.coordinator.get(F_GCJ_LON)
        return self.coordinator.get(F_LON)

    @property
    def location_accuracy(self) -> float:
        """误差圈半径，米。这就是「误差圈可见」那条验收（DESIGN.md §9.4）。

        core 的类型是 **float**（`TrackerEntity.location_accuracy -> float`），
        不是 int —— 不取整，8.0 和 1000.0 原样给出去。

        **精度缺失时不自己编一个数。** core 的默认值就是 0，而 0 的语义是
        「精确到点」；真正表达「不知道位置」的方式是让 latitude/longitude
        为 None —— core 的 `state_attributes` 在坐标缺失时连 `gps_accuracy`
        一起不发，实体变 unknown。所以这里缺失时回落到 core 的默认值，
        由坐标那一侧去表达「没有位置」。
        """
        acc = self.coordinator.get(F_ACCURACY)
        if acc is None:
            return 0.0
        return float(acc)

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        attrs = super().extra_state_attributes
        if (src := self.coordinator.get(F_SRC)) is not None:
            attrs[ATTR_SOURCE] = SRC_LABELS.get(src, src)
        if (gf := self.coordinator.get(F_GEOFENCE)) is not None:
            attrs[ATTR_GEOFENCE] = gf
        attrs[ATTR_COORD_SYSTEM] = "GCJ-02" if self._use_gcj else "WGS84"
        # 两套坐标都放进属性，方便对着底图核对偏移
        for key in (F_LAT, F_LON, F_GCJ_LAT, F_GCJ_LON):
            if (v := self.coordinator.get(key)) is not None:
                attrs[key] = v
        return attrs
