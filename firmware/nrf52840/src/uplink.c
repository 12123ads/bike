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

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(uplink, CONFIG_EBIKE_LOG_LEVEL);

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
	k_mutex_lock(&ev_lock, K_FOREVER);
	for (size_t i = 0; i < EVENT_QUEUE_SIZE; i++) {
		if (!ev_queue[i].used) {
			continue;
		}
		int n = proto_enc_event(msg_buf, sizeof(msg_buf),
					ev_queue[i].t, ev_queue[i].q,
					ev_queue[i].ev,
					ev_queue[i].detail[0] ? ev_queue[i].detail
							      : NULL);
		if (n < 0) {
			LOG_ERR("拼事件失败 %d，丢弃", n);
			ev_queue[i].used = false;
			continue;
		}
		if (modem_publish(TOPIC_UP_EVENT, (const uint8_t *)msg_buf,
				  (size_t)n, 1) == 0) {
			ev_queue[i].used = false;
		} else {
			/* 发不出去就留着下一轮再试。q 已经分配了，
			 * 服务端靠 (dev,q) 去重，重发是安全的（契约 §5）。 */
			LOG_WRN("事件发送失败，留到下一轮");
		}
	}
	k_mutex_unlock(&ev_lock);
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
				"por", unlock_current_kid());
	if (n > 0) {
		(void)modem_publish(TOPIC_UP_HELLO, (const uint8_t *)msg_buf,
				    (size_t)n, 1);
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
				(void)modem_publish(TOPIC_UP_LOC,
						    (const uint8_t *)msg_buf,
						    (size_t)n, 1);
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
		.temp = 0,
	};
	n = proto_enc_tele(msg_buf, sizeof(msg_buf), &tele);
	if (n > 0) {
		(void)modem_publish(TOPIC_UP_TELE, (const uint8_t *)msg_buf,
				    (size_t)n, 1);
	}

	/* 欠压检查（DESIGN.md §6 的软件兜底） */
	int lvl = battery_low_level(mv);
	if (lvl > 0) {
		char detail[48];
		(void)snprintf(detail, sizeof(detail), "{\"lv\":%d,\"v\":%.1f}",
			       lvl, (double)tele.volt);
		(void)uplink_queue_event(EV_LOWBATT, detail);
	}

	flush_events();

	/* 收下行。给 5 秒 —— 服务端在收到 hello 后会立刻冲刷队列（契约 §4.1），
	 * 5 秒够几条报文走完 9600 的链路。 */
	(void)modem_poll(5000);

	/* 主动发 lwt=0 覆盖遗嘱（契约 §4.2）：告诉服务端这次是优雅下线，
	 * 不是被剪线。 */
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
