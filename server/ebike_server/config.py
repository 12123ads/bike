"""配置。YAML 可选，环境变量优先，默认值能直接跑起来。

刻意不引入 pydantic-settings：这份配置只有十几项，dataclass + 一个 loader
比多一个依赖划算。
"""

from __future__ import annotations

import json
import logging
import os
import secrets
from dataclasses import dataclass, field, fields
from pathlib import Path
from typing import Any

_log = logging.getLogger("ebike.config")

DEFAULT_DIR = Path(os.environ.get("EBIKE_DIR", "/opt/ebike-tracker/run"))


def default_config_path() -> Path:
    """配置文件的默认位置。容器里 EBIKE_DIR=/data，所以是 /data/config.json。

    有这个函数是为了让 `init` 和 `run` 对「配置在哪」有同一个答案 ——
    不然容器的 CMD 得写死路径，而写死路径和 EBIKE_DIR 会打架。
    """
    return DEFAULT_DIR / "config.json"


@dataclass
class MqttConfig:
    """内置 broker。契约 §1：broker 跑在服务端进程里，不用 /opt/mqtt 的 Mosquitto。"""

    #: 明文监听。**默认只绑 127.0.0.1** —— 明文口令过网络等于把开锁凭据广播出去。
    #: 想让别的机器连，用下面的 TLS listener。
    plain_bind: str = "127.0.0.1:1883"
    #: TLS 监听，对外用。空字符串 = 不开。
    tls_bind: str = "0.0.0.0:8883"
    certfile: str = str(DEFAULT_DIR / "certs" / "server.crt")
    keyfile: str = str(DEFAULT_DIR / "certs" / "server.key")
    #: 校验客户端证书用的 CA。mode="cert" 时必须。
    cafile: str = str(DEFAULT_DIR / "certs" / "ca.crt")

    #: "password" = 用户名 + argon2 口令（默认）；"cert" = 客户端证书 SAN 认身份。
    #: 契约 §2 解释了为什么单车场景下 password 与 mTLS 安全性等价。
    mode: str = "password"
    #: mode="cert" 时，设备证书 SAN 里 spiffe://<uri_domain>/device/<id> 的域名部分
    cert_uri_domain: str = "ebike.local"

    password_file: str = str(DEFAULT_DIR / "passwd")


@dataclass
class DeviceConfig:
    """一辆车。单车场景下这个列表就一项，但仍然是列表 —— 加第二辆不用改结构。"""

    id: str = "bike01"
    #: 上报周期，秒。用来算离线阈值，也是 `interval` 指令的当前值。
    report_interval: int = 900
    #: 电压-电量曲线。48V 铅酸的粗略取值，锂电要换。
    #: 格式 [[电压, 百分比], ...]，线性插值。
    volt_curve: list[list[float]] = field(default_factory=lambda: [
        [42.0, 0.0], [46.0, 10.0], [48.0, 30.0],
        [50.4, 60.0], [54.0, 85.0], [58.8, 100.0],
    ])
    #: 地理围栏：[lat, lon, 半径米]。None = 不判，`state.gf` 为 null。
    geofence: list[float] | None = None


@dataclass
class WebConfig:
    """网页界面。默认**开**，但它和 HTTP API 共用 `http_bind`（默认只绑 127.0.0.1）。"""

    enabled: bool = True

    #: 高德 JS API 的 key。放文件里而不是配置里，免得配置文件被贴到别处时带出去。
    #: 文件内容就是一行 key。容器里由 docker-compose 只读挂到 /run/secrets/。
    gaode_key_file: str = "/root/gaode.key"
    #: 也可以直接填（优先级低于文件）。
    gaode_key: str = ""

    #: 高德 JS API 2.0 的「安全密钥」。只有用到服务端代理的插件才需要，
    #: 基础地图 + 标记 + 折线不需要。留空即可。
    gaode_security_code: str = ""

    #: 会话有效期，秒。默认 12 小时。
    session_ttl: int = 12 * 3600

    #: Cookie 是否加 Secure 标记。
    #: ⚠ **默认 False**，因为默认部署是 `http://127.0.0.1:8080`，
    #: 加了 Secure 浏览器就不会回传 cookie，登录会陷入死循环。
    #: **放到反向代理后面走 HTTPS 时，把这一项改成 true。**
    cookie_secure: bool = False

    def resolve_gaode_key(self) -> str:
        """读 key。文件优先，然后是配置里的 `gaode_key`。

        读不到返回空串 —— 网页会退化成「只显示坐标」，不影响别的功能。

        ⚠ **读不到时一定要 warn。** 之前这里是静默返回空串，
        结果容器里因为权限打不开文件（宿主的 `/root/gaode.key` 是 0600 root，
        容器以 uid 1000 跑），网页上只是地图空着，日志里一个字都没有 ——
        那种问题只能靠猜。
        """
        if self.gaode_key_file:
            p = Path(self.gaode_key_file)
            if p.exists():
                try:
                    key = p.read_text(encoding="utf-8").strip()
                    if key:
                        return key
                    _log.warning("高德 key 文件 %s 是空的", p)
                except PermissionError:
                    _log.warning(
                        "读不了高德 key 文件 %s（权限不足，当前 uid=%s）。"
                        "地图会退化成只显示坐标。"
                        "容器部署时宿主文件要能被 uid 1000 读到（chmod 644），"
                        "或者用 EBIKE_GAODE_KEY 环境变量传",
                        p, os.getuid())
                except OSError as e:
                    _log.warning("读高德 key 文件 %s 失败：%s", p, e)
            else:
                _log.info("高德 key 文件 %s 不存在，地图会退化成只显示坐标", p)
        if self.gaode_key:
            return self.gaode_key
        return ""


@dataclass
class ServerConfig:
    #: HTTP API
    http_bind: str = "127.0.0.1:8080"
    #: Bearer token。空 = **随机生成并打印到日志**，不允许无鉴权跑起来。
    api_token: str = ""

    db_path: str = str(DEFAULT_DIR / "ebike.db")

    #: 离线判定 = report_interval * 这个倍数 + 下面的常数。契约 §4.2：
    #: 不能用 LWT 判离线，因为模组关机档下每个正常周期都会非优雅断连一次。
    offline_factor: int = 3
    offline_grace: int = 120

    #: 远程开锁。**默认关闭** —— 契约 §6.1：它绕过 §5.2 的挑战应答，
    #: 实际含义是「谁能登进服务端谁就能开你的车」。
    allow_remote_unlock: bool = False

    #: 轨迹保留天数。0 = 永久。DESIGN.md §11 #9 的一半答案。
    track_retention_days: int = 365

    #: 已确认的下行行保留天数。0 = 永久。
    #: 未确认的行**永不删**（契约 §4.1：那可能是唯一还能开锁的密钥），
    #: 这里只清 `acked=1` 的历史行，否则 pending_downlink 会无限增长。
    downlink_retention_days: int = 30

    #: 判「移动」的位移阈值，米。低于它认为是 GPS 漂移不是真的动了。
    moving_threshold_m: float = 30.0

    mqtt: MqttConfig = field(default_factory=MqttConfig)
    web: WebConfig = field(default_factory=WebConfig)
    devices: list[DeviceConfig] = field(default_factory=lambda: [DeviceConfig()])

    def device(self, device_id: str) -> DeviceConfig | None:
        return next((d for d in self.devices if d.id == device_id), None)

    def offline_after(self, device_id: str) -> int:
        d = self.device(device_id)
        interval = d.report_interval if d else 900
        return interval * self.offline_factor + self.offline_grace


def _hydrate(cls: type, data: dict[str, Any]) -> Any:
    """把嵌套 dict 填进 dataclass。未知键直接报错 —— 静默忽略拼错的配置项
    会让人以为设置生效了，而它没有。
    """
    #: 嵌套 dataclass 的字段名 → 类型。
    #: 不靠 `known[key].type` 判断：`from __future__ import annotations` 之后
    #: 那是**字符串**而不是类型对象，`is_dataclass()` 永远是 False，
    #: 嵌套段就会被原样塞成 dict 然后在使用处炸掉。
    nested: dict[str, type] = {
        "mqtt": MqttConfig,
        "web": WebConfig,
    }

    kwargs: dict[str, Any] = {}
    known = {f.name: f for f in fields(cls)}
    for key, value in data.items():
        if key not in known:
            raise ValueError(f"配置里有未知项 {cls.__name__}.{key}")
        if key in nested and isinstance(value, dict):
            kwargs[key] = _hydrate(nested[key], value)
        elif key == "devices" and isinstance(value, list):
            kwargs[key] = [_hydrate(DeviceConfig, v) for v in value]
        else:
            kwargs[key] = value
    return cls(**kwargs)


def load(path: str | Path | None = None) -> ServerConfig:
    """读配置。JSON 或 YAML（YAML 需要 pyyaml，amqtt 已经带了）。

    环境变量覆盖：`EBIKE_API_TOKEN`、`EBIKE_DB`、`EBIKE_HTTP_BIND`。

    指定了 path 但文件不存在时**抛错**，不静默退回默认值 ——
    路径打错却用默认配置跑起来，比直接失败危险得多（会绑到别的端口、
    用别的数据库，而日志里看不出异常）。
    """
    cfg = ServerConfig()
    if path:
        p = Path(path)
        if not p.exists():
            raise FileNotFoundError(f"配置文件不存在：{p}")
        text = p.read_text(encoding="utf-8")
        if p.suffix in (".yaml", ".yml"):
            import yaml  # amqtt 依赖里已有
            data = yaml.safe_load(text) or {}
        else:
            data = json.loads(text)
        cfg = _hydrate(ServerConfig, data)

    if tok := os.environ.get("EBIKE_API_TOKEN"):
        cfg.api_token = tok
    if db := os.environ.get("EBIKE_DB"):
        cfg.db_path = db
    if bind := os.environ.get("EBIKE_HTTP_BIND"):
        cfg.http_bind = bind
    # 容器里读不到宿主的 0600 文件时的退路（见 WebConfig.resolve_gaode_key）
    if gk := os.environ.get("EBIKE_GAODE_KEY"):
        cfg.web.gaode_key = gk

    if not cfg.api_token:
        # 不允许无鉴权：生成一个并让启动日志打出来，比默默开放好
        cfg.api_token = secrets.token_urlsafe(24)
        cfg._token_generated = True  # type: ignore[attr-defined]
    return cfg


def write_default(path: str | Path, *, docker: bool = False) -> Path:
    """写一份带默认值的配置文件。已存在则不动，返回路径。

    `docker=True` 时把 HTTP 和明文 MQTT 都绑到 0.0.0.0 ——
    容器里绑 127.0.0.1 等于谁都连不上（那是容器自己的 loopback）。
    **明文 MQTT 因此默认关掉**（`plain_bind` 留空）：容器网络里开明文口
    等于把开锁凭据交给同一网络的任何容器。
    """
    p = Path(path)
    if p.exists():
        return p

    cfg = ServerConfig()
    cfg.api_token = secrets.token_urlsafe(24)
    if docker:
        cfg.http_bind = "0.0.0.0:8080"
        cfg.mqtt.plain_bind = ""          # 容器里不开明文口
        cfg.mqtt.tls_bind = "0.0.0.0:8883"
        # docker-compose 把宿主的 /root/gaode.key 只读挂到这里
        cfg.web.gaode_key_file = "/run/secrets/gaode.key"

    data = {
        "http_bind": cfg.http_bind,
        "api_token": cfg.api_token,
        "db_path": cfg.db_path,
        "offline_factor": cfg.offline_factor,
        "offline_grace": cfg.offline_grace,
        "allow_remote_unlock": cfg.allow_remote_unlock,
        "track_retention_days": cfg.track_retention_days,
        "downlink_retention_days": cfg.downlink_retention_days,
        "moving_threshold_m": cfg.moving_threshold_m,
        "mqtt": {
            "plain_bind": cfg.mqtt.plain_bind,
            "tls_bind": cfg.mqtt.tls_bind,
            "certfile": cfg.mqtt.certfile,
            "keyfile": cfg.mqtt.keyfile,
            "cafile": cfg.mqtt.cafile,
            "mode": cfg.mqtt.mode,
            "cert_uri_domain": cfg.mqtt.cert_uri_domain,
            "password_file": cfg.mqtt.password_file,
        },
        "web": {
            "enabled": cfg.web.enabled,
            "gaode_key_file": cfg.web.gaode_key_file,
            "gaode_security_code": cfg.web.gaode_security_code,
            "session_ttl": cfg.web.session_ttl,
            "cookie_secure": cfg.web.cookie_secure,
        },
        "devices": [
            {
                "id": d.id,
                "report_interval": d.report_interval,
                "volt_curve": d.volt_curve,
                "geofence": d.geofence,
            }
            for d in cfg.devices
        ],
    }
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n",
                 encoding="utf-8")
    # 里面有 api_token，别让同宿主的其他用户读到
    p.chmod(0o600)
    return p


def split_bind(bind: str, default_port: int) -> tuple[str, int]:
    host, _, port = bind.rpartition(":")
    if not host:
        return bind, default_port
    return host, int(port)
