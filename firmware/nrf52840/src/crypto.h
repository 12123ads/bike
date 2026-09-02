/*
 * PSA Crypto 封装 —— CryptoCell-310 硬件 HMAC-SHA256 和 TRNG。
 *
 * ⚠ 两条来自 DESIGN.md §4.2 的硬要求：
 *
 * 1. **CryptoCell DMA 只能到 SRAM**，不能直接喂 flash 里的常量。
 *    所以下面所有输入都要求调用方给栈上/静态 RAM 的缓冲。
 * 2. 原厂原话：`The device will not enter the System ON IDLE mode until
 *    CRYPTOCELL has been disabled`。**每次用完必须关**，否则整机停在
 *    毫安级而不是微安级 —— 功耗预算直接崩。
 *    下面的每个函数都是「开→算→关」的完整闭环，不给调用方留下忘记关的机会。
 */

#ifndef EBIKE_CRYPTO_H
#define EBIKE_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

int crypto_init(void);

/* TRNG。失败必须让调用方拒绝服务，不要退化成弱随机源。 */
int crypto_random(uint8_t *out, size_t len);

/* 验 MAC。**常数时间比较**，由 psa_mac_verify() 保证。
 * 返回 0 = 通过，非 0 = 不通过（不区分原因）。 */
int crypto_hmac_verify(const uint8_t *key, size_t key_len,
		       const uint8_t *msg, size_t msg_len,
		       const uint8_t *mac, size_t mac_len);

/* 常数时间比较，用于 AID 这类非密钥但也不想泄露前缀信息的比较。 */
int crypto_equal(const uint8_t *a, const uint8_t *b, size_t len);

/* 抹掉内存。**不要用 memset** —— 编译器会把「写完就不再读」的 memset 优化掉。 */
void crypto_wipe(void *p, size_t len);

#endif /* EBIKE_CRYPTO_H */
