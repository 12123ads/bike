/* 锁执行机构。见 lock.h：型号未定，这里按最保守的假设写。 */

#include "lock.h"

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(lock, CONFIG_EBIKE_LOG_LEVEL);

/* 驱动脉冲时长。电磁锁 = 保持开启的时间；电机锁 = 转一圈的时间。
 * 500 ms 是两种锁都能接受的起始值，装上实物要实测调。 */
#define PULSE_MS 500

static const struct gpio_dt_spec drive =
	GPIO_DT_SPEC_GET(DT_NODELABEL(lock_drive), gpios);
static const struct gpio_dt_spec sense =
	GPIO_DT_SPEC_GET(DT_NODELABEL(lock_sense), gpios);

static struct gpio_callback sense_cb_data;
static lock_state_cb user_cb;
static struct k_work_delayable release_work;
static struct k_work sense_work;

void lock_set_callback(lock_state_cb cb)
{
	user_cb = cb;
}

/* 脉冲结束，撤掉驱动信号。
 * 用延迟工作而不是在 lock_unlock 里 k_sleep：开锁是从 GATT 回调派生的
 * work handler 链上来的，那条路径上阻塞 500 ms 会拖住整条开锁应答。 */
static void release_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	int rc = gpio_pin_set_dt(&drive, 0);
	if (rc != 0) {
		/* 驱动信号撤不掉：电磁锁会一直通电（几百 mA），
		 * 那是几小时内抽空车电池的量级。必须大声报。 */
		LOG_ERR("撤锁驱动信号失败 rc=%d —— 锁可能持续通电！", rc);
	}
}

static void sense_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	bool open = lock_is_open();
	LOG_INF("锁位置反馈：%s", open ? "开" : "关");
	if (user_cb) {
		user_cb(open);
	}
}

static void sense_isr(const struct device *dev, struct gpio_callback *cb,
		      uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	/* 中断里不做别的事 —— 回调可能要发 MQTT，那不能在中断上下文跑 */
	k_work_submit(&sense_work);
}

int lock_unlock(void)
{
	int rc = gpio_pin_set_dt(&drive, 1);
	if (rc != 0) {
		LOG_ERR("发锁驱动信号失败 rc=%d", rc);
		return rc;
	}
	LOG_INF("开锁驱动 %d ms", PULSE_MS);
	k_work_reschedule(&release_work, K_MSEC(PULSE_MS));
	return 0;
}

int lock_lock(void)
{
	/* 电磁锁断电即锁，所以只要确保驱动信号是撤掉的。
	 * 电机锁要反转，那需要半桥 —— 定了型号再加。 */
	(void)k_work_cancel_delayable(&release_work);
	int rc = gpio_pin_set_dt(&drive, 0);
	if (rc != 0) {
		LOG_ERR("上锁失败 rc=%d", rc);
	}
	return rc;
}

bool lock_is_open(void)
{
	if (!gpio_is_ready_dt(&sense)) {
		return false;
	}
	int v = gpio_pin_get_dt(&sense);
	return v > 0;
}

int lock_init(void)
{
	k_work_init_delayable(&release_work, release_work_fn);
	k_work_init(&sense_work, sense_work_fn);

	if (!gpio_is_ready_dt(&drive)) {
		LOG_ERR("锁驱动 GPIO 未就绪");
		return -ENODEV;
	}
	/* 初始必须是「不驱动」—— 上电就通电的话，电磁锁会一直开着 */
	int rc = gpio_pin_configure_dt(&drive, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) {
		return rc;
	}

	if (!gpio_is_ready_dt(&sense)) {
		/* 没接反馈开关也能用，只是不知道锁到底动了没有 */
		LOG_WRN("锁位置反馈 GPIO 未就绪 —— 无法确认锁是否真的动了");
		return 0;
	}
	rc = gpio_pin_configure_dt(&sense, GPIO_INPUT);
	if (rc != 0) {
		return rc;
	}
	/* 两边沿都要：开和关都是要上报的状态变化 */
	rc = gpio_pin_interrupt_configure_dt(&sense, GPIO_INT_EDGE_BOTH);
	if (rc != 0) {
		LOG_ERR("配反馈中断失败 rc=%d", rc);
		return rc;
	}
	gpio_init_callback(&sense_cb_data, sense_isr, BIT(sense.pin));
	rc = gpio_add_callback(sense.port, &sense_cb_data);
	if (rc != 0) {
		return rc;
	}

	LOG_INF("锁就绪，当前状态：%s", lock_is_open() ? "开" : "关");
	return 0;
}
