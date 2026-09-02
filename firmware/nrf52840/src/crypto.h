/*
 * PSA Crypto 封装 —— CryptoCell-310 硬件 HMAC-SHA256 和 TRNG。
 *
 * ⚠ 一条来自 DESIGN.md §4.2 的硬要求：
 *
 * **CryptoCell DMA 只能到 SRAM**，不能直接喂 flash 里的常量。
 * 所以下面所有输入都要求调用方给栈上/静态 RAM 的缓冲。
 *
 * 关于「用完必须关 CRYPTOCELL」（产品规格书原话：`The device will not enter
 * the System ON IDLE mode until CRYPTOCELL has been disabled`）——
 * **应用层不需要做任何事，本文件也刻意不做。** 核实结论（2026-09-02）：
 *
 * - nrfxlib 的 `libnrf_cc310_platform` 里 `CC_PalPowerSaveModeSelect` 是
 *   **引用计数式**的 `NRF_CRYPTOCELL->ENABLE` 开关（寄存器偏移 0x500，
 *   与 MDK 的 `NRF_CRYPTOCELL_Type.ENABLE` 对得上）。
 * - 真正驱动引擎的函数（`ProcessHashDrv` / `ProcessAesDrv` / …）在自己的
 *   函数体内成对 acquire/release，所以 `psa_mac_verify` 返回时计数已归 0、
 *   ENABLE 已经是 0。连异常路径都关：`CC_PalAbort` 也会写 ENABLE=0。
 * - 产品规格书 CRYPTOCELL 章自己也这么说：`The Nordic SDK software library
 *   automatically controls enabling and disabling of the CRYPTOCELL subsystem
 *   as a part of its function calls.`
 * - 开关的括号在引擎函数内部，**与密钥句柄生命周期无关** ——
 *   不存在「忘了 psa_destroy_key 就一直开着」这种陷阱。
 *
 * 所以别调 `mbedtls_psa_crypto_free()`（那只清软件 key store，硬件一个寄存器
 * 都不碰，还会把已 import 的密钥槽一起清掉）、别调
 * `nrf_cc3xx_platform_deinit()`（它的 `CC_HalTerminate` 是纯 no-op，
 * 对功耗零贡献，但会让后续 PSA 调用失去 RNG 上下文）、更别手写
 * `NRF_CRYPTOCELL->ENABLE = 0`（会踩掉库私有的引用计数与硬件状态的一致性）。
 *
 * 真正的开销在别处：链接进来的是 `no-interrupts` 变体，验签期间 CPU 忙等
 * `CRYPTO_BUSY`，那是几十微秒到毫秒级的**活跃电流**。要省只能减少验签次数。
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
