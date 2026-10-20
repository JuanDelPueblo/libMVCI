/* mock_io.c — in-memory scripted Mini-VCI device for the offline sequence test.
 *
 * Implements the mvci_io.h transport API without hardware. It records every
 * written frame, answers the plaintext handshake with a fixed session key,
 * and synthesizes vendor-evidenced replies (T254/T255 bytes) for the
 * characterized commands. Anything else gets a generic status echo.
 *
 * Linked into mvci_seq_test instead of the real src/io.c. Never linked into
 * the driver library or the hardware-capable mvci_test binary.
 */

#include "io.h"

#include <mvci/serial.h>

#include <stdlib.h>
#include <string.h>

/* Fixed session key issued in the mock challenge. */
static const uint8_t MOCK_KEY[8] = { 0xb0, 0xcb, 0x49, 0x68, 0x07, 0x45, 0xc8, 0x7f };

#define MOCK_MAX_FRAMES 1024
#define MOCK_RAW_MAX    259
#define MOCK_QUEUE      4096

struct mvci_io {
    int dummy;
};

static struct mvci_io g_io;
static uint8_t g_written[MOCK_MAX_FRAMES][MOCK_RAW_MAX];
static int g_written_len[MOCK_MAX_FRAMES];
static int g_nwritten;
static uint8_t g_asm[MOCK_RAW_MAX + 1];
static int g_asmlen;
static uint8_t g_queue[MOCK_QUEUE];
static int g_qhead, g_qtail;
static int g_async_reply;
static uint32_t g_protocol;
static int g_poll_reply, g_device_reply_pending;
static uint8_t g_config_status;
static int g_five_baud_success;
static int g_fast_reply_len;
static int g_fast_reply_enabled;
static int g_handshake_shape;
void mock_handshake_shape(int mode) { g_handshake_shape = mode; }

void mock_async_reply(int mode) { g_async_reply = mode; }
void mock_poll_reply(void) { g_poll_reply = 1; }
void mock_config_status(uint8_t status) { g_config_status = status; }
void mock_five_baud_success(int enabled) { g_five_baud_success = enabled; }
void mock_fast_reply_length(int length)
{
    g_fast_reply_len = length;
    g_fast_reply_enabled = length >= 0 && length <= MVCI_MAX_INNER - 3;
}

const uint8_t *mock_key(void) { return MOCK_KEY; }

int mock_frame_count(void) { return g_nwritten; }

int mock_frame_bytes(int idx, uint8_t *out, int cap)
{
    if (idx < 0 || idx >= g_nwritten || !out || cap <= 0) return -1;
    if (g_written_len[idx] > cap) return -1;
    memcpy(out, g_written[idx], (size_t)g_written_len[idx]);
    return g_written_len[idx];
}

static void queue_bytes(const uint8_t *b, int n)
{
    if (n <= 0) return;
    if (g_qtail + n > (int)sizeof g_queue) return;   /* test-only: drop on overflow */
    memcpy(g_queue + g_qtail, b, (size_t)n);
    g_qtail += n;
}

/* Vendor-evidenced status inners (T254/T255 captures under this same key). */
static void queue_status(const uint8_t inner[8])
{
    uint8_t frame[MVCI_MAX_FRAME];
    int n = mvci_frame_enc(MOCK_KEY, inner, 8, frame, sizeof frame);
    if (n > 0) queue_bytes(frame, n);
}

static void queue_pid03_reply(void)
{
    static const uint8_t msg[] = {
        0x10,0x00,0x09,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x48,0x6b,0x10,0x49,0x03,0x04,0x13
    };
    uint8_t frame[MVCI_MAX_FRAME];
    int n = mvci_frame_enc(MOCK_KEY, msg, sizeof msg, frame, sizeof frame);
    if (n > 0) queue_bytes(frame, n);
}

void mock_rx_record(const uint8_t *data, int len, uint8_t status)
{
    uint8_t inner[80] = {0}, frame[MVCI_MAX_FRAME];
    if (len <= 0 || len > 64) return;
    inner[0] = (uint8_t)(len + 9);
    inner[2] = 0x09;
    inner[3] = status;
    memcpy(inner + 11, data, (size_t)len);
    int n = mvci_frame_enc(MOCK_KEY, inner, (size_t)(len + 11), frame, sizeof frame);
    if (n > 0) queue_bytes(frame, n);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void answer(const uint8_t *inner, int ilen)
{
    uint8_t st[8];
    if (ilen < 4) return;
    if (inner[2] == 0x0a && g_poll_reply) {
        static const uint8_t ack[8] = { 0x02,0x00,0x0a,0x00,0x00,0x00,0x00,0x00 };
        g_device_reply_pending = 1;
        g_poll_reply = 0;
        queue_status(ack);
        return;
    }
    if (inner[2] == 0x09 && ilen >= 7 && g_device_reply_pending
            && get_u32(inner + 3) == g_protocol) {
        g_device_reply_pending = 0;
        queue_pid03_reply();
        return;
    }
    if (inner[2] == 0x0a && g_async_reply) {
        /* Synthetic arrival ordering with the captured T002 PID03 payload.
         * The wire message layout is the existing codec's read-reply shape. */
        static const uint8_t msg[] = {
            0x10,0x00,0x09,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0x48,0x6b,0x10,0x49,0x03,0x04,0x13
        };
        static const uint8_t ack[8] = { 0x02,0x00,0x0a,0x00,0x00,0x00,0x00,0x00 };
        uint8_t frame[MVCI_MAX_FRAME];
        int n = mvci_frame_enc(MOCK_KEY, msg, sizeof msg, frame, sizeof frame);
        if (g_async_reply == 1 && n > 0) queue_bytes(frame, n);
        queue_status(ack);
        if (g_async_reply == 2 && n > 0) queue_bytes(frame, n);
        g_async_reply = 0;
        return;
    }
    if (ilen >= 4 && inner[2] == 0x07) {
        if (ilen >= 7) g_protocol = get_u32(inner + 3);
        static const uint8_t r[8] = { 0x02, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00 };
        memcpy(st, r, 8);
    } else if (ilen >= 4 && inner[2] == 0x0b) {
        static const uint8_t r[8] = { 0x02, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00 };
        memcpy(st, r, 8);
    } else if (ilen >= 4 && inner[2] == 0x0d) {
        static const uint8_t r[8] = { 0x02, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00 };
        memcpy(st, r, 8);
    } else if (ilen >= 4 && inner[2] == 0x0c) {
        /* Exact T255 stop-filter reply bytes. */
        static const uint8_t r[8] = { 0x02, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0xb8 };
        memcpy(st, r, 8);
    } else if (ilen >= 4 && inner[2] == 0x0e && inner[3] == 0x04) {
        if (g_five_baud_success) {
            static const uint8_t r[8] = { 0x03, 0x00, 0x0e, 0x08, 0x08, 0x00, 0x00, 0x00 };
            memcpy(st, r, 8);
            g_five_baud_success = 0;
        } else {
            static const uint8_t r[8] = { 0x02, 0x00, 0x0e, 0x08, 0x00, 0x00, 0x00, 0x28 };
            memcpy(st, r, 8);
        }
    } else if (ilen >= 4 && inner[2] == 0x0e && inner[3] == 0x05) {
        if (g_fast_reply_enabled) {
            uint8_t r[MVCI_MAX_INNER] = {0};
            size_t n = (size_t)g_fast_reply_len + 3;
            r[0] = (uint8_t)(g_fast_reply_len + 1); r[2] = 0x0e;
            for (int i = 0; i < g_fast_reply_len; i++) r[3 + i] = (uint8_t)(0x80 + i);
            uint8_t frame[MVCI_MAX_FRAME];
            int fn = mvci_frame_enc(MOCK_KEY, r, n, frame, sizeof frame);
            if (fn > 0) queue_bytes(frame, fn);
            g_fast_reply_len = 0;
            g_fast_reply_enabled = 0;
            return;
        }
        /* Exact T255 OBD-disconnected fast-init reply. */
        static const uint8_t r[8] = { 0x02, 0x00, 0x0e, 0x08, 0x00, 0x00, 0x00, 0xf0 };
        memcpy(st, r, 8);
    } else if (ilen >= 4 && inner[2] == 0x0e) {
        static const uint8_t r[8] = { 0x02, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00 };
        memcpy(st, r, 8);
        st[3] = g_config_status;
        g_config_status = 0;
    } else if (ilen >= 4 && inner[2] == 0x02) {
        /* Disconnect: the real device answers in plaintext (T255 frame 560). */
        static const uint8_t pl[4] = { 0x02, 0x00, 0x02, 0x00 };
        uint8_t frame[MVCI_MAX_FRAME];
        int n = mvci_frame_plain(pl, sizeof pl, frame, sizeof frame);
        if (n > 0) queue_bytes(frame, n);
        return;
    } else {
        st[0] = 0x02; st[1] = 0x00; st[2] = inner[2];
        st[3] = 0x00; st[4] = 0x00; st[5] = 0x00; st[6] = 0x00; st[7] = 0x00;
    }
    queue_status(st);
}

static void on_frame(const uint8_t *f, int n)
{
    if (g_nwritten < MOCK_MAX_FRAMES && n <= MOCK_RAW_MAX) {
        memcpy(g_written[g_nwritten], f, (size_t)n);
        g_written_len[g_nwritten] = n;
        g_nwritten++;
    }
    if (n == 3 && f[0] == 0x03) return;   /* reset: no reply */
    if (n == 12 && f[2] == 0x07 && f[3] == 0x00 && f[4] == 0x01) {
        /* identify: issue the fixed challenge */
        uint8_t pl[12] = { 0x09, 0x00, 0x01,
                            MOCK_KEY[0], MOCK_KEY[1], MOCK_KEY[2], MOCK_KEY[3],
                            MOCK_KEY[4], MOCK_KEY[5], MOCK_KEY[6], MOCK_KEY[7] };
        uint8_t frame[MVCI_MAX_FRAME];
        size_t plen = 11;
        if (g_handshake_shape == 1) pl[1] = 0x7f;
        if (g_handshake_shape == 2) pl[2] = 0x7f;
        if (g_handshake_shape == 3) plen = 10;
        if (g_handshake_shape == 4) plen = 12;
        if (g_handshake_shape == 5) pl[0] = 8;
        g_handshake_shape = 0;
        int m = mvci_frame_plain(pl, plen, frame, sizeof frame);
        if (m > 0) queue_bytes(frame, m);
        return;
    }
    /* Encrypted command: decrypt with the issued key and answer. */
    if (n >= 3 && ((n - 3) % 8) == 0) {
        uint8_t inner[MVCI_MAX_INNER];
        uint8_t tmp[MOCK_RAW_MAX];
        int plen = n - 3;
        memcpy(tmp, f + 2, (size_t)plen);
        mvci_des_decrypt(MOCK_KEY, tmp, (size_t)plen / 8);
        memcpy(inner, tmp, (size_t)plen);
        answer(inner, plen);
    }
}

mvci_io_t *mvci_io_open(const char *port)
{
    (void)port;
    g_nwritten = 0;
    g_asmlen = 0;
    g_qhead = g_qtail = 0;
    g_async_reply = 0;
    g_config_status = 0;
    g_five_baud_success = 0;
    g_fast_reply_len = 0;
    g_fast_reply_enabled = 0;
    g_protocol = 0;
    g_poll_reply = g_device_reply_pending = 0;
    return &g_io;
}

void mvci_io_close(mvci_io_t *io)
{
    (void)io;
}

int mvci_io_write(mvci_io_t *io, const uint8_t *buf, int n)
{
    (void)io;
    if (!buf || n <= 0) return -1;
    for (int i = 0; i < n; i++) {
        if (g_asmlen < (int)sizeof g_asm) g_asm[g_asmlen++] = buf[i];
        if (g_asmlen >= 1 && g_asm[0] < 3) { g_asmlen = 0; continue; }  /* resync */
        if (g_asmlen >= 1 && g_asmlen >= g_asm[0] && g_asm[0] >= 3) {
            on_frame(g_asm, g_asm[0]);
            g_asmlen = 0;
        }
    }
    return n;
}

int mvci_io_read(mvci_io_t *io, uint8_t *buf, int n, int timeout_ms)
{
    (void)io;
    (void)timeout_ms;
    if (!buf || n <= 0) return -1;
    int avail = g_qtail - g_qhead;
    if (avail <= 0) return 0;
    if (n > avail) n = avail;
    memcpy(buf, g_queue + g_qhead, (size_t)n);
    g_qhead += n;
    if (g_qhead == g_qtail) g_qhead = g_qtail = 0;
    return n;
}

void mvci_io_purge_rx(mvci_io_t *io)
{
    (void)io;
    g_qhead = g_qtail = 0;
}

/* Deadline transport entry point used by the real session pipeline. */
int mvci_io_write_timeout(mvci_io_t *io, const uint8_t *buf, int n, int timeout_ms)
{
    (void)timeout_ms;
    return mvci_io_write(io, buf, n);
}
