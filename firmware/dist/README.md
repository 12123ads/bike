# 编译产物

`firmware/nrf52840` 的构建产物，**放进 git 是为了让手上没装 NCS 的人也能直接烧**。

| 文件 | 怎么用 |
| --- | --- |
| `zephyr.uf2` | 镊子双击短接 A 排的 `RESET` 与 `GND`（第 10、11 个焊盘）让板子出现成 U 盘，把文件拖进去 |
| `zephyr.hex` | J-Link 烧。⚠ 用 `loadbin`/`loadfile` 并**显式给地址范围**，`erase` 和 `testwspeed` 在这块板子上禁用（会擦掉 MBR + SoftDevice，[`../../docs/FIRMWARE.md`](../../docs/FIRMWARE.md) §2b 有踩坑记录） |
| `manifest.json` | 机器可读的构建记录，**不要手写** —— `build.sh` 生成、测试消费 |

## 改了固件源码怎么办

```bash
firmware/build.sh          # 构建 + 拷产物 + 刷新 manifest.json，一个动作
```

然后把 `firmware/dist/` 和源码**一起提交**。

**为什么必须是一个动作**：git 不知道 `zephyr.uf2` 和 `src/*.c` 有依赖关系。
源码改了忘记重编，仓库里就躺着一份「看起来是最新的」固件 —— 而这种不同步
只在**烧板子之后**暴露，症状是「代码明明改了但行为没变」，比编译错误难查得多。
所以：

- `manifest.json` 记下**每个固件源文件的 sha256**（列表来自
  `git ls-files firmware/nrf52840`，27 个文件）
- `server/tests/test_firmware_contract.py` 重算一遍并比对 —— 改了没重编、
  新增文件没进清单、产物被手改过，四条断言分别抓
- 也可以单独查：`python3 firmware/dist/manifest.py check`

`build.sh` 用 `-p always`（全量构建）不是保险：overlay 或 Kconfig 改动在增量
构建下不一定重新生成 devicetree，产物会是旧脚号的 —— 这个踩过。

## 本次构建

| 项 | 值 |
| --- | --- |
| 日期 | 2026-09-04 |
| board target | `promicro_nrf52840/nrf52840/uf2` |
| 工具链 | NCS v3.4.0（Zephyr 4.4.0）+ Zephyr SDK 1.0.1 |
| 编译选项 | `-DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y`，**零警告** |
| FLASH | 220 652 B / 792 KB = **27.21 %** |
| RAM | 61 616 B / 256 KB = **23.50 %** |
| `zephyr.uf2` | 441 344 B，family `0xADA52840`，start `0x26000`，862 块 |
| `zephyr.hex` | 620 792 B |

```
zephyr.uf2  sha256  22cdf7e7ade2e06b2cb087cbfdc627a0e9c8138ff0984493de8aada5c5536c88
zephyr.hex  sha256  bd38359a03e7a6a4af9e46e9895a2e5a914b9510bebc437dee12be5a0ea4167f
```

同一份源码在这台机器上重复构建两次，两个文件都**字节相同**（实测）。

**⚠ 这份固件没有在真硬件上跑过。** 板子上过一次 J-Link，但那次只做只读探测
（SWD 通路、芯片体检、UICR/flash 现状），没烧任何固件进去。引脚接线、静态电流、
GNSS/4G 的 UART 时序都还没验。
