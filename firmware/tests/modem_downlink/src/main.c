/*
 * 下行 URC 解析 + AT 应答匹配的运行时测试。
 *
 * 被测的是 firmware/nrf52840/src/modem.c 的原件。`read_line()` 和
 * `at_cmd_expect()` 都是 static，所以这里**不直接调它们**，而是从公开面
 * 走进去：把字节塞进 uart-emul 的 RX FIFO，让 `modem_poll()` 读到、
 * `consume_urc()` → `handle_msub()` 走完，用 dn_cb 收结果。
 * 这条路径和真设备上完全一致。
 *
 * 两个被钉住的缺陷（2026-09-03 审计）：
 *
 *   M1  超长行**静默截断**。截断后的 HEX 仍是合法偶数长度，`hex_decode`
 *       成功，`{"c":"interval","a":{"s":900}}` 截成 `..."s":90}` 也能被
 *       正常解析 —— 上报周期静默改错 10 倍。解码层看不出来。
 *   M2  `at_cmd()`（expect="OK"）被迟到的 `SEND OK` / `CONNECT OK` 替答。
 *       `AT+CPOWD=1` 没执行却返回成功 → 模组不关机，持续耗电。
 *
 * ⚠ 这两条都**编得过、跑得起来**，只是静默做错事 —— 和 FIRMWARE.md §3c
 * 里最难抓的两个 bug 同型，所以值得一个运行时测试而不是代码审查。
 */

#include "modem.h"
#include "proto.h"

#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

static const struct device *const euart = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* --- dn_cb 收到的东西 ------------------------------------------------------- */

#define CB_MAX 8

static struct {
	char topic[288];
	uint8_t payload[PROTO_MAX_DN_PAYLOAD];
	size_t len;
} cb_log[CB_MAX];
static size_t cb_count;

/* 审计 R1 的观测点。
 *
 * `cb_forbidden` 由测试在「一条 AT 命令正在飞」的区间置位 —— 回调在那期间
 * 被调到就是重入（旧代码的行为：`handle_msub` 就地调 `dn_cb`，而
 * `consume_urc` 的两个调用点都持 `at_lock`）。
 *
 * `cb_depth` / `cb_max_depth` 数嵌套层数：投递期间若又有下行到达并被就地
 * 投递，深度会 > 1，而那正是爆栈的机制。 */
static bool cb_forbidden;
static int cb_violations;
static int cb_depth;
static int cb_max_depth;
/* 回调里要不要顺手再 poll 一次（模拟 ack_downlink → modem_publish →
 * consume_urc 那条真实的回环路径）。 */
static bool cb_repoll;

static void on_downlink(const char *topic, const uint8_t *payload, size_t len)
{
	if (cb_forbidden) {
		cb_violations++;
	}
	cb_depth++;
	if (cb_depth > cb_max_depth) {
		cb_max_depth = cb_depth;
	}

	if (cb_count < CB_MAX) {
		strncpy(cb_log[cb_count].topic, topic, sizeof(cb_log[0].topic) - 1);
		cb_log[cb_count].topic[sizeof(cb_log[0].topic) - 1] = '\0';
		size_t n = len < sizeof(cb_log[0].payload)
				   ? len : sizeof(cb_log[0].payload);
		memcpy(cb_log[cb_count].payload, payload, n);
		cb_log[cb_count].len = n;
		cb_count++;
	}

	if (cb_repoll) {
		/* 真固件在这里发 ack，那条路径会走到 modem_publish → consume_urc。
		 * 这里用 modem_poll 代替：同样是「回调里再读一轮 URC」，
		 * 而且不需要一个连上的会话。 */
		(void)modem_poll(20);
	}

	cb_depth--;
}

static void cb_reset(void)
{
	cb_count = 0;
	cb_violations = 0;
	cb_depth = 0;
	cb_max_depth = 0;
	cb_forbidden = false;
	cb_repoll = false;
	memset(cb_log, 0, sizeof(cb_log));
}

/* --- 喂字节 ---------------------------------------------------------------- */

/* 把一行（自动补 CRLF）塞进 emul 的 RX FIFO 并让 modem.c 消化。
 *
 * uart-emul 的中断回调走它自己的工作队列，不是同步调的 —— 先给它
 * 50 ms 把字节推给 modem.c 的 ISR，再 poll。与 modem_time 那份同理。 */
static void feed_raw(const char *s, size_t len)
{
	uint32_t put = uart_emul_put_rx_data(euart, (const uint8_t *)s, len);

	zassert_equal(put, (uint32_t)len,
		      "uart-emul 只吃进了 %u/%zu 字节 —— FIFO 太小，"
		      "测的就不是 read_line 了", put, len);
	k_sleep(K_MSEC(50));
}

static void feed_line(const char *line)
{
	static char buf[2048];
	int n = snprintf(buf, sizeof(buf), "%s\r\n", line);

	zassert_true(n > 0 && n < (int)sizeof(buf), "测试自己的缓冲不够");
	feed_raw(buf, (size_t)n);
	(void)modem_poll(150);
}

/* 拼一条 +MSUB URC 到 out：topic + HEX(payload)。不喂、不 poll。 */
static void build_msub(char *out, size_t out_len, const char *topic,
		       const char *json)
{
	size_t pos = (size_t)snprintf(out, out_len, "+MSUB: \"%s\",%zu,\"",
				      topic, strlen(json));

	for (size_t i = 0; json[i] != '\0'; i++) {
		pos += (size_t)snprintf(out + pos, out_len - pos, "%02X",
					(uint8_t)json[i]);
	}
	(void)snprintf(out + pos, out_len - pos, "\"\r\n");
}

/* 塞进 FIFO 但**不 poll** —— 让它像真实的迟到 URC 一样留在流里，
 * 等下一条 AT 命令去读到它。 */
static void feed_msub_raw(const char *topic, const char *json)
{
	static char line[4096];

	build_msub(line, sizeof(line), topic, json);
	feed_raw(line, strlen(line));
}

static void feed_msub(const char *topic, const char *json)
{
	feed_msub_raw(topic, json);
	(void)modem_poll(150);
}

static void *setup(void)
{
	zassert_true(device_is_ready(euart), "uart-emul 没就绪");
	zassert_ok(modem_init(on_downlink), "modem_init 失败");
	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	cb_reset();
	/* 把上一条用例可能留下的字节读干，避免串味 */
	(void)modem_poll(50);
	cb_reset();
}

ZTEST_SUITE(modem_downlink, NULL, setup, before, NULL, NULL);

/* --- 正常路径：先证明链路真的通 --------------------------------------------- */

/* 前 N 条断言全是拒绝路径时，「一律不投递」也会全绿。这条防那个。 */
ZTEST(modem_downlink, test_normal_downlink_reaches_callback)
{
	const char *json = "{\"id\":\"c-6\",\"c\":\"interval\",\"a\":{\"s\":900}}";

	feed_msub("ebike/v1/bike01/dn/cmd", json);

	zassert_equal(cb_count, 1, "下行没投递上来（收到 %zu 条）", cb_count);
	zassert_str_equal(cb_log[0].topic, "ebike/v1/bike01/dn/cmd",
			  "topic 解错了：%s", cb_log[0].topic);
	zassert_equal(cb_log[0].len, strlen(json), "payload 长度 %zu ≠ %zu",
		      cb_log[0].len, strlen(json));
	zassert_mem_equal(cb_log[0].payload, json, strlen(json),
			  "payload 内容不对");
}

/* 最长的真实下行（dn/secret 带 base64 密钥）必须能整条穿过来。
 * 这条同时钉住「LINE_MAX 装得下真实业务报文」—— 契约侧的
 * MAX_DOWNLINK_BYTES 是按这个算的。 */
ZTEST(modem_downlink, test_longest_real_downlink_fits)
{
	const char *json =
		"{\"id\":\"s-12345\",\"op\":\"set\",\"uid\":4294967295,"
		"\"kid\":65535,"
		"\"k\":\"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=\"}";

	feed_msub("ebike/v1/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/dn/secret", json);

	zassert_equal(cb_count, 1, "最长的真实下行没穿过来（%zu 条）", cb_count);
	zassert_equal(cb_log[0].len, strlen(json), "被截了：%zu ≠ %zu",
		      cb_log[0].len, strlen(json));
	zassert_mem_equal(cb_log[0].payload, json, strlen(json), "内容不对");
}

/* --- M1：超长行必须整条丢掉，不能截断后投递 --------------------------------- */

/* **这条是 M1 的核心。**
 *
 * 构造一条 payload 大到让整行超过 LINE_MAX 的 +MSUB。修复前 read_line
 * 会静默截掉尾部，`handle_msub` 拿到一个偶数长度的合法 HEX、解码成功，
 * 于是一条**被截断的 JSON** 被投递上去。
 *
 * 断言：回调**一次都不该被调用**。 */
ZTEST(modem_downlink, test_oversized_downlink_is_dropped_not_truncated)
{
	static char json[PROTO_MAX_DN_PAYLOAD * 2];
	size_t i = 0;

	/* 一个语法完整、但大到装不下的 JSON */
	i += (size_t)snprintf(json + i, sizeof(json) - i,
			      "{\"id\":\"c-9\",\"c\":\"interval\",\"a\":{\"s\":900,\"pad\":\"");
	while (i < sizeof(json) - 8) {
		json[i++] = 'A';
	}
	(void)snprintf(json + i, sizeof(json) - i, "\"}}");

	feed_msub("ebike/v1/bike01/dn/cmd", json);

	zassert_equal(cb_count, 0,
		      "超长下行被截断后投递了（%zu 字节）—— 截断的 JSON 会被"
		      "当成合法指令，`\"s\":900` 截成 `\"s\":90` 就是静默改错 10 倍",
		      cb_count ? cb_log[0].len : 0);
}

/* 超长行之后**流仍然同步**：紧跟的下一条正常下行必须照常投递。
 *
 * 这是「丢整条」能安全实现的前提。做不到的话一条大报文会让后续所有
 * 下行错位 —— 比截断更糟。 */
ZTEST(modem_downlink, test_stream_stays_synced_after_oversized_line)
{
	static char big[PROTO_MAX_DN_PAYLOAD * 2];
	size_t i = 0;
	const char *good = "{\"id\":\"c-7\",\"c\":\"ping\"}";

	i += (size_t)snprintf(big + i, sizeof(big) - i, "{\"pad\":\"");
	while (i < sizeof(big) - 6) {
		big[i++] = 'B';
	}
	(void)snprintf(big + i, sizeof(big) - i, "\"}");

	feed_msub("ebike/v1/bike01/dn/cmd", big);
	zassert_equal(cb_count, 0, "超长的那条不该投递");

	feed_msub("ebike/v1/bike01/dn/cmd", good);
	zassert_equal(cb_count, 1,
		      "超长行之后流失步了 —— 后续下行全废（收到 %zu 条）",
		      cb_count);
	zassert_mem_equal(cb_log[0].payload, good, strlen(good),
			  "同步恢复了但内容不对");
}

/* --- M2：迟到的带外 URC 不能替下一条命令答到 -------------------------------- */

/* **这条是 M2 的核心。**
 *
 * `modem_csq()` 走 `at_query("AT+CSQ", ...)`，expect 是 `"OK"`。
 * 先把一条滞留的 `SEND OK` 塞进 FIFO（模拟上一次 publish 超时放弃后
 * URC 迟到），再调 modem_csq()：
 *
 * - 修复前：`strstr("SEND OK", "OK")` 命中 → at_cmd_expect 返回 0，
 *   而 resp 里没有 `+CSQ:` → `strchr(resp, ':')` 为 NULL → 返回 -1。
 *   **命令实际没得到应答，却走了成功分支。**
 * - 修复后：整行必须是 "OK" 才算终结码，`SEND OK` 落不上，
 *   继续等真正的应答。
 *
 * 观测点：喂 `SEND OK` + 真正的 `+CSQ: 21,0` + `OK`，csq 必须是 21。
 * 修复前 `SEND OK` 会先让命令返回，21 那条读不到 → 得 -1。 */
ZTEST(modem_downlink, test_stale_send_ok_does_not_answer_next_command)
{
	/* 预先把迟到的 URC 塞进 FIFO —— 不 poll，让它留在 ring 里，
	 * 正如真实时序：publish 放弃后它才到。 */
	feed_raw("SEND OK\r\n", 9);
	feed_raw("+CSQ: 21,0\r\nOK\r\n", 16);

	int csq = modem_csq();

	zassert_equal(csq, 21,
		      "csq=%d —— 迟到的 `SEND OK` 替 AT+CSQ 答到了，"
		      "真正的应答被丢掉", csq);
}

/* 同型：`CONNECT OK` / `CONNACK OK` 也不能替答。
 * 它们在 modem_connect() 里是**显式** expect 的（子串匹配那条路径），
 * 所以不能靠「把它们加进 consume_urc」来解决 —— 只能靠「expect 恰好是
 * OK 时要求整行相等」。 */
ZTEST(modem_downlink, test_stale_connect_ok_does_not_answer_next_command)
{
	feed_raw("CONNECT OK\r\n", 12);
	feed_raw("CONNACK OK\r\n", 12);
	feed_raw("+CSQ: 18,0\r\nOK\r\n", 16);

	int csq = modem_csq();

	zassert_equal(csq, 18,
		      "csq=%d —— `CONNECT OK`/`CONNACK OK` 替 AT+CSQ 答到了", csq);
}

/* 反向护栏：真正独占一行的 `OK` 仍然要被当成终结码。
 * 上一条修法如果写成「永远不认 OK」，这条会红。 */
ZTEST(modem_downlink, test_plain_ok_still_terminates_a_command)
{
	feed_raw("+CSQ: 7,0\r\nOK\r\n", 15);

	int csq = modem_csq();

	zassert_equal(csq, 7, "正常的 OK 不被认了：csq=%d", csq);
}

/* --- 解析健壮性 ------------------------------------------------------------- */

/* 畸形 +MSUB 一律消化掉，不能投递半条。 */
ZTEST(modem_downlink, test_malformed_msub_not_delivered)
{
	static const char *const bad[] = {
		"+MSUB:",                              /* 空 */
		"+MSUB: garbage",                      /* 没有引号 */
		"+MSUB: \"topic\"",                    /* 只有 topic */
		"+MSUB: \"topic\",5,",                 /* 缺 payload 引号 */
		"+MSUB: \"topic\",5,\"ZZZZ\"",         /* 非 HEX 字符 */
		"+MSUB: \"topic\",3,\"ABC\"",          /* 奇数长度 HEX */
	};

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		cb_reset();
		feed_line(bad[i]);
		zassert_equal(cb_count, 0, "畸形 +MSUB 被投递了：%s", bad[i]);
	}
}

/* topic 超过 dn_topic 缓冲时丢整条，不能截断 topic 后投递 ——
 * 截断的 topic 会被 uplink 的 dn 路由匹配成别的东西。 */
ZTEST(modem_downlink, test_overlong_topic_dropped)
{
	static char line[2048];
	size_t pos = 0;

	pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "+MSUB: \"");
	/* dn_topic 是 288 字节，用 400 个字符顶穿它 */
	for (int i = 0; i < 400; i++) {
		line[pos++] = 'x';
	}
	(void)snprintf(line + pos, sizeof(line) - pos, "\",2,\"7B7D\"");

	feed_line(line);
	zassert_equal(cb_count, 0, "超长 topic 被截断后投递了");
}

/* --- R1：下行绝不在命令流里就地投递 ----------------------------------------- */

/* **这条是 R1 的核心。**
 *
 * 旧代码 `handle_msub` 就地调 `dn_cb`，而 `consume_urc` 有两个调用点在
 * 持 `at_lock` 的命令流里（`at_cmd_expect` 等应答的循环、`modem_publish`
 * 等裸 `>` 的循环）。于是「一条 AT 命令正在飞」的当口收到下行，会一路
 * 走到 `ack_downlink → modem_publish` **重入 `modem_publish` 自身** ——
 * static `hexbuf` 被内层覆盖（发出错误字节），且递归无界（第 5 层爆 4 KB 栈）。
 *
 * 观测手段：把一条 `+MSUB` 和 `AT+CSQ` 的应答一起塞进 FIFO，然后调
 * `modem_csq()`。命令执行期间 `cb_forbidden` 置位 —— 回调被调到就是重入。
 *
 * - 修复前：`modem_csq` 内部 `read_line` 读到 `+MSUB` → `consume_urc` →
 *   `handle_msub` 就地 `dn_cb` → `cb_violations` > 0。
 * - 修复后：`handle_msub` 只入队，`cb_violations == 0`；那条下行留到
 *   下一次 `modem_poll()` 才投递，且内容完整。
 *
 * 两条断言都要：只查 violations 的话，「永远不投递」也是绿的。 */
ZTEST(modem_downlink, test_downlink_not_delivered_from_inside_at_command)
{
	const char *json = "{\"id\":\"c-11\",\"c\":\"ping\"}";

	/* 下行先到，紧跟 AT+CSQ 的应答 —— 真实时序就是这样混在一条流上。 */
	feed_msub_raw("ebike/v1/bike01/dn/cmd", json);
	feed_raw("+CSQ: 24,0\r\nOK\r\n", 16);

	cb_forbidden = true;
	int csq = modem_csq();

	cb_forbidden = false;

	zassert_equal(csq, 24, "AT+CSQ 没拿到应答（csq=%d）—— "
				"下行 URC 把命令流搅乱了", csq);
	zassert_equal(cb_violations, 0,
		      "下行在 AT 命令执行期间被就地投递了（%d 次）—— "
		      "那条路径会重入 modem_publish：static hexbuf 被覆盖、"
		      "递归爆栈（审计 R1）", cb_violations);
	zassert_equal(cb_count, 0, "命令期间就投递了 %zu 条", cb_count);

	/* 关键的另一半：它不能被丢掉，只是被推迟。 */
	(void)modem_poll(150);
	zassert_equal(cb_count, 1,
		      "推迟之后下行丢了（收到 %zu 条）—— 排队不等于丢弃",
		      cb_count);
	zassert_mem_equal(cb_log[0].payload, json, strlen(json),
			  "推迟投递的内容不对");
	zassert_equal(cb_log[0].len, strlen(json), "长度不对");
}

/* 投递期间又来一条下行时，不能递归下去。
 *
 * 真固件的回环是 `dn_cb → handle_cmd → ack_downlink → modem_publish →
 * consume_urc`；这里用「回调里再 `modem_poll` 一次」代替，同样是
 * 「投递中再读一轮 URC」，而且不需要一个连上的会话。
 *
 * 修复前：第二条会在第一条的回调里被就地投递，深度 2 —— 那正是爆栈机制。
 * 修复后：`dn_delivering` 挡住，深度恒为 1，第二条留到外层的下一轮。 */
ZTEST(modem_downlink, test_delivery_does_not_nest)
{
	const char *a = "{\"id\":\"c-12\",\"c\":\"ping\"}";
	const char *b = "{\"id\":\"c-13\",\"c\":\"locate\"}";

	feed_msub_raw("ebike/v1/bike01/dn/cmd", a);
	feed_msub_raw("ebike/v1/bike01/dn/cmd", b);

	cb_repoll = true;
	(void)modem_poll(300);
	cb_repoll = false;

	zassert_equal(cb_max_depth, 1,
		      "回调嵌套到了 %d 层 —— 每层 880 字节，"
		      "uplink 线程栈只有 4096（审计 R1）", cb_max_depth);
	zassert_equal(cb_count, 2, "两条都该投递到（收到 %zu 条）", cb_count);
	zassert_mem_equal(cb_log[0].payload, a, strlen(a), "第一条内容不对");
	zassert_mem_equal(cb_log[1].payload, b, strlen(b),
			  "第二条内容不对 —— 队列顺序错了？"
			  "dn/secret 的连续轮换依赖这个顺序");
}

/* 队列满了要拒收新的、且**已排队的那些必须完整投递出去**。
 *
 * 反向护栏：如果 `deliver_downlinks` 在回调前就 `dn_count--`，队满时内层
 * 算出的入队位置正好是正在投递的那个槽位 —— 内容会被覆盖。
 * DN_QUEUE_SIZE 是 4，这里灌 6 条。 */
ZTEST(modem_downlink, test_queue_full_drops_newest_keeps_queued_intact)
{
	static char json[6][48];

	for (int i = 0; i < 6; i++) {
		(void)snprintf(json[i], sizeof(json[i]),
			       "{\"id\":\"c-%d\",\"c\":\"ping\"}", 20 + i);
		feed_msub_raw("ebike/v1/bike01/dn/cmd", json[i]);
	}

	cb_repoll = true;
	(void)modem_poll(400);
	cb_repoll = false;

	zassert_equal(cb_max_depth, 1, "队满时回调嵌套了 %d 层", cb_max_depth);
	zassert_true(cb_count >= 4,
		     "队列容量 4 但只投递了 %zu 条 —— 排队的被覆盖了", cb_count);

	/* 前 4 条必须是最早入队的 4 条，且内容逐字节完整。 */
	for (int i = 0; i < 4; i++) {
		zassert_mem_equal(cb_log[i].payload, json[i], strlen(json[i]),
				  "第 %d 条内容被覆盖了（队满时槽位复用）", i);
	}
}
