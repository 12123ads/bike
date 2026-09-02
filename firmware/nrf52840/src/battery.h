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

/* 欠压等级：0 = 正常，1 = 跨过第一阈值，2 = 跨过第二阈值（DESIGN.md §6）。 */
int battery_low_level(int mv);

#endif /* EBIKE_BATTERY_H */
