# NMEA 解析的测试（native_sim + ztest + uart-emul 假 GNSS 模块）。
#
# 为什么值得单测：`checksum_ok` 用 `strtol` 解校验和，而 `strtol` 解析失败
# 也返回 0 且不设错误标记 —— 于是「校验和字段是垃圾」和「校验和就是 00」
# 不可区分。实测 `$GGAAGG*--` 的 XOR 恰好是 0x00，坏行被当好行放进
# parse_gga（审计 R4）。这类「防线有洞但下游恰好兜住了」只有把真实的坏行
# 喂进完整链路才看得出来。
#
# 被测的是 firmware/nrf52840/src/gnss.c 的**原件**，一行没为测试改过。
# GNSS 的 UART 换成 uart-emul，电源门控脚换成 gpio-emul。

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gnss_nmea C)

set(FW ${CMAKE_CURRENT_LIST_DIR}/../../nrf52840)

target_sources(app PRIVATE
    src/main.c
    ${FW}/src/gnss.c
)

target_include_directories(app PRIVATE ${FW}/src)
