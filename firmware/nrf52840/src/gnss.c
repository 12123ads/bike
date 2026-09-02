/* GNSS NMEA 解析。见 gnss.h：gnss_fix() 是阻塞的。 */

#include "gnss.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

LOG_MODULE_REGISTER(gnss, CONFIG_EBIKE_LOG_LEVEL);

static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(uart0));
static const struct gpio_dt_spec power =
	GPIO_DT_SPEC_GET(DT_NODELABEL(gnss_power), gpios);

/* NMEA 一行最长 82 字节（含 CRLF），给 128 是余量 */
#define NMEA_MAX 128
#define RX_RING_SIZE 512

static uint8_t rx_ring_buf[RX_RING_SIZE];
static struct ring_buf rx_ring;
static struct k_sem line_sem;

/* 中断里只做「收字节进环形缓冲」，解析在线程里做。
 * 9600 baud 下一字节约 1 ms，中断里做解析会拖长中断时间。 */
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
		/* 缓冲满了就丢新字节。丢 NMEA 行的后果是这一轮定位晚一秒，
		 * 比丢掉整个环形缓冲的旧内容好 —— 旧内容里可能有完整的一行。 */
		(void)ring_buf_put(&rx_ring, &c, 1);
		if (c == '\n') {
			k_sem_give(&line_sem);
		}
	}
}

/* 从环形缓冲取一行（到 '\n' 为止）。返回长度，没有完整行返回 0。 */
static size_t take_line(char *out, size_t out_len)
{
	size_t n = 0;
	uint8_t c;
	while (ring_buf_get(&rx_ring, &c, 1) == 1) {
		if (c == '\r') {
			continue;
		}
		if (c == '\n') {
			out[n] = '\0';
			return n;
		}
		if (n + 1 < out_len) {
			out[n++] = (char)c;
		}
		/* 超长行就丢掉多出来的字符，继续找 '\n' —— 不要中途返回，
		 * 否则残留的半行会污染下一次解析。 */
	}
	return 0;
}

/* NMEA 校验和：'$' 之后到 '*' 之前所有字节的 XOR。
 * 必须验 —— 9600 baud 上误码不罕见，一个坏掉的纬度字段会让车瞬移。 */
static bool checksum_ok(const char *line)
{
	const char *star = strrchr(line, '*');
	if (star == NULL || line[0] != '$') {
		return false;
	}
	uint8_t sum = 0;
	for (const char *p = line + 1; p < star; p++) {
		sum ^= (uint8_t)*p;
	}
	long given = strtol(star + 1, NULL, 16);
	return (long)sum == given;
}

/* 取第 idx 个逗号分隔字段（0 = 消息类型）。空字段返回空串。 */
static bool field(const char *line, int idx, char *out, size_t out_len)
{
	int cur = 0;
	const char *p = line;
	while (cur < idx) {
		p = strchr(p, ',');
		if (p == NULL) {
			return false;
		}
		p++;
		cur++;
	}
	size_t n = 0;
	while (*p != '\0' && *p != ',' && *p != '*' && n + 1 < out_len) {
		out[n++] = *p++;
	}
	out[n] = '\0';
	return true;
}

/* NMEA 的 ddmm.mmmm → 十进制度 */
static double nmea_to_deg(const char *val, const char *hemi)
{
	if (val[0] == '\0') {
		return 0.0;
	}
	double raw = strtod(val, NULL);
	int deg = (int)(raw / 100.0);
	double min = raw - deg * 100.0;
	double out = deg + min / 60.0;
	if (hemi[0] == 'S' || hemi[0] == 'W') {
		out = -out;
	}
	return out;
}

/* 解析 GGA：定位质量、卫星数、HDOP。 */
static void parse_gga(const char *line, struct gnss_fix *f)
{
	char lat[16], ns[4], lon[16], ew[4], quality[4], sats[4], hdop[8];

	if (!field(line, 2, lat, sizeof(lat)) ||
	    !field(line, 3, ns, sizeof(ns)) ||
	    !field(line, 4, lon, sizeof(lon)) ||
	    !field(line, 5, ew, sizeof(ew)) ||
	    !field(line, 6, quality, sizeof(quality)) ||
	    !field(line, 7, sats, sizeof(sats)) ||
	    !field(line, 8, hdop, sizeof(hdop))) {
		return;
	}

	/* quality 0 = 未定位。这时候的 lat/lon 是垃圾，绝不能用。 */
	if (quality[0] == '0' || quality[0] == '\0') {
		f->valid = false;
		return;
	}

	f->lat = nmea_to_deg(lat, ns);
	f->lon = nmea_to_deg(lon, ew);
	f->sats = (uint8_t)atoi(sats);
	f->hdop = (float)strtod(hdop, NULL);
	/* 精度圈估算：HDOP × 接收机 UERE。消费级模块 UERE 取 5 m 是常见近似。
	 * 这是估算不是测量 —— 契约 §5.2 的 `a` 字段本来就只要求反映粗糙度。 */
	f->acc_m = f->hdop > 0.0f ? f->hdop * 5.0f : 25.0f;
	f->valid = (f->lat != 0.0 || f->lon != 0.0);
}

/* 解析 RMC：速度、航向、UTC 日期时间。 */
static void parse_rmc(const char *line, struct gnss_fix *f)
{
	char status[4], speed[12], course[12];

	if (!field(line, 2, status, sizeof(status))) {
		return;
	}
	if (status[0] != 'A') {
		return;   /* V = 无效 */
	}
	if (field(line, 7, speed, sizeof(speed)) && speed[0] != '\0') {
		/* NMEA 给的是节，转 m/s */
		f->speed_ms = (float)(strtod(speed, NULL) * 0.514444);
	}
	if (field(line, 8, course, sizeof(course)) && course[0] != '\0') {
		f->heading = (int16_t)strtod(course, NULL);
	}
}

int gnss_power_off(void)
{
	if (!gpio_is_ready_dt(&power)) {
		return -ENODEV;
	}
	/* 关电前把 UART 中断关掉，否则残留字节会继续触发中断 */
	uart_irq_rx_disable(uart);
	return gpio_pin_set_dt(&power, 0);
}

int gnss_fix(struct gnss_fix *out, uint32_t timeout_s)
{
	if (!device_is_ready(uart) || !gpio_is_ready_dt(&power)) {
		return -ENODEV;
	}

	memset(out, 0, sizeof(*out));
	out->heading = -1;
	out->speed_ms = -1.0f;

	ring_buf_reset(&rx_ring);
	k_sem_reset(&line_sem);

	int rc = gpio_pin_set_dt(&power, 1);
	if (rc != 0) {
		return rc;
	}
	uart_irq_rx_enable(uart);

	int64_t deadline = k_uptime_get() + (int64_t)timeout_s * 1000;
	char line[NMEA_MAX];
	struct gnss_fix acc = { .heading = -1, .speed_ms = -1.0f };
	int result = -ETIMEDOUT;

	while (k_uptime_get() < deadline) {
		/* 等一行。1 秒超时是为了能定期检查 deadline —— NMEA 正常每秒一批，
		 * 完全收不到说明模块没上电或线接错了。 */
		if (k_sem_take(&line_sem, K_SECONDS(1)) != 0) {
			continue;
		}
		while (take_line(line, sizeof(line)) > 0) {
			if (!checksum_ok(line)) {
				LOG_DBG("NMEA 校验和不对，丢弃");
				continue;
			}
			/* 前缀是 $GP/$GN/$BD —— 支持北斗所以不能只认 GP */
			if (strstr(line, "GGA") != NULL) {
				parse_gga(line, &acc);
			} else if (strstr(line, "RMC") != NULL) {
				parse_rmc(line, &acc);
			}

			/* 收敛条件：定上了且至少 4 颗星。
			 * 少于 4 颗的解在几何上是不定的，位置可能差几公里。 */
			if (acc.valid && acc.sats >= 4) {
				*out = acc;
				result = 0;
				goto done;
			}
		}
	}

	/* 超时但拿到了一个弱解：也交出去，让服务端拿 acc_m 去判要不要用。
	 * 比完全没有位置好 —— 车被偷了的时候一个 500 m 精度的点也有用。 */
	if (acc.valid) {
		*out = acc;
		result = 0;
		LOG_WRN("定位超时但有弱解：%u 颗星，精度约 %d m",
			acc.sats, (int)acc.acc_m);
	}

done:
	(void)gnss_power_off();
	if (result == 0) {
		LOG_INF("定位 %.6f,%.6f  %u 星  ±%d m",
			out->lat, out->lon, out->sats, (int)out->acc_m);
	} else {
		LOG_WRN("定位失败（%u s 内没有有效解）", timeout_s);
	}
	return result;
}

int gnss_init(void)
{
	if (!device_is_ready(uart)) {
		LOG_ERR("GNSS UART 未就绪");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&power)) {
		LOG_ERR("GNSS 电源门控 GPIO 未就绪");
		return -ENODEV;
	}

	ring_buf_init(&rx_ring, sizeof(rx_ring_buf), rx_ring_buf);
	k_sem_init(&line_sem, 0, 1);

	/* 初始断电 —— GNSS 是几十 mA，绝不能默认开着 */
	int rc = gpio_pin_configure_dt(&power, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) {
		return rc;
	}

	uart_irq_callback_user_data_set(uart, uart_isr, NULL);
	uart_irq_rx_disable(uart);

	LOG_INF("GNSS 就绪（默认断电）");
	return 0;
}
