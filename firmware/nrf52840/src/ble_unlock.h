/*
 * BLE 开锁通道：nRF52840 当 GATT peripheral，手机当 central（DESIGN.md §2）。
 *
 * 这一层**只是传输**。挑战应答、三重校验、counter 持久化全在 unlock.c，
 * 一行都没改 —— 换传输不动密码学，是这次改动能安全落地的前提。
 * 手机把和原来 NFC 完全相同的 APDU 写进 CMD 特征，应答从 RSP 特征 notify 回去：
 *
 *   手机 → CMD (write req)   : 00 A4 04 00 07 F0 45 42 49 4B 45 01 00   （SELECT AID）
 *   RSP  → 手机 (notify)     : 90 00
 *   手机 → CMD               : 00 84 00 00 10                          （GET CHALLENGE）
 *   RSP  → 手机              : nonce(16) || 90 00
 *   手机 → CMD               : 80 10 00 00 18 [uid||counter||mac] 00    （UNLOCK）
 *   RSP  → 手机              : 90 00 开 / 69 82 拒
 *
 * ⚠ 最长一条 C-APDU 是 UNLOCK 的 **30 字节**，而默认 ATT_MTU=23 只给
 *   20 字节可写载荷（ATT_MTU − 3）。所以 prj.conf 里 **必须**有
 *   CONFIG_BT_L2CAP_TX_MTU / CONFIG_BT_BUF_ACL_RX_SIZE 那一对，见那里的算式。
 *
 * ⚠ 手机侧要先写 RSP 的 CCCD 订阅通知才收得到应答 ——
 *   CONFIG_BT_GATT_ENFORCE_SUBSCRIPTION 默认 y（host/Kconfig.gatt:169-176），
 *   没订阅时 bt_gatt_notify() 直接返回 -EINVAL。这是每次连接多一个往返的来源。
 *
 * ⚠ **没有 4 cm 物理近场了。** NFC 的作用距离本身是防中继的最强一环，
 *   改 BLE 之后这一层消失，密码学挡不住链路层中继（ADR-004 §4）。
 *   所以：不做「走近自动开锁」，开锁必须由手机上的显式动作触发。
 */

#ifndef EBIKE_BLE_UNLOCK_H
#define EBIKE_BLE_UNLOCK_H

#include <stdbool.h>

/* 起协议栈（bt_enable）并注册连接回调。不开广播。 */
int ble_unlock_init(void);

/* 开/关可连接广播。
 *
 * 广播本身很便宜 —— 100~150 ms 间隔约 130 µA、1 s 间隔约 16 µA
 * （ADR-004 §3 的手算），淹没在 4G 模组 500~1500 µA 的地板里。
 * **所以静止后关广播不是为了省电，是为了不可被发现、不可被枚举、
 * 不可被沿街扫描长期跟踪一辆车。** 改动机的时候别把这条注释一起删了。 */
int ble_unlock_start(void);
int ble_unlock_stop(void);

bool ble_unlock_is_active(void);

/* 进 System OFF 之前调：停广播 + bt_disable()。
 *
 * 必须做，而且必须在 nvstore_flush() 之前：
 *   1. MPSL 占着 RTC0/TIMER0/RADIO，radio 也是 EasyDMA master，
 *      产品规格书要求进 System OFF 前 EasyDMA 传输已结束；
 *   2. SOC_FLASH_NRF_RADIO_SYNC_MPSL 会让 flash 写等 MPSL timeslot，
 *      radio 还开着时 nvstore_flush() 的最坏延迟不可控。
 * bt_disable() 是同步的（底下 sdc_disable() 返回即代表 controller 已停）。 */
void ble_unlock_shutdown(void);

#endif /* EBIKE_BLE_UNLOCK_H */
