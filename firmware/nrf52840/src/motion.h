/*
 * LIS2DW12 运动唤醒 —— 唯一的外部唤醒源（DESIGN.md §2.7）。
 * BLE 唤不醒设备：radio 在 System OFF 下断电，手机连不上一台已关机的车。
 *
 * ⚠ 阈值**只能运行时设**，不是 DTS 属性（DESIGN.md §3.7）：
 *   SENSOR_ATTR_UPPER_THRESH，单位 mg，驱动内部用 MG_TO_WK_THS_LSB 换算。
 *   一格 = FS/64 = 31.25 mg @±2 g。
 *
 * ⚠ **INT1/INT2 路由已定论**（2026-09-02，读 NCS v3.4.0 的驱动源码 + 编译验证，
 * 解决 DESIGN.md §3.7 的历史矛盾）：
 *   - `SENSOR_TRIG_MOTION`     → `ctrl4_int1_pad_ctrl.int1_wu`      = **INT1**
 *   - `SENSOR_TRIG_STATIONARY` → `ctrl5_int2_pad_ctrl.int2_sleep_chg` = **INT2**
 *     （`lis2dw12_trigger.c:77-98`，两处 route_set 是不同寄存器，不是同一个）
 *
 * 也就是说 ADR-002 §1.6 是对的、§1.4/§1.7 是错的。**本板只接了 INT1**，
 * 所以 STATIONARY 的中断线物理上不存在。
 *
 * **本实现选了出路 (c)：软件计时判静止。** `motion.c` 里**不注册**
 * `SENSOR_TRIG_STATIONARY` —— 注册它会返回 0（驱动只写寄存器不检查引脚），
 * 但事件永不到达，那是「配置成功、功能静默失效」，最难查的一类。
 * 上一版就栽在这里：`main.c` 的 `moving` 只在 STILL 分支清，
 * 于是**车动过一次之后 BLE 广播永远关不掉**，DESIGN.md §2.4 那条
 * 「静止 5 分钟后关广播（防跟踪）」直接失效，而日志里看不出任何异常。
 *
 * 另两条出路仍然成立，但都要动硬件或越过驱动，**需要时在 R2 拍板**：
 *   a) 接第二根线到 INT2（§3.5 现在有 4 个余量脚，装得下）；
 *   b) 写 `CTRL_REG7.int2_on_int1`（芯片支持把 INT2 事件并到 INT1，
 *      HAL 有 `lis2dw12_all_on_int1_set()`）—— **但 Zephyr 驱动从不调它，
 *      也没有对应的 DTS 属性**，要自己在 motion_init 里越过驱动直接写 I2C。
 *
 * 静止超时用 `CONFIG_EBIKE_STILL_AFTER_S`。它和 `main.c` 的
 * `IDLE_AFTER_STILL_MS`（关广播）是两个独立的门槛：前者判「不动了」，
 * 后者判「不动够久了，可以关广播」。前者必须明显小于后者，
 * 否则关广播的条件永远等不到 —— `motion.c` 里有 BUILD_ASSERT 钉这一条。
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

/* 当前状态。加锁读，和回调看到的是同一份。
 *
 * ⚠ **`main.c` 不该再自己存一份 `moving`。** 它以前存是因为静止事件
 * 永不到达、那份 bool 是唯一能翻的东西；现在状态机在本模块里是完整的，
 * 两份状态只会分叉。 */
enum motion_state motion_current(void);

/* 「刚有活动」—— 把静止倒计时往后推。
 *
 * 给的是**非加速度**来源的活动用的：BLE 连上来、收到开锁请求。
 * 有人正在动这辆车，即使加速度计没到阈值，也不该在这时判静止把广播关掉。
 *
 * 加速度事件不需要调这个，`trigger_handler()` 自己会推。 */
void motion_note_activity(void);

/* 曾经有个 motion_prepare_for_sleep()：已删除。
 * 它是个空壳，注释还声称「驱动已经把引脚配好了不用额外动作」——那是错的：
 * 驱动配的是边沿触发（GPIOTE IN 事件），而 GPIOTE 在 System OFF 下断电。
 * 正确的武装动作（改成 level sense + 等引脚回低 + suspend 外设）有严格顺序，
 * 整段放在 main.c 的 enter_system_off() 里。 */

#endif /* EBIKE_MOTION_H */
