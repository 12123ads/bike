"""常量与契约字段名。

**这个文件是 `docs/MQTT-CONTRACT.md` §7 的可执行副本。**
字段名全部集中在这里，不在别的文件里写字面量 —— 改契约只改这一处。
`server/tests/test_ha_contract.py` 会把这里的常量和服务端的 `contract.py`
对照，改一边不改另一边会红。
"""

from __future__ import annotations

from typing import Final

DOMAIN: Final = "ebike_tracker"

# --- 契约 §4：topic ----------------------------------------------------------

PREFIX: Final = "ebike/v1"
STATE_SUFFIX: Final = "state"


def state_topic(device_id: str) -> str:
    """单台车的 state topic。

    **刻意不用通配符 `ebike/v1/+/state`。** 两个理由：
    1. 一个 config entry 对应一辆车，订通配符会收到别的车的报文然后丢掉；
    2. 更重要的是 `amqtt` 的一个缺陷（契约 §4.3）—— 它在客户端连上时按
       **全局**订阅过滤器表推 retain 消息，绕过 ACL。用了通配符就等于
       把「所有车的 state」这个过滤器注册进那张表，加第二辆车时
       bike02 一连上就会收到 bike01 的位置。精确订阅不会。
    """
    return f"{PREFIX}/{device_id}/{STATE_SUFFIX}"


# --- 契约 §7：state 字段 ------------------------------------------------------

F_TIME: Final = "t"          # 服务端时钟，Unix 秒
F_ONLINE: Final = "on"       # 在线（服务端超时算的，不是 LWT —— 契约 §4.2）
F_MODE: Final = "mo"         # moving / parked
F_LAT: Final = "la"          # WGS84
F_LON: Final = "lo"
F_GCJ_LAT: Final = "gla"     # GCJ-02（国内底图用）
F_GCJ_LON: Final = "glo"
F_ACCURACY: Final = "a"      # 精度圈半径，米
F_SRC: Final = "s"           # g = GNSS，l = 基站
F_VOLT: Final = "v"          # 电池电压 V
F_PERCENT: Final = "pct"     # 电量百分比
F_GEOFENCE: Final = "gf"     # in / out / null
F_LAST_SEEN: Final = "ls"    # 最后一次收到上行的服务端时刻
F_LWT: Final = "lwt"         # 上次断连是否非优雅
F_LOCKED: Final = "lk"       # 锁着吗。null = 未知（没接反馈开关）

MODE_MOVING: Final = "moving"
MODE_PARKED: Final = "parked"

SRC_GNSS: Final = "g"
SRC_LBS: Final = "l"

#: `s` 字段 → 人看得懂的说法
SRC_LABELS: Final = {SRC_GNSS: "卫星定位", SRC_LBS: "基站定位"}

# --- 配置项 -------------------------------------------------------------------

CONF_DEVICE_ID: Final = "device_id"
CONF_COORD_SYSTEM: Final = "coord_system"

COORD_WGS84: Final = "wgs84"
COORD_GCJ02: Final = "gcj02"

#: 默认给 GCJ-02。理由：HA 里装的国内地图卡（高德/腾讯）都是 GCJ-02，
#: 用 WGS84 会整体偏几百米。要用 OSM 底图就在选项里切回 wgs84。
DEFAULT_COORD_SYSTEM: Final = COORD_GCJ02

#: 没收到过 state 时，实体的 `available` 怎么算。
#: MQTT 断了 HA 会自己把实体标成不可用，所以这里只管「连着但没数据」。
ATTR_SOURCE: Final = "source"
ATTR_LAST_SEEN: Final = "last_seen"
ATTR_UNGRACEFUL: Final = "last_disconnect_ungraceful"
ATTR_GEOFENCE: Final = "geofence"
ATTR_COORD_SYSTEM: Final = "coord_system"
