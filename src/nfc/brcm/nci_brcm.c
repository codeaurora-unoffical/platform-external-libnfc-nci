/*****************************************************************************
**
**  Name:          nci_brcm.c
**
**  Description:   This file contains function of the NFC unit to
**                 receive/process NCI VS commands.
**
**
**  Copyright (c) 2010-2011, Broadcom Corp., All Rights Reserved.
**  Broadcom Bluetooth Core. Proprietary and confidential.
**
******************************************************************************/
#include <string.h>
#include "nfc_target.h"
#include "bt_types.h"
#include "gki.h"
#include "trace_api.h"
#include "userial.h"
#include "upio.h"

#if NFC_INCLUDED == TRUE
#include "nci_defs.h"
#include "nci_hmsgs.h"
#include "nfc_api.h"
#include "nfc_int.h"
#include "nci_int.h"

#if (NFC_BRCM_VS_INCLUDED == TRUE)
#include "hcidefs.h"
#include "nfc_brcm_api.h"
#include "nfc_brcm_int.h"

/*****************************************************************************
** Constants and types
*****************************************************************************/

static UINT8 nci_brcm_core_reset_cmd[NCI_MSG_HDR_SIZE + NCI_CORE_PARAM_SIZE_RESET] = 
{ 
    NCI_MTS_CMD|NCI_GID_CORE, 
    NCI_MSG_CORE_RESET, 
    NCI_CORE_PARAM_SIZE_RESET, 
    NCI_RESET_TYPE_RESET_CFG 
};

static UINT8 nci_brcm_prop_build_info_cmd[NCI_MSG_HDR_SIZE] = 
{ 
    NCI_MTS_CMD|NCI_GID_PROP, 
    NCI_MSG_GET_BUILD_INFO, 
    0x00
};
#define NCI_BUILD_INFO_OFFSET_HWID  25  /* HW ID offset in build info RSP */

#define NCI_BRCM_OFFSET_BAUDRATE    5
static UINT8 nci_brcm_set_baudrate_cmd[BRCM_BT_HCI_CMD_HDR_SIZE + HCI_BRCM_UPDATE_BAUD_RATE_UNENCODED_LENGTH] =
{ 
    0x18,               /* HCI_BRCM_UPDATE_BAUDRATE_CMD */
    0xFC,               /* HCI_GRP_VENDOR_SPECIFIC      */
    HCI_BRCM_UPDATE_BAUD_RATE_UNENCODED_LENGTH, 

    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

#ifndef BRCM_NCI_DEBUG
#define BRCM_NCI_DEBUG  TRUE
#endif

#if (BRCM_NCI_DEBUG == TRUE)
#define NFC_BRCM_NCI_STATE(sta)  NCI_TRACE_DEBUG2 ("init sta: %d->%d", nci_brcm_cb.initializing_state, sta); nci_brcm_cb.initializing_state = sta;
#else
#define NFC_BRCM_NCI_STATE(sta)  nci_brcm_cb.initializing_state = sta;
#endif

/*****************************************************************************
** Local function prototypes
*****************************************************************************/

#if (BT_TRACE_VERBOSE == TRUE)
static char *nci_brcm_evt_2_str (UINT16 event);
#endif

tNCI_BRCM_CB nci_brcm_cb;

extern void nfc_brcm_prm_nci_command_complete_cback(tNFC_VS_EVT event, UINT16 data_len, UINT8 *p_data);
/*******************************************************************************
**
** Function         nci_brcm_set_local_baud_rate
**
** Description      Set baud rate on local device
**
** Returns          void
**
*******************************************************************************/
void nci_brcm_set_local_baud_rate (UINT8 baud)
{
    tUSERIAL_IOCTL_DATA data;

    NCI_TRACE_DEBUG1 ("nci_brcm_set_local_baud_rate (): baud = %d", baud);

    data.baud = baud;
    USERIAL_Ioctl (USERIAL_NFC_PORT, USERIAL_OP_BAUD_WR, &data);
}

/*****************************************************************************
**
** Function         nci_brcm_receive_msg
**
** Description
**      Handle incoming BRCM specific data from the serial port.
**
**      If there is data waiting from the serial port, this funciton reads the
**      data and parses it. Once an entire message has been read, it returns
**      TRUE.
**
*****************************************************************************/
static BOOLEAN nci_brcm_receive_msg (tNCI_BRCM_CB *p_cb, UINT8 byte)
{
    UINT16  len;
    BOOLEAN msg_received = FALSE;

    switch (p_cb->rcv_state)
    {
    case NCI_BRCM_RCV_IDLE_ST:

        /* Initialize rx parameters */
        p_cb->rcv_state = NCI_BRCM_RCV_HCI_HDR_ST;
        p_cb->rcv_len   = HCIE_PREAMBLE_SIZE;

        if ((p_cb->p_rcv_msg = (BT_HDR *) GKI_getpoolbuf (NFC_NCI_POOL_ID)) != NULL)
        {
            /* Initialize BT_HDR */
            p_cb->p_rcv_msg->len    = 0;
            p_cb->p_rcv_msg->event  = BT_EVT_TO_NFC_NCI_VS;
            p_cb->p_rcv_msg->offset = NFC_RECEIVE_MSGS_OFFSET;

            *((UINT8 *)(p_cb->p_rcv_msg + 1) + p_cb->p_rcv_msg->offset + p_cb->p_rcv_msg->len++) = byte;
        }
        else
        {
            NCI_TRACE_ERROR0( "[nfc] Unable to allocate buffer for incoming NCI message."); 
        }
        p_cb->rcv_len--;
        break;

    case NCI_BRCM_RCV_HCI_HDR_ST:
        if (p_cb->p_rcv_msg)
        {
            *((UINT8 *)(p_cb->p_rcv_msg + 1) + p_cb->p_rcv_msg->offset + p_cb->p_rcv_msg->len++) = byte;
        }
        p_cb->rcv_len--;

        /* Check if we received entire preamble yet */
        if (p_cb->rcv_len == 0)
        {
            /* Received entire preamble. Length is in the last byte(s) of the preamble */
            p_cb->rcv_len = byte;

            /* Verify that buffer is big enough to fit message */
            if ((sizeof(BT_HDR) + HCIE_PREAMBLE_SIZE + byte) > GKI_get_buf_size(p_cb->p_rcv_msg))
            {
                /* Message cannot fit into buffer */
                GKI_freebuf(p_cb->p_rcv_msg);
                p_cb->p_rcv_msg     = NULL;

                NCI_TRACE_ERROR0("HCIS: Invalid length for incoming HCI message.");
            }

            /* Message length is valid */
            if (byte)
            {
                /* Read rest of message */
                p_cb->rcv_state = NCI_BRCM_RCV_DATA_ST;
            }
            else
            {
                /* Message has no additional parameters. (Entire message has been received) */
                msg_received    = TRUE;
                p_cb->rcv_state = NCI_BRCM_RCV_IDLE_ST;  /* Next, wait for next message */
            }
        }
        break;

    case NCI_BRCM_RCV_DATA_ST:
        p_cb->rcv_len--;
        if (p_cb->p_rcv_msg)
        {
            *((UINT8 *)(p_cb->p_rcv_msg + 1) + p_cb->p_rcv_msg->offset + p_cb->p_rcv_msg->len++) = byte;

            if (p_cb->rcv_len > 0)
            {
                /* Read in the rest of the message */
                len = USERIAL_Read(USERIAL_NFC_PORT, ((UINT8 *)(p_cb->p_rcv_msg + 1) + p_cb->p_rcv_msg->offset + p_cb->p_rcv_msg->len),  p_cb->rcv_len);
                p_cb->p_rcv_msg->len    += len;
                p_cb->rcv_len           -= len;
            }
        }

        /* Check if we read in entire message yet */
        if (p_cb->rcv_len == 0)
        {
            msg_received        = TRUE;
            p_cb->rcv_state     = NCI_BRCM_RCV_IDLE_ST;      /* Next, wait for next message */
        }
        break;
    }

    /* If we received entire message */
    if (msg_received)
    {
        /* Display protocol trace message */
#if (BT_TRACE_PROTOCOL == TRUE)
        DispHciEvt (p_cb->p_rcv_msg);
#endif
    }

    return (msg_received);
}

/*******************************************************************************
**
** Function         nci_brcm_set_nfc_wake
**
** Description      Set NFC_WAKE line 
**
** Returns          void
**
*******************************************************************************/
void nci_brcm_set_nfc_wake (UINT8 cmd)
{
    NCI_TRACE_DEBUG1 ("nci_brcm_set_nfc_wake() %s", 
                      (cmd == NCI_ASSERT_NFC_WAKE ? "ASSERT" : "DEASSERT"));

    /*
    **  nfc_wake_active_mode             cmd              result of voltage on NFC_WAKE
    **
    **  NFC_LP_ACTIVE_LOW(0)    NCI_ASSERT_NFC_WAKE(0)    pull down NFC_WAKE (GND)
    **  NFC_LP_ACTIVE_LOW(0)    NCI_DEASSERT_NFC_WAKE(1)  pull up NFC_WAKE (VCC)
    **  NFC_LP_ACTIVE_HIGH(1)   NCI_ASSERT_NFC_WAKE(0)    pull up NFC_WAKE (VCC)
    **  NFC_LP_ACTIVE_HIGH(1)   NCI_DEASSERT_NFC_WAKE(1)  pull down NFC_WAKE (GND)
    */

    if (cmd == nci_brcm_cb.nfc_wake_active_mode)
        UPIO_Set (UPIO_GENERAL, NCILP_NFC_WAKE_GPIO, UPIO_OFF); /* pull down NFC_WAKE */
    else
        UPIO_Set (UPIO_GENERAL, NCILP_NFC_WAKE_GPIO, UPIO_ON);  /* pull up NFC_WAKE */
}

/*******************************************************************************
**
** Function         nci_brcm_power_mode_execute
**
** Description      If snooze mode is enabled in full power mode,
**                     Assert NFC_WAKE before sending data
**                     Deassert NFC_WAKE when idle timer expires
**
** Returns          TRUE if DH can send data to NFCC
**
*******************************************************************************/
static BOOLEAN nci_brcm_power_mode_execute (UINT8 event)
{
    BOOLEAN send_to_nfcc = FALSE;

    NCI_TRACE_DEBUG1 ("nci_brcm_power_mode_execute() event = %d", event);

    if (nci_brcm_cb.power_mode == NCI_BRCM_POWER_MODE_FULL)
    {
        if (nci_brcm_cb.snooze_mode_enabled)
        {
            /* if any transport activity */
            if (  (event == NCI_BRCM_LP_TX_DATA_EVT)
                ||(event == NCI_BRCM_LP_RX_DATA_EVT)  )
            {
                /* if idle timer is not running */
                if (nci_brcm_cb.lp_timer.in_use == FALSE)
                {
                    nci_brcm_set_nfc_wake (NCI_ASSERT_NFC_WAKE);
                }

                /* start or extend idle timer */
                nci_start_quick_timer (&nci_brcm_cb.lp_timer, 0x00, 
                                       ((UINT32)NCI_LP_IDLE_TIMEOUT)*QUICK_TIMER_TICKS_PER_SEC/1000);
            }
            else if (event == NCI_BRCM_LP_TIMEOUT_EVT)
            {
                /* let NFCC go to snooze mode */
                nci_brcm_set_nfc_wake (NCI_DEASSERT_NFC_WAKE);
            }
        }

        send_to_nfcc = TRUE;
    }
    else /* VS power mode */
    {
        if (nci_brcm_cb.p_vs_brcm_pwr_hdlr)
        {
            send_to_nfcc = (*nci_brcm_cb.p_vs_brcm_pwr_hdlr) (event);
        }
    }

    return (send_to_nfcc);
}

/*******************************************************************************
**
** Function         nci_brcm_enable_snooze_mode
**
** Description      NFA enabled snooze mode, start driving NFC_WAKE line 
**
** Returns          void
**
*******************************************************************************/
static void nci_brcm_enable_snooze_mode (UINT8 nfc_wake_active_mode)
{
    NCI_TRACE_DEBUG1 ("nci_brcm_enable_snooze_mode (): nfc_wake_active_mode=%d", nfc_wake_active_mode);

    nci_brcm_cb.snooze_mode_enabled  = TRUE;
    nci_brcm_cb.nfc_wake_active_mode = nfc_wake_active_mode;

    nci_brcm_set_nfc_wake (NCI_ASSERT_NFC_WAKE);

    /* start idle timer */
    nci_start_quick_timer (&nci_brcm_cb.lp_timer, 0x00, 
                           ((UINT32)NCI_LP_IDLE_TIMEOUT)*QUICK_TIMER_TICKS_PER_SEC/1000);
}

/*******************************************************************************
**
** Function         nci_brcm_disable_snooze_mode
**
** Description      NFA disabled snooze mode, stop driving NFC_WAKE line
**
** Returns          void
**
*******************************************************************************/
static void nci_brcm_disable_snooze_mode (void)
{
    NCI_TRACE_DEBUG0 ("nci_brcm_disable_snooze_mode ()");

    nci_brcm_cb.snooze_mode_enabled = FALSE;

    nci_brcm_set_nfc_wake (NCI_ASSERT_NFC_WAKE);
    nci_stop_quick_timer (&nci_brcm_cb.lp_timer);
}

/*******************************************************************************
**
** Function         nci_brcm_lp_timeout_cback
**
** Description      callback function for timeout
**
** Returns          void
**
*******************************************************************************/
static void nci_brcm_lp_timeout_cback (void *p_tle)
{
    NCI_TRACE_DEBUG0 ("nci_brcm_lp_timeout_cback ()");

    nci_brcm_power_mode_execute (NCI_BRCM_LP_TIMEOUT_EVT);
}

/*******************************************************************************
**
** Function         nci_brcm_tx_nci_hcit
**
** Description      Add packet type (HCIT_TYPE_NFC)
**
** Returns          TRUE, if NFCC can receive NCI message
**
*******************************************************************************/
static BOOLEAN nci_brcm_tx_nci_hcit (BT_HDR *p_msg)
{
    UINT8   *p;
    BOOLEAN send_to_nfcc = TRUE;
    UINT8   hcit;

    /* add packet type in front of NCI header */
    if (p_msg->offset > 0)
    {
        p_msg->offset--;
        p_msg->len++;

        p = (UINT8 *)(p_msg + 1) + p_msg->offset;
        *p = HCIT_TYPE_NFC;
    }
    else
    {
        NCI_TRACE_ERROR0 ("nci_brcm_tx_nci_hcit () : No space for packet type");
        hcit = HCIT_TYPE_NFC;
        USERIAL_Write (USERIAL_NFC_PORT, &hcit, 1);
    }

    return (send_to_nfcc);
}

/*******************************************************************************
**
** Function         nci_brcm_proc_rx_nci_msg
**
** Description      NFCC sends NCI message to DH. 
**                  Notify command complete if initializing NFCC
**                  Processing NTF for CE low power mode
**
** Returns          TRUE, if NFC task need to receive NCI message
**
*******************************************************************************/
static BOOLEAN nci_brcm_proc_rx_nci_msg (BT_HDR *p_msg)
{
    UINT8 *p;
    UINT8 mt, pbf, gid, op_code;
    UINT8 reset_reason, reset_type;
    UINT32 brcm_hw_id;
    tNFC_NCI_MSG  *p_nci_msg = (tNFC_NCI_MSG *)p_msg;
    tNFC_VS_CBACK *p_cback = p_nci_msg->p_cback;

    p = (UINT8 *)(p_msg + 1) + p_msg->offset;

    NFC_TRACE_DEBUG3 ("nci_brcm_proc_rx_nci_msg 0x%02x %02x, init sta:%d", *p, *(p + 1), nci_brcm_cb.initializing_state);
    NCI_MSG_PRS_HDR0 (p, mt, pbf, gid);
    NCI_MSG_PRS_HDR1 (p, op_code);

    /* if initializing BRCM NFCC */
    if (nci_brcm_cb.initializing_state != NCI_BRCM_INIT_STATE_IDLE)
    {
        if (gid == NCI_GID_CORE)
        {
            if (op_code == NCI_MSG_CORE_RESET)
            {
                if (mt == NCI_MT_RSP)
                {
                    NFC_BRCM_NCI_STATE(NCI_BRCM_INIT_STATE_W4_BUILD_INFO);

                    /* get build information to find out HW */
                    nci_brcm_send_nci_cmd (nci_brcm_prop_build_info_cmd, NCI_MSG_HDR_SIZE, NULL);
                }
                else 
                {
                    /* Call reset notification callback */
                    p++;                                /* Skip over param len */
                    STREAM_TO_UINT8(reset_reason, p);
                    STREAM_TO_UINT8(reset_type, p);
                    nfc_brcm_prm_spd_reset_ntf(reset_reason, reset_type);
                }
            }
        }
        else if (gid == NCI_GID_PROP) /* this is for download patch */
        {
            if (mt == NCI_MT_NTF)
                op_code |= NCI_NTF_BIT;
            else
                op_code |= NCI_RSP_BIT;

            if (  (op_code == NFC_VS_GET_BUILD_INFO_EVT)
                &&(nci_brcm_cb.initializing_state == NCI_BRCM_INIT_STATE_W4_BUILD_INFO)  )
            {
                p += NCI_BUILD_INFO_OFFSET_HWID;

                STREAM_TO_UINT32 (brcm_hw_id, p);

                NFC_BRCM_NCI_STATE(NCI_BRCM_INIT_STATE_W4_APP_COMPLETE);

                /* let application update baudrate or download patch */
                nci_brcm_cb.p_dev_init_cback (brcm_hw_id);
            }
            else if (p_cback)
            {
                (*p_cback) ((tNFC_VS_EVT)(op_code), 
                                           p_msg->len, (UINT8 *)(p_msg + 1) + p_msg->offset);
            }
            else if (op_code == NFC_VS_SEC_PATCH_AUTH_EVT)
            {
                NFC_TRACE_DEBUG0 ("signature!!");
                nfc_brcm_prm_nci_command_complete_cback ((tNFC_VS_EVT)(op_code), 
                                           p_msg->len, (UINT8 *)(p_msg + 1) + p_msg->offset);
            }
        }

        /* do send message to NFC task while initializing NFCC */
        return (FALSE);
    }

    if (nci_brcm_cb.power_mode == NCI_BRCM_POWER_MODE_FULL)
    {
        if (nci_brcm_cb.snooze_mode_enabled)
        {
            /* extend idle timer */
            nci_brcm_power_mode_execute (NCI_BRCM_LP_RX_DATA_EVT);
        }
    }
    else
    {
        if (nci_brcm_cb.p_vs_brcm_evt_hdlr)
        {
            return ((*nci_brcm_cb.p_vs_brcm_evt_hdlr) (p_msg));
        }
    }

    return (TRUE);
}

/*******************************************************************************
**
** Function         nci_brcm_proc_rx_packet_type
**
** Description      Processing packet type, NCI message or BT message
**
** Returns          TRUE, if more data to read
**
*******************************************************************************/
static BOOLEAN nci_brcm_proc_rx_packet_type (UINT8 *p_byte)
{
    /* if this is NCI message */
    if (*p_byte == HCIT_TYPE_NFC)
    {
        /* let NCI task process NCI message */
        nci_cb.rcv_state = NCI_RCV_IDLE_ST;
    }
    /* if this is BT message */
    else if (*p_byte == HCIT_TYPE_EVENT)
    {
        /* let NCI task use BRCM specific to read BT message */
        nci_cb.rcv_state = NCI_RCV_VS_MSG_ST;
    }
    else
    {
        NCI_TRACE_ERROR1 ("Unknown packet type drop this byte 0x%x", *p_byte); 
        return (TRUE);
    }

    /* read next byte to replace BRCM specific byte */
    if (USERIAL_Read (USERIAL_NFC_PORT, (UINT8 *)p_byte, 1) == 0)
    {
        /* no more data, wait for next NCI_TASK_EVT_DATA_RDY */
        return (FALSE);
    }
    else
    {
        /* continue to process received byte */
        return (TRUE);
    }
}

/*******************************************************************************
**
** Function         nci_brcm_rx_bt_msg
**
** Description      NFCC sends BT message to DH. 
**                  Reassemble BT message
**                  Notify command complete if initializing NFCC
**                  Forward BT message to NFC task
**
** Returns          TRUE, if NFCC can receive NCI message
**
*******************************************************************************/
static void nci_brcm_rx_bt_msg (UINT8 *p_byte)
{
    tNFC_BTVSC_CPLT vcs_cplt_params;
    UINT8           *p;
    BT_HDR          *p_msg;
    UINT16          opcode, old_opcode;
    tNFC_BTVSC_CPLT_CBACK *p_cback = nci_cb.p_last_cback;

    /* read BT message */
    if (nci_brcm_receive_msg (&nci_brcm_cb, *p_byte))
    {
        /* if complete BT message is received successfully */
        if (nci_brcm_cb.p_rcv_msg)
        {
            p_msg   = nci_brcm_cb.p_rcv_msg;
            NFC_TRACE_DEBUG1 ("nci_brcm_rx_bt_msg GOT an BT msgs init_sta:%d", nci_brcm_cb.initializing_state);
            NFC_TRACE_DEBUG2 ("event: 0x%x, last_ls:0x%x", p_msg->event, nci_cb.nci_last_ls);
            /* increase the cmd window here */
            if (p_msg->event == BT_EVT_TO_NFC_NCI_VS && nci_cb.nci_last_ls == NCI_MSGS_LS_VSC)
            {
                p = (UINT8 *)(p_msg + 1) + p_msg->offset;
                if (*p == HCI_COMMAND_COMPLETE_EVT)
                {
                    p  += 3; /* code, len, cmd window */
                    STREAM_TO_UINT16 (opcode, p);
                    p   = nci_cb.last_hdr;
                    STREAM_TO_UINT16 (old_opcode, p);
                    if (opcode == old_opcode)
                    {
                        if (p_msg->offset >= NFC_RECEIVE_MSGS_OFFSET)
                            ((tNFC_NCI_MSG *)p_msg)->p_cback  = nci_cb.p_last_cback;
                        nci_update_window(TRUE);
                    }
                }
            }

            /* if initializing BRCM NFCC */
            if (nci_brcm_cb.initializing_state == NCI_BRCM_INIT_STATE_W4_APP_COMPLETE)
            {
                /* this is command complete event for baud rate update or download patch */
                p = (UINT8 *)(p_msg + 1) + p_msg->offset;

                p += 1;    /* skip opcode */
                STREAM_TO_UINT8  (vcs_cplt_params.param_len, p);

                p += 1;    /* skip num command packets */
                STREAM_TO_UINT16 (vcs_cplt_params.opcode, p);

                vcs_cplt_params.param_len -= 3;
                vcs_cplt_params.p_param_buf = p;

                if (p_cback)
                {
                    nci_cb.p_last_cback = NULL;
                    (*p_cback) (&vcs_cplt_params);
                }

                /* do not send message to NFC task while initializing NFCC */
                GKI_freebuf (p_msg);
            }
            else
            {
                /* send BT message to NFC task */
                GKI_send_msg (NFC_TASK, NFC_MBOX_ID, p_msg);
            }
            nci_brcm_cb.p_rcv_msg = NULL;
        }

        /* start from checking BRCM packet type */
        nci_cb.rcv_state = NCI_RCV_VS_PREAMBLE_ST;
    }
}

/*******************************************************************************
**
** Function         nci_brcm_tx_bt_hcit
**
** Description      save bt msg info in control block
**                  Add packet type (HCIT_TYPE_COMMAND)
**
** Returns          TRUE, if NFCC can receive NCI message
**
*******************************************************************************/
static BOOLEAN nci_brcm_tx_bt_hcit (BT_HDR *p_msg)
{
    UINT8   *p;
    BOOLEAN send_to_nfcc = TRUE;
    UINT8   hcit;

#if (BT_TRACE_PROTOCOL == TRUE)
    DispHciCmd (p_msg);
#endif
    /* save the message header to double check the response */
    nci_cb.nci_last_ls  = (UINT8)(p_msg->layer_specific);
    p                   = (UINT8 *)(p_msg + 1) + p_msg->offset;
    memcpy(nci_cb.last_hdr, p, NCI_SAVED_HDR_SIZE);
    nci_cb.p_last_cback = ((BT_HDR_BTVSC *)p_msg)->p_cback;

    if (p_msg->offset > 0)
    {
        /* add packet type for BT message */
        p_msg->offset--;
        p_msg->len++;

        p = (UINT8 *)(p_msg + 1) + p_msg->offset;
        *p = HCIT_TYPE_COMMAND;
    }
    else
    {
        NCI_TRACE_ERROR0 ("nci_brcm_tx_bt_hcit () : No space for packet type");
        hcit = HCIT_TYPE_COMMAND;
        USERIAL_Write (USERIAL_NFC_PORT, &hcit, 1);
    }

    return (send_to_nfcc);
}

/*******************************************************************************
**
** Function         nci_brcm_evt_hdlr
**
** Description      This function processes events for Broadcom specific features
**                  in NCI transport layer.
**
** Returns          TRUE, if need to process after returning this function
**
*******************************************************************************/
static BOOLEAN nci_brcm_evt_hdlr (tNCI_INT_EVT event, void *p_data)
{
    BOOLEAN continue_to_proc = TRUE;
    BT_HDR  *p_msg;

#if (BT_TRACE_VERBOSE == TRUE)
    NCI_TRACE_EVENT2("nci_brcm_evt_hdlr ():%s(0x%04x)", nci_brcm_evt_2_str(event), event);
#else
    NCI_TRACE_EVENT1("nci_brcm_evt_hdlr ():0x%04x", event);
#endif

    switch (event)
    {
    case NCI_INT_CHECK_STATE_EVT:   /* Checks if the NFCC can accept messages     */
        continue_to_proc = nci_brcm_power_mode_execute (NCI_BRCM_LP_TX_DATA_EVT);
        break;

    case NCI_INT_SEND_NCI_MSG_EVT:  /* NFC task sends NCI command to NFCC */
        continue_to_proc = nci_brcm_tx_nci_hcit ((BT_HDR*)p_data);
        break;

    case NCI_INT_SEND_VS_CMD_EVT:
        /* the command is dispatched from command queue to be sent to transport */
        continue_to_proc = nci_brcm_tx_bt_hcit ((BT_HDR*)p_data);
        break;

    case NCI_INT_DATA_RDY_EVT:      /* transport get data from NFCC */

        if (nci_cb.rcv_state == NCI_RCV_VS_PREAMBLE_ST)
        {
            continue_to_proc = nci_brcm_proc_rx_packet_type ((UINT8 *)p_data);
        }
        else if (nci_cb.rcv_state == NCI_RCV_VS_MSG_ST)
        {
            /* read BT message */
            nci_brcm_rx_bt_msg ((UINT8 *)p_data);
        }
        break;

    case NCI_INT_RX_NCI_MSG_EVT:    /* received complete NCI message from NFCC */

        p_msg = (BT_HDR*)p_data;
        continue_to_proc = nci_brcm_proc_rx_nci_msg (p_msg);
        break;

    case NCI_INT_VS_MSG_EVT:    /* NFC task sends BT message to NFCC or called BRCM NCI APIs */

        p_msg = (BT_HDR*)p_data;

        switch (p_msg->event & BT_SUB_EVT_MASK)
        {
        case NCI_BRCM_HCI_CMD_EVT:
            /* put this message in NCI command queue */
            p_msg->event    = NCI_INT_SEND_VS_CMD_EVT;
            nci_send_cmd(p_msg);
            break;

        case NCI_BRCM_API_ENABLE_SNOOZE_EVT:
            nci_brcm_enable_snooze_mode (((tNCI_BRCM_SNOOZE_MSG *)p_data)->nfc_wake_active_mode);
            break;

        case NCI_BRCM_API_DISABLE_SNOOZE_EVT:
            nci_brcm_disable_snooze_mode ();
            break;

        default:
            if (nci_brcm_cb.p_vs_brcm_evt_hdlr)
            {
                (*nci_brcm_cb.p_vs_brcm_evt_hdlr) (p_msg);
            }
            break;
        }
        break;

    case NCI_INT_VS_INIT_EVT:   /* NFCC is turned on */

        /* if application registered callback for device initialization */
        if (nci_brcm_cb.p_dev_init_cback)
        {
            NFC_BRCM_NCI_STATE(NCI_BRCM_INIT_STATE_W4_RESET);

            /* send core reset before sending any command */
            nci_brcm_send_nci_cmd (nci_brcm_core_reset_cmd, NCI_MSG_HDR_SIZE + NCI_CORE_PARAM_SIZE_RESET, NULL);
        }
        else
        {
            /* notify complete of BRCM initialization */
            nci_task_vs_init_done ();
        }
        break;

    case NCI_INT_TERMINATE_EVT: /* NFCC is going to shut down */

        if (  (nci_brcm_cb.power_mode == NCI_BRCM_POWER_MODE_FULL)
            &&(nci_brcm_cb.snooze_mode_enabled)  )
        {
            nci_brcm_set_nfc_wake (NCI_ASSERT_NFC_WAKE);
        }

        nci_brcm_cb.power_mode          = NCI_BRCM_POWER_MODE_FULL;
        nci_brcm_cb.snooze_mode_enabled = FALSE;

        nci_stop_quick_timer (&nci_brcm_cb.lp_timer);

        while ((p_msg = (BT_HDR *)GKI_dequeue (&nci_brcm_cb.tx_data_q)) != NULL)
        {
            GKI_freebuf(p_msg);
        }

#if (defined(NFC_RESTORE_BAUD_ON_SHUTDOWN) && (NFC_RESTORE_BAUD_ON_SHUTDOWN == TRUE))
        if (nci_brcm_cb.userial_baud_rate != NFC_DEFAULT_BAUD)
        {
            NCI_BrcmSetBaudRate (NFC_DEFAULT_BAUD, NULL);
            GKI_delay (100);
        }
#endif
        break;

    default:
        break;
    }

    return (continue_to_proc);
}

/*******************************************************************************
**
** Function         nci_brcm_init
**
** Description      This function initializes Broadcom specific control blocks for 
**                  NCI transport
**
** Returns          void
**
*******************************************************************************/
void nci_brcm_init (tBRCM_DEV_INIT_CBACK *p_dev_init_cback)
{
    NCI_TRACE_DEBUG0 ("nci_brcm_init ()");

    nci_cb.p_vs_evt_hdlr  = nci_brcm_evt_hdlr;
    nci_cb.init_rcv_state = NCI_RCV_VS_PREAMBLE_ST; /* to process packet type */

    /* Clear nci brcm control block */
    memset(&nci_brcm_cb, 0, sizeof(tNCI_BRCM_CB));

    nci_brcm_cb.userial_baud_rate = NFC_DEFAULT_BAUD;
    nci_brcm_cb.lp_timer.p_cback  = nci_brcm_lp_timeout_cback;

    nci_brcm_cb.p_dev_init_cback  = p_dev_init_cback;

    nci_vs_brcm_init ();
}

/*******************************************************************************
**
** Function         nci_brcm_send_nci_cmd
**
** Description      Send NCI command to NFCC while initializing BRCM NFCC
**
** Returns          void
**
*******************************************************************************/
void nci_brcm_send_nci_cmd (UINT8 *p_data, UINT16 len, tNFC_VS_CBACK *p_cback)
{
    BT_HDR *p_buf;
    NCI_TRACE_DEBUG0 ("nci_brcm_send_nci_cmd ()");
    if ((p_buf = (BT_HDR *)GKI_getbuf ((UINT16)(BT_HDR_SIZE + NCI_VSC_MSG_HDR_SIZE + len))) != NULL)
    {
        p_buf->offset           = NCI_VSC_MSG_HDR_SIZE;
        p_buf->event            = BT_EVT_TO_NFC_NCI;
        p_buf->layer_specific   = NCI_MSGS_LS_NCI_VSC;
        p_buf->len              = len;
        memcpy ((UINT8*)(p_buf + 1) + p_buf->offset, p_data, len);
        ((tNFC_NCI_VS_MSG *)p_buf)->p_cback = p_cback;

        nci_send_cmd (p_buf);
    }
}

/*******************************************************************************
**
** Function         nci_brcm_send_bt_cmd
**
** Description      Send BT message to NFCC while initializing BRCM NFCC
**
** Returns          void
**
*******************************************************************************/
void nci_brcm_send_bt_cmd (UINT8 *p_data, UINT16 len, tNFC_BTVSC_CPLT_CBACK *p_cback)
{
    BT_HDR *p_buf;

    NCI_TRACE_DEBUG0 ("nci_brcm_send_bt_cmd ()");
    if ((p_buf = (BT_HDR *)GKI_getbuf ((UINT16)(BT_HDR_SIZE + NCI_VSC_MSG_HDR_SIZE + len))) != NULL)
    {
        p_buf->offset           = NCI_VSC_MSG_HDR_SIZE;
        p_buf->event            = NCI_INT_SEND_VS_CMD_EVT;
        p_buf->layer_specific   = NCI_MSGS_LS_VSC;
        p_buf->len              = len;
        memcpy ((UINT8*)(p_buf + 1) + p_buf->offset, p_data, len);
        ((BT_HDR_BTVSC *)p_buf)->p_cback = p_cback;

        nci_send_cmd (p_buf);
    }
}

/*******************************************************************************
**
** Function         nci_brcm_update_baudrate_cback
**
** Description      This is baud rate update complete callback.
**
** Returns          void
**
*******************************************************************************/
static void nci_brcm_update_baudrate_cback (tNFC_BTVSC_CPLT *pData)
{
    tNFC_STATUS status = pData->p_param_buf[0];

    /* if it is completed */
    if (status == HCI_SUCCESS) 
    {
        nci_brcm_set_local_baud_rate (nci_brcm_cb.userial_baud_rate);
    }

    if (nci_brcm_cb.p_update_baud_cback)
    {
        (nci_brcm_cb.p_update_baud_cback) (status);
        nci_brcm_cb.p_update_baud_cback = NULL;
    }
}

/*******************************************************************************
**
** Function         NCI_BrcmSetBaudRate
**
** Description      Set UART baud rate
**
** Returns          void
**
*******************************************************************************/
void NCI_BrcmSetBaudRate (UINT8             userial_baud_rate, 
                          tNFC_STATUS_CBACK *p_update_baud_cback)
{
    UINT8 *p = nci_brcm_set_baudrate_cmd + NCI_BRCM_OFFSET_BAUDRATE;
    UINT32 baud;

    NCI_TRACE_DEBUG1 ("NCI_BrcmSetBaudRate (): userial_baud_rate=%d", userial_baud_rate);

    baud = USERIAL_GetLineSpeed (userial_baud_rate);
    UINT32_TO_STREAM (p, baud);

    nci_brcm_cb.userial_baud_rate   = userial_baud_rate;
    nci_brcm_cb.p_update_baud_cback = p_update_baud_cback;

    nci_brcm_send_bt_cmd (nci_brcm_set_baudrate_cmd, 
                           BRCM_BT_HCI_CMD_HDR_SIZE + HCI_BRCM_UPDATE_BAUD_RATE_UNENCODED_LENGTH, 
                           nci_brcm_update_baudrate_cback);
}

/*******************************************************************************
**
** Function         NCI_BrcmDevInitDone
**
** Description      Notify NCI transport that device is initialized 
**
** Returns          void
**
*******************************************************************************/
void NCI_BrcmDevInitDone (void)
{
    NCI_TRACE_DEBUG0 ("NCI_BrcmDevInitDone ()");

    if (nci_brcm_cb.initializing_state == NCI_BRCM_INIT_STATE_W4_APP_COMPLETE)
    {
        NFC_BRCM_NCI_STATE(NCI_BRCM_INIT_STATE_IDLE);
        nci_set_cmd_timeout_val (NFC_CMD_CMPL_TIMEOUT * QUICK_TIMER_TICKS_PER_SEC);
        nci_cb.nci_cmd_cmpl_timer.p_cback   = nci_task_timeout_cback;

        nci_task_vs_init_done ();
    }
}

/*******************************************************************************
**
** Function         NCI_BrcmEnableSnoozeMode
**
** Description      Notify NCI transport that snooze mode has been enabled. 
**
** Returns          tNFC_STATUS
**
*******************************************************************************/
tNFC_STATUS NCI_BrcmEnableSnoozeMode (UINT8 nfc_wake_active_mode)
{
    tNCI_BRCM_SNOOZE_MSG *p_msg;

    NCI_TRACE_API1 ("NCI_BrcmEnableSnoozeMode (): nfc_wake_active_mode = %d", nfc_wake_active_mode);

    if (nci_brcm_cb.power_mode == NCI_BRCM_POWER_MODE_FULL)
    {
        p_msg = (tNCI_BRCM_SNOOZE_MSG *)GKI_getbuf (sizeof (tNCI_BRCM_SNOOZE_MSG));

        if (p_msg)
        {
            p_msg->bt_hdr.event = (BT_EVT_TO_NFC_NCI_VS | NCI_BRCM_API_ENABLE_SNOOZE_EVT);
            p_msg->nfc_wake_active_mode = nfc_wake_active_mode;

            GKI_send_msg (NCI_TASK, NCI_TASK_MBOX, p_msg);

            return (NFC_STATUS_OK);
        }
        else
        {
            NCI_TRACE_ERROR0 ("NCI_BrcmEnableSnoozeMode (): Out of buffer");
            return (NFC_STATUS_FAILED);
        }
    }
    else
    {
        NCI_TRACE_ERROR0 ("NCI_BrcmEnableSnoozeMode (): Not full power mode");
        return (NFC_STATUS_FAILED);
    }
}

/*******************************************************************************
**
** Function         NCI_BrcmDisableSnoozeMode
**
** Description      Notify NCI transport that snooze mode has been disabled.  
**
** Returns          tNFC_STATUS
**
*******************************************************************************/
tNFC_STATUS NCI_BrcmDisableSnoozeMode (void)
{
    BT_HDR *p_msg;

    NCI_TRACE_API0 ("NCI_BrcmDisableSnoozeMode ()");

    if (nci_brcm_cb.power_mode == NCI_BRCM_POWER_MODE_FULL)
    {
        p_msg = (BT_HDR *)GKI_getbuf (sizeof (BT_HDR));

        if (p_msg)
        {
            p_msg->event = (BT_EVT_TO_NFC_NCI_VS | NCI_BRCM_API_DISABLE_SNOOZE_EVT);

            GKI_send_msg (NCI_TASK, NCI_TASK_MBOX, p_msg);

            return (NFC_STATUS_OK);
        }
        else
        {
            NCI_TRACE_ERROR0 ("NCI_BrcmDisableSnoozeMode (): Out of buffer");
            return (NFC_STATUS_FAILED);
        }
    }
    else
    {
        NCI_TRACE_ERROR0 ("NCI_BrcmDisableSnoozeMode (): Not full power mode");
        return (NFC_STATUS_FAILED);
    }
}

#if (BT_TRACE_VERBOSE == TRUE)
/*******************************************************************************
**
** Function         nci_brcm_evt_2_str
**
** Description      convert nci brcm evt to string
**
*******************************************************************************/
static char *nci_brcm_evt_2_str(UINT16 event)
{
    switch (event)
    {
    case NCI_INT_CHECK_STATE_EVT:
        return "NCI_INT_CHECK_STATE_EVT";
    case NCI_INT_SEND_NCI_MSG_EVT:
        return "NCI_INT_SEND_NCI_MSG_EVT";
    case NCI_INT_SEND_VS_CMD_EVT:
        return "NCI_INT_SEND_VS_CMD_EVT";
    case NCI_INT_DATA_RDY_EVT:
        return "NCI_INT_DATA_RDY_EVT";
    case NCI_INT_RX_NCI_MSG_EVT:
        return "NCI_INT_RX_NCI_MSG_EVT";
    case NCI_INT_VS_MSG_EVT:
        return "NCI_INT_VS_MSG_EVT";
    case NCI_INT_VS_INIT_EVT:
        return "NCI_INT_VS_INIT_EVT";
    case NCI_INT_TERMINATE_EVT:
        return "NCI_INT_TERMINATE_EVT";
    }
    return "Unknown";
}
#endif

#endif

#endif /* NFC_INCLUDED == TRUE*/
