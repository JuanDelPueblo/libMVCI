/* mvci_serial.c — MVCI protocol + session (transport- and crypto-agnostic).
 * Transport via mvci_io.h, DES via mvci_des.c, threading via mvci_compat.h. */

#include <mvci/serial.h>
#include "io.h"
#include "compat.h"
#include "pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================
 *  Pure codec
 * ====================================================================== */

/* The one-byte frame length includes the 3-byte header/trailer. Encrypted
 * payloads must also contain whole DES blocks, so 248 is the largest cipher
 * body that can fit in a valid frame (255 - 3, rounded down to 8 bytes). */
#define MVCI_MAX_WIRE_CIPHER (((UINT8_MAX - 3u) / 8u) * 8u)

uint8_t mvci_xorsum(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) c ^= p[i];
    return c;
}

int mvci_frame_plain(const uint8_t *payload, size_t plen, uint8_t *out, size_t out_cap)
{
    if (!out || (plen && !payload) || plen > UINT8_MAX - 3u) return -1;
    size_t total = plen + 3;
    if (total > out_cap) return -1;
    out[0] = (uint8_t)total;
    out[1] = 0x00;
    if (plen) memcpy(out + 2, payload, plen);
    out[2 + plen] = mvci_xorsum(out, 2 + plen);
    return (int)total;
}

int mvci_frame_enc(const uint8_t key[8], const uint8_t *inner, size_t inner_len,
                   uint8_t *out, size_t out_cap)
{
    if (!key || !inner || !out || inner_len == 0 || inner_len > MVCI_MAX_WIRE_CIPHER)
        return -1;
    uint8_t body[MVCI_MAX_INNER];
    size_t nb = inner_len / 8 + (inner_len % 8 != 0);
    size_t clen = nb * 8;
    if (clen > sizeof body || clen > MVCI_MAX_WIRE_CIPHER) return -1;
    memset(body, 0, clen);
    memcpy(body, inner, inner_len);
    if (mvci_des_encrypt_checked(key, body, nb) != 0) return -1;
    return mvci_frame_plain(body, clen, out, out_cap);
}

int mvci_frame_payload(const uint8_t *frame, size_t frame_len, const uint8_t **payload_out)
{
    if (!frame || frame_len < 3 || frame_len > UINT8_MAX) return -1;
    if (frame[0] != frame_len) return -1;
    if (frame[1] != 0x00) return -1;
    if (mvci_xorsum(frame, frame_len - 1) != frame[frame_len - 1]) return -1;
    if (payload_out) *payload_out = frame + 2;
    return (int)(frame_len - 3);
}

int mvci_frame_decrypt(const uint8_t key[8], const uint8_t *frame, size_t frame_len,
                       uint8_t *inner_out, size_t inner_cap)
{
    if (!key || !frame || !inner_out || frame_len < 3 || frame_len > UINT8_MAX ||
        frame[0] != frame_len || frame[1] != 0x00) return -1;
    int plen = (int)frame_len - 3;
    if (plen <= 0 || (plen % 8) != 0 || (size_t)plen > inner_cap) return -1;

    uint8_t wire_cs   = frame[frame_len - 1];
    uint8_t cs_cipher = mvci_xorsum(frame, frame_len - 1);     /* over ciphertext */

    memcpy(inner_out, frame + 2, plen);
    if (mvci_des_decrypt_checked(key, inner_out, plen / 8) != 0) return -1;

    /* status replies checksum over ciphertext, message replies over plaintext */
    uint8_t cs_plain = frame[0] ^ frame[1] ^ mvci_xorsum(inner_out, plen);
    if (wire_cs != cs_cipher && wire_cs != cs_plain) return -1;
    return plen;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int mvci_inner_set_config(uint32_t proto, uint32_t param, uint32_t value, uint8_t *inner)
{
    if (!inner) return -1;
    inner[0] = 0x0e; inner[1] = 0x00; inner[2] = 0x0e; inner[3] = 0x02;
    put_u32(inner + 4, proto);
    put_u32(inner + 8, param);
    put_u32(inner + 12, value);
    return 16;
}

int mvci_inner_connect(uint32_t proto, uint32_t flags, uint32_t baud, uint8_t *inner)
{
    if (!inner) return -1;
    inner[0] = 0x0d; inner[1] = 0x00; inner[2] = 0x07;
    put_u32(inner + 3, proto);
    put_u32(inner + 7, flags);
    put_u32(inner + 11, baud);
    return 15;
}

int mvci_inner_start_filter(uint32_t proto, uint32_t msgid, uint32_t type,
                            uint8_t mask, uint8_t pattern, uint8_t *inner)
{
    if (!inner) return -1;
    inner[0] = 0x10; inner[1] = 0x00; inner[2] = 0x0b;
    put_u32(inner + 3, proto);
    put_u32(inner + 7, msgid);
    put_u32(inner + 11, type);
    inner[15] = mask;
    inner[16] = pattern;
    return 17;
}

int mvci_inner_clear_periodic(uint32_t proto, uint8_t *inner)
{
    if (!inner) return -1;
    static const uint8_t b[4] = { 0x06, 0x00, 0x0e, 0x09 };
    memcpy(inner, b, 4);
    put_u32(inner + 4, proto);
    return 8;
}

int mvci_inner_fast_init(uint32_t proto, const uint8_t *init, size_t n, uint8_t *inner)
{
    if (!init || n == 0 || !inner || n > MVCI_MAX_WIRE_CIPHER - 8u) return -1;
    inner[0] = (uint8_t)(5 + 1 + n);
    inner[1] = 0x00; inner[2] = 0x0e; inner[3] = 0x05;
    put_u32(inner + 4, proto);
    memcpy(inner + 8, init, n);
    return (int)(8 + n);
}

int mvci_inner_five_baud_init(uint32_t proto, const uint8_t *addr, size_t naddr, uint8_t *inner)
{
    if (!inner || !addr || naddr == 0 || naddr > MVCI_MAX_WIRE_CIPHER - 8u) return -1;
    inner[0] = (uint8_t)(6 + naddr);
    inner[1] = 0x00; inner[2] = 0x0e; inner[3] = 0x04;
    put_u32(inner + 4, proto);
    memcpy(inner + 8, addr, naddr);
    return (int)(8 + naddr);
}

int mvci_inner_prog_voltage(uint32_t pin, uint32_t voltage, uint8_t *inner)
{
    if (!inner) return -1;
    inner[0] = 0x09; inner[1] = 0x00; inner[2] = 0x0d;
    put_u32(inner + 3, pin);
    put_u32(inner + 7, voltage);
    return 11;
}

/* Stop message filter. Wire shape is vendor-evidenced (T255 frames 504/522):
 * ILEN 00 0C [proto u32] [msgid u32], where msgid is the handle the earlier
 * start-filter command carried. */
int mvci_inner_stop_filter(uint32_t proto, uint32_t msgid, uint8_t *inner)
{
    if (!inner) return -1;
    inner[0] = 0x09; inner[1] = 0x00; inner[2] = 0x0c;
    put_u32(inner + 3, proto);
    put_u32(inner + 7, msgid);
    return 11;
}

int mvci_inner_write_msg(uint32_t proto, const uint8_t *msg, size_t n, uint8_t *inner)
{
    if (!inner || (n && !msg) || n > MVCI_MAX_WIRE_CIPHER - 11u) return -1;
    inner[0] = (uint8_t)(1 + 8 + n);
    inner[1] = 0x00; inner[2] = 0x0a;
    put_u32(inner + 3, proto);
    put_u32(inner + 7, 0);
    memcpy(inner + 11, msg, n);
    return (int)(11 + n);
}

int mvci_inner_read_poll(uint32_t proto, uint8_t *inner)
{
    if (!inner) return -1;
    inner[0] = 0x05; inner[1] = 0x00; inner[2] = 0x09;
    put_u32(inner + 3, proto);
    inner[7] = 0x00;
    return 8;
}

int mvci_parse_read_reply(const uint8_t *inner, int inner_len,
                          uint8_t *msg_out, size_t cap, uint8_t *rxstatus_out)
{
    if (!inner || inner_len < 4 || inner_len > MVCI_MAX_INNER || inner[2] != 0x09)
        return -1;
    int mlen = (int)inner[0] - 9;                  /* msg at offset 11 */
    if (mlen <= 0) {
        if (rxstatus_out) *rxstatus_out = inner[3];
        return 0;                                   /* status reply, no message */
    }
    if (inner_len < 11 || (size_t)mlen > (size_t)inner_len - 11u ||
        (size_t)mlen > cap || !msg_out)
        return -1;
    memcpy(msg_out, inner + 11, mlen);
    if (rxstatus_out) *rxstatus_out = inner[3];
    return mlen;
}

int mvci_five_baud_is_disconnected(const uint8_t *resp, int r)
{
    static const uint8_t disc[8] = { 0x02,0x00,0x0e,0x08,0x00,0x00,0x00,0x28 };
    if (r != 8 || !resp) return 0;
    return memcmp(resp, disc, 8) == 0;
}

int mvci_five_baud_is_corolla_success(const uint8_t *resp, int r)
{
    static const uint8_t success[8] = { 0x03,0x00,0x0e,0x08,0x08,0x00,0x00,0x00 };
    return r == 8 && resp && memcmp(resp, success, sizeof success) == 0;
}

int mvci_fast_init_is_timeout(const uint8_t *resp, int r)
{
    static const uint8_t disc[8] = { 0x02,0x00,0x0e,0x08,0x00,0x00,0x00,0xf0 };
    if (r != 8 || !resp) return 0;
    return memcmp(resp, disc, 8) == 0;
}

int mvci_prog_voltage_is_supported(uint32_t pin, uint32_t voltage)
{
    return pin == 15 && voltage == 0xFFFFFFFEu;
}

/* ======================================================================
 *  Session
 * ====================================================================== */

#define MVCI_ISO14230 4
/* Conservative host bound; physical poll-reply latency is not measured. */
#define MVCI_POLL_REPLY_MAX_MS 1000

struct mvci_ctx {
    mvci_io_t       *io;
    int              have_key;
    uint8_t          key[8];
    uint32_t         proto;      /* J2534 ProtocolID of the connected channel */
    uint8_t          pending[16][MVCI_MAX_INNER];
    int              pending_len[16];
    int              pending_count;
    uint8_t          rx_frame[MVCI_MAX_FRAME];
    int              rx_used;
    int              poisoned; /* Unknown outstanding result: close/reopen required. */
    int              poll_pending; /* Issued read poll awaiting its one result. */
    uint32_t         poll_sent_ms;
    int              overflow;
    int              rx_purge_failed;
    mvci_mutex_t     lock;       /* serialises device I/O */
    int              in_poll;    /* a read poll owns lock; only it honours cancel */
    mvci_mutex_t     cancel_lock; /* leaf lock: never held across any other call */
    int              cancel;     /* set by a waiting Close/Disconnect */
    mvci_thread_t    ka_thread;
    volatile int     ka_run;
};

/* set MVCI_DEBUG=1 to trace decrypted TX/RX inners on stderr */
static int dbg_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("MVCI_DEBUG"); v = (e && *e && *e != '0'); }
    return v;
}
static void dbg_hex(const char *tag, const uint8_t *b, int n)
{
    if (!dbg_on()) return;
    fprintf(stderr, "  %s", tag);
    for (int i = 0; i < n; i++) fprintf(stderr, " %02x", b[i]);
    fprintf(stderr, "\n");
}

mvci_ctx_t *mvci_open(const char *port)
{
    mvci_ctx_t *ctx = (mvci_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->io = mvci_io_open(port);
    if (!ctx->io) { free(ctx); return NULL; }
    ctx->proto = MVCI_ISO14230;      /* until PassThruConnect selects one */
    mvci_mutex_init(&ctx->lock);
    mvci_mutex_init(&ctx->cancel_lock);
    return ctx;
}

void mvci_close(mvci_ctx_t *ctx)
{
    if (!ctx) return;
    mvci_stop_keepalive(ctx);
    mvci_io_close(ctx->io);
    mvci_mutex_destroy(&ctx->lock);
    mvci_mutex_destroy(&ctx->cancel_lock);
    free(ctx);
}

const uint8_t *mvci_key(const mvci_ctx_t *ctx)
{
    return ctx->have_key ? ctx->key : NULL;
}

/* A read poll waits in slices of this length and checks for cancel between
 * them. The original caller deadline still ends the wait: a slice never
 * shortens the budget of a read that nobody cancels. */
#define MVCI_READ_SLICE_MS 20

void mvci_set_read_cancel(mvci_ctx_t *ctx, int on)
{
    if (!ctx) return;
    mvci_mutex_lock(&ctx->cancel_lock);
    ctx->cancel = on != 0;
    mvci_mutex_unlock(&ctx->cancel_lock);
}

static int cancel_requested(mvci_ctx_t *ctx)
{
    mvci_mutex_lock(&ctx->cancel_lock);
    int c = ctx->cancel;
    mvci_mutex_unlock(&ctx->cancel_lock);
    return c;
}

/* Retain fragments across nonblocking drains. A partial command reply at its
 * deadline poisons the transaction in the caller; never guess a new boundary. */
static int read_frame_until(mvci_ctx_t *ctx, uint8_t *buf, int cap,
                             uint32_t deadline, int timed, int may_wait)
{
    int empty_slices = 0, max_empty = 0;
    if (may_wait && ctx->in_poll && timed) {
        int r0 = (int32_t)(deadline - mvci_now_ms());
        max_empty = (r0 > 0 ? r0 / MVCI_READ_SLICE_MS : 0) + 2;
    }
    for (;;) {
        /* A positive transport read can still leave most of a frame buffered.
         * Stop between reads as well as after empty slices so Close/Disconnect
         * does not wait for an arbitrarily slow byte stream. */
        if (max_empty && cancel_requested(ctx)) return MVCI_PIPE_CANCELED;
        /* A nonblocking transport call can still take execution time. Keep
         * all bytes in rx_frame if it crosses the caller's deadline. */
        int remaining = timed ? (int32_t)(deadline - mvci_now_ms()) : 0;
        if (timed && remaining <= 0) return MVCI_PIPE_TIMEOUT;
        int target = ctx->rx_used ? ctx->rx_frame[0] : 1;
        if (target < 1 || (ctx->rx_used && target < 3) || target > cap)
            return MVCI_PIPE_DESYNC;
        if (ctx->rx_used == target && target > 1) {
            memcpy(buf, ctx->rx_frame, (size_t)target);
            ctx->rx_used = 0;
            return target;
        }
        int wait = may_wait ? remaining : 0;
        int sliced = max_empty && wait > MVCI_READ_SLICE_MS;
        if (sliced) wait = MVCI_READ_SLICE_MS;
        int n = mvci_io_read(ctx->io, ctx->rx_frame + ctx->rx_used,
                             target - ctx->rx_used, wait);
        if (n < 0) return MVCI_PIPE_IO;
        if (!n) {
            /* An empty slice is not the end of the caller's budget. The cap
             * keeps a transport that returns early from spinning forever. */
            if (max_empty && cancel_requested(ctx)) return MVCI_PIPE_CANCELED;
            if (sliced && ++empty_slices <= max_empty) continue;
            return 0;
        }
        if (n > target - ctx->rx_used) return MVCI_PIPE_DESYNC;
        ctx->rx_used += n;
    }
}

static int read_frame(mvci_ctx_t *ctx, uint8_t *buf, int cap, int timeout_ms)
{
    return read_frame_until(ctx, buf, cap, mvci_now_ms() + (uint32_t)timeout_ms,
                            timeout_ms > 0, timeout_ms > 0);
}

/* Decode into the full wire capacity, independently of a command's small ACK
 * buffer. Both unsolicited and solicited diagnostic records admit 1..64 bytes. */
static int receive_inner_until(mvci_ctx_t *ctx, uint8_t *inner,
                                uint32_t deadline, int timed, int may_wait)
{
    uint8_t frame[MVCI_MAX_FRAME];
    int n = read_frame_until(ctx, frame, sizeof frame, deadline, timed, may_wait);
    if (n <= 0) return n;
    int r = mvci_frame_decrypt(ctx->key, frame, n, inner, MVCI_MAX_INNER);
    if (r < 4 || inner[1] != 0) return MVCI_PIPE_DESYNC;
    return r;
}

static int read_record(const uint8_t *inner, int r)
{
    if (inner[2] != 0x09) return MVCI_PIPE_DESYNC;
    int n = (int)inner[0] - 9;
    if (n <= 0) {
        /* Zero is the mock/ordinary empty result, 0x10 is the captured empty
         * poll. Unknown result codes must not become an empty queue. */
        return r == 8 && inner[0] == 2 &&
               (inner[3] == 0 || inner[3] == 0x10) ? 0 : MVCI_PIPE_REJECTED;
    }
    return n <= MVCI_PIPE_MAX_DATA && 11 + n <= r ? n : MVCI_PIPE_DESYNC;
}

/* Drain completed empty records too: an empty status must not hide an
 * already-buffered diagnostic frame from a Timeout=0 caller. */
static int drain_record(mvci_ctx_t *ctx, uint8_t *inner, uint32_t deadline,
                         int timed, unsigned *work_left)
{
    while (*work_left) {
        int r = receive_inner_until(ctx, inner, deadline, timed, 0);
        if (r <= 0) return r;
        --*work_left;
        int n = read_record(inner, r);
        if (n < 0) return n;
        if (n > 0) return r;
    }
    return 0; /* Fairness limit: leave the next valid record for a later call. */
}

static int queue_record(mvci_ctx_t *ctx, const uint8_t *inner, int r)
{
    int n = read_record(inner, r);
    if (n <= 0) return n;
    if (ctx->pending_count == 16) {
        ctx->overflow = 1;
        return MVCI_PIPE_OVERFLOW;
    }
    int slot = ctx->pending_count++;
    memcpy(ctx->pending[slot], inner, (size_t)r);
    ctx->pending_len[slot] = r;
    return 0;
}

/* Resolve the one tracked poll before sending or purging anything else.
 * A caller deadline can expire without losing correlation. A truly missing
 * reply eventually poisons the session independently of short read budgets. */
static int resolve_poll_until(mvci_ctx_t *ctx, uint32_t deadline)
{
    uint8_t inner[MVCI_MAX_INNER];
    while (ctx->poll_pending) {
        uint32_t now = mvci_now_ms();
        if ((int32_t)(now - ctx->poll_sent_ms) > MVCI_POLL_REPLY_MAX_MS) {
            ctx->poisoned = 1;
            return MVCI_PIPE_DESYNC;
        }
        if ((int32_t)(deadline - now) <= 0) return MVCI_PIPE_TIMEOUT;
        uint32_t age_deadline = ctx->poll_sent_ms + MVCI_POLL_REPLY_MAX_MS + 1u;
        uint32_t wait_deadline = (int32_t)(deadline - age_deadline) > 0 ? age_deadline : deadline;
        int r = receive_inner_until(ctx, inner, wait_deadline, 1, 1);
        if (r > 0) {
            if ((int32_t)(mvci_now_ms() - ctx->poll_sent_ms) > MVCI_POLL_REPLY_MAX_MS) {
                ctx->poisoned = 1;
                return MVCI_PIPE_DESYNC;
            }
            ctx->poll_pending = 0;
            r = queue_record(ctx, inner, r);
            if (r < 0) { ctx->poisoned = 1; return r; }
            return 0;
        }
        if (r < 0 && r != MVCI_PIPE_TIMEOUT) {
            ctx->poisoned = 1;
            return r;
        }
        if (!r) mvci_sleep_ms(1); /* transport may return before its budget */
    }
    return 0;
}

/* The result is byte 3 in the supported ACK shapes. The final padded byte is
 * opaque: the captured successful stop-filter ACK ends in B8. Init replies
 * have separate exact classifiers and must never pass through this helper. */
static int ack_ok(const uint8_t *resp, int r, uint8_t opcode)
{
    return r == 8 && resp[0] == 2 && resp[1] == 0 &&
           resp[2] == opcode && resp[3] == 0;
}

/* reset + identify + read challenge -> install key. Caller must hold ctx->lock
 * when called after the keepalive thread is running. */
static int do_handshake(mvci_ctx_t *ctx)
{
    uint8_t frame[MVCI_MAX_FRAME];
    int n;

    ctx->have_key = 0;
    ctx->rx_used = 0;
    ctx->pending_count = 0;
    n = mvci_frame_plain(NULL, 0, frame, sizeof frame);     /* reset */
    if (mvci_io_purge_rx_checked(ctx->io) != 0) return -1;
    if (mvci_io_write(ctx->io, frame, n) != n) return -1;
    mvci_sleep_ms(110);
    if (mvci_io_purge_rx_checked(ctx->io) != 0) return -1;

    static const uint8_t ident[] = { 0x07, 0x00, 0x01, 'M', 'V', 'C', 'I', '-', 'T' };
    n = mvci_frame_plain(ident, sizeof ident, frame, sizeof frame);
    if (mvci_io_write(ctx->io, frame, n) != n) return -1;

    n = read_frame(ctx, frame, sizeof frame, 2000);
    if (n <= 0) return -1;

    const uint8_t *pl;
    int plen = mvci_frame_payload(frame, n, &pl);
    if (plen != 11 || pl[0] != 0x09 || pl[1] != 0 || pl[2] != 1) return -1;
    memcpy(ctx->key, pl + 3, 8);
    ctx->have_key = 1;
    return 0;
}

int mvci_handshake(mvci_ctx_t *ctx)
{
    /* Called from PassThruOpen before the keepalive thread starts — no lock. */
    return do_handshake(ctx);
}

/* Caller budgets include waiting for the I/O owner. A zero-timeout drain
 * never waits behind a command transaction. */
static int lock_until(mvci_ctx_t *ctx, uint32_t deadline, int may_wait, int cancellable)
{
    for (;;) {
        if (may_wait && (int32_t)(deadline - mvci_now_ms()) <= 0) return 0;
        if (cancellable && cancel_requested(ctx)) return -1;
#ifdef _WIN32
        if (TryEnterCriticalSection(&ctx->lock)) return 1;
#else
        if (pthread_mutex_trylock(&ctx->lock) == 0) return 1;
#endif
        int remaining = (int32_t)(deadline - mvci_now_ms());
        if (!may_wait || remaining <= 0) return 0;
        mvci_sleep_ms(1);
    }
}

/* One outstanding command only. Never retry after losing its result. There
 * is no sequence field on this wire; a purge or quiet interval cannot prove
 * that a late same-opcode ACK is gone. A poisoned context requires Close/Open.
 * Preflight rejects already-buffered unsolicited ACKs before sending anything.
 * Arbitrarily delayed duplicate ACKs after a completed command still require
 * physical/protocol evidence; this code does not claim to disambiguate them. */
static int transact_until_locked(mvci_ctx_t *ctx, const uint8_t *inner, size_t inner_len,
                                 uint8_t *resp_inner, size_t resp_cap, uint32_t deadline,
                                 int cancellable)
{
    if (ctx->rx_purge_failed) return MVCI_PIPE_IO;
    if (ctx->poisoned) return MVCI_PIPE_DESYNC;
    if (ctx->poll_pending) {
        int r = resolve_poll_until(ctx, deadline);
        if (r != 0) return r;
    }
    if ((int32_t)(deadline - mvci_now_ms()) <= 0) return MVCI_PIPE_TIMEOUT;
    uint8_t frame[MVCI_MAX_FRAME], received[MVCI_MAX_INNER];
    int n = mvci_frame_enc(ctx->key, inner, inner_len, frame, sizeof frame);
    if (n < 0) return MVCI_PIPE_DESYNC;
    /* Bound an unsolicited burst. Overflow is explicit and fatal to command
     * ordering; retained records remain available after the loss report. */
    for (int i = 0; ; i++) {
        int r = receive_inner_until(ctx, received, deadline, 1, 0);
        if (r < 0) {
            if (r != MVCI_PIPE_TIMEOUT) ctx->poisoned = 1;
            return r;
        }
        if (!r) break;
        r = queue_record(ctx, received, r);
        if (r < 0 || i >= 32) {
            ctx->poisoned = 1;
            return r < 0 ? r : MVCI_PIPE_DESYNC;
        }
        if ((int32_t)(deadline - mvci_now_ms()) <= 0) return MVCI_PIPE_TIMEOUT;
    }
    if (ctx->rx_used) { ctx->poisoned = 1; return MVCI_PIPE_DESYNC; }
    /* This check is the cancellation/send boundary. Do not hold cancel_lock
     * across transport I/O: Close/Disconnect must be able to mark the read
     * canceled while a transport call is in progress. A cancellation ordered
     * after this boundary may race the write and is treated as post-send. */
    if (cancellable && cancel_requested(ctx)) return MVCI_PIPE_CANCELED;
    int remaining = (int32_t)(deadline - mvci_now_ms());
    if (remaining <= 0) return MVCI_PIPE_TIMEOUT;
    dbg_hex("TX inner:", inner, (int)inner_len);
    int w = mvci_io_write_timeout(ctx->io, frame, n, remaining);
    if (w != n) {
        ctx->poisoned = 1; /* A partial write may have reached the adapter. */
        return w < 0 ? MVCI_PIPE_IO : MVCI_PIPE_TIMEOUT;
    }
    for (;;) {
        remaining = (int32_t)(deadline - mvci_now_ms());
        int r = remaining > 0 ? receive_inner_until(ctx, received, deadline, 1, 1) : 0;
        if (r <= 0) {
            ctx->poisoned = 1;
            return r < 0 ? r : MVCI_PIPE_TIMEOUT;
        }
        dbg_hex("RX inner:", received, r);
        if (received[2] == inner[2]) {
            int init_reply = inner[2] == 0x0e && inner_len >= 4 &&
                             (inner[3] == 4 || inner[3] == 5);
            if (!init_reply && inner[2] != 9 &&
                (r != 8 || received[0] != 2)) {
                ctx->poisoned = 1;
                return MVCI_PIPE_DESYNC;
            }
            if ((int32_t)(deadline - mvci_now_ms()) < 0) {
                ctx->poisoned = 1;
                return MVCI_PIPE_TIMEOUT;
            }
            if ((size_t)r > resp_cap) { ctx->poisoned = 1; return MVCI_PIPE_DESYNC; }
            memcpy(resp_inner, received, (size_t)r);
            return r;
        }
        int q = queue_record(ctx, received, r);
        if (q < 0) { ctx->poisoned = 1; return q; }
    }
}

static int transact_locked(mvci_ctx_t *ctx, const uint8_t *inner, size_t inner_len,
                           uint8_t *resp_inner, size_t resp_cap, int timeout_ms)
{
    if (timeout_ms <= 0) return MVCI_PIPE_TIMEOUT;
    return transact_until_locked(ctx, inner, inner_len, resp_inner, resp_cap,
                                 mvci_now_ms() + (uint32_t)timeout_ms, 0);
}

int mvci_transact(mvci_ctx_t *ctx, const uint8_t *inner, size_t inner_len,
                  uint8_t *resp_inner, size_t resp_cap, int timeout_ms)
{
    if (!ctx || !ctx->have_key || !inner || inner_len < 3 ||
        !resp_inner || timeout_ms < 0) return MVCI_PIPE_DESYNC;
    uint32_t started = mvci_now_ms();
    if (!lock_until(ctx, started + (uint32_t)timeout_ms, timeout_ms > 0, 0))
        return MVCI_PIPE_TIMEOUT;
    int elapsed = (int32_t)(mvci_now_ms() - started);
    timeout_ms = elapsed >= timeout_ms ? 0 : timeout_ms - elapsed;
    int r = transact_locked(ctx, inner, inner_len, resp_inner, resp_cap, timeout_ms);
    mvci_mutex_unlock(&ctx->lock);
    return r;
}

/* ---- keepalive thread ----------------------------------------------- */

int mvci_keepalive(mvci_ctx_t *ctx)
{
    static const uint8_t ka[8] = { 0x05, 0x00, 0x09, 0x06, 0x00, 0x00, 0x00, 0x00 };
    uint8_t resp[32];
    mvci_mutex_lock(&ctx->lock);
    int pending = ctx->poll_pending;
    mvci_mutex_unlock(&ctx->lock);
    if (pending) return 0; /* Let the next foreground call resolve the poll. */
    int r = mvci_transact(ctx, ka, sizeof ka, resp, sizeof resp, 500);
    return ack_ok(resp, r, 0x09) ? 0 : -1;
}

static MVCI_THREAD_RET keepalive_thread(void *arg)
{
    mvci_ctx_t *ctx = (mvci_ctx_t *)arg;
    while (ctx->ka_run) {
        mvci_sleep_ms(15);
        if (ctx->ka_run) mvci_keepalive(ctx);
    }
    return 0;
}

int mvci_start_keepalive(mvci_ctx_t *ctx)
{
    if (ctx->ka_run) return 0;
    ctx->ka_run = 1;
    if (mvci_thread_create(&ctx->ka_thread, keepalive_thread, ctx) != 0) {
        ctx->ka_run = 0;
        return -1;
    }
    return 0;
}

void mvci_stop_keepalive(mvci_ctx_t *ctx)
{
    if (!ctx->ka_run) return;
    ctx->ka_run = 0;
    mvci_thread_join(ctx->ka_thread);
}

/* ---- higher level J2534 operations ---------------------------------- */

int mvci_connect(mvci_ctx_t *ctx, uint32_t proto, uint32_t flags, uint32_t baud)
{
    uint8_t in[32], resp[64];
    int n = mvci_inner_connect(proto, flags, baud, in);

    mvci_mutex_lock(&ctx->lock);
    int r = transact_locked(ctx, in, n, resp, sizeof resp, 2000);
    int ok = ack_ok(resp, r, 0x07);

    /* Do not retry a missing/rejected Connect result. A missing ACK has no
     * safe same-session correlation. Lifecycle recovery must establish its
     * reset boundary before issuing a fresh Connect, or require Close/Open. */
    if (ok) ctx->proto = proto;
    mvci_mutex_unlock(&ctx->lock);
    return ok ? 0 : -1;
}

int mvci_disconnect(mvci_ctx_t *ctx)
{
    static const uint8_t in[8] = { 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };
    /* The device answers Disconnect in plaintext (T254 frame 922 / T255
     * frame 560: 07 00 02 00 02 00 07), which is not DES-decryptable, so a
     * normal transact cannot read it. Validate the raw bytes exactly. */
    static const uint8_t exp[7] = { 0x07, 0x00, 0x02, 0x00, 0x02, 0x00, 0x07 };
    uint8_t frame[MVCI_MAX_FRAME], rx[sizeof exp];
    int n = mvci_frame_enc(ctx->key, in, sizeof in, frame, sizeof frame);
    if (!ctx->have_key || n < 0) return -1;
    mvci_mutex_lock(&ctx->lock);
    /* Do not purge a tracked encrypted reply. The teardown budget is bounded;
     * a missing reply leaves the session for Close/Open recovery. */
    if (ctx->poll_pending && resolve_poll_until(ctx, mvci_now_ms() + 1000) != 0) {
        mvci_mutex_unlock(&ctx->lock);
        return -1;
    }
    if (mvci_io_purge_rx_checked(ctx->io) != 0) {
        ctx->rx_purge_failed = 1;
        mvci_mutex_unlock(&ctx->lock);
        return -1;
    }
    ctx->rx_purge_failed = 0;
    int ok = -1;
    if (mvci_io_write(ctx->io, frame, n) == n) {
        int got = 0;
        uint32_t deadline = mvci_now_ms() + 1000;
        while (got < (int)sizeof exp && (int32_t)(deadline - mvci_now_ms()) > 0) {
            int r = mvci_io_read(ctx->io, rx + got, (int)sizeof exp - got, 200);
            if (r < 0) break;
            if (r > 0) got += r;
            else mvci_sleep_ms(1);
        }
        if (got == (int)sizeof exp && memcmp(rx, exp, sizeof exp) == 0) ok = 0;
    }
    /* Any attempted write without the exact reply has an unknown result.
     * Short writes and transport errors can leave part of the command on the
     * adapter, just as in transact_until_locked. A later reply cannot repair
     * the ambiguity: the context needs Close/Open. */
    if (ok != 0) ctx->poisoned = 1;
    if (ok == 0) {
        /* The exact Disconnect reply is the clean session boundary. Frames
         * queued from the old session must not reach the next one. An
         * ambiguous earlier command stays poisoned: only Close/Open clears it. */
        ctx->pending_count = 0;
        ctx->rx_used = 0;
        ctx->overflow = 0;
        ctx->poll_pending = 0;
    }
    mvci_mutex_unlock(&ctx->lock);
    return ok;
}

int mvci_start_filter(mvci_ctx_t *ctx, uint32_t msgid, uint8_t mask, uint8_t pattern)
{
    uint8_t in[32], resp[64];
    int n = mvci_inner_start_filter(ctx->proto, msgid, 1 /*PASS*/, mask, pattern, in);
    int r = mvci_transact(ctx, in, n, resp, sizeof resp, 2000);
    return ack_ok(resp, r, 0x0b) ? 0 : -1;
}

int mvci_stop_filter(mvci_ctx_t *ctx, uint32_t msgid)
{
    uint8_t in[16], resp[64];
    int n = mvci_inner_stop_filter(ctx->proto, msgid, in);
    int r = mvci_transact(ctx, in, n, resp, sizeof resp, 2000);
    return ack_ok(resp, r, 0x0c) ? 0 : -1;
}

int mvci_set_config(mvci_ctx_t *ctx, uint32_t param, uint32_t value)
{
    uint8_t in[16], resp[64];
    mvci_inner_set_config(ctx->proto, param, value, in);
    int r = mvci_transact(ctx, in, sizeof in, resp, sizeof resp, 2000);
    /* A nonzero cable status is a failure, not an applied setting. Even a
     * success ACK proves command acceptance only, not electrical timing. */
    return ack_ok(resp, r, 0x0e) ? 0 : -1;
}

int mvci_set_prog_voltage(mvci_ctx_t *ctx, uint32_t pin, uint32_t voltage)
{
    uint8_t in[16], resp[64];
    int n = mvci_inner_prog_voltage(pin, voltage, in);
    if (n < 0) return -1;
    int r = mvci_transact(ctx, in, (size_t)n, resp, sizeof resp, 2000);
    return ack_ok(resp, r, 0x0d) ? 0 : -1;
}

int mvci_five_baud_init(mvci_ctx_t *ctx, const uint8_t *addr, size_t naddr,
                        uint8_t *out, size_t cap)
{
    /* Sized for the constructor's full accepted range (8 + up to 248 address
     * bytes), so an in-range input can never overrun this buffer. */
    uint8_t in[MVCI_MAX_INNER], resp[64];
    int ilen = mvci_inner_five_baud_init(ctx->proto, addr, naddr, in);
    if (ilen < 0) return -1;
    mvci_sleep_ms(2000);
    int r = mvci_transact(ctx, in, (size_t)ilen, resp, sizeof resp, 5000);
    if (mvci_five_baud_is_disconnected(resp, r)) return MVCI_FB_NOT_CONNECTED;
    if (!mvci_five_baud_is_corolla_success(resp, r)) return -1;
    if (!out || cap < 2) return -1;
    out[0] = resp[3];
    out[1] = resp[4];
    return 2;
}

int mvci_clear_periodic(mvci_ctx_t *ctx)
{
    uint8_t in[8], resp[64];
    int n = mvci_inner_clear_periodic(ctx->proto, in);
    int r = mvci_transact(ctx, in, n, resp, sizeof resp, 1000);
    return ack_ok(resp, r, 0x0e) ? 0 : -1;
}

int mvci_fast_init(mvci_ctx_t *ctx, const uint8_t *init, size_t n,
                   uint8_t *resp_out, size_t cap)
{
    uint8_t in[MVCI_MAX_INNER], resp[64];
    if (!ctx || !resp_out) return -1;
    int ilen = mvci_inner_fast_init(ctx->proto, init, n, in);
    if (ilen < 0) return -1;
    int r = mvci_transact(ctx, in, (size_t)ilen, resp, sizeof resp, 3000);
    if (r < 3 || resp[2] != 0x0e) return -1;
    if (mvci_fast_init_is_timeout(resp, r)) return MVCI_FI_TIMEOUT;
    if (resp[0] == 0x02) {
        dbg_hex("fast init: status reply, fail closed:", resp, r);
        return -1;
    }
    int mlen = (int)resp[0] - 1;                 /* 08 -> 7 ECU key bytes @ off 3 */
    if (mlen <= 0 || 3 + mlen > r) return -1;
    if ((size_t)mlen > cap) return -1;
    if (mlen > 0) memcpy(resp_out, resp + 3, (size_t)mlen);
    return mlen;
}

int mvci_write_msg_timeout(mvci_ctx_t *ctx, const uint8_t *msg, size_t n, int timeout_ms)
{
    uint8_t in[MVCI_MAX_INNER], resp[8];
    if (!msg || !n || n > MVCI_PIPE_MAX_DATA) return MVCI_PIPE_DESYNC;
    int ilen = mvci_inner_write_msg(ctx->proto, msg, n, in);
    if (ilen < 0) return MVCI_PIPE_DESYNC;
    int r = mvci_transact(ctx, in, ilen, resp, sizeof resp, timeout_ms);
    if (r < 0) return r;
    return ack_ok(resp, r, 0x0a) ? 0 : MVCI_PIPE_REJECTED;
}

int mvci_write_msg(mvci_ctx_t *ctx, const uint8_t *msg, size_t n)
{
    return mvci_write_msg_timeout(ctx, msg, n, 1000) == 0 ? 0 : -1;
}

int mvci_poll_until(mvci_ctx_t *ctx, uint8_t *msg_out, size_t cap,
                    uint8_t *rxstatus_out, uint32_t deadline, int timed,
                    unsigned *work_left)
{
    uint8_t in[8], resp[MVCI_MAX_INNER];
    if (!ctx || !ctx->have_key || !msg_out || !work_left) return MVCI_PIPE_DESYNC;
    int got = lock_until(ctx, deadline, timed, 1);
    if (got < 0) return MVCI_PIPE_CANCELED;   /* nothing sent, nothing lost */
    if (!got) return MVCI_PIPE_TIMEOUT;
    int r, from_pending = 0;
    ctx->in_poll = 1;
    if (cancel_requested(ctx)) {
        r = MVCI_PIPE_CANCELED;               /* before any command: no poison */
    } else if (ctx->rx_purge_failed) {
        r = MVCI_PIPE_IO;
    } else if (ctx->overflow) {
        ctx->overflow = 0;
        r = MVCI_PIPE_OVERFLOW;
    } else if (!*work_left) {
        r = 0;
    } else if (ctx->pending_count && !ctx->poll_pending) {
        r = ctx->pending_len[0];
        memcpy(resp, ctx->pending[0], (size_t)r);
        from_pending = 1; /* Commit removal only within the caller deadline. */
        --*work_left;
    } else if (ctx->poisoned) {
        r = MVCI_PIPE_DESYNC;
    } else {
        /* Once a poll is sent, its next record belongs to that poll. Do not
         * drain it as unsolicited input across a public read deadline. */
        r = ctx->poll_pending ? 0 : drain_record(ctx, resp, deadline, timed, work_left);
        if (!r && timed && *work_left && !ctx->rx_used && !ctx->poll_pending) {
            int n = mvci_inner_read_poll(ctx->proto, in);
            int remaining = (int32_t)(deadline - mvci_now_ms());
            if (cancel_requested(ctx)) r = MVCI_PIPE_CANCELED;
            else if (remaining <= 0) r = MVCI_PIPE_TIMEOUT;
            else {
                uint8_t frame[MVCI_MAX_FRAME];
                int length = mvci_frame_enc(ctx->key, in, n, frame, sizeof frame);
                if (length < 0) r = MVCI_PIPE_DESYNC;
                else {
                    dbg_hex("TX inner:", in, n);
                    int w = mvci_io_write_timeout(ctx->io, frame, length, remaining);
                    if (w == length) {
                        ctx->poll_pending = 1;
                        ctx->poll_sent_ms = mvci_now_ms();
                    }
                    else { ctx->poisoned = 1; r = w < 0 ? MVCI_PIPE_IO : MVCI_PIPE_TIMEOUT; }
                }
            }
        }
        if (!r && ctx->poll_pending && *work_left) {
            if ((int32_t)(mvci_now_ms() - ctx->poll_sent_ms) > MVCI_POLL_REPLY_MAX_MS)
                r = MVCI_PIPE_DESYNC;
            else r = receive_inner_until(ctx, resp, deadline, timed, timed);
            if (r > 0) {
                if ((int32_t)(mvci_now_ms() - ctx->poll_sent_ms) > MVCI_POLL_REPLY_MAX_MS)
                    r = MVCI_PIPE_DESYNC;
                else {
                    ctx->poll_pending = 0;
                    --*work_left;
                    dbg_hex("RX inner:", resp, r);
                }
            }
        } else if (!r && timed && *work_left && ctx->rx_used) {
            r = receive_inner_until(ctx, resp, deadline, 1, 1);
            if (r > 0) --*work_left;
        }
        /* A fully sent poll keeps its exact next-reply correlation across a
         * read deadline or cancellation. Partial writes already poison. */
        if (ctx->poll_pending &&
            (int32_t)(mvci_now_ms() - ctx->poll_sent_ms) > MVCI_POLL_REPLY_MAX_MS) {
            ctx->poisoned = 1;
            r = MVCI_PIPE_DESYNC;
        }
        if (r < 0 && r != MVCI_PIPE_TIMEOUT && r != MVCI_PIPE_CANCELED)
            ctx->poisoned = 1;
    }
    if (r > 0) {
        int n = read_record(resp, r);
        if (n < 0) { ctx->poisoned = 1; r = n; }
        else if (timed && (int32_t)(deadline - mvci_now_ms()) <= 0) {
            int q = n && !from_pending ? queue_record(ctx, resp, r) : 0;
            if (q < 0) ctx->poisoned = 1;
            r = q < 0 ? q : MVCI_PIPE_TIMEOUT;
        }
        else if ((size_t)n > cap) r = MVCI_PIPE_OVERFLOW;
        else {
            if (from_pending) {
                ctx->pending_count--;
                memmove(ctx->pending, ctx->pending + 1, (size_t)ctx->pending_count * sizeof ctx->pending[0]);
                memmove(ctx->pending_len, ctx->pending_len + 1, (size_t)ctx->pending_count * sizeof ctx->pending_len[0]);
            }
            r = n ? mvci_parse_read_reply(resp, r, msg_out, cap, rxstatus_out) : 0;
        }
    } else if (!r && timed && (int32_t)(deadline - mvci_now_ms()) <= 0) {
        r = MVCI_PIPE_TIMEOUT;
    }
    ctx->in_poll = 0;
    mvci_mutex_unlock(&ctx->lock);
    return r;
}

int mvci_poll(mvci_ctx_t *ctx, uint8_t *msg_out, size_t cap,
              uint8_t *rxstatus_out, int timeout_ms)
{
    if (timeout_ms < 0) return MVCI_PIPE_DESYNC;
    unsigned work_left = MVCI_PIPE_READ_BUDGET;
    return mvci_poll_until(ctx, msg_out, cap, rxstatus_out,
                           mvci_now_ms() + (uint32_t)timeout_ms,
                           timeout_ms > 0, &work_left);
}

int mvci_reset_session(mvci_ctx_t *ctx)
{
    if (!ctx || !ctx->have_key) return MVCI_PIPE_DESYNC;
    mvci_mutex_lock(&ctx->lock);
    int r = 0;
    if (ctx->poisoned) {
        r = MVCI_PIPE_DESYNC;
    } else if (ctx->poll_pending &&
               (r = resolve_poll_until(ctx, mvci_now_ms() + 1000)) != 0) {
        /* No purge before the outstanding reply has been classified. */
    } else if (mvci_io_purge_rx_checked(ctx->io) != 0) {
        ctx->rx_purge_failed = 1;
        r = MVCI_PIPE_IO;
    } else {
        ctx->rx_purge_failed = 0;
        ctx->pending_count = 0;
        ctx->rx_used = 0;
        ctx->overflow = 0;
    }
    mvci_mutex_unlock(&ctx->lock);
    return r;
}

int mvci_clear_rx_buffer(mvci_ctx_t *ctx)
{
    if (!ctx || !ctx->have_key) return -1;
    mvci_mutex_lock(&ctx->lock);
    if (ctx->poll_pending && resolve_poll_until(ctx, mvci_now_ms() + 1000) != 0) {
        mvci_mutex_unlock(&ctx->lock);
        return -1;
    }
    if (mvci_io_purge_rx_checked(ctx->io) != 0) {
        ctx->rx_purge_failed = 1;
        mvci_mutex_unlock(&ctx->lock);
        return -1;
    }
    ctx->rx_purge_failed = 0;
    ctx->pending_count = 0;
    ctx->rx_used = 0;
    ctx->overflow = 0;
    /* Clearing host RX is not an ACK/session recovery boundary. */
    mvci_mutex_unlock(&ctx->lock);
    return 0;
}

int mvci_read_msg(mvci_ctx_t *ctx, uint8_t *msg_out, size_t cap, int timeout_ms)
{
    uint8_t rx = 0;
    int m = mvci_poll(ctx, msg_out, cap, &rx, timeout_ms);
    if (m < 0) return m;
    if (m > 0 && rx == 0x00) return m;       /* data */
    return 0;                                 /* echo / none */
}

int mvci_obd_request(mvci_ctx_t *ctx, uint8_t addr, uint8_t pid,
                     uint8_t *out, size_t cap, int timeout_ms)
{
    mvci_clear_periodic(ctx);                 /* arm a fresh K-line transaction */

    uint8_t msg[5] = { 0x82, addr, 0xf0, 0x01, pid };
    if (mvci_write_msg(ctx, msg, sizeof msg) != 0) return -1;

    uint32_t deadline = mvci_now_ms() + (uint32_t)timeout_ms;
    while ((int32_t)(deadline - mvci_now_ms()) > 0) {
        int m = mvci_read_msg(ctx, out, cap, 500);
        if (m > 0) return m;
        mvci_sleep_ms(20);
    }
    return -1;
}
