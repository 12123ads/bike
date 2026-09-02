"""集成入口。

`dependencies: ["mqtt"]` 保证 HA 的 MQTT 集成先就绪，我们复用它的连接 ——
不自己开 MQTT 客户端。这也意味着用户要先在 HA 里配好 MQTT（含 TLS 和 ca.crt），
配置流程里会检查这一点。
"""

from __future__ import annotations

import logging

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryNotReady

from .const import CONF_DEVICE_ID, DOMAIN
from .coordinator import EbikeCoordinator

_LOGGER = logging.getLogger(__name__)

PLATFORMS: list[Platform] = [
    Platform.DEVICE_TRACKER,
    Platform.SENSOR,
    Platform.BINARY_SENSOR,
]


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    device_id = entry.data[CONF_DEVICE_ID]
    coordinator = EbikeCoordinator(hass, device_id)

    try:
        await coordinator.async_start()
    except Exception as err:  # MQTT 未就绪等
        raise ConfigEntryNotReady(f"订阅 {coordinator.topic} 失败: {err}") from err

    # 取消订阅挂在 entry 上，而不是只在 async_unload_entry 里调 ——
    # 下面 async_forward_entry_setups 抛异常时 setup 算失败，
    # unload 不会被调用，订阅就悬空了（每次重试再叠一个）。
    entry.async_on_unload(coordinator.async_stop)

    hass.data.setdefault(DOMAIN, {})[entry.entry_id] = coordinator

    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)

    # 选项改了（比如切坐标系）要重载 —— 坐标系影响 device_tracker 报哪个经纬度
    entry.async_on_unload(entry.add_update_listener(_async_update_listener))

    # ⚠ **刻意不主动去取一次当前状态。**
    # state 是 retain 的（契约 §7），所以订阅成功后 broker 会立刻推最后一条过来。
    # 这正是「重启 HA 立即有位置」那条验收标准成立的机制（DESIGN.md §9.4）。
    _LOGGER.info("电瓶车 %s 已接入，等 retain 的 state（topic=%s）",
                 device_id, coordinator.topic)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    unload_ok = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if unload_ok:
        # 取消订阅不在这里做 —— 它已经注册进 entry.async_on_unload（见上），
        # HA 会在本函数返回后调用。两处都调也无害（async_stop 是幂等的），
        # 但只留一处更不容易漏。
        hass.data[DOMAIN].pop(entry.entry_id, None)
    return unload_ok


async def _async_update_listener(hass: HomeAssistant, entry: ConfigEntry) -> None:
    await hass.config_entries.async_reload(entry.entry_id)
