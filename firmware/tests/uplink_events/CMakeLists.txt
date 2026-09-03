# 事件队列锁纪律的测试（native_sim + ztest + 假外设）。
#
# 为什么值得单测：`flush_events()` 的价值不在「事件能不能发出去」，而在
# **发送期间不持 `ev_lock`**。持锁发送在功能上完全正确 —— 事件照样发出去，
# 任何「事件到达服务端」的断言都是绿的。区别只在阻塞：`publish_retry` 含
# 最坏三分钟的重连阶梯，持锁期间
#   · LIS2DW12 驱动线程的 `on_motion → uplink_queue_event` 卡住
#     （motion.c 自己写着「阻塞期间的中断会丢」）
#   · 系统工作队列上的 `still_work` / `sense_work` / nvstore 的
#     `counter_work` 全排在后面 —— 包括 `lock.c` 的**释放脉冲**
#     `release_work`，被卡就是电磁锁持续通电（几百 mA）
# 全部停摆，而且恰在「车被撬、事件密集、网络又差」的时刻。
#
# 所以判据只能是「发送阻塞时，另一个线程还能不能入队」。
#
# 被测的是 firmware/nrf52840/src/uplink.c 的**原件**，一行没为测试改过；
# 硬件依赖（modem / gnss / battery / lock / unlock / nvstore）由 src/fakes.c
# 顶掉 —— 它们的行为由测试逐条控制。

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(uplink_events C)

set(FW ${CMAKE_CURRENT_LIST_DIR}/../../nrf52840)

target_sources(app PRIVATE
    src/main.c
    src/fakes.c
    ${FW}/src/uplink.c
    ${FW}/src/proto.c
)

target_include_directories(app PRIVATE ${FW}/src)
