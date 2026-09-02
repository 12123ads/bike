#!/usr/bin/env bash
# 跑 BLE 开锁通道的 BabbleSim 运行时测试。
#
# 用法（需要 NCS 工作区 + 已编好的 BabbleSim）：
#   export ZEPHYR_BASE=/opt/ncs/zephyr
#   export BSIM_OUT_PATH=/opt/ncs/tools/bsim
#   export BSIM_COMPONENTS_PATH=$BSIM_OUT_PATH/components
#   west build -p always -b nrf52_bsim/native -d /tmp/btest \
#        firmware/tests/ble_unlock_bsim
#   firmware/tests/ble_unlock_bsim/run.sh /tmp/btest
#
# 两个仿真设备连到同一个 2.4 GHz 物理层模型：
#   d=0 peripheral —— 跑真固件的 ble_unlock.c + unlock.c
#   d=1 central    —— 假装手机，跑完三步协议与三条拒绝路径
# 退出码 0 = 两侧都 Passed（bstests 把 bst_result 当进程退出码）。

set -eu

BUILD_DIR="${1:-/tmp/btest}"
EXE="${BUILD_DIR}/ble_unlock_bsim/zephyr/zephyr.exe"
SIM_ID="ebike_unlock_$$"
VERBOSITY="${VERBOSITY:-2}"

if [ ! -x "$EXE" ]; then
	echo "找不到可执行文件 $EXE —— 先 west build" >&2
	exit 1
fi
if [ ! -x "${BSIM_OUT_PATH}/bin/bs_2G4_phy_v1" ]; then
	echo "找不到 BabbleSim 物理层 —— 先在 \$BSIM_OUT_PATH 里 make everything" >&2
	exit 1
fi

cd "${BSIM_OUT_PATH}/bin"

"$EXE" -v=${VERBOSITY} -s=${SIM_ID} -d=0 -RealEncryption=1 -testid=peripheral \
	> /tmp/${SIM_ID}_peripheral.log 2>&1 &
PID_P=$!

"$EXE" -v=${VERBOSITY} -s=${SIM_ID} -d=1 -RealEncryption=1 -testid=central \
	> /tmp/${SIM_ID}_central.log 2>&1 &
PID_C=$!

./bs_2G4_phy_v1 -v=${VERBOSITY} -s=${SIM_ID} -D=2 -sim_length=60e6 \
	> /tmp/${SIM_ID}_phy.log 2>&1 &
PID_PHY=$!

RC=0
wait $PID_P || RC=$?
RC_P=$RC
RC=0
wait $PID_C || RC=$?
RC_C=$RC
wait $PID_PHY || true

echo "--- peripheral (rc=$RC_P) ---"
grep -E "开锁|APDU|BLE 就绪|已连接|已断开|PASSED|FAILED|Passed|Failed" \
	/tmp/${SIM_ID}_peripheral.log || true
echo "--- central (rc=$RC_C) ---"
grep -E "✓|MTU|特征|订阅|开锁|重放|counter|PASSED|FAILED|Passed|Failed" \
	/tmp/${SIM_ID}_central.log || true

if [ "$RC_P" -ne 0 ] || [ "$RC_C" -ne 0 ]; then
	echo "测试失败（peripheral=$RC_P central=$RC_C）。完整日志：" >&2
	echo "  /tmp/${SIM_ID}_peripheral.log" >&2
	echo "  /tmp/${SIM_ID}_central.log" >&2
	exit 1
fi
echo "两侧都通过"
