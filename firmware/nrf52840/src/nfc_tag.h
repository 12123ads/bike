/*
 * NFC 标签侧：raw ISO-DEP，手机当读卡器（DESIGN.md §2.1 的硬约束）。
 *
 * nfc_t4t_setup(cb, NULL) 的第二参数传 NULL 就是 raw ISO-DEP 模式（默认），
 * 不做 NDEF 封装，回调里直接收 APDU。
 *
 * ⚠ 用 NFC 引脚要写 UICR.NFCPINS，那是**一次性**的（DESIGN.md §3.3）：
 *   nrfjprog -f NRF52 --memrd 0x1000120C     # 先看现状
 *   nrfjprog -f NRF52 --recover              # 要改回来只能这样
 *   nrfjprog -f NRF52 --program nice_nano_bootloader-...hex --chiperase
 * 而 --recover 会连带擦掉 UICR.REGOUT0（3.3V 设置），不重刷 bootloader 板子起不来。
 */

#ifndef EBIKE_NFC_TAG_H
#define EBIKE_NFC_TAG_H

int nfc_tag_init(void);

/* 开始监听。NFCT ACTIVATED 是 400 µA（DESIGN.md §2.5），所以不是一直开着 ——
 * 由运动唤醒触发，静止一段时间后 stop。 */
int nfc_tag_start(void);
int nfc_tag_stop(void);

bool nfc_tag_is_active(void);

#endif /* EBIKE_NFC_TAG_H */
