/*
 * 软件静止判定的测试。
 *
 * 被测的是 firmware/nrf52840/src/motion.c 的原件。
 *
 * 为什么必须有这个测试：上一版把 `SENSOR_TRIG_STATIONARY` 挂在 INT2 上，
 * 而本板只接了 INT1 —— `sensor_trigger_set()` 返回 0、日志打了一行 INFO，
 * 但事件永远不会到达。后果不是「少一个事件」，而是一条**静默失效的保证**：
 * `main.c` 的 `moving` 只在 STILL 分支清，于是车动过一次之后 BLE 广播
 * 永远关不掉（DESIGN.md §2.4 的防跟踪保证）。
 *
 * 这一类 bug 只有「走完整条链并观察状态真的翻回来」才抓得到，
 * 所以测试跑真的 LIS2DW12 驱动（挂在 i2c_emul 上的假芯片）+ 真的 GPIO 中断，
 * 而不是直接调 motion.c 的内部函数。
 */

#include "motion.h"

#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

/* --- 假 LIS2DW12（i2c_emul 上的寄存器阵列） ----------------------------------
 *
 * 只需要让驱动的 init 和中断处理跑通，不模拟真实的加速度物理：
 *   WHO_AM_I (0x0F) 必须回 0x44，否则 lis2dw12_power_up 失败、整个 init 失败；
 *   STATUS_DUP (0x37) 起 5 个字节是 all_sources_get 读的（reg.c:585），
 *     其中 ALL_INT_SRC (0x3B) 的 bit1 = wu_ia 决定驱动派发 MOTION；
 *   OUT_X_L (0x28) 起 6 字节是三轴数据，随便给。
 * 其余寄存器一律可读可写，写进去就存着。
 */

#define REG_COUNT       0x40
#define REG_WHO_AM_I    0x0F
#define REG_STATUS_DUP  0x37
#define REG_ALL_INT_SRC 0x3B
#define LIS2DW12_ID     0x44
#define WU_IA_BIT       BIT(1)

static uint8_t fake_regs[REG_COUNT];

/* stmemsc 的 burst read 会把 bit7 当自增标志（stmemsc_i2c.c 的
 * STMEMSC_I2C_ADDR_AUTO_INCR），所以取地址时要把它掩掉。 */
#define REG_ADDR(b) ((b) & 0x7F)

static int fake_transfer(const struct emul *target, struct i2c_msg *msgs,
			 int num_msgs, int addr)
{
	ARG_UNUSED(target);
	ARG_UNUSED(addr);

	if (num_msgs < 1 || msgs[0].len < 1) {
		return -EIO;
	}

	uint8_t reg = REG_ADDR(msgs[0].buf[0]);

	if (num_msgs == 1) {
		/* 写：第一字节是寄存器地址，后面是数据 */
		for (size_t i = 1; i < msgs[0].len; i++) {
			if (reg + i - 1 < REG_COUNT) {
				fake_regs[reg + i - 1] = msgs[0].buf[i];
			}
		}
		return 0;
	}

	/* 读：msgs[0] 是地址，msgs[1] 是数据 */
	for (size_t i = 0; i < msgs[1].len; i++) {
		msgs[1].buf[i] = (reg + i < REG_COUNT) ? fake_regs[reg + i] : 0;
	}
	return 0;
}

static const struct i2c_emul_api fake_api = {
	.transfer = fake_transfer,
};

static struct i2c_emul fake_emul;

static int fake_init(const struct emul *target, const struct device *parent)
{
	memset(fake_regs, 0, sizeof(fake_regs));
	fake_regs[REG_WHO_AM_I] = LIS2DW12_ID;

	fake_emul.target = target;
	fake_emul.api = &fake_api;
	fake_emul.addr = 0x19;
	return i2c_emul_register(parent, &fake_emul);
}

EMUL_DT_DEFINE(DT_NODELABEL(lis2dw12), fake_init, NULL, NULL, &fake_api, NULL);

/* --- 中断注入 ---------------------------------------------------------------- */

static const struct gpio_dt_spec irq =
	GPIO_DT_SPEC_GET(DT_NODELABEL(lis2dw12), irq_gpios);

/* 制造一次运动中断：置 wu_ia，拉一下 INT1。
 *
 * 驱动配的是 `GPIO_INT_EDGE_TO_ACTIVE`（lis2dw12_trigger.c 的
 * init_interrupt 结尾），所以要有一次 0→1 的边沿。 */
static void inject_motion(void)
{
	fake_regs[REG_ALL_INT_SRC] |= WU_IA_BIT;

	(void)gpio_emul_input_set_dt(&irq, 0);
	(void)gpio_emul_input_set_dt(&irq, 1);
	/* 驱动自己的线程（CONFIG_LIS2DW12_TRIGGER_OWN_THREAD=y）要跑起来 */
	k_sleep(K_MSEC(50));
	(void)gpio_emul_input_set_dt(&irq, 0);

	fake_regs[REG_ALL_INT_SRC] &= ~WU_IA_BIT;
}

/* --- 回调记录 ---------------------------------------------------------------- */

#define CB_MAX 16

static struct {
	enum motion_state state;
	uint16_t mg;
} cb_log[CB_MAX];
static size_t cb_n;

static void on_motion(enum motion_state state, uint16_t mg)
{
	if (cb_n < CB_MAX) {
		cb_log[cb_n].state = state;
		cb_log[cb_n].mg = mg;
		cb_n++;
	}
}

static int cb_count(enum motion_state s)
{
	int n = 0;

	for (size_t i = 0; i < cb_n; i++) {
		if (cb_log[i].state == s) {
			n++;
		}
	}
	return n;
}

/* --- 脚手架 ------------------------------------------------------------------ */

/* 测试用的静止超时。prj.conf 里设成 2 秒（真固件默认 30），
 * 否则每个用例都要等半分钟。 */
#define STILL_S CONFIG_EBIKE_STILL_AFTER_S

static void *setup(void)
{
	zassert_true(device_is_ready(DEVICE_DT_GET(DT_NODELABEL(lis2dw12))),
		     "假 LIS2DW12 没就绪 —— WHO_AM_I 回对了吗？");
	zassert_ok(motion_init(on_motion), "motion_init 失败");
	return NULL;
}

ZTEST_SUITE(motion_still, NULL, setup, NULL, NULL, NULL);

/* --- 核心：动过之后必须能自己回到静止 ----------------------------------------- */

/* 这是上一版失效的那条链。**没有这一条，广播就永远关不掉。** */
ZTEST(motion_still, test_returns_to_still_after_timeout)
{
	cb_n = 0;

	inject_motion();
	zassert_equal(motion_current(), MOTION_MOVING,
		      "注入运动中断后状态不是 MOVING —— 中断链没打通");
	zassert_equal(cb_count(MOTION_MOVING), 1, "没回调 MOVING");

	/* 等超时。给 1.5 倍余量 —— 工作队列的调度不是即时的。 */
	k_sleep(K_SECONDS(STILL_S * 3 / 2 + 1));

	zassert_equal(motion_current(), MOTION_STILL,
		      "超时后状态没回到 STILL —— 车动过一次就永远是 moving，"
		      "BLE 广播永远关不掉（DESIGN.md §2.4）");
	zassert_equal(cb_count(MOTION_STILL), 1, "没回调 STILL");
}

/* --- 持续运动不能被误判成静止 -------------------------------------------------- */

/* 骑行途中状态一直是 MOVING、不跳变。如果定时器只在状态跳变时重推，
 * 第一个超时点就会误判静止 —— 于是骑到一半广播被关掉。
 * 这一条要求 `trigger_handler()` **每次**都重推。 */
ZTEST(motion_still, test_sustained_motion_never_goes_still)
{
	cb_n = 0;

	/* 以「小于超时」的间隔连续动 3 个超时周期那么久。 */
	int64_t deadline = k_uptime_get() + STILL_S * 3 * 1000;

	while (k_uptime_get() < deadline) {
		inject_motion();
		zassert_equal(motion_current(), MOTION_MOVING,
			      "持续运动期间被判成静止了 —— 定时器没有每次重推");
		k_sleep(K_MSEC(STILL_S * 1000 / 2));
	}

	zassert_equal(motion_current(), MOTION_MOVING, "结束时应该还是 MOVING");
	zassert_equal(cb_count(MOTION_STILL), 0,
		      "持续运动期间报了 %d 次静止", cb_count(MOTION_STILL));
	/* 状态没跳变，所以 MOVING 只该回调一次（第一次进入时）。 */
	zassert_equal(cb_count(MOTION_MOVING), 1,
		      "MOVING 回调了 %d 次 —— 状态没跳变不该重复回调",
		      cb_count(MOTION_MOVING));
}

/* --- 非加速度来源的活动也要能推后 ---------------------------------------------- */

/* BLE 连上来、有人在开锁 —— 有人正在动这辆车，即使加速度没到阈值，
 * 也不该在这时判静止把广播关掉。 */
ZTEST(motion_still, test_note_activity_defers_still)
{
	cb_n = 0;

	inject_motion();
	zassert_equal(motion_current(), MOTION_MOVING, "前置状态应该是 MOVING");

	/* 在超时前反复用 note_activity 推后，跨过两个超时周期。 */
	for (int i = 0; i < 4; i++) {
		k_sleep(K_MSEC(STILL_S * 1000 / 2));
		motion_note_activity();
		zassert_equal(motion_current(), MOTION_MOVING,
			      "note_activity 没能推后静止判定（第 %d 次）", i);
	}

	/* 停止推后，让它自己超时。 */
	k_sleep(K_SECONDS(STILL_S * 3 / 2 + 1));
	zassert_equal(motion_current(), MOTION_STILL,
		      "停止 note_activity 之后应该能判静止");
}

/* --- 阈值的边界 --------------------------------------------------------------- */

/* 一格 = FS/64 = 31.25 mg @±2 g，低于一格会被驱动截成 0 = 一直触发。
 * motion.c 自己挡了这一条 —— 那是唯一能挡的地方，DTS 里没有这个属性。 */
ZTEST(motion_still, test_threshold_below_one_lsb_rejected)
{
	zassert_not_equal(motion_set_threshold_mg(30), 0,
			  "30 mg 应该被拒 —— 低于一格(31.25 mg)会被截成 0，"
			  "变成持续触发");
	zassert_ok(motion_set_threshold_mg(150), "150 mg 应该被接受");
}
