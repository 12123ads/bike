# 固件（nRF52840 / NCS-Zephyr）

> ✅ **2026-09-02：这份固件现在编得过了，而且开锁通道跑过了射频仿真。**
>
> | | 结果 |
> | --- | --- |
> | `west build` | **通过**，零警告（`-DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y` 也过） |
> | FLASH | 218 688 B / 792 KB = **26.96 %** |
> | RAM | 62 192 B / 256 KB = **23.72 %** |
> | 产物 | `zephyr.uf2` 437 760 B，family `0xADA52840`，start `0x26000` ✓ |
> | BabbleSim 运行时测试 | **两侧都 Passed**，8 条断言（见 §5） |
> | 文本层契约测试 | `server/tests/test_firmware_contract.py` **58 条全过** |
>
> 工具链：NCS **v3.4.0**（Zephyr 4.4.0）+ Zephyr SDK 1.0.1，
> board target `promicro_nrf52840/nrf52840/uf2`。装在 `/opt/ncs`。
>
> ⚠ 仍然**没有在真硬件上跑过**。仿真验的是协议与逻辑，验不了
> 引脚接线、静态电流、GNSS/4G 的 UART 时序 —— 那些是 R0~R1 的事（§4）。
>
> 第一次构建暴露了 6 个真问题，全部已修，逐条记在 §3c ——
> **其中两个是「编得过但功能静默失效」，比编译错误危险得多。**

## 1. 文件

| 文件 | 干什么 | 依赖的 DESIGN.md 条目 |
| --- | --- | --- |
| `src/main.c` | 三态状态机 + 初始化顺序 + System OFF 路径 | §2.7 运动唤醒是唯一外部唤醒源 / §6 第 4 级 |
| `src/proto.c/.h` | 契约 v1 编解码 | [`MQTT-CONTRACT.md`](MQTT-CONTRACT.md) 全文 |
| `src/unlock.c/.h` | 挑战应答三重校验 | §5.2 协议 / §5.3 密钥管理 |
| `src/crypto.c/.h` | PSA HMAC + TRNG | §4.2（**注意**：CryptoCell 的开关由 cc3xx 库自己引用计数管，应用层不做，见 `crypto.h` 的长注释） |
| `src/ble_unlock.c/.h` | BLE GATT 开锁通道（CMD write / RSP notify） | §2.1 链路 / §2.2 GATT 表 / §2.5 必须 defer 密码学 |
| `src/nvstore.c/.h` | 密钥、counter、序号 q 的持久化 | §5.2 counter 掉电不回零 |
| `src/motion.c/.h` | LIS2DW12 运动/静止触发 | §3.7 阈值只能运行时设 |
| `src/battery.c/.h` | 门控分压采样（21:1）+ 四级欠压判定 + 两条布板护栏 `BUILD_ASSERT` | §3.6 比例由 VDD 3.3 V 和源阻抗 800 kΩ 定死 / §6 |
| `src/lock.c/.h` | 锁驱动 + 位置反馈 | §7 第 1 条（型号未定） |
| `src/gnss.c/.h` | NMEA 解析 + 电源门控 | §1 选 ATGM336H 的理由 |
| `src/modem.c/.h` | Air780EP AT 状态机 | §8 全节 |
| `src/uplink.c/.h` | 一轮上报的编排 + 复位原因 + 欠压档位 | §4.1 契约的下行排队 / §5.1 的 rst |

## 2. 怎么编译

本机 `/opt/ncs` 已经装好了（NCS v3.4.0 + Zephyr SDK 1.0.1）。三行：

```bash
export ZEPHYR_BASE=/opt/ncs/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk/zephyr-sdk-1.0.1
/opt/zephyrtool/bin/west build -p always \
    -b promicro_nrf52840/nrf52840/uf2 \
    -d /tmp/bbuild firmware/nrf52840
```

产物在 `/tmp/bbuild/nrf52840/zephyr/`：`zephyr.uf2`（拖进 UF2 盘）、
`zephyr.hex`（J-Link 烧）、`zephyr.elf`（gdb 用）。

想连警告一起当错误（CI 应该这么跑）：

```bash
... west build ... -- -DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y
```

**从零装工具链**（换机器时）：

```bash
apt-get install -y git cmake ninja-build gperf ccache dfu-util \
    device-tree-compiler wget python3-dev python3-venv xz-utils file \
    make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1
python3 -m venv /opt/zephyrtool && /opt/zephyrtool/bin/pip install west
mkdir -p /opt/ncs && cd /opt/ncs
/opt/zephyrtool/bin/west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.4.0 .
/opt/zephyrtool/bin/west update -o=--depth=1 -n --narrow
/opt/zephyrtool/bin/pip install -r zephyr/scripts/requirements-base.txt \
    -r zephyr/scripts/requirements-build-test.txt \
    -r nrf/scripts/requirements-base.txt
# Zephyr SDK：只要 minimal + ARM 工具链，不用下全套 3 GB
mkdir -p /opt/zephyr-sdk && cd /opt/zephyr-sdk
B=https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1
curl -L -o min.tar.xz $B/zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz
curl -L -o arm.tar.xz $B/toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz
tar xf min.tar.xz && tar xf arm.tar.xz -C zephyr-sdk-1.0.1
cd zephyr-sdk-1.0.1 && ./setup.sh -t arm-zephyr-eabi -h -c
```

⚠ **NCS 版本不能随便降。** 这块板子的 board 定义
（`boards/others/promicro_nrf52840/`）是 **Zephyr 4.1 才进上游的**，
对应 NCS ≥ v3.1.0；v2.x 上 `-b promicro_nrf52840` 直接找不到板子。
（实测：upstream v3.7.0/v4.0.0 都是 404，v4.1.0 起才有。）

配置项（设备 id、MQTT 主机、口令）要填：

```bash
/opt/zephyrtool/bin/west build -t menuconfig -d /tmp/bbuild   # 或直接改 prj.conf
```

必填的四项（`Kconfig` 里都有说明）：

| 配置项 | 填什么 |
| --- | --- |
| `EBIKE_DEVICE_ID` | 默认 `bike01`，要和服务端配置里的设备 id 一致 |
| `EBIKE_MQTT_HOST` | **必须等于服务端证书的 CN**——`ebike-server init --hostname` 填的那个 |
| `EBIKE_MQTT_PORT` | 默认 8883 |
| `EBIKE_MQTT_PASSWORD` | `ebike-server init` 打印的那一串，只显示一次 |

⚠ **本方案不用 NFC 引脚**（开锁改走 BLE，见 [`ADR-004`](ADR-004-ble-unlock.md)），
所以 `UICR.NFCPINS` **不需要写**，那个一次性动作整个消失。

但二手板或刷过其它固件的板子可能已经被改过，那会让 P0.09/P0.10 不是普通 GPIO
（本工程把它们留作余量，见 DESIGN.md §3.5）。先读一眼，只在确实被改过时才恢复：

```bash
nrfjprog -f NRF52 --memrd 0x1000120C     # 读 UICR.NFCPINS 看现状
# 只有需要改回 GPIO 时才做下面两步：--recover 会连带擦掉 REGOUT0（3.3V 设置）
nrfjprog -f NRF52 --recover
nrfjprog -f NRF52 --program nice_nano_bootloader-0.6.0_s140_6.1.1.hex --chiperase
```

**做了 `--recover` 就必须重刷 bootloader**，否则 REGOUT0 回落到 1.8 V，板子起不来。

## 3. 已知没做完的

按会不会咬人排序：

| # | 缺什么 | 后果 | 在哪 |
| --- | --- | --- | --- |
| ~~1~~ | ~~**断线重连阶梯**~~ | ✅ **2026-09-03 已修**：`modem_reconnect()` 三级 —— **1 级** 只重建 MQTT 会话（`MDISCONNECT` → `stage_session`，~5~10 s）；**2 级** 加 `CIPSHUT` + 重新附着（~15~70 s）；**3 级** `CPOWD` + PWRKEY 硬关机 + 完整重跑（~30~90 s）。`modem_connect()` 拆成 `stage_boot`/`stage_attach`/`stage_session` 三段供阶梯复用。**不做无限重试** —— 三级全败就返回错误让上层放弃本轮（§4.4：死循环会抽干车电池）。`connected` 从裸 `bool` 改成 `atomic_t`。`uplink.c` 的 `publish_retry()` 按 `modem_is_connected()` 而非 `rc` 决定要不要重连（-E2BIG/-ENOMEM 重连一百次也没用）。5 条 ztest（假模组 + AT 命令 trace）钉住「只重跑坏掉的那一层」，3 个变异体全被抓 | `firmware/tests/modem_reconnect` |
| 2 | **睡眠仲裁器 + RI 监听** | 省电档下第一条 AT 命令可能丢。**依赖 MAIN_DTR/MAIN_RI 脚号，而那个至今无来源**（§8.7 硬门禁 / §11 #18）。而且 overlay 里的 `modem_ri` 节点**没有任何 C 代码取用** —— `AT+CFGRI=1` 发了但主控侧没人监听那根线，「模组主动唤醒主控」目前是半条链 | 同上第 2 条 / overlay |
| ~~3~~ | ~~**NITZ → Unix 秒换算**~~ | ✅ **2026-09-03 已修**：`parse_modem_time()` 走 `timeutil_timegm64()`。两条要命的规则来自合宙 AT 手册 V1.6.8：**±zz 单位是 1/4 小时**（东八区 `+32` 不是 `+08`，当小时读差 24 h）、**hh:mm:ss 是本地时间必须减偏移**（不减差 8 h，⚠ 与 nRF91 相反，别照抄 NCS `date_time_modem.c`）。顺带修掉两个静默 bug：URC 前缀是 `+NITZ:` 不是 `+CTZV:`（这家族没有 CTZV，旧代码永远匹配不上）、`AT+CTZR=1` 是只读命令发过去只会回 ERROR。11 条 ztest 钉住，4 个变异体全部被抓 | `firmware/tests/modem_time` |
| 4 | **证书灌入**（`AT+FSCREATE`/`AT+FSWRITE`） | **这是 R6 的阻塞项**：`AT+SSLCFG="cacert",0,"ca.crt"` 引用的文件必须先在模组 FS 里，而现在没有任何代码灌它。**注意这条命令现在失败即中止连接**（以前是静默忽略），所以证书没灌就连不上，症状很明确 | 同上第 4 条 / §8.8 / §11 #24 |
| 5 | **PSM+ / PRO 双档** | 只有「连上」和「关机」两态。`dn/cmd` 的 `tier` 指令会明确 ack 失败而不是假装成功 | 同上第 5 条 / §11 #20 |
| 6 | **QoS1 超时重发队列** | 靠 `SEND OK` 一次确认，没有重发。这就是 §11 #22「设备侧要不要做补发队列」落地的位置 | 同上第 6 条 |
| 7 | **位置点的断线缓存** | 事件队列有 8 条（纯 RAM，掉电丢），但**位置点掉线不缓存** —— GNSS 定上了而 publish 失败，那个点就永久丢。契约 §5.2 的批量格式和 `proto_enc_loc_batch()` 都留好了，缺的是固件侧存储 | `uplink.c` / §11 #22 |
| 8 | **nRF 自己的 OTA** | 契约里没有 OTA topic。9600 baud 灌固件不现实，当前只支持 UF2 手动升级 | §8.8 / §11 #23 |

### 3b. 2026-09-02 修掉的（原来在上面这张表里或压根没记）

| 修了什么 | 原来的症状 |
| --- | --- |
| **`at_cmd_expect` 的 `resp` 被终结码覆盖** | 数据行先写 `resp`、随后 `"OK"` 又覆盖一次 → `AT+CGREG?` 的 `strstr(resp,",1")` 恒不匹配 → **`modem_connect` 必然在 60 s 后返回 `-ENETUNREACH`**，R6 第一天就撞墙；`AT+CSQ` 同理恒返回 -1 |
| **三条静默失败的 AT 命令** | `AT+MQTTMODE=1` 决定 payload 是 HEX 还是裸字节，静默失败 → 每条上行都畸形而日志只显示「等 '>' 超时」；`AT+SSLCFG="cacert"` / `"seclevel"` 静默失败 → **TLS 退化成加密但不认证**。三条都改成失败即中止 |
| **`rst` 硬编码 `"por"`** | 假数据。`CONFIG_HWINFO=y` 开着却零调用。现在走 `hwinfo_get_reset_cause()` → 五值闭集，读完立刻清 RESETREAS（累积寄存器，不清的话 `cause==0` 这个 POR 判据永远失效） |
| **`tmp` 硬编码 0** | 假数据，服务端会当真值落库（图上一条 0 °C 的直线）。现在靠 `has_temp` 条件发送，**当前固件不采温度所以整个字段缺席**（契约 §5.3 允许省）。不采的理由见下面 §6 |
| **欠压只做了 2 级**（§6 承诺 4 级，且无标注） | 只发 `lowbatt` 事件，上报周期不变、不进 System OFF。现在四级都有：1 级周期 ×4、2 级停周期上报（只留离线开锁）、3 级进 System OFF |
| **`crypto.h` 声称「用完关 CryptoCell」而实现里没有** | 注释-实现矛盾。核实后**结论是不需要做**：cc3xx 平台库自己引用计数管 `NRF_CRYPTOCELL->ENABLE`，连 abort 路径都关。头注释已改成记录这个结论 |
| **两个不存在的 Kconfig 符号** | `CONFIG_PM=y`（nRF52 的 SoC Kconfig 没有 `HAS_PM`）和 `CONFIG_GPIO_NRFX_INTERRUPT_DETECT_MODE_PORT=y`（整棵树里没这个符号）。Zephyr 把未满足依赖的 Kconfig warning 升级成 error，**这两行可能直接让构建失败** |
| **console 会抢 uart0 或起 USB** | 板级默认把 console 挂到 **USB CDC ACM** 并开机就初始化 USB 栈（毫安级，而本设计 USB 口根本不引出）。改成 RTT —— J-Link 本来就是必备工具 |
| **死代码** | `motion_prepare_for_sleep()`（空壳，注释还声称"驱动已经配好了不用额外动作"，那是错的）、overlay 里 6 个无人使用的 alias（7 个里只留 `motion-int`，因为 `enter_system_off()` 真的用 `DT_ALIAS` 取它）、`NVSTORE_USERS_VERSION`（定义了但不参与校验，现在真的写进落盘块并校验）、`firmware/luatos/` 空目录、`contract.py` 的 `sub_all_up()`、`web_assets.py` 的遗留占位标记 |

### 3c. 第一次真编译暴露的 6 个（2026-09-02，全部已修）

按危险程度排。**前两个最值得记住 —— 它们编得过、跑得起来，只是功能不工作。**

| # | 问题 | 症状 | 为什么之前查不出来 |
| --- | --- | --- | --- |
| 1 | **`CONFIG_LIS2DW12_WAKEUP` 没开** | `SENSOR_TRIG_MOTION` 整段裹在 `#ifdef CONFIG_LIS2DW12_WAKEUP` 里（`lis2dw12_trigger.c:172-180`），`sensor_trigger_set()` 返回 **-ENOTSUP** → `motion_init()` 直接失败 → **唤醒源根本不存在**，整机只能靠定时器醒，省电模式全废 | 只开了 `SLEEP`。两个 Kconfig 名字太像，而 `SLEEP` 又确实是 STATIONARY 要的 |
| 2 | **运动阈值单位错了 9.8 倍** | `SENSOR_ATTR_UPPER_THRESH` 的单位是 **m/s²**，驱动按 `sensor_ms2_to_mg()` 反算（`lis2dw12.c:240`）。原代码把 mg 直接塞进 `val1/val2` → 150 mg 传进去变成 **15 mg** → 低于一格 31.25 mg → 寄存器写 0 → **传感器持续触发**，等于常开上报 | 纯语义错误，类型对、编译过、`sensor_attr_set()` 还返回 0 |
| 3 | **overlay 文件名少了 `_uf2`** | board target 是 `promicro_nrf52840/nrf52840/uf2`，Zephyr 按 `zephyr_build_string()` 找 `promicro_nrf52840_nrf52840_uf2.overlay`。名字不对时**静默忽略整个 overlay**，编译期报 `__device_dts_ord_DT_N_ALIAS_motion_int_... undeclared` | 文件名规则依赖 board variant，没编译过就看不出来 |
| 4 | **overlay 少 include `dt-bindings/sensor/lis2dw12.h`** | `power-mode = <LIS2DW12_DT_LP_M1>` 里那个宏没定义 → `parse error: expected number or parenthesized expression` | 板级 DTS 不会带这个头进来 |
| 5 | **`CONFIG_HWINFO` 没写** | 只写了注释「`HWINFO_NRF` 是 default y，不用显式写」—— **错的**：`Kconfig.nrf` 是 rsource 在 `if HWINFO` 里面的，父开关不开子默认值不生效。链接期 `undefined reference to z_impl_hwinfo_get_reset_cause` | 那条注释是照着 Kconfig 文件读出来的，漏看了 `if HWINFO` 这一层 |
| 6 | **`CONFIG_BASE64` 没开** | 默认 n。链接期 `undefined reference to base64_decode`（proto.c 解 `dn/secret` 的密钥要用） | 同上，是「以为默认开着」 |

第 1、2 条现在各有一条测试钉着
（`test_kconfig_symbols_that_are_link_time_landmines` /
`test_motion_threshold_uses_ms2_conversion`），第 3、4 条也有
（`test_board_overlay_filename_matches_board_target` /
`test_overlay_includes_lis2dw12_dt_bindings`）。

### 3d. 顺带定掉的一个历史悬案：INT1/INT2 路由（§11 #2）

读 NCS v3.4.0 的驱动源码定论，**ADR-002 §1.6 是对的**：

| 触发 | 驱动写哪个寄存器 | 走哪根线 |
| --- | --- | --- |
| `SENSOR_TRIG_MOTION` | `ctrl4_int1_pad_ctrl.int1_wu` | **INT1** |
| `SENSOR_TRIG_STATIONARY` | `ctrl5_int2_pad_ctrl.int2_sleep_chg` | **INT2** |

出处 `lis2dw12_trigger.c:77-98`（两处 `route_set` 是不同寄存器）。

**而本板只接了 INT1**，所以 STATIONARY 的中断线物理上不存在。
更麻烦的是：`sensor_trigger_set(STATIONARY)` 会**返回 0** ——
驱动只写寄存器，从不检查那根引脚接没接。这是「配置成功、事件永不到达」。

当前处理：**不依赖它**。`main.c` 的 IDLE 判定用 `last_still_ms` 时间戳，
STATIONARY 只是挂上去（多写一个寄存器位，无害），日志里明确说了它不会来。
三条出路（接第二根线 / 写 `CTRL_REG7.int2_on_int1` 越过驱动 / 纯软件计时）
记在 `motion.h` 的头注释里，R2 拍板。

⚠ 附带发现：芯片支持把 INT2 事件并到 INT1（HAL 有
`lis2dw12_all_on_int1_set()`），**但 Zephyr 驱动从不调它，也没有对应的
DTS 属性** —— 走这条路必须自己越过驱动直接写 I2C 寄存器。

## 4. R0 阶段必须先验的两件事

这两条来自 §8.7，都是**硬门禁**，不验就往下做会白干：

1. **`AT^WAKEUPHEX` 和 `CSCLK=3` 在 AT 固件 V1011 上到底能不能用。**
   只需要一根 UART 线加一块模组。不能用的话，每条例行 URC 都会把主控从睡眠
   拽出来，**功耗预算直接崩，而且没有替代的过滤手段**。
   `modem.c` 里这一条失败会打 `LOG_ERR` 但不中止——所以要看日志。

2. **MAIN_DTR / MAIN_RI 的具体脚号。** 至今无来源
   （`docs.openluat.com/4Gmodulepin/` 返回 404）。**布板前必须查
   `Air780EP硬件手册V1.1.pdf`。** overlay 里现在填的 P0.26 是**占位值**。

顺带 R1 的那条也别忘：**板子到手第一件事是测静态电流**（§3.4 第 3 条）。
同款克隆板实测过 0.42 µA / 7~8 µA / 750 µA 三种结果。

## 5. BabbleSim 运行时测试：开锁通道

`firmware/tests/ble_unlock_bsim/` —— 把**真固件的开锁模块**放进 2.4 GHz 射频
仿真里跑一遍。两个仿真设备：`d=0` 跑 `ble_unlock.c` + `unlock.c`（peripheral），
`d=1` 假装手机（central）。被测代码是 `firmware/nrf52840/src` 下的原件，
一行都没为测试改过。

```bash
export ZEPHYR_BASE=/opt/ncs/zephyr
export BSIM_OUT_PATH=/opt/ncs/tools/bsim
export BSIM_COMPONENTS_PATH=$BSIM_OUT_PATH/components
/opt/zephyrtool/bin/west build -p always -b nrf52_bsim/native \
    -d /tmp/btest firmware/tests/ble_unlock_bsim
firmware/tests/ble_unlock_bsim/run.sh /tmp/btest
```

**当前结果：两侧都 Passed。** 8 条断言，逐条说明它防什么：

| # | 断言 | 防什么 |
| --- | --- | --- |
| 1 | MTU 协商后可写 ≥ 30 字节 | prj.conf 那两行 MTU 被人改小 → UNLOCK APDU 装不下（实测 MTU=40，可写 37） |
| 2 | 服务与两个特征可发现，属性正确 | CMD 必须有 WRITE、RSP 必须有 NOTIFY 且**不可读**（可读会泄露上一次应答） |
| 3 | **不订阅 CCCD 就收不到应答** | 钉住 `BT_GATT_ENFORCE_SUBSCRIPTION` 的行为，也是「每次连接多一个往返」那个代价的来源 |
| 4 | 坏 MAC → `69 82` | 三重校验的第一条 |
| 5 | 合法 UNLOCK → `90 00` + 开锁回调 | 主路径真的通（前 4 条全是拒绝路径，不验这条就可能「一律拒绝」也全绿） |
| 6 | **原样重放 → `69 82`** | §5.1 的底线「嗅探到一次完整交互也不能再开一次」的唯一可执行证明 |
| 7 | 新 nonce + 旧 counter → `69 82` | 证明拒绝来自 **counter 严格递增**，不只是 nonce 一次性 |
| 8 | counter 递增后 → `90 00` | 证明 6、7 不是「坏了所以全拒」 |

仿真验不了的：引脚接线、静态电流、CC310 硬件路径（bsim 上 PSA 落到 Oberon
软件驱动）、GNSS/4G 的 UART 时序。那些是 R0~R1 的事。

**写这个测试本身抓到一个 bug**（不在固件里，在测试的 central 侧，但手机 App
会犯同一个错）：GATT 表里 **RSP 排在 CMD 前面**（服务定义顺序是 RSP、CCC、CMD），
所以「先按 UUID 找 CMD、再从它后面找 RSP」永远找不到 —— 症状是
`cmd=21 rsp=0`。正确做法是一次性枚举服务里的所有特征。**这条要写进 App 文档。**

## 6. 还没定的硬件会影响哪些代码

| 待决项 | 影响的文件 | 现在按什么假设写的 |
| --- | --- | --- |
| 锁用电磁锁还是电机锁（§7 第 1 条） | `lock.c` | 一根线、高有效、500 ms 脉冲。电机锁要改成半桥两根线 |
| LIS2DW12 STATIONARY 走 INT2 而本板只接 INT1（§3d） | `motion.c`、overlay | **已定论**：MOTION 在 INT1（可用），STATIONARY 在 INT2（线没接，事件不会来）。当前不依赖它，静止判定用 `last_still_ms` 软件计时。要真用得接第二根线或越过驱动写 `CTRL_REG7.int2_on_int1` |
| GPIO 分配（§3.5 现在有 21 个可用） | overlay | 用了 17 个，余 4 个（BLE 不占引脚，P0.09/P0.10 释放）。锁要 3 根线也装得下 |
| ADC 脚 | overlay | P0.31(AIN7)。只有 3 个真 ADC 脚，ZMK 的 A6~A10 别名不是 SAADC 通道 |
| ~~电池采样分压比~~（§3.6 / §11 #27） | `battery.c`、overlay | ✅ **已定并已改**：**21:1**（`output-ohms=470000` / `full-ohms=4.7M+4.7M+470k`）。基准是引脚上限 **VDD 3.3 V**（不是 ADC 满量程 3.6 V —— 那是绝对最大值，按它算零余量）。`battery.c` 两条 `BUILD_ASSERT` 把比例和源阻抗（≤800 kΩ）钉成编译错误，都实测触发过。换算走 `voltage_divider_scale_dt()`。**电池不是 48 V 的话改 `PACK_MAX_MV`** |

## 7. 两个「决定不做」的，理由记在这里

**1. 不报芯片温度**（`tmp` 字段整个缺席，契约 §5.3 允许省）。

nRF52840 有内置温度传感器，Zephyr 也有现成驱动（DT 节点 `temp`，
compatible `nordic,nrf-temp`，默认就 `status = "okay"`，`CONFIG_TEMP_NRF5`
默认 y —— 一行配置都不用加）。但：

- 它测的是**芯片结温**，不是环境温度。旁边 Air780EP 一发射就自热几度，
  与"车放在什么温度环境里"的相关性很弱。
- 精度 **±5 °C**（外加 ±2.5 °C 的 25 °C 点偏移）。真实 25 °C 可能报 20 也可能报 30。
- 读一次要拉 **HFXO**（产品规格书要求晶振才能达到标称精度），
  36 µs 转换期间 1.05 mA。能量上可忽略（15 分钟一次 ≈ 0.44 nA 平均），
  **但一个 ±5 °C 的数字不值得为它占报文字节、建 HA 实体、然后解释为什么冬天显示 18 度。**

要是将来真要加，形态应该是：只在 HFXO 已经因别的原因在转的时候顺手读、
字段名叫 `mcu_temp` 而不是 `temp`（别让下游当环境温度用）、按 1 °C 上报不给小数。

**2. 日常不用 System OFF，只在欠压第 3 级用。**

理由在 `main.c` 末尾的长注释里：功耗地板是 4G 模组（0.5~1.5 mA），
System ON+RTC 的 3.16 µA 和 System OFF 的 0.40 µA 差 2.8 µA，是噪声。
而 System OFF 唤醒等于**复位**，整个初始化要重跑。
欠压第 3 级是例外 —— 那时的目标不是省 2.8 µA，是别把车电池抽空。

⚠ **实测 System OFF 必须拔掉调试器冷启动。** 产品规格书：Debug Interface
mode 下 System OFF 是**被仿真的**（停在一个 `WFE` 循环里），电流测出来是毫安级。
不知道这条会误判「System OFF 没生效」。
