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
 *
 * 2. **模组上电不自启动**（§8.3）。PWRKEY 开集拉低 >1 s 才开机。
 *    内部已有 5.6k 上拉，不要外加。
 *
 * 3. **`AT^WAKEUPHEX` 的可用性未核实**（§8.7 的硬门禁 / §11 #17）。
 *    不配它的话，每条例行 URC 都会把主控从 System OFF 拽出来，功耗预算直接崩，
 *    而且**没有替代的过滤手段**。R0 阶段就要用一根 UART 线验这一条。
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

/* 运营商 NITZ 时间（免费、无往返，§8.1）。0 = 还没拿到。
 * 契约 §5.6：拿到之前上行的 t 填 0。 */
uint32_t modem_utc(void);

/* 基站定位降级（§9.5 / R9 的验收项）。AT+CIPGSMLOC=1,1，免费。 */
int modem_lbs(double *lat, double *lon, float *acc_m);

#endif /* EBIKE_MODEM_H */
