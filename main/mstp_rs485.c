#include "mstp_rs485.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "bacnet/npdu.h"
#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"
#include "bacnet/datalink/dlmstp.h"
#include "bacnet/datalink/mstp.h"
#include "bacnet/datalink/mstpdef.h"

#define MSTP_UART_PORT UART_NUM_2
#define MSTP_UART_TX_PIN GPIO_NUM_17
#define MSTP_UART_RX_PIN GPIO_NUM_16
#define MSTP_RS485_AUTO_DIRECTION 0
#define MSTP_RS485_HAS_DE_PIN 1
#define MSTP_UART_DE_PIN GPIO_NUM_18
#define MSTP_UART_BAUD_DEFAULT 38400U
#define MSTP_UART_RX_BUF_SIZE 4096
#define MSTP_UART_TX_BUF_SIZE 1024
#define MSTP_UART_TX_TIMEOUT_MS 100
#if MSTP_RS485_HAS_DE_PIN
#define MSTP_RS485_DE_PRE_TX_GUARD_MS 1
#define MSTP_RS485_DE_POST_TX_GUARD_MS 1
#else
#define MSTP_RS485_DE_PRE_TX_GUARD_MS 0
#define MSTP_RS485_DE_POST_TX_GUARD_MS 0
#endif

#ifndef BACNET_MSTP_TX_HEX_DEBUG
#define BACNET_MSTP_TX_HEX_DEBUG 0
#endif

#define BACNET_MSTP_TX_HEX_DEBUG_TARGET_MAC 40
#define BACNET_MSTP_TX_HEX_DEBUG_TARGET_DEVICE_INSTANCE 55525UL

/* MAX485-style modules use active-high DE. */
#ifndef MSTP_RS485_DE_ACTIVE_HIGH
#define MSTP_RS485_DE_ACTIVE_HIGH 1
#endif

static const char *TAG = "mstp_rs485";
static bool mstp_uart_initialized = false;
static volatile bool mstp_tx_in_progress = false;
static uint32_t mstp_baud_rate = MSTP_UART_BAUD_DEFAULT;
static int64_t mstp_last_activity_us = 0;
static volatile uint32_t mstp_rx_bytes = 0;
static volatile uint32_t mstp_preamble_55 = 0;
static volatile uint32_t mstp_preamble_55ff = 0;
static uint8_t mstp_prev_byte = 0;
static volatile uint32_t mstp_tx_frame_count = 0;

typedef struct {
    bool valid;
    uint8_t frame_type;
    bool is_data_frame;
    uint8_t pdu_type;
    bool has_invoke_id;
    uint8_t invoke_id;
    bool has_service_choice;
    uint8_t service_choice;
    uint16_t mstp_data_len;
    uint16_t apdu_frame_offset;
    int apdu_len;
} MSTP_TX_INFO;

#define MSTP_CONFIRMED_PENDING_MAX 16

static MSTP_RS485_CONFIRMED_REQUEST_META
    mstp_confirmed_pending[MSTP_CONFIRMED_PENDING_MAX] = { 0 };

static MSTP_RS485_CONFIRMED_REQUEST_META *mstp_confirmed_track_slot(uint8_t invoke_id)
{
    MSTP_RS485_CONFIRMED_REQUEST_META *free_slot = NULL;
    size_t i = 0;

    for (i = 0; i < MSTP_CONFIRMED_PENDING_MAX; i++) {
        if (mstp_confirmed_pending[i].valid &&
            mstp_confirmed_pending[i].invoke_id == invoke_id) {
            return &mstp_confirmed_pending[i];
        }
        if (!free_slot && !mstp_confirmed_pending[i].valid) {
            free_slot = &mstp_confirmed_pending[i];
        }
    }

    if (free_slot) {
        return free_slot;
    }

    return &mstp_confirmed_pending[0];
}

static MSTP_RS485_CONFIRMED_REQUEST_META *mstp_confirmed_find_slot(uint8_t invoke_id)
{
    size_t i = 0;

    for (i = 0; i < MSTP_CONFIRMED_PENDING_MAX; i++) {
        if (mstp_confirmed_pending[i].valid &&
            mstp_confirmed_pending[i].invoke_id == invoke_id) {
            return &mstp_confirmed_pending[i];
        }
    }

    return NULL;
}

static bool mstp_decode_tx_info(
    const uint8_t *payload,
    uint16_t payload_len,
    MSTP_TX_INFO *info)
{
    uint16_t data_len = 0;
    int npdu_offset = 0;
    uint8_t frame_type = 0;
    BACNET_ADDRESS dest = { 0 };
    BACNET_ADDRESS src = { 0 };
    BACNET_NPDU_DATA npdu_data = { 0 };
    uint8_t *apdu = NULL;

    if (!info) {
        return false;
    }
    memset(info, 0, sizeof(*info));

    if (!payload || (payload_len < 10)) {
        return false;
    }
    if ((payload[0] != 0x55) || (payload[1] != 0xFF)) {
        return false;
    }

    frame_type = payload[2];
    info->frame_type = frame_type;
    if ((frame_type != FRAME_TYPE_BACNET_DATA_EXPECTING_REPLY) &&
        (frame_type != FRAME_TYPE_BACNET_DATA_NOT_EXPECTING_REPLY) &&
        (frame_type != FRAME_TYPE_BACNET_EXTENDED_DATA_EXPECTING_REPLY) &&
        (frame_type != FRAME_TYPE_BACNET_EXTENDED_DATA_NOT_EXPECTING_REPLY)) {
        return false;
    }
    info->is_data_frame = true;

    data_len = ((uint16_t)payload[5] << 8) | payload[6];
    if ((uint32_t)8 + (uint32_t)data_len > (uint32_t)payload_len) {
        return false;
    }

    npdu_offset = bacnet_npdu_decode(&payload[8], data_len, &dest, &src, &npdu_data);
    if ((npdu_offset <= 0) || (npdu_offset >= (int)data_len)) {
        return false;
    }

    info->apdu_frame_offset = (uint16_t)(8 + npdu_offset);
    apdu = (uint8_t *)&payload[info->apdu_frame_offset];
    info->apdu_len = (int)data_len - npdu_offset;
    if (info->apdu_len < 2) {
        return false;
    }

    info->valid = true;
    info->mstp_data_len = data_len;
    info->pdu_type = (uint8_t)(apdu[0] & 0xF0);
    if (info->apdu_len >= 2) {
        info->invoke_id = apdu[1];
        if ((info->pdu_type == PDU_TYPE_SIMPLE_ACK) ||
            (info->pdu_type == PDU_TYPE_COMPLEX_ACK) ||
            (info->pdu_type == PDU_TYPE_ERROR) ||
            (info->pdu_type == PDU_TYPE_REJECT) ||
            (info->pdu_type == PDU_TYPE_ABORT)) {
            info->has_invoke_id = true;
        }
    }

    if ((info->apdu_len >= 3) &&
        ((info->pdu_type == PDU_TYPE_SIMPLE_ACK) ||
            (info->pdu_type == PDU_TYPE_COMPLEX_ACK) ||
            (info->pdu_type == PDU_TYPE_ERROR))) {
        info->service_choice = apdu[2];
        info->has_service_choice = true;
    }

    return true;
}

void MSTP_RS485_Confirmed_Request_Track(const MSTP_RS485_CONFIRMED_REQUEST_META *meta)
{
    MSTP_RS485_CONFIRMED_REQUEST_META *slot = NULL;

    if (!meta) {
        return;
    }

    slot = mstp_confirmed_track_slot(meta->invoke_id);
    if (!slot) {
        return;
    }

    *slot = *meta;
    slot->valid = true;
}

bool MSTP_RS485_Send_Reply_Postponed(uint8_t requester_mac, uint8_t source_mac)
{
    uint8_t frame[16] = { 0 };
    uint16_t frame_len = 0;

    frame_len = MSTP_Create_Frame(
        frame,
        sizeof(frame),
        FRAME_TYPE_REPLY_POSTPONED,
        requester_mac,
        source_mac,
        NULL,
        0);
    if (frame_len == 0) {
        return false;
    }

    MSTP_RS485_Send(frame, frame_len);
    return true;
}

#if MSTP_DEBUG_ENABLE
/* Rate-limit timestamps for TX control-frame logging (microseconds) */
static int64_t mstp_tx_token_log_us = 0;
static int64_t mstp_tx_pfm_log_us = 0;
static int64_t mstp_tx_rpfm_log_us = 0;
static volatile uint32_t mstp_tx_token_count = 0;
static volatile uint32_t mstp_tx_pfm_count = 0;
static volatile uint32_t mstp_tx_rpfm_count = 0;

static void mstp_log_frame_debug(const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len >= 8 && payload[0] == 0x55 && payload[1] == 0xFF) {
        uint8_t ftype = payload[2];
        uint8_t fdest = payload[3];
        uint8_t fsrc = payload[4];
        uint16_t flen = ((uint16_t)payload[5] << 8) | payload[6];
        int64_t now_us = esp_timer_get_time();

        if (ftype == 0x00) { /* TOKEN */
            mstp_tx_token_count++;
            if ((now_us - mstp_tx_token_log_us) >= 5000000LL) {
                ESP_LOGD(TAG, "TX TOKEN dst=%u src=%u count=%lu", fdest, fsrc,
                    (unsigned long)mstp_tx_token_count);
                mstp_tx_token_log_us = now_us;
            }
        } else if (ftype == 0x01) { /* POLL_FOR_MASTER */
            mstp_tx_pfm_count++;
            if ((now_us - mstp_tx_pfm_log_us) >= 5000000LL) {
                ESP_LOGD(TAG, "TX POLL_FOR_MASTER dst=%u src=%u count=%lu", fdest,
                    fsrc, (unsigned long)mstp_tx_pfm_count);
                mstp_tx_pfm_log_us = now_us;
            }
        } else if (ftype == 0x02) { /* REPLY_TO_POLL_FOR_MASTER */
            mstp_tx_rpfm_count++;
            if ((now_us - mstp_tx_rpfm_log_us) >= 5000000LL) {
                ESP_LOGD(TAG, "TX REPLY_TO_PFM dst=%u src=%u count=%lu", fdest,
                    fsrc, (unsigned long)mstp_tx_rpfm_count);
                mstp_tx_rpfm_log_us = now_us;
            }
        } else {
            ESP_LOGD(TAG, "TX frm#%lu type=0x%02X dst=%u src=%u dlen=%u plen=%u",
                (unsigned long)mstp_tx_frame_count, ftype, fdest, fsrc, flen,
                payload_len);
            uint16_t dump_len = payload_len < 16u ? payload_len : 16u;
            char hex_buf[49]; /* 16*3 + 1 */
            for (uint16_t i = 0; i < dump_len; i++) {
                sprintf(&hex_buf[i * 3], "%02X ", payload[i]);
            }
            hex_buf[dump_len * 3 > 0 ? dump_len * 3 - 1 : 0] = '\0';
            ESP_LOGD(TAG, "TX hex[%u]: %s", dump_len, hex_buf);
        }
    } else {
        ESP_LOGD(TAG, "TX frm#%lu plen=%u (no preamble)",
            (unsigned long)mstp_tx_frame_count, payload_len);
        uint16_t dump_len = payload_len < 16u ? payload_len : 16u;
        char hex_buf[49];
        for (uint16_t i = 0; i < dump_len; i++) {
            sprintf(&hex_buf[i * 3], "%02X ", payload[i]);
        }
        hex_buf[dump_len * 3 > 0 ? dump_len * 3 - 1 : 0] = '\0';
        ESP_LOGD(TAG, "TX hex[%u]: %s", dump_len, hex_buf);
    }
}
#endif

/* Manual DE control is compiled only for transceivers that expose DE/RE. */
#if MSTP_RS485_HAS_DE_PIN
static void mstp_rs485_set_tx_mode(bool enabled)
{
    int level = 0;

    if (MSTP_RS485_DE_ACTIVE_HIGH) {
        level = enabled ? 1 : 0;
    } else {
        level = enabled ? 0 : 1;
    }
    gpio_set_level(MSTP_UART_DE_PIN, level);
}
#endif

static bool mstp_is_confirmed_response_pdu(uint8_t pdu_type)
{
    return (pdu_type == PDU_TYPE_SIMPLE_ACK) ||
        (pdu_type == PDU_TYPE_COMPLEX_ACK) ||
        (pdu_type == PDU_TYPE_ERROR) ||
        (pdu_type == PDU_TYPE_REJECT) ||
        (pdu_type == PDU_TYPE_ABORT);
}

#if BACNET_MSTP_TX_HEX_DEBUG
static void mstp_log_hex_frame_full(const uint8_t *payload, uint16_t payload_len)
{
    uint16_t offset = 0;

    while (offset < payload_len) {
        uint16_t chunk_len = (uint16_t)(payload_len - offset);
        char hex_buf[(16 * 3) + 1] = { 0 };
        uint16_t i = 0;

        if (chunk_len > 16u) {
            chunk_len = 16u;
        }

        for (i = 0; i < chunk_len; i++) {
            (void)snprintf(
                &hex_buf[i * 3], sizeof(hex_buf) - (i * 3), "%02X ",
                payload[offset + i]);
        }
        if (chunk_len > 0) {
            hex_buf[(chunk_len * 3) - 1] = '\0';
        }

        ESP_LOGI(TAG, "mstp tx6 hex[%u..%u]=%s", (unsigned)offset,
            (unsigned)(offset + chunk_len - 1), hex_buf);
        offset = (uint16_t)(offset + chunk_len);
    }
}
#endif

void MSTP_RS485_Init(void)
{
    esp_err_t err = ESP_OK;

    if (mstp_uart_initialized) {
        return;
    }

    uart_config_t config = {
        .baud_rate = (int)mstp_baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

#if MSTP_RS485_HAS_DE_PIN
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << MSTP_UART_DE_PIN),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure DE pin: %d", err);
    }
    mstp_rs485_set_tx_mode(false);
#endif

    err = uart_param_config(MSTP_UART_PORT, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART param config failed: %d", err);
    }

    err = uart_set_pin(
        MSTP_UART_PORT, MSTP_UART_TX_PIN, MSTP_UART_RX_PIN,
        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART set pin failed: %d", err);
    }

    err = uart_driver_install(
        MSTP_UART_PORT, MSTP_UART_RX_BUF_SIZE, MSTP_UART_TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %d", err);
    }

    mstp_last_activity_us = esp_timer_get_time();
    mstp_uart_initialized = true;

#if !MSTP_RS485_HAS_DE_PIN
    ESP_LOGI(
        TAG,
        "RS485 transceiver: Waveshare isolated auto-direction, UART2 TX=17 RX=16 DE=disabled baud=38400");
#else
    ESP_LOGI(
        TAG,
        "MS/TP UART initialized on TX=%d RX=%d DE=%d @%lu baud de_active_high=%d",
        (int)MSTP_UART_TX_PIN,
        (int)MSTP_UART_RX_PIN,
        (int)MSTP_UART_DE_PIN,
        (unsigned long)mstp_baud_rate,
        (int)MSTP_RS485_DE_ACTIVE_HIGH);
#endif
}

void MSTP_RS485_Send(const uint8_t *payload, uint16_t payload_len)
{
    MSTP_TX_INFO tx_info = { 0 };
    MSTP_RS485_CONFIRMED_REQUEST_META *request_meta = NULL;
    bool is_reply_to_pfm_frame = false;
    bool is_token_frame = false;
    int64_t pfm_reply_t_before_us = 0;
    int64_t token_pass_t_before_us = 0;
#if MSTP_DEBUG_ENABLE
    int64_t now_us = 0;
    int64_t request_to_postponed_us = -1;
    int64_t request_to_final_us = -1;
#endif
    int written = 0;
    esp_err_t tx_done = ESP_OK;
#if MSTP_RS485_HAS_DE_PIN
    int de_level_after_enable = -1;
    int de_level_before_disable = -1;
    int de_level_after_disable = -1;
#endif
    TickType_t tx_wait_ticks = pdMS_TO_TICKS(MSTP_UART_TX_TIMEOUT_MS);

    if (!payload || payload_len == 0) {
        return;
    }
    if (!mstp_uart_initialized) {
        MSTP_RS485_Init();
    }

    if ((payload_len >= 8) &&
        (payload[0] == 0x55) &&
        (payload[1] == 0xFF) &&
        (payload[2] == FRAME_TYPE_REPLY_TO_POLL_FOR_MASTER)) {
        is_reply_to_pfm_frame = true;
    } else if (
        (payload_len >= 8) &&
        (payload[0] == 0x55) &&
        (payload[1] == 0xFF) &&
        (payload[2] == FRAME_TYPE_TOKEN)) {
        is_token_frame = true;
    }

    mstp_tx_frame_count++;
#if MSTP_DEBUG_ENABLE
    if (!is_reply_to_pfm_frame && !is_token_frame) {
        mstp_log_frame_debug(payload, payload_len);
    }
#endif
    mstp_decode_tx_info(payload, payload_len, &tx_info);
    if (tx_info.valid && tx_info.has_invoke_id) {
        request_meta = mstp_confirmed_find_slot(tx_info.invoke_id);
    }

    mstp_tx_in_progress = true;
#if MSTP_RS485_HAS_DE_PIN
    mstp_rs485_set_tx_mode(true);
    de_level_after_enable = gpio_get_level(MSTP_UART_DE_PIN);
    vTaskDelay(pdMS_TO_TICKS(MSTP_RS485_DE_PRE_TX_GUARD_MS));
#endif

    if (is_reply_to_pfm_frame) {
        pfm_reply_t_before_us = esp_timer_get_time();
    } else if (is_token_frame) {
        token_pass_t_before_us = esp_timer_get_time();
    }

    written = uart_write_bytes(MSTP_UART_PORT, payload, payload_len);
    if (written < 0) {
        ESP_LOGE(TAG, "UART write failed");
    }

    if (tx_wait_ticks == 0) {
        tx_wait_ticks = 1;
    }

    tx_done = uart_wait_tx_done(MSTP_UART_PORT, tx_wait_ticks);
#if !MSTP_DEBUG_ENABLE
    (void)tx_done;
#endif

#if MSTP_RS485_HAS_DE_PIN
    vTaskDelay(pdMS_TO_TICKS(MSTP_RS485_DE_POST_TX_GUARD_MS));
    de_level_before_disable = gpio_get_level(MSTP_UART_DE_PIN);
    mstp_rs485_set_tx_mode(false);
    de_level_after_disable = gpio_get_level(MSTP_UART_DE_PIN);
#endif

    if (is_reply_to_pfm_frame) {
        int64_t pfm_reply_t_after_us = esp_timer_get_time();
#if MSTP_DEBUG_ENABLE
        ESP_LOGI(
            TAG,
            "PFM_REPLY_TX src=%u dst=%u state=%s t_before=%lld t_after=%lld result=%s",
            (unsigned)payload[4],
            (unsigned)payload[3],
            dlmstp_master_state_text_current(),
            (long long)pfm_reply_t_before_us,
            (long long)pfm_reply_t_after_us,
            esp_err_to_name(tx_done));
        #else
            (void)pfm_reply_t_before_us;
            (void)pfm_reply_t_after_us;
        #endif
    } else if (is_token_frame) {
        int64_t token_pass_t_after_us = esp_timer_get_time();
        #if MSTP_DEBUG_ENABLE
        ESP_LOGI(
            TAG,
            "TOKEN_PASS_TX src=%u dst=%u t_before=%lld t_after=%lld result=%s",
            (unsigned)payload[4],
            (unsigned)payload[3],
            (long long)token_pass_t_before_us,
            (long long)token_pass_t_after_us,
            esp_err_to_name(tx_done));
        #else
            (void)token_pass_t_before_us;
            (void)token_pass_t_after_us;
        #endif
    }

    if (tx_info.valid &&
        tx_info.is_data_frame &&
        (tx_info.frame_type == FRAME_TYPE_BACNET_DATA_NOT_EXPECTING_REPLY)) {
#if MSTP_DEBUG_ENABLE
#if MSTP_RS485_HAS_DE_PIN
        ESP_LOGI(
            TAG,
            "mstp TX frame=%u dst=%u src=%u data_len=%u total_len=%u uart_write_ret=%d uart_wait_tx_done=%s de_en=%d de_pre_dis=%d de_dis=%d",
            (unsigned)tx_info.frame_type,
            (unsigned)payload[3],
            (unsigned)payload[4],
            (unsigned)tx_info.mstp_data_len,
            (unsigned)payload_len,
            written,
            esp_err_to_name(tx_done),
            de_level_after_enable,
            de_level_before_disable,
            de_level_after_disable);
        #else
            ESP_LOGI(
                TAG,
                "mstp TX frame=%u dst=%u src=%u data_len=%u total_len=%u uart_write_ret=%d uart_wait_tx_done=%s",
                (unsigned)tx_info.frame_type,
                (unsigned)payload[3],
                (unsigned)payload[4],
                (unsigned)tx_info.mstp_data_len,
                (unsigned)payload_len,
                written,
                esp_err_to_name(tx_done));
#endif
#endif
    }

#if BACNET_MSTP_TX_HEX_DEBUG
    if (tx_info.valid &&
        tx_info.is_data_frame &&
        (tx_info.frame_type == FRAME_TYPE_BACNET_DATA_NOT_EXPECTING_REPLY)) {
        uint8_t dst_mac = payload[3];
        bool target_match =
            (dst_mac == BACNET_MSTP_TX_HEX_DEBUG_TARGET_MAC) ||
            (request_meta && request_meta->valid &&
                (request_meta->object_instance ==
                    BACNET_MSTP_TX_HEX_DEBUG_TARGET_DEVICE_INSTANCE));
        int invoke_id_log = -1;
        int service_log = -1;
        int obj_type_log = -1;
        long obj_inst_log = -1;
        long prop_log = -1;

        if (target_match) {
            if (tx_info.has_invoke_id) {
                invoke_id_log = (int)tx_info.invoke_id;
            }
            if (tx_info.has_service_choice) {
                service_log = (int)tx_info.service_choice;
            }
            if (request_meta && request_meta->valid) {
                invoke_id_log = (int)request_meta->invoke_id;
                service_log = (int)request_meta->service_choice;
                obj_type_log = (int)request_meta->object_type;
                obj_inst_log = (long)request_meta->object_instance;
                prop_log = (long)request_meta->object_property;
            }

            ESP_LOGI(
                TAG,
                "mstp tx6 dbg dst=%u src=%u data_len=%u total_len=%u invoke=%d service=%d obj=%d:%ld prop=%ld apdu_len=%d",
                (unsigned)payload[3],
                (unsigned)payload[4],
                (unsigned)tx_info.mstp_data_len,
                (unsigned)payload_len,
                invoke_id_log,
                service_log,
                obj_type_log,
                obj_inst_log,
                prop_log,
                tx_info.apdu_len);
            mstp_log_hex_frame_full(payload, payload_len);
        }
    }
#endif

    if (tx_info.valid &&
        tx_info.is_data_frame &&
        tx_info.has_invoke_id &&
        mstp_is_confirmed_response_pdu(tx_info.pdu_type)) {
        if (request_meta) {
#if MSTP_DEBUG_ENABLE
            now_us = esp_timer_get_time();
            request_to_final_us = now_us - request_meta->request_rx_us;
            if (request_meta->reply_postponed_sent &&
                (request_meta->reply_postponed_tx_us >= request_meta->request_rx_us)) {
                request_to_postponed_us =
                    request_meta->reply_postponed_tx_us - request_meta->request_rx_us;
            }

            ESP_LOGI(
                TAG,
                "final response sent invoke=%u requester_mac=%u service=%u object=%u:%lu property=%lu array=%lu reply_postponed=%s req_to_postponed_ms=%ld req_to_final_ms=%ld pdu=0x%02X tx_ret=%d tx_done=%d",
                (unsigned)request_meta->invoke_id,
                (unsigned)request_meta->requester_mac,
                (unsigned)request_meta->service_choice,
                (unsigned)request_meta->object_type,
                (unsigned long)request_meta->object_instance,
                (unsigned long)request_meta->object_property,
                (unsigned long)request_meta->array_index,
                request_meta->reply_postponed_sent ? "yes" : "no",
                (long)(request_to_postponed_us >= 0 ? request_to_postponed_us / 1000 : -1),
                (long)(request_to_final_us >= 0 ? request_to_final_us / 1000 : -1),
                (unsigned)tx_info.pdu_type,
                written,
                (int)tx_done);
#endif

            memset(request_meta, 0, sizeof(*request_meta));
        }
    }

    mstp_tx_in_progress = false;
    mstp_last_activity_us = esp_timer_get_time();
}

bool MSTP_RS485_Read(uint8_t *buf)
{
    if (!buf) {
        return false;
    }
    if (!mstp_uart_initialized) {
        MSTP_RS485_Init();
    }

    int len = uart_read_bytes(MSTP_UART_PORT, buf, 1, 0);
    if (len > 0) {
        mstp_last_activity_us = esp_timer_get_time();
        mstp_rx_bytes += (uint32_t)len;
        if (*buf == 0x55) {
            mstp_preamble_55++;
        }
        if (mstp_prev_byte == 0x55 && *buf == 0xFF) {
            mstp_preamble_55ff++;
        }
        mstp_prev_byte = *buf;
        return true;
    }

    return false;
}

bool MSTP_RS485_Transmitting(void)
{
    return mstp_tx_in_progress;
}

uint32_t MSTP_RS485_Baud_Rate(void)
{
    return mstp_baud_rate;
}

bool MSTP_RS485_Baud_Rate_Set(uint32_t baud)
{
    if (baud == 0) {
        return false;
    }

    mstp_baud_rate = baud;
    if (mstp_uart_initialized) {
        esp_err_t err = uart_set_baudrate(MSTP_UART_PORT, (int)baud);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "UART set baud failed: %d", err);
            return false;
        }
    }

    return true;
}

uint32_t MSTP_RS485_Silence_Milliseconds(void)
{
    int64_t now_us = esp_timer_get_time();
    int64_t delta_us = now_us - mstp_last_activity_us;

    if (delta_us < 0) {
        return 0;
    }

    return (uint32_t)(delta_us / 1000);
}

void MSTP_RS485_Silence_Reset(void)
{
    mstp_last_activity_us = esp_timer_get_time();
}

uint32_t MSTP_RS485_Rx_Bytes_Get_Reset(void)
{
    uint32_t count = mstp_rx_bytes;
    mstp_rx_bytes = 0;
    return count;
}

void MSTP_RS485_Preamble_Counts_Get_Reset(uint32_t *preamble55, uint32_t *preamble55ff)
{
    if (preamble55) {
        *preamble55 = mstp_preamble_55;
    }
    if (preamble55ff) {
        *preamble55ff = mstp_preamble_55ff;
    }
    mstp_preamble_55 = 0;
    mstp_preamble_55ff = 0;
}

uint32_t MSTP_RS485_Tx_Frame_Count_Get_Reset(void)
{
    uint32_t count = mstp_tx_frame_count;
    mstp_tx_frame_count = 0;
    return count;
}
