/*
 * 锁执行机构。
 *
 * ⚠ **型号未定**（DESIGN.md §7 第 1 条 / §11 #10）：电磁锁（通电开、断电锁）
 * 还是电机锁（转一下）没定，这决定了驱动电路、线数和功耗。
 *
 * 本文件按**最保守的假设**写：一根线、高有效、脉冲驱动，
 * 加一个位置反馈微动开关。这两种锁都能这么驱动，只是：
 *   - 电磁锁：脉冲期间通电 = 开着，脉冲结束自动锁上 → PULSE_MS 要是「开门时长」
 *   - 电机锁：脉冲驱动电机转一下 → PULSE_MS 是「转一圈的时间」
 * 定了之后要改的只有 PULSE_MS 和 lock_engage 的语义，电机锁还要加半桥反转。
 */

#ifndef EBIKE_LOCK_H
#define EBIKE_LOCK_H

#include <stdbool.h>

int lock_init(void);

/* 开锁。返回 0 = 驱动信号发出去了（**不代表锁真的开了** ——
 * 那要看 lock_is_open() 的反馈开关）。 */
int lock_unlock(void);

/* 上锁。电磁锁是「断电即锁」，所以这个函数对它来说是空操作。 */
int lock_lock(void);

/* 位置反馈微动开关的读数。没接开关时返回 false。 */
bool lock_is_open(void);

/* 反馈开关变化的回调 —— 用来发 lock_state 事件（契约 §5.4）。 */
typedef void (*lock_state_cb)(bool open);
void lock_set_callback(lock_state_cb cb);

#endif /* EBIKE_LOCK_H */
