/*
 * 主流程。
 *
 * 状态机就三态，刻意保持简单 —— 复杂度全在 modem.c 和 unlock.c 里：
 *
 *   ACTIVE   刚被运动唤醒或刚开机。NFC 开着，定时上报。
 *   IDLE     静止一段时间了。NFC 关掉（NFCT ACTIVATED 是 400 µA，§2.5），
 *            只留运动中断和定时器。
 *   OFF      车电池跌破第三阈值（§6 第 4 级）。System OFF，只留运动唤醒；
 *            醒来等于一次复位。见文件末尾的说明。
 *
 * 唤醒源：LIS2DW12 INT1（运动）、定时器（周期上报）、NFC 场（只在 ACTIVE 有效）。
 */

#include "battery.h"
#include "crypto.h"
#include "gnss.h"
#include "lock.h"
#include "modem.h"
#include "motion.h"
#include "nfc_tag.h"
#include "nvstore.h"
#include "proto.h"
#include "unlock.h"
#include "uplink.h"

#include <errno.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/poweroff.h>

LOG_MODULE_REGISTER(main, CONFIG_EBIKE_LOG_LEVEL);

/* 静止多久之后关 NFC 进 IDLE。5 分钟：够用户走开又回来（忘了东西），
 * 也短到不会让 400 µA 白烧太久。 */
#define IDLE_AFTER_STILL_MS (5 * 60 * 1000)

/* 进 System OFF 前要武装的唤醒脚，以及要 suspend 的外设。
 * `motion_int` 与 LIS2DW12 的 `irq-gpios` 是同一根线（overlay 里两处都是
 * P1.11），这里用 alias 拿它是为了绕过传感器驱动、自己重配触发方式。 */
static const struct gpio_dt_spec wake_pin =
	GPIO_DT_SPEC_GET(DT_ALIAS(motion_int), gpios);
static const struct device *const dev_uart_modem = DEVICE_DT_GET(DT_NODELABEL(uart1));
static const struct device *const dev_uart_gnss  = DEVICE_DT_GET(DT_NODELABEL(uart0));
static const struct device *const dev_i2c        = DEVICE_DT_GET(DT_NODELABEL(i2c0));

/* 上报线程。栈要够大 —— modem.c 里有 4 KB 的 HEX 缓冲（那个是静态的），
 * 但 AT 命令拼接和 NMEA 解析都在栈上。 */
#define UPLINK_STACK_SIZE 4096
#define UPLINK_PRIORITY   5

K_THREAD_STACK_DEFINE(uplink_stack, UPLINK_STACK_SIZE);
static struct k_thread uplink_thread;

static volatile bool moving;
static int64_t last_still_ms;

/* --- 回调 ------------------------------------------------------------------- */

static void on_motion(enum motion_state state, uint16_t mg)
{
	if (state == MOTION_MOVING) {
		moving = true;
		char detail[32];
		(void)snprintf(detail, sizeof(detail), "{\"mg\":%u}", mg);
		(void)uplink_queue_event(EV_MOTION, detail);

		/* 动了就把 NFC 打开 —— 用户可能正要开锁 */
		(void)nfc_tag_start();

		/* 车被动了是防盗关心的事件，立刻上报（DESIGN.md §1「防盗感知」） */
		uplink_request_now();
	} else {
		moving = false;
		last_still_ms = k_uptime_get();
		(void)uplink_queue_event(EV_STILL, NULL);
	}
}

static void on_unlock_result(bool ok, uint32_t uid)
{
	char detail[32];
	if (ok) {
		(void)snprintf(detail, sizeof(detail), "{\"uid\":%u}", uid);
		(void)uplink_queue_event(EV_UNLOCK_OK, detail);
		/* 验证通过了才驱动锁。unlock.c 已经做完三重校验。 */
		if (lock_unlock() != 0) {
			LOG_ERR("MAC 验过了但锁没动 —— 检查驱动电路");
		}
	} else {
		/* 契约 §5.4：unlock_deny 不含失败原因 ——
		 * §5.2 说了不给攻击者区分信道，上行也不给。 */
		(void)snprintf(detail, sizeof(detail), "{\"uid\":%u}", uid);
		(void)uplink_queue_event(EV_UNLOCK_DENY, detail);
	}
}

static void on_lock_state(bool open)
{
	char detail[32];
	(void)snprintf(detail, sizeof(detail), "{\"locked\":%s}",
		       open ? "false" : "true");
	(void)uplink_queue_event(EV_LOCK_STATE, detail);
}

/* --- System OFF（DESIGN.md §6 第 4 级） ------------------------------------- */

/* 进 System OFF，只留运动唤醒。**不返回。**
 *
 * 五个步骤的顺序都是必需的，逐条说明：
 *
 * 1. **输出脚拉到安全电平。** sleep pinctrl 只在 SUSPEND 时应用，而 GPIO
 *    输出脚不受 pinctrl 管 —— 不拉低的话 GNSS 门控管和分压门控管会一直导通。
 * 2. **确认唤醒脚已回低。** LIS2DW12 默认是 latched 中断（overlay 没设
 *    `drdy-pulsed`），不读清中断源引脚就一直是高。而 level sense 一武装就会
 *    立刻触发 DETECT → `sys_poweroff()` 之后**立即被唤醒 → 空转 → 再关机**
 *    的活锁。所以先等它回低。
 * 3. **把触发方式从边沿改成电平。** 这是整个函数的关键：
 *    LIS2DW12 驱动配的是 `GPIO_INT_EDGE_TO_ACTIVE`
 *    （`lis2dw12_trigger.c` 的 `lis2dw12_init_interrupt` 结尾），走 GPIOTE
 *    IN 事件通道；**而 GPIOTE 在 System OFF 下是断电的**。
 *    能唤醒 System OFF 的只有 GPIO 的 SENSE → DETECT 信号，
 *    而 Zephyr 只在 level 模式下才写 `PIN_CNF[n].SENSE`
 *    （`gpio_nrfx.c` 的 `get_trigger()` 把 LEVEL 映射成
 *    `NRFX_GPIOTE_TRIGGER_HIGH/LOW`，nrfx 侧再落到 `nrfy_gpio_cfg_sense_set`）。
 *    改成 level 时驱动会顺手把之前占的 GPIOTE channel 释放掉。
 * 4. **suspend EasyDMA 外设。** 产品规格书要求进 System OFF 前所有 EasyDMA
 *    传输必须已完成，而 UARTE/TWIM/SAADC 全是 EasyDMA 外设。
 *    `sys_poweroff()` **自己不做这件事**（它只 irq_lock + 写 SYSTEMOFF），
 *    overlay 里那些 `*_sleep` 的 `low-power-enable` 也**只在 SUSPEND 动作时
 *    才被应用**。不 suspend 的后果是引脚继续驱动/继续上拉：接 Air780EP 那路
 *    尤其糟，模组掉电时 TX 的高电平会往模组里灌电流。
 *    **不要 suspend GPIO 端口本身** —— 那会破坏刚武装好的 SENSE。
 * 5. 复位原因位在开机时已经读过并清掉（uplink.c 的 `reset_cause_str`），
 *    这里不用再动。醒来是一次**完整复位**，RESETREAS.OFF 会被置上，
 *    hwinfo 映射成 `RESET_LOW_POWER_WAKE`，下一轮 hello 会报 `rst:"off"`。
 *
 * ⚠ **调试器接着的时候 System OFF 是被仿真的**（产品规格书：Debug Interface
 * mode 下 System OFF 只是停在一个 `WFE` 循环里），电流测出来是毫安级。
 * 实测功耗必须拔掉调试器冷启动，否则会误判「System OFF 没生效」。
 */
static void enter_system_off(void)
{
	LOG_WRN("电压跌破第三阈值，进 System OFF，只留运动唤醒");

	/* 1. 关掉所有耗电的输出 */
	(void)nfc_tag_stop();
	(void)gnss_power_off();

	/* 2. 等唤醒脚回低，避免 level sense 一武装就自触发 */
	for (int i = 0; i < 100 && gpio_pin_get_dt(&wake_pin) == 1; i++) {
		k_sleep(K_MSEC(10));
	}
	if (gpio_pin_get_dt(&wake_pin) == 1) {
		LOG_ERR("唤醒脚一直是高（LIS2DW12 中断没清？）—— "
			"关机后会立刻被唤醒");
	}

	/* 3. 边沿 → 电平，这是唯一能从 System OFF 唤醒的形式 */
	int rc = gpio_pin_configure_dt(&wake_pin, GPIO_INPUT);
	if (rc == 0) {
		rc = gpio_pin_interrupt_configure_dt(&wake_pin,
						     GPIO_INT_LEVEL_ACTIVE);
	}
	if (rc != 0) {
		/* 武装失败就别关机 —— 关了就再也醒不过来，只能靠拔电池。 */
		LOG_ERR("武装运动唤醒失败 rc=%d —— **放弃进 System OFF**", rc);
		return;
	}

	/* 4. suspend EasyDMA 外设（不含 GPIO 端口） */
	(void)nvstore_flush();
	const struct device *const suspend_list[] = {
		dev_uart_modem, dev_uart_gnss, dev_i2c,
	};
	for (size_t i = 0; i < ARRAY_SIZE(suspend_list); i++) {
		if (!device_is_ready(suspend_list[i])) {
			continue;
		}
		rc = pm_device_action_run(suspend_list[i],
					  PM_DEVICE_ACTION_SUSPEND);
		if (rc != 0 && rc != -EALREADY && rc != -ENOSYS) {
			LOG_WRN("suspend %s 失败 rc=%d（引脚会继续漏电）",
				suspend_list[i]->name, rc);
		}
	}

	sys_poweroff();   /* 不返回 */
}

/* --- 上报线程 --------------------------------------------------------------- */

static void uplink_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* 开机先报一次，让服务端知道设备活了（也顺便拿到下行队列） */
	(void)uplink_queue_event(EV_BOOT, NULL);
	(void)uplink_cycle(true);

	while (true) {
		uint32_t interval = nvstore_report_interval();

		/* 欠压第 1 级：上报周期翻 4 倍（§6 第 2 级「降低上报频率」）。
		 * 不改 nvstore 里的配置值 —— 电压回升后要能自己恢复。 */
		if (uplink_batt_level() == 1) {
			interval *= 4;
		}

		/* 等到周期到，或者被 uplink_request_now() 提前叫起来。
		 * 用信号量而不是 k_sleep：运动事件要能打断等待。 */
		bool asked = (k_sem_take(uplink_now_sem(), K_SECONDS(interval)) == 0);

		if (asked) {
			LOG_INF("提前上报（被请求）");
		} else if (!uplink_should_report()) {
			/* 欠压第 2 级：停周期上报，只保留离线开锁和运动触发。
			 * 手机 NFC 开锁完全离线（§5.2），不受这条影响。 */
			LOG_INF("欠压等级 %d，跳过这一轮周期上报",
				uplink_batt_level());
			continue;
		}

		/* 每轮都带 GNSS：不带的话轨迹就断了。
		 * 将来要省电可以让纯遥测轮跳过定位，但那要先有实测数据支撑
		 * （R8 阶段，DESIGN.md §10）。 */
		(void)uplink_cycle(true);

		/* 欠压第 3 级：主动进 System OFF。放在上报之后 ——
		 * 先把「我要睡了」这条 lowbatt 事件发出去再睡。 */
		if (uplink_batt_level() >= 3) {
			enter_system_off();
			/* 回到这里说明武装唤醒失败，那就继续跑（见 enter_system_off
			 * 里的说明）—— 醒不过来比多耗一点电严重得多。 */
		}

		/* 静止够久就关 NFC 省电。放在上报之后 ——
		 * 上报期间保持 NFC 开着，因为用户可能正好在车边。 */
		if (!moving && nfc_tag_is_active() &&
		    (k_uptime_get() - last_still_ms) > IDLE_AFTER_STILL_MS) {
			LOG_INF("静止超过 %d 分钟，关 NFC 省电（400 µA）",
				IDLE_AFTER_STILL_MS / 60000);
			(void)nfc_tag_stop();
		}

		/* counter 的延迟写窗口是 5 秒，这里主动冲一次 ——
		 * 下一轮之间可能被剪线掉电（没有备份电池，DESIGN.md §6）。 */
		(void)nvstore_flush();
	}
}

/* --- 初始化 ----------------------------------------------------------------- */

int main(void)
{
	LOG_INF("电瓶车定位固件 %s 启动（设备 %s）",
		CONFIG_EBIKE_FW_VERSION, CONFIG_EBIKE_DEVICE_ID);

	/* 顺序有讲究：
	 * crypto 在 unlock 之前（unlock_init 要读密钥并可能用到 PSA）；
	 * nvstore 在 unlock 之前（密钥从 flash 读）；
	 * nfc 在 unlock 之后（NFC 回调会调 unlock_handle_apdu）。 */
	int rc = crypto_init();
	if (rc != 0) {
		/* 没有密码学就没有开锁。这是唯一 fatal 的初始化失败 ——
		 * 其余部件坏了还能降级用，这个坏了核心功能就没了。 */
		LOG_ERR("crypto 初始化失败 rc=%d —— NFC 开锁不可用", rc);
		return rc;
	}

	rc = nvstore_init();
	if (rc != 0) {
		LOG_ERR("nvstore 初始化失败 rc=%d —— 密钥和 counter 无法持久化", rc);
		/* 继续跑：内存里的 counter 仍然能防本次开机内的重放，
		 * 掉电后会退化。比完全不能开锁好。 */
	}

	rc = unlock_init();
	if (rc != 0) {
		LOG_ERR("unlock 初始化失败 rc=%d", rc);
	}
	unlock_set_callback(on_unlock_result);

	rc = lock_init();
	if (rc != 0) {
		LOG_ERR("锁初始化失败 rc=%d —— 验证还能过但锁不会动", rc);
	}
	lock_set_callback(on_lock_state);

	rc = nfc_tag_init();
	if (rc != 0) {
		LOG_ERR("NFC 初始化失败 rc=%d —— 只能用机械钥匙", rc);
	} else {
		/* 开机就开着 NFC，静止 5 分钟后由上报线程关掉 */
		(void)nfc_tag_start();
		last_still_ms = k_uptime_get();
	}

	rc = motion_init(on_motion);
	if (rc != 0) {
		LOG_ERR("运动检测初始化失败 rc=%d —— 功耗会显著上升，"
			"且收不到「车被动了」的告警", rc);
	}

	rc = battery_init();
	if (rc != 0) {
		LOG_ERR("电池采样初始化失败 rc=%d —— 欠压兜底失效（§6）", rc);
	}

	rc = gnss_init();
	if (rc != 0) {
		LOG_ERR("GNSS 初始化失败 rc=%d —— 只能靠基站定位", rc);
	}

	rc = uplink_init();
	if (rc != 0) {
		LOG_ERR("uplink 初始化失败 rc=%d —— 上报不可用", rc);
	}

	k_thread_create(&uplink_thread, uplink_stack, UPLINK_STACK_SIZE,
			uplink_thread_fn, NULL, NULL, NULL,
			UPLINK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&uplink_thread, "uplink");

	LOG_INF("初始化完成");
	return 0;
}

/* --- 关于 System OFF ---------------------------------------------------------
 *
 * DESIGN.md §4.2 的 0.40 µA 是 **System OFF** 的数字。本实现**只在欠压第 3 级
 * 用它**（`enter_system_off()`），日常仍然是 System ON + RTC（3.16 µA）+
 * 定时器唤醒。
 *
 * 为什么日常不用 System OFF：
 *
 * 1. System OFF 唤醒等于**复位**，整个初始化要重跑一遍，MQTT 也要重连。
 *    在省电档下（模组本来就关着）代价不大，但会让 uptime、RAM 里的状态、
 *    以及 modem 会话全部丢掉。
 * 2. **真正的功耗地板是 4G 模组，不是主控**（DESIGN.md §4.1b）。
 *    3.16 µA 和 0.40 µA 的差别（2.8 µA）在 0.5~1.5 mA 的模组面前是噪声。
 *    只有在「模组完全关机」的档位上，这 2.8 µA 才占得上比例。
 * 3. 先把主路径调通再抠这 2.8 µA。R8 阶段（§10）才是做双档功耗的时候。
 *
 * 欠压第 3 级是例外：那时的目标不是省 2.8 µA，而是**别把车电池抽空**，
 * 所以宁可付「唤醒 = 复位」的代价。
 *
 * 两处更正（2026-09-02，核实上游源码后）：
 *
 * - 入口是 **`sys_poweroff()`**（`zephyr/sys/poweroff.h`），**不是**
 *   `pm_state_force(PM_STATE_SOFT_OFF)`。后者在 nRF52 上已经没有实现了：
 *   Zephyr v3.5 的迁移文档明说 Nordic nRF 改用 `sys_poweroff`，
 *   而 `soc/.../nrf52/power.c`（里面那个 `pm_state_set`）在 v3.5 就删掉了。
 *   相应地 **`CONFIG_PM` 在 nRF52 上不能开**（`soc/nordic/nrf52/Kconfig` 只
 *   `select HAS_POWEROFF`，没有 `HAS_PM`），要的是 `CONFIG_POWEROFF=y`。
 * - 唤醒源**不是**「GPIOTE PORT 事件」那个 Kconfig：
 *   `CONFIG_GPIO_NRFX_INTERRUPT_DETECT_MODE_PORT` **这个符号根本不存在**
 *   （`drivers/gpio/Kconfig.nrfx` 里只有 `GPIO_NRFX` 和
 *   `GPIO_NRFX_INTERRUPT`），已从 prj.conf 删掉。
 *   真正的机制是**选 level 触发**（见 `enter_system_off()` 第 3 步）：
 *   Zephyr 只在 LEVEL 模式下才写 `PIN_CNF[n].SENSE`，而 SENSE → DETECT
 *   是唯一能唤醒 System OFF 的 GPIO 路径；EDGE 走 GPIOTE IN 事件，
 *   而 GPIOTE 在 System OFF 下断电。
 *   （如果想让 **edge** 也走 SENSE 以省 GPIOTE 的电，那是 devicetree 的
 *   `sense-edge-mask` 属性，不是 Kconfig。本工程不需要。）
 */
