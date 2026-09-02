# 服务端

内置 MQTT broker（`amqtt`）+ 落库 + 状态派生 + HTTP API，**单进程**。
契约见 [`MQTT-CONTRACT.md`](MQTT-CONTRACT.md)，硬件背景见 [`DESIGN.md`](DESIGN.md)。

## 1. Docker 跑（推荐）

```bash
cd /opt/ebike-tracker

# 1) 生成配置、证书、口令。--hostname 必须是设备连接时用的主机名或 IP，
#    它会写进服务端证书的 CN，模组的 AT+SSLCFG="hostname" 会校验它。
docker compose run --rm ebike-server init --hostname 192.168.1.10

# 2) 起
docker compose up -d
docker compose logs -f
```

`init` 打印的三组凭据**只显示一次**：设备口令（填进固件的
`CONFIG_EBIKE_MQTT_PASSWORD`）、HA 口令、HTTP API token。
后两个也在卷里的 `/data/config.json` 中，要看可以：

```bash
docker compose run --rm --entrypoint sh ebike-server -c 'cat /data/config.json'
```

`init` 可以重复跑：配置文件已存在就不动它（**不会换掉 api_token**），
证书已存在也不重新生成（换 CA 会让已经烧进模组的 `ca.crt` 失效）。
但**设备口令每次都会重新生成**——所以别在设备已经部署之后随手跑 `init`。

### 镜像与卷

| 项 | 值 |
| --- | --- |
| 镜像 | `ebike-server:0.1.0`，两阶段构建，**215 MB** |
| 基础镜像 | `python:3.13-slim`。**不用 alpine**：`cryptography` 和 `argon2` 在 musl 上没有 wheel，要现场编译，镜像反而更大 |
| 跑的用户 | `ebike` (1000:1000)，**非 root**，`no-new-privileges` |
| PID 1 | `tini`——`uvicorn` 和 `amqtt` 靠 SIGTERM 优雅退出，没有 init 进程时 `docker stop` 会等满 10 秒。实测 `docker compose stop` **0.8 秒**完成 |
| 数据 | 具名卷 `ebike-data` → `/data`：`config.json`、`ebike.db`、`certs/`、`passwd` 全在里面 |
| 健康检查 | 每 30 秒打一次 `/health`（不鉴权、不泄露信息）。用 python 而不是 curl，镜像里没装 curl |

⚠ **想用 bind mount 代替具名卷，要先 `chown 1000:1000`**。容器里跑的是 uid 1000，
挂一个 root 拥有的目录会 `PermissionError: /data/certs`（实测过）。

### 端口映射

| 映射 | 谁用 | 为什么这么绑 |
| --- | --- | --- |
| `8883:8883` | 设备（MQTT over TLS） | 车上的模组要连得到，必须对外 |
| `127.0.0.1:8080:8080` | HTTP API **和网页界面** | **只对本机**——它们能下发指令（含远程开锁，如果你打开了）。要远程访问就放反向代理 |
| 1883 **不映射** | — | 容器配置里 `plain_bind` 本来就是空的：docker 网络里开明文口等于把开锁凭据交给同网络的任何容器 |

⚠ **高德地图 key 的挂载有个权限坑**：容器以 uid 1000 跑，
而 `/root/gaode.key` 默认是 `0600 root:root`，挂进去读不到。
`chmod 644` 或改用 `EBIKE_GAODE_KEY` 环境变量，详见 [`WEB.md`](WEB.md) §3。

**HA 在另一台机器上**（DESIGN.md §9.4），所以它连 8883，用 `init` 生成的 `ha` 账号，
并且需要 `/data/certs/ca.crt`：

```bash
docker compose run --rm --entrypoint sh ebike-server -c 'cat /data/certs/ca.crt' > ca.crt
```

## 2. 不用 Docker 跑

```bash
python3 -m venv .venv && .venv/bin/python -m pip install -e server[dev]
.venv/bin/ebike-server -c myconfig.json init --hostname 192.168.1.10
.venv/bin/ebike-server -c myconfig.json run
```

不给 `-c` 时用 `$EBIKE_DIR/config.json`（默认 `/opt/ebike-tracker/run/config.json`）。
**给了 `-c` 但文件不存在会直接报错**，不静默退回默认值——路径打错却用默认配置跑起来
（绑到别的端口、用别的数据库）比失败危险得多。

裸跑时 `plain_bind` 默认 `127.0.0.1:1883`，方便本机调试；容器配置里是空的。

## 3. HTTP API

全部要 `Authorization: Bearer <api_token>`，除了 `/health`。

**另有一个网页界面**（地图 + 状态 + 指令），挂在同一个端口的 `/`，
用会话 cookie 而不是 Bearer —— 见 [`WEB.md`](WEB.md)。

| 方法 路径 | 干什么 |
| --- | --- |
| `GET /health` | 存活检查，不鉴权（不泄露任何东西） |
| `GET /devices` | 所有设备 + 当前 state |
| `GET /state/{id}` | 单台的 state（契约 §7 的那个结构） |
| `GET /track?dev=&since=&until=&limit=&offset=` | 轨迹，分页 |
| `GET /events?dev=&limit=` | 事件历史 |
| `GET /pending?dev=` | 未确认的下行队列。**payload 已脱敏** |
| `POST /cmd/{id}/{cmd}` | 下发指令（契约 §6.1 的闭集） |
| `POST /secret/{id}` | 下发密钥。响应**不回显** `key_b64` |

时间一律是服务端时钟（`t_srv`）——设备时钟在拿到 NITZ 之前是错的（契约 §5.6）。

**远程开锁默认 403。** 它绕过 NFC 挑战应答（契约 §6.1），要用得同时打开
服务端的 `allow_remote_unlock` 和固件的 `CONFIG_EBIKE_ALLOW_REMOTE_UNLOCK`。

## 4. 为什么 broker 内置

契约 §1 有完整论证。一句话：**你只有一辆车**，而外置 broker 的价值全部来自
多设备隔离。顺带解掉的：

- 不用改 `/opt/mqtt/config/mosquitto.conf` 加 `acl_file` 并重启在跑的服务
  （§11 #4 那个「动之前会先跟你确认」的操作没有了）
- 不用设计 ACL 规则形态和设备账号配发方式（§11 #5）
- 不用给 broker 加 8883 listener 和证书挂载卷

⚠ 顺带核实到：`/opt/mqtt` 的 Mosquitto **当前根本没在运行**——没有容器、
1883/9001 无监听、日志最后一行是 `mosquitto version 2.0.22 terminating`。
所以那个「动在跑的服务」的风险本来也不存在。

**代价**：broker 和业务同生共死（容器挂了两个一起没，`restart: unless-stopped`
会拉起来）；`amqtt` 是纯 Python，不是 Mosquitto 的性能级别（单车每 5~30 分钟
几条报文，差几个数量级，不构成问题）。

## 5. amqtt 的三个坑（本机读源码 + 实测确认）

这三条都写进代码注释了，改代码前先看：

1. **`publish-acl` 没配时发布一律放行**（`topic_checking.py:70`，为兼容 hbmqtt）。
   漏配是静默放开而不是报错。`build_broker_config()` 里断言了非空。

2. **配置键名必须是下划线，订阅 ACL 必须叫 `acl`。**
   `BrokerConfig` 和插件 Config 都是 dacite **strict** 模式填充的：
   `timeout-disconnect-delay`（文档里的写法）、`subscribe_acl`、`publish-acl`
   全都会 `UnexpectedDataError` **启动失败**。而 `acl` 这个正确的键名会触发一条
   「已弃用，请用 subscribe-acl」的 DeprecationWarning——那条警告是错的，忍着。

3. **`listeners` 里必须有一个叫 `default` 的。**
   `BrokerConfig.__post_init__` 直接 `self.listeners["default"]`，缺了就 KeyError。
   代码把第一个 listener 改名成 `default` 而不是额外插一个——额外插会真的开一个
   `0.0.0.0:1883` 明文口。

第四条在 [`MQTT-CONTRACT.md` §4.3](MQTT-CONTRACT.md)：**retain 消息会漏给任何
连上来的客户端**，绕过订阅和接收 ACL。当前无害（只有 `state` 被 retain，
下行一律不 retain），但**加第二辆车之前必须解决**。

## 6. 测试

```bash
cd server && ../.venv/bin/python -m pytest -q
```

**222 条全过、1 条跳过**（本机实测，22 秒）。分九层：

| 文件 | 条数 | 测什么 |
| --- | --- | --- |
| `test_contract.py` | 39 | 契约编解码。重点在**拒绝路径**——畸形报文被放过去会一路污染到 HA |
| `test_store_derive.py` | 23 | SQLite 去重、下行队列顺序、在线判定、移动判定、坐标转换 |
| `test_api.py` | 21 | 鉴权、参数边界、远程开锁默认关、密钥不回显、配置文件生成 |
| `test_certs.py` | 9 | 真的 TLS 握手、证书 CN、私钥权限 0600、换 CA 连不上 |
| `test_e2e.py` | 14 | 真起 broker + 真 MQTT 客户端：ACL 拦得住吗、retain 发给后连上的吗、下行队列真的冲刷吗 |
| `test_web.py` | 33+1skip | 网页鉴权边界、cookie 标记、登录限速、高德 key 读取失败要 warn |
| `test_web_frontend.py` | 16 | **前端 JS**：`node --check`、坐标转换和服务端逐点比对、假 DOM 渲染。见 [`WEB.md`](WEB.md) §7 |
| `test_firmware_contract.py` | 39 | **固件 C 源码和服务端的契约一致性**（文本层交叉检查，见 [`FIRMWARE.md`](FIRMWARE.md)） |
| `test_ha_contract.py` | 28 | **HA 集成和服务端的契约一致性**，见 [`HA.md`](HA.md) §6 |

几条值得单独提的：

- `test_loc_batch_fits_packet_limit` 把契约 §5.2 那个「20 点 HEX 后不超 4100 字节」
  的算术钉住了——改字段名或加字段会让它红。
- `test_online_by_timeout_not_lwt` 钉住契约 §4.2：省电档下设备每个周期都非优雅
  断连，拿 LWT 判在线会让好车 99% 时间显示离线。
- `test_docker_defaults_bind_all_and_disable_plaintext` 钉住容器配置：
  绑 127.0.0.1 在容器里等于谁都连不上，而开明文 MQTT 等于把凭据交给同网络的容器。
- `test_retained_state_leaks_to_device_on_connect` **断言一个缺陷存在**
  （上面第 5 节那条）。amqtt 修好了它会红，那时候删掉它并更新契约文档。

## 7. Docker 里手动验过的

不是测试，是本机在容器里实际跑出来的：

```
$ docker compose ps
ebike-server   Up (healthy)   127.0.0.1:8080->8080/tcp, 0.0.0.0:8883->8883/tcp
```

模拟设备和 HA **都走 TLS 8883** 连进容器，发 hello/loc/tele，HA 侧收到的 state
逐步长出来：

```
HA state: on=False la=None                                  ← 启动时恢复的 retain
HA state: on=True  la=None                                  ← hello 之后上线
HA state: on=True  la=31.230416 gla=31.228474 a=8.0
HA state: on=True  la=31.230416 gla=31.228474 a=8.0 v=54.2 pct=86
设备伪造的 state 被 ACL 拦住了（正确）
```

**重启后数据还在**（`docker compose restart` 之后 `/state` 和 `/track` 都返回了
重启前的点），`docker compose stop` **0.8 秒**干净退出（tini + SIGTERM 生效）。
HTTP 侧 `/health` 免鉴权返回 200，无 token 访问 `/state` 返回 401。

## 8. 服务端这边还没做的

| 缺什么 | 为什么 |
| --- | --- |
| **AGPS 星历分发** | R7 才做。届时它是唯一可以 retain 的下行（契约 §4.1） |
| **地理围栏的配置界面** | `state.gf` 字段和 `DeviceConfig.geofence` 都有了，但只能改配置文件 |
| **离线首次配对**（§11 #13） | 当前假设第一把密钥随固件烧进去 |
| **轨迹下采样** | `track_retention_days` 到期直接删，不下采样（§11 #9 的另一半） |
