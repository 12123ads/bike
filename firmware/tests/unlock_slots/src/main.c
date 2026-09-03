/*
 * 密钥槽位复用的运行时测试。
 *
 * 被测的是 firmware/nrf52840/src/unlock.c 的原件 + 真的 PSA crypto。
 * `users[]` 是 static，所以全部从公开面走：`unlock_set_secret` /
 * `unlock_del_secret` / `unlock_wipe_secrets` 改密钥表，
 * `unlock_handle_apdu` 走完整的 SELECT → GET CHALLENGE → UNLOCK 三步。
 *
 * 被钉住的缺陷（2026-09-03 审计 M7）：`unlock_del_secret` / `wipe` 没清
 * `counter`。
 *
 * 后果**不是拒绝攻击者，而是拒绝一个合法的新手机**：
 *   1. 旧手机 uid=1 用到 counter=5000（一天开几十次，几个月就这个量级）
 *   2. 手机丢了，服务端下发 `{"op":"del","uid":1}`
 *   3. 新手机 uid=2 配进同一个槽位（`unlock_set_secret` 找第一个 !valid）
 *   4. 新手机从 counter=1 开始 —— 全部撞上 `counter <= u->counter` 被拒
 * 而日志只说「counter 未递增」，看起来像重放攻击。
 *
 * 上一轮的验证方式写的是「固件编译 + bsim」，但 bsim 的 8 条断言里没有
 * del/wipe 场景 —— 把 `counter = 0` 那两行删掉，那 8 条照样全过。
 *
 * nvstore 是假的（见文件末尾）：它要 flash，而这里测的是内存里 users[]
 * 的槽位语义。crypto 是真的 —— MAC 必须真的能过，否则测的就不是 counter。
 */

#include "unlock.h"
#include "crypto.h"
#include "nvstore.h"

#include <string.h>
#include <errno.h>

#include <psa/crypto.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

/* --- 假 nvstore ------------------------------------------------------------- */

/* unlock.c 只用这三个。落盘内容本身不是这里的被测对象 ——
 * 但 `nvstore_save_users` 收到的**快照**是：它必须反映清零后的 counter，
 * 否则重启后旧值又回来了。 */
static struct user_key saved[MAX_USERS];
static uint16_t saved_kid;
static int save_calls;

int nvstore_save_users(const struct user_key *users, size_t n, uint16_t kid)
{
	zassert_equal(n, MAX_USERS, "快照长度不对：%zu", n);
	memcpy(saved, users, sizeof(saved));
	saved_kid = kid;
	save_calls++;
	return 0;
}

int nvstore_load_users(struct user_key *users, size_t n, uint16_t *kid)
{
	ARG_UNUSED(users);
	ARG_UNUSED(n);
	ARG_UNUSED(kid);
	return -ENOENT;      /* 空表启动 */
}

static uint32_t queued_counter_uid;
static uint32_t queued_counter_val;

void nvstore_queue_counter(uint32_t uid, uint32_t counter)
{
	queued_counter_uid = uid;
	queued_counter_val = counter;
}

/* --- 测试用的密钥与 APDU ----------------------------------------------------- */

#define UID_OLD 1u
#define UID_NEW 2u

static const uint8_t SECRET_A[SECRET_LEN] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
};
static const uint8_t SECRET_B[SECRET_LEN] = {
	0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
	0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
	0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7,
	0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
};

static const uint8_t AID[AID_LEN] = {
	0xF0, 0x45, 0x42, 0x49, 0x4B, 0x45, 0x01,
};

static uint8_t rsp[64];
static int rsp_len;

static uint16_t sw(void)
{
	zassert_true(rsp_len >= 2, "应答太短：%d", rsp_len);
	return (uint16_t)((rsp[rsp_len - 2] << 8) | rsp[rsp_len - 1]);
}

static void do_select(void)
{
	uint8_t apdu[5 + AID_LEN];

	apdu[0] = 0x00;
	apdu[1] = 0xA4;
	apdu[2] = 0x04;
	apdu[3] = 0x00;
	apdu[4] = AID_LEN;
	memcpy(&apdu[5], AID, AID_LEN);

	rsp_len = unlock_handle_apdu(apdu, sizeof(apdu), rsp, sizeof(rsp));
	zassert_equal(sw(), SW_OK, "SELECT 没回 9000：%04X", sw());
}

static void get_nonce(uint8_t out[NONCE_LEN])
{
	/* CLA=0x00 INS=0x84（unlock.c 的 dispatch），Le = 期望的 nonce 长度。
	 * 逐字对齐 ble_unlock_bsim 那份 —— 两个测试用同一条报文。 */
	static const uint8_t apdu[] = { 0x00, 0x84, 0x00, 0x00, NONCE_LEN };

	rsp_len = unlock_handle_apdu(apdu, sizeof(apdu), rsp, sizeof(rsp));
	zassert_equal(sw(), SW_OK, "GET CHALLENGE 没回 9000：%04X", sw());
	zassert_equal(rsp_len, NONCE_LEN + 2, "nonce 长度 %d", rsp_len);
	memcpy(out, rsp, NONCE_LEN);
}

/* 算 HMAC。消息布局**逐字对齐 unlock.c 的 handle_unlock**：
 *   msg = nonce(16) || counter(4, big-endian) || CLA(1) || INS(1)
 * 故意手写一遍而不是复用固件代码 —— 固件改了布局这里必须先红一次。 */
static void hmac16(const uint8_t secret[SECRET_LEN], const uint8_t *msg,
		   size_t msg_len, uint8_t out[MAC_LEN])
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key;
	uint8_t key_ram[SECRET_LEN];
	uint8_t full[32];
	size_t full_len = 0;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
	psa_set_key_bits(&attr, SECRET_LEN * 8);

	memcpy(key_ram, secret, SECRET_LEN);
	zassert_equal(psa_import_key(&attr, key_ram, SECRET_LEN, &key),
		      PSA_SUCCESS, "psa_import_key 失败");
	zassert_equal(psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256),
				      msg, msg_len, full, sizeof(full),
				      &full_len),
		      PSA_SUCCESS, "psa_mac_compute 失败");
	(void)psa_destroy_key(key);
	memcpy(out, full, MAC_LEN);      /* 截断到 16 字节（§5.2） */
}

/* 走一次 UNLOCK，返回状态字。 */
static uint16_t try_unlock(uint32_t uid, uint32_t counter,
			   const uint8_t secret[SECRET_LEN])
{
	uint8_t nonce[NONCE_LEN];
	uint8_t apdu[5 + 4 + 4 + MAC_LEN + 1];
	uint8_t msg[NONCE_LEN + 4 + 2];

	do_select();
	get_nonce(nonce);

	apdu[0] = 0x80;
	apdu[1] = 0x10;
	apdu[2] = 0x00;
	apdu[3] = 0x00;
	apdu[4] = 4 + 4 + MAC_LEN;
	sys_put_be32(uid, &apdu[5]);
	sys_put_be32(counter, &apdu[9]);
	apdu[5 + 4 + 4 + MAC_LEN] = 0x00;     /* Le */

	memcpy(msg, nonce, NONCE_LEN);
	sys_put_be32(counter, &msg[NONCE_LEN]);
	msg[NONCE_LEN + 4] = apdu[0];
	msg[NONCE_LEN + 5] = apdu[1];
	hmac16(secret, msg, sizeof(msg), &apdu[13]);

	rsp_len = unlock_handle_apdu(apdu, sizeof(apdu), rsp, sizeof(rsp));
	return sw();
}

static void *setup(void)
{
	/* ⚠ 必须走 `crypto_init()` 而不是直接 `psa_crypto_init()`：
	 * crypto.c 有一个 `psa_ready` 门（crypto.c:15），只有 `crypto_init()`
	 * 会置上它。不置的话 `crypto_random` 返回 -EAGAIN →
	 * GET CHALLENGE 回 SW_DENIED，所有开锁用例都在第二步就红。 */
	zassert_ok(crypto_init(), "crypto_init 失败");
	zassert_ok(unlock_init(), "unlock_init 失败");
	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	/* 每条用例从空表开始 —— wipe 顺带把 counter 也清了（这正是被测行为
	 * 之一，所以下面每条用例仍然显式验证自己关心的那部分）。 */
	zassert_ok(unlock_wipe_secrets(), "wipe 失败");
	save_calls = 0;
	queued_counter_uid = 0;
	queued_counter_val = 0;
	memset(saved, 0, sizeof(saved));
}

ZTEST_SUITE(unlock_slots, NULL, setup, before, NULL, NULL);

/* --- 正常路径：先证明整条链真的通 ------------------------------------------- */

/* 后面全是「被拒/被接受」的对比断言，链路不通会让它们全绿。这条防那个。 */
ZTEST(unlock_slots, test_valid_unlock_succeeds_and_counter_advances)
{
	zassert_ok(unlock_set_secret(UID_OLD, SECRET_A, 1), "下发密钥失败");

	zassert_equal(try_unlock(UID_OLD, 1, SECRET_A), SW_OK,
		      "合法开锁被拒");
	zassert_equal(queued_counter_val, 1, "counter 没落盘排队");

	/* 严格递增：同一个 counter 再来一次要被拒（重放） */
	zassert_equal(try_unlock(UID_OLD, 1, SECRET_A), SW_DENIED,
		      "重放的 counter 被接受了");
	/* 递增之后可以 */
	zassert_equal(try_unlock(UID_OLD, 2, SECRET_A), SW_OK,
		      "递增后的 counter 被拒");
}

/* 错密钥必须被拒 —— 否则下面的「新密钥能开锁」断言不能说明任何事。 */
ZTEST(unlock_slots, test_wrong_secret_denied)
{
	zassert_ok(unlock_set_secret(UID_OLD, SECRET_A, 1), "下发密钥失败");
	zassert_equal(try_unlock(UID_OLD, 1, SECRET_B), SW_DENIED,
		      "用错密钥算的 MAC 被接受了");
}

/* --- M7：槽位复用不能继承旧 counter ----------------------------------------- */

/* **这条是 M7 的核心。**
 *
 * 场景就是「手机丢了换一台」：
 *   1. uid=1 用到 counter=5000
 *   2. `del` uid=1
 *   3. uid=2 配进同一个槽位（set_secret 找第一个 !valid，就是刚腾出来那个）
 *   4. uid=2 从 counter=1 开始 —— 必须能开锁
 *
 * 修复前：槽位的 `counter` 还是 5000，uid=2 的 counter=1 撞上
 * `counter <= u->counter` → SW_DENIED。**新手机永远开不了锁**，
 * 而日志说的是「counter 未递增」，看起来像有人在重放。 */
ZTEST(unlock_slots, test_deleted_slot_does_not_leak_counter_to_new_user)
{
	zassert_ok(unlock_set_secret(UID_OLD, SECRET_A, 1), "下发旧密钥失败");

	/* 旧手机用了很多次 —— 一天几十次，几个月就是这个量级 */
	zassert_equal(try_unlock(UID_OLD, 5000, SECRET_A), SW_OK,
		      "预置 counter 失败");

	/* 手机丢了 */
	zassert_ok(unlock_del_secret(UID_OLD), "删密钥失败");

	/* 新手机配进来 —— 会占刚腾出来的那个槽位 */
	zassert_ok(unlock_set_secret(UID_NEW, SECRET_B, 2), "下发新密钥失败");

	/* 新手机从 1 开始。这是全新的 uid，counter 必须从零算起。 */
	zassert_equal(try_unlock(UID_NEW, 1, SECRET_B), SW_OK,
		      "新用户复用槽位后继承了旧 counter —— "
		      "合法的新手机被永久拒绝（审计 M7）");
}

/* `wipe` 同理：清空之后所有槽位都该是干净的。 */
ZTEST(unlock_slots, test_wipe_clears_counter_too)
{
	zassert_ok(unlock_set_secret(UID_OLD, SECRET_A, 1), "下发密钥失败");
	zassert_equal(try_unlock(UID_OLD, 9999, SECRET_A), SW_OK,
		      "预置 counter 失败");

	zassert_ok(unlock_wipe_secrets(), "wipe 失败");

	/* 同一个 uid 重新配进来，counter 从 1 开始 */
	zassert_ok(unlock_set_secret(UID_OLD, SECRET_A, 3), "重新下发失败");
	zassert_equal(try_unlock(UID_OLD, 1, SECRET_A), SW_OK,
		      "wipe 之后 counter 没清 —— 重新配对的手机开不了锁（审计 M7）");
}

/* 清零必须**同时反映在落盘快照里**，否则重启后旧 counter 又回来了。 */
ZTEST(unlock_slots, test_cleared_counter_is_in_the_persisted_snapshot)
{
	zassert_ok(unlock_set_secret(UID_OLD, SECRET_A, 1), "下发密钥失败");
	zassert_equal(try_unlock(UID_OLD, 4242, SECRET_A), SW_OK,
		      "预置 counter 失败");

	memset(saved, 0xAA, sizeof(saved));      /* 先污染，确认真的被写过 */
	zassert_ok(unlock_del_secret(UID_OLD), "删密钥失败");

	for (size_t i = 0; i < MAX_USERS; i++) {
		zassert_false(saved[i].valid,
			      "快照里第 %zu 个槽位还是 valid", i);
		zassert_equal(saved[i].counter, 0,
			      "快照里第 %zu 个槽位的 counter 是 %u —— "
			      "重启后旧值会回来（审计 M7）",
			      i, saved[i].counter);
	}
}

/* 反向护栏：**同一个 uid** 换密钥时 counter **不该**清零。
 *
 * 这是有意的不对称（unlock.c 的注释写了）：换密钥不该让旧的重放报文重新
 * 可用。上一条修法如果写成「set 也清零」，这条会红。 */
ZTEST(unlock_slots, test_rekeying_same_uid_keeps_counter)
{
	zassert_ok(unlock_set_secret(UID_OLD, SECRET_A, 1), "下发密钥失败");
	zassert_equal(try_unlock(UID_OLD, 100, SECRET_A), SW_OK,
		      "预置 counter 失败");

	/* 同一个 uid，换一把密钥（密钥轮换，契约 §6.2） */
	zassert_ok(unlock_set_secret(UID_OLD, SECRET_B, 2), "轮换密钥失败");

	/* 旧 counter 必须还在：counter=100 是用过的，要被拒 */
	zassert_equal(try_unlock(UID_OLD, 100, SECRET_B), SW_DENIED,
		      "换密钥把 counter 清零了 —— 旧的重放报文重新可用");
	/* 递增之后可以 */
	zassert_equal(try_unlock(UID_OLD, 101, SECRET_B), SW_OK,
		      "轮换后的新密钥开不了锁");
}

/* 删掉的用户不能再开锁 —— 这是 del 的本职，和 counter 无关。 */
ZTEST(unlock_slots, test_deleted_user_cannot_unlock)
{
	zassert_ok(unlock_set_secret(UID_OLD, SECRET_A, 1), "下发密钥失败");
	zassert_ok(unlock_del_secret(UID_OLD), "删密钥失败");

	zassert_equal(try_unlock(UID_OLD, 1, SECRET_A), SW_DENIED,
		      "删掉的用户还能开锁");
}

/* 删一个不存在的 uid 要报错而不是静默成功 ——
 * 服务端靠 ack 判断「那台手机到底注销了没有」。 */
ZTEST(unlock_slots, test_deleting_unknown_uid_reports_error)
{
	zassert_equal(unlock_del_secret(4242), -ENOENT,
		      "删不存在的 uid 报了成功");
}
