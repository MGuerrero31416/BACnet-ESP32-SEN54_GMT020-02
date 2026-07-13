/**************************************************************************
 *
 * Copyright (C) 2005 Steve Karg <skarg@users.sourceforge.net>
 *
 * SPDX-License-Identifier: MIT
 *
 *********************************************************************/
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
/* BACnet Stack defines - first */
#include "bacnet/bacdef.h"
/* BACnet Stack API */
#include "bacnet/bacdcode.h"
#include "bacnet/bacerror.h"
#include "bacnet/bactext.h"
#include "bacnet/bacdevobjpropref.h"
#include "bacnet/apdu.h"
#include "bacnet/npdu.h"
#include "bacnet/abort.h"
#include "bacnet/reject.h"
#include "bacnet/rp.h"
/* basic objects, services, TSM, and datalink */
#include "bacnet/basic/object/device.h"
#if (BACNET_PROTOCOL_REVISION >= 17)
#include "bacnet/basic/object/netport.h"
#endif
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/basic/services.h"
#include "bacnet/basic/sys/debug.h"
#include "bacnet/datalink/datalink.h"
#include "bacnet/datalink/dlmstp.h"

#ifndef OBJECT_LIST_DEBUG
#define OBJECT_LIST_DEBUG 0
#endif

#ifndef RP_TX_PATH_DEBUG
#define RP_TX_PATH_DEBUG 1
#endif

#ifndef USER_BACNET_ROUTED_COMPAT_MODE
#define USER_BACNET_ROUTED_COMPAT_MODE 0
#endif

#if RP_TX_PATH_DEBUG
static const char *RP_TX_DEBUG_TAG = "rp_tx_dbg";
#endif

#ifndef BACNET_DISCOVERY_DIAGNOSTICS
#define BACNET_DISCOVERY_DIAGNOSTICS 0
#endif

#if OBJECT_LIST_DEBUG
static const char *OBJECT_LIST_DEBUG_TAG = "obj_list_dbg";

static bool object_list_debug_is_target(const BACNET_READ_PROPERTY_DATA *rpdata)
{
    return rpdata && (rpdata->object_property == PROP_OBJECT_LIST);
}

static void object_list_debug_log_reply(
    const char *result,
    uint8_t invoke_id,
    BACNET_PROPERTY_ID property,
    bool abnormal)
{
    const char *property_name =
        bactext_property_name_default(property, "unknown-property");

    if (abnormal) {
        ESP_LOGW(
            OBJECT_LIST_DEBUG_TAG,
            "handler_read_property reply=%s invoke_id=%u property=%s",
            result,
            (unsigned)invoke_id,
            property_name);
    } else {
        ESP_LOGI(
            OBJECT_LIST_DEBUG_TAG,
            "handler_read_property reply=%s invoke_id=%u property=%s",
            result,
            (unsigned)invoke_id,
            property_name);
    }
}
#endif

/** @file h_rp.c  Handles Read Property requests. */

static bool object_list_debug_target(const BACNET_READ_PROPERTY_DATA *rpdata)
{
    return rpdata && (rpdata->object_property == PROP_OBJECT_LIST);
}

static const char *object_list_debug_result_name(int len)
{
    if (len >= 0) {
        return "ACK";
    }
    if (len == BACNET_STATUS_ABORT) {
        return "Abort";
    }
    if (len == BACNET_STATUS_ERROR) {
        return "Error";
    }
    if (len == BACNET_STATUS_REJECT) {
        return "Reject";
    }

    return "Unknown";
}

static void object_list_debug_log_request(
    uint8_t invoke_id,
    uint32_t array_index)
{
    ESP_LOGI(
        "obj_list",
        "Object_List request invoke=%u array_index=%lu",
        (unsigned)invoke_id,
        (unsigned long)array_index);
}

static void object_list_debug_log_response(
    uint8_t invoke_id,
    int property_value_len,
    int final_result_code,
    int apdu_len,
    const BACNET_READ_PROPERTY_DATA *rpdata)
{
    uint32_t count = 0;

    if (!rpdata) {
        return;
    }

    if (final_result_code >= 0) {
        if (rpdata->array_index == BACNET_ARRAY_ALL) {
            count = Device_Object_List_Count();
            ESP_LOGI(
                "obj_list",
                "Object_List response invoke=%u result=ACK response_apdu_len=%d array=ALL count=%lu property_value_len=%d",
                (unsigned)invoke_id,
                apdu_len,
                (unsigned long)count,
                property_value_len);
        } else if (rpdata->array_index == 0) {
            count = Device_Object_List_Count();
            ESP_LOGI(
                "obj_list",
                "Object_List response invoke=%u result=ACK response_apdu_len=%d array=0 returned_count=%lu property_value_len=%d",
                (unsigned)invoke_id,
                apdu_len,
                (unsigned long)count,
                property_value_len);
        } else {
            ESP_LOGI(
                "obj_list",
                "Object_List response invoke=%u result=ACK response_apdu_len=%d array=%lu property_value_len=%d",
                (unsigned)invoke_id,
                apdu_len,
                (unsigned long)rpdata->array_index,
                property_value_len);
        }
    } else {
        ESP_LOGW(
            "obj_list",
            "Object_List response invoke=%u result=%s",
            (unsigned)invoke_id,
            object_list_debug_result_name(final_result_code));
    }
}

/** Handler for a ReadProperty Service request.
 * @ingroup DSRP
 * This handler will be invoked by apdu_handler() if it has been enabled
 * by a call to apdu_set_confirmed_handler().
 * This handler builds a response packet, which is
 * - an Abort if
 *   - the message is segmented
 *   - if decoding fails
 *   - if the response would be too large
 * - the result from Device_Read_Property(), if it succeeds
 * - an Error if Device_Read_Property() fails
 *   or there isn't enough room in the APDU to fit the data.
 *
 * @param service_request [in] The contents of the service request.
 * @param service_len [in] The length of the service_request.
 * @param src [in] BACNET_ADDRESS of the source of the message
 * @param service_data [in] The BACNET_CONFIRMED_SERVICE_DATA information
 *                          decoded from the APDU header of this message.
 */
void handler_read_property(
    uint8_t *service_request,
    uint16_t service_len,
    BACNET_ADDRESS *src,
    BACNET_CONFIRMED_SERVICE_DATA *service_data)
{
    BACNET_READ_PROPERTY_DATA rpdata;
    int len = 0;
    int pdu_len = 0;
    int apdu_len = -1;
    int npdu_len = -1;
    int ack_end_len = 0;
    int property_value_len = -1;
    BACNET_NPDU_DATA npdu_data;
    bool error = true; /* assume that there is an error */
    int bytes_sent = 0;
    BACNET_ADDRESS my_address;

    /* configure default error code as an abort since it is common */
    rpdata.error_code = ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED;
    /* encode the NPDU portion of the packet */
    datalink_get_my_address(&my_address);
    npdu_encode_npdu_data(&npdu_data, false, service_data->priority);
    npdu_len = npdu_encode_pdu(
        &Handler_Transmit_Buffer[0], src, &my_address, &npdu_data);
    if (npdu_len <= 0) {
        /* If 0 or negative, there were problems with the data or encoding. */
        len = BACNET_STATUS_ABORT;
        debug_print("RP: npdu_encode_pdu error.  Sending Abort!\n");
    } else if (service_len == 0) {
        len = BACNET_STATUS_REJECT;
        rpdata.error_code = ERROR_CODE_REJECT_MISSING_REQUIRED_PARAMETER;
        debug_print("RP: Missing Required Parameter. Sending Reject!\n");
    } else if (service_data->segmented_message) {
        /* we don't support segmentation - send an abort */
        len = BACNET_STATUS_ABORT;
        debug_print("RP: Segmented message.  Sending Abort!\n");
    } else {
        len = rp_decode_service_request(service_request, service_len, &rpdata);
        if (len <= 0) {
        } else {
#if BACNET_DISCOVERY_DIAGNOSTICS
            if (object_list_debug_target(&rpdata)) {
                object_list_debug_log_request(
                    service_data->invoke_id, rpdata.array_index);
            }
#endif

            /* Compact transport-aware logging and routed-compat check
             * Determine request link based on currently selected datalink.
             * The application holds the datalink lock during apdu_handler(),
             * so this reflects the transport the request arrived on. */
            bool is_bip = datalink_is_bip();
            bool is_mstp = datalink_is_mstp();
            bool routed_request = (src && src->net != 0 && src->net != BACNET_BROADCAST_NETWORK);
            const char *request_link = is_bip ? "BIP" : (is_mstp ? "MSTP" : "OTHER");
            if (object_list_debug_target(&rpdata)) {
                ESP_LOGI(
                    "obj_list",
                    "request_link=%s array_index=%lu routed_compat_enabled=%d routed_request=%d",
                    request_link,
                    (unsigned long)rpdata.array_index,
                    (int)USER_BACNET_ROUTED_COMPAT_MODE,
                    (int)routed_request);
            }
                /* When the object-type in the Object Identifier parameter
                    contains the value DEVICE and the instance in the 'Object
                    Identifier' parameter contains the value 4194303, the responding
                    BACnet-user shall treat the Object Identifier as if it correctly
                    matched the local Device object. This allows the device instance
                    of a device that does not generate I-Am messages to be
                    determined. */
            if ((rpdata.object_type == OBJECT_DEVICE) &&
                (rpdata.object_instance == BACNET_MAX_INSTANCE)) {
                rpdata.object_instance = Device_Object_Instance_Number();
            }
#if (BACNET_PROTOCOL_REVISION >= 17)
            /* When the object-type in the Object Identifier parameter
               contains the value NETWORK_PORT and the instance in the 'Object
               Identifier' parameter contains the value 4194303, the responding
               BACnet-user shall treat the Object Identifier as if it correctly
               matched the local Network Port object representing the network
               port through which the request was received. This allows the
               network port instance of the network port that was used to
               receive the request to be determined. */
            if ((rpdata.object_type == OBJECT_NETWORK_PORT) &&
                (rpdata.object_instance == BACNET_MAX_INSTANCE)) {
                rpdata.object_instance = Network_Port_Index_To_Instance(0);
            }
#endif
            apdu_len = rp_ack_encode_apdu_init(
                &Handler_Transmit_Buffer[npdu_len], service_data->invoke_id,
                &rpdata);
            /* configure our storage */
            ack_end_len = rp_ack_encode_apdu_object_property_end(NULL);
            rpdata.application_data =
                &Handler_Transmit_Buffer[npdu_len + apdu_len];
            rpdata.application_data_len =
                sizeof(Handler_Transmit_Buffer) -
                (npdu_len + apdu_len + ack_end_len);
            if (!read_property_bacnet_array_valid(&rpdata)) {
                len = BACNET_STATUS_ERROR;
            } else {
                /* Apply routed-compat abort only when compatibility mode is
                 * enabled AND the request arrived via MS/TP or is genuinely
                 * routed (src.net != 0). Do not apply for ordinary B/IP
                 * requests. */
                bool compat_applied = false;
                if (USER_BACNET_ROUTED_COMPAT_MODE &&
                    (rpdata.object_type == OBJECT_DEVICE) &&
                    (rpdata.object_property == PROP_OBJECT_LIST) &&
                    (rpdata.array_index == BACNET_ARRAY_ALL) &&
                    (is_mstp || routed_request)) {
                    /*
                     * Routed/MS/TP compatibility fallback:
                     * preserve indexed Object_List reads for MS/TP or routed
                     * requests, but allow Object_List[ALL] for direct
                     * BACnet/IP clients.
                     */
                    rpdata.error_code = ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED;
                    len = BACNET_STATUS_ABORT;
                    compat_applied = true;
                    ESP_LOGW(
                        "obj_list",
                        "compat_abort_applied=yes invoke=%u link=%s routed=%d",
                        (unsigned)service_data->invoke_id,
                        request_link,
                        (int)routed_request);
                } else {
                    /* normal processing */
                    len = Device_Read_Property(&rpdata);
                }
                /* Save the property value encoded length before it may be
                 * overwritten by subsequent encoding operations. */
                property_value_len = (len >= 0) ? len : -1;
            }
            if (len >= 0) {
                apdu_len += len;
                len = rp_ack_encode_apdu_object_property_end(
                    &Handler_Transmit_Buffer[npdu_len + apdu_len]);
                apdu_len += len;
                if (apdu_len > service_data->max_resp) {
                    /* too big for the sender - send an abort!
                       Setting of error code needed here as read property
                       processing may have overridden the default set at start
                     */
                    rpdata.error_code =
                        ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED;
                    len = BACNET_STATUS_ABORT;
                } else {
                    error = false;
                }
            }
#if BACNET_DISCOVERY_DIAGNOSTICS
            if (object_list_debug_target(&rpdata)) {
                object_list_debug_log_response(
                    service_data->invoke_id, property_value_len, len, apdu_len, &rpdata);
            }
#endif
        }
    }
    if (error) {
        if (len == BACNET_STATUS_ABORT) {
            apdu_len = abort_encode_apdu(
                &Handler_Transmit_Buffer[npdu_len], service_data->invoke_id,
                abort_convert_error_code(rpdata.error_code), true);
            debug_print("RP: Sending Abort!\n");
#if OBJECT_LIST_DEBUG
            if (object_list_debug_is_target(&rpdata)) {
                object_list_debug_log_reply(
                    "Abort", service_data->invoke_id, rpdata.object_property,
                    true);
            }
#endif
        } else if (len == BACNET_STATUS_ERROR) {
            apdu_len = bacerror_encode_apdu(
                &Handler_Transmit_Buffer[npdu_len], service_data->invoke_id,
                SERVICE_CONFIRMED_READ_PROPERTY, rpdata.error_class,
                rpdata.error_code);
            debug_print("RP: Sending Error!\n");
#if OBJECT_LIST_DEBUG
            if (object_list_debug_is_target(&rpdata)) {
                object_list_debug_log_reply(
                    "Error", service_data->invoke_id, rpdata.object_property,
                    true);
            }
#endif
        } else if (len == BACNET_STATUS_REJECT) {
            apdu_len = reject_encode_apdu(
                &Handler_Transmit_Buffer[npdu_len], service_data->invoke_id,
                reject_convert_error_code(rpdata.error_code));
            debug_print("RP: Sending Reject!\n");
#if OBJECT_LIST_DEBUG
            if (object_list_debug_is_target(&rpdata)) {
                object_list_debug_log_reply(
                    "Reject", service_data->invoke_id, rpdata.object_property,
                    true);
            }
#endif
        }
    }
    pdu_len = npdu_len + apdu_len;

#if RP_TX_PATH_DEBUG
    if (object_list_debug_target(&rpdata)) {
        (void)npdu_data;
        (void)my_address;
    }
#endif

    bytes_sent = datalink_send_pdu(
        src, &npdu_data, &Handler_Transmit_Buffer[0], pdu_len);

    /* Compact proof log for Object_List requests */
    if (object_list_debug_target(&rpdata)) {
        ESP_LOGI(
            "obj_list",
            "request_link=%s array_index=%lu compat_abort_applied=%s client_max_resp=%u encoded_response_len=%d final_result=%s datalink_send_ret=%d",
            (datalink_is_bip() ? "BIP" : (datalink_is_mstp() ? "MSTP" : "OTHER")),
            (unsigned long)rpdata.array_index,
            ((USER_BACNET_ROUTED_COMPAT_MODE && (rpdata.object_type == OBJECT_DEVICE) && (rpdata.object_property == PROP_OBJECT_LIST) && (rpdata.array_index == BACNET_ARRAY_ALL) && (datalink_is_mstp() || (src && src->net != 0 && src->net != BACNET_BROADCAST_NETWORK)))) ? "yes" : "no",
            (unsigned)service_data->max_resp,
            apdu_len,
            object_list_debug_result_name(len),
            bytes_sent);
    }

#if RP_TX_PATH_DEBUG
    if (object_list_debug_target(&rpdata)) {
        ESP_LOGI(
            RP_TX_DEBUG_TAG,
            "post-send invoke=%u service=ReadProperty datalink_send_ret=%d final_pdu_len=%d",
            (unsigned)service_data->invoke_id,
            bytes_sent,
            pdu_len);
    }
#endif

    if (bytes_sent <= 0) {
#if RP_TX_PATH_DEBUG
        if (object_list_debug_target(&rpdata)) {
            DLMSTP_SEND_STATUS status = dlmstp_send_status_last();
            const char *reason = dlmstp_send_status_text(status);
            ESP_LOGW(
                RP_TX_DEBUG_TAG,
                "post-send warning invoke=%u reason=%s datalink_send_ret=%d final_pdu_len=%d",
                (unsigned)service_data->invoke_id,
                reason,
                bytes_sent,
                pdu_len);
        }
#endif
        debug_perror("RP: Failed to send PDU");
    }

    return;
}
