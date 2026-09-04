/*
 * Air780EP 作为纯 4G modem：AT 状态机（DESIGN.md §8）。
 *
 * 这是全项目最难调的一块（§8.5 估 2000~4000 行 C）。本实现是**能跑通主路径的
 * 最小版本**，不是那 4000 行的完整体 —— 缺的东西在文件末尾列了。
 *
 * 三个必须知道的前提：
 *
 * 1. **9600 baud 锁死**（§8.1）。AT+IPR 默认自适应太脆，而两种低功耗模式
 *    都要求 9600 才能可靠唤醒。出厂 `AT+IPR=9600;&W` 设一次。
 *    后果：单包 4100 字节在 HEX 模式下是 8200 字符 ≈ 8.5 秒。
 * 2. **PWRKEY 已在硬件上接地**（2026-09-04）：模组随 VBAT 上电自启，
 *    **无法关机**（UM1.0.7 §5.3.4.1.2）。所以没有 PWRKEY GPIO，也没有
 *    power_on/power_off；重启只有 AT+CFUN=1,1 软重启，省电档只能走
 *    PSM+（用 DTR 拉低退出，见 modem_wake）。
 */

#ifndef EBIKE_MODEM_H
#define EBIKE_MODEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 收到下行时的回调。topic 和 payload 都指向内部缓冲，回调返回后失效。 */
typedef void (*modem_dn_cb)(const char *topic, const uint8_t *payload,
			    size_t len);

int modem_init(modem_dn_cb cb);

/* 开机 + 附着网络 + 连 MQTT（含 TLS）。整个过程可能几十秒。**阻塞**。 */
int modem_connect(void);

/* 掉线后恢复。三级阶梯，从最便宜的开始，只有失败才往下走：
 *
 *   1 级 MQTT 会话     ~5~10 s   服务端重启、keepalive 超时、TLS 会话过期
 *   2 级 PDP 重拨      ~15~70 s  承载被运营商拆了（`+PDP DEACT`）
 *   3 级 模组重启      ~30~90 s  模组卡死，AT 都不应答
 *
 * **不做无限重试**：三级都失败就返回错误，由调用方放弃本轮、关掉模组
 * 等下一个上报周期。在这里死循环会把车电池抽干（§4.4：设备没有独立电源）。
 *
 * 只在 `modem_is_connected()` 变 false 之后调用；模组从没连过就用
 * `modem_connect()`。**阻塞**，最坏约 3 分钟。 */
int modem_reconnect(void);

/* 断开 MQTT 并关机。省电档的默认状态。 */
int modem_disconnect(void);

bool modem_is_connected(void);

/* 发布一条。retain 恒为 false 由调用方保证（契约 §4.1）。 */
int modem_publish(const char *topic, const uint8_t *payload, size_t len,
		  int qos);

/* 处理积压的 URC。主循环里定期调 —— 下行是靠 URC 推上来的。 */
int modem_poll(uint32_t timeout_ms);

/* AT+CSQ 的 rssi 档，负数表示读不到。用于 up/tele 的 csq 字段。 */
int modem_csq(void);

/* --- DTR / RI（PWRKEY 接地后的唤醒与通知线） ---------------------------------
 *
 * modem_disconnect 的注释里说了「不关机」。中间态的省电靠这两个：
 * PSM+ 档进来用 AT+POWERMODE（还没接，见 modem.c 末尾「还缺什么」#3），
 * 出来用 DTR；RI 是模组 → 主控的「有下行」通知（120 ms 低脉冲），
 * 拉低 AT+CFGRI 后才出。当前接线：DTR 开漏只拉低，RI 直连，见
 * boards/ 那份 overlay 的电压域推导。 */

/* 拉低 DTR ~100 ms 再释放。CSCLK=1 档 = 从休眠唤醒；
 * PSM+ 档 = 退出（⚠ 释放成高 = 重新进入，见 modem.c 的语义表）。 */
int modem_wake(void);

/* 把 DTR 保持拉低（hold=true）或释放（hold=false）。
 * PSM+ 下要在「醒着」做一串 AT 交互时用，做完再释放。 */
int modem_hold_awake(bool hold);

/* RI 线是否来过脉冲（读即清除）。true = 下行可能在路上，
 * 调用方可以把 modem_poll 提前跑。未接 RI 时恒 false。 */
bool modem_ri_pending(void);

/* 运营商 NITZ 时间（免费、无往返，§8.1）。0 = 还没拿到。
 * 契约 §5.6：拿到之前上行的 t 填 0。 */
uint32_t modem_utc(void);

/* 基站定位降级（§9.5 / R9 的验收项）。AT+CIPGSMLOC=1,1，免费。 */
int modem_lbs(double *lat, double *lon, float *acc_m);

#endif /* EBIKE_MODEM_H */
