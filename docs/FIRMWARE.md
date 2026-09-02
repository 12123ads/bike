# 固件（nRF52840 / NCS-Zephyr）

> ⚠ **这份固件在本机没有被编译过一次。** 这台机器上没有 NCS/Zephyr SDK，
> 也没有任何 C 编译器（`gcc: command not found`）。所以：
>
> - 符号名、Kconfig 名、DTS 属性名都是按 [`DESIGN.md`](DESIGN.md) 里**已核实**的写的；
>   2026-09-02 又对着上游源码逐个核了一遍 PSA/hwinfo/poweroff/GPIO 这几组
>   （结论写在各文件的注释里，包括三处**更正**，见 §3b）；
> - 但**第一次 `west build` 一定会有编译错误**（拼写、头文件、API 签名细节）；
> - 逻辑层面的交叉检查由 `server/tests/test_firmware_contract.py` 覆盖——
>   它把 topic 宏、字段名、事件/指令闭集、Kconfig 默认值和服务端的 `contract.py`
>   逐项对照，**45 条测试全过**。**那测的是「两边一致」，不是「能编译」。**

## 1. 文件

| 文件 | 干什么 | 依赖的 DESIGN.md 条目 |
| --- | --- | --- |
| `src/main.c` | 三态状态机 + 初始化顺序 + System OFF 路径 | §2.5 运动唤醒替代场唤醒 / §6 第 4 级 |
| `src/proto.c/.h` | 契约 v1 编解码 | [`MQTT-CONTRACT.md`](MQTT-CONTRACT.md) 全文 |
| `src/unlock.c/.h` | 挑战应答三重校验 | §5.2 协议 / §5.3 密钥管理 |
| `src/crypto.c/.h` | PSA HMAC + TRNG | §4.2（**注意**：CryptoCell 的开关由 cc3xx 库自己引用计数管，应用层不做，见 `crypto.h` 的长注释） |
| `src/nfc_tag.c/.h` | raw ISO-DEP 标签 | §2.1 NFCT 只能当标签 / §2.2 |
| `src/nvstore.c/.h` | 密钥、counter、序号 q 的持久化 | §5.2 counter 掉电不回零 |
| `src/motion.c/.h` | LIS2DW12 运动/静止触发 | §3.7 阈值只能运行时设 |
| `src/battery.c/.h` | 门控分压采样 + 四级欠压判定 | §3.6 不门控就是 29 µA 常流 / §6 |
| `src/lock.c/.h` | 锁驱动 + 位置反馈 | §7 第 1 条（型号未定） |
| `src/gnss.c/.h` | NMEA 解析 + 电源门控 | §1 选 ATGM336H 的理由 |
| `src/modem.c/.h` | Air780EP AT 状态机 | §8 全节 |
| `src/uplink.c/.h` | 一轮上报的编排 + 复位原因 + 欠压档位 | §4.1 契约的下行排队 / §5.1 的 rst |

## 2. 怎么编译

本机不行，需要一台装了 NCS 的机器：

```bash
# NCS v2.x，board target 见 DESIGN.md §3.4（已核实）
west build -b promicro_nrf52840/nrf52840/uf2 firmware/nrf52840

# 配置：设备 id、MQTT 主机、口令都要填
west build -t menuconfig      # 或直接改 prj.conf / 加 -DCONFIG_...
```

必填的四项（`Kconfig` 里都有说明）：

| 配置项 | 填什么 |
| --- | --- |
| `EBIKE_DEVICE_ID` | 默认 `bike01`，要和服务端配置里的设备 id 一致 |
| `EBIKE_MQTT_HOST` | **必须等于服务端证书的 CN**——`ebike-server init --hostname` 填的那个 |
| `EBIKE_MQTT_PORT` | 默认 8883 |
| `EBIKE_MQTT_PASSWORD` | `ebike-server init` 打印的那一串，只显示一次 |

⚠ **烧录前先读 UICR**（§3.3 / §3.4）：用 NFC 引脚要写 `UICR.NFCPINS`，那是**一次性**的。

```bash
nrfjprog -f NRF52 --memrd 0x1000120C     # 先看现状
# 要改回 GPIO 只能 --recover，而它会连带擦掉 REGOUT0（3.3V 设置）
nrfjprog -f NRF52 --recover
nrfjprog -f NRF52 --program nice_nano_bootloader-0.6.0_s140_6.1.1.hex --chiperase
```

不重刷 bootloader 板子起不来——`--recover` 之后 REGOUT0 回落到 1.8 V。

## 3. 已知没做完的

按会不会咬人排序：

| # | 缺什么 | 后果 | 在哪 |
| --- | --- | --- | --- |
| 1 | **断线重连阶梯** | 现在只有「重跑整个开机流程」一档。真正需要的是 MQTT 重连 → CIPSHUT 重拨 → 模组重启三级 | `modem.c` 末尾第 1 条 |
| 2 | **睡眠仲裁器 + RI 监听** | 省电档下第一条 AT 命令可能丢。**依赖 MAIN_DTR/MAIN_RI 脚号，而那个至今无来源**（§8.7 硬门禁 / §11 #18）。而且 overlay 里的 `modem_ri` 节点**没有任何 C 代码取用** —— `AT+CFGRI=1` 发了但主控侧没人监听那根线，「模组主动唤醒主控」目前是半条链 | 同上第 2 条 / overlay |
| 3 | **NITZ → Unix 秒换算** | 所有上行的 `t` 都是 0。功能不坏（服务端用 `t_srv` 落库，契约 §5.6），但设备日志里没有绝对时间 | 同上第 3 条 |
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
| **死代码** | `motion_prepare_for_sleep()`（空壳，注释还声称"驱动已经配好了不用额外动作"，那是错的）、overlay 里 7 个无人使用的 alias、`NVSTORE_USERS_VERSION`（定义了但不参与校验，现在真的写进落盘块并校验）、`firmware/luatos/` 空目录 |

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

## 5. 还没定的硬件会影响哪些代码

| 待决项 | 影响的文件 | 现在按什么假设写的 |
| --- | --- | --- |
| 锁用电磁锁还是电机锁（§7 第 1 条） | `lock.c` | 一根线、高有效、500 ms 脉冲。电机锁要改成半桥两根线 |
| LIS2DW12 INT1/INT2 路由矛盾（§3.7 / §11 #2） | `motion.c`、overlay | 都挂 INT1。如果 `SENSOR_TRIG_STATIONARY` 返回 `-ENOTSUP`，就是这个矛盾的实证——`motion.c` 里那条 `LOG_WRN` 会指出来 |
| GPIO 分配（§3.5 只有 19 个可用） | overlay | 用了 17 个。锁要 3 根线的话是 19/19 零余量，INT2 就没地方接了 |
| ADC 脚 | overlay | P0.31(AIN7)。只有 3 个真 ADC 脚，ZMK 的 A6~A10 别名不是 SAADC 通道 |
| **电池采样分压比**（§3.6 / §11 #27） | `battery.c`、overlay | **现在是错的：2:1 分压配 58.8 V 会灌 29.4 V 到 P0.31，烧芯片。** overlay 写的 `output-ohms=1M / full-ohms=2M`，而 ADC 满量程只有 3.6 V（内部参考 0.6 V + gain 1/6）→ 需要 ≥16.33:1。**焊上现在这个就烧，布板前必须改。** 代码侧不用动：`DIVIDER_RATIO` 是从 DTS 属性算出来的 |

## 6. 两个「决定不做」的，理由记在这里

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
