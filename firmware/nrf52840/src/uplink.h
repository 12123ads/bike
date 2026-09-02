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

/* --- 欠压兜底（DESIGN.md §6 的四级） ----------------------------------------
 *
 * 每轮上报后由 uplink_cycle 自己调 uplink_note_battery()，把最新的欠压等级
 * 交给下面两个查询函数。主循环靠它们决定「这一轮还要不要上报」和
 * 「是不是该进 System OFF 了」。
 *
 * 为什么状态放在 uplink 而不是 battery：battery 只负责采一次电压（无状态），
 * 而「跨过阈值之后行为怎么变」是上报编排的事。
 */
void uplink_note_battery(int mv, int level);

/* 当前欠压等级（0~3）。没测过电压之前是 0。 */
int uplink_batt_level(void);

/* 这一轮该不该做周期上报。
 * 等级 2 以上返回 false —— 只保留离线开锁和运动触发的上报（§6 第 3 级）。 */
bool uplink_should_report(void);

/* 主循环等的信号量 —— uplink_request_now() 会给它。 */
struct k_sem *uplink_now_sem(void);

#endif /* EBIKE_UPLINK_H */
