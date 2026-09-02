/*
 * 持久化：密钥、counter、上行序号 q、启动计数、配置。
 *
 * 用 Zephyr settings + NVS（prj.conf 里 CONFIG_SETTINGS_NVS=y）。
 *
 * 两条容易出事的地方：
 *
 * 1. **`q` 掉电不能回零**（契约 §5）—— 服务端靠 (dev, q) 去重，回零会让
 *    新报文被当成重复的丢掉。但每条上行都写一次 flash 会磨穿它
 *    （NVS 扇区寿命 ~10 万次），所以按「预留一段」的办法：
 *    每次开机把 q 抬高一个 batch，中间只在 RAM 里递增。
 *    代价是掉电会跳掉一段 q，服务端不在乎（它只要求单调，不要求连续）。
 *
 * 2. **counter 落盘是异步的**（unlock.c 里解释了为什么不能同步）。
 *    这里做延迟合并写。
 */

#ifndef EBIKE_NVSTORE_H
#define EBIKE_NVSTORE_H

#include <stddef.h>
#include <stdint.h>

struct user_key;   /* unlock.h */

/* 每次开机预留的 q 区间大小。开机写一次 flash，之后 5000 条上行不写。
 * 省电档 15 分钟一报的话，够 52 天。 */
#define NVSTORE_Q_BATCH   5000

/* 密钥落盘格式的版本。**它真的参与校验**：落盘块是
 * `1 字节版本 + MAX_USERS 个 user_key`，读侧同时校总长度和版本号。
 * 改 struct user_key 的布局必须升这个数 —— 只靠长度校验的话，
 * 「布局改了但大小没变」会静默读出乱码密钥。
 * 升版本号之后老设备开机会 `LOG_ERR` 并当作没有密钥（只能用机械钥匙）。 */
#define NVSTORE_USERS_VERSION 1

int nvstore_init(void);

/* --- 上行序号 q --------------------------------------------------------------- */
uint32_t nvstore_next_q(void);

/* --- 启动计数（up/hello 的 boot 字段） --------------------------------------- */
uint32_t nvstore_boot_count(void);

/* --- 密钥 -------------------------------------------------------------------- */
int nvstore_save_users(const struct user_key *users, size_t n, uint16_t kid);
int nvstore_load_users(struct user_key *users, size_t n, uint16_t *kid);

/* counter 的延迟写。调用是廉价的（只更新 RAM + 排一个延迟工作）。 */
void nvstore_queue_counter(uint32_t uid, uint32_t counter);

/* 主动把待写的 counter 冲刷到 flash。进 System OFF 前调。 */
int nvstore_flush(void);

/* --- 运行时配置（可被 dn/cmd 的 interval / tier 改） ------------------------- */
uint32_t nvstore_report_interval(void);
int nvstore_set_report_interval(uint32_t seconds);

#endif /* EBIKE_NVSTORE_H */
