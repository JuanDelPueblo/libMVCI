/* passthru.c — SAE J2534 PassThru API on top of the MVCI session layer.
 *
 * Exposed as libMVCI.so (Linux) / MVCI32.dll, MVCI64.dll (Windows).
 *
 * ISO14230 K-line interface, so we model one channel
 * per opened device. Device/channel/filter IDs are small integers handed back
 * to the caller and mapped to a slot here.
 * 
 * Other Protocols are supported however unable to test without a suitable vehicle
 * 
 */

/* Mark this TU as the DLL build so j2534.h emits dllexport (Windows). The VS
 * project also defines this; guard to avoid a redefinition warning. */
#ifndef MVCI_BUILDING_DLL
#define MVCI_BUILDING_DLL
#endif
#include <mvci/j2534.h>
#include <mvci/serial.h>
#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The J2534 ABI is fixed width. A host binds these entry points with 32-bit
 * scalars, so a 64-bit scalar here would overrun every out-parameter. Fail the
 * build rather than the vehicle session. */
_Static_assert(sizeof(J2534_ULONG) == 4, "J2534 scalars must be 32-bit");
_Static_assert(sizeof(J2534_LONG) == 4, "J2534 status must be 32-bit");
_Static_assert(sizeof(PASSTHRU_MSG) == 4152, "PASSTHRU_MSG must be 4152 bytes");
_Static_assert(sizeof(SCONFIG) == 8, "SCONFIG must be 8 bytes");

#define MAX_SLOTS   4
#define MAX_FILTERS 32

typedef struct {
    int          open;
    int          connected;
    mvci_ctx_t  *ctx;
    uint32_t     proto;
    uint32_t     loopback;       /* host delivery policy, not a cable read-back */
    uint32_t     next_id;        /* public filter id generator; 0 = exhausted */
    uint32_t     filter_msg[MAX_FILTERS];  /* device msgid by slot, 0 = free */
    uint32_t     filter_id[MAX_FILTERS];   /* public id, separate from device handle */
} slot_t;

static slot_t       g_slot[MAX_SLOTS];
static mvci_mutex_t g_lock;
static int          g_init = 0;
static char         g_last_error[128] = "No error";

static void ensure_init(void)
{
    if (!g_init) { mvci_mutex_init(&g_lock); g_init = 1; }
}

static void set_err(const char *s) { strncpy(g_last_error, s, sizeof g_last_error - 1); }

/* device/channel/filter id encoding: dev = slot+1, channel = slot+0x100 */
#define DEV_ID(s)         ((J2534_ULONG)((s) + 1))
#define CH_ID(s)          ((J2534_ULONG)((s) + 0x100))
#define SLOT_FROM_DEV(id) ((int)(id) - 1)
#define SLOT_FROM_CH(id)  ((int)(id) - 0x100)

static slot_t *dev_slot(J2534_ULONG id)
{
    int i = SLOT_FROM_DEV(id);
    if (i < 0 || i >= MAX_SLOTS || !g_slot[i].open) return NULL;
    return &g_slot[i];
}
static slot_t *ch_slot(J2534_ULONG id)
{
    int i = SLOT_FROM_CH(id);
    if (i < 0 || i >= MAX_SLOTS || !g_slot[i].open) return NULL;
    return &g_slot[i];
}

/* ---- platform default port ------------------------------------------ */
static const char *default_port(void *pName)
{
#ifdef _WIN32
    (void)pName;
    return "M-VCI";                                  /* FTDI description */
#else
    if (pName && *(const char *)pName) return (const char *)pName;
    const char *env = getenv("MVCI_PORT");
    if (env) return env;
  #ifdef __APPLE__
    return "/dev/cu.usbserial";                      /* FTDI VCP node (macOS) */
  #else
    return "/dev/ttyUSB0";                           /* ftdi_sio node (Linux) */
  #endif
#endif
}

/* ====================================================================== */

J2534_LONG J2534_API PassThruOpen(void *pName, J2534_ULONG *pDeviceID)
{
    ensure_init();
    if (!pDeviceID) { set_err("NULL pDeviceID"); return ERR_NULL_PARAMETER; }

    mvci_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < MAX_SLOTS; i++) if (!g_slot[i].open) { slot = i; break; }
    if (slot < 0) { mvci_mutex_unlock(&g_lock); set_err("no free device slot"); return ERR_FAILED; }

    mvci_ctx_t *ctx = mvci_open(default_port(pName));
    if (!ctx) { mvci_mutex_unlock(&g_lock); set_err("device open failed"); return ERR_DEVICE_NOT_CONNECTED; }
    if (mvci_handshake(ctx) != 0) {
        mvci_close(ctx);
        mvci_mutex_unlock(&g_lock);
        set_err("handshake failed");
        return ERR_DEVICE_NOT_CONNECTED;
    }
    /* No background keepalive: T254/T255 prove the adapter tolerates
     * multi-second idle with zero keepalive traffic, and an autonomous
     * 15 ms keepalive would interleave extra adapter commands between the
     * characterized J2534 calls (notably across the 2000 ms five-baud
     * pre-init idle). Callers that hold a device open and idle far beyond
     * the evidenced windows may call mvci_start_keepalive() explicitly. */

    g_slot[slot].open = 1;
    g_slot[slot].connected = 0;
    g_slot[slot].ctx = ctx;
    g_slot[slot].next_id = 1;
    *pDeviceID = DEV_ID(slot);
    mvci_mutex_unlock(&g_lock);
    return STATUS_NOERROR;
}

J2534_LONG J2534_API PassThruClose(J2534_ULONG DeviceID)
{
    ensure_init();
    mvci_mutex_lock(&g_lock);
    slot_t *s = dev_slot(DeviceID);
    if (!s) { mvci_mutex_unlock(&g_lock); set_err("invalid device id"); return ERR_INVALID_DEVICE_ID; }
    if (s->connected) mvci_disconnect(s->ctx);       /* session teardown (01 00 02) */
    mvci_close(s->ctx);                              /* closes (stops keepalive if one was started explicitly) */
    memset(s, 0, sizeof *s);
    mvci_mutex_unlock(&g_lock);
    return STATUS_NOERROR;
}

J2534_LONG J2534_API PassThruConnect(J2534_ULONG DeviceID, J2534_ULONG ProtocolID,
                               J2534_ULONG Flags, J2534_ULONG BaudRate,
                               J2534_ULONG *pChannelID)
{
    ensure_init();
    slot_t *s = dev_slot(DeviceID);
    if (!s) { set_err("invalid device id"); return ERR_INVALID_DEVICE_ID; }
    if (!pChannelID) { set_err("NULL pChannelID"); return ERR_NULL_PARAMETER; }

    if (mvci_connect(s->ctx, ProtocolID, Flags, BaudRate) != 0) {
        set_err("connect failed");
        return ERR_FAILED;
    }
    s->connected = 1;
    s->proto = ProtocolID;
    /* Preserve the historical delivery of device echoes on a new channel. */
    s->loopback = 1;
    *pChannelID = CH_ID(SLOT_FROM_DEV(DeviceID));
    return STATUS_NOERROR;
}

J2534_LONG J2534_API PassThruDisconnect(J2534_ULONG ChannelID)
{
    ensure_init();
    slot_t *s = ch_slot(ChannelID);
    if (!s || !s->connected) { set_err("invalid channel id"); return ERR_INVALID_CHANNEL_ID; }
    /* Real wire teardown (01 00 02). A later PassThruConnect re-runs the
     * handshake when the device needs it (see mvci_connect), so dropping the
     * channel here does not strand the next session. */
    if (mvci_disconnect(s->ctx) != 0) { set_err("disconnect failed"); return ERR_FAILED; }
    s->connected = 0;
    s->next_id = 1;
    memset(s->filter_msg, 0, sizeof s->filter_msg);
    return STATUS_NOERROR;
}

J2534_LONG J2534_API PassThruStartMsgFilter(J2534_ULONG ChannelID, J2534_ULONG FilterType,
                                      PASSTHRU_MSG *pMaskMsg, PASSTHRU_MSG *pPatternMsg,
                                      PASSTHRU_MSG *pFlowControlMsg, J2534_ULONG *pFilterID)
{
    ensure_init();
    slot_t *s = ch_slot(ChannelID);
    if (!s || !s->connected) { set_err("invalid channel id"); return ERR_INVALID_CHANNEL_ID; }
    if (!pMaskMsg || !pPatternMsg || !pFilterID) { set_err("NULL filter msg"); return ERR_NULL_PARAMETER; }
    /* Only the one-byte PASS wire form is evidenced. Never turn a request
     * for another type, length or flags into a less restrictive filter. */
    if (FilterType != PASS_FILTER) {
        set_err("only one-byte PASS filters are supported");
        return ERR_NOT_SUPPORTED;
    }
    if (pFlowControlMsg || !pMaskMsg->DataSize ||
        pMaskMsg->DataSize != pPatternMsg->DataSize) {
        set_err("invalid filter messages");
        return ERR_INVALID_MSG;
    }
    if (pMaskMsg->DataSize != 1) {
        set_err("only one-byte PASS filters are supported");
        return ERR_NOT_SUPPORTED;
    }
    /* Retain the existing zero-initialized descriptor form. An explicit
     * ProtocolID must match the channel, and the two descriptors must agree. */
    if (pMaskMsg->ProtocolID != pPatternMsg->ProtocolID ||
        (pMaskMsg->ProtocolID && pMaskMsg->ProtocolID != s->proto)) {
        set_err("filter protocol does not match channel");
        return ERR_MSG_PROTOCOL_ID;
    }
    if (pMaskMsg->TxFlags || pPatternMsg->TxFlags) {
        set_err("filter flags are not supported");
        return ERR_INVALID_FLAGS;
    }
    int slot = -1;
    for (int i = 0; i < MAX_FILTERS; i++)
        if (!s->filter_msg[i]) { slot = i; break; }
    if (slot < 0 || !s->next_id) {
        set_err("filter capacity exceeded");
        return ERR_EXCEEDED_LIMIT;
    }
    /* Reuse an inactive device-handle slot, but give it a new public ID so
     * a stale StopMsgFilter cannot stop its replacement in this session. */
    uint32_t msgid = 0x000e7a01u + (uint32_t)slot;
    uint8_t mask = pMaskMsg->Data[0], pattern = pPatternMsg->Data[0];

    if (mvci_start_filter(s->ctx, msgid, mask, pattern) != 0) {
        set_err("start filter failed");
        return ERR_FAILED;
    }
    s->filter_msg[slot] = msgid;
    s->filter_id[slot] = s->next_id;
    *pFilterID = s->next_id++;
    return STATUS_NOERROR;
}

J2534_LONG J2534_API PassThruStopMsgFilter(J2534_ULONG ChannelID, J2534_ULONG FilterID)
{
    ensure_init();
    slot_t *s = ch_slot(ChannelID);
    if (!s || !s->connected) { set_err("invalid channel id"); return ERR_INVALID_CHANNEL_ID; }
    /* Forward the vendor-evidenced stop-filter command (T255 frames 504/522)
     * with the same device handle the start-filter command carried. */
    int slot = -1;
    for (int i = 0; i < MAX_FILTERS; i++)
        if (s->filter_msg[i] && s->filter_id[i] == FilterID) { slot = i; break; }
    if (slot < 0) {
        set_err("invalid filter id");
        return ERR_INVALID_FILTER_ID;
    }
    if (mvci_stop_filter(s->ctx, s->filter_msg[slot]) != 0) {
        set_err("stop filter failed");
        return ERR_FAILED;
    }
    s->filter_msg[slot] = 0;
    s->filter_id[slot] = 0;
    return STATUS_NOERROR;
}

J2534_LONG J2534_API PassThruWriteMsgs(J2534_ULONG ChannelID, PASSTHRU_MSG *pMsg,
                                 J2534_ULONG *pNumMsgs, J2534_ULONG TimeInterval)
{
    ensure_init();
    (void)TimeInterval;
    slot_t *s = ch_slot(ChannelID);
    if (!s || !s->connected) { set_err("invalid channel id"); return ERR_INVALID_CHANNEL_ID; }
    if (!pMsg || !pNumMsgs || *pNumMsgs == 0) { set_err("NULL/empty msg"); return ERR_NULL_PARAMETER; }
    if (pMsg->DataSize == 0 || pMsg->DataSize > 64) { set_err("bad DataSize"); return ERR_INVALID_MSG; }

    if (mvci_write_msg(s->ctx, pMsg->Data, pMsg->DataSize) != 0) {
        set_err("write failed");
        return ERR_FAILED;
    }
    *pNumMsgs = 1;
    return STATUS_NOERROR;
}

J2534_LONG J2534_API PassThruReadMsgs(J2534_ULONG ChannelID, PASSTHRU_MSG *pMsg,
                                J2534_ULONG *pNumMsgs, J2534_ULONG Timeout)
{
    ensure_init();
    slot_t *s = ch_slot(ChannelID);
    if (!s || !s->connected) { set_err("invalid channel id"); return ERR_INVALID_CHANNEL_ID; }
    if (!pMsg || !pNumMsgs || *pNumMsgs == 0) { set_err("NULL/empty msg"); return ERR_NULL_PARAMETER; }

    J2534_ULONG want = *pNumMsgs, got = 0;
    uint32_t deadline = mvci_now_ms() + (uint32_t)(Timeout ? Timeout : 1);

    while (got < want) {
        uint8_t buf[64], rx = 0;
        int m = mvci_poll(s->ctx, buf, sizeof buf, &rx, 200);
        if (m > 0) {
            /* Device status 2 is an echo, except the captured one-byte 48
             * START_OF_MESSAGE prefix. Never suppress that receive marker. */
            if (!s->loopback && rx == 2 && !(m == 1 && buf[0] == 0x48)) {
                if ((int32_t)(deadline - mvci_now_ms()) <= 0) break;
                continue;
            }
            PASSTHRU_MSG *o = &pMsg[got];
            memset(o, 0, sizeof *o);
            o->ProtocolID = s->proto;
            o->RxStatus   = rx;                       /* 0 = data, 2 = tx echo */
            o->DataSize   = (uint32_t)m;
            memcpy(o->Data, buf, (size_t)m);
            got++;
            continue;
        }
        if ((int32_t)(deadline - mvci_now_ms()) <= 0) break;
        mvci_sleep_ms(5);
    }

    *pNumMsgs = got;
    if (got == 0) { set_err("no message"); return ERR_BUFFER_EMPTY; }
    return STATUS_NOERROR;
}

J2534_LONG J2534_API PassThruIoctl(J2534_ULONG ChannelID, J2534_ULONG IoctlID,
                             void *pInput, void *pOutput)
{
    ensure_init();
    /* Voltage reads use a device handle, even though they remain unsupported.
     * Every implemented IOCTL below requires a live channel handle. */
    if (IoctlID == READ_VBATT || IoctlID == READ_PROG_VOLTAGE) {
        if (!dev_slot(ChannelID)) {
            set_err("invalid device id");
            return ERR_INVALID_DEVICE_ID;
        }
        set_err("voltage read is not supported");
        return ERR_NOT_SUPPORTED;
    }
    slot_t *s = ch_slot(ChannelID);
    if (!s || !s->connected) { set_err("invalid channel id"); return ERR_INVALID_CHANNEL_ID; }

    switch (IoctlID) {
    case GET_CONFIG: {
        SCONFIG_LIST *list = (SCONFIG_LIST *)pInput;
        if (!list || (list->NumOfParams && !list->ConfigPtr)) return ERR_NULL_PARAMETER;
        /* There is no evidenced cable GET command. A SET acknowledgement is
         * not proof that firmware applied a timing parameter. Only report the
         * LOOPBACK policy enforced by this driver's read path. Validate the
         * whole list before changing any caller-owned value. */
        for (uint32_t i = 0; i < list->NumOfParams; i++) {
            uint32_t param = list->ConfigPtr[i].Parameter;
            if (param != LOOPBACK) {
                const char *name = param == P1_MAX ? "P1_MAX" :
                                   param == P3_MIN ? "P3_MIN" :
                                   param == P4_MIN ? "P4_MIN" :
                                   param == DATA_RATE ? "DATA_RATE" : "parameter";
                snprintf(g_last_error, sizeof g_last_error,
                         "GET_CONFIG %s (0x%08x): applied cable value is unknown",
                         name, (unsigned)param);
                return ERR_NOT_SUPPORTED;
            }
        }
        for (uint32_t i = 0; i < list->NumOfParams; i++)
            list->ConfigPtr[i].Value = s->loopback;
        return STATUS_NOERROR;
    }
    case SET_CONFIG: {
        SCONFIG_LIST *list = (SCONFIG_LIST *)pInput;
        if (!list || (list->NumOfParams && !list->ConfigPtr)) return ERR_NULL_PARAMETER;
        for (uint32_t i = 0; i < list->NumOfParams; i++)
            if (list->ConfigPtr[i].Parameter == LOOPBACK && list->ConfigPtr[i].Value > 1) {
                set_err("LOOPBACK must be 0 or 1");
                return ERR_INVALID_IOCTL_VALUE;
            }
        for (uint32_t i = 0; i < list->NumOfParams; i++) {
            if (list->ConfigPtr[i].Parameter == LOOPBACK) {
                /* Forward real device echoes when enabled; never synthesize
                 * one from the requested TX bytes. No cable command needed. */
                s->loopback = list->ConfigPtr[i].Value;
                continue;
            }
            if (mvci_set_config(s->ctx, list->ConfigPtr[i].Parameter,
                                list->ConfigPtr[i].Value) != 0) {
                set_err("set_config failed");
                return ERR_FAILED;
            }
        }
        return STATUS_NOERROR;
    }
    case CLEAR_PERIODIC_MSGS:
        if (mvci_clear_periodic(s->ctx) != 0) {
            set_err("clear periodic messages failed");
            return ERR_FAILED;
        }
        return STATUS_NOERROR;

    case CLEAR_RX_BUFFER:
        /* Software and host serial RX only; no cable mailbox clear is known. */
        if (mvci_clear_rx_buffer(s->ctx) != 0) {
            set_err("clear host RX buffer failed");
            return ERR_FAILED;
        }
        return STATUS_NOERROR;

    case CLEAR_TX_BUFFER:
        /* There is no host TX queue or evidenced cable cancellation command.
         * An acknowledged write does not prove the cable has finished TX. */
        set_err("TX clear unsupported: cable cancellation is unknown");
        return ERR_NOT_SUPPORTED;

    case CLEAR_MSG_FILTERS:
        /* Use the existing stop operation, not a guessed bulk-clear opcode.
         * Commit each successful stop. Keep the failed and unvisited entries
         * active so a later retry can address the exact same device handles. */
        for (int i = 0; i < MAX_FILTERS; i++) {
            if (!s->filter_msg[i]) continue;
            if (mvci_stop_filter(s->ctx, s->filter_msg[i]) != 0) {
                set_err("clear filters failed; remaining filters retained");
                return ERR_FAILED;
            }
            s->filter_msg[i] = 0;
            s->filter_id[i] = 0;
        }
        return STATUS_NOERROR;

    case FAST_INIT: {
        PASSTHRU_MSG *in = (PASSTHRU_MSG *)pInput;
        PASSTHRU_MSG *out = (PASSTHRU_MSG *)pOutput;
        if (!out) { set_err("NULL fast init output"); return ERR_NULL_PARAMETER; }
        if (!in || in->DataSize == 0) { set_err("pattern-only fast init unsupported"); return ERR_NOT_SUPPORTED; }
        if (in->DataSize > sizeof in->Data) { set_err("fast init input too large"); return ERR_INVALID_IOCTL_VALUE; }
        uint8_t reply[61];
        int k = mvci_fast_init(s->ctx, in->Data, in->DataSize, reply, sizeof reply);
        if (k == MVCI_FI_TIMEOUT) {
            if (out) {
                memset(out, 0, sizeof *out);
                out->DataSize = 0;
            }
            set_err("fast init timeout");
            return ERR_TIMEOUT;
        }
        if (k < 0) { set_err("fast init failed"); return ERR_FAILED; }
        memset(out, 0, sizeof *out);
        out->ProtocolID = s->proto;
        out->DataSize = (uint32_t)k;
        out->ExtraDataIndex = out->DataSize;
        memcpy(out->Data, reply, (size_t)k);
        return STATUS_NOERROR;
    }

    case FIVE_BAUD_INIT: {
        SBYTE_ARRAY *in = (SBYTE_ARRAY *)pInput;
        SBYTE_ARRAY *out = (SBYTE_ARRAY *)pOutput;
        if (!in || !out) { set_err("NULL five-baud init array"); return ERR_NULL_PARAMETER; }
        if (in->NumOfBytes == 0 || !in->BytePtr) { set_err("empty five-baud init address"); return ERR_INVALID_IOCTL_VALUE; }
        if (in->NumOfBytes > MVCI_MAX_INNER - 8) { set_err("five-baud init address too large"); return ERR_FAILED; }
        if (!out->BytePtr) { set_err("NULL five-baud output buffer"); return ERR_NULL_PARAMETER; }
        if (out->NumOfBytes < 2) {
            out->NumOfBytes = 2;
            set_err("five-baud output buffer too small; required 2 bytes");
            return ERR_FAILED;
        }
        uint8_t reply[2];
        int k = mvci_five_baud_init(s->ctx, in->BytePtr, in->NumOfBytes, reply, sizeof reply);
        if (k == MVCI_FB_NOT_CONNECTED) {
            set_err("five-baud no ECU answer");
            return ERR_DEVICE_NOT_CONNECTED;
        }
        if (k < 0) { set_err("five-baud failed"); return ERR_FAILED; }
        memcpy(out->BytePtr, reply, (size_t)k);
        out->NumOfBytes = (uint32_t)k;
        return STATUS_NOERROR;
    }

    default:
        set_err("ioctl not supported");
        return ERR_NOT_SUPPORTED;
    }
}

/* ---- stubs / informational ------------------------------------------ */

J2534_LONG J2534_API PassThruStartPeriodicMsg(J2534_ULONG ChannelID, PASSTHRU_MSG *pMsg,
                                        J2534_ULONG *pMsgID, J2534_ULONG TimeInterval)
{ (void)ChannelID;(void)pMsg;(void)pMsgID;(void)TimeInterval; return ERR_NOT_SUPPORTED; }

J2534_LONG J2534_API PassThruStopPeriodicMsg(J2534_ULONG ChannelID, J2534_ULONG MsgID)
{ (void)ChannelID;(void)MsgID; return ERR_NOT_SUPPORTED; }

J2534_LONG J2534_API PassThruSetProgrammingVoltage(J2534_ULONG DeviceID, J2534_ULONG PinNumber,
                                             J2534_ULONG Voltage)
{
    ensure_init();
    slot_t *s = dev_slot(DeviceID);
    if (!s) { set_err("invalid device id"); return ERR_INVALID_DEVICE_ID; }
    if (!mvci_prog_voltage_is_supported(PinNumber, Voltage)) {
        set_err("prog voltage not supported");
        return ERR_NOT_SUPPORTED;
    }
    if (mvci_set_prog_voltage(s->ctx, PinNumber, Voltage) != 0) {
        set_err("prog voltage failed");
        return ERR_FAILED;
    }
    return STATUS_NOERROR;
}

J2534_LONG J2534_API PassThruReadVersion(J2534_ULONG DeviceID, char *pFirmwareVersion,
                                   char *pDllVersion, char *pApiVersion)
{
    ensure_init();
    if (!dev_slot(DeviceID)) return ERR_INVALID_DEVICE_ID;
    /* Report the same identity as the original Mini-VCI DLL so the host app
     * (e.g. Toyota Techstream) recognises the adapter and selects ISO14230. */
    if (pFirmwareVersion) strcpy(pFirmwareVersion, "J2534 MINIV1.03");
    if (pDllVersion)      strcpy(pDllVersion, "MVCI J2534 DLL v1.4.6");
    if (pApiVersion)      strcpy(pApiVersion, "04.04");
    return STATUS_NOERROR;
}

J2534_LONG J2534_API PassThruGetLastError(char *pErrorDescription)
{
    if (!pErrorDescription) return ERR_NULL_PARAMETER;
    strcpy(pErrorDescription, g_last_error);
    return STATUS_NOERROR;
}
