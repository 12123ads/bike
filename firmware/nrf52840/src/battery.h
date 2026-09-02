/*
 * 电池电压采样 —— 必须门控（DESIGN.md §3.6）。
 *
 * 1M+1M 分压对 58.8 V 是 **29 µA 常流**，比整机静态电流大一个量级。
 * 所以分压器低端串一个 MOSFET，只在采样的瞬间打开。
 * 这是整个功耗预算成立的前提之一。
 */

#ifndef EBIKE_BATTERY_H
#define EBIKE_BATTERY_H

#include <stdint.h>

int battery_init(void);

/* 采一次电压，毫伏。返回负数表示失败。
 * 里面会开门控 → 等稳定 → 采 → 关门控，整个过程约 1 ms。 */
int battery_read_mv(void);

/* 欠压等级（DESIGN.md §6 的四级软件兜底）：
 *   0 = 正常，或读失败（读失败不当欠压 —— 误报会让人白跑一趟）
 *   1 = 跨过第一阈值：降低上报频率，并把状态报上去让 HA 能看到
 *   2 = 跨过第二阈值：停止周期上报，只保留手机 NFC 开锁（那条路完全离线）
 *   3 = 跨过第三阈值：主动进 System OFF，只留运动唤醒
 *
 * 这一路保护的是**车电池**别被抽空，不保护设备自己 —— 设备没有独立电源（§4.4）。
 */
int battery_low_level(int mv);

#endif /* EBIKE_BATTERY_H */
