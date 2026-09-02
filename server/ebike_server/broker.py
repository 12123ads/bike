"""内置 broker：amqtt 插件 + ingest 管线。

契约 §1 的落地。这里是整个服务端最需要小心的地方，原因是 amqtt 的三个实际行为
（都是本机读源码确认的，不是文档）：

1. `publish-acl` 没配时**发布一律放行**（`amqtt/plugins/topic_checking.py:70`，
   为兼容 hbmqtt）。所以 ACL 必须显式配，漏配是静默放开。启动时断言。
2. listener 的 SSLContext 写死 `verify_mode = CERT_OPTIONAL`
   （`amqtt/broker.py:317`）。TLS 层不强制客户端证书，身份在认证插件里判。
3. 插件的事件回调是按 `on_<event>` 命名反射找的（`plugins/manager.py:150`），
   拼错方法名不会报错，只是永远不被调用。所以下面每个 handler 名字都对着
   `amqtt/events.py` 的枚举值抄的。
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, ClassVar

from amqtt.broker import BrokerContext
from amqtt.contrib.cert import UserAuthCertPlugin
from amqtt.plugins.base import BasePlugin
from amqtt.session import ApplicationMessage, Session

from . import contract as ct
from .config import ServerConfig, split_bind

if TYPE_CHECKING:
    from .service import Service

log = logging.getLogger("ebike.broker")


class IngestPlugin(BasePlugin[BrokerContext]):
    """把设备上行喂进 Service。

    做成 amqtt 插件而不是另起一个 MQTT client 连自己：契约 §1 说的
    「上行报文在进程内直接进 ingest，少一次序列化」就是这个意思。
    副作用是拿到了 `CLIENT_CONNECTED` 事件，下行队列可以在设备一连上就冲刷（§4.1），
    不用等它先发 `up/hello`。
    """

    #: Service 在启动时塞进来。**类属性**而非实例属性，因为 amqtt 用
    #: `plugin_class(context)` 固定签名实例化插件，插不进自定义参数。
    service: ClassVar["Service | None"] = None

    async def on_broker_message_received(self, *, client_id: str | None = None,
                                         message: ApplicationMessage | None = None,
                                         **_: Any) -> None:
        """每条 PUBLISH 都过这里。

        ⚠ 这个事件在 ACL 检查**之前**触发（`amqtt/broker.py:753` 的注释：
        "even if a client isn't necessarily allowed to send it"）。
        所以这里必须自己再判一次「这个 client 有没有资格发这个 topic」，
        不能依赖 broker 的 ACL 已经挡过了。
        """
        svc = IngestPlugin.service
        if svc is None or message is None or not message.topic:
            return
        try:
            await svc.ingest(client_id, message.topic, bytes(message.data or b""))
        except Exception:
            # 一条畸形报文不能弄死 broker 的 handler 任务
            log.exception("ingest 失败 topic=%s client=%s", message.topic, client_id)

    async def on_broker_client_connected(self, *, client_id: str | None = None,
                                         client_session: Session | None = None,
                                         **_: Any) -> None:
        svc = IngestPlugin.service
        if svc is None or client_id is None:
            return
        log.info("客户端连上: %s", client_id)
        # 契约 §4.1：设备一上线就冲刷未确认的下行队列。
        # 放在这里而不是等 up/hello，是因为 hello 有可能丢（QoS1 也只是至少一次，
        # 不是一定及时），而 CLIENT_CONNECTED 是 broker 本地事实。
        await svc.on_device_online(client_id)

    async def on_broker_client_disconnected(self, *, client_id: str | None = None,
                                            **_: Any) -> None:
        log.info("客户端断开: %s", client_id)

    @dataclass
    class Config:
        """本插件无配置项，但 amqtt 要求这个类存在（`_load_str_plugin` 里
        用 dacite 严格模式填充它）。"""


class DeviceCertAuthPlugin(UserAuthCertPlugin):
    """mTLS 认证 + **把身份写进 session.username**。

    上游的 `UserAuthCertPlugin` 只比对证书 SAN 与 client_id 然后返回布尔，
    **从不设置 `session.username`**（`amqtt/contrib/cert.py:57-83`）。
    而 `TopicAccessControlListPlugin` 是按 `session.username` 查 ACL 的，
    None 时退化成 `"anonymous"`（`amqtt/plugins/topic_checking.py:77-79`），
    查出来是空列表 → 一律拒。

    本机实测（mode="cert"，设备证书 SAN = spiffe://ebike.local/device/bike01）：

    - CONNECT 通过（认证本身没问题）
    - SUBSCRIBE 返回 **0x80 拒绝** → **设备永远收不到 dn/cmd 和 dn/secret**，
      远程指令和密钥轮换全废
    - PUBLISH 的报文**仍然落了库** —— 但那是因为 IngestPlugin 挂的
      MESSAGE_RECEIVED 早于 ACL 检查（`amqtt/broker.py:753`），
      broker 的转发其实也被拒了。靠一个巧合工作，不是设计。

    所以这里补上 username。用 `client_id` 而不是 SAN 里的 device id：
    父类已经断言过两者相等（`cert.py:78` 的 `match.group(1) == session.client_id`），
    取哪个都一样，但 client_id 不用再解析一次证书。
    """

    async def authenticate(self, *, session: Session) -> bool | None:
        ok = await super().authenticate(session=session)
        if ok and not session.username:
            session.username = session.client_id
        return ok


def build_acl(cfg: ServerConfig) -> tuple[dict[str, list[str]], dict[str, list[str]]]:
    """契约 §3 的那张表，生成出来。

    返回 (publish_acl, subscribe_acl)。

    设备**不能**发布 `state`（防被撬开后伪造「一切正常」覆盖服务端判断），
    也**不能**订阅自己的 `up/`。HA **只能**订阅 `state`。
    """
    publish: dict[str, list[str]] = {"svc": [f"{ct.PREFIX}/#"]}
    subscribe: dict[str, list[str]] = {
        "svc": [f"{ct.PREFIX}/#"],
        "ha": [ct.sub_all_state()],
    }
    for d in cfg.devices:
        # 设备只能发 up/# 和自己的 lwt
        publish[d.id] = [
            f"{ct.PREFIX}/{d.id}/up/#",
            f"{ct.PREFIX}/{d.id}/{ct.LWT}",
        ]
        subscribe[d.id] = [f"{ct.PREFIX}/{d.id}/dn/#"]
    return publish, subscribe


def build_broker_config(cfg: ServerConfig) -> dict[str, Any]:
    """组装 amqtt 的 BrokerConfig dict。

    用 dict 而不是 BrokerConfig 对象：`plugins` 段是 dotted-path → config 的
    映射，dict 形式和文档一致，也更容易在测试里改一两项。
    """
    publish_acl, subscribe_acl = build_acl(cfg)
    # amqtt 的坑 #1：publish-acl 空 = 全放行（`topic_checking.py:69-71`）。
    # 断言的是「每个已配置设备都有 publish 条目」而不是「字典非空」——
    # 后者恒真（build_acl 里已经塞了 svc 键），那条断言从来不会触发。
    missing = [d.id for d in cfg.devices if not publish_acl.get(d.id)]
    if missing:
        raise RuntimeError(f"这些设备没有 publish ACL 条目，会导致发布全放行：{missing}")

    listeners: dict[str, Any] = {}
    if cfg.mqtt.plain_bind:
        host, port = split_bind(cfg.mqtt.plain_bind, 1883)
        listeners["plain"] = {"type": "tcp", "bind": f"{host}:{port}"}
    if cfg.mqtt.tls_bind:
        host, port = split_bind(cfg.mqtt.tls_bind, 8883)
        listeners["tls"] = {
            "type": "tcp",
            "bind": f"{host}:{port}",
            "ssl": True,
            "certfile": cfg.mqtt.certfile,
            "keyfile": cfg.mqtt.keyfile,
            "cafile": cfg.mqtt.cafile,
        }
    if not listeners:
        raise RuntimeError("一个 listener 都没配，broker 起来也没人能连")

    # ⚠ amqtt 硬性要求 listeners 里有一个叫 `default` 的：
    # `BrokerConfig.__post_init__` 直接 `self.listeners["default"]`，缺了就 KeyError。
    # 它同时是「其余 listener 未显式设置的字段的兜底值」（`ListenerConfig.apply`）。
    # 所以把第一个 listener 改名为 default，而不是额外插一个 —— 额外插一个会
    # 真的开一个 0.0.0.0:1883 的明文口，那是把开锁凭据往公网上放。
    first = next(iter(listeners))
    listeners["default"] = listeners.pop(first)

    if cfg.mqtt.mode == "cert":
        # 用自己的子类而不是上游的 UserAuthCertPlugin：后者不设 session.username，
        # 导致 ACL 查 "anonymous" 然后一律拒（见 DeviceCertAuthPlugin 的说明）。
        auth_plugin: dict[str, Any] = {
            "ebike_server.broker.DeviceCertAuthPlugin": {
                "uri_domain": cfg.mqtt.cert_uri_domain,
            }
        }
    else:
        auth_plugin = {
            "amqtt.plugins.authentication.FileAuthPlugin": {
                "password_file": cfg.mqtt.password_file,
            }
        }

    return {
        "listeners": listeners,
        # 键名必须是下划线：BrokerConfig 也是 dacite strict 填充的，
        # `timeout-disconnect-delay`（文档里的写法）会 UnexpectedDataError 启动失败。
        "timeout_disconnect_delay": 2,
        "plugins": {
            # 显式给 plugins 会**完全替换**amqtt 的默认插件集，
            # 其中包括 `AnonymousAuthPlugin(allow_anonymous=True)` —— 正是我们要甩掉的。
            **auth_plugin,
            "amqtt.plugins.topic_checking.TopicAccessControlListPlugin": {
                # ⚠ 键名必须是**下划线**且订阅 ACL 必须叫 `acl`。
                # amqtt 0.12.0 的坑（本机实测）：插件构造函数会 warn
                # 「'acl' 已弃用，请用 'subscribe-acl'」，但它的 Config dataclass
                # 里**只有** publish_acl 和 acl 两个字段，而 dict 形式的 plugins
                # 配置是 dacite strict 模式填充的 —— 传 `subscribe_acl` 或
                # `publish-acl`（连字符）都会直接 UnexpectedDataError 启动失败。
                # 所以这里按实际字段名写，忍受那条 DeprecationWarning。
                "publish_acl": publish_acl,
                "acl": subscribe_acl,
            },
            "ebike_server.broker.IngestPlugin": {},
        },
    }
