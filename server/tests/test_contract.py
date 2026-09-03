"""契约层测试。

重点在**拒绝**路径：能解析合法报文只说明格式对了，而畸形报文被放过去会
一路污染到数据库和 HA。所以每个字段都有一条对应的拒绝测试。
"""

from __future__ import annotations

import base64
import json
from pathlib import Path

import pytest

from ebike_server import contract as ct

DOC = Path(__file__).resolve().parents[2] / "docs" / "MQTT-CONTRACT.md"


def j(obj) -> bytes:
    return json.dumps(obj, separators=(",", ":")).encode()


# --- topic ------------------------------------------------------------------


def test_topic_roundtrip():
    t = ct.topic("bike01", ct.UP_LOC)
    assert t == "ebike/v1/bike01/up/loc"
    p = ct.parse_topic(t)
    assert p.device_id == "bike01" and p.suffix == "up/loc"


@pytest.mark.parametrize("bad", ["BIKE01", "bike_01", "", "a" * 33, "bike/01"])
def test_bad_device_id_rejected(bad):
    with pytest.raises(ct.ContractError):
        ct.topic(bad, ct.UP_LOC)


@pytest.mark.parametrize("bad", [
    "ebike/v2/bike01/up/loc",      # 版本不对
    "other/v1/bike01/up/loc",      # 前缀不对
    "ebike/v1/bike01/up/unknown",  # 后缀不在闭集
    "ebike/v1/bike01",             # 层级不够
])
def test_bad_topic_rejected(bad):
    with pytest.raises(ct.ContractError):
        ct.parse_topic(bad)


def test_topic_length_within_limit():
    # 最长情况：32 字符 id + 最长后缀
    longest = ct.topic("a" * 32, ct.UP_HELLO)
    assert len(longest.encode()) <= ct.MAX_TOPIC_BYTES
    assert len(longest.encode()) == 50  # 契约 §4 写的数字


def test_ha_subscribes_only_state():
    assert ct.sub_all_state() == "ebike/v1/+/state"


# --- 信封 -------------------------------------------------------------------


def test_t_zero_allowed():
    """契约 §5.6：设备还没从 NITZ 拿到时间时填 0，是合法的。"""
    r = ct.parse_tele(j({"t": 0, "q": 1, "v": 54.2}))
    assert r["t_dev"] == 0


def test_bool_not_accepted_as_int():
    """bool 是 int 的子类，不显式挡会悄悄通过。"""
    with pytest.raises(ct.ContractError):
        ct.parse_tele(j({"t": True, "q": 1}))


def test_missing_envelope_rejected():
    with pytest.raises(ct.ContractError):
        ct.parse_tele(j({"v": 54.2}))


# --- up/loc -----------------------------------------------------------------


def test_loc_single_returns_list():
    """单点也返回 list，调用方只有一条代码路径。"""
    r = ct.parse_loc(j({"t": 1, "q": 2, "s": "g", "la": 31.2, "lo": 121.4, "a": 8}))
    assert isinstance(r, list) and len(r) == 1
    assert r[0]["src"] == "g" and r[0]["lat"] == 31.2


def test_loc_batch():
    pts = [{"t": i, "q": i, "s": "g", "la": 31.2, "lo": 121.4} for i in range(20)]
    r = ct.parse_loc(j(pts))
    assert len(r) == 20


def test_loc_batch_over_limit_is_rejected_not_truncated():
    """契约 §5.2：硬拒。静默截断会让丢点看起来像没丢点。"""
    pts = [{"t": i, "q": i, "s": "g", "la": 31.2, "lo": 121.4} for i in range(21)]
    with pytest.raises(ct.ContractError, match="20"):
        ct.parse_loc(j(pts))


def test_loc_batch_fits_packet_limit():
    """契约 §5.2 的算术：20 点 HEX 后必须还在 4100 字节内。"""
    pts = [{"t": 1788105895 + i * 300, "q": 1000 + i, "s": "g",
            "la": 31.230416, "lo": 121.473701, "a": 8.0, "sp": 5.5,
            "hd": 180, "n": 9} for i in range(ct.MAX_BATCH_POINTS)]
    raw = j(pts)
    # HEX 模式（AT+MQTTMODE=1）让字节数翻倍
    assert len(raw) * 2 <= ct.MAX_PACKET_BYTES, f"HEX 后 {len(raw)*2} 字节，超限"


def test_empty_batch_rejected():
    with pytest.raises(ct.ContractError):
        ct.parse_loc(b"[]")


@pytest.mark.parametrize("bad", [
    {"t": 1, "q": 2, "s": "x", "la": 31.2, "lo": 121.4},     # 未知定位源
    {"t": 1, "q": 2, "s": "g", "la": 91.0, "lo": 121.4},     # 纬度越界
    {"t": 1, "q": 2, "s": "g", "la": 31.2, "lo": 181.0},     # 经度越界
    {"t": 1, "q": 2, "s": "g", "la": 31.2},                  # 缺经度
    {"t": 1, "q": 2, "s": "g", "la": "31.2", "lo": 121.4},   # 字符串坐标
])
def test_bad_loc_rejected(bad):
    with pytest.raises(ct.ContractError):
        ct.parse_loc(j(bad))


def test_lbs_large_accuracy_accepted():
    """LBS 没命中基站库时精度圈会很大，拒收它等于丢掉「我还活着」这条信息。"""
    r = ct.parse_loc(j({"t": 1, "q": 2, "s": "l", "la": 31.2, "lo": 121.4,
                        "a": 20000}))
    assert r[0]["acc"] == 20000


def test_oversize_payload_rejected_before_json():
    big = b'{"t":1,"q":2,"pad":"' + b"x" * ct.MAX_PACKET_BYTES + b'"}'
    with pytest.raises(ct.ContractError, match="超过契约上限"):
        ct.parse_loc(big)


# --- up/event ---------------------------------------------------------------


def test_event_closed_set():
    r = ct.parse_event(j({"t": 1, "q": 3, "e": "motion", "d": {"mg": 180}}))
    assert r["kind"] == "motion" and r["detail"] == {"mg": 180}


def test_unknown_event_rejected():
    with pytest.raises(ct.ContractError):
        ct.parse_event(j({"t": 1, "q": 3, "e": "exploded"}))


def test_all_documented_events_parse():
    for kind in ct.EVENT_KINDS:
        ct.parse_event(j({"t": 1, "q": 3, "e": kind}))


def test_all_documented_detail_shapes_parse():
    """契约 §5.4 那张表里每种 `d` 的形状都必须能过。"""
    ok = [
        {"e": "motion", "d": {"mg": 180}},
        {"e": "unlock_ok", "d": {"uid": 1}},
        {"e": "unlock_deny", "d": {"uid": 0}},
        {"e": "lock_state", "d": {"locked": True}},
        {"e": "lock_state", "d": {"locked": False}},
        {"e": "lowbatt", "d": {"lv": 1, "v": 48.1}},
        {"e": "lowbatt", "d": {"lv": 3, "v": 41.9}},   # 固件确实会发 lv=3
        {"e": "ble_err", "d": {"c": -22}},
        {"e": "boot"},                                  # 无 d
    ]
    for body in ok:
        ct.parse_event(j({"t": 1, "q": 3, **body}))


@pytest.mark.parametrize("bad_detail", [
    # 审计 M4：字符串值曾经能一路进库、再进网页的 innerHTML（存储型 XSS）
    {"mg": "<img src=x onerror=alert(1)>"},
    {"uid": '"><svg onload=alert(2)>'},
    {"v": "46.2"},                # 字符串数字也不行
    {"mg": True},                 # bool 不是数字（bool 是 int 子类，要显式挡）
    {"locked": 1},                # locked 必须是布尔，不是 0/1
    {"lv": 9},                    # 越界：DESIGN.md §6 只有 0~3
    {"mg": -1},                   # 幅度不能是负的
    {"v": 999.9},                 # 电压上限同 up/tele
    {"evil": 1},                  # 未知键
    {"mg": 180, "evil": "x"},     # 混进未知键也拒
])
def test_bad_event_detail_rejected(bad_detail):
    with pytest.raises(ct.ContractError):
        ct.parse_event(j({"t": 1, "q": 3, "e": "motion", "d": bad_detail}))


def test_event_detail_keys_match_documented_set():
    """`d` 的键闭集必须和文档 §5.4 那张表一致。"""
    text = DOC.read_text(encoding="utf-8")
    for key in ct._EVENT_DETAIL_SPEC:
        assert f'"{key}"' in text, f"文档 §5.4 里没有 d.{key}"


# --- up/ack -----------------------------------------------------------------


def test_ack():
    r = ct.parse_ack(j({"t": 1, "q": 4, "id": "c-17", "ok": 1}))
    assert r["dn_id"] == "c-17" and r["ok"] is True
    r = ct.parse_ack(j({"t": 1, "q": 5, "id": "s-3", "ok": 0, "er": "badmac"}))
    assert r["ok"] is False and r["err"] == "badmac"


# --- 下行 -------------------------------------------------------------------


def test_build_cmd():
    raw = ct.build_cmd("c-1", "locate", {"to": 60})
    assert json.loads(raw) == {"id": "c-1", "c": "locate", "a": {"to": 60}}


def test_build_cmd_rejects_unknown():
    with pytest.raises(ct.ContractError):
        ct.build_cmd("c-1", "selfdestruct")


def test_all_documented_commands_build():
    for cmd in ct.COMMANDS:
        ct.build_cmd("c-1", cmd)


#: 合法的 §6.2 密钥：base64 的恰好 32 字节。
KEY32 = base64.b64encode(bytes(range(32))).decode()


def test_build_secret_requires_fields():
    with pytest.raises(ct.ContractError):
        ct.build_secret("s-1", "set", uid=1)          # 缺 kid/k
    with pytest.raises(ct.ContractError):
        ct.build_secret("s-1", "del")                 # 缺 uid
    raw = ct.build_secret("s-1", "set", uid=1, kid=8, key_b64=KEY32)
    assert json.loads(raw)["k"] == KEY32
    ct.build_secret("s-1", "wipe")                    # wipe 不需要 uid


def test_build_secret_rejects_wrong_key_length():
    """契约 §6.2：`k` 是 base64 的**恰好 32 字节**（审计 M3）。

    固件 `proto_dec_secret` 强校验 `olen == SECRET_LEN`，短了/长了都会被
    ack 成 `badfmt`，而服务端会一直重发同一条坏报文。在构造时就挡住。
    """
    for bad in (b"", bytes(31), bytes(33), bytes(64)):
        with pytest.raises(ct.ContractError, match="32"):
            ct.build_secret("s-1", "set", uid=1, kid=8,
                            key_b64=base64.b64encode(bad).decode())


def test_build_secret_rejects_non_base64():
    with pytest.raises(ct.ContractError, match="base64"):
        ct.build_secret("s-1", "set", uid=1, kid=8, key_b64="SUPERSECRET!!")


def test_build_secret_rejects_out_of_range_ids():
    """uid 是 uint32、kid 是 uint16（契约 §6.2 / 固件 `struct user_key`）。"""
    with pytest.raises(ct.ContractError, match="kid"):
        ct.build_secret("s-1", "set", uid=1, kid=65536, key_b64=KEY32)
    with pytest.raises(ct.ContractError, match="uid"):
        ct.build_secret("s-1", "set", uid=2**32, kid=1, key_b64=KEY32)
    with pytest.raises(ct.ContractError, match="uid"):
        ct.build_secret("s-1", "del", uid=-1)


def test_downlink_size_cap_is_below_device_line_buffer():
    """审计 M1：下行超过 `MAX_DOWNLINK_BYTES` 在设备侧会被整条丢弃，
    所以服务端构造时就要拒。上限本身必须远小于上行上限。"""
    assert ct.MAX_DOWNLINK_BYTES < ct.MAX_PACKET_BYTES
    # 合法指令 + 超大 args：只有下行上限能拦住它
    with pytest.raises(ct.ContractError, match="下行"):
        ct.build_cmd("c-1", "interval",
                     {"s": 900, "pad": "x" * ct.MAX_DOWNLINK_BYTES})
    # 正常大小的同一条指令必须过
    assert ct.build_cmd("c-1", "interval", {"s": 900})


def test_dumps_is_compact():
    """契约 §5：紧凑 JSON，HEX 模式下每省一字节都是省时间。"""
    assert ct.dumps({"a": 1, "b": 2}) == b'{"a":1,"b":2}'


# --- 脱敏 -------------------------------------------------------------------


def test_redact_nested():
    """密钥可能藏在嵌套结构里（pending 行会被整条塞进诊断响应）。"""
    out = ct.redact({"rows": [{"id": "s-1", "k": "SECRET"}], "k": "TOP"})
    assert out["k"] == "<redacted>"
    assert out["rows"][0]["k"] == "<redacted>"
    assert out["rows"][0]["id"] == "s-1"


# --- 与文档一致性 -----------------------------------------------------------


def test_downlink_never_retained():
    """契约 §4.1：下行 retain 会让连续两次密钥轮换丢掉第一把。"""
    assert ct.DN_RETAIN is False
    assert ct.STATE_RETAIN is True


def test_doc_exists_and_matches_key_constants():
    """文档与代码的关键数字必须一致 —— 改一边不改另一边这条会红。"""
    text = DOC.read_text(encoding="utf-8")
    assert str(ct.MAX_PACKET_BYTES) in text
    assert str(ct.MAX_TOPIC_BYTES) in text
    assert str(ct.MAX_BATCH_POINTS) in text
    for suffix in sorted(ct.UP_SUFFIXES | ct.DN_SUFFIXES):
        assert suffix in text, f"文档里没有 {suffix}"
    for kind in sorted(ct.EVENT_KINDS):
        assert kind in text, f"文档里没有事件 {kind}"
    for cmd in sorted(ct.COMMANDS):
        assert cmd in text, f"文档里没有指令 {cmd}"
