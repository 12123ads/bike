/*
 * BLE 开锁通道的 BabbleSim 运行时测试。
 *
 * 这不是单元测试，是**把真固件的开锁模块放进射频仿真里跑一遍**：
 * 两个仿真设备，一个跑我们的 `ble_unlock.c` + `unlock.c`（peripheral），
 * 一个当手机（central），走完整的 GATT 连接 → 订阅 → 三步 APDU。
 *
 * 它回答的是「编译过了」之外的三个问题：
 *   1. GATT 表能不能被真的发现、CMD/RSP 特征的属性对不对；
 *   2. 30 字节 UNLOCK APDU 在配好的 MTU 下能不能一次写进去；
 *   3. 三重校验的三条**拒绝**路径（坏 MAC / 重放 counter / 重用 nonce）
 *      在真链路上是不是真的回 69 82 —— 这是「录一次开锁不能再开一次」
 *      那条底线的唯一可执行证明（DESIGN.md §5.1）。
 *
 * 被测代码是 firmware/nrf52840/src 下的原件，一行都没有为测试改过。
 */

#include "ble_unlock.h"
#include "crypto.h"
#include "nvstore.h"
#include "unlock.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <psa/crypto.h>

#include "bs_tracing.h"
#include "bstests.h"

/* `bst_result` 定义在 `boards/native/nrf_bsim/common/bstests_entry.c`，
 * 头文件里没有 extern 声明 —— 上游测试一律自己写一行（例如
 * `tests/bsim/babblekit/include/babblekit/testcase.h:11`）。 */
extern enum bst_result_t bst_result;

LOG_MODULE_REGISTER(bletest, LOG_LEVEL_INF);


/* 仿真里等多久算超时。射频仿真是虚拟时间，跑得比实时快得多。 */
#define WAIT_TIME_S 30

/* 和 ble_unlock.c 里同一个基址 —— central 侧必须自己写一遍，
 * 这正好顺带验证了「UUID 是可发现的公开约定」而不是内部实现细节。 */
#define BT_UUID_EBIKE_SVC_VAL \
	BT_UUID_128_ENCODE(0x2c1327ba, 0xa717, 0x4314, 0x827e, 0x92532d7a0001)
#define BT_UUID_EBIKE_CMD_VAL \
	BT_UUID_128_ENCODE(0x2c1327ba, 0xa717, 0x4314, 0x827e, 0x92532d7a0002)
#define BT_UUID_EBIKE_RSP_VAL \
	BT_UUID_128_ENCODE(0x2c1327ba, 0xa717, 0x4314, 0x827e, 0x92532d7a0003)

static const struct bt_uuid_128 uuid_svc = BT_UUID_INIT_128(BT_UUID_EBIKE_SVC_VAL);
static const struct bt_uuid_128 uuid_cmd = BT_UUID_INIT_128(BT_UUID_EBIKE_CMD_VAL);
static const struct bt_uuid_128 uuid_rsp = BT_UUID_INIT_128(BT_UUID_EBIKE_RSP_VAL);

/* 测试用的 per-user 密钥。和固件里的 secret 长度一致（SECRET_LEN=32）。 */
static const uint8_t test_secret[SECRET_LEN] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};
#define TEST_UID 0x11223344u

#define FAIL(...)                                                              \
	do {                                                                   \
		bst_result = Failed;                                           \
		bs_trace_error_time_line(__VA_ARGS__);                         \
	} while (0)

#define PASS(...)                                                              \
	do {                                                                   \
		bst_result = Passed;                                           \
		bs_trace_info_time(1, __VA_ARGS__);                            \
	} while (0)

static void test_tick(bs_time_t HW_device_time)
{
	if (bst_result != Passed) {
		FAIL("测试在 %d 秒内没有通过\n", WAIT_TIME_S);
	}
}

static void test_init(void)
{
	bst_ticker_set_next_tick_absolute(WAIT_TIME_S * 1e6);
	bst_result = In_progress;
}

/* ======================================================================
 * peripheral 侧：就是真固件
 * ====================================================================== */

static volatile bool unlock_fired;
static volatile bool unlock_ok_seen;
static volatile uint32_t unlock_uid_seen;

static void on_unlock_result(bool ok, uint32_t uid)
{
	unlock_fired = true;
	unlock_ok_seen = ok;
	unlock_uid_seen = uid;
	LOG_INF("开锁回调: ok=%d uid=%08x", (int)ok, uid);
}

static void peripheral_main(void)
{
	int rc;

	rc = crypto_init();
	if (rc != 0) {
		FAIL("crypto_init 失败 %d\n", rc);
		return;
	}

	rc = nvstore_init();
	if (rc != 0) {
		/* bsim 上 flash 是模拟的，失败也继续 —— counter 退化成 RAM 版，
		 * 本测试的重放判定仍然成立（同一次开机内）。 */
		LOG_WRN("nvstore_init 失败 %d（继续，counter 只在 RAM）", rc);
	}

	rc = unlock_init();
	if (rc != 0) {
		FAIL("unlock_init 失败 %d\n", rc);
		return;
	}
	unlock_set_callback(on_unlock_result);

	/* 灌一把测试密钥。走的是契约 §6.2 的下行入口，不是测试后门。 */
	rc = unlock_set_secret(TEST_UID, test_secret, 7);
	if (rc != 0) {
		FAIL("unlock_set_secret 失败 %d\n", rc);
		return;
	}

	rc = ble_unlock_init();
	if (rc != 0) {
		FAIL("ble_unlock_init 失败 %d\n", rc);
		return;
	}
	rc = ble_unlock_start();
	if (rc != 0) {
		FAIL("ble_unlock_start 失败 %d\n", rc);
		return;
	}
	LOG_INF("peripheral 已开始广播，等 central");

	/* 等 central 跑完它那一串断言。peripheral 侧的通过条件只有一条：
	 * 合法 UNLOCK 必须触发一次 ok=true 的回调，且 uid 正确。 */
	for (int i = 0; i < WAIT_TIME_S * 10; i++) {
		if (unlock_fired && unlock_ok_seen && unlock_uid_seen == TEST_UID) {
			PASS("peripheral: 开锁回调按预期触发\n");
			return;
		}
		k_sleep(K_MSEC(100));
	}
	FAIL("peripheral: 没有收到合法开锁回调（fired=%d ok=%d uid=%08x）\n",
	     (int)unlock_fired, (int)unlock_ok_seen, unlock_uid_seen);
}

/* ======================================================================
 * central 侧：假装是手机
 * ====================================================================== */

static struct bt_conn *g_conn;
static uint16_t cmd_handle;
static uint16_t rsp_handle;
static uint16_t rsp_ccc_handle;

static K_SEM_DEFINE(sem_connected, 0, 1);
static K_SEM_DEFINE(sem_discovered, 0, 1);
static K_SEM_DEFINE(sem_subscribed, 0, 1);
static K_SEM_DEFINE(sem_written, 0, 1);
static K_SEM_DEFINE(sem_notified, 0, 1);
static K_SEM_DEFINE(sem_mtu, 0, 1);

static uint8_t last_rsp[64];
static uint16_t last_rsp_len;

static struct bt_gatt_discover_params disc_params;
static struct bt_gatt_subscribe_params sub_params;
static struct bt_uuid_128 disc_uuid;

static void central_scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			    struct net_buf_simple *ad)
{
	if (g_conn != NULL) {
		return;
	}
	/* 只连第一个看到的设备 —— 仿真里只有一个 peripheral。
	 * ⚠ 注意这里**没有按 service UUID 过滤**：那正好证明了
	 * 服务 UUID 在扫描响应里（广播包只有 Flags），
	 * 主动扫描能拿到合并后的 ScanRecord。 */
	int rc = bt_le_scan_stop();
	if (rc != 0 && rc != -EALREADY) {
		FAIL("bt_le_scan_stop 失败 %d\n", rc);
		return;
	}
	rc = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
			       BT_LE_CONN_PARAM_DEFAULT, &g_conn);
	if (rc != 0) {
		FAIL("bt_conn_le_create 失败 %d\n", rc);
	}
}

static void central_connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0) {
		FAIL("central 连接失败 %u\n", err);
		return;
	}
	k_sem_give(&sem_connected);
}

static void central_disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("central 断开 %u", reason);
}

BT_CONN_CB_DEFINE(central_cb) = {
	.connected = central_connected,
	.disconnected = central_disconnected,
};

/* 一次性把服务里的**所有**特征枚举完，两个都收。
 *
 * ⚠ 别按 UUID 一个一个找：RSP 在 GATT 表里排在 CMD **前面**
 * （ble_unlock.c 的服务定义顺序是 RSP、CCC、CMD），
 * 「找到 CMD 再从它后面找 RSP」会永远找不到 —— 第一版就是这么错的，
 * 实测症状是 `cmd=21 rsp=0`。手机 App 也要注意同一件事。 */
static uint8_t discover_cb(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr,
			   struct bt_gatt_discover_params *params)
{
	if (attr == NULL) {
		/* 枚举结束 */
		k_sem_give(&sem_discovered);
		return BT_GATT_ITER_STOP;
	}

	if (params->type == BT_GATT_DISCOVER_PRIMARY) {
		struct bt_gatt_service_val *svc = attr->user_data;

		LOG_INF("找到服务，handle 区间 %u..%u",
			attr->handle, svc->end_handle);

		disc_params.uuid = NULL;   /* 不过滤，全枚举 */
		disc_params.start_handle = attr->handle + 1;
		disc_params.end_handle = svc->end_handle;
		disc_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

		int rc = bt_gatt_discover(conn, &disc_params);

		if (rc != 0) {
			FAIL("枚举特征失败 %d\n", rc);
		}
		return BT_GATT_ITER_STOP;
	}

	if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
		struct bt_gatt_chrc *chrc = attr->user_data;

		if (bt_uuid_cmp(chrc->uuid, &uuid_cmd.uuid) == 0) {
			cmd_handle = chrc->value_handle;
			LOG_INF("CMD 特征 value_handle=%u props=0x%02x",
				cmd_handle, chrc->properties);
			if ((chrc->properties & BT_GATT_CHRC_WRITE) == 0) {
				FAIL("CMD 特征没有 WRITE 属性（0x%02x）\n",
				     chrc->properties);
			}
		} else if (bt_uuid_cmp(chrc->uuid, &uuid_rsp.uuid) == 0) {
			rsp_handle = chrc->value_handle;
			/* CCCD 紧跟在值属性后面 */
			rsp_ccc_handle = chrc->value_handle + 1;
			LOG_INF("RSP 特征 value_handle=%u props=0x%02x",
				rsp_handle, chrc->properties);
			if ((chrc->properties & BT_GATT_CHRC_NOTIFY) == 0) {
				FAIL("RSP 特征没有 NOTIFY 属性（0x%02x）\n",
				     chrc->properties);
			}
			if ((chrc->properties & BT_GATT_CHRC_READ) != 0) {
				/* RSP 是 notify-only：可读的话手机能拿到
				 * 「上一次的应答」，那是不必要的信息泄露。 */
				FAIL("RSP 特征可读（0x%02x）—— 应该是 notify-only\n",
				     chrc->properties);
			}
		}

		return BT_GATT_ITER_CONTINUE;
	}

	return BT_GATT_ITER_CONTINUE;
}

static uint8_t notify_cb(struct bt_conn *conn,
			 struct bt_gatt_subscribe_params *params,
			 const void *data, uint16_t length)
{
	if (data == NULL) {
		return BT_GATT_ITER_STOP;
	}
	last_rsp_len = MIN(length, sizeof(last_rsp));
	memcpy(last_rsp, data, last_rsp_len);
	k_sem_give(&sem_notified);
	return BT_GATT_ITER_CONTINUE;
}

static void subscribed_cb(struct bt_conn *conn, uint8_t err,
			  struct bt_gatt_subscribe_params *params)
{
	if (err != 0) {
		FAIL("订阅失败 %u\n", err);
		return;
	}
	k_sem_give(&sem_subscribed);
}

static void write_cb(struct bt_conn *conn, uint8_t err,
		     struct bt_gatt_write_params *params)
{
	if (err != 0) {
		LOG_ERR("写失败 ATT err=0x%02x", err);
	}
	k_sem_give(&sem_written);
}

static void mtu_cb(struct bt_conn *conn, uint8_t err,
		   struct bt_gatt_exchange_params *params)
{
	LOG_INF("MTU 交换完成 err=%u，现在 MTU=%u", err, bt_gatt_get_mtu(conn));
	k_sem_give(&sem_mtu);
}

static struct bt_gatt_exchange_params mtu_params = { .func = mtu_cb };

/* 发一条 APDU 并等 notify 回来。返回应答长度，超时返回负数。 */
static int apdu_xfer(const uint8_t *apdu, uint16_t len)
{
	static struct bt_gatt_write_params wp;

	wp.func = write_cb;
	wp.handle = cmd_handle;
	wp.offset = 0;
	wp.data = apdu;
	wp.length = len;

	k_sem_reset(&sem_written);
	k_sem_reset(&sem_notified);

	int rc = bt_gatt_write(g_conn, &wp);
	if (rc != 0) {
		LOG_ERR("bt_gatt_write 失败 %d", rc);
		return -EIO;
	}
	if (k_sem_take(&sem_written, K_SECONDS(5)) != 0) {
		LOG_ERR("写没有完成");
		return -ETIMEDOUT;
	}
	if (k_sem_take(&sem_notified, K_SECONDS(5)) != 0) {
		LOG_ERR("没收到 notify 应答");
		return -ETIMEDOUT;
	}
	return last_rsp_len;
}

static bool sw_is(uint16_t sw)
{
	if (last_rsp_len < 2) {
		return false;
	}
	uint16_t got = (last_rsp[last_rsp_len - 2] << 8) |
			last_rsp[last_rsp_len - 1];
	return got == sw;
}

/* 三步协议的第 1、2 步。成功时 nonce 写进 out。 */
static bool do_select_and_challenge(uint8_t out_nonce[NONCE_LEN])
{
	static const uint8_t select_aid[] = {
		0x00, 0xA4, 0x04, 0x00, 0x07,
		0xF0, 0x45, 0x42, 0x49, 0x4B, 0x45, 0x01,
		0x00,
	};
	static const uint8_t get_challenge[] = {
		0x00, 0x84, 0x00, 0x00, NONCE_LEN,
	};

	if (apdu_xfer(select_aid, sizeof(select_aid)) < 0 || !sw_is(SW_OK)) {
		LOG_ERR("SELECT AID 没回 90 00");
		return false;
	}
	if (apdu_xfer(get_challenge, sizeof(get_challenge)) < 0 ||
	    !sw_is(SW_OK)) {
		LOG_ERR("GET CHALLENGE 没回 90 00");
		return false;
	}
	if (last_rsp_len != NONCE_LEN + 2) {
		LOG_ERR("nonce 长度不对：%u（应该 %u）",
			last_rsp_len, NONCE_LEN + 2);
		return false;
	}
	memcpy(out_nonce, last_rsp, NONCE_LEN);
	return true;
}

/* 拼第 3 步的 UNLOCK APDU，并算出它的 MAC。
 *
 * ⚠ MAC 的消息**逐字对齐 unlock.c:210-214**：
 *     msg = nonce(16) || counter(4, big-endian) || CLA(1) || INS(1)
 * 也就是 cmd 是 **CLA|INS 两个字节**（0x80 0x10），不是一个 INS 字节。
 * 把 CLA/INS 绑进 MAC 是为了让一条 UNLOCK 的 MAC 不能被挪去当别的命令用。
 * 这里故意手写一遍而不是复用固件代码 —— 如果哪天固件改了消息布局，
 * 这个测试**必须**跟着改，而它会先失败一次，那正是我们想要的提醒。
 */
static void build_unlock(uint8_t apdu[31], const uint8_t nonce[NONCE_LEN],
			 uint32_t counter, const uint8_t secret[SECRET_LEN],
			 bool corrupt_mac)
{
	uint8_t msg[NONCE_LEN + 4 + 2];
	uint8_t mac[32];
	size_t mac_len = 0;

	apdu[0] = 0x80;                        /* CLA */
	apdu[1] = 0x10;                        /* INS */
	apdu[2] = 0x00;
	apdu[3] = 0x00;
	apdu[4] = 4 + 4 + MAC_LEN;             /* Lc = 24 */
	sys_put_be32(TEST_UID, &apdu[5]);
	sys_put_be32(counter, &apdu[9]);
	apdu[13 + MAC_LEN] = 0x00;             /* Le */

	memcpy(msg, nonce, NONCE_LEN);
	sys_put_be32(counter, &msg[NONCE_LEN]);
	msg[NONCE_LEN + 4] = apdu[0];
	msg[NONCE_LEN + 5] = apdu[1];

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key;
	uint8_t key_ram[SECRET_LEN];

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
	psa_set_key_bits(&attr, SECRET_LEN * 8);

	memcpy(key_ram, secret, SECRET_LEN);   /* PSA 要 RAM 里的密钥 */
	psa_status_t st = psa_import_key(&attr, key_ram, SECRET_LEN, &key);

	if (st != PSA_SUCCESS) {
		FAIL("psa_import_key 失败 %d\n", (int)st);
		return;
	}
	st = psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256),
			     msg, sizeof(msg), mac, sizeof(mac), &mac_len);
	psa_destroy_key(key);
	if (st != PSA_SUCCESS) {
		FAIL("psa_mac_compute 失败 %d\n", (int)st);
		return;
	}

	if (corrupt_mac) {
		mac[0] ^= 0xFF;
	}
	memcpy(&apdu[13], mac, MAC_LEN);       /* 只取前 16 字节，同固件 */
}

static void central_main(void)
{
	int rc;

	/* ⚠ 用 `ble_unlock_init()` 而不是裸 `bt_enable()`，即使 central 侧
	 * 不需要开锁模块：两个 testid 在**同一个二进制**里，所以
	 * `ble_unlock.c` 的 `BT_CONN_CB_DEFINE` 在 central 上也会被调用，
	 * 而它的 `session_reset()` 要往 `unlock_workq` 投递 —— 队列没启动就是
	 * 往未初始化的结构里写。`ble_unlock_init()` 顺带把队列起好，
	 * 而它**不开广播**（那是 `ble_unlock_start()` 的事），所以 central
	 * 不会变成第二个可连接设备。 */
	rc = ble_unlock_init();
	if (rc != 0) {
		FAIL("central ble_unlock_init 失败 %d\n", rc);
		return;
	}
	rc = psa_crypto_init();
	if (rc != PSA_SUCCESS) {
		FAIL("central psa_crypto_init 失败 %d\n", rc);
		return;
	}

	rc = bt_le_scan_start(BT_LE_SCAN_ACTIVE, central_scan_cb);
	if (rc != 0) {
		FAIL("bt_le_scan_start 失败 %d\n", rc);
		return;
	}
	if (k_sem_take(&sem_connected, K_SECONDS(10)) != 0) {
		FAIL("连不上 peripheral\n");
		return;
	}
	LOG_INF("已连接");

	/* 【断言 1】MTU 必须够装 30 字节 APDU。
	 * 默认 23 只给 20，装不下 —— 这条钉住 prj.conf 那两行没被人改小。 */
	rc = bt_gatt_exchange_mtu(g_conn, &mtu_params);
	if (rc != 0) {
		FAIL("bt_gatt_exchange_mtu 失败 %d\n", rc);
		return;
	}
	if (k_sem_take(&sem_mtu, K_SECONDS(5)) != 0) {
		FAIL("MTU 交换超时\n");
		return;
	}
	uint16_t mtu = bt_gatt_get_mtu(g_conn);

	if (mtu - 3 < 30) {
		FAIL("MTU=%u 只能写 %u 字节，装不下 30 字节的 UNLOCK APDU\n",
		     mtu, mtu - 3);
		return;
	}
	LOG_INF("MTU=%u（可写 %u ≥ 30）✓", mtu, mtu - 3);

	/* 【断言 2】GATT 表可发现，两个特征的属性正确（在 discover_cb 里查）。 */
	memcpy(&disc_uuid, &uuid_svc, sizeof(disc_uuid));
	disc_params.uuid = &disc_uuid.uuid;
	disc_params.func = discover_cb;
	disc_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	disc_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	disc_params.type = BT_GATT_DISCOVER_PRIMARY;
	rc = bt_gatt_discover(g_conn, &disc_params);
	if (rc != 0) {
		FAIL("bt_gatt_discover 失败 %d\n", rc);
		return;
	}
	if (k_sem_take(&sem_discovered, K_SECONDS(10)) != 0) {
		FAIL("服务发现超时\n");
		return;
	}
	if (cmd_handle == 0 || rsp_handle == 0) {
		FAIL("没找到 CMD/RSP 特征（cmd=%u rsp=%u）\n",
		     cmd_handle, rsp_handle);
		return;
	}

	/* 【断言 3】不订阅 CCCD 就收不到应答 —— 这条钉住
	 * BT_GATT_ENFORCE_SUBSCRIPTION 的行为，也是 §2.1 里
	 * 「每次连接多一个往返」那个代价的来源。 */
	static const uint8_t probe[] = { 0x00, 0xA4, 0x04, 0x00, 0x07,
					 0xF0, 0x45, 0x42, 0x49, 0x4B,
					 0x45, 0x01, 0x00 };
	rc = apdu_xfer(probe, sizeof(probe));
	if (rc >= 0) {
		FAIL("没订阅就收到了 notify —— ENFORCE_SUBSCRIPTION 没生效\n");
		return;
	}
	LOG_INF("未订阅时收不到应答 ✓（符合 ENFORCE_SUBSCRIPTION）");

	sub_params.notify = notify_cb;
	sub_params.subscribe = subscribed_cb;
	sub_params.value = BT_GATT_CCC_NOTIFY;
	sub_params.value_handle = rsp_handle;
	sub_params.ccc_handle = rsp_ccc_handle;
	rc = bt_gatt_subscribe(g_conn, &sub_params);
	if (rc != 0) {
		FAIL("bt_gatt_subscribe 失败 %d\n", rc);
		return;
	}
	if (k_sem_take(&sem_subscribed, K_SECONDS(5)) != 0) {
		FAIL("订阅超时\n");
		return;
	}
	LOG_INF("已订阅 RSP");

	uint8_t nonce[NONCE_LEN];
	uint8_t unlock[31];

	/* 【断言 4】坏 MAC 必须被拒。 */
	if (!do_select_and_challenge(nonce)) {
		FAIL("三步协议前两步失败\n");
		return;
	}
	build_unlock(unlock, nonce, 1, test_secret, true);
	if (apdu_xfer(unlock, sizeof(unlock)) < 0) {
		FAIL("坏 MAC 的 UNLOCK 没有得到应答\n");
		return;
	}
	if (!sw_is(SW_DENIED)) {
		FAIL("坏 MAC 没被拒（应答 %02x %02x）\n",
		     last_rsp[0], last_rsp[1]);
		return;
	}
	LOG_INF("坏 MAC 被拒 ✓");

	/* 【断言 5】合法 UNLOCK 必须开锁。counter 从 1 开始。 */
	if (!do_select_and_challenge(nonce)) {
		FAIL("重新取 nonce 失败\n");
		return;
	}
	build_unlock(unlock, nonce, 1, test_secret, false);
	uint8_t replay[31];

	memcpy(replay, unlock, sizeof(replay));   /* 留一份原样重放 */
	if (apdu_xfer(unlock, sizeof(unlock)) < 0) {
		FAIL("合法 UNLOCK 没有得到应答\n");
		return;
	}
	if (!sw_is(SW_OK)) {
		FAIL("合法 UNLOCK 被拒了（应答 %02x %02x）—— 开锁功能不通\n",
		     last_rsp[0], last_rsp[1]);
		return;
	}
	LOG_INF("合法 UNLOCK 开锁 ✓");

	/* 【断言 6】重放同一条报文必须被拒。这是 §5.1 的底线：
	 * 「嗅探到一次完整开锁交互也不能再开一次」。
	 * 注意这里连 nonce 都不重新取 —— 就是原封不动重发。 */
	if (apdu_xfer(replay, sizeof(replay)) < 0) {
		FAIL("重放报文没有得到应答\n");
		return;
	}
	if (!sw_is(SW_DENIED)) {
		FAIL("**重放成功了** —— §5.1 的底线被破（应答 %02x %02x）\n",
		     last_rsp[0], last_rsp[1]);
		return;
	}
	LOG_INF("原样重放被拒 ✓");

	/* 【断言 7】counter 必须严格递增：拿新 nonce、用旧 counter 也要拒。
	 * 这一条和断言 6 不同 —— 它证明拒绝来自 counter 而不只是 nonce 一次性。 */
	if (!do_select_and_challenge(nonce)) {
		FAIL("取第三个 nonce 失败\n");
		return;
	}
	build_unlock(unlock, nonce, 1, test_secret, false);
	if (apdu_xfer(unlock, sizeof(unlock)) < 0) {
		FAIL("旧 counter 的 UNLOCK 没有得到应答\n");
		return;
	}
	if (!sw_is(SW_DENIED)) {
		FAIL("counter 没有严格递增校验（应答 %02x %02x）\n",
		     last_rsp[0], last_rsp[1]);
		return;
	}
	LOG_INF("旧 counter 被拒 ✓");

	/* 【断言 8】counter 递增之后可以再开。证明上面几条不是「一律拒绝」。 */
	if (!do_select_and_challenge(nonce)) {
		FAIL("取第四个 nonce 失败\n");
		return;
	}
	build_unlock(unlock, nonce, 2, test_secret, false);
	if (apdu_xfer(unlock, sizeof(unlock)) < 0) {
		FAIL("counter=2 的 UNLOCK 没有得到应答\n");
		return;
	}
	if (!sw_is(SW_OK)) {
		FAIL("counter 递增后仍被拒（应答 %02x %02x）\n",
		     last_rsp[0], last_rsp[1]);
		return;
	}
	LOG_INF("counter 递增后再次开锁 ✓");

	PASS("central: 8 条断言全过（MTU/发现/订阅门/坏MAC/开锁/重放/旧counter/再开锁）\n");
}

/* ====================================================================== */

static const struct bst_test_instance test_defs[] = {
	{
		.test_id = "peripheral",
		.test_descr = "跑真固件的 ble_unlock + unlock",
		.test_pre_init_f = test_init,
		.test_tick_f = test_tick,
		.test_main_f = peripheral_main,
	},
	{
		.test_id = "central",
		.test_descr = "假装手机，跑完三步协议与三条拒绝路径",
		.test_pre_init_f = test_init,
		.test_tick_f = test_tick,
		.test_main_f = central_main,
	},
	BSTEST_END_MARKER,
};

static struct bst_test_list *install(struct bst_test_list *tests)
{
	return bst_add_tests(tests, test_defs);
}

bst_test_install_t test_installers[] = { install, NULL };

int main(void)
{
	bst_main();
	return 0;
}
