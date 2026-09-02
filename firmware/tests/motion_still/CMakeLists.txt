# 软件静止判定的测试（native_sim + ztest + i2c_emul 假 LIS2DW12）。
#
# 为什么值得单测：上一版把 SENSOR_TRIG_STATIONARY 挂在 INT2 上而本板只接 INT1,
# sensor_trigger_set() 返回 0、日志还打了 INFO,但事件永不到达 ——
# 后果是车动过一次之后 BLE 广播永远关不掉（DESIGN.md §2.4 的防跟踪保证）。
# 这类「配置成功、功能静默失效」只有走完整条链并观察状态真的翻回来才抓得到。
#
# 被测的是 firmware/nrf52840/src/motion.c 的**原件**,跑真的 LIS2DW12 驱动
# （挂在 i2c_emul 上的假芯片）+ 真的 GPIO 中断,不是直接调内部函数。

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(motion_still C)

set(FW ${CMAKE_CURRENT_LIST_DIR}/../../nrf52840)

target_sources(app PRIVATE
    src/main.c
    ${FW}/src/motion.c
)

target_include_directories(app PRIVATE ${FW}/src)
