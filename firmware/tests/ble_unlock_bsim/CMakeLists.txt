# BabbleSim 运行时测试：BLE 开锁通道
#
# 被测代码是 firmware/nrf52840/src 下的**原件**，一行都没为测试改过。
# 只补两样东西：bsim 的测试框架（bs_tracing/bstests），以及一个 central 角色。

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ble_unlock_bsim C)

set(FW ${CMAKE_CURRENT_LIST_DIR}/../../nrf52840)

target_sources(app PRIVATE
    src/main.c
    # 被测的真固件模块。**没有 main.c / uplink.c / modem.c / gnss.c** ——
    # 那几个要 UART 和 4G 模组，与开锁通道无关。
    ${FW}/src/ble_unlock.c
    ${FW}/src/unlock.c
    ${FW}/src/crypto.c
    ${FW}/src/nvstore.c
)

target_include_directories(app PRIVATE ${FW}/src)

# bsim 的测试框架头（bstests.h / bs_tracing.h）在 babblesim 的 libUtil 里，
# Zephyr 的 bsim 测试都靠这两个 include 路径。
target_include_directories(app PRIVATE
    $ENV{BSIM_COMPONENTS_PATH}/libUtilv1/src/
    $ENV{BSIM_COMPONENTS_PATH}/libPhyComv1/src/
    ${ZEPHYR_BASE}/tests/bsim/
)
