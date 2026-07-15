/**
 * @file
 * @author Steve Karg <skarg@users.sourceforge.net>
 * @date February 2023
 * @brief Implementation of the Network Layer using BACnet MS/TP transport
 * @copyright SPDX-License-Identifier: MIT
 * @defgroup DLMSTP BACnet MS/TP DataLink Network Layer
 * @ingroup DataLink
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* BACnet Stack defines - first */
#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"
/* BACnet Stack API */
#include "bacnet/basic/sys/ringbuf.h"
#include "bacnet/basic/sys/mstimer.h"
#include "bacnet/datalink/crc.h"
#include "bacnet/datalink/mstp.h"
#include "bacnet/datalink/dlmstp.h"
#include "bacnet/datalink/mstpdef.h"
#include "bacnet/npdu.h"
#include "bacnet/bacaddr.h"
#include "bacnet/basic/sys/debug.h"
#if defined(ESP_PLATFORM)
#include "esp_log.h"
#endif

/* the current MSTP port that the datalink is using */
static struct mstp_port_struct_t *MSTP_Port;
static DLMSTP_SEND_STATUS DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_OK;

static bool dlmstp_destination_valid(const BACNET_ADDRESS *dest)
{
    if (!dest || (dest->mac_len == 0)) {
        return true;
    }
    if (dest->mac[0] <= DEFAULT_MAX_MASTER) {
        return true;
    }
    if (dest->mac[0] == MSTP_BROADCAST_ADDRESS) {
        return true;
    }

    return false;
}

static bool dlmstp_master_has_token(MSTP_MASTER_STATE master_state)
{
    return (master_state == MSTP_MASTER_STATE_USE_TOKEN) ||
        (master_state == MSTP_MASTER_STATE_DONE_WITH_TOKEN) ||
        (master_state == MSTP_MASTER_STATE_WAIT_FOR_REPLY) ||
        (master_state == MSTP_MASTER_STATE_PASS_TOKEN);
}

static bool dlmstp_master_can_transmit(MSTP_MASTER_STATE master_state)
{
    return (master_state == MSTP_MASTER_STATE_USE_TOKEN) ||
        (master_state == MSTP_MASTER_STATE_ANSWER_DATA_REQUEST);
}

static const char *dlmstp_master_state_text_short(MSTP_MASTER_STATE state)
{
    switch (state) {
        case MSTP_MASTER_STATE_ANSWER_DATA_REQUEST:
            return "ANSWER_DATA_REQUEST";
        case MSTP_MASTER_STATE_USE_TOKEN:
            return "USE_TOKEN";
        case MSTP_MASTER_STATE_WAIT_FOR_REPLY:
            return "WAIT_FOR_REPLY";
        case MSTP_MASTER_STATE_DONE_WITH_TOKEN:
            return "DONE_WITH_TOKEN";
        case MSTP_MASTER_STATE_IDLE:
            return "IDLE";
        default:
            break;
    }

    return "OTHER";
}

static DLMSTP_TX_SOURCE dlmstp_classify_tx_source(
    const uint8_t *pdu,
    unsigned pdu_len)
{
    int npdu_offset = 0;
    BACNET_ADDRESS dest = { 0 };
    BACNET_ADDRESS src = { 0 };
    BACNET_NPDU_DATA npdu_data = { 0 };
    const uint8_t *apdu = NULL;
    unsigned apdu_len = 0;
    uint8_t pdu_type = 0;

    if (!pdu || (pdu_len < 2)) {
        return DLMSTP_TX_SOURCE_OTHER;
    }

    npdu_offset = bacnet_npdu_decode(pdu, (uint16_t)pdu_len, &dest, &src, &npdu_data);
    if ((npdu_offset <= 0) || ((unsigned)npdu_offset >= pdu_len)) {
        return DLMSTP_TX_SOURCE_OTHER;
    }

    apdu = &pdu[npdu_offset];
    apdu_len = pdu_len - (unsigned)npdu_offset;
    pdu_type = (uint8_t)(apdu[0] & 0xF0);

    if ((pdu_type == PDU_TYPE_SIMPLE_ACK) ||
        (pdu_type == PDU_TYPE_COMPLEX_ACK) ||
        (pdu_type == PDU_TYPE_ERROR) ||
        (pdu_type == PDU_TYPE_REJECT) ||
        (pdu_type == PDU_TYPE_ABORT)) {
        return DLMSTP_TX_SOURCE_FINAL_ACK;
    }
    if ((pdu_type == PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST) && (apdu_len >= 2)) {
        if (apdu[1] == SERVICE_UNCONFIRMED_I_AM) {
            return DLMSTP_TX_SOURCE_I_AM;
        }
        if (apdu[1] == SERVICE_UNCONFIRMED_COV_NOTIFICATION) {
            return DLMSTP_TX_SOURCE_COV;
        }
    }

    return DLMSTP_TX_SOURCE_OTHER;
}

static struct dlmstp_packet *dlmstp_oldest_packet(
    struct dlmstp_user_data_t *user,
    unsigned long *age_ms)
{
    struct dlmstp_packet *pkt = NULL;
    unsigned long now_ms = mstimer_now();

    if (age_ms) {
        *age_ms = 0;
    }
    if (!user || Ringbuf_Empty(&user->PDU_Queue)) {
        return NULL;
    }

    pkt = (struct dlmstp_packet *)(void *)Ringbuf_Peek(&user->PDU_Queue);
    if (pkt && age_ms && pkt->queued_ms) {
        *age_ms = now_ms - pkt->queued_ms;
    }

    return pkt;
}

bool dlmstp_token_held(void)
{
    if (!MSTP_Port) {
        return false;
    }

    return dlmstp_master_has_token(MSTP_Port->master_state);
}

bool dlmstp_can_transmit_now(void)
{
    if (!MSTP_Port) {
        return false;
    }

    return dlmstp_master_can_transmit(MSTP_Port->master_state);
}

/**
 * @brief send an PDU via MSTP
 * @param dest - BACnet destination address
 * @param npdu_data - network layer information
 * @param pdu - PDU data to send
 * @param pdu_len - number of bytes of PDU data to send
 * @return number of bytes sent on success, zero on failure
 */
int dlmstp_send_pdu(
    BACNET_ADDRESS *dest,
    BACNET_NPDU_DATA *npdu_data,
    uint8_t *pdu,
    unsigned pdu_len)
{
    int bytes_sent = 0;
    unsigned i = 0; /* loop counter */
    struct dlmstp_user_data_t *user = NULL;
    struct dlmstp_packet *pkt = NULL;
    struct dlmstp_packet *oldest_pkt = NULL;
    unsigned queue_capacity = 0;
    unsigned long oldest_age_ms = 0;
    uint8_t frame_type = 0;
    uint8_t destination_mac = MSTP_BROADCAST_ADDRESS;
    DLMSTP_TX_SOURCE tx_source = DLMSTP_TX_SOURCE_OTHER;
    bool responder_match = false;
    uint16_t immediate_frame_len = 0;
    MSTP_MASTER_STATE master_state = MSTP_MASTER_STATE_IDLE;

    DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_OTHER;

    if (!MSTP_Port) {
        DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_NO_PORT;
        return 0;
    }
    user = MSTP_Port->UserData;
    if (!user) {
        DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_NO_USER;
        return 0;
    }
    if (!npdu_data || !pdu || (pdu_len == 0)) {
        DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_OTHER;
        return 0;
    }
    if (!dlmstp_destination_valid(dest)) {
        DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_INVALID_DESTINATION;
        return 0;
    }
    if (pdu_len > DLMSTP_MPDU_MAX) {
        DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_PDU_TOO_LARGE;
        return 0;
    }

    if (npdu_data->data_expecting_reply) {
        frame_type = FRAME_TYPE_BACNET_DATA_EXPECTING_REPLY;
    } else {
        frame_type = FRAME_TYPE_BACNET_DATA_NOT_EXPECTING_REPLY;
    }
    if (dest && dest->mac_len) {
        destination_mac = dest->mac[0];
    }
    tx_source = dlmstp_classify_tx_source(pdu, pdu_len);
    queue_capacity = Ringbuf_Size(&user->PDU_Queue);
    master_state = MSTP_Port->master_state;

    if ((master_state == MSTP_MASTER_STATE_ANSWER_DATA_REQUEST) &&
        (tx_source == DLMSTP_TX_SOURCE_FINAL_ACK) &&
        (destination_mac != MSTP_BROADCAST_ADDRESS)) {
        responder_match = npdu_is_data_expecting_reply(
            &MSTP_Port->InputBuffer[0], MSTP_Port->DataLength,
            MSTP_Port->SourceAddress, pdu, pdu_len, destination_mac);
        if (responder_match) {
            immediate_frame_len = MSTP_Create_Frame(
                &MSTP_Port->OutputBuffer[0], MSTP_Port->OutputBufferSize,
                frame_type, destination_mac, MSTP_Port->This_Station, pdu,
                (uint16_t)pdu_len);
            if (immediate_frame_len > 0) {
                MSTP_Send_Frame(
                    MSTP_Port, &MSTP_Port->OutputBuffer[0],
                    immediate_frame_len);
                user->Statistics.transmit_pdu_counter++;
                MSTP_Port->master_state = MSTP_MASTER_STATE_IDLE;
                MSTP_Port->ReceivedValidFrame = false;
                DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_OK;
                debug_printf(
                    "dlmstp send_path=immediate_responder frame=%u dst=%u pdu_len=%u master_state=%s\n",
                    (unsigned)frame_type, (unsigned)destination_mac,
                    (unsigned)pdu_len,
                    dlmstp_master_state_text_short(master_state));

                return (int)pdu_len;
            }
            DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_OTHER;

            return 0;
        }
    }

    pkt = (struct dlmstp_packet *)(void *)Ringbuf_Data_Peek(&user->PDU_Queue);
    debug_printf(
        "dlmstp send_path=queued frame=%u dst=%u pdu_len=%u master_state=%s\n",
        (unsigned)frame_type, (unsigned)destination_mac, (unsigned)pdu_len,
        dlmstp_master_state_text_short(master_state));
    if (pkt) {
        pkt->frame_type = frame_type;
        pkt->source_tag = (uint8_t)tx_source;
        pkt->queued_ms = mstimer_now();
        for (i = 0; i < pdu_len; i++) {
            pkt->pdu[i] = pdu[i];
        }
        pkt->pdu_len = pdu_len;
        if (dest && dest->mac_len) {
            pkt->address.mac_len = 1;
            pkt->address.mac[0] = dest->mac[0];
            pkt->address.len = 0;
        } else {
            pkt->address.mac_len = 1;
            pkt->address.mac[0] = MSTP_BROADCAST_ADDRESS;
            pkt->address.len = 0;
        }
        if (Ringbuf_Data_Put(&user->PDU_Queue, (uint8_t *)pkt)) {
            bytes_sent = pdu_len;
            DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_OK;
        } else {
            DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_QUEUE_FULL;
            oldest_pkt = dlmstp_oldest_packet(user, &oldest_age_ms);
            (void)frame_type;
            (void)destination_mac;
            (void)pdu_len;
            (void)tx_source;
            (void)oldest_age_ms;
            (void)oldest_pkt;
        }
    } else {
        DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_QUEUE_FULL;
        oldest_pkt = dlmstp_oldest_packet(user, &oldest_age_ms);
        (void)queue_capacity;
        (void)frame_type;
        (void)destination_mac;
        (void)pdu_len;
        (void)tx_source;
        (void)oldest_age_ms;
        (void)oldest_pkt;
    }

    return bytes_sent;
}

DLMSTP_SEND_STATUS dlmstp_send_status_last(void)
{
    return DLMSTP_Last_Send_Status;
}

const char *dlmstp_send_status_text(DLMSTP_SEND_STATUS status)
{
    switch (status) {
        case DLMSTP_SEND_STATUS_OK:
            return "ok";
        case DLMSTP_SEND_STATUS_NO_PORT:
            return "other:no-port";
        case DLMSTP_SEND_STATUS_NO_USER:
            return "other:no-user";
        case DLMSTP_SEND_STATUS_INVALID_DESTINATION:
            return "invalid destination";
        case DLMSTP_SEND_STATUS_PDU_TOO_LARGE:
            return "other:pdu-too-large";
        case DLMSTP_SEND_STATUS_QUEUE_FULL:
            return "queue full (PDU_Queue)";
        case DLMSTP_SEND_STATUS_NOT_ALLOWED_STATE:
            return "not in allowed MS/TP state";
        case DLMSTP_SEND_STATUS_OTHER:
        default:
            break;
    }

    return "other";
}

const char *dlmstp_tx_source_text(DLMSTP_TX_SOURCE source)
{
    switch (source) {
        case DLMSTP_TX_SOURCE_I_AM:
            return "I-Am";
        case DLMSTP_TX_SOURCE_FINAL_ACK:
            return "final-ACK";
        case DLMSTP_TX_SOURCE_COV:
            return "COV";
        case DLMSTP_TX_SOURCE_OTHER:
        default:
            break;
    }

    return "other";
}

const char *dlmstp_master_state_text_current(void)
{
    if (!MSTP_Port) {
        return "NO_PORT";
    }

    return dlmstp_master_state_text_short(MSTP_Port->master_state);
}

bool dlmstp_send_reply_postponed(uint8_t destination_mac)
{
    struct dlmstp_user_data_t *user = NULL;

    if (!MSTP_Port) {
        DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_NO_PORT;
        return false;
    }
    user = MSTP_Port->UserData;
    if (!user) {
        DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_NO_USER;
        return false;
    }
    if ((destination_mac > DEFAULT_MAX_MASTER) &&
        (destination_mac != MSTP_BROADCAST_ADDRESS)) {
        DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_INVALID_DESTINATION;
        return false;
    }
    if ((MSTP_Port->master_state != MSTP_MASTER_STATE_ANSWER_DATA_REQUEST) ||
        (MSTP_Port->SourceAddress != destination_mac)) {
        DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_NOT_ALLOWED_STATE;
        return false;
    }

    MSTP_Create_And_Send_Frame(
        MSTP_Port, FRAME_TYPE_REPLY_POSTPONED, destination_mac,
        MSTP_Port->This_Station, NULL, 0);
    /* Keep ANSWER_DATA_REQUEST context so the matching final ACK can
       still be transmitted by the normal reply path when ready. */
    DLMSTP_Last_Send_Status = DLMSTP_SEND_STATUS_OK;

    return true;
}

/**
 * @brief The MS/TP state machine uses this function for getting data to send
 * @param mstp_port - specific MSTP port that is used for this datalink
 * @param timeout - number of milliseconds to wait for the data
 * @return amount of PDU data
 */
uint16_t MSTP_Get_Send(struct mstp_port_struct_t *mstp_port, unsigned timeout)
{
    uint16_t pdu_len = 0;
    struct dlmstp_packet *pkt;
    struct dlmstp_user_data_t *user;

    (void)timeout;
    if (!mstp_port) {
        return 0;
    }
    user = (struct dlmstp_user_data_t *)mstp_port->UserData;
    if (!user) {
        return 0;
    }
    if (Ringbuf_Empty(&user->PDU_Queue)) {
        return 0;
    }
    /* look at next PDU in queue without removing it */
    pkt = (struct dlmstp_packet *)(void *)Ringbuf_Peek(&user->PDU_Queue);
    /* convert the PDU into the MSTP Frame */
    pdu_len = MSTP_Create_Frame(
        &mstp_port->OutputBuffer[0], mstp_port->OutputBufferSize,
        pkt->frame_type, pkt->address.mac[0], mstp_port->This_Station,
        &pkt->pdu[0], pkt->pdu_len);
    user->Statistics.transmit_pdu_counter++;
    (void)Ringbuf_Pop(&user->PDU_Queue, NULL);

    return pdu_len;
}

/**
 * @brief The MS/TP state machine uses this function for getting data to send
 *  as the reply to a DATA_EXPECTING_REPLY frame, or nothing
 * @param mstp_port MSTP port structure for this port
 * @param timeout number of milliseconds to wait for a packet
 * @return number of bytes, or 0 if no reply is available
 */
uint16_t MSTP_Get_Reply(struct mstp_port_struct_t *mstp_port, unsigned timeout)
{
    uint16_t pdu_len = 0;
    bool matched = false;
    struct dlmstp_user_data_t *user = NULL;
    struct dlmstp_packet *pkt;

    (void)timeout;
    if (!mstp_port) {
        return 0;
    }
    user = mstp_port->UserData;
    if (!user) {
        return 0;
    }
    if (Ringbuf_Empty(&user->PDU_Queue)) {
        return 0;
    }
    /* look at next PDU in queue without removing it */
    pkt = (struct dlmstp_packet *)(void *)Ringbuf_Peek(&user->PDU_Queue);
    /* is this the reply to the DER? */
    matched = npdu_is_data_expecting_reply(
        &mstp_port->InputBuffer[0], mstp_port->DataLength,
        mstp_port->SourceAddress, &pkt->pdu[0], pkt->pdu_len,
        pkt->address.mac[0]);
    if (!matched) {
        return 0;
    }
    if (npdu_is_segmented_complex_ack_reply(pkt->pdu, pkt->pdu_len)) {
        /* In Clause 5.4.5.3, AWAIT_RESPONSE, in the transition
            SendSegmentedComplexACK, the text "transmit a BACnet-
            ComplexACK-PDU..." shall be replaced by "direct the
            MS/TP data link to transmit a Reply Postponed frame;
            transmit a BACnet-ComplexACK-PDU...."
            It is necessary to postpone the reply because
            transmission of the segmented ComplexACK
            cannot begin until the node holds the token.*/
        return MSTP_Create_Frame(
            &mstp_port->OutputBuffer[0], mstp_port->OutputBufferSize,
            FRAME_TYPE_REPLY_POSTPONED, mstp_port->SourceAddress,
            mstp_port->This_Station, NULL, 0);
    }
    /* convert the PDU into the MSTP Frame */
    pdu_len = MSTP_Create_Frame(
        &mstp_port->OutputBuffer[0], mstp_port->OutputBufferSize,
        pkt->frame_type, pkt->address.mac[0], mstp_port->This_Station,
        &pkt->pdu[0], pkt->pdu_len);
    user->Statistics.transmit_pdu_counter++;
    (void)Ringbuf_Pop(&user->PDU_Queue, NULL);

    return pdu_len;
}

/**
 * @brief MS/TP state machine callback to use for sending a frame
 * @param mstp_port - specific MSTP port that is used for this datalink
 * @param buffer - buffer to send
 * @param nbytes - number of bytes of data to send
 */
void MSTP_Send_Frame(
    struct mstp_port_struct_t *mstp_port,
    const uint8_t *buffer,
    uint16_t nbytes)
{
    struct dlmstp_user_data_t *user;
    struct dlmstp_rs485_driver *driver;

    if (!mstp_port) {
        return;
    }
    user = mstp_port->UserData;
    if (!user) {
        return;
    }
    driver = user->RS485_Driver;
    if (!driver) {
        return;
    }
    driver->send(buffer, nbytes);
    user->Statistics.transmit_frame_counter++;
}

/**
 * @brief MS/TP state machine received a frame
 * @return number of bytes queued, or 0 if unable to be queued
 */
uint16_t MSTP_Put_Receive(struct mstp_port_struct_t *mstp_port)
{
    struct dlmstp_user_data_t *user = NULL;

    if (!mstp_port) {
        return 0;
    }
    user = mstp_port->UserData;
    if (!user) {
        return 0;
    }
    user->ReceivePacketPending = true;

    return mstp_port->DataLength;
}

/**
 * @brief Run the MS/TP state machines, and get packet if available
 * @param pdu - place to put PDU data for the caller
 * @param max_pdu - number of bytes of PDU data that caller can receive
 * @return number of bytes in received packet, or 0 if no packet was received
 * @note Must be called at least once every 1 milliseconds, with no more than
 *  5 milliseconds jitter.
 */
uint16_t dlmstp_receive(
    BACNET_ADDRESS *src, uint8_t *pdu, uint16_t max_pdu, unsigned timeout)
{
    uint16_t pdu_len = 0;
    uint8_t data_register = 0;
    struct dlmstp_user_data_t *user;
    struct dlmstp_rs485_driver *driver;
    uint16_t i;
    uint32_t milliseconds;
    MSTP_MASTER_STATE master_state;

    (void)timeout;
    if (!MSTP_Port) {
        return 0;
    }
    user = MSTP_Port->UserData;
    if (!user) {
        return 0;
    }
    driver = user->RS485_Driver;
    if (!driver) {
        return 0;
    }
    while (!MSTP_Port->InputBuffer) {
        /* FIXME: develop configure an input buffer! */
    }
    if (driver->transmitting()) {
        /* we're transmitting; do nothing else */
        return 0;
    }
    /* only do receive state machine while we don't have a frame */
    while ((MSTP_Port->ReceivedValidFrame == false) &&
           (MSTP_Port->ReceivedInvalidFrame == false) &&
           (MSTP_Port->ReceivedValidFrameNotForUs == false)) {
        MSTP_Port->DataAvailable = driver->read(&data_register);
        if (MSTP_Port->DataAvailable) {
            MSTP_Port->DataRegister = data_register;
        }
        MSTP_Receive_Frame_FSM(MSTP_Port);
        if (MSTP_Port->receive_state == MSTP_RECEIVE_STATE_PREAMBLE) {
            if (user->Preamble_Callback) {
                user->Preamble_Callback();
            }
        }
        /* process another byte, if available */
        if (!driver->read(NULL)) {
            break;
        }
    }
    if (MSTP_Port->ReceivedValidFrame || MSTP_Port->ReceivedInvalidFrame ||
        MSTP_Port->ReceivedValidFrameNotForUs) {
        /* delay after reception before transmitting - per MS/TP spec */
        milliseconds = MSTP_Port->SilenceTimer(MSTP_Port);
        if (milliseconds < MSTP_Port->Tturnaround_timeout) {
            /* we're waiting; do nothing else */
            return 0;
        }
    }
    if (MSTP_Port->ReceivedValidFrame) {
        user->Statistics.receive_valid_frame_counter++;
        if (MSTP_Port->FrameType == FRAME_TYPE_POLL_FOR_MASTER) {
            user->Statistics.poll_for_master_counter++;
        }
        if (user->Valid_Frame_Rx_Callback) {
            user->Valid_Frame_Rx_Callback(
                MSTP_Port->SourceAddress, MSTP_Port->DestinationAddress,
                MSTP_Port->FrameType, MSTP_Port->InputBuffer,
                MSTP_Port->DataLength);
        }
    }
    if (MSTP_Port->ReceivedValidFrameNotForUs) {
        user->Statistics.receive_valid_frame_not_for_us_counter++;
        if (user->Valid_Frame_Not_For_Us_Rx_Callback) {
            user->Valid_Frame_Not_For_Us_Rx_Callback(
                MSTP_Port->SourceAddress, MSTP_Port->DestinationAddress,
                MSTP_Port->FrameType, MSTP_Port->InputBuffer,
                MSTP_Port->DataLength);
        }
    }
    if (MSTP_Port->ReceivedInvalidFrame) {
        user->Statistics.receive_invalid_frame_counter++;
        if (MSTP_Port->HeaderCRC != 0x55) {
            user->Statistics.bad_crc_counter++;
        } else if (MSTP_Port->DataCRC != 0xF0B8) {
            user->Statistics.bad_crc_counter++;
        }
        if (user->Invalid_Frame_Rx_Callback) {
            user->Invalid_Frame_Rx_Callback(
                MSTP_Port->SourceAddress, MSTP_Port->DestinationAddress,
                MSTP_Port->FrameType, MSTP_Port->InputBuffer,
                MSTP_Port->DataLength);
        }
    }
    if (MSTP_Port->receive_state == MSTP_RECEIVE_STATE_IDLE) {
        /* only node state machines while rx is idle */
        if (MSTP_Port->SlaveNodeEnabled) {
            MSTP_Slave_Node_FSM(MSTP_Port);
        } else if (
            (MSTP_Port->This_Station <= DEFAULT_MAX_MASTER) ||
            MSTP_Port->ZeroConfigEnabled || MSTP_Port->CheckAutoBaud) {
            master_state = MSTP_Port->master_state;
            while (MSTP_Master_Node_FSM(MSTP_Port)) {
                if (master_state != MSTP_Port->master_state) {
                    /* state changed while some states fast transition */
                    if (MSTP_Port->master_state == MSTP_MASTER_STATE_NO_TOKEN) {
                        user->Statistics.lost_token_counter++;
                    }
                    master_state = MSTP_Port->master_state;
                }
            };
        }
    }
    /* see if there is a packet available */
    if (user->ReceivePacketPending) {
        user->ReceivePacketPending = false;
        user->Statistics.receive_pdu_counter++;
        pdu_len = MSTP_Port->DataLength;
        if (pdu_len > max_pdu) {
            /* PDU is too large */
            return 0;
        }
        if (!pdu) {
            /* no place to put a PDU */
            return 0;
        }
        /* copy input buffer to PDU */
        for (i = 0; i < pdu_len; i++) {
            pdu[i] = MSTP_Port->InputBuffer[i];
        }
        if (!src) {
            /* no place to put a source address */
            return 0;
        }
        /* copy source address */
        src->len = 0;
        src->net = 0;
        src->mac_len = 1;
        src->mac[0] = MSTP_Port->SourceAddress;
    }

    return pdu_len;
}

/**
 * @brief fill a BACNET_ADDRESS with the MSTP MAC address
 * @param src - a #BACNET_ADDRESS structure
 * @param mstp_address - a BACnet MSTP address
 */
void dlmstp_fill_bacnet_address(BACNET_ADDRESS *src, uint8_t mstp_address)
{
    int i = 0;

    if (mstp_address == MSTP_BROADCAST_ADDRESS) {
        /* mac_len = 0 if broadcast address */
        src->mac_len = 0;
        src->mac[0] = 0;
    } else {
        src->mac_len = 1;
        src->mac[0] = mstp_address;
    }
    /* fill with 0's starting with index 1; index 0 filled above */
    for (i = 1; i < MAX_MAC_LEN; i++) {
        src->mac[i] = 0;
    }
    src->net = 0;
    src->len = 0;
    for (i = 0; i < MAX_MAC_LEN; i++) {
        src->adr[i] = 0;
    }
}

/**
 * @brief Set the MSTP MAC address
 * @param mac_address - MAC address to set
 */
void dlmstp_set_mac_address(uint8_t mac_address)
{
    if (MSTP_Port) {
        MSTP_Port->This_Station = mac_address;
    }

    return;
}

/**
 * @brief Get the MSTP MAC address
 * @return MSTP MAC address
 */
uint8_t dlmstp_mac_address(void)
{
    uint8_t value = 0;

    if (MSTP_Port) {
        value = MSTP_Port->This_Station;
    }

    return value;
}

/**
 * @brief Set the Max_Info_Frames parameter value
 *
 * @note This parameter represents the value of the Max_Info_Frames property
 *  of the node's Device object. The value of Max_Info_Frames specifies the
 *  maximum number of information frames the node may send before it must
 *  pass the token. Max_Info_Frames may have different values on different
 *  nodes. This may be used to allocate more or less of the available link
 *  bandwidth to particular nodes. If Max_Info_Frames is not writable in a
 *  node, its value shall be 1.
 *
 * @param max_info_frames - parameter value to set
 */
void dlmstp_set_max_info_frames(uint8_t max_info_frames)
{
    if (max_info_frames >= 1) {
        if (MSTP_Port) {
            MSTP_Port->Nmax_info_frames = max_info_frames;
        }
    }

    return;
}

/**
 * @brief Get the MSTP max-info-frames value
 * @return the MSTP max-info-frames value
 */
uint8_t dlmstp_max_info_frames(void)
{
    uint8_t value = 0;

    if (MSTP_Port) {
        value = MSTP_Port->Nmax_info_frames;
    }

    return value;
}

/**
 * @brief Set the Max_Master property value for this MSTP datalink
 *
 * @note This parameter represents the value of the Max_Master property of
 *  the node's Device object. The value of Max_Master specifies the highest
 *  allowable address for master nodes. The value of Max_Master shall be
 *  less than or equal to 127. If Max_Master is not writable in a node,
 *  its value shall be 127.
 *
 * @param max_master - value to be set
 */
void dlmstp_set_max_master(uint8_t max_master)
{
    if (max_master <= 127) {
        MSTP_Port->Nmax_master = max_master;
    }

    return;
}

/**
 * @brief Get the largest peer MAC address that we will seek
 * @return largest peer MAC address
 */
uint8_t dlmstp_max_master(void)
{
    uint8_t value = 0;

    if (MSTP_Port) {
        value = MSTP_Port->Nmax_master;
    }

    return value;
}

/**
 * @brief Initialize the data link broadcast address
 * @param my_address - address to be filled with unicast designator
 */
void dlmstp_get_my_address(BACNET_ADDRESS *my_address)
{
    int i = 0; /* counter */

    my_address->mac_len = 1;
    if (MSTP_Port) {
        my_address->mac[0] = MSTP_Port->This_Station;
    }
    my_address->net = 0; /* local only, no routing */
    my_address->len = 0;
    for (i = 0; i < MAX_MAC_LEN; i++) {
        my_address->adr[i] = 0;
    }

    return;
}

/**
 * @brief Initialize the a data link broadcast address
 * @param dest - address to be filled with broadcast designator
 */
void dlmstp_get_broadcast_address(BACNET_ADDRESS *dest)
{ /* destination address */
    int i = 0; /* counter */

    if (dest) {
        dest->mac_len = 1;
        dest->mac[0] = MSTP_BROADCAST_ADDRESS;
        dest->net = BACNET_BROADCAST_NETWORK;
        dest->len = 0; /* always zero when DNET is broadcast */
        for (i = 0; i < MAX_MAC_LEN; i++) {
            dest->adr[i] = 0;
        }
    }

    return;
}

/**
 * @brief Get the MSTP port SoleMaster status
 * @return true if the MSTP port is the SoleMaster
 */
bool dlmstp_sole_master(void)
{
    if (!MSTP_Port) {
        return false;
    }
    return MSTP_Port->SoleMaster;
}

/**
 * @brief Get the MSTP port SlaveNodeEnabled status
 * @return true if the MSTP port has SlaveNodeEnabled
 */
bool dlmstp_slave_mode_enabled(void)
{
    if (!MSTP_Port) {
        return false;
    }
    return MSTP_Port->SlaveNodeEnabled;
}

/**
 * @brief Set the MSTP port SlaveNodeEnabled flag
 * @param flag - true if the MSTP port has SlaveNodeEnabled
 * @return true if the MSTP port SlaveNodeEnabled was set
 * @note This flag is used to enable the Slave Node state machine
 * for the MSTP port.  The Slave Node state machine is used to
 * respond to requests from the Master Node.
 */
bool dlmstp_slave_mode_enabled_set(bool flag)
{
    if (!MSTP_Port) {
        return false;
    }
    MSTP_Port->SlaveNodeEnabled = flag;

    return true;
}

/**
 * @brief Get the MSTP port ZeroConfigEnabled status
 * @return true if the MSTP port has ZeroConfigEnabled
 */
bool dlmstp_zero_config_enabled(void)
{
    if (!MSTP_Port) {
        return false;
    }
    return MSTP_Port->ZeroConfigEnabled;
}

/**
 * @brief Set the MSTP port ZeroConfigEnabled flag
 * @param flag - true if the MSTP port has ZeroConfigEnabled
 * @return true if the MSTP port ZeroConfigEnabled was set
 * @note This flag is used to enable the Zero Configuration state machine
 * for the MSTP port.  The Zero Configuration state machine is used to
 * automatically assign a MAC address to the MSTP port.
 */
bool dlmstp_zero_config_enabled_set(bool flag)
{
    if (!MSTP_Port) {
        return false;
    }
    MSTP_Port->ZeroConfigEnabled = flag;

    return true;
}

/**
 * @brief Get the MSTP port AutoBaudEnabled status
 * @return true if the MSTP port has AutoBaudEnabled
 */
bool dlmstp_check_auto_baud(void)
{
    if (!MSTP_Port) {
        return false;
    }
    return MSTP_Port->CheckAutoBaud;
}

/**
 * @brief Set the MSTP port AutoBaudEnabled flag
 * @param flag - true if the MSTP port has AutoBaudEnabled
 * @return true if the MSTP port AutoBaudEnabled was set
 * @note This flag is used to enable the Zero Configuration state machine
 * for the MSTP port.  The Zero Configuration state machine is used to
 * automatically assign a MAC address to the MSTP port.
 */
bool dlmstp_check_auto_baud_set(bool flag)
{
    if (!MSTP_Port) {
        return false;
    }
    MSTP_Port->CheckAutoBaud = flag;
    if (flag) {
        MSTP_Port->Auto_Baud_State = MSTP_AUTO_BAUD_STATE_INIT;
    }

    return true;
}

/**
 * @brief Get the MSTP port MAC address that this node prefers to use.
 * @return ZeroConfigStation value, or an out-of-range value if invalid
 * @note valid values are between Nmin_poll_station and Nmax_poll_station
 *  but other values such as 0 or 255 could mean 'unconfigured'
 */
uint8_t dlmstp_zero_config_preferred_station(void)
{
    if (!MSTP_Port) {
        return 0;
    }
    return MSTP_Port->Zero_Config_Preferred_Station;
}

/**
 * @brief Set the MSTP port MAC address that this node prefers to use.
 * @param station - Zero_Config_Preferred_Station value
 * @return true if the MSTP port Zero_Config_Preferred_Station was set
 * @note valid values are between Nmin_poll_station and Nmax_poll_station
 *  but other values such as 0 or 255 could mean 'unconfigured'
 */
bool dlmstp_zero_config_preferred_station_set(uint8_t station)
{
    if (!MSTP_Port) {
        return false;
    }
    MSTP_Port->Zero_Config_Preferred_Station = station;

    return true;
}

/**
 * @brief Determine if the send PDU queue is empty
 * @return true if the send PDU is empty
 */
bool dlmstp_send_pdu_queue_empty(void)
{
    bool status = false;
    struct dlmstp_user_data_t *user;

    if (MSTP_Port) {
        user = MSTP_Port->UserData;
        if (user) {
            status = Ringbuf_Empty(&user->PDU_Queue);
        }
    }

    return status;
}

/**
 * @brief Determine if the send PDU queue is full
 * @return true if the send PDU is full
 */
bool dlmstp_send_pdu_queue_full(void)
{
    bool status = false;
    struct dlmstp_user_data_t *user;

    if (MSTP_Port) {
        user = MSTP_Port->UserData;
        if (user) {
            status = Ringbuf_Full(&user->PDU_Queue);
        }
    }

    return status;
}

unsigned dlmstp_send_pdu_queue_depth(void)
{
    unsigned depth = 0;
    struct dlmstp_user_data_t *user;

    if (MSTP_Port) {
        user = MSTP_Port->UserData;
        if (user) {
            depth = Ringbuf_Count(&user->PDU_Queue);
        }
    }

    return depth;
}

bool dlmstp_send_pdu_queue_drop_source(DLMSTP_TX_SOURCE source)
{
    bool dropped = false;
    struct dlmstp_user_data_t *user;
    volatile uint8_t *elem = NULL;
    volatile uint8_t *next = NULL;
    struct dlmstp_packet *pkt = NULL;

    if (!MSTP_Port) {
        return false;
    }
    user = MSTP_Port->UserData;
    if (!user) {
        return false;
    }

    elem = Ringbuf_Peek(&user->PDU_Queue);
    while (elem) {
        next = Ringbuf_Peek_Next(&user->PDU_Queue, (const uint8_t *)elem);
        pkt = (struct dlmstp_packet *)(void *)elem;
        if (pkt->source_tag == (uint8_t)source) {
            if (Ringbuf_Pop_Element(&user->PDU_Queue, (const uint8_t *)elem, NULL)) {
                dropped = true;
            }
        }
        elem = next;
    }

    return dropped;
}

/**
 * @brief Initialize the RS-485 baud rate
 * @param baudrate - RS-485 baud rate in bits per second (bps)
 * @return true if the baud rate was valid
 */
void dlmstp_set_baud_rate(uint32_t baud)
{
    struct dlmstp_user_data_t *user;
    struct dlmstp_rs485_driver *driver;

    if (!MSTP_Port) {
        return;
    }
    user = MSTP_Port->UserData;
    if (!user) {
        return;
    }
    driver = user->RS485_Driver;
    if (!driver) {
        return;
    }
    if (driver->baud_rate_set(baud)) {
        /* Tframe_abort=60 bit times, not to exceed 100 milliseconds.*/
        if (MSTP_Port->Tframe_abort <= 7) {
            /* within baud range, so auto-calculate range based on baud */
            MSTP_Port->Tframe_abort = 1 + ((60 * 1000UL) / baud);
        }
        /* Tturnaround=40 bit times */
        MSTP_Port->Tturnaround_timeout = 1 + ((Tturnaround * 1000) / baud);
    }
}

/**
 * @brief Return the RS-485 baud rate
 * @return baud - RS-485 baud rate in bits per second (bps)
 */
uint32_t dlmstp_baud_rate(void)
{
    struct dlmstp_user_data_t *user;
    struct dlmstp_rs485_driver *driver;

    if (!MSTP_Port) {
        return 0;
    }
    user = MSTP_Port->UserData;
    if (!user) {
        return 0;
    }
    driver = user->RS485_Driver;
    if (!driver) {
        return 0;
    }

    return driver->baud_rate();
}

/**
 * @brief Set the MS/TP Frame Complete callback
 * @param cb_func - callback function to be called when a frame is received
 */
void dlmstp_set_frame_rx_complete_callback(
    dlmstp_hook_frame_rx_complete_cb cb_func)
{
    struct dlmstp_user_data_t *user;

    if (!MSTP_Port) {
        return;
    }
    user = MSTP_Port->UserData;
    if (!user) {
        return;
    }
    user->Valid_Frame_Rx_Callback = cb_func;
}

/**
 * @brief Set the MS/TP Frame Complete callback
 * @param cb_func - callback function to be called when a frame is received
 */
void dlmstp_set_frame_not_for_us_rx_complete_callback(
    dlmstp_hook_frame_rx_complete_cb cb_func)
{
    struct dlmstp_user_data_t *user;

    if (!MSTP_Port) {
        return;
    }
    user = MSTP_Port->UserData;
    if (!user) {
        return;
    }
    user->Valid_Frame_Not_For_Us_Rx_Callback = cb_func;
}

/**
 * @brief Set the MS/TP Frame Complete callback
 * @param cb_func - callback function to be called when a frame is received
 */
void dlmstp_set_invalid_frame_rx_complete_callback(
    dlmstp_hook_frame_rx_complete_cb cb_func)
{
    struct dlmstp_user_data_t *user;

    if (!MSTP_Port) {
        return;
    }
    user = MSTP_Port->UserData;
    if (!user) {
        return;
    }
    user->Invalid_Frame_Rx_Callback = cb_func;
}

/**
 * @brief Set the MS/TP Preamble callback
 * @param cb_func - callback function to be called when a preamble is received
 */
void dlmstp_set_frame_rx_start_callback(dlmstp_hook_frame_rx_start_cb cb_func)
{
    struct dlmstp_user_data_t *user;

    if (!MSTP_Port) {
        return;
    }
    user = MSTP_Port->UserData;
    if (!user) {
        return;
    }
    user->Preamble_Callback = cb_func;
}

/**
 * @brief Reset the MS/TP statistics
 */
void dlmstp_reset_statistics(void)
{
    struct dlmstp_user_data_t *user;

    if (!MSTP_Port) {
        return;
    }
    user = MSTP_Port->UserData;
    if (!user) {
        return;
    }
    memset(&user->Statistics, 0, sizeof(struct dlmstp_statistics));
}

/**
 * @brief Copy the MSTP port statistics if they exist
 * @param statistics - MSTP port statistics
 */
void dlmstp_fill_statistics(struct dlmstp_statistics *statistics)
{
    struct dlmstp_user_data_t *user;

    if (!MSTP_Port) {
        return;
    }
    user = MSTP_Port->UserData;
    if (!user) {
        return;
    }
    if (statistics) {
        memmove(
            statistics, &user->Statistics, sizeof(struct dlmstp_statistics));
    }
}

/**
 * @brief Get the MSTP port Max-Info-Frames limit
 * @return Max-Info-Frames limit
 */
uint8_t dlmstp_max_info_frames_limit(void)
{
    return DLMSTP_MAX_INFO_FRAMES;
}

/**
 * @brief Get the MSTP port Max-Master limit
 * @return Max-Master limit
 */
uint8_t dlmstp_max_master_limit(void)
{
    return DLMSTP_MAX_MASTER;
}

/**
 * @brief Return the RS-485 silence time in milliseconds
 * @param arg - pointer to MSTP port structure
 * @return silence time in milliseconds
 */
uint32_t dlmstp_silence_milliseconds(void *arg)
{
    uint32_t milliseconds = 0;
    struct mstp_port_struct_t *port = arg;
    struct dlmstp_user_data_t *user = NULL;
    struct dlmstp_rs485_driver *driver = NULL;

    if (port) {
        user = port->UserData;
    }
    if (user) {
        driver = user->RS485_Driver;
    }
    if (driver) {
        milliseconds = driver->silence_milliseconds();
    }

    return milliseconds;
}

/**
 * @brief Return the valid frame time in milliseconds
 * @param arg - pointer to MSTP port structure
 * @return valid frame time in milliseconds
 */
uint32_t dlmstp_valid_frame_milliseconds(void *arg)
{
    uint32_t milliseconds = 0, now = 0;
    struct mstp_port_struct_t *port = arg;
    struct dlmstp_user_data_t *user = NULL;

    if (port) {
        user = port->UserData;
    }
    if (user) {
        now = mstimer_now();
        milliseconds = now - user->Valid_Frame_Milliseconds;
    }

    return milliseconds;
}

/**
 * @brief Reset the valid frame timer
 * @param arg - pointer to MSTP port structure
 * @return valid frame time in milliseconds
 */
void dlmstp_valid_frame_milliseconds_reset(void *arg)
{
    struct mstp_port_struct_t *port = arg;
    struct dlmstp_user_data_t *user = NULL;

    if (port) {
        user = port->UserData;
    }
    if (user) {
        user->Valid_Frame_Milliseconds = mstimer_now();
    }
}

/**
 * @brief Reset the RS-485 silence time to zero
 * @param arg - pointer to MSTP port structure
 */
void dlmstp_silence_reset(void *arg)
{
    struct mstp_port_struct_t *port = arg;
    struct dlmstp_user_data_t *user = NULL;
    struct dlmstp_rs485_driver *driver = NULL;

    if (port) {
        user = port->UserData;
    }
    if (user) {
        driver = user->RS485_Driver;
    }
    if (driver) {
        driver->silence_reset();
    }
}

/**
 * @brief set the MS/TP datalink interface
 * @param ifname - interface name to set
 */
void dlmstp_set_interface(const char *ifname)
{
    MSTP_Port = (struct mstp_port_struct_t *)ifname;
}

/**
 * @brief get the MS/TP datalink intferface name
 * @return interface name
 */
const char *dlmstp_get_interface(void)
{
    return (const char *)MSTP_Port;
}

/**
 * @brief Initialize this MS/TP datalink
 * @param ifname user data structure
 * @return true if the MSTP datalink is initialized
 */
bool dlmstp_init(char *ifname)
{
    bool status = false;
    struct dlmstp_user_data_t *user;

    if (ifname) {
        MSTP_Port = (struct mstp_port_struct_t *)ifname;
    }
    if (MSTP_Port) {
        MSTP_Port->SilenceTimer = dlmstp_silence_milliseconds;
        MSTP_Port->SilenceTimerReset = dlmstp_silence_reset;
        MSTP_Port->ValidFrameTimer = dlmstp_valid_frame_milliseconds;
        MSTP_Port->ValidFrameTimerReset = dlmstp_valid_frame_milliseconds_reset;
        MSTP_Port->BaudRate = dlmstp_baud_rate;
        MSTP_Port->BaudRateSet = dlmstp_set_baud_rate;
        user = (struct dlmstp_user_data_t *)MSTP_Port->UserData;
        if (user && !user->Initialized) {
            Ringbuf_Initialize(
                &user->PDU_Queue, (volatile uint8_t *)user->PDU_Buffer,
                sizeof(user->PDU_Buffer), sizeof(struct dlmstp_packet),
                DLMSTP_MAX_INFO_FRAMES);
            MSTP_Init(MSTP_Port);
#if defined(ESP_PLATFORM)
            ESP_LOGI(
                "mstp_pfm",
                "PFM_INSTRUMENTATION_ENABLED mac=%u",
                (unsigned)MSTP_Port->This_Station);
#endif
            user->Initialized = true;
        }
        status = true;
    }

    return status;
}