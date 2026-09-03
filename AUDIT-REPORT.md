# ebike-tracker 项目检查报告

> 日期：2026-09-03
> 方式：4 个 GLM-5.3 子代理并行只读审计（服务端 / 固件 / 协议契约 / HA 集成）+ 交叉验证关键结论（含 2 个可执行 PoC）。**未修改任何项目文件。**
>
> 基线：服务端 pytest **260 passed / 1 skipped**；固件可编译（零警告）且过 BabbleSim 射频仿真（8 条断言）。
> 标注约定：`[推断]` = 未经运行时验证、基于源码推理；「已实证」= 在隔离环境实际运行复现。

---

## 目录

- [高危（3 项）](#高危)
- [中危（12 项）](#中危)
- [低危](#低危)
- [HA 集成专项](#ha-集成专项)
- [契约一致性与测试盲区](#契约一致性与测试盲区)
- [已审查无发现的模块](#已审查无发现的模块)
- [建议修复优先级](#建议修复优先级)
- [修复状态（已全部修完）](#修复状态2026-09-03分支-fixaudit-findings)

---

## 高危

### H1. MQTT ingest 身份校验可被 `client_id` 冒充绕过 — `service.py:133`

`ingest` 用**客户端自报的 client_id**（而非认证用户名）做身份判断，且白名单含 `"svc"`：

```python
if client_id is not None and client_id not in {dev, "svc"}:
    log.warning("丢弃：client %r 无权发 %s", client_id, topic)
    return
```

两个致命点：

1. MQTT 协议层的 client_id 完全由客户端 CONNECT 包自报，与用户名/凭据无关（password 模式下 FileAuthPlugin 只校验 username/password）。任何持合法凭据的连接把 client_id 填成 `"svc"` 即可通过该检查，向**任意已配置设备 id** 的 topic 注入报文。
2. amqtt 的 `MESSAGE_RECEIVED` 事件在 ACL 检查**之前**触发（`amqtt/broker.py:753`，项目注释自己也写了）——broker 的 publish ACL（按 username 查）拦得住广播转发，**拦不住落库**。

而 `"svc"` 白名单本身是死代码：服务端自身发布走 `internal_message_broadcast` → `_broadcast_message`，直接进 broadcast 队列，**不触发 MESSAGE_RECEIVED、不经过 ingest**。白名单保护的是一条不存在的路径，却打开了一条真实的后门。

**已实证（隔离环境 PoC）**：

- 持最低权限 `ha` 账号凭据 + `client_id="svc"` 连接 → 发布假位置到 `ebike/v1/bike01/up/loc` → `loc` 表成功写入 `(q=999, lat=0.0, lon=0.0)` ✅
- 持 `bike01` 凭据 + `client_id="svc"` → 发布 hello → `fw` 被污染为任意字符串 ✅

危害链：`ha` 凭据（本应只读、只订 state）可伪造 `up/loc`（篡改车辆位置、触发 geofence/移动误判）、伪造 `up/ack ok=1` 销账任意下行（配合 M10 甚至不需要 `"svc"`）、伪造 `up/event`（假 lock_state/lowbatt）、污染 `fw`/`kid`。注释声称要防的「一个能登录的客户端可以往别的设备 id 下灌报文」恰恰成立。

cert 模式不受影响：`DeviceCertAuthPlugin` 父类断言 SAN device id == client_id，身份绑定成立。

**修复方向**：ingest 校验基于 session 认证身份（username）而非自报 client_id；至少删掉 `"svc"` 白名单；补一条「ha 凭据不能注入上行」的 e2e 测试。

### H2. `AT+MCONFIG` 参数错位：遗嘱 topic 被配成 `"60"`，LWT 链路断裂 — `modem.c:616-619`

```c
int n = snprintf(cmd, sizeof(cmd),
    "AT+MCONFIG=\"%s\",\"%s\",\"%s\",1,1,60,\"%s\",\"{\\\"lwt\\\":1}\"",
    PROTO_DEVICE_ID, PROTO_DEVICE_ID, CONFIG_EBIKE_MQTT_PASSWORD,
    TOPIC_LWT);
```

固件发了 **8 个参数**，合宙 Air780EP 官方手册的 MCONFIG 只有 **7 个参数位**（keepalive 属于 `AT+MCONNECT=<clean_session>,<keepalive>`，`modem.c:658` 已单独配过）。按官方参数表对位：

| 位置 | 官方语义 | 固件实际填的 |
|---|---|---|
| 4 | will_qos | `1` ✓ |
| 5 | will_retain | `1` ✓ |
| 6 | will_topic | **`60`**（无引号，官方要求字符串加引号） |
| 7 | will_message | **`ebike/v1/bike01/lwt`**（topic 串被当成遗嘱内容） |
| 8 | — | `{"lwt":1}` 多余 |

后果二选一，都严重：

- **[推断] 模组宽容解析**：遗嘱注册到 topic `"60"` → 非优雅断连时 broker 把遗嘱发到 `60` → 服务端 `parse_topic` 因前缀不对直接丢弃 → **`lwt=1` 永远不会出现**，契约 §4.2「lwt=1 且超时 = 可能真出事了（剪线）」整条链路失效，HA 侧 `ATTR_UNGRACEFUL` 永远 false。
- **[推断] 模组严格解析**：MCONFIG 回 ERROR → 连接失败 → 设备完全离线。

被架空的三方约定：`docs/MQTT-CONTRACT.md` §4「retain 位由设备 CONNECT 时的 will_retain 决定，固件在 AT+MCONFIG 里配」——描述的行为不存在；`contract.py:57-61` `LWT_RETAIN=True` 前提不成立；`service.py:245-258` `_on_lwt` 的 lwt=1 路径成死路。

**修复方向**：删掉 `60`，恢复 7 参数形式 `AT+MCONFIG="id","id","pw",1,1,"<topic>","{\"lwt\":1}"`。参数错位本身确凿（源码与手册对位）；模组实际行为待实机验证。

### H3. 固件 `users[]` 密钥表跨线程无锁并发读写 — `unlock.c:18, 69-120` vs `uplink.c:284-288`

`ble_unlock.c:100-107` 声明的单线程安全论证只覆盖 `cur_nonce/nonce_valid/selected`，但 `users[]` 和 `key_set_id` 同样是 unlock.c 的无锁静态状态，却被两个线程访问（已 grep 确认 unlock.c 内无任何锁/原子量）：

- **unlock_workq**：`apdu_work_fn → unlock_handle_apdu → handle_unlock` —— `find_user()` 拿裸指针、读 `u->secret` 做 HMAC、写 `u->counter`（unlock.c:198, 216, 238）
- **uplink 线程**：`modem_poll/at_cmd_expect → consume_urc → on_downlink → handle_secret → unlock_set_secret/unlock_del_secret/unlock_wipe_secrets`（uplink.c:284-288）—— `memcpy(u->secret, ...)`、`crypto_wipe(u->secret,...)`、改 `valid/uid`（unlock.c:86, 106-108）

可观察后果：

1. `find_user()` 返回指针后，uplink 线程可 `del` 该槽位并让 `set` 复用之给**另一个 uid** —— `handle_unlock` 后续的 `u->counter = counter` 写进**已换主**的槽位，把新用户的 counter 往回写 → 打开一个重放窗口；
2. `unlock_set_secret → nvstore_save_users` 的 `memcpy(user_mirror, users, ...)`（nvstore.c:185）可与 unlock_workq 写 counter 并发 → 撕裂的 counter 被持久化；
3. HMAC 期间 secret 被 wipe → 半新半旧的密钥参与验证。

注释自证矛盾：unlock.h「前提是**只有那一个线程**碰它们」—— 密钥管理 API 恰恰不在那个线程上。

**修复方向**：给 `users[]` 加锁，或把密钥管理操作也排队到 unlock_workq。

---

## 中危

### M1. `LINE_MAX=512` 静默截断下行 URC — `modem.c:30, 120-122`

`read_line` 逐字节丢弃超出 512 的行内容，无任何标记；而 `dn_payload[PROTO_MAX_PAYLOAD]`（3900）表明设计意图是收完整下行。**两个数字差 16 倍**：512 字节行缓冲装下 `+MSUB: "<topic>",<len>,"<hex>"` 之后只剩 236 字节的实际 payload 容量（HEX 翻倍），而代码声称能收 3900。

**修复时实测确认了真实故障模式**（初稿写的「`"s":900` 截成 `"s":90` 被 `get_int` 正常解析 → 上报周期静默改错 10 倍」**不成立**）：截断点落在 HEX 区内，而 HEX 区只有 `0-9A-F`，不可能再出现一个字面量 `"` —— 所以 `handle_msub` 必然走「找不到 payload 结束引号」分支，那个分支 `return true`（当作已消化）**且一条日志都不打**。

实际症状因此是**下行凭空消失**：设备侧无任何记录，服务端侧 `/pending` 的 `tries` 每次设备上线加一，永远等不到 ack。比「接受一个错值」更难查，但破坏面小一些（不会执行错误指令）。

`"s":900` 那条推理链本身是对的 —— `get_int` 确实会接受被截断的 `"s":90`（宿主机验证过）—— 只是**触发不了**，因为报文在到达解码层之前就被丢掉了。这也说明防线必须在读行那一层。

### M2. `at_cmd` 子串匹配可被迟到的 URC「答到」— `modem.c:393-395, 847-850`

```c
bool matched = (strstr(line, expect) != NULL);   /* expect = "OK" */
```

`"SEND OK"` / `"CONNECT OK"` 是带外 URC（文件自己的注释承认），且不在 `consume_urc` 的消化列表里。时序：publish 等 `SEND OK` 超时放弃 → URC 迟到滞留 ring → 下一条 `at_cmd`（如 `AT+CPOWD=1`）读到 `"SEND OK"` → `strstr(...,"OK")` 命中 → **命令未执行却返回成功**。后果链：`AT+CPOWD=1` 假成功 → 不走 `power_off_hard()` → 模组实际没关机持续耗电；`AT+MDISCONNECT` 假成功 → 在未拆的会话上跑 `stage_session`。

### M3. `flush_events` 持 `ev_lock` 跨网络 I/O — `uplink.c:366-392`

锁内调用 `publish_retry`，含 `modem_reconnect()`（modem.h 自述最坏约 3 分钟）。持锁期间被阻塞的线程：

- **LIS2DW12 驱动线程**：`trigger_handler → on_motion → uplink_queue_event`（motion.c:83-84 自己写着「别阻塞太久 —— 阻塞期间的中断会丢」）
- **系统工作队列**：`still_work_fn`、`sense_work_fn`（on_lock_state）、nvstore 的 `counter_work` 全排在 sysworkq 上 —— 包括 `lock.c` 的**锁释放脉冲** `release_work`，被卡即电磁锁持续通电（几百 mA）
- **unlock_workq**：`on_unlock_result → uplink_queue_event` —— 手机开锁应答在事件重发期间无响应

恰是「车被动了/被撬」事件密集、上行又最可能掉线重连的时刻。**修复方向**：锁内只做快照，锁外发送。

### M4. 网页事件面板存储型 XSS — `web_assets.py:338-343`

```javascript
ul.innerHTML = list.map(e => {
    if (e.detail.mg !== undefined) extra = ' ' + e.detail.mg + ' mg';
    ...
```

`up/event` 的 `detail` 内部值契约完全不校验（`contract.py:282-284` 只查是 dict）。设备（或经 H1 冒充设备的攻击者）发 `{"e":"motion","d":{"mg":"<img src=x onerror=fetch('/api/cmd/bike01/unlock',{method:'POST'})>"}}` → 落库 → 经 `/api/events` 回到浏览器 `innerHTML` 执行。Cookie 是 HttpOnly 拿不走，但 `samesite=strict` 挡不住**同源内**的 fetch —— 会话 cookie 会带上，XSS 可静默调用 `/api/cmd/.../unlock`（若开启）。与 H1 组合即完整攻击链：MQTT 凭据 → 伪造 event → 存储型 XSS → 网页会话内下发指令。

### M5. `compare_digest` 对非 ASCII 抛 TypeError → 未认证 500 — `web.py:69-70` / `api.py:47-48`

**已实证**：`POST /ui/login` body `{"token":"café"}` → 500；`GET /devices` 带原始 `0xE9` 字节的 Authorization 头 → 500。更糟的是 web 登录路径里该异常发生在 `throttle.record_failure` **之前**——攻击者可以无限发畸形 token 而不消耗限速额度（对比：发错误 ASCII token 十次就被锁 5 分钟）。修法：比较前转 bytes 或 `isascii()` 短路。

### M6. 登录限速可被 XFF 轮换彻底绕过 + `_fails` 无界内存增长 — `websession.py:82-94` / `web.py:38-42`

- `client_ip` 取 `X-Forwarded-For` 第一段，每个请求换一个随机 XFF 即无限次暴力尝试（注释「伪造它最多是绕过自己的限速」自我安慰的威胁模型不成立——绕过自己的限速正是问题本身）。
- `record_failure` 只增不删；过期条目仅在**同一 ip 再次失败**时顺带清理。**已实证**：10 万次伪造 XFF → `_fails` 恰好 100,000 条目，永不释放。

token 是 190 bit 随机值，暴力破解本不现实，故降为中危：日志放大器 + 内存增长原语。修法：无代理时按真实 socket 地址限速 + `_fails` 总量上限。

### M7. del 后复用密钥槽位继承旧 counter，新用户被锁在门外 — `unlock.c:73-110`

`unlock_del_secret` 只抹 secret、清 valid/uid，**counter 留在槽里**；`unlock_set_secret` 复用空槽时也不动 counter。注释「新用户槽位 counter 本来是 0」对复用槽位不成立：del 用户 A（counter=500）→ set 用户 B → B 的手机 counter 从 1 开始 → `counter <= u->counter` 全部被拒，B 直到 counter 超过 500 之前无法开锁。`unlock_wipe_secrets` 后重新配网同理。属「实现与注释前提矛盾」。

### M8. 假数据质量：电池 0.0V 与 LBS (0,0) — `uplink.c:467` / `modem.c:939-944`

- 电池读失败（mv<0）时 `.volt = 0.0f` 照发 → 服务端落库 → HA 显示 0V/0%。同一 struct 初始化里 `tmp` 用 `has_temp=false` 明确不发，注释自己写着「发一个假的 0 会被服务端当真值落库」——同一原则在 `v` 上被违反（服务端 `required=False` 本来允许省略）。
- `modem_lbs` 的 `+CIPGSMLOC` 字段为空时 `strtod` 返回 0.0，无校验 → 发 `la:0, lo:0` → 服务端范围校验拦不住 (0,0) → 地图上车瞬移到几内亚湾。对照：GNSS 路径有 `f->valid = (lat != 0 || lon != 0)` 防护，LBS 是同构场景的疏漏。

### M9. broker 代发的遗嘱到不了 `_on_lwt`，`lwt` 状态位失效 — `service.py:263`（消费方）

amqtt 处理非优雅断连走 `_handle_disconnect → _broadcast_message`，这条路径**不触发** `MESSAGE_RECEIVED` 事件——只有客户端主动 PUBLISH 才走 `_handle_message_delivery → MESSAGE_RECEIVED`。`IngestPlugin.on_broker_message_received` 是唯一调 `svc.ingest` 的入口，因此遗嘱 `{"lwt":1}` 永远不会进 `_on_lwt` 落库。**已实证**：带 will 的设备 SIGKILL 后 `dev_state.lwt` 保持 0。

`contract.py:57-59` 注释「端到端仍然对，因为 `_on_lwt` 靠收到消息更新 DB」与实际矛盾：靠遗嘱置 1 的那半边不存在。与 H2 叠加 = **lwt 功能全灭**（遗嘱配错 topic × 就算配对也进不了 ingest）。修复：`IngestPlugin` 补挂 `on_broker_message_broadcast` 事件，或在文档里明确承认该字段已死。

### M10. `up/ack` 不校验下行归属 — `service.py:190-192`

`downlink(dn_id)` 只按全局主键查，不比对 `row["dev"] == dev`。单车场景无害；加第二辆车后 bike02 可销账/拒绝 bike01 的下行（把 bike01 的密钥轮换标记为「已拒绝」）。与 H1 组合时单车也中招。修复一行：`if row and row["dev"] != dev: log+return`。

另：**cert 模式下 HA 无法认证**——`build_broker_config` 在 cert 模式只注册 `DeviceCertAuthPlugin`，`ha` 账号的口令没有任何插件认；`cmd_init` 却照常为 ha 生成口令。[推断：功能缺陷，单用户多用 password 模式]

### M11. `publish_state` 的 dev 锁纪律不一致 — `service.py:315` 等

锁的存在理由（注释）就是防「算 state → 比对 → 发布」乱序；但 `/state/{id}`（api.py:77）、`/devices`、`_tick` 三处调用 `publish_state` 都**不持锁**，只有 ingest 路径持。并发时两个 publish_state 交错 → 旧 state 晚于新 state 写 retain → HA 短暂显示陈旧值（≤30 秒自愈）。同类：`enqueue_cmd → flush_downlinks` 也不持锁，两次并发 HTTP 入队可交错发送使下行乱序——注释声称「顺序是硬要求：连续两次密钥轮换必须按序到达」。

### M12. 8883 无连接数上限且 CONNECT 读取无超时 — `broker.py:162-170` [推断]

amqtt 默认 `max_connections=-1`（无限制）；`ConnectPacket.from_stream` 无超时。8883 是公网暴露端口，慢速连接可无限堆积（每条一个 asyncio 任务 + TLS 会话内存）。listener dict 加一项 `max_connections` 即可。

---

## 低危

### 固件

| # | 位置 | 问题 |
|---|---|---|
| L1 | `modem.c:917` | `modem_lbs` 用裸 `!connected` 读原子量，违反本文件 41-47 行自己立的规矩（Cortex-M 对齐读侥幸不撕裂） |
| L2 | `proto.c:279, 323-348` | `get_int` 无 ERANGE 检查 + 三处窄化强转：`kid=65536→0`（错误 key_set_id 落盘并上报 hello）、`(int32_t)` 截断使 `s=4294992000` 绕成合法 interval 值 |
| L3 | `proto.c:214-235` | `find_val` 子串匹配：字符串**值**等于键名时先于真键命中（`{"id":"s",...,"a":{"s":900}}` → `find_val("s")` 先命中 id 的值） |
| L4 | `uplink.c:436` | `loc.sats = (int8_t)fix.sats`：uint8→int8，>127 变负数 → 判缺省 → 字段静默丢失 |
| L5 | `main.c:79` | `last_still_ms`（int64_t）sysworkq 写 / uplink 线程读，无同步，32 位 MCU 上可撕裂 [推断] |
| L6 | `prj.conf` | 未配置看门狗——无人值守防盗设备任何挂死都不可恢复直到电池耗尽 |
| L7 | `ble_unlock.c:148-149` | `active`/`notify_enabled` 跨线程裸 bool；竞态可致 `active==false` 但广播在跑 → **防跟踪的「静止关广播」静默失效** [推断] |
| L8 | `modem.c:429-454` | `power_on` 无条件 1.5s PWRKEY 脉冲；`CMD_REBOOT` 后模组仍在运行，脉冲打到已开机模组可能等于关机 → 每轮上报失败 [推断，取决于未核实的硬件参数] |
| L9 | `prj.conf:66` + `main.c:92` | 1KB 的 LIS2DW12 驱动线程栈里调 `bt_le_adv_start`，调用深度偏紧 [推断] |
| L10 | `proto.c:185-196` | `proto_enc_ack` 不转义 `dn_id`（当前 id 格式 `c-N` 安全，格式变化即雷） |
| L11 | `gnss.c:54-73` | `take_line` 跨唤醒丢失半行（`checksum_ok` 的 `$` 头检查兜住，只丢一句无错值） |
| L12 | `gnss.c:174-175` | heading 无 clamp：NMEA course=360.0 → 服务端 `hd ≤ 359` 拒收**整条**位置点 |
| L13 | `main.c:184-221` | System OFF 武装窗口：改 level 触发后 LIS2DW12 驱动回调仍挂同一根脚，锁存中断 + i2c 已 suspend 的组合可致活锁 [推断] |

### 服务端

| # | 位置 | 问题 |
|---|---|---|
| L14 | `web.py:64-66` | 登录体非 dict JSON（`"x"` / `[1,2]`）→ `AttributeError` → 500（已实证；同样不消耗限速额度） |
| L15 | `derive.py:88` | `derive_locked` 只查最近 50 条事件，事件洪流下 `lock_state` 被冲掉 → `lk` 误报「未知」；应 SQL 专查 `WHERE kind='lock_state'` |
| L16 | `certs.py:35-45` | `ensure_ca` 半状态（key 在 crt 缺）静默重签整棵 CA，已签证书全部失效且无日志提示根因 |
| L17 | `store.py:292-293` | `sync_dn_seq_to_existing` 的 GLOB `[cs]-*` 硬编码前缀闭集，加第三种前缀即静默漏扫 → 重启撞主键 500 |
| L18 | `certs.py:78,84` | 设备/服务端证书 10 年有效期，无吊销机制下放大「产线私钥」未决问题 |
| L19 | `api.py:93` | `/track` 无 `since > until` 校验（空转查询，有 limit 兜底） |
| L20 | `api.py:151-153` | `/secret` 的 `uid`/`kid`/`key_b64` 类型与内容不校验（`key_b64` 非 base64-32 字节 → 固件 ack badfmt 销账，闭环不崩但契约不 enforce） |
| L21 | `service.py:71-84` | `stop()` 关闭顺序的窗口：broker shutdown 期间事件回调若加任何 DB 写会撞已 close 连接（当前无害，埋坑） |
| L22 | amqtt `manager.py:315-323` | 插件事件回调异常仅在 DEBUG 级可见：`on_broker_client_connected → flush_downlinks` 若抛异常，该次下行冲刷静默丢失 |

### 契约 / 文档

| # | 位置 | 问题 |
|---|---|---|
| L23 | `contract.py:188` | `t` 上限 `2**31-1` vs 固件 `uint32_t`——2038 年后合法报文被拒（`q` 的 `2**32-1` 正确匹配） |
| L24 | `contract.py:221` vs `Kconfig` | `fw` 上限 16 字符只在服务端，Kconfig 无长度约束——超长版本号 → 整条 hello 被拒 → 下行队列不冲刷 |
| L25 | `contract.py:223` | 服务端不校验 `rst` 五值闭集（对 `e` 校验、对 `rst` 不校验，标准不一致；固件侧有测试钉住） |
| L26 | 文档 §5.4 | `lowbatt` 写「lv:1或2」，固件会发 lv=3；`unlock_deny` 写「uid 或 null」，固件只发数字 |
| L27 | `proto.c:22` / `proto.c:107` | `EV_BLE_ERR` 事件与 `proto_enc_loc_batch` 均无生产者（死枚举 / 契约预留，后者文档已承认） |
| L28 | 文档 §5.1/§6.2 | `kid` 的「服务端据此判断要不要补发」是措辞——实际只存不用，补发由 pending 队列无条件重发等价实现 |
| L29 | `api.py:118-135` | `/cmd` 的 `args` 完全透传无白名单；`{"s": 4294967396}` 固件截断为 100 接受，服务端 `_apply_acked_cmd` 却把原值写进 `report_interval` → 离线阈值与实际周期脱钩 |

---

## HA 集成专项

整体质量高于一般 custom integration（高 0 / 中 3 / 低 4）。

**中危：**

1. **listener 分发无逐回调异常隔离**（`coordinator.py:118-119`）：裸循环 vs core `DataUpdateCoordinator` 的逐回调 try/except。任一实体属性计算抛错（最现实触发点：`device_tracker.py:99` 的 `float(acc)`）→ 后续所有实体丢这次更新，且日志归因错位。
2. **`async_start` 半途失败泄漏连接状态订阅**（`__init__.py:33-41` + `coordinator.py:48-56`）：`async_subscribe_connection_status` 成功后 `mqtt.async_subscribe` 抛（如 MQTT entry 被禁用但 DATA_MQTT 残留）→ `_unsub_conn` 永不释放，每次 SETUP_RETRY 叠一个。
3. **`fw→sw_version` 竞速 + reload 反向清空**（`entity.py:54`）：device_info 只在实体 add 时读；reload 时新 coordinator `data={}`，若 retained state 未到 → `sw_version=None` 会把设备注册表里已有的版本**覆盖回空**（registry 的 `None != "0.1.0"` 语义）。与 HA.md「还没收到 hello 时是空的，那是正确的显示」不符。

**低危：**

4. 乱序保护的边界：非 int `t` 静默失效；服务端时钟回拨后所有新 state 被当过期丢弃直到墙钟追上（实体冻结且只有 debug 日志）[推断]。
5. `_mqtt_available` 只查 entry 存在性，disabled/setup_error 的 entry 也放行 → `KeyError('mqtt')` → SETUP_RETRY 无限循环。
6. LBS 定位也上报 `SourceType.GPS`（语义失真，不影响 zone 计算）。
7. 电量图标静态钉死 `mdi:battery-50`。

**已验证正确**：const.py 与契约 16 字段一一对应；manifest 与代码一致；中英翻译键树完全一致（22 键无单侧缺失）；LOCK device class 反转正确；`lk=null → unknown` 语义正确；unique_id 稳定性设计正确；「MQTT 连着 ∧ 收到过 state」的 available 双条件是与 core 行为对齐的正确决策。

---

## 契约一致性与测试盲区

**一致的部分**（简述）：topic 树九个后缀、三个闭集（事件 8 值 / 指令 7 值 / op 3 值）、state 16 字段三方对齐、可选字段的「负数哨兵→不发」语义、数值范围分层（4100/3900/20 点）、当前下行报文形状均在固件手写解析器能力范围内、`k` 字段脱敏、unlock 双侧默认关——均有测试钉住。

**测试盲区**（按危害排序）：

1. **modem.c 完全不在契约测试扫描范围**——H2 的直接漏网原因。AT 层至少该有一条正则钉住 MCONFIG 参数个数和 will_topic 位置。
2. **无「非优雅断连 → 遗嘱 → lwt=1」的 e2e 测试**——H2 + M9 在现有测试下全绿（lwt=1 只在单测里手工 `set_dev_fields` 模拟）。
3. **服务端宽松侧无负向测试**：`rst` 闭集、`key_b64` 长度/解码、`d` 内部结构。
4. **假值路径无测试**：`v=0.0`、LBS (0,0)、`hd=360`——「合法报文、错误数据」，现有测试只测解析成败。
5. `fw` 长度、2038 `t` 上限无边界测试。
6. `test_command_names_match` 依赖脆弱正则（要求表恰好以 "ping" 开头且 tab 缩进），表顺序调整即静默失配。

---

## 已审查无发现的模块

| 模块 | 结论 |
|---|---|
| `crypto.c/.h` | 常数时间比较交给 `psa_mac_verify`、易失密钥用后销毁、TRNG 失败即拒、`crypto_wipe` volatile 防优化。干净 |
| `battery.c/.h` | BUILD_ASSERT 护栏（分压比/源阻抗/阈值递减）扎实；门控「无论成败都关」正确；int64 换算正确 |
| `lock.c/.h` | ISR 只 submit、驱动信号撤除失败大声报、cancel+置零幂等 |
| `nvstore.c/.h` | 互斥纪律好（`save_users_locked` 必持锁）；users blob 长度+版本双校验 |
| `contract.py` | 解析边界扎实：长度先于解析、bool 显式排除、批量硬拒不截断、闭集校验、全参数化 SQL |
| `config.py` | 未知键报错、路径不存在抛错、token 空则强制生成、chmod 0600 |
| `geo.py` | 标准 GCJ-02 近似，数学正确 |
| `store.py` | 全参数化 SQL；`reserve_dn_seq` RETURNING 单语句原子递增无并发窗口 |
| `websession.py SessionStore` | 常数时间比较 + 惰性清理正确 |
| `Dockerfile` / `docker-compose.yml` | 非 root、tini、no-new-privileges、8080 只绑 127.0.0.1、1883 不映射、日志轮转，均正确 |
| amqtt 已知缺陷 | retain 泄漏给后连客户端已被 e2e 测试钉住并注明多车前必须解决，不重复报告 |

---

## 建议修复优先级

1. **H1**（一行白名单 + 一条 e2e 测试）：ingest 校验改用认证身份，删 `"svc"`。
2. **H2 + M9**（lwt 链路）：修 MCONFIG 参数；`IngestPlugin` 补挂 `on_broker_message_broadcast`；补遗嘱 e2e 测试 + modem.c 纳入契约测试扫描。
3. **H3 / M3**（固件并发）：`users[]` 加锁或密钥管理排队到 unlock_workq；`flush_events` 改快照+锁外发送。
4. **M5 + M6 + L14**（登录路径）：`isinstance(body, dict)` + ASCII 短路 + 按真实 socket 地址限速 + `_fails` 上限。
5. **M1 + M2**（modem 解析）：`LINE_MAX` 与 `dn_payload` 对齐或加显式超长拒绝；expect 匹配排除已知 URC 或改精确匹配。
6. **M4 + M10**（Web/ack 加固）：事件 detail 渲染前转义；ack 校验下行归属。
7. **M8**（数据质量）：`has_volt` 缺省不发；LBS 加 (0,0) 防护。

---

## 修复状态（2026-09-03，分支 `fix/audit-findings`）

**全部 3 高危 + 12 中危 + 相关低危已修完并验证。** 报告正文保留原始描述（含被实测推翻的 M1 故障模式，那一条已在原地更正并说明差异）——审计和修复是两件事，事后改写发现记录会丢掉「当时看到什么」这一层信息。

| # | 修了什么 | 验证方式 |
|---|---|---|
| H1 | ingest 身份判据改为**认证用户名**，删掉 `"svc"` 白名单 | e2e：ha 凭据 + `client_id="svc"` 灌遥测被拒；对照组「合法设备换 client_id 仍能发自己 topic」 |
| H2 | `AT+MCONFIG` 参数位纠正（7 参数位，keepalive 属于 `MCONNECT`） | 契约测试扫 `modem.c` 钉住 will topic 是引号字面量 |
| H3 | `users[]` 全部访问纳入 `users_lock`，快照后落盘 | bsim 8 条断言仍全过 |
| M1 | `LINE_MAX` 512→640、`dn_payload` 按新的 `PROTO_MAX_DN_PAYLOAD=256` 开、`read_line` 超长返回 `-EMSGSIZE` 并 LOG_ERR；服务端加 `MAX_DOWNLINK_BYTES` 构造期拦截（`/cmd`、`/secret` → 400） | ztest `modem_downlink` 9 条（含「超长丢整条」「丢完流仍同步」）；契约测试钉住三个数字自洽；HTTP 实测 400 |
| M2 | expect 恰好 `"OK"` 时要求**整行相等**；显式等 `SEND OK`/`CONNACK OK` 的子串路径不变 | ztest 变异体验证：改回旧写法后那两条立刻红（7 passed / 2 failed） |
| M3 | `flush_events` 改「锁内快照 → 锁外发送 → 锁内按 q 清槽」 | 固件编译 + 逐条 review；`q` 作唯一键防清错槽位 |
| M4 | `renderEvents` 改 DOM 构建 + `esc()`；服务端 `_EVENT_DETAIL_SPEC` 逐值校验（键、类型、范围） | 前端 harness 注入 `<img onerror>`/`<svg onload>`，断言 DOM 里零 `img`/`svg`/`script` 元素；浏览器实测同样结论 |
| M5+L14 | 登录路径 `isinstance(body, dict)` + ASCII 短路 | api/web 测试 |
| M6 | 限速改用真实 socket 地址（XFF 只做日志），`_fails` 加上限 | web 测试 |
| M7 | `unlock_del_secret` / `wipe` 清 `counter`（防槽位复用继承旧值把新手机锁在门外） | 固件编译 + bsim |
| M8 | `has_volt` 缺省不发 `v`；`modem_lbs` 挡 (0,0) 与越界坐标 | 宿主机 proto 测试 10 组；契约测试钉住「不得再出现 `.volt = ... : 0.0f` 无条件编码」；烟雾测试确认 `tele.volt` 落库为 NULL、网页显示 `—` |
| M9 | `IngestPlugin` 挂 `RETAINED_MESSAGE` 接 broker 代发遗嘱 → `ingest_lwt_will` | e2e：真客户端配 will 后 abort transport，`lwt` 落库为 1 |
| M10 | `up/ack` 校验 `dn_id` 归属本设备 | e2e：跨设备 ack 不销账 |
| M11 | `publish_state` / `flush_downlinks` **自己拿锁**，持锁调用方走 `_locked` 变体（`asyncio.Lock` 不可重入） | e2e 3 条：交错探针（含「绕过锁必须能观测到交错」的自检）、并发入队顺序、三路径并发不死锁 |
| M12 | 每个 listener 显式 `max_connections`（默认 32，可配） | 测试验配置字典 + `Server.semaphore` 真的建起来；另加「`write_default` 必须写全每个 dataclass 字段」的覆盖测试 |

**总验证量**：服务端 pytest **301 passed / 1 skipped**（基线 260，新增 41 条）；固件编译零警告（FLASH 27.46% / RAM 22.94%）；native_sim ztest 4 套 **29 passed**（新增 `modem_downlink` 9 条）；BabbleSim 8 条断言全过；服务端 + 设备 + 网页三方烟雾测试走通。

**顺带修掉的、审计没报的两个编译错**：`flush_events` 三处 `k_mutex_lock` 漏了 `K_FOREVER` 参数（M3 引入）、`unlock_del_secret` 的 `rc` 变量未使用（M7 引入）。两个都只在真编译时暴露 —— 审计基线说「固件可编译」，但那是审计**之前**的状态。
