"""落库与状态派生测试。

用真实的 SQLite（临时文件），不 mock —— 这一层的价值全在 SQL 的行为上
（UNIQUE 去重、ORDER BY 顺序），mock 掉就什么都没测。
"""

from __future__ import annotations

import pytest

from ebike_server import contract as ct
from ebike_server.config import DeviceConfig, ServerConfig
from ebike_server.derive import (
    build_state,
    geofence_status,
    state_changed,
    volt_to_pct,
)
from ebike_server.geo import haversine_m, out_of_china, wgs84_to_gcj02
from ebike_server.store import Store


@pytest.fixture
async def store(tmp_path):
    s = Store(str(tmp_path / "t.db"))
    await s.open()
    yield s
    await s.close()


@pytest.fixture
def cfg():
    return ServerConfig(devices=[DeviceConfig(id="bike01", report_interval=900)])


def pt(q: int, lat=31.2, lon=121.4, src="g"):
    return {"q": q, "t_dev": 1000 + q, "src": src, "lat": lat, "lon": lon,
            "acc": 8.0, "speed": None, "heading": None, "sats": 9}


# --- 去重 -------------------------------------------------------------------


async def test_loc_dedup_by_q(store):
    """契约 §5 的 q 掉电不清零，断网补发会重发同一批点。"""
    assert await store.add_loc("bike01", [pt(1), pt(2)], 5000) == 2
    # 重发 q=2 加一个新的 q=3
    assert await store.add_loc("bike01", [pt(2), pt(3)], 5100) == 1
    rows = await store.track("bike01", 0, 9999, 100, 0)
    assert len(rows) == 3


async def test_last_and_prev_loc_ordering(store):
    await store.add_loc("bike01", [pt(1), pt(2), pt(3)], 5000)
    await store.add_loc("bike01", [pt(4)], 6000)
    last = await store.last_loc("bike01")
    prev = await store.prev_loc("bike01")
    assert last["q"] == 4
    # 同一个 t_srv 内按 id 倒序，所以上一条是 q=3
    assert prev["q"] == 3


async def test_devices_are_isolated(store):
    await store.add_loc("bike01", [pt(1)], 5000)
    await store.add_loc("bike02", [pt(1)], 5000)   # 同一个 q，不同设备
    assert len(await store.track("bike01", 0, 9999, 10, 0)) == 1
    assert len(await store.track("bike02", 0, 9999, 10, 0)) == 1


# --- 下行队列（契约 §4.1） ---------------------------------------------------


async def test_downlink_queue_order_preserved(store):
    """密钥轮换必须按序到达，否则设备停在旧密钥上。"""
    for i in range(3):
        await store.enqueue_downlink(f"s-{i}", "bike01", ct.DN_SECRET, b"{}")
    rows = await store.pending_downlinks("bike01")
    assert [r["id"] for r in rows] == ["s-0", "s-1", "s-2"]


async def test_ack_removes_from_queue(store):
    await store.enqueue_downlink("s-1", "bike01", ct.DN_SECRET, b"{}")
    assert await store.mark_acked("s-1", True, None) is True
    assert await store.pending_downlinks("bike01") == []
    # 重复 ack 返回 False，不报错 —— 设备重发 ack 是正常的
    assert await store.mark_acked("s-1", True, None) is False


async def test_ack_unknown_id(store):
    assert await store.mark_acked("nope", True, None) is False


async def test_unacked_survives_resend(store):
    """flush 会重发全部未确认的，因为设备可能漏了中间某条。"""
    await store.enqueue_downlink("c-1", "bike01", ct.DN_CMD, b"{}")
    await store.mark_sent("c-1")
    await store.mark_sent("c-1")
    rows = await store.pending_downlinks("bike01")
    assert len(rows) == 1 and rows[0]["tries"] == 2


# --- 在线判定（契约 §4.2） ---------------------------------------------------


async def test_online_by_timeout_not_lwt(store, cfg):
    """LWT=1 但刚上报过 → 仍然在线。这是省电档的正常状态。"""
    await store.touch("bike01", 10_000)
    await store.set_dev_fields("bike01", lwt=1)
    st = await build_state(store, "bike01", cfg, now=10_060)
    assert st["on"] is True
    assert st["lwt"] == 1


async def test_offline_after_threshold(store, cfg):
    await store.touch("bike01", 10_000)
    # 阈值 = 900*3 + 120 = 2820
    assert cfg.offline_after("bike01") == 2820
    assert (await build_state(store, "bike01", cfg, now=12_800))["on"] is True
    assert (await build_state(store, "bike01", cfg, now=12_900))["on"] is False


async def test_never_seen_is_offline(store, cfg):
    st = await build_state(store, "bike01", cfg, now=10_000)
    assert st["on"] is False and st["ls"] is None


# --- state 内容（契约 §7） ---------------------------------------------------


async def test_state_has_both_coordinate_systems(store, cfg):
    await store.touch("bike01", 10_000)
    await store.add_loc("bike01", [pt(1, 31.230416, 121.473701)], 10_000)
    st = await build_state(store, "bike01", cfg, now=10_000)
    assert st["la"] == 31.230416 and st["lo"] == 121.473701
    # GCJ-02 是新增字段，不覆盖
    assert st["gla"] != st["la"] and st["glo"] != st["lo"]
    assert abs(st["gla"] - st["la"]) < 0.01


async def test_state_accuracy_passed_through(store, cfg):
    """「误差圈可见」的验收依赖这个字段一路透传（DESIGN.md §9.4）。"""
    await store.touch("bike01", 10_000)
    await store.add_loc("bike01", [pt(1)], 10_000)
    st = await build_state(store, "bike01", cfg, now=10_000)
    assert st["a"] == 8.0 and st["s"] == "g"


async def test_state_changed_ignores_timestamp():
    """t 每次都变，直接比 dict 会让每个周期都白发一次 state。"""
    a = {"t": 1, "on": True, "mo": "parked"}
    assert state_changed(None, a) is True
    import json
    assert state_changed(json.dumps(a), {"t": 999, "on": True, "mo": "parked"}) is False
    assert state_changed(json.dumps(a), {"t": 1, "on": False, "mo": "parked"}) is True


# --- 移动判定 ---------------------------------------------------------------


async def test_still_event_wins_over_gps_drift(store, cfg):
    """加速度计说停了就是停了 —— 它比 GPS 可信，尤其在漂移上。"""
    await store.touch("bike01", 10_000)
    await store.add_loc("bike01", [pt(1, 31.2000, 121.4)], 10_000)
    await store.add_loc("bike01", [pt(2, 31.2100, 121.4)], 10_100)  # 位移 1km
    await store.add_event("bike01", {"q": 3, "t_dev": 1, "kind": "still",
                                     "detail": None}, 10_200)
    st = await build_state(store, "bike01", cfg, now=10_200)
    assert st["mo"] == "parked"


async def test_motion_without_displacement_is_parked(store, cfg):
    """被碰了一下但没走 → 不算移动。"""
    await store.touch("bike01", 10_000)
    await store.add_loc("bike01", [pt(1, 31.2, 121.4)], 10_000)
    await store.add_loc("bike01", [pt(2, 31.2, 121.4)], 10_100)
    await store.add_event("bike01", {"q": 3, "t_dev": 1, "kind": "motion",
                                     "detail": {"mg": 180}}, 10_200)
    st = await build_state(store, "bike01", cfg, now=10_200)
    assert st["mo"] == "parked"


async def test_motion_with_displacement_is_moving(store, cfg):
    await store.touch("bike01", 10_000)
    await store.add_loc("bike01", [pt(1, 31.2000, 121.4)], 10_000)
    await store.add_loc("bike01", [pt(2, 31.2100, 121.4)], 10_100)
    await store.add_event("bike01", {"q": 3, "t_dev": 1, "kind": "motion",
                                     "detail": None}, 10_200)
    st = await build_state(store, "bike01", cfg, now=10_200)
    assert st["mo"] == "moving"


# --- 电量与围栏 -------------------------------------------------------------


def test_volt_to_pct_endpoints_and_interp():
    curve = [[42.0, 0.0], [48.0, 30.0], [58.8, 100.0]]
    assert volt_to_pct(40.0, curve) == 0     # 低于最低点钳位
    assert volt_to_pct(60.0, curve) == 100   # 高于最高点钳位
    assert volt_to_pct(48.0, curve) == 30
    assert 30 < volt_to_pct(53.0, curve) < 100
    assert volt_to_pct(None, curve) is None
    assert volt_to_pct(50.0, []) is None


def test_geofence():
    # 上海人民广场，半径 100 m
    fence = [31.2304, 121.4737, 100.0]
    assert geofence_status(31.2304, 121.4737, fence) == "in"
    assert geofence_status(31.2400, 121.4737, fence) == "out"
    assert geofence_status(31.2304, 121.4737, None) is None
    assert geofence_status(None, None, fence) is None


# --- 坐标转换 ---------------------------------------------------------------


def test_gcj02_offset_is_metre_scale():
    lat, lon = 31.230416, 121.473701
    glat, glon = wgs84_to_gcj02(lat, lon)
    # 上海的偏移量在几百米量级
    d = haversine_m(lat, lon, glat, glon)
    assert 100 < d < 1000, f"偏移 {d} m 不合常理"


def test_outside_china_unchanged():
    lat, lon = 51.5074, -0.1278   # 伦敦
    assert out_of_china(lat, lon)
    assert wgs84_to_gcj02(lat, lon) == (lat, lon)


def test_haversine_known_distance():
    # 一个纬度约 111 km
    d = haversine_m(31.0, 121.0, 32.0, 121.0)
    assert 110_000 < d < 112_000


# --- 保留策略 ---------------------------------------------------------------


async def test_prune_respects_retention(store):
    import time
    now = int(time.time())
    await store.add_loc("bike01", [pt(1)], now - 400 * 86400)  # 400 天前
    await store.add_loc("bike01", [pt(2)], now)
    assert await store.prune(365) >= 1
    rows = await store.track("bike01", 0, now + 10, 100, 0)
    assert [r["t_srv"] for r in rows] == [now]


async def test_prune_zero_means_forever(store):
    await store.add_loc("bike01", [pt(1)], 1)
    assert await store.prune(0) == 0
    assert len(await store.track("bike01", 0, 9999, 10, 0)) == 1
