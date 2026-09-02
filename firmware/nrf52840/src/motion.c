/* LIS2DW12 运动唤醒。见 motion.h 的两条警告。 */

#include "motion.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(motion, CONFIG_EBIKE_LOG_LEVEL);

static const struct device *const accel = DEVICE_DT_GET(DT_NODELABEL(lis2dw12));
static motion_cb user_cb;
static enum motion_state state = MOTION_UNKNOWN;
static uint16_t last_mg;

/* 触发回调。跑在驱动自己的线程上（CONFIG_LIS2DW12_TRIGGER_OWN_THREAD=y），
 * 所以可以做稍慢的事，但别阻塞太久 —— 阻塞期间的中断会丢。 */
static void trigger_handler(const struct device *dev,
			    const struct sensor_trigger *trig)
{
	enum motion_state next = state;

	switch ((int)trig->type) {
	case SENSOR_TRIG_MOTION:
		next = MOTION_MOVING;
		/* 读一次加速度，把触发幅度带给上层（契约 §5.4 的 d.mg）。
		 * 失败不影响状态判断 —— 幅度只是诊断信息。 */
		if (sensor_sample_fetch(dev) == 0) {
			struct sensor_value v[3];
			if (sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, v) == 0) {
				/* m/s² → mg，取三轴里最大的分量 */
				int32_t max_mg = 0;
				for (int i = 0; i < 3; i++) {
					int32_t mg = (v[i].val1 * 1000 +
						      v[i].val2 / 1000) * 1000 / 9807;
					if (mg < 0) {
						mg = -mg;
					}
					if (mg > max_mg) {
						max_mg = mg;
					}
				}
				last_mg = (uint16_t)max_mg;
			}
		}
		break;

	case SENSOR_TRIG_STATIONARY:
		next = MOTION_STILL;
		last_mg = 0;
		break;

	default:
		return;
	}

	if (next != state) {
		state = next;
		LOG_INF("运动状态 → %s (%u mg)",
			state == MOTION_MOVING ? "moving" : "still", last_mg);
		if (user_cb) {
			user_cb(state, last_mg);
		}
	}
}

int motion_set_threshold_mg(uint16_t mg)
{
	/* 一格 31.25 mg @±2 g，所以低于 31 的值会被驱动截成 0 = 一直触发。 */
	if (mg < 31) {
		LOG_ERR("阈值 %u mg 小于一格（31.25 mg），会导致持续触发", mg);
		return -EINVAL;
	}

	struct sensor_value val = {
		.val1 = mg / 1000,
		.val2 = (mg % 1000) * 1000,
	};
	int rc = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ,
				 SENSOR_ATTR_UPPER_THRESH, &val);
	if (rc != 0) {
		LOG_ERR("设阈值失败 rc=%d", rc);
		return rc;
	}
	LOG_INF("运动阈值 = %u mg", mg);
	return 0;
}

enum motion_state motion_current(void)
{
	return state;
}

int motion_init(motion_cb cb)
{
	if (!device_is_ready(accel)) {
		/* 传感器挂了整机还能用（NFC 开锁不依赖它），但省电全没了 ——
		 * 因为唤醒源没了，只能靠定时器醒。所以这里不 fatal，只是大声报。 */
		LOG_ERR("LIS2DW12 未就绪 —— 运动唤醒不可用，功耗会显著上升");
		return -ENODEV;
	}
	user_cb = cb;

	int rc = motion_set_threshold_mg(CONFIG_EBIKE_MOTION_THRESHOLD_MG);
	if (rc != 0) {
		return rc;
	}

	static struct sensor_trigger trig_motion = {
		.type = SENSOR_TRIG_MOTION,
		.chan = SENSOR_CHAN_ACCEL_XYZ,
	};
	rc = sensor_trigger_set(accel, &trig_motion, trigger_handler);
	if (rc != 0) {
		LOG_ERR("挂 MOTION 触发失败 rc=%d", rc);
		return rc;
	}

	/* STATIONARY 需要 CONFIG_LIS2DW12_SLEEP=y。
	 * ⚠ 如果这里返回 -ENOTSUP，很可能就是 §3.7 那个 INT1/INT2 矛盾的实证：
	 * 驱动把 sleep 变化事件路由到了 INT2，而我们只接了 INT1。 */
	static struct sensor_trigger trig_still = {
		.type = SENSOR_TRIG_STATIONARY,
		.chan = SENSOR_CHAN_ACCEL_XYZ,
	};
	rc = sensor_trigger_set(accel, &trig_still, trigger_handler);
	if (rc != 0) {
		LOG_WRN("挂 STATIONARY 触发失败 rc=%d —— "
			"检查 INT2 是否需要单独接线（DESIGN.md §3.7）", rc);
		/* 不返回错误：MOTION 单独也能工作，静止判定退回软件计时。 */
	}

	LOG_INF("运动检测就绪");
	return 0;
}
