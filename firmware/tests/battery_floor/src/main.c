/*
 * 电池采样合理性下限的测试。
 *
 * 被测的是 firmware/nrf52840/src/battery.c 的原件。
 *
 * 为什么必须有这个测试：整数换算链 `(raw * 600 * 6 >> 12) * 21` 里，
 * **一个 ADC LSB = 370 mV 电池电压**。原来的读失败判据 `mv <= 0` 只有
 * 一个 LSB 宽 —— `raw = 2`（满量程的 0.05%，在悬空/断线的高阻输入上是常态）
 * 就换算成 21 mV，命中 `battery_low_level()` 的第 3 级，上报线程随即
 * `enter_system_off()`（main.c:266）。
 *
 * 症状是**假欠压**：车电池好好的，车却自己进 System OFF，摇一下醒来
 * 报一轮又睡回去。日志和 lowbatt 报文都写着「电池空了」，真实故障却在
 * 采样链上。这类「读数是假的但下游当真值用」只有把真实的坏读数喂进
 * 完整链路才看得出来 —— 所以测试走真的 ADC API + 真的 GPIO 门控，
 * 而不是直接调 battery.c 的内部函数。
 *
 * `raw` 与毫伏的对应（adc-emul 用 `input_mV * res_mask / (ref/gain)` 反推）：
 * 注入 pin 电压 → raw → 电池毫伏，三段都在下面的辅助函数里算过一遍。
 */

#include "battery.h"

#include <errno.h>
#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/ztest.h>

#define ADC_NODE  DT_NODELABEL(adc0)
#define ADC_CHAN  0

static const struct device *const adc = DEVICE_DT_GET(ADC_NODE);

static const struct gpio_dt_spec gate =
	GPIO_DT_SPEC_GET(DT_NODELABEL(batt_gate), gpios);

/* --- 换算辅助 ----------------------------------------------------------------
 *
 * 三个常量必须与 overlay / 真固件一致，写死在这里是**故意的**：如果哪天
 * overlay 的分压比或 gain 变了，这些断言会红，那正是要提醒的事。
 */
#define ADC_RES_BITS 12
#define ADC_RES_MASK ((1U << ADC_RES_BITS) - 1U)
#define ADC_REF_MV   600
#define ADC_GAIN_INV 6
#define DIV_FULL_OHMS (4700000 + 4700000 + 470000)
#define DIV_OUT_OHMS  470000

/** 电池毫伏 → 应该注入 adc-emul 的原始码（走 raw 接口，跳过 emul 自己的换算）。 */
static uint32_t batt_mv_to_raw(uint32_t batt_mv)
{
	/* batt_mv = (raw * REF * GAIN >> RES) * FULL / OUT  的逆运算 */
	uint64_t pin_mv = (uint64_t)batt_mv * DIV_OUT_OHMS / DIV_FULL_OHMS;
	uint64_t raw = (pin_mv << ADC_RES_BITS) / ((uint64_t)ADC_REF_MV * ADC_GAIN_INV);

	return (uint32_t)(raw > ADC_RES_MASK ? ADC_RES_MASK : raw);
}

/** 原始码 → battery.c 会算出来的电池毫伏。断言用它，不用手写魔数。 */
static int32_t raw_to_batt_mv(uint32_t raw)
{
	int32_t pin_mv = (int32_t)((raw * ADC_REF_MV * ADC_GAIN_INV) >> ADC_RES_BITS);

	return (int32_t)((int64_t)pin_mv * DIV_FULL_OHMS / DIV_OUT_OHMS);
}

static void set_raw(uint32_t raw)
{
	zassert_ok(adc_emul_const_raw_value_set(adc, ADC_CHAN, raw),
		   "注入 raw=%u 失败", raw);
}

static void set_batt_mv(uint32_t batt_mv)
{
	set_raw(batt_mv_to_raw(batt_mv));
}

/* --- 脚手架 ------------------------------------------------------------------ */

static void *setup(void)
{
	zassert_true(device_is_ready(adc), "adc-emul 没就绪");
	zassert_ok(battery_init(), "battery_init 失败 —— "
		   "overlay 的 ref-internal-mv 给了吗？（0 会让 channel_setup 回 -ENOTSUP）");
	return NULL;
}

ZTEST_SUITE(battery_floor, NULL, setup, NULL, NULL, NULL);

/* --- 核心：raw=2 不能被当成「电池耗尽」 ---------------------------------------- */

/* **这是修复前会红的那一条。** raw=2 是满量程的 0.05%，在断线的高阻输入上
 * 完全正常，但换算出 21 mV —— 旧代码的 `mv <= 0` 挡不住，于是判第 3 级、
 * 主动进 System OFF。车是好的，行为却是「电池空了」。 */
ZTEST(battery_floor, test_two_lsb_is_not_deep_undervoltage)
{
	set_raw(2);

	int mv = battery_read_mv();

	zassert_true(mv < 0,
		     "raw=2 换算出 %d mV，battery_read_mv 却当成有效读数返回了 —— "
		     "它会被判第 3 级并主动进 System OFF（main.c:266），"
		     "而车电池是好的", raw_to_batt_mv(2));
	zassert_equal(battery_low_level(mv), 0,
		      "raw=2 被判成欠压等级 %d —— 假欠压会让车自己变半死",
		      battery_low_level(mv));
}

/* 悬空读到 0 的那一档。这一条修复前就是绿的，留着是为了钉住
 * 「0 和 2 现在走同一条路」——	而不是靠 `mv <= 0` 那个巧合。 */
ZTEST(battery_floor, test_zero_reading_is_a_failure_not_undervoltage)
{
	set_raw(0);

	int mv = battery_read_mv();

	zassert_true(mv < 0, "raw=0 应该当读失败，实际返回 %d", mv);
	zassert_equal(battery_low_level(mv), 0, "读失败不能当欠压");
}

/* --- 下限的边界：19.99 V 拒、20.00 V 收 --------------------------------------- */

/* 下限本身必须是**可观测的边界**，不是一句注释。这两条把它钉在 20 V。
 * 一个 LSB = 370 mV，所以边界两侧各取一个整 LSB 的距离，不会因为
 * 整数截断而落到同一个 raw 上。 */
ZTEST(battery_floor, test_just_below_floor_rejected)
{
	/* 找到「换算结果 < 20000」的最大 raw */
	uint32_t raw = batt_mv_to_raw(20000);

	while (raw > 0 && raw_to_batt_mv(raw) >= 20000) {
		raw--;
	}
	zassert_true(raw_to_batt_mv(raw) < 20000, "取样点没落在下限以下");

	set_raw(raw);
	int mv = battery_read_mv();

	zassert_true(mv < 0, "%d mV（raw=%u）在下限以下，应该被拒，实际返回 %d",
		     raw_to_batt_mv(raw), raw, mv);
}

ZTEST(battery_floor, test_at_or_above_floor_accepted)
{
	/* 找到「换算结果 >= 20000」的最小 raw */
	uint32_t raw = batt_mv_to_raw(20000);

	while (raw_to_batt_mv(raw) < 20000) {
		raw++;
	}

	set_raw(raw);
	int mv = battery_read_mv();

	zassert_equal(mv, raw_to_batt_mv(raw),
		      "%d mV（raw=%u）在下限之上，应该原样返回，实际 %d",
		      raw_to_batt_mv(raw), raw, mv);
	zassert_true(mv >= 20000, "返回值 %d 低于下限", mv);
}

/* --- 真实的深度欠压仍然必须触发第 3 级 ----------------------------------------- */

/* 下限不能把 §6 第 4 级（主动 System OFF 保护车电池）废掉。
 * 30 V 远低于第三阈值 42 V，但也远高于 20 V 的合理性下限 ——
 * 它必须照旧判 3。battery.c 里有对应的 BUILD_ASSERT，这里是可执行的那一份。 */
ZTEST(battery_floor, test_real_deep_undervoltage_still_triggers_level_3)
{
	set_batt_mv(30000);

	int mv = battery_read_mv();

	zassert_true(mv > 0, "30 V 是真实的深度欠压，不能被当成读失败（实际 %d）", mv);
	zassert_equal(battery_low_level(mv), 3,
		      "30 V（读到 %d mV）应该判第 3 级，实际 %d —— "
		      "合理性下限把真实欠压吞掉了", mv, battery_low_level(mv));
}

/* --- 三个等级边界照旧生效 ------------------------------------------------------ */

/* 加下限不能动到已有的分级。三条各取一个明确落在区间内的值。 */
ZTEST(battery_floor, test_level_boundaries_unchanged)
{
	struct {
		uint32_t mv;
		int level;
		const char *why;
	} const cases[] = {
		{ 43000, 2, "42~44 V 之间是第 2 级（停周期上报）" },
		{ 45000, 1, "44~46 V 之间是第 1 级（降低上报频率）" },
		{ 50400, 0, "46 V 以上是正常" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		set_batt_mv(cases[i].mv);

		int mv = battery_read_mv();

		zassert_true(mv > 0, "%u mV 应该是有效读数，实际 %d",
			     cases[i].mv, mv);
		zassert_equal(battery_low_level(mv), cases[i].level,
			      "%u mV（读到 %d mV）判成等级 %d，应该是 %d —— %s",
			      cases[i].mv, mv, battery_low_level(mv),
			      cases[i].level, cases[i].why);
	}
}

/* --- 门控纪律：拒掉的那条路也必须关门控 ---------------------------------------- */

/* 漏关一次就是永久多 5.96 µA，和整机 GPIOTE PORT 的 2.36 µA 同一量级，
 * 而且日志里完全看不见（battery.c 的注释）。新增的下限判断插在关门控**之后**，
 * 这一条钉住它没被挪到前面去。 */
ZTEST(battery_floor, test_gate_closed_after_rejected_read)
{
	set_raw(2);
	(void)battery_read_mv();

	zassert_equal(gpio_emul_output_get_dt(&gate), 0,
		      "被下限拒掉的那条路没关门控 —— 永久多耗 5.96 µA，"
		      "而日志里看不见");
}

ZTEST(battery_floor, test_gate_closed_after_successful_read)
{
	set_batt_mv(50400);
	zassert_true(battery_read_mv() > 0, "50.4 V 应该读成功");

	zassert_equal(gpio_emul_output_get_dt(&gate), 0,
		      "成功的那条路没关门控");
}

/* --- 满量程不能溢出 ----------------------------------------------------------- */

/* 58.8 V 是设计上限（PACK_MAX_MV）。它必须读成正常等级，
 * 也顺带证明换算链在满量程附近没有符号或截断问题。 */
ZTEST(battery_floor, test_full_charge_reads_normal)
{
	set_batt_mv(58800);

	int mv = battery_read_mv();

	zassert_true(mv > 46000, "满充读到 %d mV，低于第一阈值", mv);
	zassert_equal(battery_low_level(mv), 0, "满充不该是欠压");
}
