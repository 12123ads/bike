/* 上行汇聚。见 uplink.h：一轮上报是「开机→发→收→关机」的完整闭环。 */

#include "uplink.h"
#include "battery.h"
#include "crypto.h"
#include "gnss.h"
#include "lock.h"
#include "modem.h"
#include "nvstore.h"
#include "unlock.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(uplink, CONFIG_EBIKE_LOG_LEVEL);

/* --- 复位原因（契约 §5.1 的 rst 五值闭集） ---------------------------------
 *
 * `hwinfo_get_reset_cause()` 在 nRF52840 上读的是 `POWER->RESETREAS`，
 * 映射由 `drivers/hwinfo/hwinfo_nrf.c` 完成。两条必须知道的事实：
 *
 * 1. **上电复位（POR/BOR）什么位都不置，`cause == 0`。** nRF 驱动没有
 *    「reason==0 → RESET_POR」这条，flags 从 0 开始逐位 OR 而已。
 *    nRF52840 上 `RESET_POR` 只在 VBUS 位（USB 插入唤醒）时出现，语义是错配的，
 *    别拿它判 POR。所以 `por` 必须映射成 `cause == 0`。
 * 2. **RESETREAS 是累积寄存器**，只有写 1 清位或断电才归零。不清的话，
 *    有史以来按过一次 RESET 键就永远带着 `RESET_PIN`，`cause == 0`
 *    这个「por」判据从此永不成立。所以**开机读完立刻清**。
 *
 * ⚠ `RESET_LOW_POWER_WAKE` 在 nRF52840 上是三合一的：System OFF 的
 * GPIO DETECT(bit16)、LPCOMP(bit17)、NFC 场唤醒(bit19) 都映到它。
 * 本工程不用 NFCT（开锁改走 BLE），也不用 LPCOMP，所以这一位只可能来自
 * 运动唤醒的 GPIO DETECT，统一报 `off` 是准确的。
 */
static const char *reset_cause_str(void)
{
	/* **只读一次**：下面会清掉 RESETREAS，第二次读就是 0（= "por"）了。
	 * 一次开机里所有 hello 都该报同一个真实原因。 */
	static const char *cached;

	if (cached != NULL) {
		return cached;
	}
	cached = "por";

	uint32_t cause = 0;
	int rc = hwinfo_get_reset_cause(&cause);

	if (rc != 0) {
		/* -ENOSYS = 这个平台没有 hwinfo 实现（weak stub）。
		 * 编译期发现不了，只能在这里暴露出来。 */
		LOG_WRN("读不到复位原因 rc=%d —— 上报 por", rc);
		return cached;
	}

	/* 读完立刻清，否则下次开机读到历史位（见上面第 2 条）。
	 * 失败只记日志：清不掉不影响本次上报，只影响下次的准确性。 */
	if (hwinfo_clear_reset_cause() != 0) {
		LOG_WRN("清复位原因失败 —— 下次的 rst 可能带历史位");
	}

	/* 顺序要紧：同一次复位可能置多位，先判具体原因，最后才落到通用的。 */
	if (cause == 0) {
		cached = "por";               /* 上电/欠压，见上面第 1 条 */
	} else if (cause & RESET_PIN) {
		cached = "pin";
	} else if (cause & RESET_WATCHDOG) {
		cached = "wdt";
	} else if (cause & RESET_LOW_POWER_WAKE) {
		cached = "off";               /* System OFF 被 GPIO DETECT 唤醒 */
	} else if (cause & RESET_SOFTWARE) {
		cached = "soft";              /* sys_reboot() / NVIC_SystemReset() */
	} else {
		/* 剩下的（CPU lockup、调试事件）契约里没有对应取值。
		 * 报 por 而不是编一个新值 —— 发契约里没写的值是污染。 */
		LOG_INF("复位原因 0x%08x 不在契约的五值里，上报 por", cause);
		cached = "por";
	}

	LOG_INF("复位原因 0x%08x → rst=\"%s\"", cause, cached);
	return cached;
}

/* --- 欠压状态（DESIGN.md §6） ----------------------------------------------- */

static atomic_t batt_level;   /* 0~3，见 battery.h */

void uplink_note_battery(int mv, int level)
{
	int old = (int)atomic_set(&batt_level, level);

	if (level != old) {
		LOG_WRN("欠压等级 %d → %d（%d mV）：%s", old, level, mv,
			level == 0 ? "恢复正常" :
			level == 1 ? "降低上报频率" :
			level == 2 ? "停周期上报，只保留离线开锁" :
				     "准备进 System OFF");
	}
}

int uplink_batt_level(void)
{
	return (int)atomic_get(&batt_level);
}

bool uplink_should_report(void)
{
	/* 等级 2/3：停周期上报。手机 BLE 开锁完全离线（DESIGN.md §5.2），
	 * 不受影响；运动事件仍然会经 uplink_request_now() 强行上报一轮 ——
	 * 车在这个电量下被动了，比省那点电重要。 */
	return uplink_batt_level() < 2;
}

/* 待发事件队列。省电档下不为单条事件开模组，攒到下一轮一起发。
 * 8 个：一次骑行过程里的 motion/still/unlock 加起来通常不超过这个数，
 * 满了就丢最旧的（新事件比旧事件有用）。 */
#define EVENT_QUEUE_SIZE 8

struct queued_event {
	uint32_t t;
	uint32_t q;
	enum proto_event ev;
	char detail[48];
	bool used;
};

static struct queued_event ev_queue[EVENT_QUEUE_SIZE];
static struct k_mutex ev_lock;
static struct k_sem now_sem;

/* 报文缓冲。静态区：4 KB 放栈上会爆掉线程栈。 */
static char msg_buf[PROTO_MAX_PAYLOAD];

/* 发一条，掉线了就走一次重连阶梯再发一次。
 *
 * 判据是 `modem_is_connected()` 而不是 `rc`：publish 失败有两类原因，
 * 只有「会话没了」这一类重连才有意义。payload 太大（-E2BIG）、
 * 缓冲不够（-ENOMEM）重连一百次也还是失败，而每次重连最坏 3 分钟全功率。
 *
 * **只重试一次。** 重连本身已经是三级阶梯（modem.h），它失败意味着
 * 网络或模组真的不可用，继续试只是在抽车电池。
 *
 * ⚠ **不要在下行回调里用这个。** `ack_downlink()` 跑在 `modem_poll()` →
 * `consume_urc()` 的调用栈里，此刻 `at_lock` 已经被持有（k_mutex 可重入，
 * 所以 publish 本身没事），但在那里重连等于「一边遍历 URC 一边把会话拆掉」。
 * ack 发不出去是可以接受的：服务端会在下次上线时重发，而下行指令都是幂等的
 * （契约 §4.1）。
 */
static int publish_retry(const char *topic, const char *buf, size_t len)
{
	int rc = modem_publish(topic, (const uint8_t *)buf, len, 1);

	if (rc == 0 || modem_is_connected()) {
		return rc;
	}

	LOG_WRN("%s 发送失败 rc=%d 且会话已断，走重连阶梯", topic, rc);
	if (modem_reconnect() != 0) {
		return rc;   /* 保留原始失败码，重连失败本身已经打过日志 */
	}
	return modem_publish(topic, (const uint8_t *)buf, len, 1);
}

/* --- 下行处理 --------------------------------------------------------------- */

static void ack_downlink(const char *dn_id, bool ok, const char *err)
{
	int n = proto_enc_ack(msg_buf, sizeof(msg_buf), modem_utc(),
			      nvstore_next_q(), dn_id, ok, err);
	if (n < 0) {
		LOG_ERR("拼 ack 失败 %d", n);
		return;
	}
	/* ack 发不出去的后果：服务端会在下次上线时重发同一条下行（契约 §4.1）。
	 * 那是幂等的（set 同一把密钥、locate 再定一次位），所以只记日志。 */
	int rc = modem_publish(TOPIC_UP_ACK, (const uint8_t *)msg_buf,
			       (size_t)n, 1);
	if (rc != 0) {
		LOG_WRN("ack %s 发送失败 rc=%d，服务端会重发", dn_id, rc);
	}
}

static void handle_cmd(const uint8_t *payload, size_t len)
{
	struct proto_dn_cmd cmd;
	int rc = proto_dec_cmd((const char *)payload, len, &cmd);

	if (rc == -EINVAL) {
		/* 连 id 都没解出来 —— 没法 ack，只能丢 */
		LOG_ERR("下行指令无法解析，丢弃");
		return;
	}
	if (rc == -ENOTSUP) {
		LOG_WRN("未知指令，回 ack 失败");
		ack_downlink(cmd.id, false, "unknown");
		return;
	}

	switch (cmd.cmd) {
	case CMD_PING:
		ack_downlink(cmd.id, true, NULL);
		break;

	case CMD_LOCATE:
		ack_downlink(cmd.id, true, NULL);
		/* 已经在一轮上报里了，直接再请求一次 —— 本轮的 GNSS
		 * 可能是关着的（want_gnss=false 的遥测轮） */
		uplink_request_now();
		break;

	case CMD_UNLOCK:
		/* 契约 §6.1：远程开锁绕过 §5.2 的挑战应答，两边都要显式打开。 */
		if (!IS_ENABLED(CONFIG_EBIKE_ALLOW_REMOTE_UNLOCK)) {
			LOG_WRN("远程开锁被固件禁用（CONFIG_EBIKE_ALLOW_REMOTE_UNLOCK=n）");
			ack_downlink(cmd.id, false, "disabled");
			break;
		}
		if (lock_unlock() == 0) {
			ack_downlink(cmd.id, true, NULL);
			(void)uplink_queue_event(EV_UNLOCK_OK, "{\"uid\":0}");
		} else {
			ack_downlink(cmd.id, false, "hw");
		}
		break;

	case CMD_LOCK:
		ack_downlink(cmd.id, lock_lock() == 0, "hw");
		break;

	case CMD_INTERVAL:
		if (cmd.arg_int > 0 &&
		    nvstore_set_report_interval((uint32_t)cmd.arg_int) == 0) {
			LOG_INF("上报周期改为 %d s", cmd.arg_int);
			ack_downlink(cmd.id, true, NULL);
		} else {
			ack_downlink(cmd.id, false, "range");
		}
		break;

	case CMD_TIER:
		/* PRO / 关机双档切换还没实现（modem.c 末尾的第 5 条）。
		 * 明确 ack 失败，比假装成功好 —— 假装成功会让服务端以为
		 * 设备已经在 PRO 档，然后一直等它秒回。 */
		LOG_WRN("功耗档切换未实现（DESIGN.md §11 #20）");
		ack_downlink(cmd.id, false, "notimpl");
		break;

	case CMD_REBOOT:
		ack_downlink(cmd.id, true, NULL);
		LOG_WRN("收到重启指令");
		/* 先把 counter 冲刷到 flash，否则重启会丢掉延迟写窗口里的更新 */
		(void)nvstore_flush();
		k_sleep(K_MSEC(500));   /* 让 ack 走完 UART */
		sys_reboot(SYS_REBOOT_COLD);
		break;

	default:
		ack_downlink(cmd.id, false, "unknown");
		break;
	}
}

static void handle_secret(const uint8_t *payload, size_t len)
{
	struct proto_dn_secret s;
	int rc = proto_dec_secret((const char *)payload, len, &s);
	if (rc != 0) {
		LOG_ERR("密钥下发无法解析 rc=%d", rc);
		/* id 可能解出来了，尽量 ack —— 否则服务端会一直重发一条坏报文 */
		if (s.id[0] != '\0') {
			ack_downlink(s.id, false, "badfmt");
		}
		return;
	}

	if (strcmp(s.op, "set") == 0) {
		rc = unlock_set_secret(s.uid, s.key, s.kid);
	} else if (strcmp(s.op, "del") == 0) {
		rc = unlock_del_secret(s.uid);
	} else if (strcmp(s.op, "wipe") == 0) {
		rc = unlock_wipe_secrets();
	} else {
		rc = -ENOTSUP;
	}

	/* 抹掉栈上的密钥副本。不抹的话它会在栈里留到这个栈帧被覆盖为止。 */
	crypto_wipe(s.key, sizeof(s.key));

	ack_downlink(s.id, rc == 0, rc == 0 ? NULL : "store");
	if (rc == 0) {
		LOG_INF("密钥 %s 完成（uid=%u kid=%u）", s.op, s.uid, s.kid);
	} else {
		LOG_ERR("密钥 %s 失败 rc=%d", s.op, rc);
	}
}

static void on_downlink(const char *topic, const uint8_t *payload, size_t len)
{
	/* topic 的最后一段决定类型。用 strstr 而不是精确比较：
	 * 模组回的 topic 理论上就是我们订阅的那个，但多一层容错不花钱。 */
	if (strstr(topic, "/dn/cmd") != NULL) {
		handle_cmd(payload, len);
	} else if (strstr(topic, "/dn/secret") != NULL) {
		handle_secret(payload, len);
	} else {
		LOG_WRN("未知下行 topic：%s", topic);
	}
}

/* --- 事件队列 --------------------------------------------------------------- */

int uplink_queue_event(enum proto_event ev, const char *detail_json)
{
	k_mutex_lock(&ev_lock, K_FOREVER);

	struct queued_event *slot = NULL;
	for (size_t i = 0; i < EVENT_QUEUE_SIZE; i++) {
		if (!ev_queue[i].used) {
			slot = &ev_queue[i];
			break;
		}
	}
	if (slot == NULL) {
		/* 满了：覆盖最旧的。新事件比旧事件有用 ——
		 * 「刚刚被撬」比「一小时前动过」重要。 */
		slot = &ev_queue[0];
		for (size_t i = 1; i < EVENT_QUEUE_SIZE; i++) {
			if (ev_queue[i].q < slot->q) {
				slot = &ev_queue[i];
			}
		}
		LOG_WRN("事件队列满，丢掉最旧的一条");
	}

	slot->t = modem_utc();
	slot->q = nvstore_next_q();
	slot->ev = ev;
	slot->detail[0] = '\0';
	if (detail_json != NULL) {
		strncpy(slot->detail, detail_json, sizeof(slot->detail) - 1);
		slot->detail[sizeof(slot->detail) - 1] = '\0';
	}
	slot->used = true;

	k_mutex_unlock(&ev_lock);

	/* 这两类事件不等下一轮 —— 用户需要立刻知道 */
	if (ev == EV_UNLOCK_DENY || ev == EV_LOWBATT) {
		uplink_request_now();
	}
	return 0;
}

void uplink_request_now(void)
{
	k_sem_give(&now_sem);
}

static void flush_events(void)
{
	/* 锁内只做快照和清标记，**发送在锁外**（审计 M3）：
	 * publish_retry 含最坏 3 分钟的重连阶梯（modem.h），持锁发送会把
	 * 也调 uplink_queue_event 的传感器驱动线程、系统工作队列
	 * （包括锁释放脉冲 release_work！）和 unlock_workq 全部卡住 ——
	 * 恰在「车被撬、事件密集、网络又差」的时刻。
	 *
	 * 发送失败的槽位重新标 used，留到下一轮（q 已分配，服务端按
	 * (dev,q) 去重，重发安全 —— 契约 §5）。 */
	static struct queued_event snap[EVENT_QUEUE_SIZE];
	bool pending[EVENT_QUEUE_SIZE];

	k_mutex_lock(&ev_lock);
	for (size_t i = 0; i < EVENT_QUEUE_SIZE; i++) {
		pending[i] = ev_queue[i].used;
		if (pending[i]) {
			snap[i] = ev_queue[i];
		}
	}
	k_mutex_unlock(&ev_lock);

	for (size_t i = 0; i < EVENT_QUEUE_SIZE; i++) {
		if (!pending[i]) {
			continue;
		}
		int n = proto_enc_event(msg_buf, sizeof(msg_buf),
					snap[i].t, snap[i].q, snap[i].ev,
					snap[i].detail[0] ? snap[i].detail
							  : NULL);
		if (n < 0) {
			LOG_ERR("拼事件失败 %d，丢弃", n);
			k_mutex_lock(&ev_lock);
			if (ev_queue[i].used && ev_queue[i].q == snap[i].q) {
				ev_queue[i].used = false;
			}
			k_mutex_unlock(&ev_lock);
			continue;
		}
		if (publish_retry(TOPIC_UP_EVENT, msg_buf, (size_t)n) == 0) {
			k_mutex_lock(&ev_lock);
			/* 只清「还是这一条」的槽位：发送期间新事件可能
			 * 覆盖了被丢最旧的它（队列满时）。q 是唯一键。 */
			if (ev_queue[i].used && ev_queue[i].q == snap[i].q) {
				ev_queue[i].used = false;
			}
			k_mutex_unlock(&ev_lock);
		} else {
			LOG_WRN("事件发送失败，留到下一轮");
		}
	}
}

/* --- 一轮上报 --------------------------------------------------------------- */

int uplink_cycle(bool want_gnss)
{
	int rc = modem_connect();
	if (rc != 0) {
		LOG_ERR("模组连接失败 rc=%d，本轮放弃", rc);
		/* 关掉模组，别让它开着耗电等下一轮 */
		(void)modem_disconnect();
		return rc;
	}

	/* hello 必须是连上后的第一条（契约 §4）：服务端靠它里面的 kid
	 * 判断要不要补发密钥，也靠它触发下行队列冲刷。 */
	int n = proto_enc_hello(msg_buf, sizeof(msg_buf), modem_utc(),
				nvstore_next_q(), nvstore_boot_count(),
				reset_cause_str(), unlock_current_kid());
	if (n > 0) {
		/* hello 是本轮的第一条，也是最值得重试的一条：服务端靠它
		 * 触发下行队列冲刷（契约 §4.1），丢了这条整轮的下行都收不到。 */
		(void)publish_retry(TOPIC_UP_HELLO, msg_buf, (size_t)n);
	}

	/* 位置。GNSS 拿不到就退回基站定位（契约 §5.2 的 s="l"，DESIGN.md §9.5）。 */
	if (want_gnss) {
		struct gnss_fix fix;
		struct proto_loc loc = {
			.t = modem_utc(),
			.q = nvstore_next_q(),
			.speed = -1.0f,
			.heading = -1,
			.sats = -1,
		};
		bool have = false;

		if (gnss_fix(&fix, 90) == 0) {
			loc.src = 'g';
			loc.lat = fix.lat;
			loc.lon = fix.lon;
			loc.acc = fix.acc_m;
			loc.speed = fix.speed_ms;
			loc.heading = fix.heading;
			loc.sats = (int8_t)fix.sats;
			have = true;
		} else {
			double lat, lon;
			float acc;
			if (modem_lbs(&lat, &lon, &acc) == 0) {
				loc.src = 'l';
				loc.lat = lat;
				loc.lon = lon;
				loc.acc = acc;
				have = true;
				LOG_INF("GNSS 无解，用基站定位降级");
			}
		}

		if (have) {
			n = proto_enc_loc(msg_buf, sizeof(msg_buf), &loc);
			if (n > 0) {
				(void)publish_retry(TOPIC_UP_LOC, msg_buf,
						    (size_t)n);
			}
		} else {
			LOG_WRN("GNSS 和基站定位都没结果，本轮不报位置");
		}
	}

	/* 遥测 */
	int mv = battery_read_mv();
	struct proto_tele tele = {
		.t = modem_utc(),
		.q = nvstore_next_q(),
		.volt = mv > 0 ? (float)mv / 1000.0f : 0.0f,
		.csq = (int8_t)modem_csq(),
		.uptime = (uint32_t)(k_uptime_get() / 1000),
		/* ⚠ **不报芯片温度。** 以前这里硬编码 `.temp = 0`，那是假数据 ——
		 * 服务端会当真值落库，图上出现一条 0 °C 的直线，而没人知道它是假的。
		 *
		 * 真读也不值得：nRF52840 的内置传感器测的是**结温**不是环境温度
		 * （旁边 modem 一发射就自热几度），而且精度是 ±5 °C
		 * （TTEMP,ACC，外加 ±2.5 °C 的 25 °C 点偏移）—— 真实 25 °C 可能报 20
		 * 也可能报 30。它还要拉 HFXO（PS 要求晶振才能达到标称精度）。
		 * 一个 ±5 °C 的数字不值得为它付这些，也不值得占报文字节。
		 * 契约 §5.3 明确写了 tmp 可省，所以整个字段不发。
		 */
		.has_temp = false,
	};
	n = proto_enc_tele(msg_buf, sizeof(msg_buf), &tele);
	if (n > 0) {
		(void)publish_retry(TOPIC_UP_TELE, msg_buf, (size_t)n);
	}

	/* 欠压兜底（DESIGN.md §6）。四级里这里做前两级的**上报**，
	 * 第 2/3/4 级的**行为**由 uplink_低压档 那套在下面和主循环里执行。 */
	int lvl = battery_low_level(mv);
	if (lvl > 0) {
		char detail[48];
		(void)snprintf(detail, sizeof(detail), "{\"lv\":%d,\"v\":%.1f}",
			       lvl, (double)tele.volt);
		(void)uplink_queue_event(EV_LOWBATT, detail);
	}
	uplink_note_battery(mv, lvl);

	flush_events();

	/* 收下行。给 5 秒 —— 服务端在收到 hello 后会立刻冲刷队列（契约 §4.1），
	 * 5 秒够几条报文走完 9600 的链路。 */
	(void)modem_poll(5000);

	/* 主动发 lwt=0 覆盖遗嘱（契约 §4.2）：告诉服务端这次是优雅下线，
	 * 不是被剪线。
	 *
	 * ⚠ 这一条**刻意不用 publish_retry**：如果会话已经断了，broker 会自己
	 * 投递 LWT（lwt=1），那个结果是**正确的** —— 会话非正常结束确实不是
	 * 优雅下线。为了盖掉一个真实的遗嘱而花最坏 3 分钟全功率重连，
	 * 是拿电池换一个错误的状态位。 */
	n = proto_enc_lwt(msg_buf, sizeof(msg_buf), false);
	if (n > 0) {
		(void)modem_publish(TOPIC_LWT, (const uint8_t *)msg_buf,
				    (size_t)n, 1);
	}

	(void)modem_disconnect();
	return 0;
}

int uplink_init(void)
{
	k_mutex_init(&ev_lock);
	k_sem_init(&now_sem, 0, 1);
	memset(ev_queue, 0, sizeof(ev_queue));
	return modem_init(on_downlink);
}

/* 主循环等这个信号量，所以暴露给 main。 */
struct k_sem *uplink_now_sem(void)
{
	return &now_sem;
}
