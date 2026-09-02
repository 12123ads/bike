/* 契约 v1 的设备侧编解码。见 proto.h 的说明。 */

#include "proto.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/base64.h>

LOG_MODULE_REGISTER(proto, CONFIG_EBIKE_LOG_LEVEL);

static const char *const event_names[] = {
	[EV_BOOT]        = "boot",
	[EV_MOTION]      = "motion",
	[EV_STILL]       = "still",
	[EV_UNLOCK_OK]   = "unlock_ok",
	[EV_UNLOCK_DENY] = "unlock_deny",
	[EV_LOCK_STATE]  = "lock_state",
	[EV_LOWBATT]     = "lowbatt",
	[EV_NFC_ERR]     = "nfc_err",
};

const char *proto_event_name(enum proto_event ev)
{
	if ((size_t)ev >= ARRAY_SIZE(event_names) || event_names[ev] == NULL) {
		return "nfc_err";   /* 不该发生；退化成一个合法值而不是发出非法报文 */
	}
	return event_names[ev];
}

/* snprintf 返回的是「本来需要多少字节」，>= len 就是被截断了。
 * 截断的 JSON 服务端会丢掉（契约层解析失败），所以这里一律当错误。 */
static int finish(int written, size_t len)
{
	if (written < 0 || (size_t)written >= len) {
		LOG_ERR("报文缓冲不足：需要 %d，只有 %zu", written, len);
		return -ENOMEM;
	}
	return written;
}

int proto_enc_hello(char *buf, size_t len, uint32_t t, uint32_t q,
		    uint32_t boot, const char *rst, uint16_t kid)
{
	return finish(snprintf(buf, len,
		"{\"t\":%u,\"q\":%u,\"fw\":\"%s\",\"boot\":%u,\"rst\":\"%s\",\"kid\":%u}",
		t, q, CONFIG_EBIKE_FW_VERSION, boot, rst ? rst : "por", kid), len);
}

/* 一个位置点的字段部分（不含外层括号），方便批量复用。
 * 坐标 6 位小数：约 0.11 m，再多是噪声，而每多一位是 20 个点 × 2 字节
 * （HEX 模式翻倍，契约 §5）。 */
static int enc_loc_fields(char *buf, size_t len, const struct proto_loc *p)
{
	int n = snprintf(buf, len,
		"{\"t\":%u,\"q\":%u,\"s\":\"%c\",\"la\":%.6f,\"lo\":%.6f",
		p->t, p->q, p->src, p->lat, p->lon);
	if (n < 0 || (size_t)n >= len) {
		return -ENOMEM;
	}

	/* 可选字段：缺省的不发，省字节 */
	if (p->acc >= 0.0f) {
		int k = snprintf(buf + n, len - n, ",\"a\":%.1f", (double)p->acc);
		if (k < 0 || (size_t)(n + k) >= len) {
			return -ENOMEM;
		}
		n += k;
	}
	if (p->speed >= 0.0f) {
		int k = snprintf(buf + n, len - n, ",\"sp\":%.1f", (double)p->speed);
		if (k < 0 || (size_t)(n + k) >= len) {
			return -ENOMEM;
		}
		n += k;
	}
	if (p->heading >= 0) {
		int k = snprintf(buf + n, len - n, ",\"hd\":%d", p->heading);
		if (k < 0 || (size_t)(n + k) >= len) {
			return -ENOMEM;
		}
		n += k;
	}
	if (p->sats >= 0) {
		int k = snprintf(buf + n, len - n, ",\"n\":%d", p->sats);
		if (k < 0 || (size_t)(n + k) >= len) {
			return -ENOMEM;
		}
		n += k;
	}

	if ((size_t)(n + 2) > len) {
		return -ENOMEM;
	}
	buf[n++] = '}';
	buf[n] = '\0';
	return n;
}

int proto_enc_loc(char *buf, size_t len, const struct proto_loc *p)
{
	return enc_loc_fields(buf, len, p);
}

int proto_enc_loc_batch(char *buf, size_t len,
			const struct proto_loc *pts, size_t n)
{
	if (n == 0) {
		return -EINVAL;   /* 契约 §5.2：空数组服务端会拒 */
	}
	if (n > PROTO_MAX_BATCH) {
		/* 不截断到 20：静默丢点会让「丢了」看起来像「没丢」 */
		LOG_ERR("批量 %zu 点超过契约上限 %d", n, PROTO_MAX_BATCH);
		return -E2BIG;
	}

	size_t off = 0;
	if (off + 1 >= len) {
		return -ENOMEM;
	}
	buf[off++] = '[';

	for (size_t i = 0; i < n; i++) {
		if (i > 0) {
			if (off + 1 >= len) {
				return -ENOMEM;
			}
			buf[off++] = ',';
		}
		int k = enc_loc_fields(buf + off, len - off, &pts[i]);
		if (k < 0) {
			return k;
		}
		off += k;
	}

	if (off + 2 > len) {
		return -ENOMEM;
	}
	buf[off++] = ']';
	buf[off] = '\0';
	return (int)off;
}

int proto_enc_tele(char *buf, size_t len, const struct proto_tele *t)
{
	int n = snprintf(buf, len, "{\"t\":%u,\"q\":%u,\"v\":%.1f,\"up\":%u",
			 t->t, t->q, (double)t->volt, t->uptime);
	if (n < 0 || (size_t)n >= len) {
		return -ENOMEM;
	}
	if (t->csq >= 0) {
		int k = snprintf(buf + n, len - n, ",\"csq\":%d", t->csq);
		if (k < 0 || (size_t)(n + k) >= len) {
			return -ENOMEM;
		}
		n += k;
	}
	int k = snprintf(buf + n, len - n, ",\"tmp\":%d}", t->temp);
	return finish(n + k, len);
}

int proto_enc_event(char *buf, size_t len, uint32_t t, uint32_t q,
		    enum proto_event ev, const char *detail_json)
{
	if (detail_json != NULL && detail_json[0] != '\0') {
		return finish(snprintf(buf, len,
			"{\"t\":%u,\"q\":%u,\"e\":\"%s\",\"d\":%s}",
			t, q, proto_event_name(ev), detail_json), len);
	}
	return finish(snprintf(buf, len, "{\"t\":%u,\"q\":%u,\"e\":\"%s\"}",
			       t, q, proto_event_name(ev)), len);
}

int proto_enc_ack(char *buf, size_t len, uint32_t t, uint32_t q,
		  const char *dn_id, bool ok, const char *err)
{
	if (!ok && err != NULL) {
		return finish(snprintf(buf, len,
			"{\"t\":%u,\"q\":%u,\"id\":\"%s\",\"ok\":0,\"er\":\"%s\"}",
			t, q, dn_id, err), len);
	}
	return finish(snprintf(buf, len,
		"{\"t\":%u,\"q\":%u,\"id\":\"%s\",\"ok\":%d}",
		t, q, dn_id, ok ? 1 : 0), len);
}

int proto_enc_lwt(char *buf, size_t len, bool ungraceful)
{
	return finish(snprintf(buf, len, "{\"lwt\":%d}", ungraceful ? 1 : 0), len);
}

/* --- 解析 ------------------------------------------------------------------
 * 手写的极简 JSON 取值。只支持「平坦对象 + 一层 `a` 子对象」，
 * 这正好是契约 §6 下行报文的形状。
 *
 * 刻意不写通用 JSON 解析器：通用解析器要处理转义、嵌套数组、unicode，
 * 那是几百行加一堆边界情况，而下行报文的生产者是我们自己的服务端，
 * 形状完全可控。**代价是这个解析器对非我方报文的健壮性有限** ——
 * 但下行 topic 有 ACL 保护（契约 §3），只有服务端能往那里发。
 */

/* 找 "key": 后面那个值的起始位置。找不到返回 NULL。 */
static const char *find_val(const char *json, size_t len, const char *key)
{
	char pat[24];
	int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
	if (n < 0 || (size_t)n >= sizeof(pat)) {
		return NULL;
	}
	/* json 不保证 NUL 结尾，所以不能用 strstr */
	size_t klen = (size_t)n;
	for (size_t i = 0; i + klen < len; i++) {
		if (memcmp(json + i, pat, klen) != 0) {
			continue;
		}
		const char *p = json + i + klen;
		const char *end = json + len;
		while (p < end && (*p == ' ' || *p == ':')) {
			p++;
		}
		return p < end ? p : NULL;
	}
	return NULL;
}

static int get_str(const char *json, size_t len, const char *key,
		   char *out, size_t out_len)
{
	const char *p = find_val(json, len, key);
	if (p == NULL || *p != '"') {
		return -ENOENT;
	}
	p++;
	const char *end = json + len;
	size_t i = 0;
	while (p < end && *p != '"') {
		if (i + 1 >= out_len) {
			return -ENOMEM;   /* 不截断：截断的 id 会导致 ack 销不了账 */
		}
		out[i++] = *p++;
	}
	if (p >= end) {
		return -EINVAL;       /* 引号没闭合 */
	}
	out[i] = '\0';
	return (int)i;
}

static int get_int(const char *json, size_t len, const char *key, int64_t *out)
{
	const char *p = find_val(json, len, key);
	if (p == NULL) {
		return -ENOENT;
	}
	char tmp[24];
	size_t i = 0;
	const char *end = json + len;
	if (p < end && (*p == '-' || *p == '+')) {
		tmp[i++] = *p++;
	}
	while (p < end && *p >= '0' && *p <= '9' && i + 1 < sizeof(tmp)) {
		tmp[i++] = *p++;
	}
	if (i == 0) {
		return -EINVAL;
	}
	tmp[i] = '\0';
	*out = strtoll(tmp, NULL, 10);
	return 0;
}

int proto_dec_cmd(const char *json, size_t len, struct proto_dn_cmd *out)
{
	memset(out, 0, sizeof(*out));
	out->arg_int = -1;

	if (get_str(json, len, "id", out->id, sizeof(out->id)) < 0) {
		LOG_WRN("下行缺 id，无法 ack，丢弃");
		return -EINVAL;
	}

	char cmd[16];
	if (get_str(json, len, "c", cmd, sizeof(cmd)) < 0) {
		return -EINVAL;
	}

	/* 契约 §6.1 的闭集。未知指令要能 ack 成失败，所以保留 id 再返回。 */
	static const struct {
		const char *name;
		enum proto_cmd val;
	} table[] = {
		{ "ping",     CMD_PING },
		{ "locate",   CMD_LOCATE },
		{ "unlock",   CMD_UNLOCK },
		{ "lock",     CMD_LOCK },
		{ "interval", CMD_INTERVAL },
		{ "tier",     CMD_TIER },
		{ "reboot",   CMD_REBOOT },
	};
	out->cmd = CMD_UNKNOWN;
	for (size_t i = 0; i < ARRAY_SIZE(table); i++) {
		if (strcmp(cmd, table[i].name) == 0) {
			out->cmd = table[i].val;
			break;
		}
	}

	/* 参数在 "a" 子对象里。直接在整个报文上找 "s"/"to"/"m" ——
	 * 顶层没有这几个键，所以不会误取。 */
	int64_t v;
	if (get_int(json, len, "s", &v) == 0) {
		out->arg_int = (int32_t)v;
	} else if (get_int(json, len, "to", &v) == 0) {
		out->arg_int = (int32_t)v;
	}
	(void)get_str(json, len, "m", out->arg_str, sizeof(out->arg_str));

	return out->cmd == CMD_UNKNOWN ? -ENOTSUP : 0;
}

int proto_dec_secret(const char *json, size_t len, struct proto_dn_secret *out)
{
	memset(out, 0, sizeof(*out));

	if (get_str(json, len, "id", out->id, sizeof(out->id)) < 0) {
		return -EINVAL;
	}
	if (get_str(json, len, "op", out->op, sizeof(out->op)) < 0) {
		return -EINVAL;
	}

	int64_t v;
	if (get_int(json, len, "uid", &v) == 0) {
		out->uid = (uint32_t)v;
	}
	if (get_int(json, len, "kid", &v) == 0) {
		out->kid = (uint16_t)v;
	}

	char b64[64];
	int n = get_str(json, len, "k", b64, sizeof(b64));
	if (n > 0) {
		size_t olen = 0;
		int rc = base64_decode(out->key, sizeof(out->key), &olen,
				       (const uint8_t *)b64, (size_t)n);
		/* 密钥解错了绝不能「先用着」—— 那会导致设备用一把错的密钥
		 * 覆盖掉当前有效的，把自己锁在外面。 */
		if (rc != 0 || olen != sizeof(out->key)) {
			LOG_ERR("密钥 base64 解码失败 rc=%d len=%zu", rc, olen);
			/* 抹掉可能的部分结果 */
			memset(out->key, 0, sizeof(out->key));
			return -EINVAL;
		}
		out->key_len = olen;
		out->has_key = true;
	}

	if (strcmp(out->op, "set") == 0 && !out->has_key) {
		return -EINVAL;
	}
	return 0;
}
