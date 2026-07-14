/* Minimal example: connect to Wi‑Fi and initialize BACnet. */
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "wifi_helper.h"
#include "display.h"
#include "analog_value.h"
#include "binary_value.h"
#include "analog_input.h"
#include "binary_input.h"
#include "binary_output.h"
#include "sen54.h"
#include "sen54_temperature_compensation.h"
#include "mstp_rs485.h"
#include "User_Settings.h"

/* bacnet-stack headers */
#include "bacnet/basic/object/device.h"
#include "bacnet/basic/object/av.h"
#include "bacnet/basic/object/bv.h"
#include "bacnet/basic/object/ai.h"
#include "bacnet/basic/object/bi.h"
#include "bacnet/basic/object/bo.h"
#include "bacnet/basic/service/s_iam.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/bacaddr.h"
#include "bacnet/basic/bbmd/h_bbmd.h"
#include "bacnet/datalink/datalink.h"
#include "bacnet/datalink/bip.h"
#include "bacnet/datalink/dlmstp.h"
#include "bacnet/datalink/mstp.h"
/* service handlers from bacnet-stack library */
#include "bacnet/basic/services.h"
#include "bacnet/basic/service/h_rp.h"
#include "bacnet/basic/service/h_rpm.h"
#include "bacnet/basic/service/h_wp.h"
#include "bacnet/basic/service/h_whois.h"
#include "bacnet/basic/service/h_iam.h"
#include "bacnet/basic/service/h_cov.h"
#include "bacnet/basic/service/s_whois.h"
#include "bacnet/whois.h"
#include "bacnet/iam.h"
#include "bacnet/npdu.h"
#include "bacnet/rp.h"
#include "bacnet/rpm.h"
#include "bacnet/dcc.h"
#include "bacnet/config.h"
#include "bacnet/basic/npdu/h_npdu.h"
#include "bacnet/bacenum.h"

static const char *TAG = "bacnet";

/* Temporary discovery tracing gate.
 * Keep off in normal operation so serial logs stay focused.
 */
#ifndef BACNET_DISCOVERY_DEBUG
#define BACNET_DISCOVERY_DEBUG 0
#endif

#define MSTP_TRANSPORT_BUFFER_SIZE 1024
#define MSTP_APDU_BUFFER_SIZE 1024

/* Temperature compensation AV instance mappings */
/* SEN54 temp compensation AV instances defined earlier to allow helper usage */

int override_nvs_on_flash = 0;  /* Exported for AV/BV modules */


static void bacnet_register_with_bbmd(void);
static void bacnet_receive_task(void *pvParameters);
static void bacnet_mstp_receive_task(void *pvParameters);
static void bacnet_cov_task(void *pvParameters);
static void sen54_task(void *pvParameters);
static void sen54_reapply_temperature_offset(void);

/* AV instance numbers for SEN54 temperature compensation */
#define SEN54_TEMP_OFFSET_AV_INSTANCE 8U
#define SEN54_TEMP_SLOPE_AV_INSTANCE 9U
#define SEN54_TEMP_TIME_CONSTANT_AV_INSTANCE 10U

static bool sen54_publish_temperature_compensation(void)
{
    sen54_temperature_compensation_t params;
    esp_err_t rc = sen54_temperature_compensation_get(&params);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "SEN54 temp compensation read failed (%d)", rc);
        return false;
    }

    bool u1 = Analog_Value_Present_Value_Set(SEN54_TEMP_OFFSET_AV_INSTANCE, params.offset_c, 16);
    bool u2 = Analog_Value_Present_Value_Set(SEN54_TEMP_SLOPE_AV_INSTANCE, params.slope, 16);
    bool u3 = Analog_Value_Present_Value_Set(SEN54_TEMP_TIME_CONSTANT_AV_INSTANCE, (float)params.time_constant_s, 16);

    if (!(u1 && u2 && u3)) {
        ESP_LOGE(TAG, "SEN54 temp compensation: BACnet update failed (u1=%d u2=%d u3=%d)", (int)u1, (int)u2, (int)u3);
        return false;
    }

    ESP_LOGI(TAG, "SEN54 temp compensation: offset=%.3f C slope=%.5f time_constant=%u s",
             params.offset_c, params.slope, (unsigned)params.time_constant_s);
    return true;
}
static void handler_who_is_debug(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src);
#if BACNET_DISCOVERY_DIAGNOSTICS
static void bacnet_track_confirmed_request_bip(
    const uint8_t *apdu,
    int apdu_len,
    const BACNET_ADDRESS *src,
    const char *link);
#endif
static TaskHandle_t bacnet_cov_task_handle = NULL;
static SemaphoreHandle_t bacnet_datalink_mutex = NULL;
static volatile uint32_t mstp_pdu_count = 0;
static volatile uint32_t mstp_apdu_count = 0;
static volatile uint32_t mstp_rp_total = 0;
static volatile uint32_t mstp_wp_total = 0;
static float mstp_rp_last_value = 0.0f;

typedef struct {
    bool valid;
    const char *link;
    BACNET_ADDRESS src;
    BACNET_ADDRESS dest;
} BACNET_WHOIS_RX_CONTEXT;

static BACNET_WHOIS_RX_CONTEXT s_whois_rx_context = {
    .valid = false,
    .link = "unknown"
};

#define SEN54_AUTO_CLEANING_AV_INSTANCE 7U
#define SEN54_MEASUREMENT_ENABLE_BV_INSTANCE 2U
#define SEN54_FAN_CLEANING_BV_INSTANCE 3U
#define SEN54_CLEAR_STATUS_BV_INSTANCE 4U

#define SEN54_STATUS_FAN_ERROR_BIT  (1UL << 4)
#define SEN54_STATUS_LASER_ERROR_BIT (1UL << 5)
#define SEN54_STATUS_RHT_ERROR_BIT  (1UL << 6)
#define SEN54_STATUS_VOC_ERROR_BIT  (1UL << 7)

static bool sen54_auto_cleaning_interval_valid = false;
static uint32_t sen54_auto_cleaning_interval_seconds = 0;

static void sen54_set_command_bv_inactive(uint32_t instance)
{
    Binary_Value_Present_Value_Set(instance, BINARY_INACTIVE);
}

static bool wifi_connected_now(void)
{
    if (!USER_ENABLE_BACNET_IP) {
        return false;
    }

    wifi_ap_record_t ap_info = {0};
    return esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
}

static void wifi_ip_string_now(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }

    out[0] = '\0';
    if (!USER_ENABLE_BACNET_IP) {
        snprintf(out, out_len, "No IP");
        return;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        snprintf(out, out_len, "No IP");
        return;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(out, out_len, IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(out, out_len, "No IP");
    }
}

static void bacnet_log_whois_iam(const uint8_t *apdu, int apdu_len, const char *link)
{
    if (!apdu || apdu_len < 2) {
        return;
    }

    uint8_t pdu_type = apdu[0] & 0xF0;
    if (pdu_type != PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST) {
        return;
    }

    uint8_t service_choice = apdu[1];
    if (service_choice == SERVICE_UNCONFIRMED_WHO_IS) {
#if BACNET_DISCOVERY_DEBUG
        int32_t low_limit = -1;
        int32_t high_limit = -1;
        int len = whois_decode_service_request(
            &apdu[2], (unsigned)(apdu_len - 2), &low_limit, &high_limit);
        if (len >= 0) {
            bool in_range = true;
            if (low_limit >= 0 && high_limit >= 0) {
                uint32_t instance = USER_BACNET_DEVICE_INSTANCE;
                in_range = (instance >= (uint32_t)low_limit &&
                    instance <= (uint32_t)high_limit);
            }
            ESP_LOGI(
                TAG,
                "%s Who-Is low=%ld high=%ld local_instance=%lu match=%s",
                link,
                (long)low_limit,
                (long)high_limit,
                (unsigned long)USER_BACNET_DEVICE_INSTANCE,
                in_range ? "yes" : "no");
            (void)in_range;
            (void)low_limit;
            (void)high_limit;
        } else {
            ESP_LOGW(TAG, "%s Who-Is decode failed len=%d", link, apdu_len);
        }
#endif
    } else if (service_choice == SERVICE_UNCONFIRMED_I_AM) {
        uint32_t device_id = BACNET_MAX_INSTANCE;
        unsigned max_apdu = 0;
        int segmentation = SEGMENTATION_NONE;
        uint16_t vendor_id = 0;
        int len = iam_decode_service_request(
            &apdu[2], &device_id, &max_apdu, &segmentation, &vendor_id);
        if (len >= 0) {
            ESP_LOGI(
                TAG,
                "%s I-Am device=%lu vendor=%u max_apdu=%u",
                link,
                (unsigned long)device_id,
                (unsigned)vendor_id,
                max_apdu);
        } else {
            ESP_LOGW(TAG, "%s I-Am decode failed len=%d", link, apdu_len);
        }
    }
}

#if BACNET_DISCOVERY_DEBUG
static const char *bacnet_whois_target_type(const BACNET_ADDRESS *dest)
{
    if (dest && dest->net == BACNET_BROADCAST_NETWORK) {
        return "global_broadcast";
    }
    if (dest && ((dest->net > 0U) || (dest->mac_len > 0U))) {
        return "directed";
    }

    return "local_broadcast";
}
#endif

static void bacnet_whois_context_set(
    const char *link,
    const BACNET_ADDRESS *src,
    const BACNET_ADDRESS *dest)
{
    s_whois_rx_context.valid = true;
    s_whois_rx_context.link = link ? link : "unknown";

    if (src) {
        s_whois_rx_context.src = *src;
    } else {
        memset(&s_whois_rx_context.src, 0, sizeof(s_whois_rx_context.src));
    }

    if (dest) {
        s_whois_rx_context.dest = *dest;
    } else {
        memset(&s_whois_rx_context.dest, 0, sizeof(s_whois_rx_context.dest));
    }
}

static void bacnet_whois_context_clear(void)
{
    s_whois_rx_context.valid = false;
    s_whois_rx_context.link = "unknown";
    memset(&s_whois_rx_context.src, 0, sizeof(s_whois_rx_context.src));
    memset(&s_whois_rx_context.dest, 0, sizeof(s_whois_rx_context.dest));
}

static int bacnet_send_i_am_with_reason(const char *reason, const char *link, bool dcc_gate)
{
    BACNET_ADDRESS dest = { 0 };
    BACNET_NPDU_DATA npdu_data = { 0 };
    uint32_t device_instance = Device_Object_Instance_Number();
    unsigned max_apdu = Device_Max_APDU_Accepted();
    int segmentation = SEGMENTATION_NONE;
    uint16_t vendor_id = Device_Vendor_Identifier();
    int pdu_len = 0;
    int bytes_sent = 0;

    if (dcc_gate && dcc_communication_initiation_disabled()) {
        ESP_LOGI(
            TAG,
            "I-Am TX reason=%s via=%s skipped: communication initiation disabled",
            reason ? reason : "unspecified",
            link ? link : "unknown");
        return 0;
    }

    pdu_len = iam_encode_pdu(Handler_Transmit_Buffer, &dest, &npdu_data);
    bytes_sent =
        datalink_send_pdu(&dest, &npdu_data, Handler_Transmit_Buffer, pdu_len);

    ESP_LOGI(
        TAG,
        "I-Am TX reason=%s via=%s device_instance=%lu max_apdu=%u segmentation=%d vendor_id=%u result=%d",
        reason ? reason : "unspecified",
        link ? link : "unknown",
        (unsigned long)device_instance,
        max_apdu,
        segmentation,
        (unsigned)vendor_id,
        bytes_sent);

    if (link && strcmp(link, "MSTP") == 0) {
        ESP_LOGI(TAG, "I-Am MS/TP send result=%d", bytes_sent);
    }

    return bytes_sent;
}

static void handler_who_is_debug(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src)
{
    int decode_len = BACNET_STATUS_ERROR;
    int32_t low_limit = -1;
    int32_t high_limit = -1;
    uint32_t local_instance = Device_Object_Instance_Number();
    bool has_limits = false;
    bool in_range = true;
    bool send_i_am = false;
#if BACNET_DISCOVERY_DEBUG
    const BACNET_ADDRESS *ctx_src = src;
    BACNET_ADDRESS empty_dest = { 0 };
    const BACNET_ADDRESS *ctx_dest = &empty_dest;
#endif
    const char *link = "unknown";

    if (s_whois_rx_context.valid) {
#if BACNET_DISCOVERY_DEBUG
        ctx_src = &s_whois_rx_context.src;
        ctx_dest = &s_whois_rx_context.dest;
#endif
        link = s_whois_rx_context.link;
    }

#if BACNET_DISCOVERY_DEBUG
    ESP_LOGI(
        TAG,
        "%s Who-Is RX src_mstp_mac=%d dest_mac=%d class=%s len=%u (pre-filter)",
        link,
        (ctx_src && ctx_src->mac_len > 0U) ? (int)ctx_src->mac[0] : -1,
        (ctx_dest && ctx_dest->mac_len > 0U) ? (int)ctx_dest->mac[0] : -1,
        bacnet_whois_target_type(ctx_dest),
        (unsigned)service_len);

    if (service_len > 0U) {
        ESP_LOGI(TAG, "%s Who-Is raw APDU payload (%u bytes):", link, (unsigned)service_len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, service_request, service_len, ESP_LOG_INFO);
    } else {
        ESP_LOGI(TAG, "%s Who-Is raw APDU payload: <empty>", link);
    }
#endif

    if (service_len == 0U) {
        has_limits = false;
        in_range = true;
        send_i_am = true;
    } else {
        decode_len = whois_decode_service_request(
            service_request, service_len, &low_limit, &high_limit);
        has_limits =
            (decode_len > 0) &&
            ((uint16_t)decode_len == service_len) &&
            (low_limit >= 0) &&
            (high_limit >= 0);

        if (has_limits) {
            in_range =
                (local_instance >= (uint32_t)low_limit) &&
                (local_instance <= (uint32_t)high_limit);
            send_i_am = in_range;
        } else {
            in_range = false;
            send_i_am = false;
        }
    }

#if BACNET_DISCOVERY_DEBUG
    if (!has_limits && service_len > 0U) {
        ESP_LOGW(
            TAG,
            "%s Who-Is limit decode failed/partial decode_len=%d service_len=%u",
            link,
            decode_len,
            (unsigned)service_len);
    }
#endif

    if (has_limits) {
        in_range =
            (local_instance >= (uint32_t)low_limit) &&
            (local_instance <= (uint32_t)high_limit);
        send_i_am = in_range;
    }

#if BACNET_DISCOVERY_DEBUG
    ESP_LOGI(
        TAG,
        "%s Who-Is eval has_limits=%s low_limit=%ld high_limit=%ld local_instance=%lu in_range=%s send_i_am=%s",
        link,
        has_limits ? "yes" : "no",
        (long)low_limit,
        (long)high_limit,
        (unsigned long)local_instance,
        in_range ? "yes" : "no",
        send_i_am ? "yes" : "no");
#endif

    if (send_i_am) {
        (void)bacnet_send_i_am_with_reason("response_to_who_is", link, false);
    }
}

static char datalink_bip[] = "bip";
static char datalink_mstp[] = "mstp";
static char *datalink_default = NULL;
static bool s_bacnet_ip_active = false;

static uint8_t mstp_rx_buffer[MSTP_TRANSPORT_BUFFER_SIZE];
static uint8_t mstp_tx_buffer[MSTP_TRANSPORT_BUFFER_SIZE];
static struct mstp_port_struct_t mstp_port;
static struct dlmstp_user_data_t mstp_user;
static struct dlmstp_rs485_driver mstp_rs485_driver = {
    .init = MSTP_RS485_Init,
    .send = MSTP_RS485_Send,
    .read = MSTP_RS485_Read,
    .transmitting = MSTP_RS485_Transmitting,
    .baud_rate = MSTP_RS485_Baud_Rate,
    .baud_rate_set = MSTP_RS485_Baud_Rate_Set,
    .silence_milliseconds = MSTP_RS485_Silence_Milliseconds,
    .silence_reset = MSTP_RS485_Silence_Reset
};

static void bacnet_track_confirmed_request_mstp(
    const uint8_t *apdu,
    int apdu_len,
    const BACNET_ADDRESS *src,
    const BACNET_ADDRESS *dest,
    const char *link)
{
    MSTP_RS485_CONFIRMED_REQUEST_META track = { 0 };
    BACNET_READ_PROPERTY_DATA rp_data = { 0 };
    BACNET_RPM_DATA rpm_data = { 0 };
    uint16_t object_type = 0xFFFF;
    uint32_t object_instance = BACNET_MAX_INSTANCE;
    uint32_t object_property = 0xFFFFFFFFUL;
    uint32_t array_index = BACNET_ARRAY_ALL;
    bool routed_request = false;
    bool should_send_reply_postponed = false;
    bool reply_postponed_sent = false;
    int64_t request_rx_us = 0;
    int64_t reply_postponed_tx_us = 0;
    int len = 0;
    uint8_t pdu_type = 0;
    uint8_t service_choice = 0;
    uint8_t invoke_id = 0;
    uint8_t requester_mac = 0xFF;

    if (!apdu || (apdu_len < 5)) {
        return;
    }

    pdu_type = apdu[0] & 0xF0;
    if (pdu_type != PDU_TYPE_CONFIRMED_SERVICE_REQUEST) {
        return;
    }
    if ((apdu[0] & BIT(3)) != 0) {
        /* Segmented request: leave behavior unchanged. */
        return;
    }

    invoke_id = apdu[2];
    service_choice = apdu[3];
    if (src && src->len > 0) {
        requester_mac = src->mac[0];
    }

    if (src &&
        (src->net != 0) &&
        (src->net != BACNET_BROADCAST_NETWORK)) {
        routed_request = true;
    }

    if (service_choice == SERVICE_CONFIRMED_READ_PROPERTY) {
        len = rp_decode_service_request(&apdu[4], (uint16_t)(apdu_len - 4), &rp_data);
        if (len > 0) {
            object_type = (uint16_t)rp_data.object_type;
            object_instance = rp_data.object_instance;
            object_property = (uint32_t)rp_data.object_property;
            array_index = rp_data.array_index;
            if (rp_data.object_property == PROP_OBJECT_LIST) {
                should_send_reply_postponed = true;
            }
        }
    } else if (service_choice == SERVICE_CONFIRMED_READ_PROP_MULTIPLE) {
        len = rpm_decode_object_id(&apdu[4], (unsigned)(apdu_len - 4), &rpm_data);
        if (len > 0) {
            object_type = (uint16_t)rpm_data.object_type;
            object_instance = rpm_data.object_instance;
            len += rpm_decode_object_property(
                &apdu[4 + len],
                (unsigned)(apdu_len - 4 - len),
                &rpm_data);
            if (len > 0) {
                object_property = (uint32_t)rpm_data.object_property;
                array_index = rpm_data.array_index;
            }
        }
        should_send_reply_postponed = true;
    }

    if (routed_request) {
        should_send_reply_postponed = true;
    }

    request_rx_us = esp_timer_get_time();
    ESP_LOGI(
        TAG,
        "%s request received invoke=%u requester_mac=%u service=%u object=%u:%lu property=%lu array=%lu",
        link,
        (unsigned)invoke_id,
        (unsigned)requester_mac,
        (unsigned)service_choice,
        (unsigned)object_type,
        (unsigned long)object_instance,
        (unsigned long)object_property,
        (unsigned long)array_index);

    if (should_send_reply_postponed && requester_mac != 0xFF) {
        reply_postponed_sent =
            dlmstp_send_reply_postponed(requester_mac);
        if (reply_postponed_sent) {
            reply_postponed_tx_us = esp_timer_get_time();
        }
    }

#if MSTP_DEBUG_ENABLE
    ESP_LOGI(
        TAG,
        "%s reply postponed sent invoke=%u requester_mac=%u yes=%s req_to_postponed_ms=%ld",
        link,
        (unsigned)invoke_id,
        (unsigned)requester_mac,
        reply_postponed_sent ? "yes" : "no",
        (long)(reply_postponed_sent ? ((reply_postponed_tx_us - request_rx_us) / 1000) : -1));
#endif

    track.invoke_id = invoke_id;
    track.requester_mac = requester_mac;
    track.service_choice = service_choice;
    track.object_type = object_type;
    track.object_instance = object_instance;
    track.object_property = object_property;
    track.array_index = array_index;
    track.reply_postponed_sent = reply_postponed_sent;
    track.request_rx_us = request_rx_us;
    track.reply_postponed_tx_us = reply_postponed_tx_us;
    (void)dest;
    MSTP_RS485_Confirmed_Request_Track(&track);
}

static void bacnet_datalink_lock(char *name)
{
    if (bacnet_datalink_mutex) {
        xSemaphoreTake(bacnet_datalink_mutex, portMAX_DELAY);
    }
    datalink_set(name);
}

static void bacnet_datalink_unlock(void)
{
    if (datalink_default) {
        datalink_set(datalink_default);
    }
    if (bacnet_datalink_mutex) {
        xSemaphoreGive(bacnet_datalink_mutex);
    }
}

static bool bacnet_mstp_init(void)
{
    MSTP_RS485_Init();

    memset(&mstp_port, 0, sizeof(mstp_port));
    memset(&mstp_user, 0, sizeof(mstp_user));

    mstp_user.RS485_Driver = &mstp_rs485_driver;
    mstp_port.UserData = &mstp_user;
    mstp_port.InputBuffer = mstp_rx_buffer;
    mstp_port.InputBufferSize = sizeof(mstp_rx_buffer);
    mstp_port.OutputBuffer = mstp_tx_buffer;
    mstp_port.OutputBufferSize = sizeof(mstp_tx_buffer);

    dlmstp_set_interface((const char *)&mstp_port);
    dlmstp_set_mac_address(USER_MSTP_MAC_ADDRESS);
    dlmstp_set_max_info_frames(USER_MSTP_MAX_INFO_FRAMES);
    dlmstp_set_max_master(USER_MSTP_MAX_MASTER);
    dlmstp_set_baud_rate(USER_MSTP_BAUD_RATE);
    dlmstp_check_auto_baud_set(USER_MSTP_AUTO_BAUD);
    dlmstp_slave_mode_enabled_set(false);

    ESP_LOGI(
        TAG,
        "MS/TP startup profile: mac=%u baud=%lu max_master=%u max_info=%u tx_buf=%u rx_buf=%u apdu_buf=%u max_apdu=%u",
        (unsigned)USER_MSTP_MAC_ADDRESS,
        (unsigned long)USER_MSTP_BAUD_RATE,
        (unsigned)USER_MSTP_MAX_MASTER,
        (unsigned)dlmstp_max_info_frames(),
        (unsigned)sizeof(mstp_tx_buffer),
        (unsigned)sizeof(mstp_rx_buffer),
        (unsigned)MSTP_APDU_BUFFER_SIZE,
        (unsigned)Device_Max_APDU_Accepted());

    return dlmstp_init((char *)&mstp_port);
}

/* BACnet receive task - processes incoming BACnet messages */
static void bacnet_receive_task(void *pvParameters)
{
    (void)pvParameters;
    BACNET_ADDRESS src = {0};
    static uint8_t rx_buffer[MSTP_APDU_BUFFER_SIZE];
    uint16_t pdu_len = 0;

    ESP_LOGI(TAG, "BACnet receive task started");

    while (1) {
        /* Poll for incoming BACnet messages */
        memset(&src, 0, sizeof(src));
        pdu_len = bip_receive(&src, rx_buffer, sizeof(rx_buffer), 100);
        if (pdu_len > 0) {
            /* Save original source from UDP socket before NPDU decode modifies it */
            BACNET_ADDRESS orig_src = src;
            BACNET_ADDRESS dest = {0};
            BACNET_NPDU_DATA npdu_data = {0};
            int apdu_offset = bacnet_npdu_decode(
                rx_buffer, pdu_len, &dest, &src, &npdu_data);
            /* If NPDU didn't have source routing info, restore from UDP socket */
            if (src.len == 0) {
                src = orig_src;
            }
            if (apdu_offset > 0 && apdu_offset < (int)pdu_len) {
                bacnet_log_whois_iam(&rx_buffer[apdu_offset], pdu_len - apdu_offset, "bip");
                /* Lightweight tracking of confirmed read requests for diagnostics */
#if BACNET_DISCOVERY_DIAGNOSTICS
                bacnet_track_confirmed_request_bip(&rx_buffer[apdu_offset], pdu_len - apdu_offset, &src, "BIP");
#endif
                bacnet_datalink_lock(datalink_bip);
                if (((rx_buffer[apdu_offset] & 0xF0U) ==
                        PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST) &&
                    (apdu_offset + 1 < (int)pdu_len) &&
                    (rx_buffer[apdu_offset + 1] == SERVICE_UNCONFIRMED_WHO_IS)) {
                    bacnet_whois_context_set("BIP", &src, &dest);
                }
                apdu_handler(&src, &rx_buffer[apdu_offset], pdu_len - apdu_offset);
                bacnet_whois_context_clear();
                bacnet_datalink_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* BACnet MS/TP receive task - processes incoming MS/TP frames */
static void bacnet_mstp_receive_task(void *pvParameters)
{
    (void)pvParameters;
    BACNET_ADDRESS src = {0};
    static uint8_t rx_buffer[MSTP_APDU_BUFFER_SIZE];
    uint16_t pdu_len = 0;

    ESP_LOGI(TAG, "BACnet MS/TP receive task started");

    while (1) {
        memset(&src, 0, sizeof(src));
        pdu_len = dlmstp_receive(&src, rx_buffer, sizeof(rx_buffer), 0);
        if (pdu_len > 0) {
            mstp_pdu_count++;
            BACNET_ADDRESS dest = {0};
            BACNET_NPDU_DATA npdu_data = {0};
            int apdu_offset = bacnet_npdu_decode(
                rx_buffer, pdu_len, &dest, &src, &npdu_data);
            if (apdu_offset > 0 && apdu_offset < (int)pdu_len) {
                mstp_apdu_count++;
                if (USER_MSTP_ENABLE_REPLY_POSTPONED) {
                    bacnet_track_confirmed_request_mstp(
                        &rx_buffer[apdu_offset],
                        pdu_len - apdu_offset,
                        &src,
                        &dest,
                        "mstp");
                }
                if ((apdu_offset + 4) <= (int)pdu_len) {
                    uint8_t pdu_type = rx_buffer[apdu_offset] & 0xF0;
                    uint8_t service_choice = rx_buffer[apdu_offset + 3];
                    if (pdu_type == PDU_TYPE_CONFIRMED_SERVICE_REQUEST &&
                        service_choice == SERVICE_CONFIRMED_READ_PROPERTY) {
                        mstp_rp_total++;
                        mstp_rp_last_value = Analog_Value_Present_Value(1);
                    } else if (pdu_type == PDU_TYPE_CONFIRMED_SERVICE_REQUEST &&
                        service_choice == SERVICE_CONFIRMED_WRITE_PROPERTY) {
                        mstp_wp_total++;
                    }
                }
                bacnet_log_whois_iam(&rx_buffer[apdu_offset], pdu_len - apdu_offset, "mstp");
                bacnet_datalink_lock(datalink_mstp);
                if (((rx_buffer[apdu_offset] & 0xF0U) ==
                        PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST) &&
                    (apdu_offset + 1 < (int)pdu_len) &&
                    (rx_buffer[apdu_offset + 1] == SERVICE_UNCONFIRMED_WHO_IS)) {
                    bacnet_whois_context_set("MSTP", &src, &dest);
                }
                apdu_handler(&src, &rx_buffer[apdu_offset], pdu_len - apdu_offset);
                bacnet_whois_context_clear();
                bacnet_datalink_unlock();
            } else {
                ESP_LOGW(TAG, "MS/TP RX frame decode failed: len=%u apdu_offset=%d src.len=%u src.mac=%u",
                    (unsigned)pdu_len, apdu_offset, (unsigned)src.len,
                    (unsigned)(src.len ? src.mac[0] : 0));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* Application entry: init Wi‑Fi and BACnet */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    const char *wifi_status_summary = "disabled";
    s_bacnet_ip_active = false;

    if (USER_ENABLE_BACNET_MSTP) {
        Device_Set_Max_APDU_Accepted(USER_BACNET_MSTP_MAX_APDU);
    }

    bacnet_datalink_mutex = xSemaphoreCreateMutex();
    if (!bacnet_datalink_mutex) {
        ESP_LOGE(TAG, "Failed to create BACnet datalink mutex");
    }
    
    /* If OVERRIDE_NVS_ON_FLASH is set, erase NVS to reset to code defaults.
     * Guard against wiping provisioned Wi-Fi when no compile-time defaults exist.
     */
    override_nvs_on_flash = USER_OVERRIDE_NVS_ON_FLASH;
    if (override_nvs_on_flash && USER_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "OVERRIDE_NVS_ON_FLASH ignored: USER_WIFI_SSID is empty and erase would remove provisioned Wi-Fi credentials");
        override_nvs_on_flash = 0;
    }

    if (override_nvs_on_flash) {
        ESP_LOGI(TAG, "Override flag set - erasing NVS to reset to defaults");
        nvs_flash_erase();
        ret = nvs_flash_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reinitialize NVS after erase: %d", ret);
        } else {
            ESP_LOGI(TAG, "NVS reinitialized successfully");
        }
    } else if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI(TAG, "NVS needs initialization");
        nvs_flash_erase();
        nvs_flash_init();
    } else if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized from existing data");
    }

    User_Settings_Print();

    if (USER_ENABLE_BACNET_IP) {
        wifi_startup_status_t wifi_status;

        /* Initialize network stack (must be done before WiFi init) */
        esp_netif_init();
        esp_event_loop_create_default();

        wifi_status = wifi_init_sta();

        if (wifi_status == WIFI_STARTUP_CONNECTED) {
            wifi_status_summary = "connected";
            s_bacnet_ip_active = true;
        } else {
            wifi_status_summary = "unavailable";
            ESP_LOGW(TAG, "WiFi unavailable; continuing with BACnet MS/TP only");
        }

        ESP_LOGI(TAG, "WiFi status: %s", wifi_status_summary);

        if (s_bacnet_ip_active) {
            ESP_LOGI(TAG, "Initializing BACnet stack (B/IP)");
            datalink_set(datalink_bip);
            if (!datalink_init(NULL)) {
                ESP_LOGE(TAG, "Failed to initialize BACnet datalink");
                s_bacnet_ip_active = false;
            } else {
                bacnet_register_with_bbmd();
            }
        }
    } else {
        ESP_LOGI(TAG, "WiFi status: %s", wifi_status_summary);
    }

    if (USER_ENABLE_BACNET_MSTP) {
        ESP_LOGI(TAG, "Initializing BACnet MS/TP");
        if (!bacnet_mstp_init()) {
            ESP_LOGE(TAG, "Failed to initialize BACnet MS/TP datalink");
        } else {
            datalink_set(datalink_mstp);
            if (!datalink_init((char *)&mstp_port)) {
                ESP_LOGE(TAG, "Failed to initialize BACnet MS/TP datalink interface");
            }
        }
    }

    if (s_bacnet_ip_active) {
        datalink_default = datalink_bip;
    } else if (USER_ENABLE_BACNET_MSTP) {
        datalink_default = datalink_mstp;
    }
    if (datalink_default) {
        datalink_set(datalink_default);
    }

    Device_Init(NULL);
    User_Settings_InitDeviceIdentity();

    /* Register service handlers - using bacnet-stack library handlers */
    ESP_LOGI(TAG, "Registering BACnet service handlers");
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, handler_i_am_add);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is_debug);
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);
    /* Read Property - REQUIRED for BACnet devices */
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROPERTY, handler_read_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE, handler_read_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROPERTY, handler_write_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_SUBSCRIBE_COV, handler_cov_subscribe);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_SUBSCRIBE_COV_PROPERTY, handler_cov_subscribe_property);

    /* Initialize COV subscription list */
    handler_cov_init();

    /* Create BACnet objects (AV, BV, AI, BI, BO) */
    bacnet_create_analog_values();
    bacnet_create_binary_values();
    bacnet_create_analog_inputs();
    bacnet_create_binary_inputs();
    bacnet_create_binary_outputs_with_gpio_sync();  /* Create BO with GPIO sync task */

    ESP_LOGI(TAG, "Broadcasting I-Am");
    if (s_bacnet_ip_active) {
        bacnet_datalink_lock(datalink_bip);
        (void)bacnet_send_i_am_with_reason("startup", "BIP", true);
        bacnet_datalink_unlock();
    }
    if (USER_ENABLE_BACNET_MSTP) {
        bacnet_datalink_lock(datalink_mstp);
        (void)bacnet_send_i_am_with_reason("startup", "MSTP", true);
        bacnet_datalink_unlock();
    }

    /* Initialize display */
    ESP_LOGI(TAG, "Initializing display");
    display_init();

    /* Start BACnet receive task to handle incoming messages */
    if (s_bacnet_ip_active) {
        if (xTaskCreate(bacnet_receive_task, "bacnet_rx", 16384, NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create bacnet_rx task");
        }
    }
    if (USER_ENABLE_BACNET_MSTP) {
        if (xTaskCreate(bacnet_mstp_receive_task, "bacnet_mstp_rx", 12288, NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create bacnet_mstp_rx task");
        }
    }
    if (xTaskCreate(bacnet_cov_task, "bacnet_cov", 24576, NULL, 4, &bacnet_cov_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create bacnet_cov task");
    }
#if BOARD_SEN54_ENABLED
    if (xTaskCreate(sen54_task, "sen54", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sen54 task");
    }
#else
    ESP_LOGI(TAG, "SEN54 disabled by configuration");
#endif

    if (USER_ENABLE_BACNET_MSTP) {
        ESP_LOGI(TAG, "BACnet MS/TP ready");
        dlmstp_reset_statistics();
    }

    /* Keep the task alive - maintenance + display updates */
    uint32_t display_tick = 0;
    uint32_t mstp_rx_tick = 0;
    uint32_t mstp_last_seen_pdu = 0;
    uint8_t mstp_alive_ticks = 0;
    while (1) {
        if (s_bacnet_ip_active) {
            bacnet_datalink_lock(datalink_bip);
            datalink_maintenance_timer(1);
            bacnet_datalink_unlock();
        }

        if (USER_ENABLE_BACNET_MSTP && ++mstp_rx_tick % 30 == 0) {
            uint32_t rx_bytes = MSTP_RS485_Rx_Bytes_Get_Reset();
            uint32_t preamble_55 = 0;
            uint32_t preamble_55ff = 0;
            struct dlmstp_statistics mstp_stats = {0};
            MSTP_RS485_Preamble_Counts_Get_Reset(&preamble_55, &preamble_55ff);
            dlmstp_fill_statistics(&mstp_stats);
#if !MSTP_DEBUG_ENABLE
            (void)rx_bytes;
            (void)preamble_55;
            (void)preamble_55ff;
#endif
            if (mstp_stats.bad_crc_counter > 0 ||
                mstp_stats.receive_invalid_frame_counter > 0 ||
                mstp_stats.lost_token_counter > 0) {
                ESP_LOGW(
                    TAG,
                    "MS/TP errors(30s): bad_crc=%lu invalid=%lu lost_token=%lu",
                    (unsigned long)mstp_stats.bad_crc_counter,
                    (unsigned long)mstp_stats.receive_invalid_frame_counter,
                    (unsigned long)mstp_stats.lost_token_counter);
            }
#if MSTP_DEBUG_ENABLE
            else {
                ESP_LOGD(
                    TAG,
                    "MS/TP 30s stats: rx_bytes=%lu preamble55=%lu preamble55ff=%lu pdu=%lu apdu=%lu rp=%lu wp=%lu valid=%lu invalid=%lu not_for_us=%lu bad_crc=%lu tx_frames=%lu rx_pdu=%lu poll=%lu lost_token=%lu sole_master=%d",
                    (unsigned long)rx_bytes,
                    (unsigned long)preamble_55,
                    (unsigned long)preamble_55ff,
                    (unsigned long)mstp_pdu_count,
                    (unsigned long)mstp_apdu_count,
                    (unsigned long)mstp_rp_total,
                    (unsigned long)mstp_wp_total,
                    (unsigned long)mstp_stats.receive_valid_frame_counter,
                    (unsigned long)mstp_stats.receive_invalid_frame_counter,
                    (unsigned long)mstp_stats.receive_valid_frame_not_for_us_counter,
                    (unsigned long)mstp_stats.bad_crc_counter,
                    (unsigned long)mstp_stats.transmit_frame_counter,
                    (unsigned long)mstp_stats.receive_pdu_counter,
                    (unsigned long)mstp_stats.poll_for_master_counter,
                    (unsigned long)mstp_stats.lost_token_counter,
                    dlmstp_sole_master() ? 1 : 0);
            }
#endif
            mstp_pdu_count = 0;
            mstp_apdu_count = 0;
            mstp_rp_total = 0;
            mstp_wp_total = 0;
            dlmstp_reset_statistics();
        }

        if (USER_ENABLE_BACNET_MSTP) {
            if (mstp_pdu_count != mstp_last_seen_pdu) {
                mstp_last_seen_pdu = mstp_pdu_count;
                mstp_alive_ticks = 6;
            } else if (mstp_alive_ticks > 0) {
                mstp_alive_ticks--;
            }
        } else {
            mstp_alive_ticks = 0;
        }

        display_set_link_status(
            wifi_connected_now(),
            USER_ENABLE_BACNET_MSTP && (mstp_alive_ticks > 0));
        
        /* Update display every 2 seconds */
        if (++display_tick % 2 == 0) {
            float av1 = Analog_Value_Present_Value(1);
            float av2 = Analog_Value_Present_Value(2);
            float av3 = Analog_Value_Present_Value(3);
            float av4 = Analog_Value_Present_Value(4);
            char ip_text[20];
            wifi_ip_string_now(ip_text, sizeof(ip_text));
            display_update_values(av1, av2, av3, av4);
            display_update_footer(
                USER_BACNET_DEVICE_INSTANCE,
                USER_MSTP_MAC_ADDRESS,
                ip_text);
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* COV task - handles COV timer and notifications */
static void bacnet_cov_task(void *pvParameters)
{
    (void)pvParameters;
    while (1) {
        char *active_datalink = datalink_default;
        if (!active_datalink) {
            if (s_bacnet_ip_active) {
                active_datalink = datalink_bip;
            } else if (USER_ENABLE_BACNET_MSTP) {
                active_datalink = datalink_mstp;
            }
        }

        if (active_datalink) {
            bacnet_datalink_lock(active_datalink);
            handler_cov_timer_seconds(1);
            handler_cov_task();
            bacnet_datalink_unlock();
        } else {
            handler_cov_timer_seconds(1);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void sen54_set_status_bi_if_changed(uint32_t instance,
                                           bool active,
                                           const char *label)
{
    BACNET_BINARY_PV current;
    BACNET_BINARY_PV next;

    current = Binary_Input_Present_Value(instance);
    next = active ? BINARY_ACTIVE : BINARY_INACTIVE;

    if (current != next) {
        Binary_Input_Present_Value_Set(instance, next);
        ESP_LOGI(TAG, "[SEN54] %s: %s", label, active ? "ON" : "OFF");
    }
}

static void sen54_update_status_objects(uint32_t status)
{
    sen54_set_status_bi_if_changed(1, (status & SEN54_STATUS_FAN_ERROR_BIT) != 0,
                                   "Fan Failure");
    sen54_set_status_bi_if_changed(2, (status & SEN54_STATUS_LASER_ERROR_BIT) != 0,
                                   "Laser Error");
    sen54_set_status_bi_if_changed(3, (status & SEN54_STATUS_VOC_ERROR_BIT) != 0,
                                   "VOC Sensor Error");
    sen54_set_status_bi_if_changed(4, (status & SEN54_STATUS_RHT_ERROR_BIT) != 0,
                                   "RHT Sensor Error");
}

static bool sen54_poll_and_publish_status(void)
{
    static bool status_valid = false;
    static uint32_t last_status = 0;
    uint32_t status = 0;

    if (!sen54_read_device_status(&status)) {
        ESP_LOGW(TAG, "[SEN54] Device status read failed");
        return false;
    }

    if (!status_valid || (status != last_status)) {
        ESP_LOGI(TAG, "[SEN54] Device Status = 0x%08lX", (unsigned long)status);
        sen54_update_status_objects(status);
        last_status = status;
        status_valid = true;
    }

    return true;
}

static bool sen54_publish_auto_cleaning_interval(uint32_t seconds)
{
    if (!Analog_Value_Present_Value_Set(
            SEN54_AUTO_CLEANING_AV_INSTANCE, (float)seconds, 16)) {
        return false;
    }

    sen54_auto_cleaning_interval_seconds = seconds;
    sen54_auto_cleaning_interval_valid = true;

    return true;
}

static void sen54_init_auto_cleaning_interval(void)
{
    uint32_t seconds = 0;

    if (sen54_read_auto_cleaning_interval(&seconds)) {
        if (sen54_publish_auto_cleaning_interval(seconds)) {
            ESP_LOGI(TAG, "[SEN54] Auto Cleaning Interval = %lu s",
                (unsigned long)seconds);
        } else {
            ESP_LOGE(TAG,
                "[SEN54] Auto Cleaning Interval read succeeded but AV%u update failed",
                (unsigned)SEN54_AUTO_CLEANING_AV_INSTANCE);
        }
    } else {
        ESP_LOGE(TAG, "[SEN54] Auto Cleaning Interval read failed");
    }
}

bool bacnet_av7_auto_cleaning_interval_write(
    float requested_value,
    float *applied_value,
    BACNET_ERROR_CLASS *error_class,
    BACNET_ERROR_CODE *error_code)
{
    uint32_t requested_seconds = 0;
    uint32_t old_seconds = 0;

    if (error_class) {
        *error_class = ERROR_CLASS_PROPERTY;
    }
    if (error_code) {
        *error_code = ERROR_CODE_VALUE_OUT_OF_RANGE;
    }

    if (!isfinite(requested_value) || requested_value <= 0.0f ||
        requested_value > 4294967295.0f) {
        return false;
    }

    requested_seconds = (uint32_t)requested_value;
    if ((float)requested_seconds != requested_value) {
        return false;
    }

    if (!sen54_write_auto_cleaning_interval(requested_seconds)) {
        if (error_class) {
            *error_class = ERROR_CLASS_DEVICE;
        }
        if (error_code) {
            *error_code = ERROR_CODE_OPERATIONAL_PROBLEM;
        }
        return false;
    }

    old_seconds = sen54_auto_cleaning_interval_valid
        ? sen54_auto_cleaning_interval_seconds
        : (uint32_t)Analog_Value_Present_Value(SEN54_AUTO_CLEANING_AV_INSTANCE);

    if (old_seconds != requested_seconds) {
        ESP_LOGI(TAG, "[SEN54] Auto Cleaning Interval changed:");
        ESP_LOGI(TAG, "Old: %lu s", (unsigned long)old_seconds);
        ESP_LOGI(TAG, "New: %lu s", (unsigned long)requested_seconds);
    }

    sen54_auto_cleaning_interval_seconds = requested_seconds;
    sen54_auto_cleaning_interval_valid = true;

    if (applied_value) {
        *applied_value = (float)requested_seconds;
    }

    return true;
}

/* SEN54 task - reads sensor data and writes to BACnet Analog Value objects
 *
 * PERIPHERAL-TO-BACNET MAPPING:
 * - AV1 (instance 1): Temperature (°C)
 * - AV2 (instance 2): Relative Humidity (%RH)
 * - AV3 (instance 3): PM2.5 concentration (μg/m³)
 * - AV4 (instance 4): VOC Index (dimensionless)
 */
static void sen54_task(void *pvParameters)
{
    (void)pvParameters;
    sen54_data_t sensor_data;
    uint8_t consecutive_failures = 0;
    TickType_t last_status_poll = 0;
    const TickType_t status_poll_interval_ticks = pdMS_TO_TICKS(5000);

    /* Give SEN54 time to boot after power-up */
    vTaskDelay(pdMS_TO_TICKS(3000));

    sen54_init();
    /* Publish BV2 = ACTIVE after every successful boot initialization.
     * BV2 (SEN54 Measurement Enable) is a runtime operating command only and
     * is intentionally not persisted across reboots.  This device is designed
     * for continuous environmental monitoring; measurement-enabled is always
     * the default state after any firmware restart, watchdog reset, brownout,
     * or power cycle, regardless of the value BV2 had before the reset. */
    if (sen54_is_measurement_enabled()) {
        Binary_Value_Present_Value_Set(SEN54_MEASUREMENT_ENABLE_BV_INSTANCE, BINARY_ACTIVE);
    }
    /* BV3 is a momentary command-only object and must always boot INACTIVE. */
    sen54_set_command_bv_inactive(SEN54_FAN_CLEANING_BV_INSTANCE);
    /* BV4 is a momentary command-only object and must always boot INACTIVE. */
    sen54_set_command_bv_inactive(SEN54_CLEAR_STATUS_BV_INSTANCE);
    sen54_init_auto_cleaning_interval();

    /* Initialize BI1..BI4 (BI0..BI3 logical mapping) from current SEN54 status. */
    sen54_poll_and_publish_status();
    last_status_poll = xTaskGetTickCount();

    /* Read temperature-compensation parameters once at startup (read-only) */
    (void)sen54_publish_temperature_compensation();

    /* Reapply stored temperature offset from NVS (if any) after sensor init */
    sen54_reapply_temperature_offset();

    /* Wait for the sensor fan and particle chamber to stabilize */
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1) {
        if ((xTaskGetTickCount() - last_status_poll) >= status_poll_interval_ticks) {
            sen54_poll_and_publish_status();
            last_status_poll = xTaskGetTickCount();
        }

        /* BV1 written ACTIVE triggers a full SEN54 reset (I2C 0xD304) */
        if (Binary_Value_Present_Value(1) == BINARY_ACTIVE) {
            ESP_LOGI(TAG, "[SEN54] Full reset command requested (BV1 ACTIVE)");
            esp_err_t err = sen54_full_reset();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "[SEN54] Full reset command failed (%s)", esp_err_to_name(err));
            }
            sen54_set_command_bv_inactive(1);
            /* Sync BV2 — full reset restarts measurement */
            Binary_Value_Present_Value_Set(SEN54_MEASUREMENT_ENABLE_BV_INSTANCE,
                sen54_is_measurement_enabled() ? BINARY_ACTIVE : BINARY_INACTIVE);

            if (sen54_poll_and_publish_status()) {
                last_status_poll = xTaskGetTickCount();
                /* After a full reset and successful status refresh, reapply stored offset */
                sen54_reapply_temperature_offset();
            }
            continue;
        }

        /* BV2 controls SEN54 measurement enable/disable */
        {
            BACNET_BINARY_PV bv2 = Binary_Value_Present_Value(SEN54_MEASUREMENT_ENABLE_BV_INSTANCE);
            bool currently_enabled = sen54_is_measurement_enabled();

            if (bv2 == BINARY_ACTIVE && !currently_enabled) {
                if (!sen54_start_measurement()) {
                    /* Command failed — revert BV2 to reflect actual state */
                    Binary_Value_Present_Value_Set(SEN54_MEASUREMENT_ENABLE_BV_INSTANCE, BINARY_INACTIVE);
                }
            } else if (bv2 == BINARY_INACTIVE && currently_enabled) {
                if (!sen54_stop_measurement()) {
                    /* Command failed — revert BV2 to reflect actual state */
                    Binary_Value_Present_Value_Set(SEN54_MEASUREMENT_ENABLE_BV_INSTANCE, BINARY_ACTIVE);
                }
            }
        }

        /* BV3 behaves as a momentary pushbutton for manual fan cleaning. */
        if (Binary_Value_Present_Value(SEN54_FAN_CLEANING_BV_INSTANCE) == BINARY_ACTIVE) {
            sen54_start_fan_cleaning();

            /* Always auto-return to INACTIVE (momentary command object). */
            sen54_set_command_bv_inactive(SEN54_FAN_CLEANING_BV_INSTANCE);
        }

        /* BV4 behaves as a momentary command object for clear device status. */
        if (Binary_Value_Present_Value(SEN54_CLEAR_STATUS_BV_INSTANCE) == BINARY_ACTIVE) {
            if (sen54_clear_device_status()) {
                if (sen54_poll_and_publish_status()) {
                    last_status_poll = xTaskGetTickCount();
                } else {
                    ESP_LOGE(TAG, "[SEN54] Device status refresh after clear failed");
                }
            }

            /* Always auto-return to INACTIVE (momentary command object). */
            sen54_set_command_bv_inactive(SEN54_CLEAR_STATUS_BV_INSTANCE);
        }

        if (sen54_is_measurement_enabled()) {
            if (sen54_read(&sensor_data)) {
                consecutive_failures = 0;
                Analog_Value_Present_Value_Set(1, sensor_data.temperature, 16);
                Analog_Value_Present_Value_Set(2, sensor_data.humidity,    16);
                Analog_Value_Present_Value_Set(3, sensor_data.pm2_5,       16);
                Analog_Value_Present_Value_Set(4, sensor_data.voc_index,   16);
                Analog_Value_Present_Value_Set(5, sensor_data.pm1_0,       16);
                Analog_Value_Present_Value_Set(6, sensor_data.pm4_0,       16);
            } else {
                consecutive_failures++;

                /* -1 signals no valid data to BACnet clients */
                Analog_Value_Present_Value_Set(1, -1.0f, 16);
                Analog_Value_Present_Value_Set(2, -1.0f, 16);
                Analog_Value_Present_Value_Set(3, -1.0f, 16);
                Analog_Value_Present_Value_Set(4, -1.0f, 16);
                Analog_Value_Present_Value_Set(5, -1.0f, 16);
                Analog_Value_Present_Value_Set(6, -1.0f, 16);

                if (consecutive_failures >= 5) {
                    ESP_LOGW(TAG, "SEN54 read failed %u times, reinitializing sensor",
                             (unsigned)consecutive_failures);
                    sen54_init();
                    if (sen54_poll_and_publish_status()) {
                        last_status_poll = xTaskGetTickCount();
                        /* Reapply stored offset after recovery init */
                        sen54_reapply_temperature_offset();
                    }
                    consecutive_failures = 0;
                    vTaskDelay(pdMS_TO_TICKS(1500));
                }
            }
        } /* if (sen54_is_measurement_enabled()) */

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void bacnet_register_with_bbmd(void)
{
    BACNET_IP_ADDRESS bbmd_addr = { { USER_BBMD_IP_OCTET_1, USER_BBMD_IP_OCTET_2,
                                     USER_BBMD_IP_OCTET_3, USER_BBMD_IP_OCTET_4 },
                                    USER_BBMD_PORT };
    int result = bvlc_register_with_bbmd(&bbmd_addr, USER_BBMD_TTL_SECONDS);
    ESP_LOGI(TAG, "BBMD register result: %d", result);
}

#if BACNET_DISCOVERY_DIAGNOSTICS
static void bacnet_track_confirmed_request_bip(
    const uint8_t *apdu,
    int apdu_len,
    const BACNET_ADDRESS *src,
    const char *link)
{
    uint8_t pdu_type = 0;
    uint8_t service_choice = 0;
    uint8_t invoke_id = 0;
    BACNET_READ_PROPERTY_DATA rp_data = { 0 };
    BACNET_RPM_DATA rpm_data = { 0 };
    int len = 0;

    if (!apdu || apdu_len < 5) {
        return;
    }
    pdu_type = apdu[0] & 0xF0;
    if (pdu_type != PDU_TYPE_CONFIRMED_SERVICE_REQUEST) {
        return;
    }
    if ((apdu[0] & BIT(3)) != 0) {
        /* segmented - skip */
        return;
    }
    invoke_id = apdu[2];
    service_choice = apdu[3];
    if (service_choice == SERVICE_CONFIRMED_READ_PROPERTY) {
        len = rp_decode_service_request(&apdu[4], (uint16_t)(apdu_len - 4), &rp_data);
        if (len > 0) {
            ESP_LOGI(TAG, "%s request received invoke=%u service=RP object=%u:%lu property=%lu array=%lu apdu_len=%d max_apdu=%u",
                link,
                (unsigned)invoke_id,
                (unsigned)rp_data.object_type,
                (unsigned long)rp_data.object_instance,
                (unsigned long)rp_data.object_property,
                (unsigned long)rp_data.array_index,
                apdu_len,
                (unsigned)Device_Max_APDU_Accepted());
        }
    } else if (service_choice == SERVICE_CONFIRMED_READ_PROP_MULTIPLE) {
        len = rpm_decode_object_id(&apdu[4], (unsigned)(apdu_len - 4), &rpm_data);
        if (len > 0) {
            len += rpm_decode_object_property(&apdu[4 + len], (unsigned)(apdu_len - 4 - len), &rpm_data);
            ESP_LOGI(TAG, "%s request received invoke=%u service=RPM object=%u:%lu property=%lu array=%lu apdu_len=%d max_apdu=%u",
                link,
                (unsigned)invoke_id,
                (unsigned)rpm_data.object_type,
                (unsigned long)rpm_data.object_instance,
                (unsigned long)rpm_data.object_property,
                (unsigned long)rpm_data.array_index,
                apdu_len,
                (unsigned)Device_Max_APDU_Accepted());
        }
    }
}
#endif

bool bacnet_sen54_temperature_compensation_write(
    uint32_t object_instance,
    float requested_value,
    float *applied_value,
    BACNET_ERROR_CLASS *error_class,
    BACNET_ERROR_CODE *error_code)
{
    sen54_temperature_compensation_t params;
    sen54_temperature_compensation_t new_params;
    esp_err_t rc;

    if (error_class) {
        *error_class = ERROR_CLASS_PROPERTY;
    }
    if (error_code) {
        *error_code = ERROR_CODE_VALUE_OUT_OF_RANGE;
    }

    /* Validate and read current parameters */
    rc = sen54_temperature_compensation_get(&params);
    if (rc != ESP_OK) {
        if (error_class) {
            *error_class = ERROR_CLASS_DEVICE;
        }
        if (error_code) {
            *error_code = ERROR_CODE_OPERATIONAL_PROBLEM;
        }
        return false;
    }

    new_params = params; /* start from current values */

    if (object_instance == SEN54_TEMP_OFFSET_AV_INSTANCE) {
        if (!isfinite(requested_value)) {
            return false;
        }
        long raw = lroundf(requested_value * 200.0f);
        if (raw < INT16_MIN || raw > INT16_MAX) {
            return false;
        }
        new_params.offset_c = (float)raw / 200.0f;
    } else if (object_instance == SEN54_TEMP_SLOPE_AV_INSTANCE) {
        if (!isfinite(requested_value)) {
            return false;
        }
        long raw = lroundf(requested_value * 10000.0f);
        if (raw < INT16_MIN || raw > INT16_MAX) {
            return false;
        }
        new_params.slope = (float)raw / 10000.0f;
    } else if (object_instance == SEN54_TEMP_TIME_CONSTANT_AV_INSTANCE) {
        if (!isfinite(requested_value) || requested_value < 0.0f ||
            requested_value > 65535.0f) {
            return false;
        }
        uint32_t requested = (uint32_t)requested_value;
        if ((float)requested != requested_value) {
            return false;
        }
        new_params.time_constant_s = (uint16_t)requested;
    } else {
        return false;
    }

    /* Write full parameter set to sensor */
    if (sen54_temperature_compensation_set(&new_params) != ESP_OK) {
        if (error_class) {
            *error_class = ERROR_CLASS_DEVICE;
        }
        if (error_code) {
            *error_code = ERROR_CODE_OPERATIONAL_PROBLEM;
        }
        return false;
    }

    /* Read back verified values */
    rc = sen54_temperature_compensation_get(&params);
    if (rc != ESP_OK) {
        if (error_class) {
            *error_class = ERROR_CLASS_DEVICE;
        }
        if (error_code) {
            *error_code = ERROR_CODE_OPERATIONAL_PROBLEM;
        }
        return false;
    }

    /* Update BACnet AVs with verified values */
    (void)Analog_Value_Present_Value_Set(SEN54_TEMP_OFFSET_AV_INSTANCE, params.offset_c, 16);
    (void)Analog_Value_Present_Value_Set(SEN54_TEMP_SLOPE_AV_INSTANCE, params.slope, 16);
    (void)Analog_Value_Present_Value_Set(SEN54_TEMP_TIME_CONSTANT_AV_INSTANCE, (float)params.time_constant_s, 16);

    /* Return applied value for the requested AV */
    if (applied_value) {
        if (object_instance == SEN54_TEMP_TIME_CONSTANT_AV_INSTANCE) {
            *applied_value = (float)params.time_constant_s;
        } else if (object_instance == SEN54_TEMP_OFFSET_AV_INSTANCE) {
            *applied_value = params.offset_c;
        } else {
            *applied_value = params.slope;
        }
    }

    return true;
}

static void sen54_reapply_temperature_offset(void)
{
    /* Do nothing if NVS override mode requests compiled defaults */
    if (override_nvs_on_flash) {
        ESP_LOGI(TAG, "[SEN54] NVS override active; not restoring temperature compensation from NVS");
        return;
    }

    /* Load AV8/AV9/AV10 persisted PVs from NVS (float blob). If a key
     * does not exist, fall back to the compiled default for that AV.
     */
    float offset_c = 0.0f;
    float slope = 0.0f;
    float tc_f = 0.0f;
    bool found_offset = false, found_slope = false, found_tc = false;
    esp_err_t rc;

    rc = bacnet_nvs_load_av_pv(SEN54_TEMP_OFFSET_AV_INSTANCE, &offset_c, &found_offset);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "[SEN54] NVS read error for AV%u offset: %s", (unsigned)SEN54_TEMP_OFFSET_AV_INSTANCE, esp_err_to_name(rc));
    }
    rc = bacnet_nvs_load_av_pv(SEN54_TEMP_SLOPE_AV_INSTANCE, &slope, &found_slope);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "[SEN54] NVS read error for AV%u slope: %s", (unsigned)SEN54_TEMP_SLOPE_AV_INSTANCE, esp_err_to_name(rc));
    }
    rc = bacnet_nvs_load_av_pv(SEN54_TEMP_TIME_CONSTANT_AV_INSTANCE, &tc_f, &found_tc);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "[SEN54] NVS read error for AV%u time-constant: %s", (unsigned)SEN54_TEMP_TIME_CONSTANT_AV_INSTANCE, esp_err_to_name(rc));
    }

    /* If not found, use compiled default from USER_AV_INITIAL_VALUES (map instance->index) */
    if (!found_offset) {
        unsigned idx = Analog_Value_Instance_To_Index(SEN54_TEMP_OFFSET_AV_INSTANCE);
        if (idx < USER_AV_COUNT) {
            offset_c = USER_AV_INITIAL_VALUES[idx];
            ESP_LOGI(TAG, "[SEN54] AV%u not in NVS; using compiled default %.3f",
                     (unsigned)SEN54_TEMP_OFFSET_AV_INSTANCE, offset_c);
        }
    } else {
        ESP_LOGI(TAG, "[SEN54] Compensation NVS load: AV8 offset=%.3f found=yes", offset_c);
    }
    if (!found_slope) {
        unsigned idx = Analog_Value_Instance_To_Index(SEN54_TEMP_SLOPE_AV_INSTANCE);
        if (idx < USER_AV_COUNT) {
            slope = USER_AV_INITIAL_VALUES[idx];
            ESP_LOGI(TAG, "[SEN54] AV%u not in NVS; using compiled default %.5f",
                     (unsigned)SEN54_TEMP_SLOPE_AV_INSTANCE, slope);
        }
    } else {
        ESP_LOGI(TAG, "[SEN54] Compensation NVS load: AV9 slope=%.5f found=yes", slope);
    }
    if (!found_tc) {
        unsigned idx = Analog_Value_Instance_To_Index(SEN54_TEMP_TIME_CONSTANT_AV_INSTANCE);
        if (idx < USER_AV_COUNT) {
            tc_f = USER_AV_INITIAL_VALUES[idx];
            ESP_LOGI(TAG, "[SEN54] AV%u not in NVS; using compiled default %.0f",
                     (unsigned)SEN54_TEMP_TIME_CONSTANT_AV_INSTANCE, tc_f);
        }
    } else {
        ESP_LOGI(TAG, "[SEN54] Compensation NVS load: AV10 time_constant=%.0f found=yes", tc_f);
    }

    /* Validate and convert tc to uint16 */
    if (!isfinite(offset_c) || !isfinite(slope) || !isfinite(tc_f)) {
        ESP_LOGW(TAG, "[SEN54] Loaded compensation values not finite; skipping reapply");
        return;
    }

    uint16_t time_constant = 0;
    if (tc_f < 0.0f) {
        time_constant = 0;
    } else if (tc_f > 65535.0f) {
        time_constant = 65535;
    } else {
        time_constant = (uint16_t)tc_f;
    }

    /* Prepare parameters and apply them in one operation */
    sen54_temperature_compensation_t newp = { .offset_c = offset_c, .slope = slope, .time_constant_s = time_constant };

    /* First-run NVS initialization: if any of the AV8/AV9/AV10 keys were not
     * found in NVS, write the compiled default value into NVS so subsequent
     * boots read the stored value. Commit once after all writes succeed.
     */
    if (!found_offset || !found_slope || !found_tc) {
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open("bacnet", NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[SEN54] NVS open failed for initialization: %s", esp_err_to_name(err));
        } else {
            bool wrote_any = false;
            esp_err_t set_err;
            char key[32];

            if (!found_offset) {
                snprintf(key, sizeof(key), "analog_%lu_val", (unsigned long)SEN54_TEMP_OFFSET_AV_INSTANCE);
                set_err = nvs_set_blob(nvs_handle, key, &offset_c, sizeof(offset_c));
                if (set_err == ESP_OK) {
                    wrote_any = true;
                    ESP_LOGI(TAG, "[SEN54] No saved AV%u offset; initialized NVS with default %.3f",
                             (unsigned)SEN54_TEMP_OFFSET_AV_INSTANCE, offset_c);
                } else {
                    ESP_LOGW(TAG, "[SEN54] Failed to initialize AV%u offset in NVS: %s",
                             (unsigned)SEN54_TEMP_OFFSET_AV_INSTANCE, esp_err_to_name(set_err));
                }
            }

            if (!found_slope) {
                snprintf(key, sizeof(key), "analog_%lu_val", (unsigned long)SEN54_TEMP_SLOPE_AV_INSTANCE);
                set_err = nvs_set_blob(nvs_handle, key, &slope, sizeof(slope));
                if (set_err == ESP_OK) {
                    wrote_any = true;
                    ESP_LOGI(TAG, "[SEN54] No saved AV%u slope; initialized NVS with default %.5f",
                             (unsigned)SEN54_TEMP_SLOPE_AV_INSTANCE, slope);
                } else {
                    ESP_LOGW(TAG, "[SEN54] Failed to initialize AV%u slope in NVS: %s",
                             (unsigned)SEN54_TEMP_SLOPE_AV_INSTANCE, esp_err_to_name(set_err));
                }
            }

            if (!found_tc) {
                snprintf(key, sizeof(key), "analog_%lu_val", (unsigned long)SEN54_TEMP_TIME_CONSTANT_AV_INSTANCE);
                set_err = nvs_set_blob(nvs_handle, key, &tc_f, sizeof(tc_f));
                if (set_err == ESP_OK) {
                    wrote_any = true;
                    ESP_LOGI(TAG, "[SEN54] No saved AV%u time constant; initialized NVS with default %.0f",
                             (unsigned)SEN54_TEMP_TIME_CONSTANT_AV_INSTANCE, tc_f);
                } else {
                    ESP_LOGW(TAG, "[SEN54] Failed to initialize AV%u time constant in NVS: %s",
                             (unsigned)SEN54_TEMP_TIME_CONSTANT_AV_INSTANCE, esp_err_to_name(set_err));
                }
            }

            if (wrote_any) {
                esp_err_t commit_rc = nvs_commit(nvs_handle);
                if (commit_rc == ESP_OK) {
                    ESP_LOGI(TAG, "[SEN54] Committed initialized temperature-compensation defaults to NVS");
                } else {
                    ESP_LOGW(TAG, "[SEN54] Failed to commit initialized defaults to NVS: %s", esp_err_to_name(commit_rc));
                }
            }

            nvs_close(nvs_handle);
        }
    }

    if (sen54_temperature_compensation_set(&newp) != ESP_OK) {
        ESP_LOGE(TAG, "[SEN54] Failed to apply temperature compensation from NVS: offset=%.3f slope=%.5f tc=%u",
                 offset_c, slope, (unsigned)time_constant);
        return;
    }

    /* Read back and update AVs */
    sen54_temperature_compensation_t params;
    if (sen54_temperature_compensation_get(&params) != ESP_OK) {
        ESP_LOGE(TAG, "[SEN54] Readback failed after applying temperature compensation");
        return;
    }

    (void)Analog_Value_Present_Value_Set(SEN54_TEMP_OFFSET_AV_INSTANCE, params.offset_c, 16);
    (void)Analog_Value_Present_Value_Set(SEN54_TEMP_SLOPE_AV_INSTANCE, params.slope, 16);
    (void)Analog_Value_Present_Value_Set(SEN54_TEMP_TIME_CONSTANT_AV_INSTANCE, (float)params.time_constant_s, 16);

    ESP_LOGI(TAG, "[SEN54] Restored temperature compensation from NVS: offset=%.3f C slope=%.5f time_constant=%u s",
             params.offset_c, params.slope, (unsigned)params.time_constant_s);
}