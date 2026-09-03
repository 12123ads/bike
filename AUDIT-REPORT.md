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
- [第二轮审计（修复后复查，7 项新发现）](#第二轮审计2026-09-03修复后复查)
- [第二轮修复状态（已全部修完）](#第二轮修复状态2026-09-03分支-fixaudit-findings)

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

---

## 第二轮审计（2026-09-03，修复后复查）

> 方式：主审计员逐文件读 + 4 个可执行 PoC。目标是**第一轮修复引入的新代码**（当时最大的未审计面）以及第一轮判「无发现」的模块。
> 基线：服务端 301 passed / 1 skipped，固件零警告，ztest 29 passed，bsim 8 断言全过 —— **以下每一条都在全绿基线上成立。**
>
> 7 个并行子代理全部超时未产出，本轮结论全部由主审计员自己读代码 + 跑 PoC 得到。子代理的空转不计入证据。

### R1 [高] `modem_publish` 可重入自身，`static hexbuf` 被内层覆盖 → 上行发出错误字节 + 无界递归爆栈 — `modem.c:865, 905`

这是本轮最严重的一条，**和第一轮 M2/M3 是同一处代码但不同缺陷**，第一轮没看到。

```c
/* modem.c:865-869 —— 填缓冲在拿锁之前，且缓冲是 static */
static char hexbuf[PROTO_MAX_PAYLOAD * 2 + 1];
for (size_t i = 0; i < len; i++) snprintf(&hexbuf[i*2], 3, "%02X", payload[i]);
/* modem.c:879 拿锁；modem.c:905 在等裸 '>' 的循环里： */
(void)consume_urc(line);        /* ← 这里能一路走回 modem_publish */
```

重入链（全部是本仓库代码，无第三方）：
`modem_publish:905 → consume_urc → handle_msub:246 → dn_cb → uplink.c on_downlink → handle_cmd → ack_downlink:183 → modem_publish`

两个独立后果，都用 PoC 证明了：

**(a) 线上字节损坏。** `hexbuf` 和 `cmd` 都是 `static`，内层把它们重写一遍；外层在 `modem.c:913` 才 `uart_write_str(hexbuf)`，写出去的已经是内层的内容。PoC 输出（结构逐行照抄 modem.c:852-926）：

```
[外层 cmd]   AT+MPUBEX="ebike/v1/bike01/up/tele",1,0,41
[内层 cmd]   AT+MPUBEX="ebike/v1/bike01/up/ack",1,0,19
[内层 hex]   7B2269223A22632D3432222C226F6B223A317D
[外层 hex]   7B2269223A22632D3432222C226F6B223A317D   ← 应该是 up/tele 的 82 个字符
外层声明 len=41（模组要收 82 个 HEX 字符），实际写出 36 个
```

模组按外层声明的 `len=41` 去取 82 个 HEX 字符，只拿到 36 个，**后面 46 个字符从下一条 AT 命令的字节里补** —— 一次损坏同时毁掉这条上行和紧随其后的 AT 命令流。

**(b) 无界递归爆栈。** `k_mutex` 同线程可重入（`kernel/mutex.c`：`owner == _current` 时只是 `lock_count++`），所以内层拿得到锁、不会死锁，只会继续往下递归。用编译产物 `/tmp/fwbuild/nrf52840/zephyr/zephyr.elf` 的真实帧大小算：

| 帧 | 大小 |
|---|---|
| `modem_publish`（9 寄存器 + `sub sp,#652`） | 688 B |
| 重入环 `consume_urc`+`on_downlink`+`ack_downlink` | 192 B |
| **每多一层** | **880 B** |

`uplink_thread_fn`+`uplink_cycle` 打底 216 B，`UPLINK_STACK_SIZE=4096`（main.c:68）：

```
第 1 层  904 B    第 3 层 2664 B
第 2 层 1784 B    第 4 层 3544 B
第 5 层 4424 B  ** 越过 4096 **
```

**触发条件**：设备上线时服务端把**全部**未确认下行一次性冲刷（`broker.py:95 on_broker_client_connected → on_device_online → flush_downlinks` 逐条 `internal_message_broadcast`）。队列里积 4 条以上就到第 5 层。而队列积压恰恰是常态 —— 省电档下设备几十分钟才上线一次，期间每条 `/cmd`、每次密钥轮换都在排队。

爆栈的落地行为：`CONFIG_MPU_STACK_GUARD=y` 会抓到，但 `CONFIG_RESET_ON_FATAL_ERROR` **未设**，默认 `k_sys_fatal_error_handler` 走 `arch_system_halt()` → `arch_irq_lock()` + `for(;;)` 死转。**没有看门狗**（`.config` 里无 `CONFIG_WATCHDOG`）。所以结果是**车彻底变砖直到人工断电**：不上报、不响应下行、BLE 开锁也停（那也要 CPU）。

`uplink.c:150-155` 的注释显示作者**察觉到了这条路径的存在**（「不要在下行回调里用 `publish_retry`」「`at_lock` 可重入，所以 publish 本身没事」），但结论下错了：可重入正是问题所在，而共享 `static` 缓冲和栈深度都没被考虑。

### R2 [高] 限速仍然按可伪造的 `X-Forwarded-For` 计数 —— M6 的修复只做了一半 — `web.py:35-42`

第一轮报告 M6 那一栏写的是「**限速改用真实 socket 地址（XFF 只做日志）**」。代码里没有这件事：

```python
def client_ip(request: Request) -> str:
    fwd = request.headers.get("x-forwarded-for")
    if fwd:
        return fwd.split(",")[0].strip()      # ← 仍然优先用它当限速键
    return request.client.host if request.client else "?"
```

实际落地的只有 `websession.py:90-101` 的 `max_tracked_ips=1024` 上限（那条是真的，防住了内存无界增长）。PoC 结果：

```
A. 不带 XFF 连打 12 次错 token  -> [401×10, 429, 429]     限速生效
B. 每次换一个伪造 XFF 打 60 次  -> 全部 401，一个 429 都没有
C. B 之后再换一个 XFF          -> 401（限速对攻击者完全无效）
```

更糟的是 `max_tracked_ips` 上限本身成了**放大器**：`_prune_all` 按 `_fails[k][0]`（首次失败时间）丢最旧的键，攻击者轮换伪造来源把表灌满时，**真实用户的限速记录会被挤掉**。PoC 第 D 段：真实用户已被 `blocked`，攻击者灌 20 个伪造 IP 之后 `blocked()` 返回 `False` —— 上限修复反而给了攻击者一条清掉别人限速状态的路。

`web.py:37-38` 的注释「伪造它最多是绕过自己的限速」是错的：限速的作用对象就是攻击者，「绕过自己的限速」等于限速不存在。而注释里说的威胁模型（`websession.py:74-76`：每次请求一次 `compare_digest` 可用来打满 CPU）恰恰是被这条绕过打开的。

**触发条件**：任何能连到 8080 的人，一个请求头。默认只绑 `127.0.0.1`，但文档推荐放反向代理后面 —— 那时 XFF 就是外部可控的。

### R3 [中] `/cmd` 的 `args` 只挡大小不挡形状，非法值会走完「入队 → 下发 → 设备拒绝 → 无限重发」全程 — `contract.py:401-404`

`build_cmd` 对 `args` 的唯一检查是整条报文的字节数：

```python
body: dict[str, Any] = {"id": dn_id, "c": cmd}
if args:
    body["a"] = args        # 键名、类型、范围一概不看
return _dumps_downlink(body)
```

PoC 实测被接受的：`{"s":"900"}`（字符串）、`{"s":-5}`、`{"s":1e30}`、`{"bogus":1,...}`（未知键）、`{"a":{"b":{"c":[1,2,3]}}}`（嵌套）。只有撑到 256 字节才被拒。

设备侧一定会拒（`nvstore_set_report_interval` 判 60~86400，`proto.c get_int` 对 `"900"` 返回 `-EINVAL`），但拒绝路径的代价是：**`mark_acked` 只在 `ok=1` 时销账**（`service.py:204-213`），`ack ok=0` 的行 `acked` 仍是 0 → 留在 `pending_downlink` → **每次设备上线都重发一遍**，永久。第一轮 M1 在这里加的 400 只挡了大小这一维。

顺带一个解析器性质（proto.c 探针 G 段）：`get_int` 的 `tmp[24]` 对 `s=1e30` 这种超长数字串，`strtoll` 饱和成 `INT64_MAX`，再 `(int32_t)` 截断 —— 不会崩，但值完全不可预测。真正的防线应该在服务端。

### R4 [中] NMEA 校验和可被绕过：`strtol` 解析失败返回 0，恰好 XOR=0 的坏行被当好行 — `gnss.c:87-88`

```c
long given = strtol(star + 1, NULL, 16);
return (long)sum == given;      // strtol 失败也返回 0，不区分
```

PoC：`"$GGAAGG*--"` 的 XOR 恰好是 `0x00`，校验和字段 `"--"` 无法解析 → `strtol` 返回 0 → **`checksum_ok` 返回 true**，且这一行含 `"GGA"`（`gnss.c:225` 用 `strstr` 判类型），所以会进 `parse_gga`。

实际影响有限但真实：进 `parse_gga` 后 `field()` 取不到第 2~8 个字段（只有 1 个逗号）→ 直接 `return`，不会写坏坐标。所以**当前不会导致车瞬移**，但那是靠 `parse_gga` 的字段数检查兜住的，不是校验和挡住的 —— 文件顶部注释（`gnss.c:76`）声称「必须验 —— 一个坏掉的纬度字段会让车瞬移」，而这道防线有洞。9600 baud 上的误码要恰好让整行 XOR 归零且校验和两字符都非十六进制，概率低但不为零。

同段另一条（`gnss.c:156`）：`f->valid = (f->lat != 0.0 || f->lon != 0.0)` —— 几内亚湾那个 (0,0) 点会被判成无效解。这个取舍本身合理（比误信垃圾好），但和服务端 `modem_lbs` 的 (0,0) 防护（第一轮 M8）是同一个哨兵值语义，值得统一记一笔。

### R5 [中] 三张数据表只按时间 prune，`prune` 从不 VACUUM — `store.py:375-385`

`prune` 对 `loc`/`tele`/`event` 只 `DELETE ... WHERE t_srv<?`，默认保留 365 天。按默认 `report_interval=900`（每天 96 轮）估：

| 期限 | 行数 | 估算体积 |
|---|---|---|
| 30 天 | loc 2 880 + tele 2 880 | ≈ 0.5 MB |
| 365 天 | loc 35 040 + tele 35 040 | ≈ 5.6 MB |

单车场景下这个量级无害，**所以不是容量问题**。真正的问题是 `DELETE` 不回收文件：页进 freelist 供复用，`ebike.db` 只增不减。容器用的是具名卷，用户看到的是「卷一直在长」而没有任何手段缩回去 —— 没有 `VACUUM` 入口，也没有文档说明。多车或缩短上报周期后会放大（周期改 60 秒 = 15 倍）。

### R6 [中] `esc()` 只转义 5 个字符，用在 HTML 属性位置不够 — `web_assets.py:331-334, 357`

```js
function esc(v) { return String(v).replace(/[&<>"']/g, ...); }
// web_assets.py:357 —— 拼进 class 属性
return '<li class="ev-' + esc(e.kind) + '">...'
```

转义表覆盖 `& < > " '`，对**引号包裹的属性值**是够的。当前所有属性位置都带引号（已逐个核对：`class="ev-..."` 是唯一一处），所以**现在是安全的**。记这一条是因为 `e.kind` 走的是契约闭集 + 第一轮 `_EVENT_DETAIL_SPEC` 双重校验，而注释自称「第二道墙：上游若放松校验，这里不会变成存储型 XSS」—— 那个承诺对无引号属性位置不成立，未来有人在这里加一个 `<li data-x=` + esc(...) 就会破。

另外 `mapFailed`（web_assets.py:474-481）和 `main().catch`（:586-588）把 `e.message` 直接拼进 `innerHTML` 而**不过 `esc()`**。当前这两处的 message 只来自本地 `new Error('...')` 字面量和 `fetch` 的网络错误，不含服务端数据，所以够不成注入；但 `api()`（:279）把服务端的 `body.detail` 塞进 `new Error` —— 而 `body.detail` 在 `_check_dev`（web.py:193）里含用户提供的 `device_id`。链路是：`GET /api/state/<恶意 id>` → 404 detail 含该 id → `refreshState` 的 catch 只 `console.warn`（:487，不进 DOM）。**所以目前断在最后一步**，是运气不是设计。

### R7 [低] `ACL` 里 `svc` 账号有 `ebike/v1/#` 全权限，但没有任何代码为它创建凭据 — `broker.py:178-182`

```python
publish: dict[str, list[str]] = {"svc": [f"{ct.PREFIX}/#"]}
subscribe: dict[str, list[str]] = {"svc": [f"{ct.PREFIX}/#"], "ha": [ct.sub_all_state()]}
```

`__main__.py` 的 `init` 只为每个 `cfg.devices` 和 `"ha"` 建口令（:85, :93），**没有 `svc`**。服务端自己发下行走的是 `broker.internal_message_broadcast`（不占客户端连接、不过 ACL）。所以 `svc` 这两条 ACL 是死配置。

第一轮 H1 删掉的是 `ingest` 里的 `"svc"` 白名单，ACL 里这两条留下了。风险不是「现在能被用」（没凭据就登不上），而是「**口令文件是用户可编辑的**」—— 谁在 passwd 里手加一行 `svc` 就拿到全 topic 树的读写权，包括伪造 `state`（`build_acl` 的 docstring 明确说设备不能发 `state`，理由是防伪造「一切正常」）。删掉这两条是零成本的。

### 交叉验证：两条推测被证伪，如实记录

审计过程中有两条看起来成立的推理，PoC 跑完是不成立的，记下来避免以后重复怀疑：

1. **`_auth_users` 在 session take-over 时被误清** —— 推理是：同 `client_id` 重连走 `broker.py:571` 的 take-over 分支，旧连接的 `CLIENT_DISCONNECTED` 会 `pop` 掉新连接刚写入的条目（`broker.py:100`），之后该设备所有上行 `username=None` 被静默丢弃。**PoC 证伪**：take-over 后 `_auth_users` 仍是 `{'bike01': 'bike01'}`，新连接的 `q=2` 正常落库。amqtt 的 take-over 走 `old_session[1].stop()` 而不是完整的 disconnect 事件路径。
2. **`GET /state` 有写副作用会打乱 retain 顺序** —— `publish_state` 确实会写库和发 MQTT，但 `state_changed` 比对挡住了：PoC 连打 100 次 `GET /state`，broker 写次数 **0**。只有 `force=True`（仅进程启动路径）每次都写。

### 测试覆盖核对：第一轮有三项没有真正被钉住

| 项 | 声称的验证 | 实际情况 |
|---|---|---|
| M3 `flush_events` 锁纪律 | 「固件编译 + 逐条 review」 | **无任何测试**。`grep flush_events\|ev_lock` 在 `server/tests/` 和 `firmware/tests/` 全无命中。改回持锁发送不会有任何东西变红 |
| M7 `counter` 清零 | 「固件编译 + bsim」 | **bsim 没测这条**。`ble_unlock_bsim/src/main.c` 没有 `del_secret`/`wipe` 调用；8 条断言里没有「删用户再加新用户」的场景。把 `users[i].counter = 0` 删掉，8 条照样全过 |
| M2 迟到 URC | ztest 变异体验证过（7 passed / 2 failed） | **真的钉住了**，这条是好的 —— 列在这里作为对照：M3/M7 应该做到这个程度 |

H2（MCONFIG 参数位）的两条文本测试（`test_firmware_contract.py:683-709`）是有效的：`assert '\\"%s\\",\\"%s\\",\\"%s\\",1,1,\\"%s\\"' in line` 直接钉住参数形状，`assert ",1,1,60," not in line` 专门防旧 bug 复现。M10、M11、M12 都有真 e2e/集成测试，其中 M11 的 `test_publish_state_calls_are_serialized` 带探针自检（先证明加锁后零交错，再绕过锁断言必须交错），是这批测试里质量最高的一条。

### 本轮判定无发现的部分

| 模块 | 结论 |
|---|---|
| `service.py` `_locked` 拆分 | 逐个调用点核对完毕：`_publish_state_locked` 的三个调用方（`ingest:161`、`ingest_lwt_will:293`、`publish_state:373`）全部持锁；`_flush_downlinks_locked` 的三个（`_on_up:193`、`enqueue_cmd:318`/`enqueue_secret:326`、`flush_downlinks:336`）同样。`on_device_online:300` 调公开版且不持锁，正确。无重入死锁 |
| `broker.py` 事件回调名 | 四个 `on_broker_*` 逐个比对 `amqtt/events.py` 的 `BrokerEvents` 枚举：`message_received`、`client_connected`、`client_disconnected`、`retained_message` 全部命中。`plugins/manager.py:150-156` 确认是 `getattr(plugin, f"on_{event}")` 反射，拼错即静默失效 —— 没有拼错的 |
| `IngestPlugin` 异常传播 | `fire_event` 用 `asyncio.ensure_future` + `_clean_fired_events` 回调，异常只在 DEBUG 级别打印且**明确不让它弄死 broker**（`manager.py:322` 的注释）。`on_broker_message_received:74-76` 自己也 catch 了。一条畸形报文不会影响别的设备 |
| `read_line` 的 `-EMSGSIZE` 流同步 | 三个调用方（`at_cmd_expect:408`、`modem_publish:890`、`modem_poll:937`）全部 `continue`。`read_line:156-161` 在溢出时继续读到 `'\n'` 才返回，流真的是同步的 |
| `unlock.c` 锁覆盖 | `users[]` 的每一处访问都在 `users_lock` 内（`find_user_locked` 注明持锁调用、`snapshot_users_locked` 同）；flash 写和 `result_cb` 都在锁外。`handle_unlock:265-295` 的临界区只有 HMAC（纯内存）。第一轮 H3 修得完整 |
| `nvstore.c` 锁纪律 | `save_users_locked` 的两个调用方（`counter_work_fn:116`、`nvstore_save_users`）都持 `lock`。`nvstore_flush:146` 直接调 `counter_work_fn(NULL)`，它自己会拿锁 —— 不是持锁调用，正确 |
| `ble_unlock.c` 并发模型 | `busy` 用 `atomic_cas` 独占 `pending.buf`；入队失败时 `bt_conn_unref` + `atomic_clear` 都退回来了（:256-264），不会永久卡死。`CONFIG_BT_MAX_CONN=1` 是 `unlock.c` 无锁 nonce 状态的前提，`test_ble_single_connection_for_lockfree_unlock_state` 钉住了它 |
| `lock.c` 释放脉冲 | `release_work` 在 sysworkq 上，和 `still_work`/`counter_work`/`sense_work` 共队列。理论上 flash 写（`CONFIG_SOC_FLASH_NRF_RADIO_SYNC_MPSL=y`，要等 MPSL timeslot）能推迟脉冲撤销。但 `nvstore` 单次 `settings_save_one` 是毫秒量级，相对 `PULSE_MS=500` 的余量足够，**且第一轮 M3 把长阻塞的 `publish_retry` 从任何工作队列上挪走了** —— 现在 sysworkq 上没有秒级阻塞项。判无发现 |
| `proto.c` 编码层 | `finish()`（:35-42）正确处理 `snprintf` 返回「想写的长度」语义（`>= len` 即截断即 `-ENOMEM`）；每个可选字段的增量 `snprintf` 都单独判了 `(size_t)(n+k) >= len`。无把截断当成功的路径 |
| `derive.py` NULL 承受 | M8 之后 `tele["volt"]` 为 NULL 时，`state["v"]`/`state["pct"]` 是 `None`（键存在值为 null，不是缺席）。`volt_to_pct:29` 首行就 `if volt is None: return None`。PoC 验证了「有 v → 无 v」的过渡会让 `state_changed` 返回 True 正常重发 |
| HA 缺字段承受性 | `sensor.py` 全部走 `coord.get(...)` 返回 `None` → HA 显示 unknown，无 `KeyError` 路径；`device_tracker.py:70-81` 坐标缺失返回 `None`（且明确不回落 WGS84）；`location_accuracy:96-99` 缺失回落 `0.0` 并在 docstring 里解释了为什么由坐标侧表达「没位置」。`binary_sensor.py` 三个 `value_fn` 都用 `isinstance(v, bool)` 守着。**一个字段缺席打挂所有实体的路径不存在** |
| `config_flow.py` | `DEVICE_ID_RE` 与契约 `contract.py:22` 逐字一致（`^[a-z0-9-]{1,32}$`），非法输入走 `errors[CONF_DEVICE_ID]` 而不是异常；`async_set_unique_id` + `_abort_if_unique_id_configured` 防重复添加 |
| `store.py` SQL | 全参数化（唯一的 f-string 是 `prune` 的表名，来自硬编码元组 `("loc","tele","event")`）；`reserve_dn_seq` 的 `INSERT...ON CONFLICT...RETURNING` 单语句原子 |

### 本轮建议修复顺序

1. **R1**（固件重入）—— 唯一会让车变砖的一条。最小修法：`modem_publish` 期间挂一个「正在发布」标志，`consume_urc` 在标志置位时只把 `+MSUB` 行**排队**不投递（或直接丢弃并依赖服务端重发，契约 §4.1 说下行都是幂等的）。顺带把 `hexbuf`/`cmd` 的填充移到拿锁之后。
2. **R2**（限速绕过）—— 一行：`client_ip` 改用 `request.client.host`，XFF 只写日志。这本来就是第一轮报告承诺的东西。
3. **R3**（args 形状校验）—— 按指令建一张 args schema，`interval` 只接受 `{"s": int in [60,86400]}`。防的是「永久重发」而不是设备崩溃。
4. **R7**（删 `svc` ACL）—— 两行删除，零成本。
5. **R5**（VACUUM 入口）、**R4**（校验和严格化）、**R6**（`esc` 用法收紧）—— 都不紧急。
6. **补 M3/M7 的测试** —— 这两项现在靠 review 撑着，而 review 已经在 R1 上漏了一次同一个文件的问题。

---

## 第二轮修复状态（2026-09-03，分支 `fix/audit-findings`）

**7 项全修完并验证。** 报告正文保留原始发现描述 —— 审计和修复是两件事。

| # | 修了什么 | 验证方式 |
|---|---|---|
| R1 | `handle_msub` 只入队（4 槽位环形队列），`dn_cb` 只在 `modem_poll()` 的 `deliver_downlinks()` 里调 —— 那里不持 `at_lock`、不在任何命令流里。`dn_delivering` 挡住投递期间的再次投递 | ztest 3 条新用例；**变异体：改回就地投递立刻 3 条红**（9 passed / 3 failed）。契约测试补两条文本护栏（`dn_cb(` 只允许一个调用点、`at_cmd_expect`/`modem_publish` 里不许出现 `deliver_downlinks()`） |
| R2 | `client_ip` → `rate_key`，只用 `request.client.host`；XFF 只进日志（新增 `log_source`）。`LoginThrottle._prune_all` 逐出时**跳过已达上限的键** | web 测试 2 条；变异体：改回优先 XFF 立刻红（「每次换一个伪造 XFF 就绕过了限速：[401]」）；改回按最旧丢也立刻红 |
| R3 | 新增 `contract.CMD_ARGS`：每个指令的 `a` 逐键逐值校验（类型、区间、未知键全拒）。区间抄固件的真实判据 | 契约测试 15 组坏输入 + 覆盖表（`CMD_ARGS` 必须与 `COMMANDS` 一一对应）；API 测试验 400；变异体：拆掉校验 2 条红 |
| R4 | `checksum_ok` 自己解两位 HEX（`hexval`），要求恰好两位且行尾干净 —— 不再用 `strtol`（失败返回 0 撞上真实 XOR） | **新增 `firmware/tests/gnss_nmea` 11 条**；变异体：改回 `strtol` 版立刻红，报「1.033333,2.050000（9 星）被当成有效定位」 |
| R5 | `SCHEMA` 加 `auto_vacuum=INCREMENTAL`（**必须在 `journal_mode=WAL` 之前**），`prune` 之后调新增的 `reclaim()` | store 测试 2 条（断言**文件大小真的降下来**）；两个变异体各红一次：PRAGMA 顺序调回去 → `auto_vacuum=0`；去掉 `fetchall` → 「1994752 → 1990656 字节」 |
| R6 | `mapFailed` 内部 `esc()`（调用方传纯文本）、`main().catch` 转义 `e.message`、设备下拉改 DOM 构建 | 前端测试 2 条（带深度感知的 sink 扫描器，不是脆弱正则）；变异体：去掉 esc 立刻红 |
| R7 | 删掉 ACL 里的 `svc` 全权限账号（`init` 从来没为它建过口令，但口令文件是用户可编辑的）。顺带把「`publish_acl` 非空」的断言补上 —— 删掉 `svc` 之后它不再恒真 | certs 测试 2 条（无 `svc`、无整棵树权限、空设备列表必须启动失败） |

**顺带补上第一轮欠的两笔测试债**（那两项当时写的验证方式是「固件编译 + review」，而 review 已经在 R1 上漏了同一个文件的问题）：

- **M3 锁纪律** → 新增 `firmware/tests/uplink_events` 7 条。判据是「publish 卡住时另一个线程还能不能入队」而不是「事件发出去了」—— 持锁的实现最终也会入队，只是晚三分钟。变异体：改回持锁发送 1 条红，且**那条用例耗时从 0.5 s 变 10 s**，探针真的被卡住了。
- **M7 counter 清零** → 新增 `firmware/tests/unlock_slots` 8 条。场景就是「手机丢了换一台」：旧 uid 用到 counter=5000 → `del` → 新 uid 占同一槽位 → 从 counter=1 开始必须能开锁。**后果不是拒绝攻击者而是拒绝一个合法的新手机**，而日志说「counter 未递增」看起来像重放。反向护栏：同 uid 轮换密钥时 counter **必须保留**（否则旧重放报文重新可用）。变异体：改回不清 counter 5 条红。

**总验证量**：服务端 pytest **313 passed / 1 skipped**（第一轮后 301，本轮新增 12 条）；固件编译零警告（`-DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y`，FLASH 27.50% / RAM 23.55%）；native_sim ztest **7 套 58 passed**（新增 3 套 26 条）；BabbleSim 8 条断言全过；真 broker + 真 MQTT 客户端 + 真 HTTP 的烟雾测试逐条走通（4 条下行一次性冲刷按序到达且都远小于 `LINE_MAX`、坏 args 一条都进不了队列、`v` 缺席落 null、prune 后文件 1.99 → 0.08 MB、遗嘱 `lwt=1` 落库）。

**修 R5 时踩到的两个静默失效**，都只有量文件大小才看得出来，记在代码注释里：

1. `PRAGMA auto_vacuum` 必须在 `journal_mode=WAL` **之前**。反过来写它静默失效（读回来是 0），回收永远不生效且没有任何报错。
2. `PRAGMA incremental_vacuum` 的回收发生在**步进游标**的过程里。只 `execute` 不 `fetchall` 只走一步 —— 1583 个空闲页里回收 1 个。

**修 R1 时避开的一个坑**：`deliver_downlinks` 里槽位必须**在回调返回之后**才释放。先 `dn_count--` 再回调的话，队列满时内层 `handle_msub` 算出的入队位置正好等于 `dn_head` —— 会覆盖**正在投递的那一条**。`test_queue_full_drops_newest_keeps_queued_intact` 钉住这个。
