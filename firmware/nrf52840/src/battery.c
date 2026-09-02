/* 电池电压采样。见 battery.h：门控是功耗预算的前提。 */

#include "battery.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/adc/voltage_divider.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(battery, CONFIG_EBIKE_LOG_LEVEL);

#define VBATT_NODE DT_NODELABEL(vbatt)

static const struct gpio_dt_spec gate =
	GPIO_DT_SPEC_GET(DT_NODELABEL(batt_gate), gpios);

/* 分压比不再手算：voltage_divider_scale_dt() 用 int64 做
 * `mv * full_ohms / output_ohms`，非整数比例也不丢精度。
 * 上一版是 `full/output` 的**整数除**，21:1 恰好整除所以无害，
 * 但换成 19.29:1 之类就会被截断成 19 —— 1.5% 的系统误差，
 * 而且是静默的。 */
static const struct voltage_divider_dt_spec vbatt =
	VOLTAGE_DIVIDER_DT_SPEC_GET(VBATT_NODE);

/* ---- 编译期护栏：分压比配错就不让它编过 --------------------------------------
 *
 * 这一段存在的唯一理由：**配错了是烧芯片，而且物理不可逆**。
 * 上一版 overlay 写的是 1M/1M（2:1），58.8 V 会往 P0.31 灌 29.4 V。
 * 那种错误在代码里看不出来、在日志里看不出来、只在焊完通电的瞬间表现为
 * 一颗死掉的 nRF52840。所以它必须是编译错误，不能是注释里的警告。
 */

/** 电池组的最坏情况电压（毫伏）。48 V 铅酸 = 4 × 12 V，满充按 14.7 V/只算。
 *  ⚠ 换电池规格（60 V / 72 V / 锂电）必须改这里，然后重算 overlay 的分压比。 */
#define PACK_MAX_MV 58800

/** 引脚电压上限（毫伏）。本板 REGOUT0 = 3V3 → VDD = 3.3 V
 *  （nRF52840 PS v1.1 Table 172 推荐工作条件）。
 *
 *  注意**不要**用 Table 173 的绝对最大值 VDD+0.3 = 3.6 V 来算：那是
 *  「短时暴露不永久损坏」的极限，不是可用范围。也**不要**用 ADC 满量程
 *  3.6 V（gain 1/6 × 内部参考 600 mV）—— 那是转换器的上限，
 *  跟引脚能承受多少伏是两件无关的事。 */
#define PIN_MAX_MV 3300

/* 电阻 1% 容差的最坏组合：上臂偏小 + 下臂偏大 → 分压比最小 → 引脚电压最高。
 * 用整数算：ratio_worst = (Rtop*99 + Rbot*101) / (Rbot*101)，
 * 代进去就是 PACK_MAX_MV * (Rbot*101) / (Rtop*99 + Rbot*101)。 */
#define R_TOP_OHMS  (DT_PROP(VBATT_NODE, full_ohms) - DT_PROP(VBATT_NODE, output_ohms))
#define R_BOT_OHMS  DT_PROP(VBATT_NODE, output_ohms)

#define PIN_MV_WORST                                                          \
	((uint64_t)PACK_MAX_MV * ((uint64_t)R_BOT_OHMS * 101) /                \
	 ((uint64_t)R_TOP_OHMS * 99 + (uint64_t)R_BOT_OHMS * 101))

/* ⚠ 断言文本必须是纯 ASCII：GCC 把 `_Static_assert` 的消息按字节转义输出，
 * 中文会变成一串 `\37777777745...`，等于没有提示。实测过。 */
BUILD_ASSERT(PIN_MV_WORST <= PIN_MAX_MV,
	     "vbatt divider ratio too low: pack at full charge exceeds VDD on "
	     "AIN7/P0.31 and will destroy the SoC. Fix full-ohms/output-ohms in "
	     "the vbatt node (boards/*.overlay), or PACK_MAX_MV here.");

/* 源阻抗 = Rtop ‖ Rbot，必须 ≤ 800 kΩ，否则 overlay 里那个 40 µs 采集时间
 * 不成立（PS v1.1 §6.23.10.1 的 tACQ 表）。超了不会烧，但读数会偏低，
 * 而且偏多少取决于引脚电容 —— 是个查不出来的读数误差。 */
#define R_SOURCE_OHMS \
	((uint64_t)R_TOP_OHMS * R_BOT_OHMS / DT_PROP(VBATT_NODE, full_ohms))

BUILD_ASSERT(R_SOURCE_OHMS <= 800000,
	     "vbatt source impedance above 800k: the 40us acquisition time in "
	     "the overlay is not enough and readings will be low. Lower the "
	     "resistor values or raise zephyr,acquisition-time.");

/* 三个欠压阈值必须严格递减，否则中间那级永远不会生效（battery.h 的语义）。 */
BUILD_ASSERT(CONFIG_EBIKE_LOW_VOLT_1 > CONFIG_EBIKE_LOW_VOLT_2 &&
	     CONFIG_EBIKE_LOW_VOLT_2 > CONFIG_EBIKE_LOW_VOLT_3,
	     "need EBIKE_LOW_VOLT_1 > EBIKE_LOW_VOLT_2 > EBIKE_LOW_VOLT_3, "
	     "otherwise the middle level never triggers.");

/* 门控打开后等多久再采。Rs = 448 kΩ 配引脚电容（PS v1.1 给的 CSAMPLE 2.5 pF，
 * 加走线和 ESD 结电容按 20 pF 估）的 RC 常数约 9 µs，给 500 µs 是几十倍余量；
 * 这段时间流的 6 µA 可以忽略。
 *
 * 注意 voltage-divider binding 自带 `power-on-sample-delay-us`（默认 100 µs），
 * 但那条路要走上游的 ADC voltage-divider 驱动；本文件自己开关门控
 * （因为要保证「无论采样成不成功都关」），所以那个属性用不上。 */
#define SETTLE_US 500

static bool ready;

int battery_init(void)
{
	if (!adc_is_ready_dt(&vbatt.port)) {
		LOG_ERR("ADC 未就绪");
		return -ENODEV;
	}
	int rc = adc_channel_setup_dt(&vbatt.port);
	if (rc != 0) {
		LOG_ERR("adc_channel_setup 失败 rc=%d", rc);
		return rc;
	}

	if (!gpio_is_ready_dt(&gate)) {
		LOG_ERR("分压门控 GPIO 未就绪");
		return -ENODEV;
	}
	/* 初始必须是关的 —— 上电就打开会一直流 5.96 µA */
	rc = gpio_pin_configure_dt(&gate, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) {
		return rc;
	}

	ready = true;
	return 0;
}

int battery_read_mv(void)
{
	if (!ready) {
		return -EAGAIN;
	}

	int16_t raw = 0;
	struct adc_sequence seq = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};
	int rc = adc_sequence_init_dt(&vbatt.port, &seq);
	if (rc != 0) {
		return rc;
	}

	rc = gpio_pin_set_dt(&gate, 1);
	if (rc != 0) {
		return rc;
	}
	k_busy_wait(SETTLE_US);

	rc = adc_read_dt(&vbatt.port, &seq);

	/* 无论采样成不成功都要关门控。漏关一次就是永久多 5.96 µA —— 和整机
	 * GPIOTE PORT 的 2.36 µA 同一量级，而这个错误在日志里完全看不见，
	 * 只会在几个月后表现为电池掉得比预期快。 */
	int gate_rc = gpio_pin_set_dt(&gate, 0);
	if (gate_rc != 0) {
		LOG_ERR("关分压门控失败 rc=%d —— 会持续多耗 5.96 µA！", gate_rc);
	}

	if (rc != 0) {
		LOG_ERR("adc_read 失败 rc=%d", rc);
		return rc;
	}

	int32_t mv = raw;
	rc = adc_raw_to_millivolts_dt(&vbatt.port, &mv);
	if (rc != 0) {
		return rc;
	}

	int32_t batt_mv = mv;
	rc = voltage_divider_scale_dt(&vbatt, &batt_mv);
	if (rc != 0) {
		/* full-ohms 缺失才会走到这 —— overlay 里写着，正常不可能 */
		LOG_ERR("分压换算失败 rc=%d（overlay 少了 full-ohms？）", rc);
		return rc;
	}

	LOG_DBG("电池 %d mV（ADC %d mV，分压 %u/%u）", batt_mv, mv,
		vbatt.full_ohms, vbatt.output_ohms);
	return (int)batt_mv;
}

int battery_low_level(int mv)
{
	if (mv <= 0) {
		return 0;   /* 读失败不当欠压处理 —— 误报会让人白跑一趟 */
	}
	if (mv < CONFIG_EBIKE_LOW_VOLT_3) {
		return 3;   /* 主动进 System OFF，只留运动唤醒（§6 第 4 级） */
	}
	if (mv < CONFIG_EBIKE_LOW_VOLT_2) {
		return 2;   /* 停周期上报，只保留离线开锁（§6 第 3 级） */
	}
	if (mv < CONFIG_EBIKE_LOW_VOLT_1) {
		return 1;   /* 降低上报频率并上报状态（§6 第 2 级） */
	}
	return 0;
}
