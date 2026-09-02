"""状态派生：把落库的原始报文算成 `state`，就是 HA 唯一订阅的那个 topic。

契约 §7 定义了字段。三条不显然的规则集中在这里：

1. **在线是服务端超时算的，不是 LWT**（契约 §4.2）。模组关机档下设备每个正常
   周期都会非优雅断连一次，拿 LWT 判在线会让好车 99% 时间显示离线。
2. **GCJ-02 是新增字段，不覆盖 WGS84**（契约 §7）。
3. **`mo` 需要事件和位移同时支持**：只看位移会把 GPS 漂移当成移动，
   只看加速度计会把「被撞了一下但没走」当成移动。
"""

from __future__ import annotations

import json
import time
from typing import Any

from .config import DeviceConfig, ServerConfig
from .geo import haversine_m, wgs84_to_gcj02
from .store import Store


def volt_to_pct(volt: float | None, curve: list[list[float]]) -> int | None:
    """电压 → 电量百分比，线性插值。

    这个数字**只是个指示**：铅酸电池的电压在负载下会塌，静置会回弹，
    同一个电压对应的实际剩余容量能差 20%。别拿它做决策，做决策用 §6 的阈值。
    """
    if volt is None or not curve:
        return None
    pts = sorted(curve, key=lambda p: p[0])
    if volt <= pts[0][0]:
        return int(pts[0][1])
    if volt >= pts[-1][0]:
        return int(pts[-1][1])
    for (v0, p0), (v1, p1) in zip(pts, pts[1:]):
        if v0 <= volt <= v1:
            if v1 == v0:
                return int(p0)
            return int(round(p0 + (p1 - p0) * (volt - v0) / (v1 - v0)))
    return None


def geofence_status(lat: float | None, lon: float | None,
                    fence: list[float] | None) -> str | None:
    """`in` / `out` / None（未配围栏）。"""
    if fence is None or lat is None or lon is None or len(fence) != 3:
        return None
    flat, flon, radius = fence
    return "in" if haversine_m(lat, lon, flat, flon) <= radius else "out"


async def derive_mode(store: Store, dev: str, cfg: ServerConfig) -> str | None:
    """`moving` / `parked`。

    判据：最近一条 `motion`/`still` 事件 **AND** 最近两个点的位移。
    两者都要，理由写在模块 docstring 里。
    """
    events = await store.events(dev, limit=20)
    latest_motion = next((e for e in events if e["kind"] in ("motion", "still")), None)

    last = await store.last_loc(dev)
    prev = await store.prev_loc(dev)
    moved = False
    if last and prev:
        moved = haversine_m(last["lat"], last["lon"],
                            prev["lat"], prev["lon"]) > cfg.moving_threshold_m

    if latest_motion is None:
        # 还没有过运动事件：只能靠位移
        return "moving" if moved else "parked"
    if latest_motion["kind"] == "still":
        # 加速度计说停了，就是停了 —— 它比 GPS 可信，尤其在漂移上
        return "parked"
    # 加速度计说动了：如果位置也变了就是真在走，否则可能只是被碰了一下
    return "moving" if moved else "parked"


async def derive_locked(store: Store, dev: str) -> bool | None:
    """锁是否锁着。`None` = 不知道。

    来源是 `lock_state` 事件的 `detail.locked`（契约 §5.4）。
    **`None` 是常态而不是异常**：位置反馈微动开关是选配的
    （`lock.c` 里没接就只 warn），没接就永远拿不到这个事件。
    所以 HA 侧要能显示「未知」，不能把 None 当成「没锁」——
    那会让人以为车没锁好而白跑一趟。
    """
    for e in await store.events(dev, limit=50):
        if e["kind"] == "lock_state" and isinstance(e.get("detail"), dict):
            v = e["detail"].get("locked")
            if isinstance(v, bool):
                return v
    return None


async def build_state(store: Store, dev: str, cfg: ServerConfig,
                      now: int | None = None) -> dict[str, Any]:
    """算出契约 §7 的 state 报文。纯读，不写库。"""
    now = int(time.time()) if now is None else now
    ds = await store.dev_state(dev)
    last = await store.last_loc(dev)
    tele = await store.last_tele(dev)
    dcfg: DeviceConfig | None = cfg.device(dev)

    last_seen = ds.get("last_seen")
    # 契约 §4.2：超时判离线，阈值随上报周期联动
    online = last_seen is not None and (now - last_seen) <= cfg.offline_after(dev)

    state: dict[str, Any] = {
        "t": now,
        "on": online,
        "mo": await derive_mode(store, dev, cfg),
        "ls": last_seen,
        "lwt": int(ds.get("lwt") or 0),
        # 锁状态。None = 没接反馈开关或还没上报过（契约 §7）
        "lk": await derive_locked(store, dev),
    }

    if last:
        state["la"] = round(last["lat"], 6)
        state["lo"] = round(last["lon"], 6)
        glat, glon = wgs84_to_gcj02(last["lat"], last["lon"])
        # 契约 §7：新增字段，不覆盖 —— 换底图不该需要改服务端
        state["gla"] = round(glat, 6)
        state["glo"] = round(glon, 6)
        state["a"] = last["acc"]
        state["s"] = last["src"]
        state["gf"] = geofence_status(last["lat"], last["lon"],
                                      dcfg.geofence if dcfg else None)
    else:
        state["gf"] = None

    if tele:
        state["v"] = tele["volt"]
        state["pct"] = volt_to_pct(tele["volt"],
                                   dcfg.volt_curve if dcfg else [])
    return state


def state_changed(old_json: str | None, new: dict[str, Any]) -> bool:
    """`t` 每次都变，所以不能直接比整个 dict —— 那样每个周期都会重发一次
    完全相同的 state，白占 retain 写入和 HA 的更新。
    """
    if not old_json:
        return True
    try:
        old = json.loads(old_json)
    except json.JSONDecodeError:
        return True
    return {k: v for k, v in old.items() if k != "t"} != \
           {k: v for k, v in new.items() if k != "t"}
