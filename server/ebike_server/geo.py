"""坐标转换与几何。

WGS84 → GCJ-02。契约 §7 定的是**两套都发、不覆盖原字段**，
因为国内底图（高德/腾讯）是 GCJ-02 而 HA 默认底图 OSM 是 WGS84。

算法是公开的 GCJ-02 偏移近似式，误差在米级。BD-09 不做（契约 §8）。
"""

from __future__ import annotations

import math

# GCJ-02 偏移算法的常数
_A = 6378245.0            # 克拉索夫斯基椭球长半轴
_EE = 0.00669342162296594  # 偏心率平方

#: 中国大陆粗略包围盒。境外坐标 GCJ-02 与 WGS84 相同，不做偏移。
_CN_LAT = (0.8293, 55.8271)
_CN_LON = (72.004, 137.8347)


def out_of_china(lat: float, lon: float) -> bool:
    return not (_CN_LON[0] <= lon <= _CN_LON[1] and _CN_LAT[0] <= lat <= _CN_LAT[1])


def _transform_lat(x: float, y: float) -> float:
    ret = (-100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y
           + 0.2 * math.sqrt(abs(x)))
    ret += (20.0 * math.sin(6.0 * x * math.pi)
            + 20.0 * math.sin(2.0 * x * math.pi)) * 2.0 / 3.0
    ret += (20.0 * math.sin(y * math.pi)
            + 40.0 * math.sin(y / 3.0 * math.pi)) * 2.0 / 3.0
    ret += (160.0 * math.sin(y / 12.0 * math.pi)
            + 320.0 * math.sin(y * math.pi / 30.0)) * 2.0 / 3.0
    return ret


def _transform_lon(x: float, y: float) -> float:
    ret = (300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y
           + 0.1 * math.sqrt(abs(x)))
    ret += (20.0 * math.sin(6.0 * x * math.pi)
            + 20.0 * math.sin(2.0 * x * math.pi)) * 2.0 / 3.0
    ret += (20.0 * math.sin(x * math.pi)
            + 40.0 * math.sin(x / 3.0 * math.pi)) * 2.0 / 3.0
    ret += (150.0 * math.sin(x / 12.0 * math.pi)
            + 300.0 * math.sin(x / 30.0 * math.pi)) * 2.0 / 3.0
    return ret


def wgs84_to_gcj02(lat: float, lon: float) -> tuple[float, float]:
    """WGS84 → GCJ-02。境外原样返回。"""
    if out_of_china(lat, lon):
        return lat, lon
    dlat = _transform_lat(lon - 105.0, lat - 35.0)
    dlon = _transform_lon(lon - 105.0, lat - 35.0)
    rad = lat / 180.0 * math.pi
    magic = math.sin(rad)
    magic = 1 - _EE * magic * magic
    sqrt_magic = math.sqrt(magic)
    dlat = (dlat * 180.0) / ((_A * (1 - _EE)) / (magic * sqrt_magic) * math.pi)
    dlon = (dlon * 180.0) / (_A / sqrt_magic * math.cos(rad) * math.pi)
    return lat + dlat, lon + dlon


def haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """两点距离，米。用来判「是不是真的动了」和地理围栏。"""
    r = 6371008.8  # 地球平均半径
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = p2 - p1
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))
