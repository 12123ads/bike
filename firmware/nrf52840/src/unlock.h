/*
 * 开锁的挑战应答 —— DESIGN.md §5.2。这是整个固件里唯一「错了就有人偷走车」的部分。
 *
 * 三步 APDU（手机是读卡器，车是标签）：
 *   1) SELECT AID    : 00 A4 04 00 07  F0 45 42 49 4B 45 01  00
 *   2) GET CHALLENGE : 00 84 00 00 10          → nonce(16) || 90 00
 *   3) UNLOCK        : 80 10 00 00 Lc [ uid(4) || counter(4) || mac(16) ] 00
 *                                              → 90 00 开 / 69 82 拒
 *
 *   mac = HMAC-SHA256(secret, nonce || counter || cmd)[0..15]
 *
 * 三重校验全过才开：MAC 正确、counter 严格递增、nonce 一次性。
 */

#ifndef EBIKE_UNLOCK_H
#define EBIKE_UNLOCK_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define AID_LEN            7
#define NONCE_LEN          16
#define MAC_LEN            16
#define SECRET_LEN         32
#define MAX_USERS          8

/* APDU 状态字 */
#define SW_OK              0x9000
#define SW_DENIED          0x6982   /* 一律回这个，不区分失败原因（§5.2） */
#define SW_WRONG_LENGTH    0x6700
#define SW_FILE_NOT_FOUND  0x6A82
#define SW_INS_NOT_SUPPORTED 0x6D00

/* 一个用户的密钥槽。放在头文件里是因为 nvstore 要按这个结构落盘。
 *
 * ⚠ 这个结构的**二进制布局就是 flash 上的格式**。改字段顺序或类型
 * 会让老设备读出乱码密钥 —— 加字段要同时升 NVSTORE_USERS_VERSION。 */
struct user_key {
	uint32_t uid;
	uint8_t secret[SECRET_LEN];
	uint32_t counter;    /* 已接受过的最大值；必须严格递增 */
	bool valid;
};

/* 处理一条来自手机的 APDU，把应答写进 rsp。
 * 返回应答长度；缓冲不够返回负数。
 *
 * 这个函数会被 NFC 回调直接调用，**必须不阻塞**：
 * 手机侧的 presence check 默认 125 ms，超了就断链。
 * 所以里面不做 flash 写入 —— counter 的持久化是异步交给 nvstore 的。
 */
int unlock_handle_apdu(const uint8_t *apdu, size_t len,
		       uint8_t *rsp, size_t rsp_len);

/* 每次进入 NFC 会话时调 —— 作废上一次发出去但没用掉的 nonce。
 * 不调也不会不安全（nonce 用过即废），但会让「贴一下走开再贴」多一次往返。 */
void unlock_session_reset(void);

/* 密钥管理（契约 §6.2 的下行落到这里） */
int unlock_set_secret(uint32_t uid, const uint8_t secret[SECRET_LEN], uint16_t kid);
int unlock_del_secret(uint32_t uid);
int unlock_wipe_secrets(void);
uint16_t unlock_current_kid(void);

/* 开锁成功/失败的回调，由 main 注册，用来发事件和驱动锁 */
typedef void (*unlock_result_cb)(bool ok, uint32_t uid);
void unlock_set_callback(unlock_result_cb cb);

int unlock_init(void);

#endif /* EBIKE_UNLOCK_H */
