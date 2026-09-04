#!/bin/sh
# 构建固件并刷新 firmware/dist/ —— **改了固件源码就跑这个**。
#
# 为什么要有这个脚本而不是「记住那三行 west 命令」：dist/ 里的二进制必须和
# 源码同步，而人只会记住前半句。这个脚本把「构建 → 拷产物 → 写清单」做成
# 一个原子动作，清单里记下每个源文件的 sha256，
# `server/tests/test_firmware_contract.py` 拿它当断言 ——
# 源码改了没重编，pytest 当场红，不用等到烧板子。
#
# 用法：从仓库根目录跑 `firmware/build.sh`。
set -eu

ZEPHYR_BASE="${ZEPHYR_BASE:-/opt/ncs/zephyr}"
ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-/opt/zephyr-sdk/zephyr-sdk-1.0.1}"
WEST="${WEST:-/opt/zephyrtool/bin/west}"
export ZEPHYR_BASE ZEPHYR_SDK_INSTALL_DIR

BOARD='promicro_nrf52840/nrf52840/uf2'
BUILD_DIR="${BUILD_DIR:-/tmp/ebike-fw-build}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIST="$ROOT/firmware/dist"
LOG="$BUILD_DIR.log"

cd "$ROOT"

# ⚠ `-p always` 是必须的，不是保险：overlay 或 Kconfig 改动在增量构建下
# 不一定重新生成 devicetree，产物会是旧脚号的（踩过）。
echo "== west build ($BOARD) =="
"$WEST" build -p always -b "$BOARD" -d "$BUILD_DIR" firmware/nrf52840 \
    -- -DCONFIG_COMPILER_WARNINGS_AS_ERRORS=y 2>&1 | tee "$LOG"

echo "== 拷产物 + 写清单 =="
mkdir -p "$DIST"
cp "$BUILD_DIR/nrf52840/zephyr/zephyr.uf2" "$BUILD_DIR/nrf52840/zephyr/zephyr.hex" "$DIST/"

BOARD="$BOARD" BUILD_LOG="$LOG" python3 "$ROOT/firmware/dist/manifest.py" write
echo "== 完成：$DIST =="
