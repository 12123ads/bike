/* BLE 开锁通道。见 ble_unlock.h 的报文说明与那三条 ⚠。 */

#include "ble_unlock.h"
#include "unlock.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/version.h>

LOG_MODULE_REGISTER(ble_unlock, CONFIG_EBIKE_LOG_LEVEL);

/* --- UUID -------------------------------------------------------------------
 *
 * 随机生成的一个 128-bit 基址（RFC 4122 v4），低 16 位区分三个 UUID：
 *   ...0001 服务 / ...0002 CMD（write）/ ...0003 RSP（notify）
 * 基址 2c1327ba-a717-4314-827e-92532d7a xxxx。
 */
#define BT_UUID_EBIKE_SVC_VAL \
	BT_UUID_128_ENCODE(0x2c1327ba, 0xa717, 0x4314, 0x827e, 0x92532d7a0001)
#define BT_UUID_EBIKE_CMD_VAL \
	BT_UUID_128_ENCODE(0x2c1327ba, 0xa717, 0x4314, 0x827e, 0x92532d7a0002)
#define BT_UUID_EBIKE_RSP_VAL \
	BT_UUID_128_ENCODE(0x2c1327ba, 0xa717, 0x4314, 0x827e, 0x92532d7a0003)

#define BT_UUID_EBIKE_SVC BT_UUID_DECLARE_128(BT_UUID_EBIKE_SVC_VAL)
#define BT_UUID_EBIKE_CMD BT_UUID_DECLARE_128(BT_UUID_EBIKE_CMD_VAL)
#define BT_UUID_EBIKE_RSP BT_UUID_DECLARE_128(BT_UUID_EBIKE_RSP_VAL)

/* --- 广播参数 ---------------------------------------------------------------
 *
 * BT_LE_ADV_OPT_CONN 是 Zephyr 4.0 新符号，数值上等于旧的
 * CONNECTABLE|ONE_TIME（BIT(0)|BIT(1)，migration-guide-4.0.rst 也明说两者等价），
 * 而旧符号在 4.3 已删除。语义都是「连上就停广播、不自动恢复」——
 * 恢复由下面的 recycled 回调做，这正是电池产品想要的：
 * 断开之后要不要继续广播由我们的状态机说，不由协议栈说。
 *
 * ⚠ 两个坑：
 *   1. 不能写 `#if defined(BT_LE_ADV_OPT_CONN)` —— 它是 enum 成员不是宏，
 *      预处理器看不见。只能按版本号分支。
 *   2. 版本号要用 `KERNEL_VERSION_NUMBER`（version.h 里是无条件 `#define`），
 *      **不要用 `ZEPHYR_VERSION_CODE` 配 `ZEPHYR_VERSION(a,b,c)`** ——
 *      那两个是 `#cmakedefine`，没定义时 `#if` 里的
 *      `ZEPHYR_VERSION(4,0,0)` 会展开成 `0 (4,0,0)`，是语法错误而不是 0。
 *      编码：major<<16 | minor<<8 | patch，所以 4.0.0 是 0x040000。
 */
#if KERNEL_VERSION_NUMBER >= 0x040000
#define EBIKE_ADV_OPT BT_LE_ADV_OPT_CONN
#else
#define EBIKE_ADV_OPT (BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_ONE_TIME)
#endif

/* 100~150 ms（BT_GAP_ADV_FAST_INT_*_2）。
 *
 * 选它而不是 SLOW（1~1.2 s）的理由是**时延**，不是功耗：FAST_2 约 130 µA、
 * SLOW 约 16 µA，两者都远低于 4G 模组最省档的 500 µA，在整机预算里都看不见。
 * 而 1 s 间隔会让手机发现设备多等 1~2 秒，用户站在车前是能感觉到的。 */
#define EBIKE_ADV_INT_MIN BT_GAP_ADV_FAST_INT_MIN_2
#define EBIKE_ADV_INT_MAX BT_GAP_ADV_FAST_INT_MAX_2

/* 广播包里**只放 Flags**，不放名字也不放服务 UUID：
 * 防盗产品不该对着街上喊「我是一台可以被 BLE 开锁的车」。
 * 服务 UUID 放在 **scan response** 里 —— 安卓的 BLE 扫描是主动扫描
 * （会发 SCAN_REQ），ScanRecord 由广播包和扫描响应合并而成，所以
 * `ScanFilter.setServiceUuid()` 仍然能匹配到，而被动嗅探只看到一个裸 Flags。
 *
 * 「必须带 ScanFilter」是安卓侧的硬要求：无过滤扫描在息屏或定位关闭时会被
 * 静默挂起（ScanManager 的 requiresScreenOn/requiresLocationOn），
 * 挂起时 App 收不到任何回调。
 *
 * 设备名仍然可以在连上之后从 GAP 的 Device Name 特征读到（CONFIG_BT_DEVICE_NAME）。 */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_EBIKE_SVC_VAL),
};

/* --- 状态 -------------------------------------------------------------------
 *
 * 并发模型（从 NFC 迁过来最容易漏的一处，也是这个模块唯一需要动脑的地方）：
 *
 *   写 APDU     BT RX WQ 线程   → on_cmd_write：只 memcpy + submit
 *   连接/断开   BT RX WQ 线程   → on_connected/on_disconnected：只 submit
 *   静止关广播  uplink 线程     → ble_unlock_stop：只 submit
 *                                        ↓
 *   **unlock 状态的每一次访问都在 unlock_workq 这一个线程上**
 *   （apdu_work_fn 调 unlock_handle_apdu，session_reset_work_fn 调
 *    unlock_session_reset）
 *
 * `unlock.c` 的 cur_nonce / nonce_valid / selected 是无锁静态状态，
 * 上面这个「单一访问线程」是它们安全的**唯一**理由。所以连
 * `unlock_session_reset()` 这种看着无害的调用也必须走队列 ——
 * 直接在回调里调，就是在另一个线程上写同一批变量。
 * 延迟几毫秒作废 nonce 没有安全影响（nonce 本来就用过即废）。
 *
 * CONFIG_BT_MAX_CONN 一旦 > 1，这个论证就失效（会有两条并发的 APDU 流），
 * 那时必须给 unlock.c 加锁。prj.conf 里那行 `=1` 是论证的一部分。
 *
 * 为什么不复用 system workqueue：nvstore 的 counter 延迟落盘挂在 sysworkq 上，
 * 而 flash 写被 SOC_FLASH_NRF_RADIO_SYNC_MPSL 排到 MPSL timeslot 之后 ——
 * 别让开锁的响应时延排在一次 flash 擦写后面。
 */
#define UNLOCK_WORKQ_STACK_SIZE 2048
#define UNLOCK_WORKQ_PRIORITY   6

K_THREAD_STACK_DEFINE(unlock_workq_stack, UNLOCK_WORKQ_STACK_SIZE);
static struct k_work_q unlock_workq;

/* 一次在飞的 APDU。C-APDU 最长是 UNLOCK 的 30 字节，给 64 留余量。 */
static struct {
	struct k_work work;
	struct bt_conn *conn;      /* 收到这条 APDU 的连接，已 ref */
	uint8_t buf[64];
	uint16_t len;
} pending;

/* 有 APDU 在处理中。write 回调立刻返回、应答异步 notify，所以手机完全可以在
 * 上一条还在算的时候就发下一条（ATT 只保证「一个请求一个响应」，不保证我们
 * 已经处理完）。**这个标志不是防御性代码，是必需的** —— 没有它第二条写会
 * 直接踩掉正在处理的 pending.buf。 */
static atomic_t busy;

static void session_reset_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	unlock_session_reset();
}

static K_WORK_DEFINE(session_reset_work, session_reset_work_fn);

/* 作废上一次会话残留的 nonce。可以从任何线程调 —— 真正的动作在
 * unlock_workq 上做，见上面的并发模型。 */
static void session_reset(void)
{
	(void)k_work_submit_to_queue(&unlock_workq, &session_reset_work);
}

static bool notify_enabled;
static bool active;

/* --- GATT ------------------------------------------------------------------- */

/* 服务表要在 apdu_work_fn 之前定义 —— 那里要用 `ebike_svc.attrs[2]` 当
 * notify 的属性句柄（照 sdk-nrf 的 nus.c）。所以两个回调先声明后定义。 */
static ssize_t on_cmd_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    const void *buf, uint16_t len, uint16_t offset,
			    uint8_t flags);
static void rsp_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

BT_GATT_SERVICE_DEFINE(ebike_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_EBIKE_SVC),
	/* notify-only：不给读权限，手机拿不到「上一次的应答」。
	 * 也没有独立的 nonce 读特征 —— 那会让任何人连上来读一下就作废
	 * 合法用户的 nonce，把 unlock.c 的 nonce_valid 变成远程可 DoS 的状态位。
	 * nonce 只在收到合法 GET CHALLENGE 时生成。 */
	BT_GATT_CHARACTERISTIC(BT_UUID_EBIKE_RSP,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(rsp_ccc_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	/* 明文写，不要求配对/加密（BT_GATT_PERM_WRITE 而不是 _WRITE_ENCRYPT）。
	 * 这是有意的：链路加密在这里买不到东西 —— 设备没有屏幕也没有键盘
	 * （IO capability = NoInputNoOutput），LESC 只能退化成 Just Works，
	 * 而 Just Works 没有 MITM 防护（smp.c 的 remote_sec_level_reachable()
	 * 对 JUST_WORKS 直接返回 BT_SMP_ERR_AUTH_REQUIREMENTS，也就是
	 * L3/L4 根本达不到）。真正的认证在 §5.2 的 HMAC 挑战应答里，
	 * 那一层不依赖链路加密。见 ADR-004 §5。 */
	BT_GATT_CHARACTERISTIC(BT_UUID_EBIKE_CMD,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE,
			       NULL, on_cmd_write, NULL),
);

/* notify 的属性句柄。attrs 序：
 * [0]=primary service, [1]=RSP chrc decl, [2]=RSP value, [3]=CCC,
 * [4]=CMD chrc decl, [5]=CMD value —— BT_GATT_CHARACTERISTIC 展开成两个属性。 */
#define RSP_VALUE_ATTR (&ebike_svc.attrs[2])

static void apdu_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	/* 应答最长 = nonce(16) + SW(2) = 18 字节。 */
	uint8_t rsp[64];
	int n = unlock_handle_apdu(pending.buf, pending.len, rsp, sizeof(rsp));

	if (n < 0) {
		LOG_ERR("APDU 处理失败 %d", n);
		/* 回一个明确的拒绝而不是不回 —— 不回会让手机等到超时，
		 * 用户看到的是「点了没反应」而不是「被拒绝了」。
		 * 用 SW_DENIED 而不是裸字节：契约 §5.2 要求一律回同一个
		 * 状态字，不给攻击者区分失败原因的信道。 */
		rsp[0] = (uint8_t)(SW_DENIED >> 8);
		rsp[1] = (uint8_t)(SW_DENIED & 0xFF);
		n = 2;
	}

	if (!notify_enabled) {
		/* CONFIG_BT_GATT_ENFORCE_SUBSCRIPTION 默认 y，没订阅时
		 * bt_gatt_notify() 会返回 -EINVAL。提前说清楚原因，
		 * 否则现场只会看到一个 -22。 */
		LOG_WRN("手机没订阅 RSP 的 CCCD，应答发不出去");
	} else {
		int rc = bt_gatt_notify(pending.conn, RSP_VALUE_ATTR,
					rsp, (uint16_t)n);
		if (rc != 0) {
			LOG_ERR("回应答失败 rc=%d", rc);
		}
	}

	bt_conn_unref(pending.conn);
	pending.conn = NULL;
	atomic_clear(&busy);
}

static ssize_t on_cmd_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    const void *buf, uint16_t len, uint16_t offset,
			    uint8_t flags)
{
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);   /* prepare/exec 不会来：BT_ATT_PREPARE_COUNT 默认 0，
			      * 那两个 opcode 根本没进 ATT 的处理表。 */

	if (offset != 0) {
		/* 不接受 long write。整条 APDU 一次写进来，MTU 已经配够。 */
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len == 0 || len > sizeof(pending.buf)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	if (!atomic_cas(&busy, 0, 1)) {
		return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
	}

	memcpy(pending.buf, buf, len);
	pending.len = len;
	pending.conn = bt_conn_ref(conn);

	/* 只 memcpy + submit，绝不在这里跑 psa_mac_verify()：
	 * 这个回调占的是协议栈自己的 "BT RX WQ"，官方指导明说
	 * 「Keep callbacks short, and defer work that is long-running」。
	 * 不 defer 的话，攻击者灌 UNLOCK 写请求就能把 BT 处理整条拖死 —— 那是
	 * 一个 NFC 侧不存在的 DoS（NFC 得贴上来）。 */
	int rc = k_work_submit_to_queue(&unlock_workq, &pending.work);
	if (rc < 0) {
		/* 排不进去就必须把 busy 和 conn 引用退回来 ——
		 * 漏一次就永久卡死开锁通道（busy 再也不会被清）。 */
		LOG_ERR("APDU 入队失败 rc=%d", rc);
		bt_conn_unref(pending.conn);
		pending.conn = NULL;
		atomic_clear(&busy);
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	return len;   /* 必须返回 len，否则 att.c 判成 ATT 错误 */
}

static void rsp_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_DBG("RSP 订阅 = %d", (int)notify_enabled);
}

/* --- 连接回调 --------------------------------------------------------------- */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err != 0) {
		LOG_WRN("连接失败 %s err=%u", addr, err);
		return;
	}

	/* 对应原来 NFC 的 FIELD_ON：作废上一次会话残留的 nonce。
	 * 不作废也不会不安全（nonce 用过即废），但会少一次无效往返。 */
	session_reset();
	LOG_INF("已连接 %s，MTU=%u", addr, bt_gatt_get_mtu(conn));
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);

	notify_enabled = false;
	session_reset();
	LOG_INF("已断开 reason=%u", reason);
}

static void on_recycled(void)
{
	/* 连接对象已经归还池子，这才是重启广播的正确时机 ——
	 * 在 disconnected 里重启可能因为对象还没释放而失败（conn.h 的注释）。
	 * 只有还在「开着」的状态才恢复：静止后是主动关的，不能被一次断开唤回来。 */
	if (!active) {
		return;
	}
	int rc = bt_le_adv_start(BT_LE_ADV_PARAM(EBIKE_ADV_OPT,
						 EBIKE_ADV_INT_MIN,
						 EBIKE_ADV_INT_MAX, NULL),
				 ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (rc != 0) {
		LOG_ERR("断开后重启广播失败 rc=%d", rc);
		active = false;
	}
}

BT_CONN_CB_DEFINE(ble_unlock_conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.recycled = on_recycled,
};

/* --- 对外接口 --------------------------------------------------------------- */

int ble_unlock_init(void)
{
	k_work_queue_start(&unlock_workq, unlock_workq_stack,
			   K_THREAD_STACK_SIZEOF(unlock_workq_stack),
			   UNLOCK_WORKQ_PRIORITY, NULL);
	k_thread_name_set(k_work_queue_thread_get(&unlock_workq), "unlock");
	k_work_init(&pending.work, apdu_work_fn);

	/* 同步 enable（cb=NULL）：init 顺序里 unlock_init 已经跑完，
	 * 广播一开就可能来连接，所以要等栈真的就绪。 */
	int rc = bt_enable(NULL);
	if (rc != 0) {
		LOG_ERR("bt_enable 失败 rc=%d", rc);
		return rc;
	}

	/* 不开 CONFIG_BT_SETTINGS，所以这里没有 settings_load("bt") 那一跳 ——
	 * 我们不用 SMP，没有 LTK/IRK 要持久化，也就不该让 BT 去碰
	 * nvstore 的那 32 kB storage 分区（counter 就住在那里）。 */
	LOG_INF("BLE 就绪（GATT peripheral）");
	return 0;
}

int ble_unlock_start(void)
{
	if (active) {
		return 0;
	}
	int rc = bt_le_adv_start(BT_LE_ADV_PARAM(EBIKE_ADV_OPT,
						 EBIKE_ADV_INT_MIN,
						 EBIKE_ADV_INT_MAX, NULL),
				 ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (rc != 0) {
		LOG_ERR("bt_le_adv_start 失败 rc=%d", rc);
		return rc;
	}
	active = true;
	return 0;
}

int ble_unlock_stop(void)
{
	if (!active) {
		return 0;
	}
	/* 先置 false，再停 —— 否则 stop 引发的回调可能把广播又开回来。 */
	active = false;

	int rc = bt_le_adv_stop();
	if (rc != 0 && rc != -EALREADY) {
		LOG_ERR("bt_le_adv_stop 失败 rc=%d", rc);
		active = true;
		return rc;
	}
	session_reset();
	return 0;
}

bool ble_unlock_is_active(void)
{
	return active;
}

void ble_unlock_shutdown(void)
{
	(void)ble_unlock_stop();

	int rc = bt_disable();
	if (rc != 0) {
		/* 不因此放弃关机 —— 和「武装唤醒失败就不关机」的处理不同：
		 * 醒不过来是致命的，radio 多耗一点电只是浪费。 */
		LOG_ERR("bt_disable 失败 rc=%d（radio 可能还开着）", rc);
	}
}
