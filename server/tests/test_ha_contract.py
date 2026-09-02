"""HA 集成与服务端契约的一致性。

HA 集成在这台机器上**能**被真 HA import（`docs/HA.md` 记了怎么验的），
但它跑起来是在另一台机器上。这些测试防的是「服务端改了 state 字段名，
HA 侧还读旧名」——那种错误的表现是 HA 里所有实体变成 unknown，
而日志里一句话都没有。
"""

from __future__ import annotations

import ast
import json
import re
from pathlib import Path

import pytest

from ebike_server import contract as ct
from ebike_server.config import DeviceConfig, MqttConfig, ServerConfig
from ebike_server.derive import build_state
from ebike_server.store import Store

HA = Path(__file__).resolve().parents[2] / "homeassistant" / "custom_components" / "ebike_tracker"
CONST_PY = HA / "const.py"
MANIFEST = HA / "manifest.json"


@pytest.fixture(scope="module")
def ha_const() -> dict[str, object]:
    """把 const.py 里的顶层字面量赋值抠出来。

    用 ast 而不是 import：这台机器上的 Python 是 3.13 而 HA 要 3.14+，
    直接 import 会因为 homeassistant 依赖失败。const.py 本身没有 HA 依赖，
    但 ast 解析更稳，也顺便证明了「常量都是字面量、没有运行时计算」。

    有些常量引用了前面的常量（`SRC_LABELS` 里用了 `SRC_GNSS`），
    所以边解析边把已知值喂回去。
    """
    if not CONST_PY.exists():
        pytest.skip("HA 集成还没写")
    tree = ast.parse(CONST_PY.read_text(encoding="utf-8"))
    out: dict[str, object] = {}

    def resolve(node: ast.AST) -> object:
        """literal_eval 的加强版：允许引用已解析出的常量。"""
        if isinstance(node, ast.Name):
            if node.id in out:
                return out[node.id]
            raise ValueError(node.id)
        if isinstance(node, ast.Dict):
            return {resolve(k): resolve(v)
                    for k, v in zip(node.keys, node.values) if k is not None}
        if isinstance(node, (ast.List, ast.Tuple, ast.Set)):
            return [resolve(e) for e in node.elts]
        return ast.literal_eval(node)

    for node in tree.body:
        targets: list[str] = []
        value: ast.AST | None = None
        if isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            targets = [node.target.id]
            value = node.value
        elif isinstance(node, ast.Assign):
            targets = [t.id for t in node.targets if isinstance(t, ast.Name)]
            value = node.value
        if not targets or value is None:
            continue
        try:
            resolved = resolve(value)
        except (ValueError, TypeError, SyntaxError):
            continue
        for name in targets:
            out[name] = resolved
    return out


# --- topic ------------------------------------------------------------------


def test_prefix_matches(ha_const):
    assert ha_const["PREFIX"] == ct.PREFIX


def test_state_suffix_matches(ha_const):
    assert ha_const["STATE_SUFFIX"] == ct.STATE


def test_ha_uses_exact_topic_not_wildcard():
    """契约 §4.3：用通配符会把「所有车的 state」注册进 amqtt 的全局过滤器表，
    加第二辆车时 bike02 一连上就会收到 bike01 的位置。

    只看代码不看注释 —— const.py 的注释里就解释了为什么不用通配符，
    那段文字本身包含 `/+/`。
    """
    src = CONST_PY.read_text(encoding="utf-8")
    assert "sub_all_state" not in src
    assert re.search(r'return f"\{PREFIX\}/\{device_id\}/\{STATE_SUFFIX\}"', src), \
        "state_topic 不是精确 topic"

    for f in sorted(HA.rglob("*.py")):
        tree = ast.parse(f.read_text(encoding="utf-8"))

        # docstring 里会解释「为什么不用通配符」，那段文字本身含 `/+/`。
        # 先把所有 docstring 节点收集起来排除掉。
        docstrings = set()
        for node in ast.walk(tree):
            if isinstance(node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef,
                                 ast.ClassDef)) and node.body:
                first = node.body[0]
                if (isinstance(first, ast.Expr)
                        and isinstance(first.value, ast.Constant)
                        and isinstance(first.value.value, str)):
                    docstrings.add(id(first.value))

        for node in ast.walk(tree):
            if (isinstance(node, ast.Constant) and isinstance(node.value, str)
                    and id(node) not in docstrings):
                assert "/+/" not in node.value, f"{f.name} 有通配符字面量"
            if isinstance(node, ast.JoinedStr):
                literal = "".join(
                    v.value for v in node.values
                    if isinstance(v, ast.Constant) and isinstance(v.value, str)
                )
                assert "/+" not in literal, f"{f.name} 的 f-string 里有通配符"


# --- state 字段 --------------------------------------------------------------

#: HA 的常量名 → 服务端 state 里的实际键
FIELD_MAP = {
    "F_TIME": "t",
    "F_ONLINE": "on",
    "F_MODE": "mo",
    "F_LAT": "la",
    "F_LON": "lo",
    "F_GCJ_LAT": "gla",
    "F_GCJ_LON": "glo",
    "F_ACCURACY": "a",
    "F_SRC": "s",
    "F_VOLT": "v",
    "F_PERCENT": "pct",
    "F_GEOFENCE": "gf",
    "F_LAST_SEEN": "ls",
    "F_LWT": "lwt",
    "F_LOCKED": "lk",
}


@pytest.mark.parametrize("name,key", sorted(FIELD_MAP.items()))
def test_field_constant_values(ha_const, name, key):
    assert ha_const[name] == key, f"HA 的 {name} 是 {ha_const[name]!r}，契约是 {key!r}"


async def test_every_ha_field_exists_in_real_state(tmp_path):
    """**这条是最重要的**：拿服务端真算出来的 state，检查 HA 要读的每个键都在。

    服务端删掉或改名一个字段，这条会红。
    """
    cfg = ServerConfig(
        db_path=str(tmp_path / "t.db"),
        api_token="x",
        mqtt=MqttConfig(plain_bind="", tls_bind=""),
        devices=[DeviceConfig(id="bike01", geofence=[31.2, 121.4, 100.0])],
    )
    store = Store(cfg.db_path)
    await store.open()
    try:
        await store.touch("bike01", 10_000)
        await store.add_loc("bike01", [{
            "q": 1, "t_dev": 1, "src": "g", "lat": 31.230416, "lon": 121.473701,
            "acc": 8.0, "speed": None, "heading": None, "sats": 9,
        }], 10_000)
        await store.add_tele("bike01", {"q": 2, "t_dev": 1, "volt": 54.2,
                                        "csq": 18, "uptime": 60, "temp": 20},
                             10_000)
        await store.add_event("bike01", {"q": 3, "t_dev": 1, "kind": "lock_state",
                                         "detail": {"locked": True}}, 10_000)
        state = await build_state(store, "bike01", cfg, now=10_000)
    finally:
        await store.close()

    for name, key in FIELD_MAP.items():
        assert key in state, f"服务端的 state 里没有 {key!r}（HA 的 {name}）"

    # 值也要是 HA 能用的类型
    assert isinstance(state["on"], bool)
    assert isinstance(state["lk"], bool)
    assert state["mo"] in ("moving", "parked")
    assert state["s"] in ("g", "l")


async def test_lk_is_none_without_lock_sensor(tmp_path):
    """位置反馈微动开关是选配的，所以 `lk` 为 None 是常态。
    HA 侧必须显示「未知」而不是「没锁」——那会让人白跑一趟。"""
    cfg = ServerConfig(db_path=str(tmp_path / "t.db"), api_token="x",
                       mqtt=MqttConfig(plain_bind="", tls_bind=""),
                       devices=[DeviceConfig(id="bike01")])
    store = Store(cfg.db_path)
    await store.open()
    try:
        await store.touch("bike01", 10_000)
        state = await build_state(store, "bike01", cfg, now=10_000)
    finally:
        await store.close()
    assert state["lk"] is None


# --- 枚举值 ------------------------------------------------------------------


def test_mode_values_match(ha_const):
    assert ha_const["MODE_MOVING"] == "moving"
    assert ha_const["MODE_PARKED"] == "parked"


def test_src_values_match(ha_const):
    assert ha_const["SRC_GNSS"] == ct.SRC_GNSS
    assert ha_const["SRC_LBS"] == ct.SRC_LBS


def test_src_labels_cover_all_sources(ha_const):
    labels = ha_const["SRC_LABELS"]
    assert set(labels) == ct.SOURCES, "有定位源没有对应的中文说法"


# --- manifest 与翻译 ----------------------------------------------------------


def test_manifest_valid_and_declares_mqtt():
    m = json.loads(MANIFEST.read_text(encoding="utf-8"))
    assert m["domain"] == "ebike_tracker"
    assert m["config_flow"] is True
    # 复用 HA 自己的 MQTT 连接，不自己开客户端
    assert "mqtt" in m["dependencies"]
    # state 是推过来的，不是轮询
    assert m["iot_class"] == "local_push"
    # 不引入第三方依赖 —— 装 custom component 不该拉 pip 包
    assert m["requirements"] == []


def test_translations_cover_all_entities():
    """每个实体的 translation_key 都要有中英文翻译，否则界面显示的是键名。"""
    keys: set[str] = set()
    for platform in ("sensor.py", "binary_sensor.py"):
        text = (HA / platform).read_text(encoding="utf-8")
        keys |= {(platform.split(".")[0], k)
                 for k in re.findall(r'translation_key="([a-z_]+)"', text)}

    for lang in ("zh-Hans", "en"):
        data = json.loads(
            (HA / "translations" / f"{lang}.json").read_text(encoding="utf-8"))
        entity = data.get("entity", {})
        for domain, key in keys:
            assert key in entity.get(domain, {}), \
                f"{lang}.json 缺 entity.{domain}.{key}"


def test_config_flow_errors_have_translations():
    text = (HA / "config_flow.py").read_text(encoding="utf-8")
    errors = set(re.findall(r'errors\[[^\]]+\] = "([a-z_]+)"', text))
    aborts = set(re.findall(r'async_abort\(reason="([a-z_]+)"\)', text))
    for lang in ("zh-Hans", "en"):
        data = json.loads(
            (HA / "translations" / f"{lang}.json").read_text(encoding="utf-8"))
        cfg = data["config"]
        for e in errors:
            assert e in cfg.get("error", {}), f"{lang}.json 缺 config.error.{e}"
        for a in aborts:
            assert a in cfg.get("abort", {}), f"{lang}.json 缺 config.abort.{a}"
    # already_configured 由 _abort_if_unique_id_configured 抛，代码里搜不到
    for lang in ("zh-Hans", "en"):
        data = json.loads(
            (HA / "translations" / f"{lang}.json").read_text(encoding="utf-8"))
        assert "already_configured" in data["config"]["abort"]


def test_device_id_regex_matches_contract():
    """HA 的输入校验必须和契约 §4 一致，否则用户能填一个服务端不认的 id。"""
    text = (HA / "config_flow.py").read_text(encoding="utf-8")
    m = re.search(r'DEVICE_ID_RE = re\.compile\(r"([^"]+)"\)', text)
    assert m, "config_flow 里没有设备 id 校验"
    assert m.group(1) == ct.DEVICE_ID_RE.pattern


def test_no_lock_entity_platform():
    """契约 §6.1：远程开锁两边默认都关着。给一个按了会失败的锁按钮比不给更糟。"""
    assert not (HA / "lock.py").exists(), \
        "有 lock 平台 —— 那会给出一个默认按不动的开锁按钮"
    init = (HA / "__init__.py").read_text(encoding="utf-8")
    assert "Platform.LOCK" not in init
