/* 挑战应答开锁。见 unlock.h 的协议说明。 */

#include "unlock.h"
#include "crypto.h"
#include "nvstore.h"

#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(unlock, CONFIG_EBIKE_LOG_LEVEL);

/* AID: F0 45 42 49 4B 45 01 —— "EBIKE" 加专用前缀 F0 和版本 01 */
static const uint8_t aid[AID_LEN] = { 0xF0, 0x45, 0x42, 0x49, 0x4B, 0x45, 0x01 };

/* users[]/key_set_id 的互斥（2026-09-03 审计 H3）。
 *
 * ⚠ 单线程论证只覆盖 nonce 状态：APDU 处理（unlock_workq）和密钥下发
 * （uplink 线程的 handle_secret → unlock_set/del/wipe_secrets）是**两个
 * 线程**，都会碰 users[] —— find_user 返回的裸指针可在 HMAC 期间被
 * del/set 换主，counter 回写会进错槽位（重放窗口）。所以这部分必须
 * 上锁；cur_nonce/nonce_valid/selected 仍然只在 unlock_workq 上碰
 * （unlock_session_reset 经 ble_unlock.c 排队到同一队列），无需锁。
 *
 * 锁内不做慢操作：flash 写（nvstore_save_users）和 result_cb
 * （会走 uplink_queue_event，见审计 M3）都放到锁外 —— 用局部快照
 * 换取临界区只有内存操作。 */
static struct k_mutex users_lock;

static struct user_key users[MAX_USERS];
static uint16_t key_set_id;

/* 当前这一次会话发出去的 nonce。
 * `nonce_valid` 就是「一次性」的实现：验完立刻清掉，
 * 所以重放同一个 (nonce, mac) 对第二次会走到「没有待验的 nonce」分支。 */
static uint8_t cur_nonce[NONCE_LEN];
static bool nonce_valid;
static bool selected;    /* SELECT AID 过了没 —— 没 select 就不接受 UNLOCK */

static unlock_result_cb result_cb;

void unlock_set_callback(unlock_result_cb cb)
{
	result_cb = cb;
}

uint16_t unlock_current_kid(void)
{
	k_mutex_lock(&users_lock, K_FOREVER);
	uint16_t kid = key_set_id;
	k_mutex_unlock(&users_lock);
	return kid;
}

void unlock_session_reset(void)
{
	nonce_valid = false;
	selected = false;
	memset(cur_nonce, 0, sizeof(cur_nonce));
}

static int put_sw(uint8_t *rsp, size_t rsp_len, uint16_t sw)
{
	if (rsp_len < 2) {
		return -ENOMEM;
	}
	rsp[0] = (uint8_t)(sw >> 8);
	rsp[1] = (uint8_t)(sw & 0xFF);
	return 2;
}

/* 持锁调用。 */
static struct user_key *find_user_locked(uint32_t uid)
{
	for (size_t i = 0; i < MAX_USERS; i++) {
		if (users[i].valid && users[i].uid == uid) {
			return &users[i];
		}
	}
	return NULL;
}

/* 持锁调用：把 users 表快照到 out（锁外做 flash 写）。
 * 返回 key_set_id 的快照。 */
static uint16_t snapshot_users_locked(struct user_key out[MAX_USERS])
{
	memcpy(out, users, sizeof(users));
	return key_set_id;
}

/* --- 密钥管理 ---------------------------------------------------------------
 * 全部从 uplink 线程调（handle_secret）。锁保护与 unlock_workq 的并发，
 * flash 写在锁外做（快照）。 */

int unlock_set_secret(uint32_t uid, const uint8_t secret[SECRET_LEN], uint16_t kid)
{
	static struct user_key snap[MAX_USERS];
	uint16_t kid_out;
	int rc;

	k_mutex_lock(&users_lock, K_FOREVER);
	struct user_key *u = find_user_locked(uid);
	if (u == NULL) {
		for (size_t i = 0; i < MAX_USERS; i++) {
			if (!users[i].valid) {
				u = &users[i];
				break;
			}
		}
	}
	if (u == NULL) {
		k_mutex_unlock(&users_lock);
		LOG_ERR("密钥槽满了（%d 个）", MAX_USERS);
		return -ENOSPC;
	}

	u->uid = uid;
	memcpy(u->secret, secret, SECRET_LEN);
	/* counter **不清零** —— 换密钥不该让旧的重放报文重新可用。
	 * 同一 uid 复用槽位时保留旧 counter 是有意的；
	 * **不同 uid** 复用（先 del 再 set）的槽位在 del 时已清零（见下）。 */
	u->valid = true;
	key_set_id = kid;
	kid_out = snapshot_users_locked(snap);
	k_mutex_unlock(&users_lock);

	rc = nvstore_save_users(snap, MAX_USERS, kid_out);
	if (rc != 0) {
		LOG_ERR("密钥落盘失败 rc=%d —— 掉电就丢", rc);
	}
	return rc;
}

int unlock_del_secret(uint32_t uid)
{
	static struct user_key snap[MAX_USERS];
	uint16_t kid_out;

	k_mutex_lock(&users_lock, K_FOREVER);
	struct user_key *u = find_user_locked(uid);
	if (u == NULL) {
		k_mutex_unlock(&users_lock);
		return -ENOENT;
	}
	/* 抹掉密钥字节而不只是清 valid：flash 上的残留是可读的。
	 * counter 也清零：这个槽位将来可能被**另一个 uid** 复用，
	 * 旧 counter 留着会把新用户锁在门外（审计 M7 —— 新手机从 1 开始
	 * 发 counter，`counter <= u->counter` 会全部被拒）。 */
	crypto_wipe(u->secret, SECRET_LEN);
	u->valid = false;
	u->uid = 0;
	u->counter = 0;
	kid_out = snapshot_users_locked(snap);
	k_mutex_unlock(&users_lock);

	return nvstore_save_users(snap, MAX_USERS, kid_out);
}

int unlock_wipe_secrets(void)
{
	static struct user_key snap[MAX_USERS];
	uint16_t kid_out;
	int rc;

	k_mutex_lock(&users_lock, K_FOREVER);
	for (size_t i = 0; i < MAX_USERS; i++) {
		crypto_wipe(users[i].secret, SECRET_LEN);
		users[i].valid = false;
		users[i].uid = 0;
		users[i].counter = 0;   /* 同 del：防复用槽位继承旧 counter */
	}
	kid_out = snapshot_users_locked(snap);
	k_mutex_unlock(&users_lock);

	rc = nvstore_save_users(snap, MAX_USERS, kid_out);
	return rc;
}

/* --- APDU 处理 -------------------------------------------------------------- */

static int handle_select(const uint8_t *apdu, size_t len,
			uint8_t *rsp, size_t rsp_len)
{
	/* 00 A4 04 00 Lc <aid> [Le] */
	if (len < 5) {
		return put_sw(rsp, rsp_len, SW_WRONG_LENGTH);
	}
	uint8_t lc = apdu[4];
	if (lc != AID_LEN || len < 5u + lc) {
		return put_sw(rsp, rsp_len, SW_WRONG_LENGTH);
	}
	if (crypto_equal(apdu + 5, aid, AID_LEN) != 0) {
		selected = false;
		return put_sw(rsp, rsp_len, SW_FILE_NOT_FOUND);
	}
	selected = true;
	nonce_valid = false;
	return put_sw(rsp, rsp_len, SW_OK);
}

static int handle_get_challenge(uint8_t *rsp, size_t rsp_len)
{
	if (!selected) {
		return put_sw(rsp, rsp_len, SW_DENIED);
	}
	if (rsp_len < NONCE_LEN + 2) {
		return -ENOMEM;
	}
	/* TRNG 硬件源。失败必须拒绝而不是退化成弱随机 —— 可预测的 nonce
	 * 让整个挑战应答退化成固定密码。 */
	if (crypto_random(cur_nonce, NONCE_LEN) != 0) {
		LOG_ERR("TRNG 取随机失败");
		nonce_valid = false;
		return put_sw(rsp, rsp_len, SW_DENIED);
	}
	nonce_valid = true;
	memcpy(rsp, cur_nonce, NONCE_LEN);
	rsp[NONCE_LEN] = 0x90;
	rsp[NONCE_LEN + 1] = 0x00;
	return NONCE_LEN + 2;
}

static int handle_unlock(const uint8_t *apdu, size_t len,
			 uint8_t *rsp, size_t rsp_len)
{
	/* 80 10 00 00 Lc [ uid(4) || counter(4) || mac(16) ] 00 */
	const size_t body_len = 4 + 4 + MAC_LEN;

	/* 失败一律 SW_DENIED，且不区分原因（§5.2：不给攻击者区分信道）。
	 * 长度错是唯一例外 —— 那是协议层错误，不泄露任何密码学信息。 */
	if (len < 5) {
		return put_sw(rsp, rsp_len, SW_WRONG_LENGTH);
	}
	uint8_t lc = apdu[4];
	if (lc != body_len || len < 5u + lc) {
		return put_sw(rsp, rsp_len, SW_WRONG_LENGTH);
	}

	if (!selected || !nonce_valid) {
		LOG_WRN("拒绝：没有待验的 nonce（可能是重放）");
		return put_sw(rsp, rsp_len, SW_DENIED);
	}

	/* nonce 一次性：无论后面验成不验成，这个 nonce 到此作废。
	 * 放在最前面而不是最后，是为了让任何提前 return 都不会漏掉它。 */
	nonce_valid = false;

	const uint8_t *body = apdu + 5;
	uint32_t uid = ((uint32_t)body[0] << 24) | ((uint32_t)body[1] << 16) |
		       ((uint32_t)body[2] << 8) | body[3];
	uint32_t counter = ((uint32_t)body[4] << 24) | ((uint32_t)body[5] << 16) |
			   ((uint32_t)body[6] << 8) | body[7];
	const uint8_t *mac = body + 8;

	/* users_lock：校验 + counter 更新必须是一个原子段（审计 H3）——
	 * 否则 find_user 拿到的指针可在 HMAC 期间被 uplink 线程的
	 * 密钥下发换主，counter 回写进错槽位。HMAC 是纯内存运算
	 * （CryptoCell，几十 µs 量级），持锁可接受；result_cb
	 * （会走 uplink_queue_event，可能阻塞 —— 审计 M3）和
	 * nvstore_queue_counter 都挪到锁外。 */
	bool ok = false;
	uint32_t known_counter = 0;
	k_mutex_lock(&users_lock, K_FOREVER);
	struct user_key *u = find_user_locked(uid);
	if (u == NULL) {
		LOG_WRN("拒绝：未知 uid=%u", uid);
	} else {
		/* mac = HMAC-SHA256(secret, nonce || counter || cmd)[0..15]
		 * cmd 就是 UNLOCK 的 CLA|INS 两字节 —— 把命令绑进 MAC，
		 * 这样一条 UNLOCK 的 MAC 不能被挪去当别的命令用。 */
		uint8_t msg[NONCE_LEN + 4 + 2];
		memcpy(msg, cur_nonce, NONCE_LEN);
		memcpy(msg + NONCE_LEN, body + 4, 4);
		msg[NONCE_LEN + 4] = apdu[0];
		msg[NONCE_LEN + 5] = apdu[1];

		int rc = crypto_hmac_verify(u->secret, SECRET_LEN, msg,
					    sizeof(msg), mac, MAC_LEN);
		crypto_wipe(msg, sizeof(msg));

		if (rc != 0) {
			LOG_WRN("拒绝：MAC 不对 uid=%u", uid);
		} else if (counter <= u->counter) {
			/* counter 严格递增。等于也拒 —— 等于就是重放。 */
			LOG_WRN("拒绝：counter 未递增 uid=%u 收到 %u 已有 %u",
				uid, counter, u->counter);
		} else {
			u->counter = counter;
			ok = true;
		}
		known_counter = u->counter;
	}
	k_mutex_unlock(&users_lock);

	if (!ok) {
		if (result_cb) {
			result_cb(false, uid);
		}
		return put_sw(rsp, rsp_len, SW_DENIED);
	}

	/* 落盘是异步的（这里不能阻塞，手机侧 presence check 只有 125 ms）。
	 * ⚠ 后果：counter 更新到 flash 之前掉电，那个 counter 值可以被重放一次。
	 * 这是有意的取舍 —— 同步写 flash 会让开锁超时失败，那是每天都会碰到的问题，
	 * 而「正好在开锁瞬间掉电且攻击者录到了那条报文」不是。 */
	nvstore_queue_counter(uid, counter);

	LOG_INF("开锁 uid=%u counter=%u（此前 %u）", uid, counter, known_counter);
	if (result_cb) {
		result_cb(true, uid);
	}
	return put_sw(rsp, rsp_len, SW_OK);
}

int unlock_handle_apdu(const uint8_t *apdu, size_t len,
		       uint8_t *rsp, size_t rsp_len)
{
	if (len < 4) {
		return put_sw(rsp, rsp_len, SW_WRONG_LENGTH);
	}

	uint8_t cla = apdu[0];
	uint8_t ins = apdu[1];

	if (cla == 0x00 && ins == 0xA4) {
		return handle_select(apdu, len, rsp, rsp_len);
	}
	if (cla == 0x00 && ins == 0x84) {
		return handle_get_challenge(rsp, rsp_len);
	}
	if (cla == 0x80 && ins == 0x10) {
		return handle_unlock(apdu, len, rsp, rsp_len);
	}

	return put_sw(rsp, rsp_len, SW_INS_NOT_SUPPORTED);
}

int unlock_init(void)
{
	k_mutex_init(&users_lock);
	memset(users, 0, sizeof(users));
	int rc = nvstore_load_users(users, MAX_USERS, &key_set_id);
	if (rc != 0 && rc != -ENOENT) {
		LOG_ERR("读密钥失败 rc=%d", rc);
		return rc;
	}

	size_t n = 0;
	for (size_t i = 0; i < MAX_USERS; i++) {
		if (users[i].valid) {
			n++;
		}
	}
	LOG_INF("载入 %zu 把密钥，kid=%u", n, key_set_id);
	if (n == 0) {
		/* DESIGN.md §5.4 第 2 条：离线首次配对没有方案。
		 * 当前假设第一把密钥随固件烧进去，没有就只能等 4G 下发。 */
		LOG_WRN("一把密钥都没有 —— BLE 开锁不可用，只能用机械钥匙");
	}
	unlock_session_reset();
	return 0;
}
