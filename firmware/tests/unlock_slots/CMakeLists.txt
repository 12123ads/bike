# 密钥槽位复用的测试（native_sim + ztest + 真 PSA + 假 nvstore）。
#
# 为什么值得单测：审计 M7 修的是 `unlock_del_secret` / `unlock_wipe_secrets`
# 要清 `counter`。不清的后果**不是拒绝开锁，而是拒绝一个合法的新手机**：
# 槽位被新 uid 复用时继承了旧 uid 的 counter（可能已经很大），新手机从
# counter=1 开始的合法请求全部撞上 `counter <= u->counter` 被拒 ——
# 而日志只说「counter 未递增」，看起来像重放攻击。
#
# 上一轮的验证方式是「固件编译 + bsim」，而 bsim 的 8 条断言里**没有**
# del/wipe 场景 —— 把 `counter = 0` 删掉那 8 条照样全过。这个应用补上它。
#
# 被测的是 firmware/nrf52840/src/unlock.c 的**原件** + 真的 PSA crypto
# （bsim 上同样是 Oberon 软件驱动）。只有 nvstore 是假的 —— 它要 flash，
# 而这里测的是内存里 users[] 的槽位语义。

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(unlock_slots C)

set(FW ${CMAKE_CURRENT_LIST_DIR}/../../nrf52840)

target_sources(app PRIVATE
    src/main.c
    ${FW}/src/unlock.c
    ${FW}/src/crypto.c
)

target_include_directories(app PRIVATE ${FW}/src)
