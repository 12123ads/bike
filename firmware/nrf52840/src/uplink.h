/*
 * 上行汇聚：把 GNSS / 遥测 / 事件打包发出去，并处理下行。
 *
 * 这一层的存在理由是**把「模组要开机」这件事收在一个地方**。
 * 省电档下模组是关着的（契约 §4.1），所以「发一条上行」实际是
 * 「开机 → 连 MQTT → 发积压的 → 收下行 → 关机」的一整轮。
 */

#ifndef EBIKE_UPLINK_H
#define EBIKE_UPLINK_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "motion.h"
#include "proto.h"

int uplink_init(void);

/* 跑一整轮上报：开模组 → 定位 → 发 hello/loc/tele → 收下行 → 关模组。
 * **阻塞几十秒**，在专门的线程里调。 */
int uplink_cycle(bool want_gnss);

/* 排一条事件，等下一轮上报时一起发（省电档下不为一条事件单独开模组）。
 * 例外：`unlock_deny` 和 `lowbatt` 会立刻触发一轮上报。 */
int uplink_queue_event(enum proto_event ev, const char *detail_json);

/* 立刻上报（被运动唤醒或收到 locate 指令时用）。 */
void uplink_request_now(void);

/* 主循环等的信号量 —— uplink_request_now() 会给它。 */
struct k_sem *uplink_now_sem(void);

#endif /* EBIKE_UPLINK_H */
