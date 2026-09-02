"""MQTT 契约 v1 的**唯一**实现：topic 构造/解析、报文校验、脱敏。

这个模块是 `docs/MQTT-CONTRACT.md` 的可执行副本，必须与它逐字一致。
`tests/test_contract.py` 会把本文件里的常量和那份文档对照，改一边不改另一边会红。

刻意保持纯函数、无 I/O、无第三方依赖 —— 固件调试时可以单独 import 它来造报文。
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from typing import Any

VERSION = "v1"
PREFIX = f"ebike/{VERSION}"

#: 设备 id。出厂烧录，等于 MQTT 用户名。契约 §4
DEVICE_ID_RE = re.compile(r"^[a-z0-9-]{1,32}$")

#: Air780EP 的单包上限，契约 §1 / DESIGN.md §8.2
MAX_PACKET_BYTES = 4100
#: topic 上限，同上
MAX_TOPIC_BYTES = 256
#: 批量位置点上限。契约 §5.2 的算术：20 × 95B ≈ 1.95KB，HEX 后 3.9KB，留 5% 余量
MAX_BATCH_POINTS = 20

# --- topic 后缀 ---------------------------------------------------------------

UP_HELLO = "up/hello"
UP_LOC = "up/loc"
UP_TELE = "up/tele"
UP_EVENT = "up/event"
UP_ACK = "up/ack"
LWT = "lwt"
DN_CMD = "dn/cmd"
DN_SECRET = "dn/secret"
STATE = "state"

#: 设备可以发布的
UP_SUFFIXES = frozenset({UP_HELLO, UP_LOC, UP_TELE, UP_EVENT, UP_ACK})
#: 服务端可以发布的下行
DN_SUFFIXES = frozenset({DN_CMD, DN_SECRET})

#: retain 策略。**这三个是文档化的常量，发布路径不读它们** ——
#: `service.publish_state` 无条件调 `retain_message`，
#: `flush_downlinks` 走的 `internal_message_broadcast` 天生不设 retain。
#: 留着是因为 `tests/test_contract.py` 拿它们钉住契约里的这三条决定，
#: 改契约时会先在这里红。
#:
#: 下行**一律不 retain**。契约 §4.1：retain 每 topic 只留一条，
#: 连续两次密钥轮换会让第一把永久丢失，而它可能是唯一还能开锁的那把。
DN_RETAIN = False
#: `state` retain —— 「重启 HA 立即有位置」这条验收成立的全部原因。契约 §7
STATE_RETAIN = True
#: LWT 的 retain 由**设备**决定（`AT+MCONFIG` 的 will_retain），服务端不参与。
#: 固件主动发的那条 `{"lwt":0}` 是普通 publish、retain=0，覆盖不了 broker 的
#: retain 表 —— 但端到端仍然对，因为 `_on_lwt` 靠收到消息更新 DB，不靠 retain。
#: 契约 §4 那段说明记录了这个分裂。
LWT_RETAIN = True

#: `up/event` 的 `e` 闭集。契约 §5.4
EVENT_KINDS = frozenset({
    "boot", "motion", "still", "unlock_ok", "unlock_deny",
    "lock_state", "lowbatt", "nfc_err",
})

#: `dn/cmd` 的 `c` 闭集。契约 §6.1
COMMANDS = frozenset({
    "ping", "locate", "unlock", "lock", "interval", "tier", "reboot",
})

#: `dn/secret` 的 `op` 闭集。契约 §6.2
SECRET_OPS = frozenset({"set", "del", "wipe"})

#: 定位源。`"l"` 对应 DESIGN.md §9.5 的 `src=lbs`
SRC_GNSS = "g"
SRC_LBS = "l"
SOURCES = frozenset({SRC_GNSS, SRC_LBS})

#: 日志与 API 响应里必须抹掉的字段。契约 §6.2：`k` 是明文 HMAC secret
SECRET_FIELDS = frozenset({"k"})


class ContractError(ValueError):
    """报文不符合契约。

    刻意用一个异常类型：调用方对「畸形报文」只有一种处理方式（丢弃 + 记日志），
    区分子类型没有用，反而会诱使调用方去 catch 具体类型然后放过一部分。
    """


# --- topic ------------------------------------------------------------------


def check_device_id(device_id: str) -> str:
    if not isinstance(device_id, str) or not DEVICE_ID_RE.match(device_id):
        raise ContractError(f"设备 id 不合契约 §4: {device_id!r}")
    return device_id


def topic(device_id: str, suffix: str) -> str:
    """构造 topic。长度上限在这里兜住，不指望调用方记得。"""
    t = f"{PREFIX}/{check_device_id(device_id)}/{suffix}"
    if len(t.encode()) > MAX_TOPIC_BYTES:
        raise ContractError(f"topic 超过 {MAX_TOPIC_BYTES} 字节: {t}")
    return t


@dataclass(frozen=True)
class ParsedTopic:
    device_id: str
    suffix: str


def parse_topic(t: str) -> ParsedTopic:
    """解析 topic。**不认识的一律抛错**，不返回 None——
    调用方拿到 None 很容易忘了判，抛错则不会被漏掉。
    """
    parts = t.split("/")
    if len(parts) < 4 or parts[0] != "ebike" or parts[1] != VERSION:
        raise ContractError(f"topic 不属于本契约: {t!r}")
    device_id = check_device_id(parts[2])
    suffix = "/".join(parts[3:])
    if suffix not in UP_SUFFIXES | DN_SUFFIXES | {LWT, STATE}:
        raise ContractError(f"未知 topic 后缀: {suffix!r}")
    return ParsedTopic(device_id, suffix)


def sub_all_state() -> str:
    """HA 订阅的**唯一** topic。契约 §4 / DESIGN.md §9.4 的「跟 HA 解耦」。"""
    return f"{PREFIX}/+/{STATE}"


# --- 校验辅助 ----------------------------------------------------------------


def _num(obj: dict[str, Any], key: str, *, lo: float, hi: float,
         required: bool = True, default: float | None = None) -> float | None:
    if key not in obj:
        if required:
            raise ContractError(f"缺字段 {key!r}")
        return default
    v = obj[key]
    # bool 是 int 的子类，`{"a": true}` 会悄悄通过 —— 显式挡掉
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise ContractError(f"{key!r} 必须是数字，收到 {type(v).__name__}")
    if not lo <= v <= hi:
        raise ContractError(f"{key!r}={v} 超出 [{lo}, {hi}]")
    return float(v)


def _int(obj: dict[str, Any], key: str, *, lo: int, hi: int,
         required: bool = True, default: int | None = None) -> int | None:
    if key not in obj:
        if required:
            raise ContractError(f"缺字段 {key!r}")
        return default
    v = obj[key]
    if isinstance(v, bool) or not isinstance(v, int):
        raise ContractError(f"{key!r} 必须是整数，收到 {type(v).__name__}")
    if not lo <= v <= hi:
        raise ContractError(f"{key!r}={v} 超出 [{lo}, {hi}]")
    return v


def _str(obj: dict[str, Any], key: str, *, maxlen: int,
         required: bool = True, default: str | None = None) -> str | None:
    if key not in obj:
        if required:
            raise ContractError(f"缺字段 {key!r}")
        return default
    v = obj[key]
    if not isinstance(v, str):
        raise ContractError(f"{key!r} 必须是字符串，收到 {type(v).__name__}")
    if len(v) > maxlen:
        raise ContractError(f"{key!r} 超过 {maxlen} 字符")
    return v


def _envelope(obj: dict[str, Any]) -> tuple[int, int]:
    """所有上行共有的 `t` / `q`。契约 §5。

    `t` 允许为 0 —— 设备还没从 NITZ 拿到时间时就填 0，明确表示「我不知道几点」
    （契约 §5.6）。所以这里不校验 `t` 的合理性，落库用 `t_srv`。
    """
    t = _int(obj, "t", lo=0, hi=2**31 - 1)
    q = _int(obj, "q", lo=0, hi=2**32 - 1)
    assert t is not None and q is not None
    return t, q


# --- 上行报文 ----------------------------------------------------------------


def load_payload(raw: bytes) -> Any:
    """解 JSON。**先挡长度再解析** —— 4100 字节是设备侧的物理上限（§1），
    比这大的包不可能是我们的设备发的，没必要花 CPU 去解析它。
    """
    if len(raw) > MAX_PACKET_BYTES:
        raise ContractError(f"payload {len(raw)} 字节，超过契约上限 {MAX_PACKET_BYTES}")
    try:
        return json.loads(raw)
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        raise ContractError(f"payload 不是合法 JSON: {e}") from e


def _obj(v: Any) -> dict[str, Any]:
    if not isinstance(v, dict):
        raise ContractError(f"报文必须是 JSON 对象，收到 {type(v).__name__}")
    return v


def parse_hello(raw: bytes) -> dict[str, Any]:
    """契约 §5.1。`kid` 让服务端知道设备手上是哪一代密钥集，据此决定要不要补发。"""
    o = _obj(load_payload(raw))
    t, q = _envelope(o)
    return {
        "t_dev": t, "q": q,
        "fw": _str(o, "fw", maxlen=16, required=False),
        "boot": _int(o, "boot", lo=0, hi=2**32 - 1, required=False),
        "rst": _str(o, "rst", maxlen=8, required=False),
        "kid": _int(o, "kid", lo=0, hi=65535, required=False, default=0),
    }


def _one_loc(o: dict[str, Any]) -> dict[str, Any]:
    t, q = _envelope(o)
    src = _str(o, "s", maxlen=1)
    if src not in SOURCES:
        raise ContractError(f"定位源 s={src!r} 不在 {sorted(SOURCES)}")
    lat = _num(o, "la", lo=-90.0, hi=90.0)
    lon = _num(o, "lo", lo=-180.0, hi=180.0)
    return {
        "t_dev": t, "q": q, "src": src, "lat": lat, "lon": lon,
        # 精度圈上限给到 50 km：LBS 在没有基站数据库命中时会给出很大的圈，
        # 拒收它等于丢掉「我不知道在哪但我还活着」这个信息
        "acc": _num(o, "a", lo=0.0, hi=50_000.0, required=False),
        "speed": _num(o, "sp", lo=0.0, hi=200.0, required=False),
        "heading": _int(o, "hd", lo=0, hi=359, required=False),
        "sats": _int(o, "n", lo=0, hi=64, required=False),
    }


def parse_loc(raw: bytes) -> list[dict[str, Any]]:
    """契约 §5.2。返回**总是** list —— 单点是长度 1 的 list，
    调用方不需要分两条路径处理，也就不会只在一条路径上做去重。
    """
    v = load_payload(raw)
    if isinstance(v, list):
        if not v:
            raise ContractError("批量位置报文不能是空数组")
        if len(v) > MAX_BATCH_POINTS:
            # 硬拒不截断：静默截断会让「丢了点」看起来像「没丢点」
            raise ContractError(
                f"批量 {len(v)} 点，超过契约 §5.2 的上限 {MAX_BATCH_POINTS}")
        return [_one_loc(_obj(item)) for item in v]
    return [_one_loc(_obj(v))]


def parse_tele(raw: bytes) -> dict[str, Any]:
    """契约 §5.3。电压范围按 48V 系统取 0~70V：满充 58.8V，留出余量。"""
    o = _obj(load_payload(raw))
    t, q = _envelope(o)
    return {
        "t_dev": t, "q": q,
        "volt": _num(o, "v", lo=0.0, hi=70.0, required=False),
        "csq": _int(o, "csq", lo=0, hi=99, required=False),
        "uptime": _int(o, "up", lo=0, hi=2**32 - 1, required=False),
        "temp": _int(o, "tmp", lo=-60, hi=125, required=False),
    }


def parse_event(raw: bytes) -> dict[str, Any]:
    """契约 §5.4。`e` 是闭集，未知值拒收。"""
    o = _obj(load_payload(raw))
    t, q = _envelope(o)
    kind = _str(o, "e", maxlen=16)
    if kind not in EVENT_KINDS:
        raise ContractError(f"事件 e={kind!r} 不在 {sorted(EVENT_KINDS)}")
    detail = o.get("d")
    if detail is not None and not isinstance(detail, dict):
        raise ContractError("d 必须是对象或缺省")
    return {"t_dev": t, "q": q, "kind": kind, "detail": detail}


def parse_ack(raw: bytes) -> dict[str, Any]:
    """契约 §5.5。`id` 必须逐字回抄下行的 id，服务端据此销账。"""
    o = _obj(load_payload(raw))
    t, q = _envelope(o)
    ok = _int(o, "ok", lo=0, hi=1)
    return {
        "t_dev": t, "q": q,
        "dn_id": _str(o, "id", maxlen=32),
        "ok": bool(ok),
        "err": _str(o, "er", maxlen=32, required=False),
    }


#: suffix → parser。dispatch 表放在这里而不是 ingest 里，
#: 是为了让「契约支持哪些报文」只有一处答案。
UP_PARSERS = {
    UP_HELLO: parse_hello,
    UP_LOC: parse_loc,
    UP_TELE: parse_tele,
    UP_EVENT: parse_event,
    UP_ACK: parse_ack,
}


# --- 下行报文 ----------------------------------------------------------------


def dumps(obj: Any) -> bytes:
    """紧凑 JSON。分隔符里不留空格 —— 契约 §5 的理由：
    HEX 模式让串口字节翻倍，9600 baud 下省 100 字节 = 省 0.2 秒模组开机时间。
    """
    raw = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode()
    if len(raw) > MAX_PACKET_BYTES:
        raise ContractError(f"下行 {len(raw)} 字节，超过 {MAX_PACKET_BYTES}")
    return raw


def build_cmd(dn_id: str, cmd: str, args: dict[str, Any] | None = None) -> bytes:
    """契约 §6.1。

    注意 `unlock` 在这里**不做权限判断** —— 是否允许远程开锁是配置层的事
    （`allow_remote_unlock`），在 api.py 里判。契约层只管报文合法性。
    """
    if cmd not in COMMANDS:
        raise ContractError(f"未知指令 {cmd!r}，闭集是 {sorted(COMMANDS)}")
    body: dict[str, Any] = {"id": dn_id, "c": cmd}
    if args:
        body["a"] = args
    return dumps(body)


def build_secret(dn_id: str, op: str, *, uid: int | None = None,
                 kid: int | None = None, key_b64: str | None = None) -> bytes:
    """契约 §6.2。**这条报文里有明文密钥材料**，保护完全依赖 TLS。

    `retain=False` 是硬要求（DN_RETAIN），发送方不要自己传 retain。
    """
    if op not in SECRET_OPS:
        raise ContractError(f"未知 op {op!r}，闭集是 {sorted(SECRET_OPS)}")
    body: dict[str, Any] = {"id": dn_id, "op": op}
    if op in ("set", "del"):
        if uid is None:
            raise ContractError(f"op={op} 必须带 uid")
        body["uid"] = uid
    if op == "set":
        if key_b64 is None or kid is None:
            raise ContractError("op=set 必须带 kid 和 k")
        body["kid"] = kid
        body["k"] = key_b64
    return dumps(body)


def redact(obj: Any) -> Any:
    """脱敏。日志与 HTTP 响应经过这里，`k` 字段永远不落到磁盘上。

    递归处理是必要的：`pending_downlink` 的行会被整条塞进诊断响应里。
    """
    if isinstance(obj, dict):
        return {
            k: ("<redacted>" if k in SECRET_FIELDS else redact(v))
            for k, v in obj.items()
        }
    if isinstance(obj, list):
        return [redact(v) for v in obj]
    return obj
