# 模组时间串 → Unix 秒的换算测试（native_sim + ztest）。
#
# 为什么值得单测：这段换算有两个「不知道就一定算错、而且错得很隐蔽」的点 ——
# ±zz 的单位是 1/4 小时（东八区 = +32，当成小时读差 24 小时），
# 以及 hh:mm:ss 是本地时间必须减偏移（不减差 8 小时）。
# 两种错法都产出一个「看起来完全合理」的时间戳，只有对着已知答案算才发现。
#
# 被测的是 firmware/nrf52840/src/modem.c 的**原件**，一行没为测试改过。
# modem.c 需要 uart1 和 modem_pwrkey 两个 DT 节点，overlay 里用
# uart-emul + gpio-emul 顶上（见 boards/native_sim.overlay）。

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(modem_time C)

set(FW ${CMAKE_CURRENT_LIST_DIR}/../../nrf52840)

target_sources(app PRIVATE
    src/main.c
    # 被测原件。proto.c 也要带上：modem.c 用了 PROTO_MAX_PAYLOAD 和几个 topic 宏，
    # 而那些宏在 proto.h 里，不需要 proto.c 的函数 —— 但链接器要 proto.c 里
    # 定义的东西时会报 undefined，带上比后面追查省事。
    ${FW}/src/modem.c
    ${FW}/src/proto.c
)

target_include_directories(app PRIVATE ${FW}/src)
