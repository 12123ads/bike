# 断线重连阶梯的测试（native_sim + ztest + 假模组）。
#
# 为什么值得单测：阶梯的价值不在「能不能恢复」，而在「只重跑坏掉的那一层」。
# 无脑走 3 级也能恢复连接，功能断言照样绿 —— 区别只在 5 s vs 90 s，
# 以及那 90 秒里模组全功率约 22 mA 直接吃车电池。
# 所以判据只能是「跑了哪些 AT 命令」，这就要求能观察串口上的字节流。
#
# 被测的是 firmware/nrf52840/src/modem.c 的**原件**，一行没为测试改过。
# uart-emul 上挂一个假模组线程记 trace 并按脚本回应。

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(modem_reconnect C)

set(FW ${CMAKE_CURRENT_LIST_DIR}/../../nrf52840)

target_sources(app PRIVATE
    src/main.c
    ${FW}/src/modem.c
    ${FW}/src/proto.c
)

target_include_directories(app PRIVATE ${FW}/src)
