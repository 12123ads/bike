/*
 * uplink.c 的硬件依赖替身。
 *
 * 这些函数的**行为由测试逐条控制**（见 fakes.h 的那几个钩子），所以是
 * 「可编程的假实现」而不是空壳 —— 空壳测不出锁纪律：判据是
 * 「publish 阻塞期间另一个线程能不能入队」，那要求 publish 能被卡住。
 *
 * 只顶掉外设层。`uplink.c` 和 `proto.c` 是原件。
 */

#include "fakes.h"

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>

/* --- 可编程状态 -------------------------------------------------------------- */

struct fake_state fake;

void fake_reset(void)
{
	memset(&fake, 0, sizeof(fake));
	fake.connected = true;
	fake.utc = 1756900000;
	fake.batt_mv = 52000;
	k_sem_init(&fake.publish_gate, 0, 1);
	k_sem_init(&fake.publish_entered, 0, 1);
}

/* --- modem ----------------------------------------------------------------- */

int modem_init(modem_dn_cb cb)
{
	fake.dn_cb = cb;
	return 0;
}

int modem_connect(void)
{
	fake.connect_calls++;
	return fake.connect_rc;
}

int modem_reconnect(void)
{
	fake.reconnect_calls++;
	return fake.reconnect_rc;
}

int modem_disconnect(void)
{
	fake.disconnect_calls++;
	return 0;
}

bool modem_is_connected(void)
{
	return fake.connected;
}

int modem_publish(const char *topic, const uint8_t *payload, size_t len,
		  int qos)
{
	ARG_UNUSED(qos);

	if (fake.publish_n < FAKE_PUBLISH_MAX) {
		strncpy(fake.published[fake.publish_n].topic, topic,
			sizeof(fake.published[0].topic) - 1);
		size_t n = len < sizeof(fake.published[0].payload) - 1
				   ? len : sizeof(fake.published[0].payload) - 1;
		memcpy(fake.published[fake.publish_n].payload, payload, n);
		fake.published[fake.publish_n].payload[n] = '\0';
		fake.published[fake.publish_n].len = len;
		fake.publish_n++;
	}

	/* 卡住这一次 publish —— 模拟 publish_retry 里那条最坏三分钟的重连阶梯。
	 * 测试用 `publish_entered` 知道「已经进来了」，用 `publish_gate` 决定
	 * 什么时候放它走。 */
	if (fake.block_publish_on_topic[0] != '\0' &&
	    strstr(topic, fake.block_publish_on_topic) != NULL) {
		fake.block_publish_on_topic[0] = '\0';   /* 只卡第一次 */
		k_sem_give(&fake.publish_entered);
		(void)k_sem_take(&fake.publish_gate, K_SECONDS(10));
	}

	return fake.publish_rc;
}

int modem_poll(uint32_t timeout_ms)
{
	ARG_UNUSED(timeout_ms);
	return 0;
}

int modem_csq(void)
{
	return 19;
}

uint32_t modem_utc(void)
{
	return fake.utc;
}

int modem_lbs(double *lat, double *lon, float *acc_m)
{
	ARG_UNUSED(lat);
	ARG_UNUSED(lon);
	ARG_UNUSED(acc_m);
	return -ENOTSUP;
}

/* --- gnss ------------------------------------------------------------------ */

int gnss_init(void)
{
	return 0;
}

int gnss_fix(struct gnss_fix *out, uint32_t timeout_s)
{
	ARG_UNUSED(timeout_s);
	memset(out, 0, sizeof(*out));
	out->heading = -1;
	out->speed_ms = -1.0f;
	return -ETIMEDOUT;
}

int gnss_power_off(void)
{
	return 0;
}

/* --- battery --------------------------------------------------------------- */

int battery_init(void)
{
	return 0;
}

int battery_read_mv(void)
{
	return fake.batt_mv;
}

int battery_low_level(int mv)
{
	ARG_UNUSED(mv);
	return 0;
}

/* --- lock ------------------------------------------------------------------ */

int lock_init(void)
{
	return 0;
}

int lock_unlock(void)
{
	fake.unlock_calls++;
	return 0;
}

int lock_lock(void)
{
	fake.lock_calls++;
	return 0;
}

bool lock_is_open(void)
{
	return false;
}

void lock_set_callback(lock_state_cb cb)
{
	ARG_UNUSED(cb);
}

/* --- unlock ---------------------------------------------------------------- */

int unlock_set_secret(uint32_t uid, const uint8_t secret[SECRET_LEN],
		      uint16_t kid)
{
	ARG_UNUSED(secret);
	fake.set_secret_uid = uid;
	fake.set_secret_kid = kid;
	fake.set_secret_calls++;
	return fake.secret_rc;
}

int unlock_del_secret(uint32_t uid)
{
	fake.del_secret_uid = uid;
	fake.del_secret_calls++;
	return fake.secret_rc;
}

int unlock_wipe_secrets(void)
{
	fake.wipe_calls++;
	return fake.secret_rc;
}

uint16_t unlock_current_kid(void)
{
	return fake.set_secret_kid;
}

/* --- nvstore --------------------------------------------------------------- */

int nvstore_init(void)
{
	return 0;
}

uint32_t nvstore_next_q(void)
{
	return ++fake.q;
}

uint32_t nvstore_boot_count(void)
{
	return 1;
}

void nvstore_queue_counter(uint32_t uid, uint32_t counter)
{
	ARG_UNUSED(uid);
	ARG_UNUSED(counter);
}

int nvstore_flush(void)
{
	return 0;
}

uint32_t nvstore_report_interval(void)
{
	return 900;
}

int nvstore_set_report_interval(uint32_t seconds)
{
	if (seconds < 60 || seconds > 86400) {
		return -EINVAL;
	}
	fake.report_interval = seconds;
	return 0;
}

/* --- motion ---------------------------------------------------------------- */

int motion_init(motion_cb cb)
{
	ARG_UNUSED(cb);
	return 0;
}

int motion_set_threshold_mg(uint16_t mg)
{
	ARG_UNUSED(mg);
	return 0;
}

void motion_note_activity(void)
{
}

enum motion_state motion_current(void)
{
	return MOTION_STILL;
}

/* --- crypto ---------------------------------------------------------------- */

void crypto_wipe(void *p, size_t len)
{
	memset(p, 0, len);
}
