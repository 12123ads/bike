"""配置流程：只问一个设备 id，其余全靠 HA 自己的 MQTT 配置。

刻意不问 broker 地址/端口/证书 —— 那些在 HA 的 MQTT 集成里配一次就够了，
在这里再问一遍等于让用户维护两份，而且会诱使我们自己开一条 MQTT 连接。
"""

from __future__ import annotations

import re
from typing import Any

import voluptuous as vol
from homeassistant.components import mqtt
from homeassistant.config_entries import (
    ConfigFlow,
    ConfigFlowResult,
    OptionsFlow,
    ConfigEntry,
)
from homeassistant.core import callback
from homeassistant.helpers.selector import (
    SelectSelector,
    SelectSelectorConfig,
    SelectSelectorMode,
)

from .const import (
    CONF_COORD_SYSTEM,
    CONF_DEVICE_ID,
    COORD_GCJ02,
    COORD_WGS84,
    DEFAULT_COORD_SYSTEM,
    DOMAIN,
)

# 契约 §4：设备 id 是 [a-z0-9-]{1,32}，且出现在 topic 层级里
DEVICE_ID_RE = re.compile(r"^[a-z0-9-]{1,32}$")

COORD_SELECTOR = SelectSelector(
    SelectSelectorConfig(
        options=[COORD_GCJ02, COORD_WGS84],
        translation_key="coord_system",
        mode=SelectSelectorMode.DROPDOWN,
    )
)


class EbikeConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        errors: dict[str, str] = {}

        if user_input is not None:
            device_id = user_input[CONF_DEVICE_ID].strip()

            if not DEVICE_ID_RE.match(device_id):
                errors[CONF_DEVICE_ID] = "invalid_device_id"
            else:
                # 同一辆车只能加一次 —— 加两次会有两套重名实体
                await self.async_set_unique_id(device_id)
                self._abort_if_unique_id_configured()

                return self.async_create_entry(
                    title=f"电瓶车 {device_id}",
                    data={CONF_DEVICE_ID: device_id},
                    options={
                        CONF_COORD_SYSTEM: user_input.get(
                            CONF_COORD_SYSTEM, DEFAULT_COORD_SYSTEM
                        )
                    },
                )

        # MQTT 没配好就直接说清楚，别让用户加完之后对着一堆 unavailable 实体猜
        if not await _mqtt_available(self.hass):
            return self.async_abort(reason="mqtt_required")

        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema(
                {
                    vol.Required(CONF_DEVICE_ID, default="bike01"): str,
                    vol.Optional(
                        CONF_COORD_SYSTEM, default=DEFAULT_COORD_SYSTEM
                    ): COORD_SELECTOR,
                }
            ),
            errors=errors,
        )

    @staticmethod
    @callback
    def async_get_options_flow(entry: ConfigEntry) -> OptionsFlow:
        return EbikeOptionsFlow()


class EbikeOptionsFlow(OptionsFlow):
    """只有坐标系一项 —— 换底图不该需要重装集成。"""

    async def async_step_init(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        if user_input is not None:
            return self.async_create_entry(data=user_input)

        current = self.config_entry.options.get(
            CONF_COORD_SYSTEM, DEFAULT_COORD_SYSTEM
        )
        return self.async_show_form(
            step_id="init",
            data_schema=vol.Schema(
                {vol.Optional(CONF_COORD_SYSTEM, default=current): COORD_SELECTOR}
            ),
        )


async def _mqtt_available(hass) -> bool:
    """HA 的 MQTT 集成配好了吗。

    用 `hass.config_entries` 判而不是 try/except 一次订阅：后者会在
    「MQTT 正在启动」时误报失败。
    """
    return bool(hass.config_entries.async_entries(mqtt.DOMAIN))
