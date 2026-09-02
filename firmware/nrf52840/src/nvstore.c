/* 持久化。见 nvstore.h 里关于 q 预留和 counter 延迟写的说明。 */

#include "nvstore.h"
#include "unlock.h"

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nvstore, CONFIG_EBIKE_LOG_LEVEL);

#define KEY_Q         "ebike/q"
#define KEY_BOOT      "ebike/boot"
#define KEY_USERS     "ebike/users"
#define KEY_KID       "ebike/kid"
#define KEY_INTERVAL  "ebike/itv"

/* counter 合并写的延迟。5 秒：足够把「连续贴两下」合成一次写，
 * 又短到用户不会在这段时间内把车骑走并断电。 */
#define COUNTER_FLUSH_DELAY  K_SECONDS(5)

struct persisted {
	uint32_t q_ceiling;      /* flash 上记的 q 上界 */
	uint32_t boot_count;
	uint16_t kid;
	uint32_t report_interval;
};

static struct persisted p;
static uint32_t q_next;          /* RAM 里的下一个 q */
static bool counter_dirty;
static struct k_work_delayable counter_work;
static struct k_mutex lock;

/* users 的镜像。counter 延迟写要用它，所以 nvstore 也持有一份。 */
static struct user_key user_mirror[MAX_USERS];
static bool mirror_valid;

/* --- settings 回调 ----------------------------------------------------------- */

static int on_setting(const char *name, size_t len, settings_read_cb read_cb,
		      void *cb_arg)
{
	if (settings_name_steq(name, "q", NULL)) {
		return read_cb(cb_arg, &p.q_ceiling, sizeof(p.q_ceiling)) < 0
			       ? -EINVAL : 0;
	}
	if (settings_name_steq(name, "boot", NULL)) {
		return read_cb(cb_arg, &p.boot_count, sizeof(p.boot_count)) < 0
			       ? -EINVAL : 0;
	}
	if (settings_name_steq(name, "kid", NULL)) {
		return read_cb(cb_arg, &p.kid, sizeof(p.kid)) < 0 ? -EINVAL : 0;
	}
	if (settings_name_steq(name, "itv", NULL)) {
		return read_cb(cb_arg, &p.report_interval,
			       sizeof(p.report_interval)) < 0 ? -EINVAL : 0;
	}
	if (settings_name_steq(name, "users", NULL)) {
		/* 长度必须精确匹配，否则说明落盘格式变了 —— 宁可当没有，
		 * 也不要把错位的字节当密钥用。 */
		if (len != sizeof(user_mirror)) {
			LOG_ERR("密钥块长度 %zu，期望 %zu —— 忽略（格式变了？）",
				len, sizeof(user_mirror));
			return 0;
		}
		if (read_cb(cb_arg, user_mirror, sizeof(user_mirror)) < 0) {
			return -EINVAL;
		}
		mirror_valid = true;
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(ebike, "ebike", NULL, on_setting, NULL, NULL);

/* --- counter 延迟写 ---------------------------------------------------------- */

static void counter_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	k_mutex_lock(&lock, K_FOREVER);
	if (counter_dirty) {
		int rc = settings_save_one(KEY_USERS, user_mirror,
					   sizeof(user_mirror));
		if (rc != 0) {
			LOG_ERR("counter 落盘失败 rc=%d", rc);
		} else {
			counter_dirty = false;
		}
	}
	k_mutex_unlock(&lock);
}

void nvstore_queue_counter(uint32_t uid, uint32_t counter)
{
	k_mutex_lock(&lock, K_FOREVER);
	for (size_t i = 0; i < MAX_USERS; i++) {
		if (user_mirror[i].valid && user_mirror[i].uid == uid) {
			user_mirror[i].counter = counter;
			counter_dirty = true;
			break;
		}
	}
	k_mutex_unlock(&lock);
	/* reschedule 而不是 schedule：连续开锁只写一次 flash */
	k_work_reschedule(&counter_work, COUNTER_FLUSH_DELAY);
}

int nvstore_flush(void)
{
	/* 取消延迟工作并立刻写。进 System OFF 前必须调，
	 * 否则那 5 秒窗口里的 counter 更新会丢。 */
	(void)k_work_cancel_delayable(&counter_work);
	counter_work_fn(NULL);
	return 0;
}

/* --- q --------------------------------------------------------------------- */

uint32_t nvstore_next_q(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	uint32_t v = q_next++;

	/* 用完预留区间就再抬一段。正常情况下 5000 条才发生一次。 */
	if (q_next >= p.q_ceiling) {
		p.q_ceiling = q_next + NVSTORE_Q_BATCH;
		int rc = settings_save_one(KEY_Q, &p.q_ceiling,
					   sizeof(p.q_ceiling));
		if (rc != 0) {
			/* 写失败了还是要往下走：q 单调性在本次开机内仍成立，
			 * 只有掉电重启后可能回退。宁可继续上报也不要卡死。 */
			LOG_ERR("q 上界落盘失败 rc=%d", rc);
		}
	}
	k_mutex_unlock(&lock);
	return v;
}

uint32_t nvstore_boot_count(void)
{
	return p.boot_count;
}

/* --- 密钥 ------------------------------------------------------------------- */

int nvstore_save_users(const struct user_key *users, size_t n, uint16_t kid)
{
	if (n != MAX_USERS) {
		return -EINVAL;
	}
	k_mutex_lock(&lock, K_FOREVER);
	memcpy(user_mirror, users, sizeof(user_mirror));
	p.kid = kid;
	int rc = settings_save_one(KEY_USERS, user_mirror, sizeof(user_mirror));
	if (rc == 0) {
		counter_dirty = false;
		rc = settings_save_one(KEY_KID, &p.kid, sizeof(p.kid));
	}
	k_mutex_unlock(&lock);
	return rc;
}

int nvstore_load_users(struct user_key *users, size_t n, uint16_t *kid)
{
	if (n != MAX_USERS) {
		return -EINVAL;
	}
	if (!mirror_valid) {
		return -ENOENT;
	}
	memcpy(users, user_mirror, sizeof(user_mirror));
	*kid = p.kid;
	return 0;
}

/* --- 配置 ------------------------------------------------------------------- */

uint32_t nvstore_report_interval(void)
{
	return p.report_interval;
}

int nvstore_set_report_interval(uint32_t seconds)
{
	/* 下界 60 秒：更短的周期在省电档下毫无意义（模组开机就要几秒），
	 * 而且会把车电池当成设备电池用。上界 24 小时。 */
	if (seconds < 60 || seconds > 86400) {
		return -EINVAL;
	}
	p.report_interval = seconds;
	return settings_save_one(KEY_INTERVAL, &p.report_interval,
				 sizeof(p.report_interval));
}

/* --- 初始化 ----------------------------------------------------------------- */

int nvstore_init(void)
{
	k_mutex_init(&lock);
	k_work_init_delayable(&counter_work, counter_work_fn);

	p.report_interval = CONFIG_EBIKE_REPORT_INTERVAL;

	int rc = settings_subsys_init();
	if (rc != 0) {
		LOG_ERR("settings_subsys_init 失败 rc=%d", rc);
		return rc;
	}
	rc = settings_load();
	if (rc != 0) {
		LOG_ERR("settings_load 失败 rc=%d", rc);
		return rc;
	}

	/* q 从上次预留的上界开始 —— 跳过的那一段永远不会被复用，
	 * 保证掉电后 q 仍然单调（契约 §5 的去重依赖它）。 */
	q_next = p.q_ceiling;
	p.q_ceiling = q_next + NVSTORE_Q_BATCH;
	(void)settings_save_one(KEY_Q, &p.q_ceiling, sizeof(p.q_ceiling));

	p.boot_count++;
	(void)settings_save_one(KEY_BOOT, &p.boot_count, sizeof(p.boot_count));

	LOG_INF("nvstore：q 从 %u 起，第 %u 次启动，上报周期 %u s",
		q_next, p.boot_count, p.report_interval);
	return 0;
}
