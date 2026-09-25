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
    uint32_t     next_id;        /* filter/msg id generator */
    uint32_t     filter_msg[MAX_FILTERS];  /* device msgid by (FilterID-1), 0 = free */
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
    (void)FilterType; (void)pFlowControlMsg;
    slot_t *s = ch_slot(ChannelID);
    if (!s || !s->connected) { set_err("invalid channel id"); return ERR_INVALID_CHANNEL_ID; }
    if (!pMaskMsg || !pPatternMsg || !pFilterID) { set_err("NULL filter msg"); return ERR_NULL_PARAMETER; }

    uint8_t mask    = pMaskMsg->DataSize    ? pMaskMsg->Data[0]    : 0;
    uint8_t pattern = pPatternMsg->DataSize ? pPatternMsg->Data[0] : 0;
    if (s->next_id == 0 || s->next_id > MAX_FILTERS) { set_err("too many filters"); return ERR_FAILED; }
    uint32_t msgid  = 0x000e7a00u + (s->next_id & 0xff);    /* device-style handle */

    if (mvci_start_filter(s->ctx, msgid, mask, pattern) != 0) {
        set_err("start filter failed");
        return ERR_FAILED;
    }
    s->filter_msg[s->next_id - 1] = msgid;
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
    if (FilterID == 0 || FilterID > MAX_FILTERS || s->filter_msg[FilterID - 1] == 0) {
        set_err("invalid filter id");
        return ERR_INVALID_FILTER_ID;
    }
    if (mvci_stop_filter(s->ctx, s->filter_msg[FilterID - 1]) != 0) {
        set_err("stop filter failed");
        return ERR_FAILED;
    }
    s->filter_msg[FilterID - 1] = 0;
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
    slot_t *s = ch_slot(ChannelID);
    if (!s) { set_err("invalid channel id"); return ERR_INVALID_CHANNEL_ID; }

    switch (IoctlID) {
    case SET_CONFIG: {
        SCONFIG_LIST *list = (SCONFIG_LIST *)pInput;
        if (!list || (list->NumOfParams && !list->ConfigPtr)) return ERR_NULL_PARAMETER;
        for (uint32_t i = 0; i < list->NumOfParams; i++)
            if (mvci_set_config(s->ctx, list->ConfigPtr[i].Parameter,
                                list->ConfigPtr[i].Value) != 0) {
                set_err("set_config failed");
                return ERR_FAILED;
            }
        return STATUS_NOERROR;
    }
    case CLEAR_PERIODIC_MSGS:
        return mvci_clear_periodic(s->ctx) == 0 ? STATUS_NOERROR : ERR_FAILED;

    case CLEAR_TX_BUFFER:
    case CLEAR_RX_BUFFER:
    case CLEAR_MSG_FILTERS:
        return STATUS_NOERROR;

    case FAST_INIT: {
        PASSTHRU_MSG *in = (PASSTHRU_MSG *)pInput;
        PASSTHRU_MSG *out = (PASSTHRU_MSG *)pOutput;
        if (!in || in->DataSize == 0) return ERR_NULL_PARAMETER;
        uint8_t reply[32];
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
        if (out) {
            memset(out, 0, sizeof *out);
            out->ProtocolID = s->proto;
            out->DataSize = (uint32_t)k;
            memcpy(out->Data, reply, (size_t)k);
        }
        return STATUS_NOERROR;
    }

    case FIVE_BAUD_INIT: {
        SBYTE_ARRAY *in = (SBYTE_ARRAY *)pInput;
        SBYTE_ARRAY *out = (SBYTE_ARRAY *)pOutput;
        if (!in || in->NumOfBytes == 0 || !in->BytePtr) return ERR_INVALID_IOCTL_VALUE;
        uint8_t reply[32];
        int k = mvci_five_baud_init(s->ctx, in->BytePtr, in->NumOfBytes, reply, sizeof reply);
        if (k == MVCI_FB_NOT_CONNECTED) {
            set_err("five-baud no ECU answer");
            return ERR_DEVICE_NOT_CONNECTED;
        }
        if (k < 0) { set_err("five-baud failed"); return ERR_FAILED; }
        if (out && out->BytePtr && out->NumOfBytes >= (uint32_t)k) {
            memcpy(out->BytePtr, reply, (size_t)k);
            out->NumOfBytes = (uint32_t)k;
        }
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
