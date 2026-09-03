/* Air780EP AT 状态机。见 modem.h 的三个前提和文件末尾的「还缺什么」。 */

#include "modem.h"
#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/timeutil.h>

LOG_MODULE_REGISTER(modem, CONFIG_EBIKE_LOG_LEVEL);

static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(uart1));
static const struct gpio_dt_spec pwrkey =
	GPIO_DT_SPEC_GET(DT_NODELABEL(modem_pwrkey), gpios);

/* 9600 baud ≈ 1 字节/ms。一行 AT 应答通常 < 64 字节，
 * 但 MQTT 下行的 URC 会带整个 payload，所以行缓冲要够大。
 *
 * ⚠ **这个尺寸就是「设备能收多大的下行」**（审计 M1）。
 * `+MSUB: "<topic>",<len>,"<hex payload>"` 一整条在一行里，而 HEX 模式
 * 让 payload 字节数翻倍：
 *
 *   可收 payload = (LINE_MAX-1 - 框架) / 2，框架 = `+MSUB: ""` + topic
 *                                          + `,<len>,` + 两个引号 ≈ 39
 *   512 → 236 字节      640 → 300 字节
 *
 * 上一版 LINE_MAX=512 而 `dn_payload` 按 PROTO_MAX_PAYLOAD(3900) 开，
 * 两个数字差 16 倍：**声称能收 3900，实际 236 以上就收不到**。
 *
 * 而且失败是完全静默的（实测确认，不是推断）：read_line 截掉尾部 →
 * 那一刀必然砍掉 payload 的结束引号（HEX 区里只有 0-9A-F，不可能出现
 * 一个字面量 `"`）→ `handle_msub` 走「找不到结束引号」分支 →
 * `return true`（当作已消化）**且一条日志都不打**。
 * 症状是「下行凭空消失、服务端每次上线重发同一条」，
 * 而不是「收到一个截断的错值」。
 *
 * 现在两端都硬拒且都有日志：设备侧 read_line 返回 -EMSGSIZE 并 LOG_ERR，
 * 服务端侧 `contract.MAX_DOWNLINK_BYTES` = PROTO_MAX_DN_PAYLOAD，
 * 构造时就 400。两个数字由 test_firmware_contract.py 钉住一致。 */
#define LINE_MAX      640
#define RX_RING_SIZE  2048
/* AT 命令的默认超时。9600 下一来一回加模组处理，2 秒是宽裕的；
 * 网络相关的命令单独给更长的超时。 */
#define AT_TIMEOUT_MS 2000

static uint8_t rx_ring_buf[RX_RING_SIZE];
static struct ring_buf rx_ring;
static struct k_sem rx_sem;
static struct k_mutex at_lock;

static modem_dn_cb dn_cb;
/* MQTT 会话是否可用。**必须是 atomic**：掉线 URC 在 poll 线程里清它，
 * modem_publish / uplink 在上报线程里读它。原来是裸 `bool`，
 * 那是「一个线程写、另一个线程读」的非同步共享 —— 在 Cortex-M 上
 * 单字节读写不会撕裂，所以实际不会出错，但编译器有权把它缓存在寄存器里，
 * 于是「URC 说掉线了而循环里的 connected 还是 true」是合法优化结果。 */
static atomic_t connected;
/* 模组时间。两个字段必须成对读写 —— 拆开读会拿到「新 utc 配旧 uptime」，
 * 算出来的时间比真值差一整个上报周期。URC 在 poll 线程里写，
 * modem_utc() 在上报线程里读，所以要一把锁；用 spinlock 而不是 at_lock，
 * 因为 at_lock 会被一条 AT 命令占住几秒。 */
static struct k_spinlock time_lock;
static uint32_t nitz_utc;
static int64_t nitz_uptime;   /* 拿到时间时的 uptime，用来推算当前时间 */

/* 下行 URC 的解析缓冲。放静态区而不是栈上。
 * dn_payload 按**下行**上限开（PROTO_MAX_DN_PAYLOAD），不是上行的 3900 ——
 * 下行整条 URC 在一行里，LINE_MAX 才是天花板（审计 M1）。 */
static char dn_topic[288];
static uint8_t dn_payload[PROTO_MAX_DN_PAYLOAD];

/* --- UART 底层 -------------------------------------------------------------- */

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	if (!uart_irq_update(dev)) {
		return;
	}
	while (uart_irq_rx_ready(dev)) {
		uint8_t c;
		if (uart_fifo_read(dev, &c, 1) != 1) {
			break;
		}
		if (ring_buf_put(&rx_ring, &c, 1) != 1) {
			/* 环形缓冲满 = 我们处理不过来。丢字节会让当前这条应答
			 * 变成畸形，上层会超时重试 —— 比阻塞中断好。 */
			LOG_WRN("modem RX 缓冲满，丢字节");
		}
		k_sem_give(&rx_sem);
	}
}

static void uart_write_str(const char *s)
{
	for (const char *p = s; *p != '\0'; p++) {
		uart_poll_out(uart, (unsigned char)*p);
	}
}

/* 读一行（以 '\n' 结束）。返回长度；超时返回 -ETIMEDOUT，
 * **超长返回 -EMSGSIZE**（行已被读干到 '\n'，调用方可以继续读下一行）。
 *
 * ⚠ 裸 '>' 提示符**不带换行**（§8.5 的陷阱之一），所以单独处理：
 * 读到 '>' 就立刻返回，不等换行。
 *
 * ⚠ **超长必须显式报错，不能静默截断**（审计 M1）。截断的下一站是
 * `handle_msub` 的「找不到 payload 结束引号」分支，它 `return true` 且
 * **不打日志** —— 下行凭空消失，而服务端每次设备上线都重发同一条。
 * 报 -EMSGSIZE 让这件事有一条 LOG_ERR，且调用方能区分它和超时。 */
static int read_line(char *out, size_t out_len, uint32_t timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	size_t n = 0;
	bool overflow = false;

	while (k_uptime_get() < deadline) {
		uint8_t c;
		if (ring_buf_get(&rx_ring, &c, 1) != 1) {
			int64_t left = deadline - k_uptime_get();
			if (left <= 0) {
				break;
			}
			(void)k_sem_take(&rx_sem, K_MSEC(left > 50 ? 50 : left));
			continue;
		}

		if (c == '\r') {
			continue;
		}
		if (c == '\n') {
			if (n == 0 && !overflow) {
				continue;   /* 空行，AT 应答前后都有 */
			}
			out[n] = '\0';
			if (overflow) {
				LOG_ERR("行超过 %d 字节被丢弃（前 %zu 字节：%.32s…）"
					" —— 下行报文过大？", LINE_MAX, n, out);
				return -EMSGSIZE;
			}
			return (int)n;
		}
		if (n + 1 < out_len) {
			out[n++] = (char)c;
		} else {
			/* 继续读到 '\n' 保持行同步，但整行作废 */
			overflow = true;
		}

		/* 裸提示符：收到就返回，它后面没有换行 */
		if (n == 1 && !overflow && out[0] == '>') {
			out[1] = '\0';
			return 1;
		}
	}
	if (n > 0 && !overflow) {
		out[n] = '\0';
		return (int)n;
	}
	return overflow ? -EMSGSIZE : -ETIMEDOUT;
}

/* --- URC 处理 --------------------------------------------------------------- */

/* 十六进制字符串 → 字节。AT+MQTTMODE=1 之后 payload 是 HEX 的（§8.2）。 */
static int hex_decode(const char *hex, size_t hex_len, uint8_t *out,
		      size_t out_len)
{
	if (hex_len % 2 != 0) {
		return -EINVAL;
	}
	size_t n = hex_len / 2;
	if (n > out_len) {
		return -ENOMEM;
	}
	for (size_t i = 0; i < n; i++) {
		char b[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
		char *endp = NULL;
		long v = strtol(b, &endp, 16);
		if (endp != b + 2) {
			return -EINVAL;
		}
		out[i] = (uint8_t)v;
	}
	return (int)n;
}

/* MQTT 下行 URC 的形状（AT 手册 V1.6.8）：
 *   +MSUB: "<topic>",<len>,"<hex payload>"
 * 返回 true 表示这一行被当作下行消化掉了。 */
static bool handle_msub(const char *line)
{
	if (strncmp(line, "+MSUB:", 6) != 0) {
		return false;
	}

	const char *p = strchr(line, '"');
	if (p == NULL) {
		return true;   /* 是 +MSUB 但格式不认识，消化掉别往上传 */
	}
	p++;
	const char *q = strchr(p, '"');
	if (q == NULL) {
		return true;
	}
	size_t tlen = (size_t)(q - p);
	if (tlen >= sizeof(dn_topic)) {
		LOG_ERR("下行 topic 过长 %zu", tlen);
		return true;
	}
	memcpy(dn_topic, p, tlen);
	dn_topic[tlen] = '\0';

	/* payload 在第二对引号里 */
	p = strchr(q + 1, '"');
	if (p == NULL) {
		return true;
	}
	p++;
	q = strchr(p, '"');
	if (q == NULL) {
		return true;
	}

	int n = hex_decode(p, (size_t)(q - p), dn_payload, sizeof(dn_payload));
	if (n < 0) {
		LOG_ERR("下行 payload HEX 解码失败 %d", n);
		return true;
	}

	LOG_INF("下行 %s（%d 字节）", dn_topic, n);
	if (dn_cb) {
		dn_cb(dn_topic, dn_payload, (size_t)n);
	}
	return true;
}

/* 掉线类 URC。收到就把 connected 清掉，让上层去重连。 */
static bool handle_disconnect_urc(const char *line)
{
	static const char *const patterns[] = {
		"+PDP DEACT", "CLOSED", "TCP ERROR", "+MDISCONNECT",
		"CONNECT FAIL",
	};
	for (size_t i = 0; i < ARRAY_SIZE(patterns); i++) {
		if (strstr(line, patterns[i]) != NULL) {
			LOG_WRN("模组报掉线：%s", line);
			atomic_set(&connected, 0);
			return true;
		}
	}
	return false;
}

/* --- 模组时间 → Unix 秒 ------------------------------------------------------ */

/* 早于这个时刻的一概丢掉。模组没跟网络同步时会给出一个格式合法但年份很早的
 * 日期，而上报一个 2018 年的时间戳比上报 0 更糟 —— 契约 §5.6 的语义是
 * 「不知道就填 0」，不是「填一个看起来像时间的数」。
 *
 * 门槛取 2024-01-01 的理由：手册自己的例子用的是 2018 年
 * （`+CCLK: "18/08/07,13:28:31+32"`），这大概就是未同步时的量级；
 * 而厂商 2024-11 的实机抓图必须被接受 —— 它是本文件那两条换算规则的
 * 唯一实证来源，测试直接拿它当基准值。
 * ⚠ 门槛定得比实测数据还晚会把真数据判成脏数据。第一版写 2025-01-01，
 * 测试当场红了，就是这个错。 */
#define MIN_PLAUSIBLE_UTC 1704067200LL   /* 2024-01-01T00:00:00Z */

/* 解析模组的时间串 → Unix 秒。两个来源同一个格式：
 *   AT+CCLK?  → `+CCLK: "yy/MM/dd,hh:mm:ss±zz"`      ← 带引号
 *   URC       → `+NITZ:yy/MM/dd,hh:mm:ss±zz,<ds>`    ← 不带引号，尾部多个 ds
 *
 * 两个**不知道就一定算错**的点，都出自合宙 Cat.1 AT 命令手册 V1.6.8：
 *
 * 1. **±zz 的单位是 1/4 小时，不是小时。** 东八区是 `+32`，不是 `+08`。
 *    §3.12 原文：「时区(用当地时间和GMT 时间之间的差别来表示，
 *    以1/4 小时格式来表示；范围-47...+48)」。另有两处互证：厂商 NTP 教程
 *    「实际上时区范围（-12~12）……所以将整个时区范围扩展 4 倍」；
 *    `+NITZ` 的 `<ds>` 表写「+1 小时(等于 4 个 quarter in <tz>)」。
 *    当成小时读 → 差 24 小时。
 *
 * 2. **hh:mm:ss 是本地时间，偏移已经加进去了**，所以要**减**回去。
 *    证据是厂商 NTP 教程里两张 LLCOM 抓图：主机 Beijing 时钟
 *    `[2024/11/06 09:47:14]` 发 `AT+CCLK?`，模组答 `"24/11/06,09:47:13+32"`
 *    —— 两边一致；若模组给的是 UTC，应该答 01:47。
 *    ⚠ **这一条和 nRF91 相反**：NCS 的 `date_time_modem.c` 把它的 CCLK
 *    当 UTC，不做偏移。照抄那份代码到这块模组上就是 8 小时误差。
 *
 * `<ds>`（夏令时）刻意不看：按 `<ds>` 表的说明，夏令时调整已经折进 zz 了。
 */
static int parse_modem_time(const char *s, uint32_t *out)
{
	/* 跳到第一个数字。CCLK 带引号、NITZ 不带，跳过去就统一了。 */
	while (*s != '\0' && (*s < '0' || *s > '9')) {
		s++;
	}

	unsigned int yy, mm, dd, hh, mi, ss;
	int zz = 0;
	int n = sscanf(s, "%2u/%2u/%2u,%2u:%2u:%2u%3d",
		       &yy, &mm, &dd, &hh, &mi, &ss, &zz);
	/* 6 个也收：手册没写上电默认值，而 27.007 允许省掉末尾三个字符，
	 * 所以时区缺失按偏移 0 处理，而不是把整条丢掉。 */
	if (n != 6 && n != 7) {
		return -EINVAL;
	}

	/* timeutil_timegm64() **不做任何范围检查**（读过 lib/utils/timeutil.c：
	 * 就是一个多项式，没有校验、没有 errno），所以校验必须在这里做完。 */
	if (mm < 1 || mm > 12 || dd < 1 || dd > 31 ||
	    hh > 23 || mi > 59 || ss > 59 || zz < -48 || zz > 48) {
		return -EINVAL;
	}

	struct tm tm = {
		.tm_year = (int)yy + 2000 - 1900,   /* tm_year 从 1900 起算 */
		.tm_mon  = (int)mm - 1,             /* tm_mon 是 0~11 */
		.tm_mday = (int)dd,
		.tm_hour = (int)hh,
		.tm_min  = (int)mi,
		.tm_sec  = (int)ss,
	};
	/* tm_wday / tm_yday / tm_isdst 不用填：timeutil_timegm64() 只读上面六个。 */

	int64_t epoch = timeutil_timegm64(&tm) - (int64_t)zz * 900;
	if (epoch < MIN_PLAUSIBLE_UTC) {
		return -EINVAL;
	}

	*out = (uint32_t)epoch;
	return 0;
}

/* 解析成功就采纳；失败只 warn —— 时间拿不到不影响任何功能（契约 §5.6：t 填 0）。 */
static void note_modem_time(const char *src, const char *s)
{
	uint32_t utc = 0;
	if (parse_modem_time(s, &utc) != 0) {
		LOG_WRN("%s 的时间串没解出来（模组多半还没跟网络同步）：%s", src, s);
		return;
	}

	K_SPINLOCK(&time_lock) {
		nitz_utc = utc;
		nitz_uptime = k_uptime_get();
	}
	LOG_INF("%s 时间已采纳：Unix %u", src, utc);
}

static bool handle_time_urc(const char *line)
{
	/* ⚠ 这个模组家族**没有 `+CTZV`**（274 页手册全文 grep 零命中），
	 * 时间 URC 叫 `+NITZ:`。上一版认的 `+CTZV:` 和 `*PSUTTZ` 都是别家模组的，
	 * 在这里永远匹配不上 —— 也就是说「靠 URC 更新时间」这条路当时是断的。
	 *
	 * `AT+CTZR`（NITZ URC 上报）**缺省就是打开的**，而且手册明写
	 * 「该命令不支持设置，仅支持查询」，所以不需要也不能去配它。 */
	if (strncmp(line, "+NITZ:", 6) != 0) {
		return false;
	}
	note_modem_time("NITZ URC", line + 6);
	return true;
}

/* 消化一行不是命令应答的东西。返回 true = 已消化。 */
static bool consume_urc(const char *line)
{
	return handle_msub(line) || handle_disconnect_urc(line) ||
	       handle_time_urc(line);
}

/* --- AT 命令 ---------------------------------------------------------------- */

/* 发一条命令，等到 expect（通常 "OK"）或 "ERROR"。
 * 期间遇到的 URC 交给 consume_urc —— 这就是「URC 和应答混在一条流上」
 * 这个麻烦的处理点（§8.5）。 */
static int at_cmd_expect(const char *cmd, const char *expect,
			 uint32_t timeout_ms, char *resp, size_t resp_len)
{
	k_mutex_lock(&at_lock, K_FOREVER);

	if (cmd != NULL) {
		LOG_DBG("→ %s", cmd);
		uart_write_str(cmd);
		uart_write_str("\r\n");
	}

	char line[LINE_MAX];
	int64_t deadline = k_uptime_get() + timeout_ms;
	int ret = -ETIMEDOUT;

	while (k_uptime_get() < deadline) {
		int64_t left = deadline - k_uptime_get();
		int n = read_line(line, sizeof(line), (uint32_t)left);
		if (n == -EMSGSIZE) {
			/* 超长行已被丢弃但流是同步的 —— 继续等本命令的应答，
			 * 不要把它当成超时（审计 M1）。 */
			continue;
		}
		if (n < 0) {
			break;
		}
		LOG_DBG("← %s", line);

		if (consume_urc(line)) {
			continue;
		}
		/* 回显：模组可能开着 ATE1，把命令本身回显回来 */
		if (cmd != NULL && strcmp(line, cmd) == 0) {
			continue;
		}

		/* ⚠ **只有信息行才写 resp。**
		 *
		 * 原来这里无条件 strncpy 再判 expect，于是查询命令的数据行先被
		 * 存进 resp，紧随其后的终结码 "OK" **又覆盖一次** ——
		 * `AT+CGREG?` 的 `strstr(resp, ",1")` 和 `AT+CSQ` 的
		 * `strchr(resp, ':')` 因此恒不匹配：附着检测会在 60 秒后必然
		 * 返回 -ENETUNREACH（连不上网），csq 恒为 -1。
		 *
		 * 判 `strcmp(line,"OK")` 而不是 `strstr`：`CONNECT OK` /
		 * `CONNACK OK` 这些**是**要交给调用方的信息行（它们同时也是
		 * 某些命令的 expect），不能一起排除掉。 */
		bool is_final_ok = (strcmp(line, "OK") == 0);

		/* ⚠ **迟到的带外 URC 不能替本命令「答到」**（审计 M2）。
		 *
		 * `at_cmd()` 的 expect 是 `"OK"`，而 `SEND OK` / `CONNECT OK` /
		 * `CONNACK OK` 都是带外 URC（§8.5），子串匹配会命中它们。
		 * 时序：publish 等 `SEND OK` 超时放弃 → URC 迟到滞留 ring →
		 * 下一条 `AT+CPOWD=1` 读到它 → `strstr(...,"OK")` 命中 →
		 * **命令没执行却返回成功**（模组实际没关机，持续耗电）。
		 *
		 * 修法：expect 恰好是 `"OK"` 时要求**整行**是 "OK"（终结码本来
		 * 就是独占一行）；调用方显式等 `SEND OK` 之类时才用子串匹配。 */
		bool matched = (strcmp(expect, "OK") == 0)
			? is_final_ok
			: (strstr(line, expect) != NULL);
		bool errored = (!matched && strstr(line, "ERROR") != NULL);

		if (resp != NULL && resp_len > 0 && line[0] != '\0' && !is_final_ok) {
			strncpy(resp, line, resp_len - 1);
			resp[resp_len - 1] = '\0';
		}

		if (matched) {
			ret = 0;
			break;
		}
		if (errored) {
			LOG_WRN("命令失败：%s → %s", cmd ? cmd : "(等待)", line);
			ret = -EIO;
			break;
		}
	}

	k_mutex_unlock(&at_lock);
	return ret;
}

static int at_cmd(const char *cmd)
{
	return at_cmd_expect(cmd, "OK", AT_TIMEOUT_MS, NULL, 0);
}

static int at_query(const char *cmd, char *resp, size_t resp_len)
{
	return at_cmd_expect(cmd, "OK", AT_TIMEOUT_MS, resp, resp_len);
}

/* --- 开机 / 关机 ------------------------------------------------------------- */

static int power_on(void)
{
	/* PWRKEY 开集拉低 >1 s。给 1.5 s 余量。
	 * ⚠ 模组内部已有 5.6k 上拉，不要外加（§8.1）。 */
	int rc = gpio_pin_set_dt(&pwrkey, 1);   /* ACTIVE_LOW，1 = 拉低 */
	if (rc != 0) {
		return rc;
	}
	k_sleep(K_MSEC(1500));
	rc = gpio_pin_set_dt(&pwrkey, 0);
	if (rc != 0) {
		return rc;
	}

	/* 模组启动要几秒。期间连发 AT 训练波特率 ——
	 * 文档明说「发一个 AT 往往不够，要连发几个」（§8.3）。 */
	for (int i = 0; i < 30; i++) {
		k_sleep(K_MSEC(500));
		if (at_cmd_expect("AT", "OK", 500, NULL, 0) == 0) {
			LOG_INF("模组已响应（第 %d 次尝试）", i + 1);
			return 0;
		}
	}
	LOG_ERR("模组开机后 15 s 内无响应");
	return -ETIMEDOUT;
}

/* 硬关机：PWRKEY 拉低。用在两处 ——
 *   modem_disconnect() 的 AT+CPOWD=1 失败时；
 *   modem_reconnect() 的 3 级（模组卡死到 AT 都不应答，软关机没意义）。
 *
 * 手册对 PWRKEY 关机脉宽没给单独的数（只写了开机 >1 s），
 * 这里沿用开机的 1.5 s。返回值刻意不看：走到这里已经是兜底路径，
 * 连 GPIO 都失败的话也没有下一招。 */
static void power_off_hard(void)
{
	(void)gpio_pin_set_dt(&pwrkey, 1);   /* ACTIVE_LOW，1 = 拉低 */
	k_sleep(K_MSEC(1500));
	(void)gpio_pin_set_dt(&pwrkey, 0);
}

/* --- 公开接口 --------------------------------------------------------------- */

int modem_init(modem_dn_cb cb)
{
	if (!device_is_ready(uart)) {
		LOG_ERR("modem UART 未就绪");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&pwrkey)) {
		LOG_ERR("PWRKEY GPIO 未就绪");
		return -ENODEV;
	}

	dn_cb = cb;
	ring_buf_init(&rx_ring, sizeof(rx_ring_buf), rx_ring_buf);
	k_sem_init(&rx_sem, 0, 1);
	k_mutex_init(&at_lock);

	int rc = gpio_pin_configure_dt(&pwrkey, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) {
		return rc;
	}

	uart_irq_callback_user_data_set(uart, uart_isr, NULL);
	uart_irq_rx_enable(uart);

	LOG_INF("modem 就绪（未开机）");
	return 0;
}

/* --- 连接的三个阶段 ---------------------------------------------------------
 *
 * 拆成三段不是为了好看，是因为**重连时该从哪一段重来，取决于坏在哪**：
 *
 *   stage_boot     开机 + 串口/唤醒配置      —— 只有模组本身死了才需要重跑
 *   stage_attach   等 CGREG + 取时间          —— PDP/承载出问题才需要重跑
 *   stage_session  TLS + MQTT + 订阅          —— 单纯 MQTT 掉线只需要这一段
 *
 * 9600 baud 下这个区别是实打实的时间：stage_boot 里的开机等待最坏 15 s、
 * stage_attach 里的附着轮询最坏 60 s，而 stage_session 大约 5~10 s。
 * 每次掉线都从 stage_boot 重来，等于把一次 5 秒的恢复做成 80 秒。
 */

static int stage_boot(void)
{
	int rc = power_on();
	if (rc != 0) {
		return rc;
	}

	/* 关回显，省掉每条命令的回显往返 —— 9600 下这是实打实的时间 */
	(void)at_cmd("ATE0");

	/* 波特率固定成 9600 并存起来。已经是 9600 才能通到这里，
	 * 所以这一条是「确保下次开机也是 9600」（§8.1）。 */
	(void)at_cmd("AT+IPR=9600;&W");

	/* RI 唤醒 + 魔术串过滤。
	 * ⚠ AT^WAKEUPHEX 在 AT 固件 V1011 上能不能用**未核实**（§8.7 / §11 #17）。
	 * 失败了这里只 warn 不中止 —— 功能仍然可用，但功耗预算会崩，
	 * 所以日志要写清楚，R0 阶段就要验这一条。 */
	if (at_cmd("AT+CFGRI=1") != 0) {
		LOG_WRN("AT+CFGRI 失败 —— 模组无法主动唤醒主控");
	}
	if (at_cmd("AT^WAKEUPHEX=\"45424B\"") != 0) {
		LOG_ERR("AT^WAKEUPHEX 不可用 —— 每条 URC 都会唤醒主控，"
			"功耗预算会崩（DESIGN.md §8.7），且没有替代手段");
	}
	return 0;
}

static int stage_attach(void)
{
	/* 等网络附着。CREG/CGREG 到 1 或 5（本地/漫游注册）。
	 * 冷启动附着可能几十秒，所以单独给 60 秒。 */
	bool attached = false;
	for (int i = 0; i < 60; i++) {
		char resp[64] = { 0 };
		if (at_query("AT+CGREG?", resp, sizeof(resp)) == 0) {
			if (strstr(resp, ",1") != NULL || strstr(resp, ",5") != NULL) {
				attached = true;
				break;
			}
		}
		k_sleep(K_SECONDS(1));
	}
	if (!attached) {
		LOG_ERR("60 s 内没附着上网络");
		return -ENETUNREACH;
	}
	LOG_INF("网络已附着");

	/* 拿运营商时间（免费、无往返，§8.1）。契约 §5.6：拿不到就上报 t=0。
	 *
	 * ⚠ 上一版这里发 `AT+CTZR=1`，那是错的：手册写「该命令不支持设置，
	 * 仅支持查询。缺省为打开」—— 发过去只会换回一个 ERROR。
	 * `AT+CTZU`（NITZ 自动更新）缺省也是使能。两条都不用配，已删除；
	 * 9600 baud 下每条无用命令都是实打实的往返时间。
	 *
	 * 已经有时间了就跳过 —— 重连时没必要再花一个往返（NITZ URC 也会推）。 */
	if (modem_utc() == 0) {
		char clk[64] = { 0 };
		if (at_query("AT+CCLK?", clk, sizeof(clk)) == 0) {
			note_modem_time("AT+CCLK?", clk);
		} else {
			LOG_WRN("AT+CCLK? 失败 —— 上行的 t 会是 0（契约 §5.6）");
		}
	}
	return 0;
}

static int stage_session(void)
{
	/* TLS 配置。证书用 AT+FSWRITE 预先写进模组 FS（§8.2），
	 * 本函数假设已经写好 —— 灌证书是产线动作，不是每次连接都做。
	 * ⚠ 契约 §2 把设备侧从双向 TLS 降级成「TLS + 口令」，
	 * 所以这里只配 CA 和 hostname 校验，不配客户端证书。
	 *
	 * **这三条都必须检查返回值。** 前两条原来是 `(void)`：
	 * cacert 静默失败 → 没有可信根；seclevel 静默失败而模组默认 0
	 * → **TLS 退化成「加密但不认证」，中间人可以冒充服务端收走位置、
	 * 下发伪造的开锁指令**。宁可连不上也不要连上一个不认证的通道。 */
	int rc = at_cmd("AT+SSLCFG=\"cacert\",0,\"ca.crt\"");
	if (rc != 0) {
		LOG_ERR("配 CA 失败 —— ca.crt 是否已用 AT+FSWRITE 写进模组 FS？"
			"（DESIGN.md §8.8 的产线动作）");
		return rc;
	}
	/* seclevel 1 = 只校验服务端证书。2 才是双向。 */
	rc = at_cmd("AT+SSLCFG=\"seclevel\",0,1");
	if (rc != 0) {
		LOG_ERR("配 seclevel 失败 —— 不能在不校验服务端证书的情况下继续");
		return rc;
	}
	/* hostname 校验：证书 CN 必须等于 CONFIG_EBIKE_MQTT_HOST
	 * （= 服务端 `ebike-server init --hostname` 填的那个）。
	 * 这一条失败也是致命的：不校验 hostname 等于接受任何持有
	 * 「某个我们信任的 CA 签发的证书」的服务端。 */
	rc = at_cmd("AT+SSLCFG=\"hostname\",0,\"" CONFIG_EBIKE_MQTT_HOST "\"");
	if (rc != 0) {
		LOG_ERR("配 hostname 失败 —— 会导致不校验证书 CN，拒绝继续");
		return rc;
	}

	/* MQTT 参数 + LWT（契约 §4.2：payload 是 {"lwt":1}，retain）。
	 *
	 * ⚠ 参数顺序对着合宙 Air780EP AT 手册的 MCONFIG 语法（7 个参数位）：
	 *   AT+MCONFIG=<clientid>,<username>,<password>[,<will_qos>,
	 *               <will_retain>,"<will_topic>","<will_message>"]
	 * keepalive 不在 MCONFIG 里 —— 它属于下面的 AT+MCONNECT。
	 *
	 * 上一版多塞了一个 "60"，把整个后半段顶错位：will_topic 被配成
	 * "60"、topic 串被当成遗嘱内容 —— 遗嘱发不到契约的 lwt topic，
	 * 服务端永远收不到 lwt=1（2026-09-03 审计 H2）。 */
	char cmd[512];
	int n = snprintf(cmd, sizeof(cmd),
		"AT+MCONFIG=\"%s\",\"%s\",\"%s\",1,1,\"%s\",\"{\\\"lwt\\\":1}\"",
		PROTO_DEVICE_ID, PROTO_DEVICE_ID, CONFIG_EBIKE_MQTT_PASSWORD,
		TOPIC_LWT);
	if (n < 0 || (size_t)n >= sizeof(cmd)) {
		return -ENOMEM;
	}
	rc = at_cmd(cmd);
	if (rc != 0) {
		LOG_ERR("AT+MCONFIG 失败");
		return rc;
	}

	/* HEX 模式：二进制安全，代价是串口字节数翻倍（契约 §5 的算术前提）。
	 *
	 * **必须检查返回值。** 这是唯一一条「配置状态决定报文编码」的命令：
	 * modem_publish 无条件把 payload 转成 HEX，但 AT+MPUBEX 的长度参数填的是
	 * **原始字节数**。这条静默失败 → 模组按裸字节解释 → 长度对不上 →
	 * **每一条上行都畸形或被拒**，而日志只会显示「等 '>' 超时」，
	 * 排查方向会完全跑偏。同理 handle_msub 的 hex_decode 会对非 HEX 的
	 * 下行整片返回 -EINVAL。 */
	rc = at_cmd("AT+MQTTMODE=1");
	if (rc != 0) {
		LOG_ERR("AT+MQTTMODE=1 失败 —— HEX 编码没生效，"
			"所有上行都会畸形，拒绝继续");
		return rc;
	}

	/* 加密连接。明文是 AT+MIPSTART。 */
	n = snprintf(cmd, sizeof(cmd), "AT+SSLMIPSTART=\"%s\",%d",
		     CONFIG_EBIKE_MQTT_HOST, CONFIG_EBIKE_MQTT_PORT);
	if (n < 0 || (size_t)n >= sizeof(cmd)) {
		return -ENOMEM;
	}
	/* "CONNECT OK" 是带外 URC 而不是命令应答（§8.5 的陷阱），
	 * 所以这里 expect 的是它而不是 OK。TLS 握手在 9600 下要几秒。 */
	rc = at_cmd_expect(cmd, "CONNECT OK", 30000, NULL, 0);
	if (rc != 0) {
		LOG_ERR("TCP/TLS 连接失败 —— 检查证书 CN、CA 是否已写进模组 FS");
		return rc;
	}

	rc = at_cmd_expect("AT+MCONNECT=1,60", "CONNACK OK", 15000, NULL, 0);
	if (rc != 0) {
		LOG_ERR("MQTT CONNACK 失败 —— 检查用户名口令");
		return rc;
	}

	/* 订阅下行。契约 §4：设备只能订自己的 dn/#（ACL 也只允许这个）。 */
	n = snprintf(cmd, sizeof(cmd), "AT+MSUB=\"%s\",1", TOPIC_DN_WILDCARD);
	if (n < 0 || (size_t)n >= sizeof(cmd)) {
		return -ENOMEM;
	}
	rc = at_cmd_expect(cmd, "SUBACK", 10000, NULL, 0);
	if (rc != 0) {
		LOG_ERR("订阅下行失败 —— 收不到指令和密钥下发");
		return rc;
	}

	atomic_set(&connected, 1);
	LOG_INF("MQTT 已连接");
	return 0;
}

int modem_connect(void)
{
	int rc = stage_boot();
	if (rc != 0) {
		return rc;
	}
	rc = stage_attach();
	if (rc != 0) {
		return rc;
	}
	return stage_session();
}

/* --- 断线重连阶梯 ------------------------------------------------------------
 *
 * 三级，从最便宜的开始，只有失败才往下走。设计依据是「哪一层坏了」：
 *
 *   1 级 MQTT 会话     ~5~10 s   服务端重启、keepalive 超时、TLS 会话过期
 *   2 级 PDP 重拨      ~15~70 s  承载被运营商拆了（`+PDP DEACT`）
 *   3 级 模组重启      ~30~90 s  模组自己卡死，AT 都不应答
 *
 * 为什么不无脑走 3 级：9600 baud 下 3 级最坏 90 秒，而 1 级能解决的情况
 * （服务端重启、keepalive 超时）在实际运行里是最常见的那一类。
 * 把 5 秒的活干成 90 秒，代价不只是时间 —— 那 90 秒里模组是全功率的
 * （§4.1b 唤醒尾巴约 22 mA），直接吃车电池。
 *
 * 为什么每级之前都要先拆干净：AT 版 MQTT 的会话状态在模组里，
 * 不先 `AT+MDISCONNECT` / `AT+CIPSHUT` 就重连，模组会用「已经有连接」
 * 拒掉新的 `AT+SSLMIPSTART`。拆的命令一律 `(void)` —— 本来就可能因为
 * 「没连接可拆」而返回 ERROR，那不是失败。
 *
 * ⚠ 不做无限重试。三级都失败就返回错误，让上层（uplink_cycle）放弃本轮、
 * 关掉模组等下一个周期 —— 那才是省电的行为。**在这里死循环重试会把
 * 车电池抽干**，而设备没有独立电源（§4.4）。
 */

/* 每级之间的等待。运营商侧的问题（拆承载、限速）往往几秒后自己就好了，
 * 立刻重试只是把同一个失败再撞一次。 */
#define RECONNECT_BACKOFF_MS 3000

int modem_reconnect(void)
{
	LOG_WRN("开始断线重连阶梯");

	/* --- 1 级：只重建 MQTT 会话 --------------------------------------- */
	atomic_set(&connected, 0);
	(void)at_cmd("AT+MDISCONNECT");
	if (stage_session() == 0) {
		LOG_INF("重连成功（1 级：MQTT 会话）");
		return 0;
	}
	LOG_WRN("1 级重连失败，升级到 PDP 重拨");
	k_sleep(K_MSEC(RECONNECT_BACKOFF_MS));

	/* --- 2 级：拆掉承载重新附着 --------------------------------------- */
	atomic_set(&connected, 0);
	(void)at_cmd("AT+MDISCONNECT");
	(void)at_cmd("AT+CIPSHUT");
	if (stage_attach() == 0 && stage_session() == 0) {
		LOG_INF("重连成功（2 级：PDP 重拨）");
		return 0;
	}
	LOG_WRN("2 级重连失败，升级到模组重启");
	k_sleep(K_MSEC(RECONNECT_BACKOFF_MS));

	/* --- 3 级：整个模组断电重启 --------------------------------------- */
	atomic_set(&connected, 0);
	/* 软关机可能因为模组已经卡死而不应答，所以不看返回值，
	 * 后面 power_off_hard() 无条件走一遍 PWRKEY。 */
	(void)at_cmd("AT+CPOWD=1");
	power_off_hard();
	/* 关机后要等内部电源真的塌下去，否则下一次 PWRKEY 会被当成
	 * 「已经开机」而不触发开机。手册没给这个时间，取 2 s 是保守值。 */
	k_sleep(K_SECONDS(2));

	int rc = modem_connect();
	if (rc == 0) {
		LOG_INF("重连成功（3 级：模组重启）");
		return 0;
	}
	LOG_ERR("三级重连全部失败 rc=%d —— 放弃本轮，等下个上报周期", rc);
	return rc;
}

int modem_disconnect(void)
{
	if (atomic_get(&connected) != 0) {
		(void)at_cmd("AT+MDISCONNECT");
		(void)at_cmd("AT+CIPSHUT");
		atomic_set(&connected, 0);
	}
	/* 关机而不是进 PSM+：契约 §4.1 说了默认档是模组关机，
	 * 下行靠服务端排队等设备下次上线（不靠 broker retain）。 */
	int rc = at_cmd("AT+CPOWD=1");
	if (rc != 0) {
		LOG_WRN("软关机失败 rc=%d，改用 PWRKEY", rc);
		power_off_hard();
	}
	return 0;
}

bool modem_is_connected(void)
{
	return atomic_get(&connected) != 0;
}

int modem_publish(const char *topic, const uint8_t *payload, size_t len,
		  int qos)
{
	if (atomic_get(&connected) == 0) {
		return -ENOTCONN;
	}
	if (len > PROTO_MAX_PAYLOAD) {
		LOG_ERR("payload %zu 超过上限 %d", len, PROTO_MAX_PAYLOAD);
		return -E2BIG;
	}

	/* HEX 模式：payload 要转成十六进制字符串，字节数翻倍。
	 * 这就是契约 §5 里「省 100 字节 = 省 0.2 秒」的来源。 */
	static char hexbuf[PROTO_MAX_PAYLOAD * 2 + 1];
	for (size_t i = 0; i < len; i++) {
		(void)snprintf(&hexbuf[i * 2], 3, "%02X", payload[i]);
	}
	hexbuf[len * 2] = '\0';

	/* AT+MPUBEX 支持二进制（EX 版）。retain 参数恒为 0（契约 §4.1）。 */
	static char cmd[320];
	int n = snprintf(cmd, sizeof(cmd), "AT+MPUBEX=\"%s\",%d,0,%zu",
			 topic, qos, len);
	if (n < 0 || (size_t)n >= sizeof(cmd)) {
		return -ENOMEM;
	}

	k_mutex_lock(&at_lock, K_FOREVER);
	uart_write_str(cmd);
	uart_write_str("\r\n");

	/* 等裸 '>' 提示符 —— 它**不带换行**（§8.5 的陷阱）。 */
	char line[LINE_MAX];
	int rc = -EIO;
	int64_t deadline = k_uptime_get() + AT_TIMEOUT_MS;
	while (k_uptime_get() < deadline) {
		int64_t left = deadline - k_uptime_get();
		int r = read_line(line, sizeof(line), (uint32_t)left);
		if (r == -EMSGSIZE) {
			continue;   /* 超长行已丢弃，流仍同步（审计 M1） */
		}
		if (r < 0) {
			break;
		}
		if (line[0] == '>') {
			rc = 0;
			break;
		}
		if (strstr(line, "ERROR") != NULL) {
			LOG_ERR("MPUBEX 被拒：%s", line);
			k_mutex_unlock(&at_lock);
			return -EIO;
		}
		(void)consume_urc(line);
	}
	if (rc != 0) {
		LOG_ERR("等 '>' 提示符超时");
		k_mutex_unlock(&at_lock);
		return -ETIMEDOUT;
	}

	uart_write_str(hexbuf);
	k_mutex_unlock(&at_lock);

	/* "SEND OK" 也是带外 URC。9600 下 4 KB HEX 要 8.5 秒，
	 * 所以超时按 payload 长度算：每字节 4 ms（HEX 翻倍再留一倍余量）+ 5 秒底。 */
	uint32_t timeout = 5000 + (uint32_t)len * 4;
	rc = at_cmd_expect(NULL, "SEND OK", timeout, NULL, 0);
	if (rc != 0) {
		LOG_ERR("发布未确认 topic=%s（%zu 字节）", topic, len);
		return rc;
	}
	LOG_DBG("已发布 %s（%zu 字节）", topic, len);
	return 0;
}

int modem_poll(uint32_t timeout_ms)
{
	char line[LINE_MAX];
	int64_t deadline = k_uptime_get() + timeout_ms;
	int handled = 0;

	while (k_uptime_get() < deadline) {
		int64_t left = deadline - k_uptime_get();
		int n = read_line(line, sizeof(line), (uint32_t)left);
		if (n == -EMSGSIZE) {
			continue;   /* 超长行已丢弃，流仍同步（审计 M1） */
		}
		if (n < 0) {
			break;
		}
		LOG_DBG("← (poll) %s", line);
		if (consume_urc(line)) {
			handled++;
		}
	}
	return handled;
}

int modem_csq(void)
{
	char resp[64] = { 0 };
	if (at_query("AT+CSQ", resp, sizeof(resp)) != 0) {
		return -1;
	}
	/* +CSQ: <rssi>,<ber> */
	const char *p = strchr(resp, ':');
	if (p == NULL) {
		return -1;
	}
	int rssi = atoi(p + 1);
	/* 99 = 未知/不可测，当成读不到 */
	return (rssi >= 0 && rssi < 99) ? rssi : -1;
}

uint32_t modem_utc(void)
{
	uint32_t base;
	int64_t taken_at;

	K_SPINLOCK(&time_lock) {
		base = nitz_utc;
		taken_at = nitz_uptime;
	}

	if (base == 0) {
		return 0;   /* 契约 §5.6：不知道就填 0，别猜 */
	}
	/* 用 uptime 差补上从拿到时间到现在的这段。
	 * 模组关机档下这段可能很长，误差随之累积 —— 这正是契约要求服务端
	 * 用 t_srv 落库、t_dev 只作诊断的原因（§5.6）。 */
	int64_t elapsed_ms = k_uptime_get() - taken_at;
	return base + (uint32_t)(elapsed_ms / 1000);
}

int modem_lbs(double *lat, double *lon, float *acc_m)
{
	/* 审计 L1：原来是裸 `!connected`，违反本文件 41-47 行自己立的规矩
	 * （connected 必须 atomic 访问）。 */
	if (atomic_get(&connected) == 0) {
		return -ENOTCONN;
	}
	char resp[128] = { 0 };
	/* AT+CIPGSMLOC=1,1 是免费的基站定位（§9.5）。
	 * AT+AIRLBS 精度更好但付费，且含 WiFi 融合。 */
	int rc = at_cmd_expect("AT+CIPGSMLOC=1,1", "+CIPGSMLOC:", 20000,
			       resp, sizeof(resp));
	if (rc != 0) {
		LOG_WRN("基站定位失败 —— 可能模组不支持或要付费开通（§11 #15）");
		return rc;
	}

	/* +CIPGSMLOC: <locationcode>,<lon>,<lat>,<date>,<time> —— 注意经度在前 */
	const char *p = strchr(resp, ':');
	if (p == NULL) {
		return -EINVAL;
	}
	p = strchr(p, ',');
	if (p == NULL) {
		return -EINVAL;
	}
	double lon_v = strtod(p + 1, NULL);
	p = strchr(p + 1, ',');
	if (p == NULL) {
		return -EINVAL;
	}
	double lat_v = strtod(p + 1, NULL);

	/* ⚠ 字段为空或不是数字时 strtod 返回 0.0，**不能当成有效坐标** ——
	 * (0,0) 在几内亚湾，服务端的范围校验（契约 §5.2 的 ±90/±180）拦不住它，
	 * 落库后地图上车会瞬移到那里（审计 M2）。
	 * GNSS 路径有同样的防护（gnss.c 的 `f->valid = (lat != 0 || lon != 0)`），
	 * 这里补齐。顺带把范围也判掉：基站定位偶发给出越界值时同样丢弃。 */
	if ((lat_v == 0.0 && lon_v == 0.0) ||
	    lat_v < -90.0 || lat_v > 90.0 ||
	    lon_v < -180.0 || lon_v > 180.0) {
		LOG_WRN("基站定位给了无效坐标 %.6f,%.6f —— 丢弃", lat_v, lon_v);
		return -EINVAL;
	}

	*lat = lat_v;
	*lon = lon_v;

	/* 基站定位没有精度字段。给一个保守的固定值 ——
	 * 契约 §5.2 要求 `a` 能反映粗糙度，1000 m 是城区基站定位的典型量级。
	 * 服务端和 HA 会把它画成一个大圈，那正是想要的效果。 */
	*acc_m = 1000.0f;

	LOG_INF("基站定位 %.6f,%.6f（±1000 m 估）", *lat, *lon);
	return 0;
}

/* --- 这个实现还缺什么（对照 DESIGN.md §8.5 的 2000~4000 行估算） -------------
 *
 * 已经有的：URC/应答混流解析、裸 '>' 提示符、HEX 编解码、掉线 URC 识别、
 * 按 payload 长度算的发送超时、开机波特率训练。
 *
 * **还缺的，按重要性排**：
 * 1. **睡眠仲裁器**。每次写 AT 之前要判断模组醒没醒（§8.5），
 *    没醒要先用 UART 字节或 DTR 唤醒。当前实现假设模组一直是醒的，
 *    所以省电档下第一条命令可能丢。
 *    ⚠ 这一条依赖 MAIN_DTR 的脚号，而那个**至今无来源**（§8.7 硬门禁 / §11 #18）。
 * 2. **证书灌入**（AT+FSCREATE / AT+FSWRITE）。本文件假设证书已在模组 FS 里。
 *    谁在产线上做这一步、私钥以什么形式存在，四份历史文档都没写（§8.8 / §11 #24）。
 * 3. **PSM+ / PRO 双档切换**（§8.4 / §11 #20）。现在只有「连上」和「关机」两态。
 * 4. **QoS1 的重发与 packet id 跟踪**。现在靠 "SEND OK" 一次确认，
 *    没有超时重发队列 —— 契约 §9.1 那个「设备侧要不要做补发队列」的待决项（§11 #22）
 *    就落在这里。
 *
 * **已补上的**：
 *   - NITZ/CCLK → Unix 秒换算（`parse_modem_time()`）
 *   - 断线重连三级阶梯（`modem_reconnect()`，测试见 firmware/tests/modem_reconnect）
 */

