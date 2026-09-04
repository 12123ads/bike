# 固件（nRF52840 / NCS-Zephyr）

> ✅ **2026-09-02：这份固件现在编得过了，而且开锁通道跑过了射频仿真。**
>
> | | 结果 |
> | --- | --- |
> | `west build` | **通过**，零警告（`-DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y` 也过） |
> | FLASH | 220 652 B / 792 KB = **27.21 %** |
> | RAM | 61 616 B / 256 KB = **23.50 %** |
> | 产物 | `zephyr.uf2` 441 344 B，family `0xADA52840`，start `0x26000` ✓ |
> | BabbleSim 运行时测试 | **两侧都 Passed**，8 条断言（见 §5） |
> | native_sim ztest | **8 套 67 条全过**（见 §3d） |
> | 文本层契约测试 | `server/tests/test_firmware_contract.py` **82 条全过** |
>
> 上表是 **2026-09-04 最后一次构建**的数字（引脚复位那次）。**编译产物已入库**：
> [`firmware/dist/`](../firmware/dist/) 里有 `zephyr.uf2` 和 `zephyr.hex` ——
> 手上没装 NCS 也能直接烧。改了固件源码跑 **`firmware/build.sh`**（构建 + 拷产物 +
> 刷新 `manifest.json` 一个动作），四条断言钉住 dist/ 与源码同步，见 §2c。
>
> 工具链：NCS **v3.4.0**（Zephyr 4.4.0）+ Zephyr SDK 1.0.1，
> board target `promicro_nrf52840/nrf52840/uf2`。装在 `/opt/ncs`。
>
> ⚠ 仍然**没有在真硬件上跑过**。仿真验的是协议与逻辑，验不了
> 引脚接线、静态电流、GNSS/4G 的 UART 时序 —— 那些是 R0~R1 的事（§4）。
>
> **2026-09-04 补充**：主控板已经上过一次 J-Link，**SWD 通路可用、芯片体检合格、
> bootloader 版本正好匹配**（结果见 [`DESIGN.md` §3.4](DESIGN.md)，
> 这台机器上的探针限制与调用范式见 §2b）。但固件本身**还是没在真硬件上跑过** ——
> 那次只做了只读探测，没有烧任何固件进去。
>
> **同日第二件事**：对着克隆板 netlist 逐脚复核 overlay，抓到**三个这块板
> 根本没引出的脚**（`batt_gate` P0.04、`modem_pwrkey` P0.12、`modem_ri` P0.26），
> 前两个有代码在用。已全部改到引出脚上，白名单钉成断言（§6 / DESIGN.md §3.5）。
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

**日常就用这一条**（本机 `/opt/ncs` 已经装好 NCS v3.4.0 + Zephyr SDK 1.0.1）：

```bash
firmware/build.sh
```

它做三件事：全量构建（带 `-DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y`）、把
`zephyr.uf2`/`zephyr.hex` 拷进 [`firmware/dist/`](../firmware/dist/)、
刷新 `dist/manifest.json`。**改了固件源码就跑它，然后把 `dist/` 和源码一起提交** ——
理由见 §2c。想换构建目录给 `BUILD_DIR=`，默认 `/tmp/ebike-fw-build`。

手动构建（调试时不想动 `dist/`）：

```bash
export ZEPHYR_BASE=/opt/ncs/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk/zephyr-sdk-1.0.1
/opt/zephyrtool/bin/west build -p always \
    -b promicro_nrf52840/nrf52840/uf2 \
    -d /tmp/bbuild firmware/nrf52840
```

产物在 `/tmp/bbuild/nrf52840/zephyr/`：`zephyr.uf2`（拖进 UF2 盘）、
`zephyr.hex`（J-Link 烧）、`zephyr.elf`（gdb 用）。

⚠ **`-p always` 不是保险**：overlay 或 Kconfig 改动在增量构建下不一定重新
生成 devicetree，产物会是旧脚号的。这个踩过（引脚复位那次）。

想连警告一起当错误（CI 应该这么跑，`build.sh` 已经默认这样）：

```bash
... west build ... -- -DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y
```

### 2a. 从零装工具链（换机器时）

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

**手上这块板子已经读过了（2026-09-04）：`NFCPINS = 0xFFFFFFFE`（已开放）、
`REGOUT0 = 0xFFFFFFFD`（已是 3.3 V）→ 两步都不用做，`--recover` 也不要做。**
完整体检结果见 [`DESIGN.md` §3.4](DESIGN.md)。

## 2b. 烧录与调试：这台机器上的实际情况（2026-09-04 实测）

**探针是山寨 J-Link OB**（`VID_1366&PID_0101`，固件 `J-Link ARM-OB STM32 compiled
Aug 22 2012`，HW V7.00，S/N 报 `-1`）。两个后果：

- **速度上限 4000 kHz**。它只支持 `16 MHz/n, n≥4`，`-speed 8000` 会被静默降回 4000。
- **不要让 J-Link 给它刷固件**。脚本第一行放 `exec DisableAutoUpdateFW`，
  否则每次连接都可能试图更新这块克隆探针的固件。

SWD 四线接法与实测的 UICR/flash 现状记在 [`DESIGN.md` §3.4](DESIGN.md)，不重复。
调用范式（J-Link Commander V6.44e，已验证可用）：

```
JLink.exe -device nRF52840_xxAA -if SWD -speed 4000 -autoconnect 1 \
          -ExitOnError 1 -CommanderScript probe.jlink
```

```
exec DisableAutoUpdateFW
si SWD
speed 4000
connect
h
mem32 0x10001208,1          ; 只读探测：mem32 / savebin
savebin dump.bin, 0x26000, 0x20000
g
q
```

⚠ **`testwspeed` / `testcspeed` 在这块板子上禁用**，`erase` 同理。
`testwspeed` 默认往 `0x00000000` 写 128 KB 递增图案，**会擦掉 MBR + SoftDevice**，
而这块板子的 bootloader 依赖它们。已经踩过一次并完整恢复（过程与校验见
[`DESIGN.md` §3.4](DESIGN.md)）。写 flash 只用 `loadbin` + `verifybin` 并显式给地址范围。

**日志只能走 RTT，而 RTT 通路还没验过。** `prj.conf` 关掉了板级 USB CDC ACM
（`CONFIG_BOARD_SERIAL_BACKEND_CDC_ACM=n`），两个 UARTE 被 GNSS 和 Air780EP 占满，
所以 `CONFIG_LOG_BACKEND_RTT=y` 是唯一的日志出口。当前 flash 里是 Adafruit Arduino
残留固件，不产生 RTT 输出，所以没法用它验证读通路。另外 **V6.44e 的 Commander
没有 `rtt` / `rttread` 命令**（`?` 输出确认过），要用独立的
`JLinkRTTLogger.exe -Device nRF52840_xxAA -If SWD -Speed 4000` 或 `JLinkRTTClient.exe`。
**R0 点灯那一步同时是 RTT 通路的验收项** —— blinky 起来但 RTT 无输出，
意味着后面所有阶段都在没有日志的情况下调试，必须当场解决。

### 2c. 编译产物入库，以及怎么保证它不过期

`firmware/dist/` 里有 `zephyr.uf2`、`zephyr.hex`、`manifest.json`，
**手上没装 NCS 也能直接烧**（这块板的升级方式就是拖 UF2 文件）。

代价是一个真实风险：**git 不知道 `zephyr.uf2` 和 `src/*.c` 有依赖关系。**
源码改了忘记重编，仓库里就躺着一份「看起来是最新的」固件 —— 而这种不同步
只在**烧板子之后**暴露，症状是「代码明明改了但行为没变」，比编译错误难查得多。

所以 `manifest.json` 记下**每个固件源文件的 sha256**（列表来自
`git ls-files firmware/nrf52840`，27 个文件）、产物的 sha256 与大小、
UF2 头部的 family/起始地址/块数、FLASH/RAM 用量与警告数。
四条断言在 `server/tests/test_firmware_contract.py` 里消费它：

| 断言 | 抓什么 |
| --- | --- |
| `test_dist_is_not_stale` | **改了源码没重编**（也抓新增文件没进清单、清单里的文件已删除） |
| `test_dist_artifacts_match_manifest` | 产物被手改过或拷错了 |
| `test_dist_build_was_clean_and_bootable` | UF2 family 不是 `0xADA52840`（拖进 U 盘会被**静默忽略**）、起始地址不是 `0x26000`（会覆盖 SoftDevice）、有警告 |
| `test_dist_fits_the_app_partition` | FLASH/RAM 逼近上限 —— 钉的是趋势，要在加功能之前发现，不是等某次构建突然链接失败 |

也可以单独查：`python3 firmware/dist/manifest.py check`。

**不要手写 `manifest.json`** —— `build.sh` 生成它。

## 3. 已知没做完的

按会不会咬人排序：

| # | 缺什么 | 后果 | 在哪 |
| --- | --- | --- | --- |
| ~~1~~ | ~~**断线重连阶梯**~~ | ✅ **2026-09-03 已修**：`modem_reconnect()` 三级 —— **1 级** 只重建 MQTT 会话（`MDISCONNECT` → `stage_session`，~5~10 s）；**2 级** 加 `CIPSHUT` + 重新附着（~15~70 s）；**3 级** `CPOWD` + PWRKEY 硬关机 + 完整重跑（~30~90 s）。`modem_connect()` 拆成 `stage_boot`/`stage_attach`/`stage_session` 三段供阶梯复用。**不做无限重试** —— 三级全败就返回错误让上层放弃本轮（§4.4：死循环会抽干车电池）。`connected` 从裸 `bool` 改成 `atomic_t`。`uplink.c` 的 `publish_retry()` 按 `modem_is_connected()` 而非 `rc` 决定要不要重连（-E2BIG/-ENOMEM 重连一百次也没用）。5 条 ztest（假模组 + AT 命令 trace）钉住「只重跑坏掉的那一层」，3 个变异体全被抓 | `firmware/tests/modem_reconnect` |
| 2 | **睡眠仲裁器 + RI 监听** | 省电档下第一条 AT 命令可能丢。**依赖 MAIN_DTR/MAIN_RI 脚号，而那个至今无来源**（§8.7 硬门禁 / §11 #18）。2026-09-04 **删掉了 overlay 里的 `modem_ri` 节点**：它没有任何 C 代码取用，而且原来填的 P0.26 是这块板没引出的脚（DESIGN.md §3.5）—— `AT+CFGRI=1` 发了但主控侧没人监听那根线，「模组主动唤醒主控」目前是半条链。定脚号时从 §3.5 的 8 个余量里取 | 同上第 2 条 / overlay |
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

## 3e. 运行时测试清单（native_sim + ztest）

`firmware/tests/` 下的每个目录都是一个独立的 ztest 应用，**编的是
`firmware/nrf52840/src` 里的原件**，一行没为测试改过。硬件依赖用
`uart-emul` / `gpio-emul` / 假驱动顶掉（各自的 `boards/native_sim.overlay`）。

```bash
export ZEPHYR_BASE=/opt/ncs/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk/zephyr-sdk-1.0.1
for t in modem_time modem_downlink motion_still modem_reconnect \
         gnss_nmea uplink_events unlock_slots battery_floor; do
    /opt/zephyrtool/bin/west build -p always -b native_sim -d /tmp/t_$t \
        firmware/tests/$t && /tmp/t_$t/$t/zephyr/zephyr.exe
done
```

| 目录 | 测什么 | 结果 |
| --- | --- | --- |
| `modem_time` | NITZ 串 → Unix 秒（±zz 是 1/4 小时、hh:mm:ss 是本地时间），以及「宁可上报 0 也不上报像时间的错数」的拒绝路径 | 11 passed |
| `modem_downlink` | **下行 URC 解析 + AT 应答匹配 + 下行不在命令流里就地投递**（审计 M1/M2/R1） | 12 passed |
| `motion_still` | 运动/静止状态机与软件静止计时 | 4 passed |
| `modem_reconnect` | 三级重连阶梯「只重跑坏掉的那一层」 | 5 passed |
| `gnss_nmea` | NMEA 校验和**真的挡住坏行**、ddmm.mmmm 换算、南纬西经取负、quality=0 不当定位（审计 R4） | 11 passed |
| `uplink_events` | 事件队列**发送期间不持 `ev_lock`**、发失败留到下一轮、`dn/secret` 接线（审计 M3） | 7 passed |
| `unlock_slots` | 密钥槽位复用**不继承旧 counter**、同 uid 轮换**保留** counter（审计 M7） | 8 passed |
| `battery_floor` | **采样链坏了不能被当成「电池耗尽」**：`raw=2` 换算出 21 mV 而旧判据只有 `mv <= 0` 一个 LSB 宽 → 假欠压 → 主动 System OFF。顺带钉住三个等级边界、真实深度欠压仍判 3、两条路径都关门控 | 9 passed |

`modem_downlink` 为什么值得单独一套：它盯的三条都**编得过、跑得起来、
只是静默做错事** —— 和 §3c 里最难抓的那两个同型。

- **M1 行缓冲**：`LINE_MAX=512` 只能收 236 B 下行，而 `dn_payload` 按 3900 B
  开——差 16 倍。超限的行被截断后必然丢掉 payload 的结束引号，
  `handle_msub` 走「格式不认识」分支**且不打日志**：下行凭空消失，
  服务端每次上线重发同一条。现在 `read_line` 返回 `-EMSGSIZE` 并 LOG_ERR，
  `LINE_MAX=640` / `PROTO_MAX_DN_PAYLOAD=256` / `contract.MAX_DOWNLINK_BYTES`
  三个数字互相自洽，由 `test_firmware_contract.py` 钉住。
- **M2 应答匹配**：`at_cmd()` 用 `strstr(line,"OK")` 判成功，而 `SEND OK` /
  `CONNECT OK` / `CONNACK OK` 都是带外 URC。一条迟到的 `SEND OK` 能替下一条
  命令答到——`AT+CPOWD=1` 没执行却返回成功，**模组不关机、持续耗电**。
  现在 expect 恰好是 `"OK"` 时要求整行相等；调用方显式等 `SEND OK` 的
  子串匹配路径不受影响（两条测试分别钉住这两侧）。
- **R1 重入**（第二轮审计，最严重的一条）：`handle_msub` 就地调 `dn_cb`，
  而 `consume_urc` 的两个调用点都**持 `at_lock`**（`at_cmd_expect` 等应答的
  循环、`modem_publish` 等裸 `>` 的循环）。于是「一条 AT 命令在飞」时收到
  下行 → `ack_downlink` → **重入 `modem_publish` 自身**。两个后果实测：
  static `hexbuf` 被内层覆盖（外层声明 41 字节却只写出 36 个 HEX 字符，
  模组从下一条命令的字节里补齐）；`k_mutex` 同线程可重入所以不死锁、
  只是无界递归 —— 按编译产物量的真实帧大小是每层 880 B，第 5 层越过
  `UPLINK_STACK_SIZE`(4096)。而 `RESET_ON_FATAL_ERROR` 没开、没有看门狗，
  爆栈 = `arch_system_halt()` 死转 = **车变砖直到人工断电**。
  触发条件是常态：设备上线时服务端一次性冲刷全部未确认下行，积 4 条就到。
  现在 `handle_msub` 只入队（4 槽位环形队列），`dn_cb` 只在 `modem_poll()`
  的 `deliver_downlinks()` 里调 —— 那里不持锁、也不在任何命令流里。

**变异体验证**（每一条都真的跑过）：

| 改回旧写法 | 结果 |
| --- | --- |
| `handle_msub` 就地投递（R1） | `modem_downlink` 3 条红（9 passed / 3 failed） |
| `at_cmd` 用 `strstr` 判 OK（M2） | 2 条红（7 passed / 2 failed） |
| `checksum_ok` 用 `strtol`（R4） | `gnss_nmea` 1 条红，报「1.033333,2.050000（9 星）被当成有效定位」 |
| `flush_events` 持锁发送（M3） | `uplink_events` 1 条红（那条用例耗时从 0.5 s 变 10 s —— 探针真的被卡住了） |
| `del`/`wipe` 不清 counter（M7） | `unlock_slots` 5 条红 |
| 删掉 `battery_read_mv` 的下限判断 | `battery_floor` 3 条红（6 passed / 3 failed），报「raw=2 换算出 21 mV，battery_read_mv 却当成有效读数返回了」 |
| `batt_gate`/`vbatt` 回 P0.04、`modem_pwrkey` 回 P0.12（引脚复核） | `test_overlay_only_uses_pins_the_board_exposes` 红，报「overlay 用了这块板没引出的脚：['P0.04', 'P0.12']」 |
| 删掉 overlay 末尾的 `&i2c1 { status = "disabled"; }` | `test_overlay_disables_board_peripherals_that_steal_header_pins` 红，报「上游板级 DTS 把它设成 okay，它的 pinctrl 会占掉排针脚」 |
| `batt_gate` 挪到 P0.06（引出但已是 uart0 TX） | 2 条红：脚位互斥那条 + 门控与分压器同脚那条 |
| 删掉 `i2c0_default` 的 `bias-pull-up` | `test_i2c_has_internal_pullups_in_both_states` 红，报「i2c0_default 缺 bias-pull-up」 |
| 只删 `i2c0_sleep` 的 `bias-pull-up` | 同一条红，报的是 `i2c0_sleep` —— 两个 state 分别钉 |
| `clock-frequency` 改 `I2C_BITRATE_FAST` 但仍用内部上拉 | `test_i2c_stays_at_100khz_while_using_internal_pullups` 红 |

M1 那两条在旧代码下仍然过——因为旧代码的失败方式是「静默丢弃」而不是
「投递截断值」，而这两条断言的正是「不投递」。这个差异本身是实测得到的
（见 `modem.c` 里 `LINE_MAX` 的注释），审计报告初稿把它写成了「接受错误值」，
已更正。

## 4. R0 阶段必须先验的两件事

这两条来自 §8.7，都是**硬门禁**，不验就往下做会白干：

1. **`AT^WAKEUPHEX` 和 `CSCLK=3` 在 AT 固件 V1011 上到底能不能用。**
   只需要一根 UART 线加一块模组。不能用的话，每条例行 URC 都会把主控从睡眠
   拽出来，**功耗预算直接崩，而且没有替代的过滤手段**。
   `modem.c` 里这一条失败会打 `LOG_ERR` 但不中止——所以要看日志。

2. **MAIN_DTR / MAIN_RI 的具体脚号。** 至今无来源
   （`docs.openluat.com/4Gmodulepin/` 返回 404）。**布板前必须查
   `Air780EP硬件手册V1.1.pdf`。** overlay 里的 `modem_ri` 节点已删除
   （原来填的 P0.26 这块板没引出，DESIGN.md §3.5），定了脚号之后
   从余量里取一个、连监听代码一起加。

第 1 条的验收要读日志，而**日志只能走 RTT，那条通路本身也还没验过**（§2b）——
所以次序是：先烧个最小固件确认 RTT 出字，再去验 `AT^WAKEUPHEX`。

顺带 R1 的那条也别忘：**板子到手第一件事是测静态电流**（DESIGN.md §3.4 第 3 条）。
同款克隆板实测过 0.42 µA / 7~8 µA / 750 µA 三种结果。**2026-09-04 那次上机没量** ——
而且量的时候必须拔掉调试器冷启动（§7 末尾那条：Debug Interface mode 下 System OFF 是仿真的）。

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
| ~~GPIO 分配~~（§3.5） | overlay | ✅ **2026-09-04 已核实并已改**：用了 **13 个**，余 **8 个**（P0.02 P0.09 P0.29 P1.01 P1.02 P1.04 P1.06 P1.07）。同时修掉三个**这块板没引出**的脚：`batt_gate` P0.04 → **P0.10**、`modem_pwrkey` P0.12 → **P1.00**、`modem_ri` P0.26 → 节点删除。SoC 有 48 个 GPIO 而板子只引出 21 个，填错 `west build` 照过 —— 白名单和四条护栏现在钉在 `test_firmware_contract.py`。顺带关掉上游板级 DTS 打开的 `i2c1`（抢 J2.12/J2.13）和 `spi2`（抢整个 J4） |
| ADC 脚 | overlay | P0.31(AIN7)。只有 3 个真 ADC 脚，ZMK 的 A6~A10 别名不是 SAADC 通道 |
| ~~I2C 上拉~~（§3.7） | overlay | ✅ **2026-09-04 已定并已改**：**不装外部电阻**，`i2c0_default`/`i2c0_sleep` 都加 `bias-pull-up` 用内部 RPU（11~16 kΩ）。改之前**两种上拉都没有** —— 而那不是设计选择，是 I2C 完全不通：nrfx 的 `TWIM_PIN_INIT` 会配 PULLUP，但 Zephyr twim 驱动传 `skip_gpio_cfg=true`，引脚配置全归 pinctrl。实测 pincfg `0x0C000018`(pull=NONE) → `0x0C000618`(pull=UP)。代价：锁在 100 kHz、总线电容 ≤ 74 pF（只挂这一个器件、线要短）。两条约束各有一条断言钉住 |
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
