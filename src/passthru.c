/* passthru.c — SAE J2534 PassThru API on top of the MVCI session layer.
 *
 * Exposed as libMVCI.so (Linux) / MVCI32.dll, MVCI64.dll (Windows).
 *
 * ISO14230 K-line interface, so we model one channel
 * per opened device. Device and channel handles are unique 32-bit values that
 * are never reused; filter IDs are per channel. Each is matched to a slot here.
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
#include "pipeline.h"
#include <limits.h>

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
#define J2534_VERSION_TEXT_SIZE 80
#ifdef _MSC_VER
#define MVCI_THREAD_LOCAL __declspec(thread)
#else
#define MVCI_THREAD_LOCAL _Thread_local
#endif

/* Handles are unique, never reused and never derived from an array offset.
 * One counter issues device and channel handles, so the two can not collide
 * and a stale handle from an earlier Open or Connect never names a new object.
 * The counter does not wrap: at the last value every Open/Connect refuses. */
#ifndef MVCI_FIRST_HANDLE
#define MVCI_FIRST_HANDLE 1u
#endif
#define MVCI_LAST_HANDLE  0xFFFFFFFEu

/* Slot guard state is protected by g_lock. Calls into a slot are counted so
 * Close frees the context only after every call has left. */
typedef struct {
    int          opening;    /* reserved by an Open that is still in I/O */
    int          open;
    int          connected;
    int          closing;    /* Close has begun: lookups fail at once */
    int          excl;       /* Connect/Disconnect/Close owns the slot */
    int          excl_wait;  /* exclusive calls queued; they make reads give way */
    int          busy;       /* shared calls inside the slot */
    uint32_t     dev_id;     /* 0 = none */
    uint32_t     ch_id;      /* 0 = no channel */
    mvci_ctx_t  *ctx;
    uint32_t     proto;
    uint32_t     loopback;       /* host delivery policy, not a cable read-back */
    uint32_t     next_id;        /* public filter id generator; 0 = exhausted */
    uint32_t     filter_msg[MAX_FILTERS];  /* device msgid by slot, 0 = free */
    uint32_t     filter_id[MAX_FILTERS];   /* public id, separate from device handle */
    mvci_mutex_t cfg;        /* serialises filter and IOCTL state on the channel */
} slot_t;

static slot_t       g_slot[MAX_SLOTS];
static mvci_mutex_t g_lock;       /* slot guard state; never held across I/O */
static mvci_cond_t  g_cv;         /* any guard state change */
static mvci_mutex_t g_open_lock;  /* one Open handshake at a time */
static uint32_t     g_next_handle = MVCI_FIRST_HANDLE;
static MVCI_THREAD_LOCAL char g_last_error[J2534_VERSION_TEXT_SIZE] = "No error";

/* One-time initialisation, safe against concurrent first calls. The locks are
 * never destroyed: a late caller must not touch a dead lock. */
static void init_globals(void)
{
    mvci_mutex_init(&g_lock);
    mvci_mutex_init(&g_open_lock);
    mvci_cond_init(&g_cv);
    for (int i = 0; i < MAX_SLOTS; i++) mvci_mutex_init(&g_slot[i].cfg);
}

#ifdef _WIN32
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_once_cb(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    init_globals();
    return TRUE;
}
static void ensure_init(void) { InitOnceExecuteOnce(&g_once, init_once_cb, NULL, NULL); }
#else
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void ensure_init(void) { (void)pthread_once(&g_once, init_globals); }
#endif

/* Test-only seam for handle exhaustion; absent from the shipped library. */
#ifdef MVCI_TESTING
#if defined(__GNUC__) && !defined(_WIN32)
__attribute__((visibility("hidden")))
#endif
void mvci_test_set_next_handle(uint32_t next)
{
    ensure_init();
    mvci_mutex_lock(&g_lock);
    g_next_handle = next;
    mvci_mutex_unlock(&g_lock);
}
#endif

static void set_err(const char *s)
{
    if (!s) s = "unspecified error";
    (void)snprintf(g_last_error, sizeof g_last_error, "%s", s);
}

/* Caller holds g_lock. 0 = identifier space exhausted. */
static uint32_t alloc_handle_locked(void)
{
    if (g_next_handle < MVCI_FIRST_HANDLE || g_next_handle > MVCI_LAST_HANDLE) return 0;
    return g_next_handle++;
}

/* Caller holds g_lock. Exact match only: no arithmetic on a caller handle. */
static slot_t *find_locked(uint32_t id, int by_channel)
{
    if (!id) return NULL;
    for (int i = 0; i < MAX_SLOTS; i++) {
        slot_t *s = &g_slot[i];
        if (!s->open || s->closing) continue;
        if ((by_channel ? s->ch_id : s->dev_id) == id) return s;
    }
    return NULL;
}

/* ---- slot lifetime guard ---------------------------------------------
 *
 * Every public call that touches a slot runs between slot_enter() and
 * slot_leave(). Close frees the context only after every such call has left.
 *
 *   SLOT_SHARED   Read/Write/Ioctl/filters/voltage/version. Run together.
 *   SLOT_DISC     Disconnect. Runs alone.
 *   SLOT_CONNECT  Connect. Runs alone; refuses a live channel without waiting.
 *   SLOT_CLOSE    Close. Marks the slot closing first, so later lookups fail
 *                 at once, then waits for the running calls to leave.
 *
 * A channel-scoped shared call never waits: a queued or running Disconnect or
 * Close means the channel is going away, so it fails at once and its caller's
 * Read/Write budget is untouched. Device-scoped shared calls (voltage,
 * version) wait for a running Connect. Exclusive calls have no caller
 * timeout; a waiting one cancels running reads (see mvci_set_read_cancel) so a
 * long read does not hold it off. An uncancelable running call (filter,
 * IOCTL, write) is bounded by its own transport timeout. Close may therefore
 * wait for such a call to finish. */
enum { SLOT_SHARED = 0, SLOT_DISC = 1, SLOT_CONNECT = 2, SLOT_CLOSE = 3 };
enum { ENTER_OK = 0, ENTER_INVALID = 1, ENTER_BUSY = 2, ENTER_IN_USE = 3 };

/* Caller holds g_lock. */
static void slot_sync_cancel_locked(slot_t *s)
{
    if (s->ctx) mvci_set_read_cancel(s->ctx, s->excl_wait > 0 || s->closing);
}

static slot_t *slot_enter(J2534_ULONG id, int by_channel, int mode, int *why)
{
    mvci_mutex_lock(&g_lock);
    slot_t *s = find_locked(id, by_channel);
    if (!s) { mvci_mutex_unlock(&g_lock); *why = ENTER_INVALID; return NULL; }

    if (mode == SLOT_SHARED) {
        if (s->excl || s->excl_wait) {
            if (by_channel) { mvci_mutex_unlock(&g_lock); *why = ENTER_BUSY; return NULL; }
            while ((s = find_locked(id, by_channel)) != NULL && (s->excl || s->excl_wait))
                mvci_cond_wait(&g_cv, &g_lock);
            if (!s) { mvci_mutex_unlock(&g_lock); *why = ENTER_INVALID; return NULL; }
        }
        s->busy++;
        mvci_mutex_unlock(&g_lock);
        *why = ENTER_OK;
        return s;
    }

    if (mode == SLOT_CONNECT && s->connected) {
        mvci_mutex_unlock(&g_lock);
        *why = ENTER_IN_USE;
        return NULL;
    }
    if (mode == SLOT_CLOSE) s->closing = 1;
    else s->excl_wait++;
    slot_sync_cancel_locked(s);
    mvci_cond_broadcast(&g_cv);        /* queued shared callers re-test */

    while ((s->excl || s->busy) && (mode == SLOT_CLOSE || !s->closing))
        mvci_cond_wait(&g_cv, &g_lock);

    if (mode == SLOT_CLOSE) {
        s->excl = 1;
        mvci_mutex_unlock(&g_lock);
        *why = ENTER_OK;
        return s;
    }
    s->excl_wait--;
    /* The slot can not have been freed while we queued (Close needs excl
     * and we are not counted as busy), but Close may have marked it. A
     * Disconnect/Connect ahead of us may have changed the channel. */
    int ok = s->open && !s->closing;
    int in_use = 0;
    if (ok && mode == SLOT_DISC) ok = s->ch_id == id;
    if (ok && mode == SLOT_CONNECT) ok = s->dev_id == id;
    if (ok && mode == SLOT_CONNECT && s->connected) { ok = 0; in_use = 1; }
    if (!ok) {
        slot_sync_cancel_locked(s);
        mvci_cond_broadcast(&g_cv);
        mvci_mutex_unlock(&g_lock);
        *why = in_use ? ENTER_IN_USE : ENTER_INVALID;
        return NULL;
    }
    s->excl = 1;
    mvci_mutex_unlock(&g_lock);
    *why = ENTER_OK;
    return s;
}

static void slot_leave(slot_t *s, int mode)
{
    mvci_mutex_lock(&g_lock);
    if (mode == SLOT_SHARED) s->busy--;
    else s->excl = 0;
    slot_sync_cancel_locked(s);
    mvci_cond_broadcast(&g_cv);
    mvci_mutex_unlock(&g_lock);
}

/* Finish a Close that owns the slot: free it. Its handles are never issued
 * again, so a stale handle stays invalid after the slot is reused. */
static void slot_release_closed(slot_t *s)
{
    mvci_mutex_lock(&g_lock);
    /* Queued Connect/Disconnect calls see `closing`, leave and uncount
     * themselves. Reuse of the slot waits for them. */
    while (s->excl_wait > 0) mvci_cond_wait(&g_cv, &g_lock);
    s->open = s->connected = s->closing = s->excl = 0;
    s->busy = 0;
    s->dev_id = s->ch_id = 0;
    s->ctx = NULL;
    s->proto = s->loopback = s->next_id = 0;
    memset(s->filter_msg, 0, sizeof s->filter_msg);
    memset(s->filter_id, 0, sizeof s->filter_id);
    mvci_cond_broadcast(&g_cv);
    mvci_mutex_unlock(&g_lock);
}

/* 1 when a Connect/Disconnect/Close waits for this slot. A long read polls
 * this and gives way, so those calls are not held off. */
static int slot_abort_pending(slot_t *s)
{
    mvci_mutex_lock(&g_lock);
    int r = s->excl_wait > 0 || s->closing;
    mvci_mutex_unlock(&g_lock);
    return r;
}

/* Fields that a read polls while an IOCTL may change them. */
static uint32_t slot_loopback(slot_t *s)
{
    mvci_mutex_lock(&g_lock);
    uint32_t v = s->loopback;
    mvci_mutex_unlock(&g_lock);
    return v;
}
static void slot_set_loopback(slot_t *s, uint32_t v)
{
    mvci_mutex_lock(&g_lock);
    s->loopback = v;
    mvci_mutex_unlock(&g_lock);
}

/* Serialise filter and IOCTL state changes on one channel. Returns 0, with the
 * lock held, unless a Close/Disconnect queued while we waited. */
static int cfg_lock(slot_t *s)
{
    mvci_mutex_lock(&s->cfg);
    if (slot_abort_pending(s)) { mvci_mutex_unlock(&s->cfg); return -1; }
    return 0;
}
static void cfg_unlock(slot_t *s) { mvci_mutex_unlock(&s->cfg); }

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

    /* Opens run one at a time but do not hold g_lock across the handshake,
     * so a slow Open does not stall calls on other devices. */
    mvci_mutex_lock(&g_open_lock);
    mvci_mutex_lock(&g_lock);
    slot_t *s = NULL;
    for (int i = 0; i < MAX_SLOTS; i++)
        if (!g_slot[i].open && !g_slot[i].opening) { s = &g_slot[i]; break; }
    if (!s) {
        mvci_mutex_unlock(&g_lock); mvci_mutex_unlock(&g_open_lock);
        set_err("no free device slot");
        return ERR_FAILED;
    }
    uint32_t id = alloc_handle_locked();
    if (!id) {
        mvci_mutex_unlock(&g_lock); mvci_mutex_unlock(&g_open_lock);
        set_err("handle space exhausted");
        return ERR_FAILED;
    }
    s->opening = 1;
    mvci_mutex_unlock(&g_lock);

    mvci_ctx_t *ctx = mvci_open(default_port(pName));
    const char *fail = NULL;
    if (!ctx) {
        fail = "device open failed";
    } else if (mvci_handshake(ctx) != 0) {
        mvci_close(ctx);
        ctx = NULL;
        fail = "handshake failed";
    }
    /* No background keepalive: T254/T255 prove the adapter tolerates
     * multi-second idle with zero keepalive traffic, and an autonomous
     * 15 ms keepalive would interleave extra adapter commands between the
     * characterized J2534 calls (notably across the 2000 ms five-baud
     * pre-init idle). Callers that hold a device open and idle far beyond
     * the evidenced windows may call mvci_start_keepalive() explicitly. */

    mvci_mutex_lock(&g_lock);
    s->opening = 0;
    if (ctx) {
        s->open = 1;
        s->connected = 0;
        s->closing = s->excl = s->excl_wait = s->busy = 0;
        s->ctx = ctx;
        s->dev_id = id;
        s->ch_id = 0;
        s->next_id = 1;
        mvci_set_read_cancel(ctx, 0);
    }
    mvci_cond_broadcast(&g_cv);
    mvci_mutex_unlock(&g_lock);
    mvci_mutex_unlock(&g_open_lock);
    if (!ctx) { set_err(fail); return ERR_DEVICE_NOT_CONNECTED; }
    *pDeviceID = id;
    return STATUS_NOERROR;
}

J2534_LONG J2534_API PassThruClose(J2534_ULONG DeviceID)
{
    ensure_init();
    int why;
    slot_t *s = slot_enter(DeviceID, 0, SLOT_CLOSE, &why);
    if (!s) { set_err("invalid device id"); return ERR_INVALID_DEVICE_ID; }
    /* No call is inside the slot now and none can enter. Detach the context
     * first so a late waiter never signals a freed one. */
    mvci_mutex_lock(&g_lock);
    mvci_ctx_t *ctx = s->ctx;
    s->ctx = NULL;
    mvci_mutex_unlock(&g_lock);
    if (s->connected) mvci_disconnect(ctx);          /* session teardown (01 00 02) */
    mvci_close(ctx);                                 /* closes (stops keepalive if one was started explicitly) */
    slot_release_closed(s);
    return STATUS_NOERROR;
}

/* K-line channels this driver supports, and the one baud rate evidenced for
 * them (Corolla ISO 9141-2 / ISO 14230 at 10400). Anything else is refused
 * before a byte reaches the cable. */
#define MVCI_KLINE_BAUD        10400u
#define MVCI_CONNECT_FLAGS_OK  (ISO9141_NO_CHECKSUM | ISO9141_K_LINE_ONLY)

static J2534_LONG pipeline_error(int r);

J2534_LONG J2534_API PassThruConnect(J2534_ULONG DeviceID, J2534_ULONG ProtocolID,
                               J2534_ULONG Flags, J2534_ULONG BaudRate,
                               J2534_ULONG *pChannelID)
{
    ensure_init();
    mvci_mutex_lock(&g_lock);
    int known = find_locked(DeviceID, 0) != NULL;
    mvci_mutex_unlock(&g_lock);
    if (!known) { set_err("invalid device id"); return ERR_INVALID_DEVICE_ID; }
    if (!pChannelID) { set_err("NULL pChannelID"); return ERR_NULL_PARAMETER; }

    if (ProtocolID != ISO9141 && ProtocolID != ISO14230) {
        if (ProtocolID >= J1850VPW && ProtocolID <= SCI_B_TRANS) {
            set_err("protocol not supported");
            return ERR_NOT_SUPPORTED;
        }
        set_err("invalid protocol id");
        return ERR_INVALID_PROTOCOL_ID;
    }
    if (Flags & ~(J2534_ULONG)MVCI_CONNECT_FLAGS_OK) {
        set_err("connect flags not supported");
        return ERR_INVALID_FLAGS;
    }
    if (BaudRate != MVCI_KLINE_BAUD) {
        set_err("baud rate not supported");
        return ERR_INVALID_BAUDRATE;
    }

    int why;
    slot_t *s = slot_enter(DeviceID, 0, SLOT_CONNECT, &why);
    if (!s) {
        if (why == ENTER_IN_USE) { set_err("channel already connected"); return ERR_CHANNEL_IN_USE; }
        set_err("invalid device id");
        return ERR_INVALID_DEVICE_ID;
    }
    J2534_LONG rc = STATUS_NOERROR;
    mvci_mutex_lock(&g_lock);
    uint32_t chid = alloc_handle_locked();
    mvci_mutex_unlock(&g_lock);
    if (!chid) {
        set_err("handle space exhausted");
        rc = ERR_FAILED;
    } else {
        /* Start from a clean host session: no stale frames from a previous
         * channel, and no new Connect on a context with an unknown result.
         * Nothing reaches the cable if this refuses. */
        int r = mvci_reset_session(s->ctx);
        if (r != 0) {
            rc = pipeline_error(r);
        } else if (mvci_connect(s->ctx, ProtocolID, Flags, BaudRate) != 0) {
            /* A missing or rejected Connect ACK is never retried here. A
             * missing ACK leaves the context poisoned until Close/Open. */
            set_err("connect failed");
            rc = ERR_FAILED;
        } else {
            mvci_mutex_lock(&g_lock);
            s->connected = 1;
            s->ch_id = chid;
            s->proto = ProtocolID;
            /* Preserve the historical delivery of device echoes on a new channel. */
            s->loopback = 1;
            s->next_id = 1;
            memset(s->filter_msg, 0, sizeof s->filter_msg);
            memset(s->filter_id, 0, sizeof s->filter_id);
            mvci_mutex_unlock(&g_lock);
            *pChannelID = chid;
        }
    }
    slot_leave(s, SLOT_CONNECT);
    return rc;
}

J2534_LONG J2534_API PassThruDisconnect(J2534_ULONG ChannelID)
{
    ensure_init();
    int why;
    slot_t *s = slot_enter(ChannelID, 1, SLOT_DISC, &why);
    if (!s) { set_err("invalid channel id"); return ERR_INVALID_CHANNEL_ID; }
    /* Real wire teardown (01 00 02). The exact reply is the session boundary:
     * mvci_disconnect drops the old session's queued and partial frames. */
    J2534_LONG rc = STATUS_NOERROR;
    if (mvci_disconnect(s->ctx) != 0) {
        set_err("disconnect failed");
        rc = ERR_FAILED;
    } else {
        mvci_mutex_lock(&g_lock);
        s->connected = 0;
        s->ch_id = 0;               /* the old channel handle is dead for good */
        s->proto = 0;
        s->loopback = 0;
        s->next_id = 1;
        memset(s->filter_msg, 0, sizeof s->filter_msg);
        memset(s->filter_id, 0, sizeof s->filter_id);
        mvci_mutex_unlock(&g_lock);
    }
    slot_leave(s, SLOT_DISC);
    return rc;
}

static J2534_LONG start_filter_impl(slot_t *s, J2534_ULONG FilterType,
                                   PASSTHRU_MSG *pMaskMsg, PASSTHRU_MSG *pPatternMsg,
                                   PASSTHRU_MSG *pFlowControlMsg, J2534_ULONG *pFilterID)
{
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

static J2534_LONG stop_filter_impl(slot_t *s, J2534_ULONG FilterID)
{
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

/* Enter a channel-scoped call for state changes (filters, IOCTLs). Returns the
 * slot with the shared guard and the channel's cfg lock held, or NULL with the
 * error already recorded. */
static slot_t *channel_state_enter(J2534_ULONG ChannelID, J2534_LONG *rc)
{
    int why;
    slot_t *s = slot_enter(ChannelID, 1, SLOT_SHARED, &why);
    if (s && cfg_lock(s) != 0) { slot_leave(s, SLOT_SHARED); s = NULL; }
    if (!s) { set_err("invalid channel id"); *rc = ERR_INVALID_CHANNEL_ID; }
    return s;
}
static void channel_state_leave(slot_t *s)
{
    cfg_unlock(s);
    slot_leave(s, SLOT_SHARED);
}

J2534_LONG J2534_API PassThruStartMsgFilter(J2534_ULONG ChannelID, J2534_ULONG FilterType,
                                      PASSTHRU_MSG *pMaskMsg, PASSTHRU_MSG *pPatternMsg,
                                      PASSTHRU_MSG *pFlowControlMsg, J2534_ULONG *pFilterID)
{
    ensure_init();
    J2534_LONG rc;
    slot_t *s = channel_state_enter(ChannelID, &rc);
    if (!s) return rc;
    rc = start_filter_impl(s, FilterType, pMaskMsg, pPatternMsg, pFlowControlMsg, pFilterID);
    channel_state_leave(s);
    return rc;
}

J2534_LONG J2534_API PassThruStopMsgFilter(J2534_ULONG ChannelID, J2534_ULONG FilterID)
{
    ensure_init();
    J2534_LONG rc;
    slot_t *s = channel_state_enter(ChannelID, &rc);
    if (!s) return rc;
    rc = stop_filter_impl(s, FilterID);
    channel_state_leave(s);
    return rc;
}

/* Pipeline errors remain distinct from a valid empty poll. Counts describe
 * completed adapter ACKs/delivered records, never ECU acceptance. */
static J2534_LONG pipeline_error(int r)
{
    if (r == MVCI_PIPE_TIMEOUT) { set_err("deadline expired; command result may be unknown"); return ERR_TIMEOUT; }
    if (r == MVCI_PIPE_IO) { set_err("transport lost or I/O failed; close and reopen"); return ERR_DEVICE_NOT_CONNECTED; }
    if (r == MVCI_PIPE_OVERFLOW) { set_err("receive data lost: bounded queue or record overflow"); return ERR_BUFFER_OVERFLOW; }
    if (r == MVCI_PIPE_CANCELED) { set_err("interrupted by channel close or disconnect"); return ERR_FAILED; }
    if (r == MVCI_PIPE_REJECTED) { set_err("adapter rejected command or returned unknown status"); return ERR_FAILED; }
    set_err("invalid frame or ambiguous reply; close and reopen");
    return ERR_FAILED;
}

/* Read/Write budget: the deadline is taken once, when the public call starts,
 * and the implementation spends the same deadline. Waiting for the I/O lock is
 * charged to it. The slot guard never waits for a channel-scoped call (see
 * slot_enter), so it can not consume the caller's budget either. */
static J2534_LONG write_msgs_impl(slot_t *s, PASSTHRU_MSG *pMsg, J2534_ULONG want,
                                  J2534_ULONG *pNumMsgs, J2534_ULONG Timeout,
                                  uint32_t deadline)
{
    /* Validate the entire batch before any side effect. Unproven flags are
     * refused instead of silently being encoded as zero. */
    for (J2534_ULONG i = 0; i < want; i++) {
        if (!pMsg[i].DataSize || pMsg[i].DataSize > MVCI_PIPE_MAX_DATA) { set_err("bad DataSize"); return ERR_INVALID_MSG; }
        if (pMsg[i].ProtocolID != s->proto) { set_err("message protocol differs from channel"); return ERR_MSG_PROTOCOL_ID; }
        if (pMsg[i].TxFlags) { set_err("transmit flags not supported"); return ERR_INVALID_FLAGS; }
    }
    /* There is no asynchronous TX worker/queue. Do not claim queued success
     * or send an untracked command when the caller asks for queue-only mode. */
    if (!Timeout) { set_err("zero-timeout queued writes not supported"); return ERR_NOT_SUPPORTED; }
    for (J2534_ULONG i = 0; i < want; i++) {
        int remaining = (int32_t)(deadline - mvci_now_ms());
        if (remaining <= 0) return pipeline_error(MVCI_PIPE_TIMEOUT);
        int r = mvci_write_msg_timeout(s->ctx, pMsg[i].Data, pMsg[i].DataSize, remaining);
        if (r < 0) return pipeline_error(r);
        ++*pNumMsgs;
    }
    return STATUS_NOERROR;
}

/* Common front of Read/Write. Returns the entered slot, or NULL with *rc and
 * the error text set. *pNumMsgs is zero on every refusal. */
static slot_t *msgs_enter(J2534_ULONG ChannelID, PASSTHRU_MSG *pMsg, J2534_ULONG *pNumMsgs,
                          J2534_ULONG Timeout, J2534_ULONG *want, J2534_LONG *rc)
{
    if (!pNumMsgs) { set_err("NULL pNumMsgs"); *rc = ERR_NULL_PARAMETER; return NULL; }
    *want = *pNumMsgs;
    *pNumMsgs = 0;
    mvci_mutex_lock(&g_lock);
    int known = find_locked(ChannelID, 1) != NULL;
    mvci_mutex_unlock(&g_lock);
    if (!known) { set_err("invalid channel id"); *rc = ERR_INVALID_CHANNEL_ID; return NULL; }
    if (!pMsg || !*want) { set_err("NULL/empty msg"); *rc = ERR_NULL_PARAMETER; return NULL; }
    /* Timeouts above INT_MAX are refused, not widened into "forever". */
    if (Timeout > INT_MAX) {
        set_err("timeout exceeds supported monotonic interval");
        *rc = ERR_INVALID_TIME_INTERVAL;
        return NULL;
    }
    int why;
    slot_t *s = slot_enter(ChannelID, 1, SLOT_SHARED, &why);
    if (!s) {
        set_err(why == ENTER_BUSY ? "channel is closing or disconnecting" : "invalid channel id");
        *rc = ERR_INVALID_CHANNEL_ID;
    }
    return s;
}

J2534_LONG J2534_API PassThruWriteMsgs(J2534_ULONG ChannelID, PASSTHRU_MSG *pMsg,
                                 J2534_ULONG *pNumMsgs, J2534_ULONG Timeout)
{
    ensure_init();
    uint32_t deadline = mvci_now_ms() + Timeout;
    J2534_ULONG want = 0;
    J2534_LONG rc;
    slot_t *s = msgs_enter(ChannelID, pMsg, pNumMsgs, Timeout, &want, &rc);
    if (!s) return rc;
    rc = write_msgs_impl(s, pMsg, want, pNumMsgs, Timeout, deadline);
    slot_leave(s, SLOT_SHARED);
    return rc;
}

/* A waiting Close/Disconnect ends the read. Messages already moved into the
 * caller's buffer are reported, not dropped. */
static J2534_LONG read_interrupted(J2534_ULONG delivered)
{
    if (delivered) return STATUS_NOERROR;
    return pipeline_error(MVCI_PIPE_CANCELED);
}

static J2534_LONG read_msgs_impl(slot_t *s, PASSTHRU_MSG *pMsg, J2534_ULONG want,
                                 J2534_ULONG *pNumMsgs, J2534_ULONG Timeout,
                                 uint32_t deadline)
{
    unsigned work_left = MVCI_PIPE_READ_BUDGET;
    while (*pNumMsgs < want) {
        if (slot_abort_pending(s)) return read_interrupted(*pNumMsgs);
        int remaining = (int32_t)(deadline - mvci_now_ms());
        if (Timeout && remaining <= 0) return pipeline_error(MVCI_PIPE_TIMEOUT);
        if (Timeout) work_left = MVCI_PIPE_READ_BUDGET;
        else if (!work_left) break;
        uint8_t buf[MVCI_PIPE_MAX_DATA], rx = 0;
        int m = mvci_poll_until(s->ctx, buf, sizeof buf, &rx, deadline,
                                Timeout != 0, &work_left);
        if (m == MVCI_PIPE_CANCELED) return read_interrupted(*pNumMsgs);
        if (m < 0) return pipeline_error(m);
        if (m > 0 && (slot_loopback(s) || rx != 2 || (m == 1 && buf[0] == 0x48))) {
            PASSTHRU_MSG *o = &pMsg[*pNumMsgs];
            memset(o, 0, sizeof *o);
            o->ProtocolID = s->proto;
            int prefix = rx == 2 && m == 1 && buf[0] == 0x48;
            o->RxStatus = rx == 2 && !prefix ? TX_MSG_TYPE : rx;
            o->DataSize = (uint32_t)m;
            /* Complete records have no extra bytes. Preserve the observed
             * one-byte receive indication separately from real TX echoes. */
            o->ExtraDataIndex = prefix ? 0 : (uint32_t)m;
            memcpy(o->Data, buf, (size_t)m);
            ++*pNumMsgs;
            continue;
        }
        if (!Timeout) {
            if (m > 0) continue; /* suppressed echo; continue the nonblocking drain */
            if (*pNumMsgs) return STATUS_NOERROR;
            set_err("no message available");
            return ERR_BUFFER_EMPTY;
        }
        /* A valid empty reply can be immediate. Pace within the same budget;
         * virtual-clock tests exercise this without wall-clock sleeps. */
        remaining = (int32_t)(deadline - mvci_now_ms());
        if (remaining <= 0) return pipeline_error(MVCI_PIPE_TIMEOUT);
        if (!m) mvci_sleep_ms(remaining < 5 ? remaining : 5);
    }
    if (*pNumMsgs) return STATUS_NOERROR;
    set_err("no message available");
    return ERR_BUFFER_EMPTY;
}

J2534_LONG J2534_API PassThruReadMsgs(J2534_ULONG ChannelID, PASSTHRU_MSG *pMsg,
                                J2534_ULONG *pNumMsgs, J2534_ULONG Timeout)
{
    ensure_init();
    uint32_t deadline = mvci_now_ms() + Timeout;
    J2534_ULONG want = 0;
    J2534_LONG rc;
    slot_t *s = msgs_enter(ChannelID, pMsg, pNumMsgs, Timeout, &want, &rc);
    if (!s) return rc;
    rc = read_msgs_impl(s, pMsg, want, pNumMsgs, Timeout, deadline);
    slot_leave(s, SLOT_SHARED);
    return rc;
}

static J2534_LONG ioctl_impl(slot_t *s, J2534_ULONG IoctlID, void *pInput, void *pOutput)
{
    switch (IoctlID) {
    case GET_CONFIG: {
        SCONFIG_LIST *list = (SCONFIG_LIST *)pInput;
        if (!list || (list->NumOfParams && !list->ConfigPtr)) {
            set_err("NULL configuration list or parameter array");
            return ERR_NULL_PARAMETER;
        }
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
            list->ConfigPtr[i].Value = slot_loopback(s);
        return STATUS_NOERROR;
    }
    case SET_CONFIG: {
        SCONFIG_LIST *list = (SCONFIG_LIST *)pInput;
        if (!list || (list->NumOfParams && !list->ConfigPtr)) {
            set_err("NULL configuration list or parameter array");
            return ERR_NULL_PARAMETER;
        }
        for (uint32_t i = 0; i < list->NumOfParams; i++)
            if (list->ConfigPtr[i].Parameter == LOOPBACK && list->ConfigPtr[i].Value > 1) {
                set_err("LOOPBACK must be 0 or 1");
                return ERR_INVALID_IOCTL_VALUE;
            }
        for (uint32_t i = 0; i < list->NumOfParams; i++) {
            if (list->ConfigPtr[i].Parameter == LOOPBACK) {
                /* Forward real device echoes when enabled; never synthesize
                 * one from the requested TX bytes. No cable command needed. */
                slot_set_loopback(s, list->ConfigPtr[i].Value);
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

J2534_LONG J2534_API PassThruIoctl(J2534_ULONG ChannelID, J2534_ULONG IoctlID,
                             void *pInput, void *pOutput)
{
    ensure_init();
    /* Voltage reads use a device handle, even though they remain unsupported.
     * Every implemented IOCTL requires a live channel handle. */
    if (IoctlID == READ_VBATT || IoctlID == READ_PROG_VOLTAGE) {
        int why;
        slot_t *d = slot_enter(ChannelID, 0, SLOT_SHARED, &why);
        if (!d) {
            set_err("invalid device id");
            return ERR_INVALID_DEVICE_ID;
        }
        slot_leave(d, SLOT_SHARED);
        set_err("voltage read is not supported");
        return ERR_NOT_SUPPORTED;
    }
    J2534_LONG rc;
    slot_t *s = channel_state_enter(ChannelID, &rc);
    if (!s) return rc;
    rc = ioctl_impl(s, IoctlID, pInput, pOutput);
    channel_state_leave(s);
    return rc;
}

/* ---- stubs / informational ------------------------------------------ */

J2534_LONG J2534_API PassThruStartPeriodicMsg(J2534_ULONG ChannelID, PASSTHRU_MSG *pMsg,
                                        J2534_ULONG *pMsgID, J2534_ULONG TimeInterval)
{ (void)ChannelID;(void)pMsg;(void)pMsgID;(void)TimeInterval;
  set_err("periodic transmission is not supported"); return ERR_NOT_SUPPORTED; }

J2534_LONG J2534_API PassThruStopPeriodicMsg(J2534_ULONG ChannelID, J2534_ULONG MsgID)
{ (void)ChannelID;(void)MsgID;
  set_err("periodic transmission is not supported"); return ERR_NOT_SUPPORTED; }

J2534_LONG J2534_API PassThruSetProgrammingVoltage(J2534_ULONG DeviceID, J2534_ULONG PinNumber,
                                             J2534_ULONG Voltage)
{
    ensure_init();
    int why;
    slot_t *s = slot_enter(DeviceID, 0, SLOT_SHARED, &why);
    if (!s) { set_err("invalid device id"); return ERR_INVALID_DEVICE_ID; }
    J2534_LONG rc = STATUS_NOERROR;
    if (!mvci_prog_voltage_is_supported(PinNumber, Voltage)) {
        set_err("prog voltage not supported");
        rc = ERR_NOT_SUPPORTED;
    } else if (mvci_set_prog_voltage(s->ctx, PinNumber, Voltage) != 0) {
        set_err("prog voltage failed");
        rc = ERR_FAILED;
    }
    slot_leave(s, SLOT_SHARED);
    return rc;
}

J2534_LONG J2534_API PassThruReadVersion(J2534_ULONG DeviceID, char *pFirmwareVersion,
                                   char *pDllVersion, char *pApiVersion)
{
    ensure_init();
    int why;
    slot_t *s = slot_enter(DeviceID, 0, SLOT_SHARED, &why);
    if (!s) { set_err("invalid device id"); return ERR_INVALID_DEVICE_ID; }
    slot_leave(s, SLOT_SHARED);
    if (!pFirmwareVersion || !pDllVersion || !pApiVersion) {
        set_err("NULL version output");
        return ERR_NULL_PARAMETER;
    }
    /* J2534 defines these output strings as 80-byte fields, but provides no
     * capacity arguments. The adapter firmware is not queried by this driver. */
    (void)snprintf(pFirmwareVersion, J2534_VERSION_TEXT_SIZE,
                   "%s", "Unknown (not queried)");
    (void)snprintf(pDllVersion, J2534_VERSION_TEXT_SIZE,
                   "%s", "libMVCI 1.0.0");
    (void)snprintf(pApiVersion, J2534_VERSION_TEXT_SIZE, "%s", "04.04");
    return STATUS_NOERROR;
}

J2534_LONG J2534_API PassThruGetLastError(char *pErrorDescription)
{
    if (!pErrorDescription) { set_err("NULL error output"); return ERR_NULL_PARAMETER; }
    (void)snprintf(pErrorDescription, J2534_VERSION_TEXT_SIZE, "%s", g_last_error);
    return STATUS_NOERROR;
}
