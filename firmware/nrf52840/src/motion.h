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

/* 当前状态。**注意 main.c 用的是自己那份 `moving`** —— 那份由回调更新，
 * 是「上次事件说的」；这个是查询驱动状态机。两者一致，留这个是给调试用。 */
enum motion_state motion_current(void);

/* 曾经有个 motion_prepare_for_sleep()：已删除。
 * 它是个空壳，注释还声称「驱动已经把引脚配好了不用额外动作」——那是错的：
 * 驱动配的是边沿触发（GPIOTE IN 事件），而 GPIOTE 在 System OFF 下断电。
 * 正确的武装动作（改成 level sense + 等引脚回低 + suspend 外设）有严格顺序，
 * 整段放在 main.c 的 enter_system_off() 里。 */

#endif /* EBIKE_MOTION_H */
