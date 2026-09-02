# 固件（nRF52840 / NCS-Zephyr）

> ⚠ **这份固件在本机没有被编译过一次。** 这台机器上没有 NCS/Zephyr SDK，
> 也没有任何 C 编译器（`gcc: command not found`）。所以：
>
> - 符号名、Kconfig 名、DTS 属性名都是按 [`DESIGN.md`](DESIGN.md) 里**已核实**的写的；
> - 但**第一次 `west build` 一定会有编译错误**（拼写、头文件、API 签名细节）；
> - 逻辑层面的交叉检查由 `server/tests/test_firmware_contract.py` 覆盖——
>   它把 topic 宏、字段名、事件/指令闭集、Kconfig 默认值和服务端的 `contract.py`
>   逐项对照，39 条测试全过。**那测的是「两边一致」，不是「能编译」。**

## 1. 文件

| 文件 | 干什么 | 依赖的 DESIGN.md 条目 |
| --- | --- | --- |
| `src/main.c` | 三态状态机 + 初始化顺序 | §2.5 运动唤醒替代场唤醒 |
| `src/proto.c/.h` | 契约 v1 编解码 | [`MQTT-CONTRACT.md`](MQTT-CONTRACT.md) 全文 |
| `src/unlock.c/.h` | 挑战应答三重校验 | §5.2 协议 / §5.3 密钥管理 |
| `src/crypto.c/.h` | PSA HMAC + TRNG | §4.2 CryptoCell 用完必须关 |
| `src/nfc_tag.c/.h` | raw ISO-DEP 标签 | §2.1 NFCT 只能当标签 / §2.2 |
| `src/nvstore.c/.h` | 密钥、counter、序号 q 的持久化 | §5.2 counter 掉电不回零 |
| `src/motion.c/.h` | LIS2DW12 运动/静止触发 | §3.7 阈值只能运行时设 |
| `src/battery.c/.h` | 门控分压采样 | §3.6 不门控就是 29 µA 常流 |
| `src/lock.c/.h` | 锁驱动 + 位置反馈 | §7 第 1 条（型号未定） |
| `src/gnss.c/.h` | NMEA 解析 + 电源门控 | §1 选 ATGM336H 的理由 |
| `src/modem.c/.h` | Air780EP AT 状态机 | §8 全节 |
| `src/uplink.c/.h` | 一轮上报的编排 | §4.1 契约的下行排队 |

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
| 2 | **睡眠仲裁器** | 省电档下第一条 AT 命令可能丢。**依赖 MAIN_DTR 脚号，而那个至今无来源**（§8.7 硬门禁 / §11 #18） | 同上第 2 条 |
| 3 | **NITZ → Unix 秒换算** | 所有上行的 `t` 都是 0。功能不坏（服务端用 `t_srv` 落库，契约 §5.6），但设备日志里没有绝对时间 | 同上第 3 条 |
| 4 | **证书灌入**（`AT+FSCREATE`/`AT+FSWRITE`） | 代码假设 CA 已在模组 FS 里。谁在产线做这一步、私钥怎么存，四份历史文档都没写（§8.8 / §11 #24） | 同上第 4 条 |
| 5 | **PSM+ / PRO 双档** | 只有「连上」和「关机」两态。`dn/cmd` 的 `tier` 指令会明确 ack 失败而不是假装成功 | 同上第 5 条 / §11 #20 |
| 6 | **QoS1 超时重发队列** | 靠 `SEND OK` 一次确认，没有重发。这就是 §11 #22「设备侧要不要做补发队列」落地的位置 | 同上第 6 条 |
| 7 | **System OFF** | 当前用 System ON + RTC（3.16 µA）而不是 System OFF（0.40 µA）。**故意的**——功耗地板是 4G 模组（0.5~1.5 mA，§4.1b），这 2.8 µA 是噪声。R8 阶段再抠 | `main.c` 末尾的长注释 |
| 8 | **nRF 自己的 OTA** | 契约里没有 OTA topic。9600 baud 灌固件不现实，当前只支持 UF2 手动升级 | §8.8 / §11 #23 |

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
