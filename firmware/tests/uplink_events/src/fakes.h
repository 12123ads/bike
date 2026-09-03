/*
 * uplink.c 的替身外设状态。测试通过这些字段控制假实现的行为，
 * 并读回「被调用了什么」。
 */

#ifndef EBIKE_TEST_FAKES_H
#define EBIKE_TEST_FAKES_H

#include "battery.h"
#include "crypto.h"
#include "gnss.h"
#include "lock.h"
#include "modem.h"
#include "motion.h"
#include "nvstore.h"
#include "unlock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#define FAKE_PUBLISH_MAX 16

struct fake_state {
	/* --- modem 的可编程返回值 --- */
	bool connected;
	int connect_rc;
	int reconnect_rc;
	int publish_rc;
	uint32_t utc;

	int connect_calls;
	int reconnect_calls;
	int disconnect_calls;

	modem_dn_cb dn_cb;

	/* 发出去的报文（topic + payload 原文） */
	struct {
		char topic[64];
		char payload[256];
		size_t len;
	} published[FAKE_PUBLISH_MAX];
	size_t publish_n;

	/* 卡住第一次 topic 含这个子串的 publish。空串 = 不卡。
	 * 这是测锁纪律的关键：`publish_retry` 里那条最坏三分钟的重连阶梯，
	 * 在测试里就是这个门。 */
	char block_publish_on_topic[32];
	struct k_sem publish_gate;      /* 测试 give 它放 publish 走 */
	struct k_sem publish_entered;   /* publish 进门时 give，测试 take */

	/* --- 其它外设 --- */
	int batt_mv;
	int unlock_calls;
	int lock_calls;
	uint32_t q;
	uint32_t report_interval;

	int secret_rc;
	int set_secret_calls;
	int del_secret_calls;
	int wipe_calls;
	uint32_t set_secret_uid;
	uint16_t set_secret_kid;
	uint32_t del_secret_uid;
};

extern struct fake_state fake;

void fake_reset(void);

#endif /* EBIKE_TEST_FAKES_H */
