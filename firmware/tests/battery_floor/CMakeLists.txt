# 电池采样合理性下限的测试（native_sim + ztest + adc-emul 假 ADC）。
#
# 为什么值得单测：整数换算链是 `(raw * 600 * 6 >> 12) * 21`，**一个 ADC LSB
# 等于 370 mV 电池电压**。于是 `raw = 2`（满量程的 0.05%）就换算成 21 mV，
# 而 `battery_low_level()` 原来的读失败判据只有 `mv <= 0` —— 21 mV 直接命中
# 第 3 级，`main.c` 的上报线程随即 `enter_system_off()`。
#
# 症状是**假欠压**：车电池好好的，车却自己进 System OFF，摇一下醒来报一轮
# 又睡回去，上报的 lowbatt 里写着 `v=0.0`。日志和报文都指向「电池空了」，
# 真实故障却在采样链上（线没接 / 门控 MOS 常闭 / 分压电阻虚焊）。
# 这类「读数是假的但下游当真值用」只有把真实的坏读数喂进完整链路才看得出来。
#
# 被测的是 firmware/nrf52840/src/battery.c 的**原件**，一行没为测试改过。
# SAADC 换成 adc-emul，门控脚换成 gpio-emul。

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(battery_floor C)

set(FW ${CMAKE_CURRENT_LIST_DIR}/../../nrf52840)

target_sources(app PRIVATE
    src/main.c
    ${FW}/src/battery.c
)

target_include_directories(app PRIVATE ${FW}/src)
