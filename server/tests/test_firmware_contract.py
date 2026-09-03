"""固件与服务端的契约一致性。

固件（C）在这台机器上**编译不了**（没有 NCS SDK，也没有 C 编译器），
所以这些测试是**文本层面**的交叉检查：把 firmware/nrf52840 里的常量和 topic
字符串抠出来，和 server 的 contract.py 对照。

抠不到编译错误，但抠得到「改了一边忘了另一边」—— 那是这两份代码最可能
出现的分歧，而且是运行时才暴露、隔着 9600 baud 极难诊断的那种。
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from ebike_server import contract as ct

FW = Path(__file__).resolve().parents[2] / "firmware" / "nrf52840"
PROTO_H = FW / "src" / "proto.h"
PROTO_C = FW / "src" / "proto.c"
KCONFIG = FW / "Kconfig"


@pytest.fixture(scope="module")
def proto_h() -> str:
    if not PROTO_H.exists():
        pytest.skip(f"没有固件源码 {PROTO_H}")
    return PROTO_H.read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def proto_c() -> str:
    return PROTO_C.read_text(encoding="utf-8")


def define_str(text: str, name: str) -> str | None:
    m = re.search(rf'#define\s+{name}\s+"([^"]*)"', text)
    return m.group(1) if m else None


def define_int(text: str, name: str) -> int | None:
    m = re.search(rf"#define\s+{name}\s+(\d+)", text)
    return int(m.group(1)) if m else None


# --- topic 前缀与后缀 --------------------------------------------------------


def test_prefix_matches(proto_h):
    assert define_str(proto_h, "PROTO_PREFIX") == ct.PREFIX


@pytest.mark.parametrize("macro,suffix", [
    ("TOPIC_UP_HELLO", ct.UP_HELLO),
    ("TOPIC_UP_LOC", ct.UP_LOC),
    ("TOPIC_UP_TELE", ct.UP_TELE),
    ("TOPIC_UP_EVENT", ct.UP_EVENT),
    ("TOPIC_UP_ACK", ct.UP_ACK),
    ("TOPIC_LWT", ct.LWT),
    ("TOPIC_DN_CMD", ct.DN_CMD),
    ("TOPIC_DN_SECRET", ct.DN_SECRET),
])
def test_topic_suffixes_match(proto_h, macro, suffix):
    """固件的 topic 宏是拼接式的，所以只检查后缀部分出现在定义里。"""
    m = re.search(rf'#define\s+{macro}\s+(.+)', proto_h)
    assert m, f"固件里没有 {macro}"
    assert f'/{suffix}"' in m.group(1), \
        f"{macro} 的后缀与服务端的 {suffix!r} 不一致：{m.group(1)}"


def test_device_never_publishes_state(proto_h):
    """契约 §3：设备不能发 state。固件里就不该有那个 topic 宏。"""
    assert "TOPIC_STATE" not in proto_h
    assert '/state"' not in proto_h


# --- 数值常量 ---------------------------------------------------------------


def test_batch_limit_matches(proto_h):
    assert define_int(proto_h, "PROTO_MAX_BATCH") == ct.MAX_BATCH_POINTS


def test_firmware_payload_limit_is_below_contract_limit(proto_h):
    """固件留了余量给 AT 命令头，所以它的上限应该**小于**契约上限。"""
    fw_max = define_int(proto_h, "PROTO_MAX_PAYLOAD")
    assert fw_max is not None
    assert fw_max <= ct.MAX_PACKET_BYTES, "固件缓冲比契约上限还大，会被服务端拒"
    assert fw_max >= ct.MAX_PACKET_BYTES * 0.9, \
        "固件上限比契约小太多，20 点批量可能放不下"


def test_downlink_limit_matches_server(proto_h):
    """审计 M1：下行上限两边必须是同一个数字。

    服务端 `MAX_DOWNLINK_BYTES` 挡住构造，固件 `PROTO_MAX_DN_PAYLOAD` 决定
    解析缓冲。不一致的一侧会静默丢报文（固件小）或让坏报文入队（服务端大）。
    """
    assert define_int(proto_h, "PROTO_MAX_DN_PAYLOAD") == ct.MAX_DOWNLINK_BYTES


def test_line_buffer_fits_largest_downlink(proto_h, modem_c):
    """审计 M1：`+MSUB: "<topic>",<len>,"<hex>"` 整条在**一行**里到达，
    HEX 让 payload 翻倍 —— LINE_MAX 必须装得下最大下行，否则整条被丢。

    topic 的实际上界由 `DEVICE_ID_RE` 定（≤32 字符），不是 `MAX_TOPIC_BYTES`
    那个宽松的门限值。
    """
    line_max = define_int(modem_c, "LINE_MAX")
    dn_max = define_int(proto_h, "PROTO_MAX_DN_PAYLOAD")
    assert line_max is not None and dn_max is not None
    # 最长的下行 topic：设备 id 顶到 32 字符 + 最长后缀
    longest_topic = max(len(ct.topic("x" * 32, s)) for s in ct.DN_SUFFIXES)
    # `+MSUB: "<topic>",<len>,"<hex>"` 的框架字符（引号、逗号、长度数字）
    frame = len('+MSUB: "",9999,""')
    need = dn_max * 2 + longest_topic + frame
    assert line_max >= need, (
        f"LINE_MAX={line_max} 装不下最大下行：{dn_max} 字节 HEX 后 "
        f"{dn_max * 2} + topic {longest_topic} + 框架 {frame} = {need}")


def test_read_line_reports_overflow_instead_of_truncating(modem_c):
    """审计 M1：超长行**不能静默截断**。

    截断后的 HEX 仍是合法偶数长度，`hex_decode` 会成功、`get_int` 会把
    `"s":900` 截成的 `"s":90` 正常解析 —— 上报周期静默改错 10 倍。
    """
    m = re.search(r"static int read_line\([^)]*\)\s*\{(.*?)\n\}", modem_c, re.S)
    assert m, "找不到 read_line"
    body = m.group(1)
    assert "EMSGSIZE" in body, "read_line 不报超长，可能又在静默截断"
    assert "overflow" in body, "没有 overflow 标记，截断行会被当成正常行返回"


def test_dn_payload_buffer_sized_for_downlink_not_uplink(modem_c):
    """下行 payload 缓冲按下行上限开。按 PROTO_MAX_PAYLOAD(3900) 开是矛盾的：
    行缓冲装不下那么大的 HEX，多出来的空间只会掩盖截断（审计 M1）。

    审计 R1 之后它是 `struct dn_msg` 的成员（一个小队列，不再是单缓冲）。
    """
    m = re.search(r"uint8_t payload\[(\w+)\];", modem_c)
    assert m, "找不到下行 payload 缓冲的定义"
    assert m.group(1) == "PROTO_MAX_DN_PAYLOAD", \
        f"下行 payload 按 {m.group(1)} 开，应该按 PROTO_MAX_DN_PAYLOAD"


def test_downlink_is_queued_not_dispatched_inline(modem_c):
    """审计 R1：`handle_msub` 只入队，`dn_cb` 只在 `deliver_downlinks` 里调。

    就地调 `dn_cb` 会重入 `modem_publish`（`consume_urc` 的两个调用点都持
    `at_lock`）：static `hexbuf` 被内层覆盖 → 发出错误字节；递归无界 →
    第 5 层爆 4 KB 的 uplink 线程栈 → `arch_system_halt()` 死转（没开
    `RESET_ON_FATAL_ERROR`、没有看门狗）= 车变砖。

    文本层钉住「`dn_cb(` 只出现在一个函数里」。运行时行为由
    `firmware/tests/modem_downlink` 的三条 ztest 钉住（含变异体验证）。
    """
    # 除了声明和赋值，`dn_cb(` 的调用点必须只有一处
    calls = re.findall(r"^\t+dn_cb\(", modem_c, re.MULTILINE)
    assert len(calls) == 1, \
        f"`dn_cb(` 有 {len(calls)} 个调用点 —— 只允许 deliver_downlinks 一处"

    # 那一处必须在 deliver_downlinks 里，而不是 handle_msub 里
    deliver = re.search(r"static void deliver_downlinks\(void\)\s*\{(.*?)\n\}",
                        modem_c, re.DOTALL)
    assert deliver, "找不到 deliver_downlinks —— R1 的投递点没了？"
    assert "dn_cb(" in deliver.group(1), "deliver_downlinks 里没有投递"

    msub = re.search(r"static bool handle_msub\(const char \*line\)\s*\{(.*?)\n\}",
                     modem_c, re.DOTALL)
    assert msub, "找不到 handle_msub"
    assert "dn_cb(" not in msub.group(1), \
        "handle_msub 又在就地投递下行了 —— 那条路径会重入 modem_publish（R1）"


def test_downlink_delivery_is_outside_at_lock(modem_c):
    """审计 R1 的另一半：投递点必须在 `modem_poll` 里，而不在任何
    持 `at_lock` 的函数里。

    `at_cmd_expect` 和 `modem_publish` 都是「拿锁 → 循环读行 →
    `consume_urc`」的形状；只要 `consume_urc` 不投递，它们就安全。
    """
    poll = re.search(r"int modem_poll\(uint32_t timeout_ms\)\s*\{(.*?)\n\}",
                     modem_c, re.DOTALL)
    assert poll, "找不到 modem_poll"
    assert "deliver_downlinks()" in poll.group(1), \
        "modem_poll 不投递下行了 —— 那下行永远发不上去"

    for fn in ("at_cmd_expect", "modem_publish"):
        body = re.search(rf"\b{fn}\((.*?)\n\}}", modem_c, re.DOTALL)
        assert body, f"找不到 {fn}"
        assert "deliver_downlinks()" not in body.group(1), \
            f"{fn} 持 at_lock 时投递下行 —— 会重入 modem_publish（R1）"


# --- 闭集 -------------------------------------------------------------------


def test_event_names_match(proto_c):
    """契约 §5.4 的事件闭集。固件的 event_names[] 是上报时用的字符串。"""
    block = re.search(r"event_names\[\]\s*=\s*\{(.*?)\n\};", proto_c, re.S)
    assert block, "找不到 event_names[] 定义"
    names = set(re.findall(r'=\s*"([a-z_]+)"', block.group(1)))
    assert names == ct.EVENT_KINDS, (
        f"事件闭集不一致\n固件独有: {names - ct.EVENT_KINDS}\n"
        f"服务端独有: {ct.EVENT_KINDS - names}")


def test_command_names_match(proto_c):
    """契约 §6.1 的指令闭集。固件的 table[] 是解析下行时用的。"""
    block = re.search(r'\{\s*"ping".*?\n\t\};', proto_c, re.S)
    assert block, "找不到指令表定义"
    names = set(re.findall(r'\{\s*"([a-z]+)"\s*,', block.group(0)))
    assert names == ct.COMMANDS, (
        f"指令闭集不一致\n固件独有: {names - ct.COMMANDS}\n"
        f"服务端独有: {ct.COMMANDS - names}")


def test_secret_ops_match(proto_c):
    """契约 §6.2 的 op 闭集。固件在 proto_dec_secret 里比较字符串。"""
    # `set` 在 proto_dec_secret 里判，`del`/`wipe` 在 uplink.c 里判
    uplink = (FW / "src" / "uplink.c").read_text(encoding="utf-8")
    found = set(re.findall(r's\.op,\s*"([a-z]+)"', proto_c + uplink))
    assert found == ct.SECRET_OPS, (
        f"密钥操作闭集不一致：固件 {found}，服务端 {ct.SECRET_OPS}")


# --- 报文字段名 -------------------------------------------------------------


@pytest.mark.parametrize("field", ["t", "q", "s", "la", "lo", "a", "sp", "hd", "n"])
def test_loc_field_names_in_firmware(proto_c, field):
    """位置报文的字段名。拼错了服务端会拒收整条报文。"""
    assert f'\\"{field}\\":' in proto_c, f"固件的位置报文里没有字段 {field!r}"


@pytest.mark.parametrize("field", ["fw", "boot", "rst", "kid"])
def test_hello_field_names(proto_c, field):
    assert f'\\"{field}\\":' in proto_c


@pytest.mark.parametrize("field", ["v", "csq", "up", "tmp"])
def test_tele_field_names(proto_c, field):
    """契约 §5.3。`tmp` 的编码路径仍然在（有真值时会发），
    只是当前固件不采温度所以恒缺席 —— 见 test_tele_temp_is_optional。"""
    assert f'\\"{field}\\":' in proto_c


def test_tele_temp_is_optional(proto_c):
    """契约 §5.3 说 `tmp` 可省。**不能用假值占位。**

    固件曾经硬编码 `.temp = 0`，服务端会把它当真值落库 ——
    图上出现一条 0 °C 的直线，而没人知道它是假的。
    现在靠 `has_temp` 决定发不发，缺省时整个字段不出现。
    """
    assert "has_temp" in proto_c, "tmp 没有可选开关，可能又在发假值"
    m = re.search(r"if \(t->has_temp\)", proto_c)
    assert m, "tmp 不是条件发送的"

    uplink = (FW / "src" / "uplink.c").read_text(encoding="utf-8")
    assert re.search(r"\.has_temp\s*=\s*false", uplink), \
        "uplink 没有显式声明不报温度"
    assert not re.search(r"\.temp\s*=\s*0\s*,", uplink), \
        "uplink 又在用 .temp = 0 当占位值"


def test_tele_volt_is_optional_not_fake_zero(proto_c):
    """契约 §5.3：`v` 也可省。**ADC 读失败不能发 0.0**（审计 M8）。

    固件曾经写 `.volt = mv > 0 ? mv/1000 : 0.0f` 并无条件编进报文 ——
    服务端把 0.0 当真值落库、`volt_to_pct` 插值到曲线最低点，HA 上
    显示 0V/0%，看起来像被剪线。这和 `tmp` 是同一个原则，同一个修法。
    """
    assert "has_volt" in proto_c, "v 没有可选开关，可能又在发假 0.0"
    assert re.search(r"if \(t->has_volt\)", proto_c), "v 不是条件发送的"

    uplink = (FW / "src" / "uplink.c").read_text(encoding="utf-8")
    assert re.search(r"\.has_volt\s*=\s*mv > 0", uplink), \
        "uplink 没把 has_volt 绑到 ADC 读数有效性上"


def test_lbs_rejects_null_island_and_out_of_range():
    """审计 M2：`+CIPGSMLOC` 字段为空时 strtod 返回 0.0，
    (0,0)（几内亚湾）落在服务端的 ±90/±180 校验内，拦不住 ——
    固件侧必须自己判。GNSS 路径本来就有同款防护（gnss.c 的 valid）。
    """
    modem = (FW / "src" / "modem.c").read_text(encoding="utf-8")
    m = re.search(r"int modem_lbs\(.*?\n\}", modem, re.S)
    assert m, "找不到 modem_lbs"
    body = m.group(0)
    assert "lat_v == 0.0 && lon_v == 0.0" in body, \
        "modem_lbs 没挡 (0,0) —— 坏响应会变成「车在几内亚湾」"
    assert "-90.0" in body and "180.0" in body, "modem_lbs 没做范围校验"


def test_ack_fields(proto_c):
    for field in ("id", "ok", "er"):
        assert f'\\"{field}\\":' in proto_c


def test_lwt_payload_shape(proto_c):
    """契约 §4.2：lwt payload 是 {"lwt":0|1}。"""
    assert '\\"lwt\\":' in proto_c


# --- 配置一致性 -------------------------------------------------------------


def test_report_interval_defaults_agree():
    """固件的默认上报周期决定服务端的离线阈值（契约 §4.2），
    两边默认值不一致会让新装的设备一开始就显示离线。"""
    text = KCONFIG.read_text(encoding="utf-8")
    m = re.search(r"config EBIKE_REPORT_INTERVAL.*?default (\d+)", text, re.S)
    assert m, "Kconfig 里没有 EBIKE_REPORT_INTERVAL 默认值"
    from ebike_server.config import DeviceConfig
    assert int(m.group(1)) == DeviceConfig().report_interval


def test_remote_unlock_off_on_both_sides():
    """契约 §6.1：两边都必须默认关。只关一边等于没关。"""
    text = KCONFIG.read_text(encoding="utf-8")
    m = re.search(r"config EBIKE_ALLOW_REMOTE_UNLOCK.*?default (\w+)", text, re.S)
    assert m and m.group(1) == "n", "固件的远程开锁默认没关"
    from ebike_server.config import ServerConfig
    assert ServerConfig().allow_remote_unlock is False


def test_device_id_default_matches():
    text = KCONFIG.read_text(encoding="utf-8")
    m = re.search(r'config EBIKE_DEVICE_ID.*?default "([^"]+)"', text, re.S)
    from ebike_server.config import DeviceConfig
    assert m and m.group(1) == DeviceConfig().id


def test_motion_threshold_within_sensor_resolution():
    """LIS2DW12 一格 31.25 mg @±2 g，低于一格会持续触发（DESIGN.md §3.7）。"""
    text = KCONFIG.read_text(encoding="utf-8")
    m = re.search(r"config EBIKE_MOTION_THRESHOLD_MG.*?default (\d+)", text, re.S)
    assert m
    mg = int(m.group(1))
    assert 31 <= mg <= 2000
    # DESIGN.md §3.7 建议从 100~200 mg 起调
    assert 100 <= mg <= 200, f"默认阈值 {mg} mg 不在建议的 100~200 区间"


def test_low_volt_thresholds_ordered():
    """第二阈值必须比第一低，否则 battery_low_level 的判断顺序会出错。"""
    text = KCONFIG.read_text(encoding="utf-8")
    v1 = int(re.search(r"config EBIKE_LOW_VOLT_1.*?default (\d+)", text, re.S).group(1))
    v2 = int(re.search(r"config EBIKE_LOW_VOLT_2.*?default (\d+)", text, re.S).group(1))
    assert v2 < v1, f"第二阈值 {v2} 不低于第一阈值 {v1}"
    # 48V 系统：满充 58.8V，BMS 截止通常在 40~42V
    assert 38000 < v2 < v1 < 52000


def test_low_volt_three_thresholds_ordered():
    """DESIGN.md §6 是**四级**兜底，需要三个阈值。

    顺序必须 v3 < v2 < v1，否则 battery_low_level 的判断链会短路 ——
    比如 v3 > v2 时第 2 级永远不会生效。
    """
    text = KCONFIG.read_text(encoding="utf-8")
    v = {}
    for n in (1, 2, 3):
        m = re.search(rf"config EBIKE_LOW_VOLT_{n}.*?default (\d+)", text, re.S)
        assert m, f"Kconfig 里没有 EBIKE_LOW_VOLT_{n}"
        v[n] = int(m.group(1))
    assert v[3] < v[2] < v[1], f"阈值顺序不对：{v}"
    # 48V 铅酸的物理底线是 12 节 × 1.75 V = 42 V
    assert 38000 <= v[3] < 52000


def test_battery_implements_four_levels():
    """§6 承诺四级，代码就得有四级（0/1/2/3）。
    只做两级又不标注，是「看着做完了其实是半的」。"""
    src = (FW / "src" / "battery.c").read_text(encoding="utf-8")
    for lvl, cfg in ((3, "LOW_VOLT_3"), (2, "LOW_VOLT_2"), (1, "LOW_VOLT_1")):
        assert f"CONFIG_EBIKE_{cfg}" in src, f"battery.c 没用 {cfg}"
        assert re.search(rf"return {lvl};", src), f"battery.c 不会返回等级 {lvl}"


def test_reset_cause_maps_to_contract_closed_set():
    """契约 §5.1 的 `rst` 是 por/pin/wdt/soft/off 五值。

    固件曾经硬编码 "por"（假数据），现在走 hwinfo。两件必须做对的事：
    1. `cause == 0` 才是 POR —— nRF 驱动对上电复位什么位都不置，
       而 nRF52840 的 `RESET_POR` 只在 USB VBUS 唤醒时出现，语义是错配的。
    2. 读完必须清 —— RESETREAS 是累积寄存器，不清的话按过一次 RESET 键
       就永远带着 RESET_PIN，`cause == 0` 这个 POR 判据从此不成立。
    """
    src = (FW / "src" / "uplink.c").read_text(encoding="utf-8")
    assert "hwinfo_get_reset_cause" in src, "没读复位原因"
    assert "hwinfo_clear_reset_cause" in src, "读完没清 RESETREAS"
    assert "cause == 0" in src, "没有把 cause==0 当作 POR"
    assert "RESET_LOW_POWER_WAKE" in src, "没有把 System OFF 唤醒映射成 off"

    for value in ("por", "pin", "wdt", "soft", "off"):
        assert f'"{value}"' in src, f"rst 闭集里缺 {value!r}"
    # 不能再有那个硬编码的占位
    assert not re.search(r'nvstore_boot_count\(\),\s*\n\s*"por"', src), \
        "hello 还在硬编码 rst=\"por\""


def test_system_off_uses_level_sense_not_edge():
    """GPIOTE 在 System OFF 下断电，边沿触发唤不醒芯片 ——
    只有 GPIO 的 SENSE→DETECT 能，而 Zephyr 只在 level 模式下写 SENSE。

    LIS2DW12 驱动配的是 `GPIO_INT_EDGE_TO_ACTIVE`，所以进 System OFF 前
    必须由应用层重配成 level。这条防的是「关机了但再也醒不过来」。
    """
    src = (FW / "src" / "main.c").read_text(encoding="utf-8")
    assert "sys_poweroff" in src, "没有进 System OFF 的路径"
    assert "GPIO_INT_LEVEL_ACTIVE" in src, \
        "唤醒脚没配 level sense —— System OFF 之后醒不过来"
    assert "PM_DEVICE_ACTION_SUSPEND" in src, \
        "没 suspend EasyDMA 外设（引脚会继续漏电）"


def test_prj_conf_has_no_nonexistent_symbols():
    """两个曾经写在 prj.conf 里的符号是不存在的，Kconfig 会 warn，
    而 Zephyr 把这类 warn 升级成 error：

    - `CONFIG_PM`：nRF52 的 SoC Kconfig 只 select HAS_POWEROFF，没有 HAS_PM
    - `CONFIG_GPIO_NRFX_INTERRUPT_DETECT_MODE_PORT`：整棵树里没有这个符号
      （`drivers/gpio/Kconfig.nrfx` 只有 GPIO_NRFX 和 GPIO_NRFX_INTERRUPT）
    """
    text = (FW / "prj.conf").read_text(encoding="utf-8")
    live = [ln.strip() for ln in text.splitlines()
            if ln.strip() and not ln.strip().startswith("#")]
    assert not [ln for ln in live if ln.startswith("CONFIG_PM=")], \
        "prj.conf 又有 CONFIG_PM=（nRF52 没有 HAS_PM）"
    assert not [ln for ln in live if "DETECT_MODE" in ln], \
        "prj.conf 又有那个不存在的 DETECT_MODE 符号"
    # 该有的两个
    assert "CONFIG_POWEROFF=y" in live
    assert "CONFIG_PM_DEVICE=y" in live


# --- BLE 开锁通道（ADR-004） -------------------------------------------------


@pytest.fixture(scope="module")
def prj_conf() -> str:
    return (FW / "prj.conf").read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def ble_c() -> str:
    p = FW / "src" / "ble_unlock.c"
    if not p.exists():
        pytest.skip(f"没有 {p}")
    return p.read_text(encoding="utf-8")


def kconf_int(text: str, name: str) -> int | None:
    m = re.search(rf"^CONFIG_{name}=(\d+)$", text, re.M)
    return int(m.group(1)) if m else None


def test_att_mtu_fits_longest_apdu(prj_conf):
    """默认 ATT_MTU=23 只给 20 字节可写，装不下 UNLOCK 的 30 字节 C-APDU。

    算式（DESIGN.md §2.3）：
      写请求可写载荷 = ATT_MTU − 3（1 B opcode + 2 B handle）
      本地 ATT_MTU  = MIN(BT_L2CAP_RX_MTU, BT_L2CAP_TX_MTU)
      BT_L2CAP_RX_MTU = CONFIG_BT_BUF_ACL_RX_SIZE − 4（L2CAP 头）

    这条钉的是「改小了会静默截断整条开锁报文」——手机侧只会看到一个
    ATT 错误码，隔着蓝牙极难诊断。
    """
    tx_mtu = kconf_int(prj_conf, "BT_L2CAP_TX_MTU")
    acl_rx = kconf_int(prj_conf, "BT_BUF_ACL_RX_SIZE")
    assert tx_mtu is not None, "prj.conf 没设 CONFIG_BT_L2CAP_TX_MTU（默认 23 不够）"
    assert acl_rx is not None, "prj.conf 没设 CONFIG_BT_BUF_ACL_RX_SIZE（默认 27 不够）"

    # UNLOCK: 80 10 00 00 Lc [uid(4) || counter(4) || mac(16)] 00
    apdu_len = 5 + 4 + 4 + 16 + 1
    assert apdu_len == 30

    local_mtu = min(acl_rx - 4, tx_mtu)
    assert local_mtu - 3 >= apdu_len, (
        f"MTU 不够：本地 ATT_MTU={local_mtu}，可写 {local_mtu - 3} < {apdu_len}")


def test_ble_single_connection_for_lockfree_unlock_state(prj_conf):
    """`unlock.c` 的 cur_nonce/nonce_valid/selected 是无锁静态状态。

    它们安全的唯一理由是「所有访问都只发生在 ble_unlock.c 那一个 work
    handler 线程里」（DESIGN.md §2.5）。BT_MAX_CONN > 1 会引入第二个并发源，
    那时必须补锁 —— 所以这个值不是资源调优参数，是安全论证的一部分。
    """
    assert kconf_int(prj_conf, "BT_MAX_CONN") == 1, \
        "BT_MAX_CONN != 1，unlock.c 的无锁状态论证不再成立（DESIGN.md §2.5）"


def test_ble_no_smp_no_bt_settings(prj_conf):
    """不开 SMP 是有意的（DESIGN.md §5.5 / ADR-004 §5）：

    设备没有屏幕也没有键盘，IO capability 只能是 NoInputNoOutput，
    那种配置下 LESC 只能退化成 Just Works（没有 MITM 防护）。
    连带地不需要 BT_SETTINGS —— 没有 LTK/IRK 要落盘，
    也就不该让 BT 去分摊 counter 所在那块 32 kB storage 分区的擦写寿命。

    这条测试防的是「有人为了『更安全』顺手开了 SMP」，那会同时
    引入 flash 争用和 settings_load 的时序要求。
    """
    live = [ln.strip() for ln in prj_conf.splitlines()
            if ln.strip() and not ln.strip().startswith("#")]
    assert "CONFIG_BT_SMP=n" in live, "SMP 没有显式关掉"
    assert "CONFIG_BT_SETTINGS=n" in live, "BT_SETTINGS 没有显式关掉"


def test_ble_is_peripheral_only(prj_conf):
    """设备只广播、只等连接，从不扫描也从不发起连接（DESIGN.md §2.1）。"""
    live = [ln.strip() for ln in prj_conf.splitlines()
            if ln.strip() and not ln.strip().startswith("#")]
    assert "CONFIG_BT=y" in live
    assert "CONFIG_BT_PERIPHERAL=y" in live
    assert not [ln for ln in live if ln.startswith("CONFIG_BT_CENTRAL=")], \
        "开了 central 角色 —— 本设备不该主动连别人"
    assert not [ln for ln in live if ln.startswith("CONFIG_BT_OBSERVER=")], \
        "开了 observer 角色 —— 本设备不该扫描"


def test_gatt_write_callback_defers_crypto(ble_c):
    """写回调跑在协议栈自己的 "BT RX WQ" 上（att.c 同步调 attr->write）。

    在那里跑 psa_mac_verify 会让未认证的攻击者用 UNLOCK 写请求拖垮
    整条 BT 处理 —— 这是 NFC 侧不存在的 DoS（NFC 得贴上来）。
    所以 on_cmd_write 只能 memcpy + submit，密码学必须在自有 work queue 里。
    """
    # ⚠ 文件里 on_cmd_write 出现两次：一处前向声明（服务表要定义在
    # work handler 之前），一处定义。`[^;]*?\)\s*\n\{` 只匹配定义那一处。
    m = re.search(r"static ssize_t on_cmd_write\([^;]*?\)\s*\n\{.*?\n\}", ble_c, re.S)
    assert m, "找不到 on_cmd_write 定义"
    body = m.group(0)
    assert "unlock_handle_apdu" not in body, \
        "on_cmd_write 里直接调了 unlock_handle_apdu —— 必须 defer（DESIGN.md §2.5）"
    assert "k_work_submit_to_queue" in body, "on_cmd_write 没有把工作交给自有队列"

    m = re.search(r"static void apdu_work_fn\(.*?\n\}", ble_c, re.S)
    assert m, "找不到 apdu_work_fn 定义"
    assert "unlock_handle_apdu" in m.group(0), "work handler 里没有处理 APDU"


def test_unlock_state_touched_only_from_own_workqueue(ble_c):
    """`unlock.c` 的 nonce/counter 状态是无锁的（DESIGN.md §2.5）。

    安全前提是「所有访问都在 unlock_workq 那一个线程上」。连接/断开回调跑在
    BT RX WQ 上、静止关广播跑在 uplink 线程上 —— 那两处**不能**直接调
    `unlock_session_reset()`，必须经 `session_reset()` 交给队列。

    这条防的是「看着无害就顺手直接调」——它不会崩，只会在极少数时序下
    把一个正在被验证的 nonce 抹掉，现场表现为「偶尔第一次开锁失败」。
    """
    direct = [ln for ln in ble_c.splitlines()
              if "unlock_session_reset()" in ln and not ln.lstrip().startswith("*")]
    # 只允许 work handler 里那一处
    assert len(direct) == 1, (
        "unlock_session_reset() 被直接调了多处，应该只在 work handler 里：\n"
        + "\n".join(direct))

    m = re.search(r"static void session_reset_work_fn\(.*?\n\}", ble_c, re.S)
    assert m and "unlock_session_reset()" in m.group(0), \
        "唯一那处不在 session_reset_work_fn 里"


def test_radio_stopped_before_flash_write_on_poweroff():
    """进 System OFF 前必须先停 radio，再 nvstore_flush()。

    两条独立的理由（DESIGN.md §2.7）：
    1. radio 是 EasyDMA master，产品规格书要求进 System OFF 前 EasyDMA 已结束；
    2. SOC_FLASH_NRF_RADIO_SYNC_MPSL 让 flash 写等 MPSL timeslot ——
       radio 还开着时那次 flush 的最坏延迟不可控。
    """
    src = (FW / "src" / "main.c").read_text(encoding="utf-8")
    m = re.search(r"static void enter_system_off\(void\).*?\n\}", src, re.S)
    assert m, "找不到 enter_system_off"
    body = m.group(0)
    assert "ble_unlock_shutdown" in body, "关机前没停 BLE（radio 还开着）"
    assert body.index("ble_unlock_shutdown") < body.index("nvstore_flush"), \
        "nvstore_flush 排在停 radio 之前 —— flash 写会等 MPSL timeslot"


def test_nfc_is_fully_removed():
    """ADR-004 是一次干净切换：不留 nfc_tag 模块、不留 CONFIG_NFC_*。

    留着的话下一个人会以为还有第二条开锁通道，而它既没有天线也没有
    UICR.NFCPINS —— 那是一条编得过但永远不工作的路径。
    """
    src_dir = FW / "src"
    assert not (src_dir / "nfc_tag.c").exists(), "nfc_tag.c 还在"
    assert not (src_dir / "nfc_tag.h").exists(), "nfc_tag.h 还在"

    cmake = (FW / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "nfc_tag.c" not in cmake, "CMakeLists 还在编 nfc_tag.c"
    assert "ble_unlock.c" in cmake, "CMakeLists 没编 ble_unlock.c"

    text = (FW / "prj.conf").read_text(encoding="utf-8")
    live = [ln.strip() for ln in text.splitlines()
            if ln.strip() and not ln.strip().startswith("#")]
    assert not [ln for ln in live if ln.startswith("CONFIG_NFC")], \
        "prj.conf 还有 CONFIG_NFC_*"

    for name in ("main.c", "uplink.c", "proto.c", "proto.h"):
        s = (src_dir / name).read_text(encoding="utf-8")
        assert "nfc_tag_" not in s, f"{name} 还在调 nfc_tag_*"


# --- 编译/仿真时才暴露出来的几条（2026-09-02 第一次 west build 之后补） ------


def test_board_overlay_filename_matches_board_target():
    """overlay 文件名必须带 `_uf2` 变体后缀，否则 **Zephyr 直接忽略它**。

    board target 是 `promicro_nrf52840/nrf52840/uf2`，`zephyr_build_string()`
    把 qualifiers 拼成 `promicro_nrf52840_nrf52840_uf2`。名字少了 `_uf2`
    时 cmake 一声不响地不加载 —— 症状是编译期
    `__device_dts_ord_DT_N_ALIAS_motion_int_... undeclared`（实测过），
    因为整个 overlay 的节点都不存在。
    """
    boards = FW / "boards"
    assert (boards / "promicro_nrf52840_nrf52840_uf2.overlay").exists(), \
        "overlay 文件名不含 _uf2 变体后缀 —— 构建时会被静默忽略"


def test_overlay_includes_lis2dw12_dt_bindings():
    """overlay 里用了 `LIS2DW12_DT_*` 宏，就必须自己 include 那个头。

    板级 DTS 不会带进 `dt-bindings/sensor/lis2dw12.h`，少了它是
    `parse error: expected number or parenthesized expression`（实测过）。
    """
    ov = (FW / "boards" / "promicro_nrf52840_nrf52840_uf2.overlay").read_text(
        encoding="utf-8")
    if "LIS2DW12_DT_" in ov:
        assert "dt-bindings/sensor/lis2dw12.h" in ov, \
            "用了 LIS2DW12_DT_* 宏但没 include 对应的 dt-bindings 头"


def test_kconfig_symbols_that_are_link_time_landmines(prj_conf):
    """三个默认 n / 需要显式打开的符号，缺了都是**链接期**才报错。

    - `CONFIG_BASE64`：proto.c 的 `base64_decode()`
    - `CONFIG_HWINFO`：uplink.c 的 `hwinfo_get_reset_cause()`。
      注意 `HWINFO_NRF` 的 `default y` 在 `if HWINFO` 里面，
      所以只写注释「default y 不用显式写」是错的（实测过）。
    - `CONFIG_LIS2DW12_WAKEUP`：`SENSOR_TRIG_MOTION` 整段在这个 ifdef 里，
      缺了不是链接错误而是**运行期 -ENOTSUP**，唤醒源直接不存在。
    """
    live = [ln.strip() for ln in prj_conf.splitlines()
            if ln.strip() and not ln.strip().startswith("#")]
    for sym in ("CONFIG_BASE64=y", "CONFIG_HWINFO=y",
                "CONFIG_LIS2DW12_WAKEUP=y"):
        assert sym in live, f"prj.conf 缺 {sym}"


def test_lis2dw12_sleep_stays_off(prj_conf):
    """`CONFIG_LIS2DW12_SLEEP` 必须**保持关闭**。

    这一条原来是反的（断言它 `=y`），因为那时代码注册了
    `SENSOR_TRIG_STATIONARY`。但那个触发走 **INT2**，而本板只接了 INT1 ——
    `sensor_trigger_set()` 返回 0、日志还打 INFO，事件却永远不到达。
    后果不是「少一个事件」而是 `main.c` 的运动状态永远翻不回 still，
    于是 BLE 广播永远关不掉，DESIGN.md §2.4 的防跟踪保证静默失效。

    2026-09-03 改成软件计时（`CONFIG_EBIKE_STILL_AFTER_S`），
    motion.c 不再注册 STATIONARY，这个符号成了纯负担。
    钉住它别被人「顺手」加回来。
    """
    live = [ln.strip() for ln in prj_conf.splitlines()
            if ln.strip() and not ln.strip().startswith("#")]
    assert "CONFIG_LIS2DW12_SLEEP=y" not in live, (
        "CONFIG_LIS2DW12_SLEEP 被打开了 —— 它只 gate STATIONARY，"
        "而那个触发走 INT2、本板没接线。见 src/motion.h")


def test_motion_does_not_register_stationary():
    """motion.c 不能注册 `SENSOR_TRIG_STATIONARY`。

    注册它会成功（返回 0），但事件永不到达 —— 「配置成功、功能静默失效」，
    最难查的一类。静止判定必须走 `CONFIG_EBIKE_STILL_AFTER_S` 的软件计时。
    """
    motion_c = (FW / "src" / "motion.c").read_text(encoding="utf-8")
    live = "\n".join(
        ln for ln in motion_c.splitlines()
        if not ln.lstrip().startswith(("*", "/*", "//")))
    assert "SENSOR_TRIG_STATIONARY" not in live, (
        "motion.c 又注册了 SENSOR_TRIG_STATIONARY —— 它走 INT2，本板没接线")
    assert "CONFIG_EBIKE_STILL_AFTER_S" in live, (
        "motion.c 没用 CONFIG_EBIKE_STILL_AFTER_S —— 软件静止计时没了")


def test_motion_threshold_uses_ms2_conversion():
    """`SENSOR_ATTR_UPPER_THRESH` 的单位是 m/s²，不是 mg。

    驱动按 `sensor_ms2_to_mg()` 反算（lis2dw12.c:240），直接塞 mg 会
    **小 9.8 倍**：150 mg 变 15 mg，低于一格 31.25 mg → 寄存器写 0 →
    传感器持续触发。这条钉住那次换算没被人改回去。
    """
    src = (FW / "src" / "motion.c").read_text(encoding="utf-8")
    assert "sensor_ug_to_ms2" in src, \
        "motion.c 没用 sensor_ug_to_ms2 做单位换算（阈值会小 9.8 倍）"
    assert not re.search(r"\.val1\s*=\s*mg\s*/\s*1000", src), \
        "motion.c 又在直接把 mg 塞进 sensor_value"


def test_bsim_test_mtu_matches_firmware(prj_conf):
    """bsim 测试的 MTU 配置必须和固件一致。

    测试里断言 1 在**运行时**验 `MTU-3 >= 30`；如果测试的 prj.conf
    配得比固件宽松，那条断言就失去了保护固件配置的意义。
    """
    bsim = FW.parent / "tests" / "ble_unlock_bsim" / "prj.conf"
    if not bsim.exists():
        pytest.skip("没有 bsim 测试")
    btext = bsim.read_text(encoding="utf-8")
    for sym in ("BT_L2CAP_TX_MTU", "BT_BUF_ACL_RX_SIZE"):
        assert kconf_int(btext, sym) == kconf_int(prj_conf, sym), \
            f"bsim 测试与固件的 {sym} 不一致"


# --- AT 命令层（2026-09-03 审计 H2 之后补：modem.c 之前完全不在扫描范围） ------


@pytest.fixture(scope="module")
def modem_c() -> str:
    p = FW / "src" / "modem.c"
    if not p.exists():
        pytest.skip(f"没有固件源码 {p}")
    return p.read_text(encoding="utf-8")


def test_mconfig_will_topic_is_quoted_topic_literal(proto_h, modem_c):
    """Air780EP 的 MCONFIG 是 7 参数位：
    clientid, username, password, will_qos, will_retain, will_topic, will_message。
    keepalive 属于 AT+MCONNECT，不在 MCONFIG 里。

    上一版在 will_retain 后多塞了一个 `60`，把后半段整体顶错位：
    will_topic 配成了 "60"、LWT topic 串被当成遗嘱内容 —— 遗嘱永远
    发不到契约的 lwt topic，服务端收不到 lwt=1（审计 H2）。
    """
    # 只认真正的格式串行（注释里的语法示例也含 AT+MCONFIG，不能误匹配）。
    # 格式串行以制表符开头、AT+MCONFIG 紧跟在开引号后。
    lines = [ln for ln in modem_c.splitlines()
             if '"AT+MCONFIG=' in ln]
    assert lines, "modem.c 里找不到 AT+MCONFIG 格式串"
    line = lines[0]
    # 格式串里的 " 在 C 源码中是 \\" —— 7 参数位：3 字符串,1,1,字符串,字符串
    assert '\\"%s\\",\\"%s\\",\\"%s\\",1,1,\\"%s\\"' in line, \
        "MCONFIG 参数形状变了 —— 对照 Air780EP 手册的 7 参数位重新核对"
    assert ",1,1,60," not in line, \
        "MCONFIG 里又混进了 keepalive —— keepalive 属于 AT+MCONNECT"
    assert "TOPIC_LWT" in modem_c


def test_mconnect_carries_keepalive(modem_c):
    """keepalive 的正确位置：AT+MCONNECT=<clean_session>,<keepalive>。"""
    assert 'AT+MCONNECT=1,60' in modem_c, \
        "keepalive 不在 AT+MCONNECT 里 —— 检查它是不是又被塞进 MCONFIG"
