/*
 * 断线重连阶梯的测试。
 *
 * 被测的是 firmware/nrf52840/src/modem.c 的原件（`modem_reconnect()`）。
 *
 * 为什么必须有这个测试：阶梯的**全部价值**在于「只重跑坏掉的那一层」。
 * 一个无脑走 3 级（整个模组重启）的实现在功能上完全正确 —— 它也能恢复连接，
 * 单元测试里「重连成功」的断言照样绿。区别只在时间和电流：
 * 1 级约 5~10 s，3 级最坏 90 s，而那 90 秒里模组是全功率的（约 22 mA）。
 * 所以判据不能是「有没有恢复」，只能是「**跑了哪些 AT 命令**」。
 *
 * 做法：uart-emul 上挂一个假模组线程，它把 modem.c 发出的每条命令记进
 * 一个 trace，再按脚本回应。测试对 trace 断言。
 */

#include "modem.h"

#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

static const struct device *const euart = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* --- 命令 trace -------------------------------------------------------------- */

/* 附着失败那条路径会轮询 60 次 CGREG，所以条数要给够。 */
#define TRACE_MAX      256
#define TRACE_LINE_MAX 48

static char trace[TRACE_MAX][TRACE_LINE_MAX];
static size_t trace_n;
static struct k_mutex trace_lock;

static void trace_reset(void)
{
	k_mutex_lock(&trace_lock, K_FOREVER);
	trace_n = 0;
	k_mutex_unlock(&trace_lock);
}

static void trace_add(const char *cmd)
{
	k_mutex_lock(&trace_lock, K_FOREVER);
	if (trace_n < TRACE_MAX) {
		strncpy(trace[trace_n], cmd, TRACE_LINE_MAX - 1);
		trace[trace_n][TRACE_LINE_MAX - 1] = '\0';
		trace_n++;
	}
	k_mutex_unlock(&trace_lock);
}

/* 前缀匹配的条数。 */
static int trace_count(const char *prefix)
{
	size_t len = strlen(prefix);
	int n = 0;

	k_mutex_lock(&trace_lock, K_FOREVER);
	for (size_t i = 0; i < trace_n; i++) {
		if (strncmp(trace[i], prefix, len) == 0) {
			n++;
		}
	}
	k_mutex_unlock(&trace_lock);
	return n;
}

/* 完全相等的条数。裸 `AT`（开机时的波特率训练）必须用这个 ——
 * 用前缀会把所有 `AT+...` 都算进去，那个断言就永远成立、永远没意义。 */
static int trace_count_exact(const char *cmd)
{
	int n = 0;

	k_mutex_lock(&trace_lock, K_FOREVER);
	for (size_t i = 0; i < trace_n; i++) {
		if (strcmp(trace[i], cmd) == 0) {
			n++;
		}
	}
	k_mutex_unlock(&trace_lock);
	return n;
}

/* 第一次出现的下标，没出现返回 -1。用来断言顺序。 */
static int trace_first(const char *prefix)
{
	size_t len = strlen(prefix);
	int idx = -1;

	k_mutex_lock(&trace_lock, K_FOREVER);
	for (size_t i = 0; i < trace_n; i++) {
		if (strncmp(trace[i], prefix, len) == 0) {
			idx = (int)i;
			break;
		}
	}
	k_mutex_unlock(&trace_lock);
	return idx;
}

/* 完全相等的第一次出现下标。裸 `AT` 必须用这个（理由同 trace_count_exact）。 */
static int trace_first_exact(const char *cmd)
{
	int idx = -1;

	k_mutex_lock(&trace_lock, K_FOREVER);
	for (size_t i = 0; i < trace_n; i++) {
		if (strcmp(trace[i], cmd) == 0) {
			idx = (int)i;
			break;
		}
	}
	k_mutex_unlock(&trace_lock);
	return idx;
}

static void trace_dump(void)
{
	k_mutex_lock(&trace_lock, K_FOREVER);
	printk("--- trace（%zu 条）---\n", trace_n);
	for (size_t i = 0; i < trace_n; i++) {
		printk("  [%2zu] %s\n", i, trace[i]);
	}
	k_mutex_unlock(&trace_lock);
}

/* --- 失败注入 ---------------------------------------------------------------- */

/* 让 stage_session 的 TCP/TLS 连接失败几次。选 SSLMIPSTART 作为失败点，
 * 因为服务端重启 / TLS 会话过期在真机上就是在这一步表现出来的。 */
static int fail_session_times;
/* 让 stage_attach 失败几次：CGREG 回「未注册」，那个函数会轮询 60 秒后放弃。 */
static int fail_attach_times;
/* 让 power_on 的 AT 训练失败几次（模拟模组卡死到 AT 都不应答）。 */
static int fail_at_times;

static void inject(int session, int attach, int at)
{
	fail_session_times = session;
	fail_attach_times = attach;
	fail_at_times = at;
}

/* --- 假模组 ------------------------------------------------------------------ */

static void reply(const char *s)
{
	char buf[96];
	int n = snprintf(buf, sizeof(buf), "%s\r\n", s);

	if (n > 0 && n < (int)sizeof(buf)) {
		(void)uart_emul_put_rx_data(euart, (const uint8_t *)buf,
					    (size_t)n);
	}
}

/* 按命令回应。这张表就是 modem.c 走过的全部 AT 面。 */
static void respond(const char *cmd)
{
	trace_add(cmd);

	/* 开机时的波特率训练。modem.c 连发最多 30 次，每次等 500 ms。 */
	if (strcmp(cmd, "AT") == 0) {
		if (fail_at_times > 0) {
			fail_at_times--;
			return;   /* 不回应 —— 模组卡死的样子就是这样 */
		}
		reply("OK");
		return;
	}

	/* 附着查询。`,1` = 本地注册（modem.c 认 ",1" 和 ",5"）。 */
	if (strcmp(cmd, "AT+CGREG?") == 0) {
		if (fail_attach_times > 0) {
			fail_attach_times--;
			reply("+CGREG: 0,0");   /* 未注册 */
		} else {
			reply("+CGREG: 0,1");
		}
		reply("OK");
		return;
	}

	if (strcmp(cmd, "AT+CCLK?") == 0) {
		reply("+CCLK: \"25/06/15,12:00:00+00\"");
		reply("OK");
		return;
	}

	/* TCP/TLS 连接。"CONNECT OK" 是带外 URC 而不是命令应答（§8.5）。 */
	if (strncmp(cmd, "AT+SSLMIPSTART", 14) == 0) {
		if (fail_session_times > 0) {
			fail_session_times--;
			reply("ERROR");
		} else {
			reply("CONNECT OK");
		}
		return;
	}
	if (strncmp(cmd, "AT+MCONNECT", 11) == 0) {
		reply("CONNACK OK");
		return;
	}
	if (strncmp(cmd, "AT+MSUB", 7) == 0) {
		reply("SUBACK");
		return;
	}

	/* 其余一律 OK：ATE0 / AT+IPR / AT+CFGRI / AT^WAKEUPHEX / AT+SSLCFG /
	 * AT+MCONFIG / AT+MQTTMODE / AT+MDISCONNECT / AT+CIPSHUT / AT+CFUN。 */
	reply("OK");
}

/* 假模组线程：把 TX 缓冲里的字节攒成行，逐行喂给 respond()。 */
static void fake_modem_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	char line[128];
	size_t n = 0;

	while (true) {
		uint8_t byte;

		if (uart_emul_get_tx_data(euart, &byte, 1) != 1) {
			k_sleep(K_MSEC(2));
			continue;
		}
		if (byte == '\r') {
			continue;
		}
		if (byte == '\n') {
			if (n > 0) {
				line[n] = '\0';
				respond(line);
				n = 0;
			}
			continue;
		}
		if (n + 1 < sizeof(line)) {
			line[n++] = (char)byte;
		}
	}
}

K_THREAD_DEFINE(fake_modem, 2048, fake_modem_fn, NULL, NULL, NULL, 5, 0, 0);

/* --- 脚手架 ------------------------------------------------------------------ */

static void *setup(void)
{
	zassert_true(device_is_ready(euart), "uart-emul 没就绪");
	k_mutex_init(&trace_lock);
	zassert_ok(modem_init(NULL), "modem_init 失败");
	return NULL;
}

/* 每个用例都从「已连接」开始。 */
static void before(void *f)
{
	ARG_UNUSED(f);
	inject(0, 0, 0);
	trace_reset();
	zassert_ok(modem_connect(), "前置的 modem_connect 应该成功");
	zassert_true(modem_is_connected(), "前置连接后应该是 connected");
	trace_reset();   /* 只看重连本身发了什么 */
}

ZTEST_SUITE(modem_reconnect, NULL, setup, before, NULL, NULL);

/* --- 1 级：只重建 MQTT 会话 --------------------------------------------------- */

/* 这是最重要的一条：**MQTT 掉线不该重启模组、也不该重新附着**。
 * 一个「无脑走 3 级」的实现在这里会红。 */
ZTEST(modem_reconnect, test_level1_only_rebuilds_session)
{
	zassert_ok(modem_reconnect(), "1 级重连应该成功");
	zassert_true(modem_is_connected(), "重连后应该是 connected");

	if (trace_count_exact("AT") != 0 || trace_count("AT+CGREG?") != 0) {
		trace_dump();
	}

	/* 没有重启模组：裸 AT（开机训练）一次都没发。 */
	zassert_equal(trace_count_exact("AT"), 0,
		      "1 级重连重启了模组 —— 白花 30~90 s 全功率");
	/* 没有重新附着：CGREG 一次都没查。 */
	zassert_equal(trace_count("AT+CGREG?"), 0,
		      "1 级重连重跑了附着 —— 白花最坏 60 s");
	/* 也没有拆承载。 */
	zassert_equal(trace_count("AT+CIPSHUT"), 0,
		      "1 级重连拆了 PDP 承载 —— 那是 2 级的动作");

	/* 确实重建了会话：先拆旧的，再连新的。 */
	zassert_equal(trace_count("AT+MDISCONNECT"), 1,
		      "没先拆掉旧的 MQTT 会话 —— 模组会用「已有连接」拒掉新的");
	zassert_equal(trace_count("AT+SSLMIPSTART"), 1, "没重连 TCP/TLS");
	zassert_equal(trace_count("AT+MSUB"), 1,
		      "没重新订阅下行 —— 会话是新的，订阅不会自己回来");

	/* 顺序：拆在连之前。 */
	zassert_true(trace_first("AT+MDISCONNECT") < trace_first("AT+SSLMIPSTART"),
		     "先连后拆了，顺序反了");
}

/* --- 2 级：PDP 重拨 ----------------------------------------------------------- */

/* 1 级失败一次就该升到 2 级：拆承载 + 重新附着 + 重建会话，
 * 但**仍然不重启模组**。 */
ZTEST(modem_reconnect, test_level2_redials_pdp_without_restart)
{
	inject(1, 0, 0);   /* 让 SSLMIPSTART 失败一次 */

	zassert_ok(modem_reconnect(), "2 级重连应该成功");
	zassert_true(modem_is_connected(), "重连后应该是 connected");

	if (trace_count_exact("AT") != 0) {
		trace_dump();
	}

	/* 关键：升到 2 级但没升到 3 级。 */
	zassert_equal(trace_count_exact("AT"), 0,
		      "2 级重连重启了模组 —— 越级了");
	zassert_equal(trace_count("AT+CPOWD"), 0,
		      "2 级重连关机了 —— 那是 3 级的动作");

	/* 2 级该做的三件事。 */
	zassert_equal(trace_count("AT+CIPSHUT"), 1, "没拆 PDP 承载");
	zassert_true(trace_count("AT+CGREG?") >= 1, "没重新附着");
	/* 一次 1 级失败 + 一次 2 级成功 = 两次 SSLMIPSTART。 */
	zassert_equal(trace_count("AT+SSLMIPSTART"), 2,
		      "SSLMIPSTART 次数不对（1 级失败 1 次 + 2 级成功 1 次）");

	/* 顺序：CIPSHUT 在重新附着之前。 */
	zassert_true(trace_first("AT+CIPSHUT") < trace_first("AT+CGREG?"),
		     "先附着后拆承载了，顺序反了");
}

/* --- 3 级：模组软重启 --------------------------------------------------------- */

/* 1 级和 2 级都失败才该重启模组。
 * `inject(3, ...)` 让 SSLMIPSTART 一直失败到 3 级之后才好。
 *
 * ⚠ 3 级的命令是 `AT+CFUN=1,1`（软重启），**不是** AT+CPOWD：
 * PWRKEY 已在硬件上接地，关掉的模组没人能开回来（手册明写「上电开机
 * 模式下无法关机」），CPOWD 这条路整个不该出现。 */
ZTEST(modem_reconnect, test_level3_restarts_modem)
{
	/* 1 级失败 1 次、2 级失败 1 次，第 3 级里的那次成功 → 注入 2 次失败。 */
	inject(2, 0, 0);

	zassert_ok(modem_reconnect(), "3 级重连应该成功");
	zassert_true(modem_is_connected(), "重连后应该是 connected");

	if (trace_count_exact("AT") == 0) {
		trace_dump();
	}

	/* 3 级的标志：软重启 + 重新开机训练 + 完整重跑。
	 * ⚠ 断言的是 AT+CFUN=1,1，用前缀 "AT+CFUN" 就够 —— 那个命令
	 * 只在这一处出现。 */
	zassert_equal(trace_count("AT+CFUN"), 1, "没发软重启 AT+CFUN=1,1");
	zassert_equal(trace_count("AT+CPOWD"), 0,
		      "PWRKEY 已接地 —— CPOWD 关掉的模组开不回来，"
		      "这条路必须整个不存在");
	zassert_true(trace_count_exact("AT") >= 1,
		     "没重新做开机波特率训练 —— 模组没真的重启");
	zassert_true(trace_count("AT^WAKEUPHEX") >= 1,
		     "重启后没重配 WAKEUPHEX —— 那个配置随重启丢了");

	/* 顺序：软重启在**重新开机**之前。
	 *
	 * ⚠ 这里不能拿 `AT+CGREG?` 当基准。第一版写的是
	 * `trace_first("AT+CPOWD") < trace_first("AT+CGREG?")`，测试红了 ——
	 * 而**代码是对的、断言是错的**：2 级已经跑过一次 stage_attach，
	 * 所以 trace 里第一条 CGREG 来自 2 级，本来就在 3 级动作的前面。
	 * 正确的基准是重启后那次开机训练（裸 `AT`），它只可能出现在 3 级。 */
	zassert_true(trace_first("AT+CFUN") < trace_first_exact("AT"),
		     "软重启不在重新开机训练之前，顺序反了");
}

/* --- 全败：不能死循环 --------------------------------------------------------- */

/* 三级都失败必须**返回错误**，不能在里面无限重试 ——
 * 那 90 秒一轮的全功率重试会把车电池抽干，而设备没有独立电源（§4.4）。
 *
 * 判据是「函数返回了，且 SSLMIPSTART 的次数有上界」。若实现改成死循环，
 * 这个用例会挂在 ztest 的超时上，也算红。 */
ZTEST(modem_reconnect, test_all_levels_fail_gives_up)
{
	inject(99, 0, 0);   /* SSLMIPSTART 永远失败 */

	zassert_not_equal(modem_reconnect(), 0, "全败时应该返回错误");
	zassert_false(modem_is_connected(), "全败后不该还是 connected");

	int tries = trace_count("AT+SSLMIPSTART");

	if (tries > 3) {
		trace_dump();
	}
	/* 三级各试一次 = 3 次。给到 3 是精确值，不是上界估计。 */
	zassert_equal(tries, 3,
		      "SSLMIPSTART 试了 %d 次 —— 阶梯应该恰好三级，不能重试循环",
		      tries);
}

/* --- 掉线 URC 要能触发状态变化 ------------------------------------------------- */

/* 阶梯是由 `modem_is_connected()` 变 false 驱动的，而那个标志是掉线 URC 清的。
 * 这一条钉住那条链：`+PDP DEACT` → connected=false。
 *
 * 顺带钉住 `connected` 必须是跨线程可见的：URC 在 poll 线程里清，
 * 上报线程里读。原来是裸 bool，编译器有权把它缓存在寄存器里。 */
ZTEST(modem_reconnect, test_disconnect_urc_clears_connected)
{
	zassert_true(modem_is_connected(), "前置状态应该是 connected");

	/* 模组主动报承载被拆。 */
	reply("+PDP DEACT");
	k_sleep(K_MSEC(50));
	(void)modem_poll(100);

	zassert_false(modem_is_connected(),
		      "+PDP DEACT 之后 connected 没被清掉 —— 上层永远不会去重连");
}
