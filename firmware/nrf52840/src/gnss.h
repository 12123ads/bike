/*
 * GNSS：ATGM336H-5N，NMEA over UART 9600。
 *
 * 支持北斗（NEO-7M 在协议层没有北斗且已 EOL，所以选它 —— DESIGN.md §1）。
 *
 * 电源门控：GNSS 模块工作时是几十 mA 量级，不能常开。
 * 采一次位置的流程是「上电 → 等定位 → 读 → 断电」，冷启动可能要几十秒，
 * 所以 gnss_fix() 是阻塞的，必须在自己的线程里调，不能在中断或 NFC 回调里。
 */

#ifndef EBIKE_GNSS_H
#define EBIKE_GNSS_H

#include <stdbool.h>
#include <stdint.h>

struct gnss_fix {
	double lat;
	double lon;
	float hdop;      /* 用来估精度圈 */
	float acc_m;     /* 由 hdop 估出来的水平精度，米 */
	float speed_ms;
	int16_t heading;
	uint8_t sats;
	uint32_t utc;    /* NMEA 里的 UTC 秒；0 = 无效 */
	bool valid;
};

int gnss_init(void);

/* 开电、等定位、关电。timeout_s 内没定上返回 -ETIMEDOUT。
 * **阻塞**，别在回调里调。 */
int gnss_fix(struct gnss_fix *out, uint32_t timeout_s);

/* 强制断电 —— 进休眠前调，防止上一次采样异常退出后电源没关。 */
int gnss_power_off(void);

#endif /* EBIKE_GNSS_H */
