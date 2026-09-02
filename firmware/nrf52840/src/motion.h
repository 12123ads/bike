/*
 * LIS2DW12 运动唤醒 —— 主唤醒源（DESIGN.md §2.5：NFC 场唤醒不划算，
 * NFCT ACTIVATED 400 µA，而且安卓大部分时间不发场）。
 *
 * ⚠ 阈值**只能运行时设**，不是 DTS 属性（DESIGN.md §3.7）：
 *   SENSOR_ATTR_UPPER_THRESH，单位 mg，驱动内部用 MG_TO_WK_THS_LSB 换算。
 *   一格 = FS/64 = 31.25 mg @±2 g。
 *
 * ⚠ INT1/INT2 路由在历史文档里自相矛盾（§3.7 的未解决项 #2）：
 *   ADR-002 §1.4/§1.7 说 INT2 不用，§1.6 的状态机说 STATIONARY 走 int2_sleep_chg。
 *   本文件按「都挂在 INT1」写，如果 R2 阶段发现 STATIONARY 只出现在 INT2，
 *   就要多接一根线（§3.5 的余量装得下）或退回软件计时判静止。
 */

#ifndef EBIKE_MOTION_H
#define EBIKE_MOTION_H

#include <stdbool.h>
#include <stdint.h>

enum motion_state {
	MOTION_UNKNOWN,
	MOTION_MOVING,
	MOTION_STILL,
};

typedef void (*motion_cb)(enum motion_state state, uint16_t mg);

int motion_init(motion_cb cb);

/* 设阈值。100~200 mg 是电瓶车的起调范围。 */
int motion_set_threshold_mg(uint16_t mg);

enum motion_state motion_current(void);

/* 进 System OFF 前调：确认中断被配成能唤醒芯片的形式。 */
int motion_prepare_for_sleep(void);

#endif /* EBIKE_MOTION_H */
