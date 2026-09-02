"""电瓶车定位服务端。

内置 MQTT broker（amqtt）+ 落库 + 状态派生 + HTTP API，单进程。
契约见 docs/MQTT-CONTRACT.md，硬件与决策背景见 docs/DESIGN.md。
"""

__version__ = "0.1.0"
