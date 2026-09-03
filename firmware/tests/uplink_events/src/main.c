/*
 * 事件队列锁纪律的运行时测试。
 *
 * 被测的是 firmware/nrf52840/src/uplink.c 的原件。`flush_events()` 是 static，
 * 所以从公开面走进去：`uplink_queue_event()` 排事件，`uplink_cycle()` 冲刷。
 *
 * 被钉住的缺陷（2026-09-03 审计 M3）：`flush_events` 曾经**持 `ev_lock`
 * 跨网络 I/O** —— 锁内调 `publish_retry`，而它含最坏三分钟的重连阶梯。
 *
 * 为什么必须有这个测试：持锁发送在功能上完全正确，事件照样发出去，
 * 任何「事件到达」的断言都是绿的。区别只在阻塞期间谁被卡住：
 *   · LIS2DW12 驱动线程的 `on_motion → uplink_queue_event`
 *     （motion.c 自己写着「阻塞期间的中断会丢」）
 *   · 系统工作队列上的 `still_work`、`sense_work`、nvstore 的 `counter_work`
 *     —— 包括 `lock.c` 的**释放脉冲** `release_work`，被卡就是电磁锁
 *     持续通电（几百 mA）
 * 而且恰在「车被撬、事件密集、网络又差」的时刻。
 *
 * 所以判据是「发送阻塞时，另一个线程还能不能入队」，而不是「事件发出去了」。
 *
 * 第二条被钉住的（审计 M7）：`unlock_del_secret` / `unlock_wipe_secrets`
 * 的下行路径 —— 见 test_secret_del_and_wipe_reach_unlock_layer。
 */

#include "fakes.h"
#include "uplink.h"
#include "proto.h"

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

/* 抢锁的线程 —— 优先级比 ztest 主线程高一点，好让它在 publish 卡住的
 * 窗口里真的跑起来。 */
#define PROBE_STACK 2048
K_THREAD_STACK_DEFINE(probe_stack, PROBE_STACK);
static struct k_thread probe_thread;

/* 探针结果 */
static struct {
	int64_t queued_at_ms;     /* 入队返回的时刻 */
	int rc;
	bool done;
} probe;

static void probe_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* 这就是 on_motion 在真设备上做的事 */
	probe.rc = uplink_queue_event(EV_MOTION, "{\"mg\":260}");
	probe.queued_at_ms = k_uptime_get();
	probe.done = true;
}

static void *setup(void)
{
	fake_reset();
	zassert_ok(uplink_init(), "uplink_init 失败");
	return NULL;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_reset();
	memset(&probe, 0, sizeof(probe));
	/* uplink_init 已经跑过（setup），fake_reset 把 dn_cb 清了 ——
	 * 重新注册，否则下行测试收不到东西。 */
	zassert_ok(uplink_init(), "uplink_init 失败");
}

ZTEST_SUITE(uplink_events, NULL, setup, before, NULL, NULL);

/* --- 正常路径：先证明链路真的通 --------------------------------------------- */

/* 后面的断言都是「阻塞时能不能怎样」，「一律不发」也会绿。这条防那个。 */
ZTEST(uplink_events, test_queued_event_is_published_next_cycle)
{
	zassert_ok(uplink_queue_event(EV_MOTION, "{\"mg\":260}"), "入队失败");

	zassert_ok(uplink_cycle(false), "一轮上报失败");

	bool found = false;

	for (size_t i = 0; i < fake.publish_n; i++) {
		if (strstr(fake.published[i].topic, "up/event") != NULL &&
		    strstr(fake.published[i].payload, "\"e\":\"motion\"") != NULL) {
			found = true;
			zassert_not_null(strstr(fake.published[i].payload,
						"\"mg\":260"),
					 "detail 丢了：%s",
					 fake.published[i].payload);
		}
	}
	zassert_true(found, "排队的事件没发出去（发了 %zu 条）", fake.publish_n);
}

/* 发成功的事件必须从队列里销掉，不能下一轮重发。 */
ZTEST(uplink_events, test_published_event_is_not_resent)
{
	zassert_ok(uplink_queue_event(EV_STILL, NULL), "入队失败");
	zassert_ok(uplink_cycle(false), "第一轮失败");

	size_t after_first = fake.publish_n;

	zassert_ok(uplink_cycle(false), "第二轮失败");

	for (size_t i = after_first; i < fake.publish_n; i++) {
		zassert_is_null(strstr(fake.published[i].topic, "up/event"),
				"已发成功的事件被重发了");
	}
}

/* 发失败的事件要留到下一轮 —— (dev,q) 去重让重发安全（契约 §5）。 */
ZTEST(uplink_events, test_failed_event_is_retried_next_cycle)
{
	zassert_ok(uplink_queue_event(EV_LOWBATT, "{\"lv\":1,\"v\":46.2}"),
		   "入队失败");

	fake.publish_rc = -EIO;      /* 全都发失败 */
	fake.connected = true;       /* 会话还在 —— publish_retry 不走重连 */
	(void)uplink_cycle(false);

	fake.publish_rc = 0;
	fake.publish_n = 0;
	zassert_ok(uplink_cycle(false), "第二轮失败");

	bool found = false;

	for (size_t i = 0; i < fake.publish_n; i++) {
		if (strstr(fake.published[i].payload, "\"e\":\"lowbatt\"") != NULL) {
			found = true;
		}
	}
	zassert_true(found, "发失败的事件没留到下一轮 —— 事件被丢了");
}

/* --- M3：发送期间绝不持 ev_lock ---------------------------------------------- */

/* **这条是 M3 的核心。**
 *
 * 卡住 `up/event` 的那次 publish（模拟重连阶梯），同时让另一个线程调
 * `uplink_queue_event()` —— 那正是 LIS2DW12 驱动线程在真设备上做的事。
 *
 * - 修复前：`flush_events` 持 `ev_lock` 调 publish，探针线程在
 *   `k_mutex_lock(&ev_lock)` 上睡到 publish 返回，`queued_at_ms` 落在
 *   放行之后。
 * - 修复后：锁内只做快照，探针立刻拿到锁，`queued_at_ms` 落在放行之前。
 *
 * 判据是**时刻**而不是「最终有没有入队」：持锁的实现最终也会入队，
 * 只是晚了三分钟 —— 而那三分钟里锁释放脉冲也排在后面。
 */
ZTEST(uplink_events, test_queue_event_not_blocked_by_publish_in_flight)
{
	zassert_ok(uplink_queue_event(EV_UNLOCK_DENY, "{\"uid\":7}"),
		   "预置事件失败");

	/* 卡住事件那一条 publish */
	strcpy(fake.block_publish_on_topic, "up/event");

	/* 后台跑一轮上报 —— 它会在 up/event 那条 publish 上停住 */
	static struct k_thread cycle_thread;
	static K_THREAD_STACK_DEFINE(cycle_stack, 4096);

	k_thread_create(&cycle_thread, cycle_stack, K_THREAD_STACK_SIZEOF(cycle_stack),
			(k_thread_entry_t)(void *)uplink_cycle,
			(void *)false, NULL, NULL,
			K_PRIO_PREEMPT(7), 0, K_NO_WAIT);

	/* 等它真的进到 publish 里 */
	zassert_ok(k_sem_take(&fake.publish_entered, K_SECONDS(5)),
		   "publish 没被卡住 —— 这个探针测不出 M3，别信下面的断言");

	int64_t blocked_at = k_uptime_get();

	/* 现在「网络卡着」。传感器线程要入队一条新事件。 */
	k_thread_create(&probe_thread, probe_stack, PROBE_STACK,
			probe_fn, NULL, NULL, NULL,
			K_PRIO_PREEMPT(6), 0, K_NO_WAIT);

	/* 给它 500 ms —— 真设备上 publish 会卡几十秒到三分钟，
	 * 这里只要能区分「立刻」和「等 publish 结束」就够。 */
	k_sleep(K_MSEC(500));

	bool queued_while_blocked = probe.done;
	int64_t queued_at = probe.queued_at_ms;

	/* 放 publish 走，收尾 */
	k_sem_give(&fake.publish_gate);
	(void)k_thread_join(&cycle_thread, K_SECONDS(10));
	(void)k_thread_join(&probe_thread, K_SECONDS(10));

	zassert_true(queued_while_blocked,
		     "publish 阻塞期间入队被卡住了 —— flush_events 持 ev_lock "
		     "跨网络 I/O（审计 M3）。真设备上这一卡最坏三分钟，"
		     "期间锁释放脉冲也排在后面 = 电磁锁持续通电");
	zassert_true(queued_at - blocked_at < 400,
		     "入队等了 %lld ms —— 说明它在等锁",
		     (long long)(queued_at - blocked_at));
	zassert_ok(probe.rc, "入队返回 %d", probe.rc);
}

/* 反向护栏：阻塞期间入队的那条事件不能丢，下一轮要发出去。
 * 上一条修法如果写成「锁外发送但快照期间的新事件被覆盖」，这条会红。 */
ZTEST(uplink_events, test_event_queued_during_publish_survives)
{
	zassert_ok(uplink_queue_event(EV_UNLOCK_DENY, "{\"uid\":7}"),
		   "预置事件失败");
	strcpy(fake.block_publish_on_topic, "up/event");

	static struct k_thread cycle_thread;
	static K_THREAD_STACK_DEFINE(cycle_stack, 4096);

	k_thread_create(&cycle_thread, cycle_stack, K_THREAD_STACK_SIZEOF(cycle_stack),
			(k_thread_entry_t)(void *)uplink_cycle,
			(void *)false, NULL, NULL,
			K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	zassert_ok(k_sem_take(&fake.publish_entered, K_SECONDS(5)),
		   "publish 没被卡住");

	/* 阻塞期间入队一条 boot（用一个前面没出现过的类型，好认） */
	zassert_ok(uplink_queue_event(EV_BOOT, NULL), "阻塞期间入队失败");

	k_sem_give(&fake.publish_gate);
	(void)k_thread_join(&cycle_thread, K_SECONDS(10));

	/* 下一轮必须把它发出去 */
	fake.publish_n = 0;
	zassert_ok(uplink_cycle(false), "第二轮失败");

	bool found = false;

	for (size_t i = 0; i < fake.publish_n; i++) {
		if (strstr(fake.published[i].payload, "\"e\":\"boot\"") != NULL) {
			found = true;
		}
	}
	zassert_true(found,
		     "发送期间入队的事件被丢了 —— 锁外发送把新事件的槽位清掉了");
}

/* --- M7：密钥删除/清空的下行路径 --------------------------------------------- */

/* `unlock_del_secret` / `unlock_wipe_secrets` 必须真的被 `dn/secret` 触发。
 *
 * 审计 M7 修的是那两个函数**内部**要清 `counter`（防槽位复用继承旧值把新
 * 手机锁在门外）。那一半属于 unlock.c，这里钉的是**上行下行的接线**：
 * 报文 → 正确的函数 → ack。接线断了的话 M7 的修复根本跑不到。
 *
 * （counter 清零本身由 unlock.c 的 users_lock 段保证，bsim 的 8 条断言覆盖
 * 开锁路径；「删了再加新用户不继承旧 counter」需要一个能操作 users[] 的
 * 测试应用，那是 unlock.c 自己的测试范围。）
 */
ZTEST(uplink_events, test_secret_del_and_wipe_reach_unlock_layer)
{
	zassert_not_null(fake.dn_cb, "modem_init 没拿到 dn_cb");

	const char *del = "{\"id\":\"s-1\",\"op\":\"del\",\"uid\":7}";

	fake.dn_cb("ebike/v1/bike01/dn/secret", (const uint8_t *)del,
		   strlen(del));
	zassert_equal(fake.del_secret_calls, 1, "del 没走到 unlock_del_secret");
	zassert_equal(fake.del_secret_uid, 7, "uid 传错：%u", fake.del_secret_uid);

	const char *wipe = "{\"id\":\"s-2\",\"op\":\"wipe\"}";

	fake.dn_cb("ebike/v1/bike01/dn/secret", (const uint8_t *)wipe,
		   strlen(wipe));
	zassert_equal(fake.wipe_calls, 1, "wipe 没走到 unlock_wipe_secrets");

	/* 两条都要 ack —— 服务端靠 ack 销账，不 ack 就永久重发 */
	int acks = 0;

	for (size_t i = 0; i < fake.publish_n; i++) {
		if (strstr(fake.published[i].topic, "up/ack") != NULL) {
			acks++;
			zassert_not_null(strstr(fake.published[i].payload,
						"\"ok\":1"),
					 "ack 报了失败：%s",
					 fake.published[i].payload);
		}
	}
	zassert_equal(acks, 2, "只发了 %d 条 ack", acks);
}

/* 密钥层返回失败时，ack 必须报失败 —— 报成功会让服务端销账，
 * 而设备上那把密钥其实没写进去。 */
ZTEST(uplink_events, test_secret_failure_is_acked_as_failure)
{
	fake.secret_rc = -ENOENT;

	const char *del = "{\"id\":\"s-3\",\"op\":\"del\",\"uid\":9}";

	fake.dn_cb("ebike/v1/bike01/dn/secret", (const uint8_t *)del,
		   strlen(del));

	bool saw_fail = false;

	for (size_t i = 0; i < fake.publish_n; i++) {
		if (strstr(fake.published[i].topic, "up/ack") != NULL &&
		    strstr(fake.published[i].payload, "\"ok\":0") != NULL) {
			saw_fail = true;
		}
	}
	zassert_true(saw_fail, "密钥操作失败却 ack 成功 —— 服务端会错误销账");
}
