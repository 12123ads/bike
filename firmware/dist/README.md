# 编译产物

这两个文件是 `firmware/nrf52840` 的构建产物，**放进 git 是为了让手上没装
NCS 的人也能直接烧**。改了固件源码就要重新生成并连同源码一起提交 ——
不同步的二进制比没有二进制更糟。

| 文件 | 怎么用 |
| --- | --- |
| `zephyr.uf2` | 镊子双击短接 J3 的 `RESET` 与 `GND`（第 10、11 个焊盘）让板子出现成 U 盘，把文件拖进去 |
| `zephyr.hex` | J-Link 烧。⚠ 用 `loadbin`/`loadfile` 并**显式给地址范围**，`erase` 和 `testwspeed` 在这块板子上禁用（会擦掉 MBR + SoftDevice，[`../../docs/FIRMWARE.md`](../../docs/FIRMWARE.md) §2b 有踩坑记录） |

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

**⚠ 这份固件没有在真硬件上跑过。** 板子上过一次 J-Link，但那次只做只读探测
（SWD 通路、芯片体检、UICR/flash 现状），没烧任何固件进去。引脚接线、静态电流、
GNSS/4G 的 UART 时序都还没验。

## 怎么重新生成

```bash
export ZEPHYR_BASE=/opt/ncs/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk/zephyr-sdk-1.0.1
/opt/zephyrtool/bin/west build -p always \
    -b promicro_nrf52840/nrf52840/uf2 \
    -d /tmp/bbuild firmware/nrf52840 \
    -- -DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y
cp /tmp/bbuild/nrf52840/zephyr/zephyr.{uf2,hex} firmware/dist/
```
