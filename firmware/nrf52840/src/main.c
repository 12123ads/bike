/*
 * 主流程。
 *
 * 状态机就三态，刻意保持简单 —— 复杂度全在 modem.c 和 unlock.c 里：
 *
 *   ACTIVE   刚被运动唤醒或刚开机。NFC 开着，定时上报。
 *   IDLE     静止一段时间了。NFC 关掉（NFCT ACTIVATED 是 400 µA，§2.5），
 *            只留运动中断和定时器。
 *   （System OFF 目前**没有用**，见文件末尾的说明。）
 *
 * 唤醒源：LIS2DW12 INT1（运动）、定时器（周期上报）、NFC 场（只在 ACTIVE 有效）。
 */

#include "battery.h"
#include "crypto.h"
#include "gnss.h"
#include "lock.h"
#include "modem.h"
#include "motion.h"
#include "nfc_tag.h"
#include "nvstore.h"
#include "proto.h"
#include "unlock.h"
#include "uplink.h"

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

LOG_MODULE_REGISTER(main, CONFIG_EBIKE_LOG_LEVEL);

/* 静止多久之后关 NFC 进 IDLE。5 分钟：够用户走开又回来（忘了东西），
 * 也短到不会让 400 µA 白烧太久。 */
#define IDLE_AFTER_STILL_MS (5 * 60 * 1000)

/* 上报线程。栈要够大 —— modem.c 里有 4 KB 的 HEX 缓冲（那个是静态的），
 * 但 AT 命令拼接和 NMEA 解析都在栈上。 */
#define UPLINK_STACK_SIZE 4096
#define UPLINK_PRIORITY   5

K_THREAD_STACK_DEFINE(uplink_stack, UPLINK_STACK_SIZE);
static struct k_thread uplink_thread;

static volatile bool moving;
static int64_t last_still_ms;

/* --- 回调 ------------------------------------------------------------------- */

static void on_motion(enum motion_state state, uint16_t mg)
{
	if (state == MOTION_MOVING) {
		moving = true;
		char detail[32];
		(void)snprintf(detail, sizeof(detail), "{\"mg\":%u}", mg);
		(void)uplink_queue_event(EV_MOTION, detail);

		/* 动了就把 NFC 打开 —— 用户可能正要开锁 */
		(void)nfc_tag_start();

		/* 车被动了是防盗关心的事件，立刻上报（DESIGN.md §1「防盗感知」） */
		uplink_request_now();
	} else {
		moving = false;
		last_still_ms = k_uptime_get();
		(void)uplink_queue_event(EV_STILL, NULL);
	}
}

static void on_unlock_result(bool ok, uint32_t uid)
{
	char detail[32];
	if (ok) {
		(void)snprintf(detail, sizeof(detail), "{\"uid\":%u}", uid);
		(void)uplink_queue_event(EV_UNLOCK_OK, detail);
		/* 验证通过了才驱动锁。unlock.c 已经做完三重校验。 */
		if (lock_unlock() != 0) {
			LOG_ERR("MAC 验过了但锁没动 —— 检查驱动电路");
		}
	} else {
		/* 契约 §5.4：unlock_deny 不含失败原因 ——
		 * §5.2 说了不给攻击者区分信道，上行也不给。 */
		(void)snprintf(detail, sizeof(detail), "{\"uid\":%u}", uid);
		(void)uplink_queue_event(EV_UNLOCK_DENY, detail);
	}
}

static void on_lock_state(bool open)
{
	char detail[32];
	(void)snprintf(detail, sizeof(detail), "{\"locked\":%s}",
		       open ? "false" : "true");
	(void)uplink_queue_event(EV_LOCK_STATE, detail);
}

/* --- 上报线程 --------------------------------------------------------------- */

static void uplink_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* 开机先报一次，让服务端知道设备活了（也顺便拿到下行队列） */
	(void)uplink_queue_event(EV_BOOT, NULL);
	(void)uplink_cycle(true);

	while (true) {
		uint32_t interval = nvstore_report_interval();

		/* 等到周期到，或者被 uplink_request_now() 提前叫起来。
		 * 用信号量而不是 k_sleep：运动事件要能打断等待。 */
		if (k_sem_take(uplink_now_sem(), K_SECONDS(interval)) == 0) {
			LOG_INF("提前上报（被请求）");
		}

		/* 每轮都带 GNSS：不带的话轨迹就断了。
		 * 将来要省电可以让纯遥测轮跳过定位，但那要先有实测数据支撑
		 * （R8 阶段，DESIGN.md §10）。 */
		(void)uplink_cycle(true);

		/* 静止够久就关 NFC 省电。放在上报之后 ——
		 * 上报期间保持 NFC 开着，因为用户可能正好在车边。 */
		if (!moving && nfc_tag_is_active() &&
		    (k_uptime_get() - last_still_ms) > IDLE_AFTER_STILL_MS) {
			LOG_INF("静止超过 %d 分钟，关 NFC 省电（400 µA）",
				IDLE_AFTER_STILL_MS / 60000);
			(void)nfc_tag_stop();
		}

		/* counter 的延迟写窗口是 5 秒，这里主动冲一次 ——
		 * 下一轮之间可能被剪线掉电（没有备份电池，DESIGN.md §6）。 */
		(void)nvstore_flush();
	}
}

/* --- 初始化 ----------------------------------------------------------------- */

int main(void)
{
	LOG_INF("电瓶车定位固件 %s 启动（设备 %s）",
		CONFIG_EBIKE_FW_VERSION, CONFIG_EBIKE_DEVICE_ID);

	/* 顺序有讲究：
	 * crypto 在 unlock 之前（unlock_init 要读密钥并可能用到 PSA）；
	 * nvstore 在 unlock 之前（密钥从 flash 读）；
	 * nfc 在 unlock 之后（NFC 回调会调 unlock_handle_apdu）。 */
	int rc = crypto_init();
	if (rc != 0) {
		/* 没有密码学就没有开锁。这是唯一 fatal 的初始化失败 ——
		 * 其余部件坏了还能降级用，这个坏了核心功能就没了。 */
		LOG_ERR("crypto 初始化失败 rc=%d —— NFC 开锁不可用", rc);
		return rc;
	}

	rc = nvstore_init();
	if (rc != 0) {
		LOG_ERR("nvstore 初始化失败 rc=%d —— 密钥和 counter 无法持久化", rc);
		/* 继续跑：内存里的 counter 仍然能防本次开机内的重放，
		 * 掉电后会退化。比完全不能开锁好。 */
	}

	rc = unlock_init();
	if (rc != 0) {
		LOG_ERR("unlock 初始化失败 rc=%d", rc);
	}
	unlock_set_callback(on_unlock_result);

	rc = lock_init();
	if (rc != 0) {
		LOG_ERR("锁初始化失败 rc=%d —— 验证还能过但锁不会动", rc);
	}
	lock_set_callback(on_lock_state);

	rc = nfc_tag_init();
	if (rc != 0) {
		LOG_ERR("NFC 初始化失败 rc=%d —— 只能用机械钥匙", rc);
	} else {
		/* 开机就开着 NFC，静止 5 分钟后由上报线程关掉 */
		(void)nfc_tag_start();
		last_still_ms = k_uptime_get();
	}

	rc = motion_init(on_motion);
	if (rc != 0) {
		LOG_ERR("运动检测初始化失败 rc=%d —— 功耗会显著上升，"
			"且收不到「车被动了」的告警", rc);
	}

	rc = battery_init();
	if (rc != 0) {
		LOG_ERR("电池采样初始化失败 rc=%d —— 欠压兜底失效（§6）", rc);
	}

	rc = gnss_init();
	if (rc != 0) {
		LOG_ERR("GNSS 初始化失败 rc=%d —— 只能靠基站定位", rc);
	}

	rc = uplink_init();
	if (rc != 0) {
		LOG_ERR("uplink 初始化失败 rc=%d —— 上报不可用", rc);
	}

	k_thread_create(&uplink_thread, uplink_stack, UPLINK_STACK_SIZE,
			uplink_thread_fn, NULL, NULL, NULL,
			UPLINK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&uplink_thread, "uplink");

	LOG_INF("初始化完成");
	return 0;
}

/* --- 关于 System OFF ---------------------------------------------------------
 *
 * DESIGN.md §4.2 的那个 0.40 µA 是 **System OFF** 的数字，本实现**没有用到它**。
 * 当前是 System ON + RTC（3.16 µA）+ 定时器唤醒。
 *
 * 为什么先不做 System OFF：
 *
 * 1. System OFF 唤醒等于**复位**，整个初始化要重跑一遍，MQTT 也要重连。
 *    在省电档下（模组本来就关着）这没什么额外代价，但会让 uptime、
 *    RAM 里的状态、以及 modem 的会话全部丢掉。
 * 2. **真正的功耗地板是 4G 模组，不是主控**（DESIGN.md §4.1b）。
 *    3.16 µA 和 0.40 µA 的差别（2.8 µA）在 0.5~1.5 mA 的模组面前是噪声。
 *    只有在「模组完全关机」的档位上，这 2.8 µA 才占得上比例。
 * 3. 先把主路径调通再抠这 2.8 µA。R8 阶段（§10）才是做双档功耗的时候。
 *
 * 要做的话入口是：`pm_state_force(0, &(struct pm_state_info){PM_STATE_SOFT_OFF, 0, 0})`，
 * 唤醒源靠 GPIOTE PORT 事件（prj.conf 里已经配了
 * CONFIG_GPIO_NRFX_INTERRUPT_DETECT_MODE_PORT=y —— 那是 2.36 µA 而不是
 * IN 事件的 17.37 µA，§4.2 的陷阱）。进去之前必须调 nvstore_flush()。
 */
