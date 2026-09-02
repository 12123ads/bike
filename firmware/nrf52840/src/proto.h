/*
 * 契约 v1 的设备侧实现 —— 与 docs/MQTT-CONTRACT.md 和
 * server/ebike_server/contract.py 必须逐字一致。
 *
 * 不用 JSON 库（prj.conf 里 CONFIG_JSON_LIBRARY=n）：报文字段少且固定，
 * snprintf 拼出来的字节数可控，而 Zephyr 的 json 库要额外 descriptor 表，
 * 在这个规模上是纯负担。代价是拼错了编译器不会告诉你 —— 所以字段名全部
 * 走下面的宏，不在调用处写字面量。
 */

#ifndef EBIKE_PROTO_H
#define EBIKE_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* 契约 §4 */
#define PROTO_PREFIX      "ebike/v1"
#define PROTO_DEVICE_ID   CONFIG_EBIKE_DEVICE_ID

#define TOPIC_UP_HELLO    PROTO_PREFIX "/" PROTO_DEVICE_ID "/up/hello"
#define TOPIC_UP_LOC      PROTO_PREFIX "/" PROTO_DEVICE_ID "/up/loc"
#define TOPIC_UP_TELE     PROTO_PREFIX "/" PROTO_DEVICE_ID "/up/tele"
#define TOPIC_UP_EVENT    PROTO_PREFIX "/" PROTO_DEVICE_ID "/up/event"
#define TOPIC_UP_ACK      PROTO_PREFIX "/" PROTO_DEVICE_ID "/up/ack"
#define TOPIC_LWT         PROTO_PREFIX "/" PROTO_DEVICE_ID "/lwt"
#define TOPIC_DN_WILDCARD PROTO_PREFIX "/" PROTO_DEVICE_ID "/dn/#"
#define TOPIC_DN_CMD      PROTO_PREFIX "/" PROTO_DEVICE_ID "/dn/cmd"
#define TOPIC_DN_SECRET   PROTO_PREFIX "/" PROTO_DEVICE_ID "/dn/secret"

/* 契约 §1：Air780EP 单包上限。留 200 字节余量给 AT 命令头。 */
#define PROTO_MAX_PAYLOAD  3900
/* 契约 §5.2：批量位置点上限，服务端会硬拒超过的 */
#define PROTO_MAX_BATCH    20

/* 契约 §5.4 的事件闭集 */
enum proto_event {
	EV_BOOT,
	EV_MOTION,
	EV_STILL,
	EV_UNLOCK_OK,
	EV_UNLOCK_DENY,
	EV_LOCK_STATE,
	EV_LOWBATT,
	EV_BLE_ERR,
};

/* 契约 §6.1 的指令闭集 */
enum proto_cmd {
	CMD_UNKNOWN = 0,
	CMD_PING,
	CMD_LOCATE,
	CMD_UNLOCK,
	CMD_LOCK,
	CMD_INTERVAL,
	CMD_TIER,
	CMD_REBOOT,
};

/* 契约 §5.2 的一个位置点 */
struct proto_loc {
	uint32_t t;        /* 设备时钟，未同步时填 0（契约 §5.6） */
	uint32_t q;        /* 单调序号，掉电不清零 */
	char src;          /* 'g' = GNSS，'l' = 基站 */
	double lat;
	double lon;
	float acc;         /* 精度圈半径，米 */
	float speed;       /* m/s，负数表示缺省 */
	int16_t heading;   /* 度，负数表示缺省 */
	int8_t sats;       /* 负数表示缺省 */
};

struct proto_tele {
	uint32_t t;
	uint32_t q;
	float volt;        /* 电池电压 V */
	int8_t csq;        /* AT+CSQ 的 rssi，负数缺省 */
	uint32_t uptime;
	/* 芯片结温，摄氏度。**不能用负数当缺省标记** —— 冬天真的会是负的，
	 * 所以缺省靠下面的 has_temp。契约 §5.3 说 tmp 可省。 */
	int8_t temp;
	bool has_temp;
};

/* 解析出来的下行指令 */
struct proto_dn_cmd {
	char id[32];
	enum proto_cmd cmd;
	int32_t arg_int;   /* interval 的 s / locate 的 to；-1 = 无 */
	char arg_str[16];  /* tier 的 m */
};

/* 解析出来的下行密钥（契约 §6.2） */
struct proto_dn_secret {
	char id[32];
	char op[8];        /* set / del / wipe */
	uint32_t uid;
	uint16_t kid;
	uint8_t key[32];   /* base64 解出来的原始字节 */
	size_t key_len;
	bool has_key;
};

/* --- 编码（设备 → 服务端） --------------------------------------------------
 * 全部返回写入的字节数，缓冲不够返回负数。**不会截断** ——
 * 截断出来的 JSON 服务端会当畸形报文丢掉，而日志里只看到「发过了」。
 */
int proto_enc_hello(char *buf, size_t len, uint32_t t, uint32_t q,
		    uint32_t boot, const char *rst, uint16_t kid);

int proto_enc_loc(char *buf, size_t len, const struct proto_loc *p);

/* 批量。n > PROTO_MAX_BATCH 直接返回负数，不自作主张只发前 20 个。 */
int proto_enc_loc_batch(char *buf, size_t len,
			const struct proto_loc *pts, size_t n);

int proto_enc_tele(char *buf, size_t len, const struct proto_tele *t);

int proto_enc_event(char *buf, size_t len, uint32_t t, uint32_t q,
		    enum proto_event ev, const char *detail_json);

int proto_enc_ack(char *buf, size_t len, uint32_t t, uint32_t q,
		  const char *dn_id, bool ok, const char *err);

/* 契约 §4.2：主动下线前发 lwt=0 覆盖掉 broker 的遗嘱 */
int proto_enc_lwt(char *buf, size_t len, bool ungraceful);

/* --- 解析（服务端 → 设备） -------------------------------------------------- */
int proto_dec_cmd(const char *json, size_t len, struct proto_dn_cmd *out);
int proto_dec_secret(const char *json, size_t len, struct proto_dn_secret *out);

const char *proto_event_name(enum proto_event ev);

#endif /* EBIKE_PROTO_H */
