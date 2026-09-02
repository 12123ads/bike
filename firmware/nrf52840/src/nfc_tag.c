/* NFC 标签侧。见 nfc_tag.h 的 UICR 警告。 */

#include "nfc_tag.h"
#include "unlock.h"

#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <nfc_t4t_lib.h>

LOG_MODULE_REGISTER(nfc_tag, CONFIG_EBIKE_LOG_LEVEL);

/* 应答缓冲。最长的应答是 GET CHALLENGE 的 16 字节 nonce + 2 字节 SW。
 * 给 64 是留余量，nfc_t4t 的上限是 0xFFF0，不构成约束。 */
static uint8_t rsp_buf[64];
static bool active;

static void t4t_callback(void *context, nfc_t4t_event_t event,
			 const uint8_t *data, size_t data_length,
			 uint32_t flags)
{
	ARG_UNUSED(context);
	ARG_UNUSED(flags);

	switch (event) {
	case NFC_T4T_EVENT_FIELD_ON:
		/* 手机贴上来了。作废上一次会话残留的 nonce ——
		 * 不作废也不会不安全（nonce 用过即废），但会少一次无效往返。 */
		unlock_session_reset();
		LOG_DBG("NFC 场出现");
		break;

	case NFC_T4T_EVENT_FIELD_OFF:
		unlock_session_reset();
		LOG_DBG("NFC 场消失");
		break;

	case NFC_T4T_EVENT_DATA_IND: {
		/* ⚠ 这个回调必须快：手机侧 presence check 默认 125 ms
		 * （EXTRA_READER_PRESENCE_CHECK_DELAY），超了就断链。
		 * 所以 unlock_handle_apdu 里不写 flash（counter 是异步落盘的）。
		 *
		 * CONFIG_NFC_THREAD_CALLBACK=y 让回调跑在线程上下文而不是中断里，
		 * 所以这里可以调 PSA（CryptoCell 要能睡）。 */
		int n = unlock_handle_apdu(data, data_length,
					   rsp_buf, sizeof(rsp_buf));
		if (n < 0) {
			LOG_ERR("APDU 处理失败 %d", n);
			/* 回一个明确的拒绝，而不是不回 —— 不回会让手机等到超时，
			 * 用户看到的是「贴了没反应」而不是「被拒绝了」。 */
			rsp_buf[0] = 0x69;
			rsp_buf[1] = 0x82;
			n = 2;
		}
		int rc = nfc_t4t_response_pdu_send(rsp_buf, (size_t)n);
		if (rc != 0) {
			LOG_ERR("回应答失败 rc=%d", rc);
		}
		break;
	}

	case NFC_T4T_EVENT_DATA_TRANSMITTED:
		break;

	default:
		LOG_DBG("NFC 事件 %d", (int)event);
		break;
	}
}

int nfc_tag_init(void)
{
	/* 第二参数 NULL = raw ISO-DEP（默认模式），不做 NDEF 封装 */
	int rc = nfc_t4t_setup(t4t_callback, NULL);
	if (rc != 0) {
		LOG_ERR("nfc_t4t_setup 失败 rc=%d", rc);
		return rc;
	}
	LOG_INF("NFC 标签就绪（raw ISO-DEP）");
	return 0;
}

int nfc_tag_start(void)
{
	if (active) {
		return 0;
	}
	int rc = nfc_t4t_emulation_start();
	if (rc != 0) {
		LOG_ERR("nfc_t4t_emulation_start 失败 rc=%d", rc);
		return rc;
	}
	active = true;
	return 0;
}

int nfc_tag_stop(void)
{
	if (!active) {
		return 0;
	}
	int rc = nfc_t4t_emulation_stop();
	if (rc != 0) {
		LOG_ERR("nfc_t4t_emulation_stop 失败 rc=%d", rc);
		return rc;
	}
	active = false;
	unlock_session_reset();
	return 0;
}

bool nfc_tag_is_active(void)
{
	return active;
}
