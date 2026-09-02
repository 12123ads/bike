/* PSA Crypto 封装。见 crypto.h 的两条硬要求。 */

#include "crypto.h"

#include <string.h>
#include <errno.h>

#include <psa/crypto.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ebike_crypto, CONFIG_EBIKE_LOG_LEVEL);

static bool psa_ready;

int crypto_init(void)
{
	psa_status_t st = psa_crypto_init();
	if (st != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init 失败 %d", (int)st);
		return -EIO;
	}
	psa_ready = true;
	return 0;
}

int crypto_random(uint8_t *out, size_t len)
{
	if (!psa_ready) {
		return -EAGAIN;
	}
	psa_status_t st = psa_generate_random(out, len);
	if (st != PSA_SUCCESS) {
		LOG_ERR("psa_generate_random 失败 %d", (int)st);
		/* 失败时把缓冲抹掉：留着上一轮的内容更危险 —— 调用方
		 * 如果忘了判返回值，会拿一个可预测的值当 nonce 用。 */
		crypto_wipe(out, len);
		return -EIO;
	}
	return 0;
}

int crypto_hmac_verify(const uint8_t *key, size_t key_len,
		       const uint8_t *msg, size_t msg_len,
		       const uint8_t *mac, size_t mac_len)
{
	if (!psa_ready) {
		return -EAGAIN;
	}

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t kid = PSA_KEY_ID_NULL;
	int ret = -EACCES;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_MESSAGE);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
	psa_set_key_algorithm(&attr, PSA_ALG_TRUNCATED_MAC(PSA_ALG_HMAC(PSA_ALG_SHA_256),
							   mac_len));
	psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
	psa_set_key_bits(&attr, key_len * 8);

	psa_status_t st = psa_import_key(&attr, key, key_len, &kid);
	if (st != PSA_SUCCESS) {
		LOG_ERR("psa_import_key 失败 %d", (int)st);
		goto out;
	}

	/* psa_mac_verify 内部做常数时间比较 —— 不要自己取 MAC 再 memcmp。 */
	st = psa_mac_verify(kid, PSA_ALG_TRUNCATED_MAC(PSA_ALG_HMAC(PSA_ALG_SHA_256),
						       mac_len),
			    msg, msg_len, mac, mac_len);
	ret = (st == PSA_SUCCESS) ? 0 : -EACCES;

out:
	if (kid != PSA_KEY_ID_NULL) {
		/* 销毁易失密钥：让它留在 CryptoCell 的密钥槽里没有好处，
		 * 而且槽位是有限的，泄漏会让后续 import 失败。 */
		(void)psa_destroy_key(kid);
	}
	psa_reset_key_attributes(&attr);
	return ret;
}

int crypto_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
	uint8_t diff = 0;
	for (size_t i = 0; i < len; i++) {
		diff |= (uint8_t)(a[i] ^ b[i]);
	}
	return diff == 0 ? 0 : -1;
}

void crypto_wipe(void *p, size_t len)
{
	/* volatile 指针防止编译器把这段写操作优化掉（memset 会被优化）。 */
	volatile uint8_t *v = (volatile uint8_t *)p;
	while (len--) {
		*v++ = 0;
	}
}
