# 下行 URC 解析 + AT 应答匹配的运行时测试（native_sim + ztest）。
#
# 为什么值得单测：这两条都属于「编得过、跑得起来、只是静默做错事」那一类，
# 和 §3c 里最难抓的两个 bug 同型。
#
#   M1  行缓冲超长时**静默截断**。截断后的 HEX 仍是合法偶数长度，解码成功，
#       `{"c":"interval","a":{"s":900}}` 被截成 `..."s":90}` 也能正常解析 ——
#       上报周期静默改错 10 倍。解码层看不出来，只能在读行那一层挡。
#   M2  `at_cmd()` 用 `strstr(line,"OK")` 判成功，而 `SEND OK` / `CONNECT OK`
#       都是带外 URC。一条迟到的 `SEND OK` 能替下一条命令「答到」——
#       `AT+CPOWD=1` 没执行却返回成功，模组不关机，持续耗电。
#
# 被测的是 firmware/nrf52840/src/modem.c 的**原件**，一行没为测试改过。
# 入口全部走公开面（modem_poll / modem_csq + dn_cb），和真设备上一致。

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(modem_downlink C)

set(FW ${CMAKE_CURRENT_LIST_DIR}/../../nrf52840)

target_sources(app PRIVATE
    src/main.c
    ${FW}/src/modem.c
    ${FW}/src/proto.c
)

target_include_directories(app PRIVATE ${FW}/src)
