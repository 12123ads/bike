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
