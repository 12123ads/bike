/* 电池电压采样。见 battery.h：门控是功耗预算的前提。 */

#include "battery.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(battery, CONFIG_EBIKE_LOG_LEVEL);

#define VBATT_NODE DT_NODELABEL(vbatt)

static const struct gpio_dt_spec gate =
	GPIO_DT_SPEC_GET(DT_NODELABEL(batt_gate), gpios);

static const struct adc_dt_spec adc_ch =
	ADC_DT_SPEC_GET_BY_IDX(VBATT_NODE, 0);

/* 分压比：full_ohms / output_ohms = 2M / 1M = 2 */
#define DIVIDER_RATIO \
	(DT_PROP(VBATT_NODE, full_ohms) / DT_PROP(VBATT_NODE, output_ohms))

/* 门控打开后等多久再采。1M 阻抗 + 引脚电容的 RC 常数在几十 µs 量级，
 * 给 500 µs 是宽裕的余量；这段时间流的 29 µA 可以忽略。 */
#define SETTLE_US 500

static bool ready;

int battery_init(void)
{
	if (!adc_is_ready_dt(&adc_ch)) {
		LOG_ERR("ADC 未就绪");
		return -ENODEV;
	}
	int rc = adc_channel_setup_dt(&adc_ch);
	if (rc != 0) {
		LOG_ERR("adc_channel_setup 失败 rc=%d", rc);
		return rc;
	}

	if (!gpio_is_ready_dt(&gate)) {
		LOG_ERR("分压门控 GPIO 未就绪");
		return -ENODEV;
	}
	/* 初始必须是关的 —— 上电就打开会一直流 29 µA */
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
	int rc = adc_sequence_init_dt(&adc_ch, &seq);
	if (rc != 0) {
		return rc;
	}

	rc = gpio_pin_set_dt(&gate, 1);
	if (rc != 0) {
		return rc;
	}
	k_busy_wait(SETTLE_US);

	rc = adc_read_dt(&adc_ch, &seq);

	/* 无论采样成不成功都要关门控。漏关一次就是永久多 29 µA，
	 * 而这个错误在日志里完全看不见 —— 只会在几个月后表现为电池掉得比预期快。 */
	int gate_rc = gpio_pin_set_dt(&gate, 0);
	if (gate_rc != 0) {
		LOG_ERR("关分压门控失败 rc=%d —— 会持续多耗 29 µA！", gate_rc);
	}

	if (rc != 0) {
		LOG_ERR("adc_read 失败 rc=%d", rc);
		return rc;
	}

	int32_t mv = raw;
	rc = adc_raw_to_millivolts_dt(&adc_ch, &mv);
	if (rc != 0) {
		return rc;
	}

	int32_t batt_mv = mv * DIVIDER_RATIO;
	LOG_DBG("电池 %d mV（ADC %d mV，分压比 %d）", batt_mv, mv, DIVIDER_RATIO);
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
