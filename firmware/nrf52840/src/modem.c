/* Air780EP AT 状态机。见 modem.h 的三个前提和文件末尾的「还缺什么」。 */

#include "modem.h"
#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

LOG_MODULE_REGISTER(modem, CONFIG_EBIKE_LOG_LEVEL);

static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(uart1));
static const struct gpio_dt_spec pwrkey =
	GPIO_DT_SPEC_GET(DT_NODELABEL(modem_pwrkey), gpios);

/* 9600 baud ≈ 1 字节/ms。一行 AT 应答通常 < 64 字节，
 * 但 MQTT 下行的 URC 会带整个 payload，所以行缓冲要够大。 */
#define LINE_MAX      512
#define RX_RING_SIZE  2048
/* AT 命令的默认超时。9600 下一来一回加模组处理，2 秒是宽裕的；
 * 网络相关的命令单独给更长的超时。 */
#define AT_TIMEOUT_MS 2000

static uint8_t rx_ring_buf[RX_RING_SIZE];
static struct ring_buf rx_ring;
static struct k_sem rx_sem;
static struct k_mutex at_lock;

static modem_dn_cb dn_cb;
static bool connected;
static uint32_t nitz_utc;
static int64_t nitz_uptime;   /* 拿到 NITZ 时的 uptime，用来推算当前时间 */

/* 下行 URC 的解析缓冲。放静态区而不是栈上：LINE_MAX + payload 可能上 KB。 */
static char dn_topic[288];
static uint8_t dn_payload[PROTO_MAX_PAYLOAD];

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

/* 读一行（以 '\n' 结束）。返回长度，超时返回 -ETIMEDOUT。
 *
 * ⚠ 裸 '>' 提示符**不带换行**（§8.5 的陷阱之一），所以单独处理：
 * 读到 '>' 就立刻返回，不等换行。 */
static int read_line(char *out, size_t out_len, uint32_t timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	size_t n = 0;

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
			if (n == 0) {
				continue;   /* 空行，AT 应答前后都有 */
			}
			out[n] = '\0';
			return (int)n;
		}
		if (n + 1 < out_len) {
			out[n++] = (char)c;
		}

		/* 裸提示符：收到就返回，它后面没有换行 */
		if (n == 1 && out[0] == '>') {
			out[1] = '\0';
			return 1;
		}
	}
	if (n > 0) {
		out[n] = '\0';
		return (int)n;
	}
	return -ETIMEDOUT;
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
			connected = false;
			return true;
		}
	}
	return false;
}

static bool handle_time_urc(const char *line)
{
	/* +CTZV / *PSUTTZ 之类的时间 URC。格式随固件版本变，
	 * 这里只认最常见的 +CCLK 风格 "yy/MM/dd,hh:mm:ss±zz"。 */
	const char *p = strchr(line, '"');
	if (strncmp(line, "+CTZV:", 6) != 0 && strstr(line, "PSUTTZ") == NULL) {
		return false;
	}
	if (p == NULL) {
		return true;
	}
	/* 完整的 NITZ → Unix 秒换算要处理时区和闰年，留给 modem_utc 用
	 * AT+CCLK? 主动查一次，比在 URC 里解析可靠。 */
	LOG_DBG("收到时间 URC，稍后用 AT+CCLK? 取准确值");
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

		if (resp != NULL && resp_len > 0 && line[0] != '\0') {
			/* 只留最后一条非空的信息行 —— 查询命令的结果在那里 */
			strncpy(resp, line, resp_len - 1);
			resp[resp_len - 1] = '\0';
		}

		if (strstr(line, expect) != NULL) {
			ret = 0;
			break;
		}
		if (strstr(line, "ERROR") != NULL) {
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

/* --- 开机 ------------------------------------------------------------------- */

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

int modem_connect(void)
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

	/* 拿运营商时间（免费、无往返，§8.1）。契约 §5.6：拿不到就上报 t=0。 */
	(void)at_cmd("AT+CTZR=1");
	char clk[64] = { 0 };
	if (at_query("AT+CCLK?", clk, sizeof(clk)) == 0) {
		LOG_INF("模组时间：%s", clk);
		/* NITZ → Unix 秒的换算见 modem_utc() 的注释：本版未实现，
		 * 所以 nitz_utc 保持 0，上行的 t 就是 0。 */
	}

	/* TLS 配置。证书用 AT+FSWRITE 预先写进模组 FS（§8.2），
	 * 本函数假设已经写好 —— 灌证书是产线动作，不是每次连接都做。
	 * ⚠ 契约 §2 把设备侧从双向 TLS 降级成「TLS + 口令」，
	 * 所以这里只配 CA 和 hostname 校验，不配客户端证书。 */
	(void)at_cmd("AT+SSLCFG=\"cacert\",0,\"ca.crt\"");
	/* seclevel 1 = 只校验服务端证书。2 才是双向。 */
	(void)at_cmd("AT+SSLCFG=\"seclevel\",0,1");
	if (at_cmd("AT+SSLCFG=\"hostname\",0,\"" CONFIG_EBIKE_MQTT_HOST "\"") != 0) {
		LOG_WRN("配 hostname 失败 —— TLS 握手可能因证书 CN 不匹配而失败");
	}

	/* MQTT 参数。LWT 在这里配（契约 §4.2：payload 是 {"lwt":1}，retain）。 */
	char cmd[512];
	int n = snprintf(cmd, sizeof(cmd),
		"AT+MCONFIG=\"%s\",\"%s\",\"%s\",1,1,60,\"%s\",\"{\\\"lwt\\\":1}\"",
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

	/* HEX 模式：二进制安全，代价是串口字节数翻倍（契约 §5 的算术前提）。 */
	(void)at_cmd("AT+MQTTMODE=1");

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

	connected = true;
	LOG_INF("MQTT 已连接");
	return 0;
}

int modem_disconnect(void)
{
	if (connected) {
		(void)at_cmd("AT+MDISCONNECT");
		(void)at_cmd("AT+CIPSHUT");
		connected = false;
	}
	/* 关机而不是进 PSM+：契约 §4.1 说了默认档是模组关机，
	 * 下行靠服务端排队等设备下次上线（不靠 broker retain）。 */
	int rc = at_cmd("AT+CPOWD=1");
	if (rc != 0) {
		LOG_WRN("软关机失败 rc=%d，改用 PWRKEY", rc);
		(void)gpio_pin_set_dt(&pwrkey, 1);
		k_sleep(K_MSEC(1500));
		(void)gpio_pin_set_dt(&pwrkey, 0);
	}
	return 0;
}

bool modem_is_connected(void)
{
	return connected;
}

int modem_publish(const char *topic, const uint8_t *payload, size_t len,
		  int qos)
{
	if (!connected) {
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
	if (nitz_utc == 0) {
		return 0;   /* 契约 §5.6：不知道就填 0，别猜 */
	}
	/* 用 uptime 差补上从拿到 NITZ 到现在的时间 */
	int64_t elapsed_ms = k_uptime_get() - nitz_uptime;
	return nitz_utc + (uint32_t)(elapsed_ms / 1000);
}

int modem_lbs(double *lat, double *lon, float *acc_m)
{
	if (!connected) {
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
	*lon = strtod(p + 1, NULL);
	p = strchr(p + 1, ',');
	if (p == NULL) {
		return -EINVAL;
	}
	*lat = strtod(p + 1, NULL);

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
 *
 * 1. **断线重连阶梯**。现在 connected=false 之后靠上层重新调 modem_connect()，
 *    那会走完整的开机流程。真正需要的是分级恢复：
 *    MQTT 重连 → CIPSHUT + 重拨 PDP → 整个模组重启。
 * 2. **睡眠仲裁器**。每次写 AT 之前要判断模组醒没醒（§8.5），
 *    没醒要先用 UART 字节或 DTR 唤醒。当前实现假设模组一直是醒的，
 *    所以省电档下第一条命令可能丢。
 *    ⚠ 这一条依赖 MAIN_DTR 的脚号，而那个**至今无来源**（§8.7 硬门禁 / §11 #18）。
 * 3. **NITZ → Unix 秒的换算**。现在 nitz_utc 永远是 0，所以所有上行的 t 都是 0。
 *    服务端用 t_srv 落库（契约 §5.6），所以功能上没坏，但设备侧日志里没有绝对时间。
 * 4. **证书灌入**（AT+FSCREATE / AT+FSWRITE）。本文件假设证书已在模组 FS 里。
 *    谁在产线上做这一步、私钥以什么形式存在，四份历史文档都没写（§8.8 / §11 #24）。
 * 5. **PSM+ / PRO 双档切换**（§8.4 / §11 #20）。现在只有「连上」和「关机」两态。
 * 6. **QoS1 的重发与 packet id 跟踪**。现在靠 "SEND OK" 一次确认，
 *    没有超时重发队列 —— 契约 §9.1 那个「设备侧要不要做补发队列」的待决项（§11 #22）
 *    就落在这里。
 */

