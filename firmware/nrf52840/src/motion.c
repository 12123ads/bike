/* LIS2DW12 运动唤醒 + 软件静止判定。见 motion.h 的两条警告。 */

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

/* --- 静止判定：软件计时，不用 STATIONARY 事件 --------------------------------
 *
 * **为什么不用硬件的 STATIONARY**：它走 INT2
 * （`ctrl5_int2_pad_ctrl.int2_sleep_chg`，lis2dw12_trigger.c:92-98），
 * 而本板只接了 INT1。`sensor_trigger_set(STATIONARY)` 会**返回 0**
 * （驱动只写寄存器，不检查引脚接没接），但事件永远不会到达。
 *
 * 上一版就是这么挂着的，后果不是「少一个事件」而是一条**静默失效的保证**：
 * `main.c` 的 `moving` 只在 `MOTION_STILL` 分支里被清掉，所以
 * **车动过一次之后 `moving` 永远是 true，BLE 广播就永远关不掉** ——
 * DESIGN.md §2.4 那条「静止 5 分钟后关广播，理由是防跟踪」直接死掉，
 * 而日志里什么都看不出来（挂载那次调用返回 0，还打了一行 INFO）。
 *
 * 现在改成显式计时：每来一次 MOTION 就把定时器往后推，超时没再动就判静止。
 * 这是 motion.h 里列的出路 (c)，也是唯一不需要改硬件的那条。
 *
 * ⚠ 定时器必须在**每次** MOTION 事件都重推，不能只在状态跳变时推 ——
 * 持续运动期间状态一直是 MOVING、不跳变，只在跳变时推的话第一次
 * 超时就会在骑行中途误判静止。
 */
static struct k_work_delayable still_work;

#define STILL_AFTER K_SECONDS(CONFIG_EBIKE_STILL_AFTER_S)

/* 状态跳变的唯一出口。跑在两个上下文里：驱动线程（MOTION）和
 * 系统工作队列（静止超时），所以要一把锁保证 `state`/`last_mg`
 * 与回调调用是一致的一组。 */
static struct k_mutex state_lock;

static void set_state(enum motion_state next, uint16_t mg)
{
	bool changed = false;

	k_mutex_lock(&state_lock, K_FOREVER);
	if (next != state) {
		state = next;
		last_mg = mg;
		changed = true;
	}
	k_mutex_unlock(&state_lock);

	if (!changed) {
		return;
	}
	LOG_INF("运动状态 → %s (%u mg)",
		next == MOTION_MOVING ? "moving" : "still", mg);
	if (user_cb) {
		user_cb(next, mg);
	}
}

/* 静止超时。跑在系统工作队列上 —— 回调里会走到 `uplink_queue_event()`，
 * 那条路可能落一次 flash（`nvstore_next_q()` 的 q_ceiling 批量写）。
 * 这和 `nvstore.c` 的 `counter_work` 是同一个队列、同一类动作，
 * 所以不是新引入的风险。 */
static void still_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	/* 从 UNKNOWN 也允许跳到 STILL：开机后一直没动过，
	 * 那「静止」就是已知事实，报上去比留着 UNKNOWN 有用。 */
	set_state(MOTION_STILL, 0);
}

/* 触发回调。跑在驱动自己的线程上（CONFIG_LIS2DW12_TRIGGER_OWN_THREAD=y），
 * 所以可以做稍慢的事，但别阻塞太久 —— 阻塞期间的中断会丢。
 *
 * 只处理 MOTION。**没有 STATIONARY 分支** —— 那个事件在本板上永远不会到达
 * （只接了 INT1，见文件顶部）。静止由 still_work 判。 */
static void trigger_handler(const struct device *dev,
			    const struct sensor_trigger *trig)
{
	if ((int)trig->type != SENSOR_TRIG_MOTION) {
		/* 别的触发类型不该出现（motion_init 只挂了 MOTION）。
		 * 出现了说明有人加了新触发却没加处理，报出来。 */
		LOG_WRN("收到未预期的触发类型 %d，忽略", (int)trig->type);
		return;
	}

	uint16_t mg = 0;

	/* 读一次加速度，把触发幅度带给上层（契约 §5.4 的 d.mg）。
	 * 失败不影响状态判断 —— 幅度只是诊断信息。 */
	if (sensor_sample_fetch(dev) == 0) {
		struct sensor_value v[3];

		if (sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, v) == 0) {
			/* m/s² → mg，取三轴里最大的分量 */
			int32_t max_mg = 0;

			for (int i = 0; i < 3; i++) {
				int32_t axis = (v[i].val1 * 1000 +
						v[i].val2 / 1000) * 1000 / 9807;
				if (axis < 0) {
					axis = -axis;
				}
				if (axis > max_mg) {
					max_mg = axis;
				}
			}
			mg = (uint16_t)max_mg;
		}
	}

	/* **每次**都重推，不只在状态跳变时 —— 骑行途中状态一直是 MOVING
	 * 不跳变，只在跳变时推会在第一个超时点误判静止。
	 * reschedule 而不是 schedule：已排程的会被推后，不会叠加。 */
	(void)k_work_reschedule(&still_work, STILL_AFTER);

	set_state(MOTION_MOVING, mg);
}

int motion_set_threshold_mg(uint16_t mg)
{
	/* 一格 31.25 mg @±2 g，所以低于一格的值会被驱动截成 0 = 一直触发。 */
	if (mg < 31) {
		LOG_ERR("阈值 %u mg 小于一格（31.25 mg），会导致持续触发", mg);
		return -EINVAL;
	}

	/* ⚠ `SENSOR_ATTR_UPPER_THRESH` 的单位是 **m/s²**，不是 mg。
	 * 驱动里是 `sensor_ms2_to_mg(val)`（lis2dw12.c:240），它按
	 * `(val1*1e6 + val2) * 1000 / SENSOR_G` 换算，SENSOR_G = 9806650。
	 * 直接把 mg 塞进 val1/val2 会**小 9.8 倍**：150 mg 传进去变成 15 mg，
	 * 而 15 mg 低于一格 → 寄存器写 0 → 传感器一直在报运动。
	 * 用 `sensor_ug_to_ms2()` 做转换（mg → µg 要 ×1000）。 */
	struct sensor_value val;
	sensor_ug_to_ms2((int32_t)mg * 1000, &val);

	int rc = sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ,
				 SENSOR_ATTR_UPPER_THRESH, &val);
	if (rc != 0) {
		LOG_ERR("设阈值失败 rc=%d", rc);
		return rc;
	}
	LOG_INF("运动阈值 = %u mg（%d.%06d m/s²）", mg, val.val1, val.val2);
	return 0;
}

enum motion_state motion_current(void)
{
	k_mutex_lock(&state_lock, K_FOREVER);
	enum motion_state s = state;
	k_mutex_unlock(&state_lock);
	return s;
}

void motion_note_activity(void)
{
	(void)k_work_reschedule(&still_work, STILL_AFTER);
}

int motion_init(motion_cb cb)
{
	if (!device_is_ready(accel)) {
		/* 传感器挂了整机还能用（BLE 开锁不依赖它），但省电全没了 ——
		 * 因为唤醒源没了，只能靠定时器醒。所以这里不 fatal，只是大声报。 */
		LOG_ERR("LIS2DW12 未就绪 —— 运动唤醒不可用，功耗会显著上升");
		return -ENODEV;
	}
	user_cb = cb;
	k_mutex_init(&state_lock);
	k_work_init_delayable(&still_work, still_work_fn);

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
		/* MOTION 是唯一的唤醒源，它挂不上就没有省电模式可言。
		 * 最可能的原因是 CONFIG_LIS2DW12_WAKEUP 没开 —— 驱动里
		 * `case SENSOR_TRIG_MOTION` 整段裹在那个 ifdef 里，会回 -ENOTSUP。 */
		LOG_ERR("挂 MOTION 触发失败 rc=%d（检查 CONFIG_LIS2DW12_WAKEUP）", rc);
		return rc;
	}

	/* ⚠ **刻意不挂 `SENSOR_TRIG_STATIONARY`。**
	 *
	 * 它走 INT2（`ctrl5_int2_pad_ctrl.int2_sleep_chg`，
	 * lis2dw12_trigger.c:92-98），而本板只接了 INT1。
	 * `sensor_trigger_set()` 会**返回 0**（驱动只写寄存器不检查引脚），
	 * 于是上一版留下了一个「配置成功、功能静默失效」的坑：
	 * `main.c` 的 `moving` 只在 STILL 分支清，事件永不到达 →
	 * 车动过一次之后广播永远关不掉（DESIGN.md §2.4 的防跟踪保证失效）。
	 *
	 * 挂它唯一的作用是多写一个寄存器位，代价是让读代码的人以为
	 * 硬件静止检测是可用的。所以整段删掉，静止改由 still_work 判。
	 *
	 * 要换回硬件方案得先动硬件：接第二根线到 INT2（§3.5 有 4 个余量脚），
	 * 或越过驱动写 `CTRL_REG7.int2_on_int1`（Zephyr 驱动从不调它，
	 * 也没有对应 DTS 属性）。见 motion.h 的出路 (a)/(b)。
	 *
	 * 开机先排一次超时：如果开机后一直没动过，
	 * STILL_AFTER 之后会报一次 still，而不是永远停在 UNKNOWN。 */
	(void)k_work_reschedule(&still_work, STILL_AFTER);

	LOG_INF("运动检测就绪（静止判定：软件计时 %d s）",
		CONFIG_EBIKE_STILL_AFTER_S);
	return 0;
}
