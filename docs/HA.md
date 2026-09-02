# Home Assistant 集成

> 装在 **HA 那台机器**上，不是跑服务端的这台（DESIGN.md §9.4「跟 HA 解耦」）。
> 实体清单和字段来自 [`MQTT-CONTRACT.md`](MQTT-CONTRACT.md) §7。
> 这解掉了 DESIGN.md §11 #7。

## 1. 装

把整个目录拷到 HA 的配置目录：

```bash
# 在 HA 那台机器上
scp -r homeassistant/custom_components/ebike_tracker \
    root@ha-host:/config/custom_components/
```

**先配好 HA 自己的 MQTT 集成**（集成 → 添加 → MQTT），因为本集成
`dependencies: ["mqtt"]`，复用 HA 的连接而不自己开一条：

| 字段 | 填什么 |
| --- | --- |
| Broker | 服务端的主机名或 IP，**必须和服务端证书的 CN 一致**（`ebike-server init --hostname` 填的那个） |
| Port | **8883** |
| Username | `ha` |
| Password | `init` 打印的 HA 口令 |
| 高级 → CA 证书 | 上传服务端的 `ca.crt` |

取 `ca.crt`：

```bash
docker compose run --rm --entrypoint sh ebike-server -c 'cat /data/certs/ca.crt' > ca.crt
```

⚠ **HA 把 CA 存成「内容」而不是路径**（`mqtt/util.py` 的
`async_create_certificate_temp_files`，它每次启动把内容写到临时目录）。
所以要通过 UI 上传，手改 `.storage` 时填路径会 `X509: NO_CERTIFICATE_OR_CRL_FOUND`
——这个我踩过。

然后重启 HA，集成 → 添加 → 搜「电瓶车」，填设备 id（默认 `bike01`，
要和固件的 `CONFIG_EBIKE_DEVICE_ID` 及服务端配置一致）。

## 2. 实体

一个设备卡片下 9 个实体：

| 实体 | 类型 | 说明 |
| --- | --- | --- |
| `device_tracker.电瓶车_bike01` | GPS | 地图上的车。`gps_accuracy` 就是**误差圈**，这是 DESIGN.md §9.4 那条验收 |
| `sensor.…_电池电压` | voltage | V，1 位小数 |
| `sensor.…_电量` | % | 由电压曲线插值。**只是个指示**——铅酸电压在负载下会塌，同一电压能差 20% 容量 |
| `sensor.…_定位精度` | m | 和 `gps_accuracy` 同源，单独出一个是为了能画历史曲线 |
| `sensor.…_定位方式` | 文本 | 「卫星定位」/「基站定位」 |
| `sensor.…_最后上报` | timestamp | **服务端时钟**，不是设备时钟（契约 §5.6：设备拿到 NITZ 前 `t` 是 0） |
| `binary_sensor.…_在线` | connectivity | **服务端超时算的，不是 LWT**（契约 §4.2） |
| `binary_sensor.…_移动中` | moving | 加速度计事件 + 位移共同判 |
| `binary_sensor.…_车锁` | lock | **可能是「未知」**，见下 |

### 三个刻意的设计

**1. 没有 `lock` 实体，只有 `binary_sensor`。**
`lock` 实体带开锁按钮，而远程开锁绕过 NFC 挑战应答、服务端和固件**两边默认都关着**
（契约 §6.1）。给一个按了会 403 的按钮比不给按钮更糟。真要远程开锁，
在 HA 里配一个显式的 `rest_command`（见 §4）。

**2. 车锁状态可能显示「未知」，这是对的。**
位置反馈微动开关是选配的（`lock.c` 里没接只 warn）。没接就永远拿不到
`lock_state` 事件，契约 §7 的 `lk` 就是 `null`。
**显示「未知」而不是「没锁」**——后者会让人以为车没锁好而白跑一趟。

**3. 车离线时实体仍然「可用」。**
车离线时服务端**照样发** state（算出 `on=false` 并 retain），所以实体是可用的，
只是「在线」显示为 off。真正 unavailable 只有「HA 刚启动还没收到 retain」或
「MQTT 断了」。混在一起的后果是车一离线所有实体变 unavailable、历史图断掉、
**看不到最后一次在哪**——那恰恰是车被偷时最需要的信息。

## 3. 坐标系

契约 §7 两套坐标都发，集成里可选（集成 → 配置）：

| 选项 | 用在 |
| --- | --- |
| **GCJ-02**（默认） | 国内地图卡（高德/腾讯） |
| WGS84 | HA 默认的 OSM 底图 |

选错的表现是车偏几百米。改选项会重载集成。
两套坐标都放在 `device_tracker` 的属性里（`la`/`lo`/`gla`/`glo`），方便对着底图核。

⚠ **GCJ 字段缺失时不回落到 WGS84** —— 静默回落会让车偏几百米而界面上看不出异常，
宁可显示「没有位置」。

## 4. 远程开锁（要自己配，默认没有）

三处都要打开才生效：

1. 服务端配置 `allow_remote_unlock: true`
2. 固件 `CONFIG_EBIKE_ALLOW_REMOTE_UNLOCK=y`
3. HA 里加一个 `rest_command`：

```yaml
# configuration.yaml
rest_command:
  ebike_unlock:
    url: "http://<服务端>:8080/cmd/bike01/unlock"
    method: POST
    headers:
      Authorization: !secret ebike_api_token   # "Bearer xxx"
```

⚠ 这条命令的实际含义是「**谁能登进 HA 谁就能开你的车**」。
本地手机 NFC 开锁不受影响，它完全离线（DESIGN.md §5.2）。

⚠ 服务端的 8080 默认只绑 `127.0.0.1`，HA 在另一台机器上的话要先解决网络可达性
（反向代理或改绑定），**别直接把 8080 摊到公网**。

## 5. 怎么验的

固件那份是「没编译过」，**这份不是** —— 在本机用真的 HA 跑起来验过：

```bash
docker run -d --name ha-test --network ebike-test \
  -v /tmp/hatest/config:/config ghcr.io/home-assistant/home-assistant:stable
```

HA **2026.8.3 / Python 3.14.6**，服务端和 HA 各一个容器、走同一 docker 网络、
设备走 **TLS 8883** 并校验 hostname。实测结果：

```
DEBUG custom_components.ebike_tracker.coordinator: 已订阅 ebike/v1/bike01/state
INFO  custom_components.ebike_tracker: 电瓶车 bike01 已接入，等 retain 的 state
DEBUG custom_components.ebike_tracker.coordinator:
      state: on=True mo=parked la=31.230416 gla=31.228474 a=8.0 v=54.2 pct=86 lk=True
```

9 个实体全部注册进 entity registry，取到的值：

```
device_tracker.…_bike01        = not_home  latitude=31.228474 (GCJ-02) gps_accuracy=8 source=卫星定位
sensor.…_battery_voltage       = 54.2
sensor.…_charge                = 86
sensor.…_location_accuracy     = 8.0
sensor.…_fix_source            = 卫星定位
sensor.…_last_report           = 2026-09-01T15:11:02+00:00
binary_sensor.…_online         = on
binary_sensor.…_moving         = off
binary_sensor.…_lock           = off        （off = 锁着；HA 的 LOCK device class 语义是反的）
```

**「重启 HA 立即有位置」那条验收成立**：订阅到收到 retain 的 state 之间
**0.86 秒**（`23:20:37.040` → `23:20:37.896`）。

再发一组「锁开了 + 位移 + 基站定位降级」，实体跟着变：

```
binary_sensor.…_lock  = on           （锁开了）
binary_sensor.…_moving = on
device_tracker        gps_accuracy=1000  source=基站定位
```

边界情况在真 HA 里单独测过 coordinator：`not json` / 空串 / `[1,2,3]` / `null` /
截断的 JSON / 非 UTF-8 字节 **全部被容忍**（只记日志、不置 `has_data`、不触发实体刷新），
乱序的旧 state 被丢弃，同一秒的更新被接受。

## 6. 一致性测试

服务端那边有 **28 条**测试盯着这份集成（`server/tests/test_ha_contract.py`）：

```bash
cd server && ../.venv/bin/python -m pytest tests/test_ha_contract.py -q
```

它们防的是「服务端改了 state 字段名，HA 侧还读旧名」——那种错误的表现是
HA 里所有实体变 unknown 而日志一句话都没有。最关键的一条
`test_every_ha_field_exists_in_real_state` 拿**服务端真算出来的 state**
逐个检查 HA 要读的 15 个键。

另外几条钉住了上面那些设计决定：`test_no_lock_entity_platform`（不许加 lock 平台）、
`test_ha_uses_exact_topic_not_wildcard`（不许用通配符订阅，理由是契约 §4.3 那个
amqtt 缺陷）、`test_lk_is_none_without_lock_sensor`（没接反馈开关时必须是 None）。

## 7. 没做的

| 缺什么 | 说明 |
| --- | --- |
| **轨迹历史** | HA 只订 `state`，历史轨迹在服务端的 `/track`。要在 HA 里画轨迹得另写一个走 HTTP 的实体，或者用 HA 自己的 recorder 记 `device_tracker` 的历史（够用） |
| **地理围栏在 HA 侧配** | `gf` 字段透传成属性了，但围栏本身在服务端配置里改。HA 的 zone 是另一套，两套并存会互相矛盾 |
| **告警自动化** | 没预置。`binary_sensor.…_移动中` + HA 自己的 zone 就能拼出「车离开家了」，但那是用户的策略不是集成的事 |
| **多车** | 加第二个 config entry 就行（集成支持），**但先看契约 §4.3** 那个 amqtt retain 泄漏 |
