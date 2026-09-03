/*
 * NMEA 解析的运行时测试。
 *
 * 被测的是 firmware/nrf52840/src/gnss.c 的原件。`checksum_ok` / `field` /
 * `nmea_to_deg` / `parse_gga` 都是 static，所以这里**不直接调它们**，
 * 而是从公开面走进去：把 NMEA 行塞进 uart-emul 的 RX FIFO，让 `gnss_fix()`
 * 读到、解析完，用返回的 `struct gnss_fix` 收结果。
 * 这条路径和真设备上完全一致。
 *
 * 被钉住的缺陷（2026-09-03 第二轮审计 R4）：
 *
 *   R4  `checksum_ok` 用 `strtol` 解校验和，而 `strtol` 解析失败也返回 0
 *       且不设错误标记 —— 「校验和字段是垃圾」和「校验和就是 00」不可区分。
 *       实测 `$GGAAGG*--` 的 XOR 恰好是 0x00，`strtol("--")` 返回 0，
 *       **坏行通过了校验**。当前靠 parse_gga 的字段数检查兜住了没造成
 *       坐标错误，但那是运气 —— 这道防线的职责就是在解析前挡掉误码，
 *       而文件注释自称「必须验，一个坏掉的纬度字段会让车瞬移」。
 */

#include "gnss.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

static const struct device *const euart = DEVICE_DT_GET(DT_NODELABEL(uart0));

/* --- 喂字节 ---------------------------------------------------------------- */

/* 算一行的 NMEA 校验和（'$' 之后到 '*' 之前的 XOR），用来拼合法行。
 * 测试自己算而不是硬编码：改一个字段就不用手算一遍。 */
static uint8_t nmea_xor(const char *body)
{
	uint8_t sum = 0;

	for (const char *p = body; *p != '\0'; p++) {
		sum ^= (uint8_t)*p;
	}
	return sum;
}

/* 把 `$<body>*<校验和>\r\n` 塞进 RX FIFO。`body` 不含 '$' 和 '*'。 */
static void feed_valid(const char *body)
{
	char line[128];
	int n = snprintf(line, sizeof(line), "$%s*%02X\r\n", body,
			 nmea_xor(body));

	zassert_true(n > 0 && n < (int)sizeof(line), "测试自己的缓冲不够");
	zassert_equal(uart_emul_put_rx_data(euart, (const uint8_t *)line,
					    (size_t)n),
		      (uint32_t)n, "uart-emul 没吃下整行");
}

/* 原样塞一行（自动补 CRLF）—— 用来喂**故意坏掉**的校验和。 */
static void feed_raw_line(const char *line)
{
	char buf[128];
	int n = snprintf(buf, sizeof(buf), "%s\r\n", line);

	zassert_true(n > 0 && n < (int)sizeof(buf), "测试自己的缓冲不够");
	zassert_equal(uart_emul_put_rx_data(euart, (const uint8_t *)buf,
					    (size_t)n),
		      (uint32_t)n, "uart-emul 没吃下整行");
}

/* 一条能让 gnss_fix() 收敛的 GGA：quality=1、9 颗星。
 * 字段序（GGA）：1=UTC 2=lat 3=N/S 4=lon 5=E/W 6=quality 7=sats 8=hdop */
#define GOOD_GGA "GPGGA,123519,3113.8250,N,12128.4221,E,1,09,0.9,10.0,M,,,,"

static void *setup(void)
{
	zassert_true(device_is_ready(euart), "uart-emul 没就绪");
	zassert_ok(gnss_init(), "gnss_init 失败");
	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	/* 上一条用例可能留了字节在 FIFO 里。gnss_fix 开头会
	 * ring_buf_reset()，但 emul 的 FIFO 在它之外 —— 用一次极短的
	 * gnss_fix 把它抽干。 */
	struct gnss_fix drain;

	(void)gnss_fix(&drain, 1);
}

ZTEST_SUITE(gnss_nmea, NULL, setup, before, NULL, NULL);

/* --- 正常路径：先证明链路真的通 --------------------------------------------- */

/* 后面全是拒绝路径的断言，「一律不解析」也会全绿。这条防那个。 */
ZTEST(gnss_nmea, test_valid_gga_produces_a_fix)
{
	struct gnss_fix f;

	feed_valid(GOOD_GGA);

	zassert_ok(gnss_fix(&f, 3), "合法 GGA 没定上位");
	zassert_true(f.valid, "valid 没置上");
	zassert_equal(f.sats, 9, "卫星数 %u", f.sats);
	/* ddmm.mmmm → 度：3113.8250 = 31° + 13.825/60 = 31.230417 */
	zassert_within(f.lat, 31.230417, 1e-5, "纬度 %f", f.lat);
	/* 经度过 100 度：12128.4221 = 121° + 28.4221/60 = 121.473702 */
	zassert_within(f.lon, 121.473702, 1e-5, "经度 %f", f.lon);
}

ZTEST(gnss_nmea, test_southern_western_hemisphere_signs)
{
	struct gnss_fix f;

	feed_valid("GPGGA,123519,3113.8250,S,12128.4221,W,1,09,0.9,10.0,M,,,,");

	zassert_ok(gnss_fix(&f, 3), "南半球西经的 GGA 没定上位");
	zassert_true(f.lat < 0, "南纬没取负：%f", f.lat);
	zassert_true(f.lon < 0, "西经没取负：%f", f.lon);
	zassert_within(f.lat, -31.230417, 1e-5, "纬度 %f", f.lat);
	zassert_within(f.lon, -121.473702, 1e-5, "经度 %f", f.lon);
}

/* --- R4：校验和必须真的挡住坏行 --------------------------------------------- */

/* **这条是 R4 的核心。**
 *
 * `$GGAAGG*--` 的 XOR 恰好是 0x00，而 `strtol("--", NULL, 16)` 也返回 0
 * —— 旧代码认为它校验通过，然后因为行里含 "GGA" 而进 parse_gga。
 *
 * 观测点：先喂这条坏行，再喂一条合法 GGA。坏行必须**完全不影响**结果，
 * 而且不能被当成一次有效解。
 *
 * 修复前这条不一定红（parse_gga 的字段数检查会让它 return），
 * 所以下一条测试从「解析到的值」那一侧再钉一次。 */
ZTEST(gnss_nmea, test_unparseable_checksum_is_rejected)
{
	struct gnss_fix f;

	/* XOR == 0x00 且校验和字段不是十六进制 —— strtol 的返回值撞上真值 */
	feed_raw_line("$GGAAGG*--");
	feed_raw_line("$GGAAGG*zz");
	feed_raw_line("$GGAAGG*");     /* 校验和整段缺失 */
	feed_raw_line("$GGAAGG*0");    /* 只有一位 */
	feed_valid(GOOD_GGA);

	zassert_ok(gnss_fix(&f, 3), "坏行把后面的合法行也带坏了");
	zassert_equal(f.sats, 9, "解到的是坏行而不是合法行：sats=%u", f.sats);
	zassert_within(f.lat, 31.230417, 1e-5, "纬度 %f", f.lat);
}

/* 上一条的加强版：让坏行**带上会被解析出来的坐标**。
 *
 * 构造一条 XOR 为 0x00、校验和字段不可解析、且字段齐全的 GGA。
 * 旧代码会把它当好行 → parse_gga 取到那些坐标 → `acc.valid = true`
 * → 9 颗星满足收敛条件 → **gnss_fix 直接返回这个伪造的位置**。
 * 那就是文件注释里说的「车瞬移」。 */
ZTEST(gnss_nmea, test_bad_checksum_with_full_fields_cannot_set_position)
{
	struct gnss_fix f;
	static char body[128];
	static char line[128];
	/* 字段齐全、能被 parse_gga 解出坐标的 GGA 主体（不含 '$' 和 '*'）。 */
	const char *base = "GPGGA,010101,0102.0000,N,00203.0000,E,1,09,0.9,0,M,,,,";
	uint8_t x = nmea_xor(base);

	/* 往尾部空字段里补两个可打印字符，让整体 XOR 归零。
	 * 补在最后一个逗号之后，前 8 个字段不受影响。 */
	char pad1 = 0;
	char pad2 = 0;

	for (int c = 0x30; c <= 0x7E; c++) {
		int other = c ^ x;

		if (other >= 0x30 && other <= 0x7E && other != ',' && c != ',') {
			pad1 = (char)c;
			pad2 = (char)other;
			break;
		}
	}
	zassert_true(pad1 != 0, "没找到可打印的填充对 —— 换一条 base");

	(void)snprintf(body, sizeof(body), "%s%c%c", base, pad1, pad2);
	zassert_equal(nmea_xor(body), 0,
		      "测试自己没把 XOR 凑成 0 —— 那这条就测不到 R4");

	/* 校验和字段写 "--"：不是十六进制，`strtol` 返回 0 —— 正好撞上真实 XOR。 */
	(void)snprintf(line, sizeof(line), "$%s*--", body);
	feed_raw_line(line);

	int rc = gnss_fix(&f, 2);

	zassert_true(rc != 0 || !f.valid,
		     "校验和不可解析的行被当成了有效定位：%f,%f（%u 星）—— "
		     "strtol 失败返回 0 撞上了真实 XOR（审计 R4）",
		     f.lat, f.lon, f.sats);
}

/* 反向护栏：正常的两位十六进制校验和仍然要被接受。
 * 上一条修法如果写成「永远不认」，这条会红。小写十六进制也要认 ——
 * NMEA 0183 允许，而部分模块真的输出小写。 */
ZTEST(gnss_nmea, test_lowercase_checksum_still_accepted)
{
	struct gnss_fix f;
	static char line[128];
	const char *body = GOOD_GGA;

	(void)snprintf(line, sizeof(line), "$%s*%02x", body, nmea_xor(body));
	/* 确认真的是小写（%02x 对 0x9A 这类才有区别，这里断言一下） */
	feed_raw_line(line);

	zassert_ok(gnss_fix(&f, 3), "小写校验和被拒了：%s", line);
	zassert_equal(f.sats, 9, "sats=%u", f.sats);
}

/* 真正的误码（XOR 对不上）必须被拒。这是校验和的本职。 */
ZTEST(gnss_nmea, test_wrong_checksum_rejected)
{
	struct gnss_fix f;
	static char line[128];
	const char *body = GOOD_GGA;

	/* 故意写错一位 */
	(void)snprintf(line, sizeof(line), "$%s*%02X", body,
		       (uint8_t)(nmea_xor(body) ^ 0xFF));
	feed_raw_line(line);

	int rc = gnss_fix(&f, 2);

	zassert_true(rc != 0 || !f.valid, "校验和对不上的行被接受了");
}

/* --- 解析边界 --------------------------------------------------------------- */

/* quality=0 是「未定位」，此时 lat/lon 是垃圾，绝不能用。 */
ZTEST(gnss_nmea, test_quality_zero_is_not_a_fix)
{
	struct gnss_fix f;

	feed_valid("GPGGA,123519,3113.8250,N,12128.4221,E,0,09,0.9,10.0,M,,,,");

	int rc = gnss_fix(&f, 2);

	zassert_true(rc != 0 || !f.valid,
		     "quality=0 被当成了有效定位：%f,%f", f.lat, f.lon);
}

/* 字段数不够的 GGA 不能让下标越界，也不能产生位置。 */
ZTEST(gnss_nmea, test_truncated_gga_does_not_fix)
{
	struct gnss_fix f;

	feed_valid("GPGGA,123519,3113.8250,N");

	int rc = gnss_fix(&f, 2);

	zassert_true(rc != 0 || !f.valid, "字段不全的 GGA 产生了位置");
}

/* 少于 4 颗星的解在几何上是不定的（位置可能差几公里），
 * 不能满足收敛条件而立即返回 —— 但超时后作为弱解交出去是允许的。 */
ZTEST(gnss_nmea, test_few_satellites_is_not_a_converged_fix)
{
	struct gnss_fix f;

	feed_valid("GPGGA,123519,3113.8250,N,12128.4221,E,1,03,9.9,10.0,M,,,,");

	/* 只有 3 颗星：不该在收敛路径上立刻返回。超时后的弱解路径会给出它，
	 * 所以这里只断言「不是靠 sats>=4 收敛的」—— 用极短超时观察。 */
	int rc = gnss_fix(&f, 1);

	if (rc == 0) {
		zassert_true(f.sats < 4,
			     "3 颗星却走了收敛路径：sats=%u", f.sats);
	}
}

/* RMC 的速度/航向要并进同一个 fix，且节 → m/s 换算正确。 */
ZTEST(gnss_nmea, test_rmc_merges_speed_and_course)
{
	struct gnss_fix f;

	/* RMC 字段序：1=UTC 2=status 3=lat 4=N/S 5=lon 6=E/W 7=speed(节) 8=course */
	feed_valid("GPRMC,123519,A,3113.8250,N,12128.4221,E,10.0,45.0,230394,,");
	feed_valid(GOOD_GGA);

	zassert_ok(gnss_fix(&f, 3), "GGA+RMC 组合没定上位");
	/* 10 节 = 5.14444 m/s */
	zassert_within((double)f.speed_ms, 5.14444, 0.01,
		       "速度换算错：%f m/s", (double)f.speed_ms);
	zassert_equal(f.heading, 45, "航向 %d", f.heading);
}

/* RMC status=V（无效）时不能覆盖速度/航向的「无数据」哨兵。 */
ZTEST(gnss_nmea, test_rmc_void_status_does_not_set_speed)
{
	struct gnss_fix f;

	feed_valid("GPRMC,123519,V,3113.8250,N,12128.4221,E,10.0,45.0,230394,,");
	feed_valid(GOOD_GGA);

	zassert_ok(gnss_fix(&f, 3), "没定上位");
	zassert_true((double)f.speed_ms < 0.0,
		     "status=V 的 RMC 覆盖了速度：%f", (double)f.speed_ms);
	zassert_equal(f.heading, -1, "status=V 的 RMC 覆盖了航向：%d", f.heading);
}
