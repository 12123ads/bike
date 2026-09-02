# 电瓶车定位固件（nRF52840 / NCS-Zephyr）
#
# ⚠ 本文件在写入时**没有被编译验证过** —— 这台机器上没有 NCS/Zephyr SDK，
#    也没有任何 C 编译器（`gcc: command not found`）。语法与 API 用法是按
#    DESIGN.md 里已核实的符号名写的，但**第一次 `west build` 一定会有编译错误**。
#    见 docs/FIRMWARE.md 的「怎么编译」一节。

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ebike_tracker C)

target_sources(app PRIVATE
    src/main.c
    src/proto.c
    src/nvstore.c
    src/crypto.c
    src/unlock.c
    src/ble_unlock.c
    src/motion.c
    src/battery.c
    src/lock.c
    src/gnss.c
    src/modem.c
    src/uplink.c
)

target_include_directories(app PRIVATE src)
