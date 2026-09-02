/*
 * 模组时间串 → Unix 秒的换算测试。
 *
 * 被测的是 firmware/nrf52840/src/modem.c 的原件。它的 parse_modem_time()
 * 是 static，所以这里**不直接调它**，而是从 modem.c 的公开面走进去：
 * 把一条 `+NITZ:` URC 塞进 uart-emul 的 RX FIFO，让 modem_poll() 读到、
 * consume_urc() → handle_time_urc() → parse_modem_time() 走完，
 * 再用 modem_utc() 读结果。这条路径和真设备上完全一致。
 *
 * 这么做有一个额外好处：顺便钉住了「URC 前缀是 +NITZ: 而不是 +CTZV:」。
 * 上一版认的是 `+CTZV:` 和 `*PSUTTZ`（别家模组的形状），在这块模组上
 * 永远匹配不上 —— 那种错误只有走完整条链才暴露，只测 parse 函数抓不到。
 *
 * 每条断言的期望值都是独立算出来的（Python 的 calendar.timegm），
 * 不是从被测代码反推的。
 */

#include "modem.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

static const struct device *const euart = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* 把一行喂给 modem.c 的行读取器，然后让它消化掉。
 *
 * read_line() 以 '\n' 结束一行，所以必须带换行；'\r' 会被丢掉，
 * 这里连 CRLF 一起发，和真模组一样。 */
static void feed_line(const char *line)
{
	char buf[128];
	int n = snprintf(buf, sizeof(buf), "%s\r\n", line);

	zassert_true(n > 0 && n < (int)sizeof(buf), "测试自己的缓冲不够");
	uint32_t put = uart_emul_put_rx_data(euart, (const uint8_t *)buf,
					     (size_t)n);
	zassert_equal(put, (uint32_t)n, "uart-emul 只吃进了 %u/%d 字节", put, n);

	/* uart-emul 的中断回调走它自己的工作队列，不是同步调的 ——
	 * 给它一点时间把字节推给 modem.c 的 ISR。 */
	k_sleep(K_MSEC(50));

	/* 100 ms 足够读完一行；读不到东西 read_line() 会超时退出。 */
	(void)modem_poll(100);
}

/* ⚠ **`modem_utc()` 是个走着的钟**，不是一个存下来的常量：它返回
 * `采纳时的值 + 从那时起的 uptime 差`。所以不能把两次相隔较久的
 * `modem_utc()` 直接比 —— 跨过一个整秒就差 1。
 *
 * 后果：每个「脏数据不该被采纳」的用例都必须**紧挨着**先喂一遍已知好值，
 * 让参考点重新落在几十毫秒之内。第一版把好值抠出来存成 `good`、
 * 然后在循环里比了 9 次，累计 1.5 s，测试自己红了。
 *
 * 一次 feed_line 约 170 ms（50 ms 等工作队列 + 100 ms poll），
 * 所以「好值 + 一个脏值 + 读」这个三步在 400 ms 内，安全。 */
#define GOOD_LINE "+NITZ:25/06/15,12:00:00+00,0"
#define GOOD_UTC  1749988800U

/* 喂一个已知好值，确认它进去了，返回期望值。 */
static uint32_t feed_known_good(void)
{
	feed_line(GOOD_LINE);
	zassert_equal(modem_utc(), GOOD_UTC, "前置的好值没进去：%u",
		      modem_utc());
	return GOOD_UTC;
}

static void *setup(void)
{
	zassert_true(device_is_ready(euart), "uart-emul 没就绪");
	zassert_ok(modem_init(NULL), "modem_init 失败");
	return NULL;
}

ZTEST_SUITE(modem_time, NULL, setup, NULL, NULL, NULL);

/* --- 核心：两个「不知道就一定算错」的点 ------------------------------------- */

/* 厂商 NTP 教程里那张 LLCOM 抓图的原值。
 * 主机 Beijing 时钟 [2024/11/06 09:47:14] 发 AT+CCLK?，模组答这一串。
 *
 * 期望值 1730857633 = 2024-11-06T01:47:13Z，独立算法：
 *   calendar.timegm((2024,11,6,9,47,13)) = 1730886433   ← 当成 UTC 的话
 *   1730886433 - 32*900                  = 1730857633   ← 减掉 +8h 偏移
 */
ZTEST(modem_time, test_vendor_screenshot_beijing)
{
	feed_line("+NITZ:24/11/06,09:47:13+32,0");
	zassert_equal(modem_utc(), 1730857633U,
		      "东八区换算错了：得到 %u，应为 1730857633", modem_utc());
}

/* zz 当成整小时读会差 24 小时（+32 小时 vs +8 小时）。
 * 这一条用「如果按小时读会得到什么」来反向钉死单位：
 *   按小时读 = 1730886433 - 32*3600 = 1730771233
 * 上面那条已经断言了正确值，这里断言**不等于**错误值，
 * 把「碰巧过」的可能性排掉。 */
ZTEST(modem_time, test_zz_is_quarter_hours_not_hours)
{
	feed_line("+NITZ:24/11/06,09:49:28+32,0");
	uint32_t got = modem_utc();

	zassert_equal(got, 1730857768U, "quarter-hour 换算错了：%u", got);
	zassert_not_equal(got, 1730771233U,
			  "把 zz 当成整小时读了 —— 差 24 小时");
	zassert_not_equal(got, 1730886433U,
			  "没减本地偏移 —— 差 8 小时（照抄 nRF91 的错法）");
}

/* 西时区：负偏移要往**加**的方向走。
 *   calendar.timegm((2025,12,31,23,59,59)) = 1767225599
 *   1767225599 - (-20)*900 = 1767225599 + 18000 = 1767243599
 *   = 2026-01-01T04:59:59Z —— 顺便跨了年 */
ZTEST(modem_time, test_negative_offset_crosses_year)
{
	feed_line("+NITZ:25/12/31,23:59:59-20,0");
	zassert_equal(modem_utc(), 1767243599U,
		      "负偏移/跨年错了：%u", modem_utc());
}

/* 半时区（印度 +5:30 = +22 个 1/4 小时）。
 * 这是 quarter-hour 编码存在的**理由** —— 整小时编码表示不了它。
 *   calendar.timegm((2025,3,1,5,30,0)) = 1740807000
 *   1740807000 - 22*900 = 1740787200 = 2025-03-01T00:00:00Z */
ZTEST(modem_time, test_half_hour_zone)
{
	feed_line("+NITZ:25/03/01,05:30:00+22,0");
	zassert_equal(modem_utc(), 1740787200U,
		      "半时区错了：%u", modem_utc());
}

/* 零偏移：模组还没配时区时 §3.12 的第一个例子就是 +00。
 *   calendar.timegm((2025,6,15,12,0,0)) = 1749988800，减 0 不变 */
ZTEST(modem_time, test_zero_offset)
{
	feed_line("+NITZ:25/06/15,12:00:00+00,0");
	zassert_equal(modem_utc(), 1749988800U,
		      "零偏移错了：%u", modem_utc());
}

/* --- 拒绝路径：宁可上报 0，不上报一个像时间的错数 --------------------------- */

/* 手册 §3.12 自己的例子是 2018 年（+CCLK: "18/08/07,13:28:31+32"）。
 * 模组没跟网络同步时会给出这类「格式合法但年份很早」的值，
 * 而契约 §5.6 的语义是「不知道就填 0」，不是「填一个看起来像时间的数」。
 *
 * 判据是「没有被这条脏数据改写」：先喂已知好值，再喂脏值，
 * 确认读到的仍是好值。这样也就不依赖用例的执行顺序。 */
ZTEST(modem_time, test_rejects_unsynced_early_year)
{
	uint32_t good = feed_known_good();

	feed_line("+NITZ:18/08/07,13:28:31+32,0");
	zassert_equal(modem_utc(), good,
		      "2018 年的脏数据被采纳了 —— 应该整条丢掉");
}

/* 字段越界。timeutil_timegm64() 不做任何范围检查（就是个多项式），
 * 所以校验必须在 parse 里做完；漏一个就会算出一个荒谬但非零的时间。
 *
 * 每个脏值前面都重新喂一次好值 —— 见 feed_known_good() 上面那段注释：
 * modem_utc() 是走着的钟，抠一次 `good` 然后比 9 次会被 uptime 漂移打败。 */
ZTEST(modem_time, test_rejects_out_of_range_fields)
{
	static const char *const bad[] = {
		"+NITZ:25/13/15,12:00:00+00,0",   /* 月 13 */
		"+NITZ:25/00/15,12:00:00+00,0",   /* 月 0 */
		"+NITZ:25/06/32,12:00:00+00,0",   /* 日 32 */
		"+NITZ:25/06/00,12:00:00+00,0",   /* 日 0 */
		"+NITZ:25/06/15,24:00:00+00,0",   /* 时 24 */
		"+NITZ:25/06/15,12:60:00+00,0",   /* 分 60 */
		"+NITZ:25/06/15,12:00:60+00,0",   /* 秒 60 */
		"+NITZ:25/06/15,12:00:00+99,0",   /* 时区超 +48 */
		"+NITZ:25/06/15,12:00:00-99,0",   /* 时区超 -48 */
	};

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		uint32_t good = feed_known_good();

		feed_line(bad[i]);
		zassert_equal(modem_utc(), good,
			      "越界字段被采纳了：%s", bad[i]);
	}
}

/* 格式根本不对的，也必须丢掉而不是解出半个值。 */
ZTEST(modem_time, test_rejects_malformed)
{
	static const char *const bad[] = {
		"+NITZ:",                          /* 空 */
		"+NITZ:garbage",                   /* 没有数字 */
		"+NITZ:25/06/15",                  /* 只有日期，缺时间 */
		"+NITZ:25/06/15,12:00",            /* 缺秒 */
	};

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		uint32_t good = feed_known_good();

		feed_line(bad[i]);
		zassert_equal(modem_utc(), good,
			      "畸形串被采纳了：%s", bad[i]);
	}
}

/* --- URC 前缀：这块模组家族没有 +CTZV ---------------------------------------- */

/* 上一版的 handle_time_urc() 认 `+CTZV:` 和 `*PSUTTZ`，那是别家模组的形状 ——
 * 合宙 274 页 AT 手册全文 grep 零命中。也就是说「靠 URC 更新时间」那条路
 * 当时是断的，而且断得完全静默。
 *
 * 这一条钉住：+CTZV 不该被当成时间源。
 * 它同时也是一个回归护栏 —— 如果有人「顺手」把 +CTZV 加回去，这里会红。 */
ZTEST(modem_time, test_ctzv_is_not_a_time_source)
{
	uint32_t good = feed_known_good();

	/* 一个格式完全合法、但前缀是别家模组的串。若被采纳，
	 * modem_utc() 会变成 2026-01-22T19:08:21Z = 1769108901。 */
	feed_line("+CTZV:26/01/23,03:08:21+32,0");
	zassert_equal(modem_utc(), good,
		      "+CTZV 被当成时间源了 —— 这个模组家族没有这个 URC");
}

/* NITZ 前缀后面有没有空格都要认：手册文档里写的是 `+NITZ:11/08/02,...`
 * （无空格），而 §20 的真实抓包是 `+NITZ: 26/01/23,...`（有空格）。
 * 两种都出现在厂商自己的文档里，所以两种都必须吃下去。 */
ZTEST(modem_time, test_tolerates_space_after_colon)
{
	feed_line("+NITZ: 26/01/23,03:08:21+32,0");
	zassert_equal(modem_utc(), 1769108901U,
		      "带空格的 NITZ 没解出来：%u", modem_utc());
}

/* 时区字段缺失。手册没写上电默认值，而 27.007 允许省掉末尾三个字符。
 * 缺就按偏移 0 处理，不要整条丢掉 —— 一个没有时区的时间仍然比没有时间好。
 *   calendar.timegm((2025,6,15,12,0,0)) = 1749988800 */
ZTEST(modem_time, test_missing_timezone_treated_as_zero)
{
	feed_line("+NITZ:25/06/15,12:00:00");
	zassert_equal(modem_utc(), 1749988800U,
		      "缺时区的串没按偏移 0 处理：%u", modem_utc());
}
