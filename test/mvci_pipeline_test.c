/* Offline public-API regression: scripted transport and virtual time only.
 * Build with serial.c passthru.c des.c, NOT io.c or a separate mock_io.c.
 * -Wl,--wrap=clock_gettime,--wrap=nanosleep,--wrap=pthread_mutex_trylock
 * supplies deterministic time and lock contention. */
#include <mvci/j2534.h>
#include <mvci/serial.h>
#include "io.h"
#include "compat.h"
#include "pipeline.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static uint64_t clock_ns = 1000000000;
int __wrap_clock_gettime(clockid_t id, struct timespec *t)
{
    (void)id;
    t->tv_sec = clock_ns / 1000000000;
    t->tv_nsec = clock_ns % 1000000000;
    return 0;
}
int __wrap_nanosleep(const struct timespec *t, struct timespec *rest)
{
    (void)rest;
    clock_ns += (uint64_t)t->tv_sec * 1000000000 + t->tv_nsec;
    return 0;
}
#define mvci_io_read scripted_base_read
#define mvci_io_write scripted_base_write
#define mvci_io_write_timeout scripted_base_write_timeout
#include "mock_io.c"
#undef mvci_io_read
#undef mvci_io_write
#undef mvci_io_write_timeout

static int reject_op, reject_at, op_calls, drop_op, lost, chunk;
static int tx_delay, read_wait, async_len, async_count, bad_poll;
static int tx_budget, lock_busy, init_success;
static int poll_calls, poll_missing_after;
static int delayed_poll_num, delayed_poll_ms, delayed_data, delayed_split;
static uint8_t delayed_frame[MVCI_MAX_FRAME];
static int delayed_len, delayed_pos;
static uint64_t delayed_ready_ns;
static int rx_stream, stream_generated, immediate_reads, max_read_timeout;
static uint64_t immediate_ns;
int __real_pthread_mutex_trylock(pthread_mutex_t *);
int __wrap_pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    return lock_busy ? EBUSY : __real_pthread_mutex_trylock(mutex);
}
static uint8_t extra_data[64];

int mvci_io_read(mvci_io_t *io, uint8_t *buf, int n, int timeout_ms)
{
    assert(timeout_ms >= 0);
    if (lost) return -1;
    if (timeout_ms > max_read_timeout) max_read_timeout = timeout_ms;
    if (delayed_len && g_qhead == g_qtail) {
        uint64_t end = clock_ns + (uint64_t)timeout_ms * 1000000;
        if (clock_ns < delayed_ready_ns) {
            if (timeout_ms && end >= delayed_ready_ns) clock_ns = delayed_ready_ns;
            else if (timeout_ms) clock_ns = end;
        }
        if (clock_ns >= delayed_ready_ns) {
            int take = delayed_split && delayed_pos == 0 ? delayed_split : delayed_len - delayed_pos;
            queue_bytes(delayed_frame + delayed_pos, take);
            delayed_pos += take;
            if (delayed_pos == delayed_len) delayed_len = delayed_pos = 0;
            else delayed_ready_ns = clock_ns + 5000000;
        }
    }
    if (rx_stream && g_qhead == g_qtail) {
        /* An infinite source: the public call must stop itself. The guard
         * fails the test instead of injecting EOF to make the call return. */
        assert(++stream_generated <= 1024);
        if (rx_stream == 2 || (rx_stream == 3 && stream_generated % 2)) {
            const uint8_t empty[8] = {2, 0, 9, 0x10};
            queue_status(empty);
        } else mock_rx_record(extra_data, 6, 2);
    }
    if (chunk && n > chunk) n = chunk;
    int r = scripted_base_read(io, buf, n, timeout_ms);
    if (r > 0) {
        ++immediate_reads;
        clock_ns += immediate_ns;
    }
    if (!r && read_wait && !delayed_len) clock_ns += (uint64_t)timeout_ms * 1000000;
    return r;
}
int mvci_io_write(mvci_io_t *io, const uint8_t *buf, int n)
{
    return scripted_base_write(io, buf, n);
}
int mvci_io_write_timeout(mvci_io_t *io, const uint8_t *buf, int n, int timeout_ms)
{
    tx_budget = timeout_ms;
    if (tx_delay) {
        int delay = tx_delay < timeout_ms ? tx_delay : timeout_ms;
        clock_ns += (uint64_t)delay * 1000000;
        if (tx_delay >= timeout_ms) return 0;
    }
    uint8_t inner[MVCI_MAX_INNER];
    int len = mvci_frame_decrypt(mock_key(), buf, n, inner, sizeof inner);
    int op = len > 2 ? inner[2] : -1;
    int r = scripted_base_write(io, buf, n);
    if (op == 0x0e && len >= 4 && inner[3] == 4 && init_success) {
        mvci_io_purge_rx(io);
        const uint8_t success[8] = {3, 0, 0x0e, 8, 8, 0, 0, 0};
        queue_status(success);
    }
    if (op == drop_op) mvci_io_purge_rx(io);
    if (op == reject_op && ++op_calls == reject_at) {
        mvci_io_purge_rx(io);
        uint8_t status[8] = {2, 0, (uint8_t)op, 8};
        queue_status(status);
    }
    if (op == 9 && bad_poll) {
        mvci_io_purge_rx(io);
        uint8_t status[8] = {2, 0, 9, 8};
        queue_status(status);
    }
    if (op == 9) {
        ++poll_calls;
        if (poll_missing_after && poll_calls == poll_missing_after)
            mvci_io_purge_rx(io);
        if (delayed_poll_num && poll_calls == delayed_poll_num) {
            if (delayed_data) {
                static const uint8_t data[5] = {0x48, 0x6b, 0x10, 0x49, 0x09};
                mvci_io_purge_rx(io);
                mock_rx_record(data, sizeof data, 0);
            }
            delayed_len = g_qtail - g_qhead;
            assert(delayed_len > 0 && delayed_len <= (int)sizeof delayed_frame);
            memcpy(delayed_frame, g_queue + g_qhead, (size_t)delayed_len);
            g_qhead = g_qtail = 0;
            delayed_pos = 0;
            delayed_ready_ns = clock_ns + (uint64_t)delayed_poll_ms * 1000000;
        }
    }
    if (op == 0x0a && async_count) {
        mvci_io_purge_rx(io);
        for (int i = 0; i < async_count; i++) mock_rx_record(extra_data, async_len, 0);
        uint8_t status[8] = {2, 0, 0x0a, 0};
        queue_status(status);
    }
    return r;
}

static J2534_ULONG device, channel;
static PASSTHRU_MSG messages[3], received[20];
static void start(void)
{
    reject_op = drop_op = -1;
    rx_stream = stream_generated = immediate_reads = max_read_timeout = 0;
    immediate_ns = 0;
    lock_busy = init_success = 0;
    poll_calls = poll_missing_after = 0;
    delayed_poll_num = delayed_poll_ms = delayed_data = delayed_split = delayed_len = delayed_pos = 0;
    reject_at = op_calls = lost = chunk = tx_delay = read_wait = async_count = bad_poll = 0;
    assert(PassThruOpen((void *)"script-only", &device) == 0);
    assert(PassThruConnect(device, ISO9141, 0x1200, 10400, &channel) == 0);
    memset(messages, 0, sizeof messages);
    for (int i = 0; i < 3; i++) {
        messages[i].ProtocolID = ISO9141;
        messages[i].DataSize = 6;
        memcpy(messages[i].Data, "\x68\x6a\xf1\x01\x0c\xd0", 6);
    }
}
static void stop(void)
{
    lost = lock_busy = 0;
    assert(PassThruClose(device) == 0);
}
static long config(void)
{
    SCONFIG one = {P3_MIN, 0};
    SCONFIG_LIST list = {1, &one};
    return PassThruIoctl(channel, SET_CONFIG, &list, NULL);
}
static void test_ack_results(void)
{
    const int ops[] = {7, 0x0b, 0x0c, 0x0d, 0x0e, 0x0a};
    for (unsigned i = 0; i < sizeof ops / sizeof *ops; i++) {
        start();
        reject_op = ops[i]; reject_at = 1;
        long r;
        J2534_ULONG n = 1, filter = 0;
        if (ops[i] == 7) {
            assert(PassThruDisconnect(channel) == 0);
            r = PassThruConnect(device, ISO9141, 0x1200, 10400, &channel);
        } else if (ops[i] == 0x0b || ops[i] == 0x0c) {
            PASSTHRU_MSG mask = {0}, pattern = {0};
            mask.DataSize = pattern.DataSize = 1;
            mask.Data[0] = 0x80;
            r = PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, NULL, &filter);
            if (ops[i] == 0x0c) {
                assert(r == 0);
                r = PassThruStopMsgFilter(channel, filter);
            }
        } else if (ops[i] == 0x0d) r = PassThruSetProgrammingVoltage(device, 15, 0xfffffffeu);
        else if (ops[i] == 0x0e) r = PassThruIoctl(channel, CLEAR_PERIODIC_MSGS, NULL, NULL);
        else {
            r = PassThruWriteMsgs(channel, messages, &n, 10);
            assert(n == 0);
        }
        assert(r == ERR_FAILED);
        stop();
    }
    start(); reject_op = 0x0e; reject_at = 1;
    assert(config() == ERR_FAILED);
    assert(config() == 0); /* A complete rejection is not a missing result. */
    stop();
    start(); init_success = 1;
    uint8_t address = 0x33, keys[2] = {0};
    SBYTE_ARRAY in = {1, &address}, out = {2, keys};
    assert(PassThruIoctl(channel, FIVE_BAUD_INIT, &in, &out) == 0);
    assert(out.NumOfBytes == 2 && keys[0] == 8 && keys[1] == 8);
    stop();
    reject_op = 9; reject_at = 1; op_calls = 0;
    mvci_ctx_t *ctx = mvci_open("script-only");
    assert(ctx && mvci_handshake(ctx) == 0);
    assert(mvci_keepalive(ctx) == -1);
    mvci_close(ctx);
    puts("PASS: nonzero ACK results propagate; exact Corolla init success remains distinct");
}
static void test_stale_ack_and_timeout(void)
{
    start();
    uint8_t stale[8] = {2, 0, 0x0e, 0}; queue_status(stale);
    int before = mock_frame_count();
    assert(config() == ERR_FAILED && mock_frame_count() == before);
    mock_config_status(8);
    assert(config() == ERR_FAILED && mock_frame_count() == before);
    assert(PassThruIoctl(channel, CLEAR_RX_BUFFER, NULL, NULL) == 0);
    assert(config() == ERR_FAILED && mock_frame_count() == before);
    stop();
    start(); drop_op = 0x0a; read_wait = 1;
    uint64_t began = clock_ns;
    J2534_ULONG n = 2;
    assert(PassThruWriteMsgs(channel, messages, &n, 17) == ERR_TIMEOUT && n == 0);
    assert(clock_ns - began == 17000000 && tx_budget == 17);
    uint8_t late[8] = {2, 0, 0x0a, 0}; queue_status(late);
    before = mock_frame_count(); n = 1;
    assert(PassThruWriteMsgs(channel, messages, &n, 17) == ERR_FAILED && n == 0);
    assert(mock_frame_count() == before);
    stop();
    puts("PASS: unsolicited/late ACK cannot substitute for a later result; RX clear cannot recover ambiguity");
}
static void test_write_batch_and_deadline(void)
{
    start(); J2534_ULONG n = 3;
    assert(PassThruWriteMsgs(channel, messages, &n, 30) == 0 && n == 3);
    reject_op = 0x0a; reject_at = 2; n = 3;
    assert(PassThruWriteMsgs(channel, messages, &n, 30) == ERR_FAILED && n == 1);
    int before = mock_frame_count(); uint64_t began = clock_ns; n = 1;
    assert(PassThruWriteMsgs(channel, messages, &n, 0) == ERR_NOT_SUPPORTED && n == 0);
    assert(clock_ns == began && mock_frame_count() == before);
    messages[1].TxFlags = ISO15765_FRAME_PAD; n = 2;
    assert(PassThruWriteMsgs(channel, messages, &n, 30) == ERR_INVALID_FLAGS && n == 0);
    messages[1].TxFlags = 0; messages[1].ProtocolID = CAN; n = 2;
    assert(PassThruWriteMsgs(channel, messages, &n, 30) == ERR_MSG_PROTOCOL_ID && n == 0);
    assert(mock_frame_count() == before);
    stop();
    start(); tx_delay = 6; began = clock_ns; n = 3;
    assert(PassThruWriteMsgs(channel, messages, &n, 10) == ERR_TIMEOUT && n == 1);
    assert(clock_ns - began == 10000000 && tx_budget == 4);
    stop();
    puts("PASS: batches, partial counts, unsupported queue-only mode and total write deadline");
}
static void test_read_deadlines_and_errors(void)
{
    start(); J2534_ULONG n = 2; int before = mock_frame_count();
    uint64_t began = clock_ns;
    assert(PassThruReadMsgs(channel, received, &n, 0) == ERR_BUFFER_EMPTY && n == 0);
    assert(clock_ns == began && mock_frame_count() == before);
    uint8_t empty[8] = {2, 0, 9, 0x10}; queue_status(empty);
    mock_rx_record(extra_data, 64, 0); n = 2;
    assert(PassThruReadMsgs(channel, received, &n, 0) == 0 && n == 1 && received[0].DataSize == 64);
    assert(mock_frame_count() == before && clock_ns == began);
    mock_rx_record(extra_data, 5, 0); n = 2;
    assert(PassThruReadMsgs(channel, received, &n, 13) == ERR_TIMEOUT && n == 1);
    assert(clock_ns - began == 13000000);
    stop();
    start(); mock_rx_record(extra_data, 5, 0);
    const uint8_t corrupt[11] = {11}; queue_bytes(corrupt, sizeof corrupt); n = 2;
    assert(PassThruReadMsgs(channel, received, &n, 0) == ERR_FAILED && n == 1);
    stop();
    start(); lost = 1; n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 0) == ERR_DEVICE_NOT_CONNECTED && n == 0);
    stop();
    start(); bad_poll = 1; n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 10) == ERR_FAILED && n == 0);
    stop();
    start(); drop_op = 9; read_wait = 1; n = 1;
    began = clock_ns;
    assert(PassThruReadMsgs(channel, received, &n, 9) == ERR_TIMEOUT && n == 0);
    assert(clock_ns - began == 9000000);
    mock_rx_record(extra_data, 5, 0); n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 0) == 0 && n == 1);
    stop();
    puts("PASS: zero-timeout drains, partial timeout, corrupt frame, provider loss and unknown/missing poll result");
}
static void test_pending_poll_across_public_deadlines(void)
{
    for (int delay = 5; delay <= 60; ++delay) {
        start(); delayed_poll_num = 1; delayed_poll_ms = delay; read_wait = 1;
        J2534_ULONG count = 1;
        assert(PassThruReadMsgs(channel, received, &count, 20) == ERR_TIMEOUT && count == 0);
        count = 1;
        assert(PassThruWriteMsgs(channel, messages, &count, 100) == 0 && count == 1);
        assert(config() == 0);
        stop();
    }
    /* The virtual transport releases a real framed reply only at ready time.
     * Each public operation must resolve it before sending or purging. */
    for (int operation = 0; operation < 4; ++operation) {
        start(); delayed_poll_num = 1; delayed_poll_ms = 35; read_wait = 1;
        J2534_ULONG n = 1;
        assert(PassThruReadMsgs(channel, received, &n, 20) == ERR_TIMEOUT && n == 0);
        int before = mock_frame_count();
        if (operation == 0) {
            n = 1;
            long wr = PassThruWriteMsgs(channel, messages, &n, 50);
            assert(wr == 0 && n == 1);
            assert(mock_frame_count() == before + 1);
        } else if (operation == 1) {
            assert(config() == 0 && mock_frame_count() == before + 1);
        } else if (operation == 2) {
            assert(PassThruIoctl(channel, CLEAR_RX_BUFFER, NULL, NULL) == 0);
            assert(mock_frame_count() == before);
        } else {
            assert(PassThruDisconnect(channel) == 0);
            assert(mock_frame_count() == before + 1);
        }
        stop();
    }

    start(); delayed_poll_num = 1; delayed_poll_ms = 38; delayed_data = 1; read_wait = 1;
    J2534_ULONG n = 2;
    assert(PassThruReadMsgs(channel, received, &n, 20) == ERR_TIMEOUT && n == 0);
    int before = mock_frame_count();
    n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 7) == ERR_TIMEOUT && n == 0);
    assert(mock_frame_count() == before); /* two deadlines, one poll */
    n = 2;
    assert(PassThruReadMsgs(channel, received, &n, 20) == ERR_TIMEOUT && n == 1);
    assert(received[0].DataSize == 5 && mock_frame_count() >= before + 1);
    stop();

    start(); delayed_poll_num = 1; delayed_poll_ms = 18; delayed_data = 1;
    delayed_split = 4; read_wait = 1;
    n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 20) == ERR_TIMEOUT && n == 0);
    before = mock_frame_count();
    n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 0) == ERR_BUFFER_EMPTY && n == 0);
    assert(mock_frame_count() == before);
    n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 20) == 0 && n == 1);
    assert(received[0].DataSize == 5 && mock_frame_count() == before);
    stop();

    start(); delayed_poll_num = 1; delayed_poll_ms = 22; delayed_data = 1; read_wait = 1;
    n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 20) == ERR_TIMEOUT && n == 0);
    before = mock_frame_count();
    clock_ns += 3000000;
    n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 0) == 0 && n == 1);
    assert(received[0].DataSize == 5 && mock_frame_count() == before);
    stop();

    start(); delayed_poll_num = 1; delayed_poll_ms = 30; read_wait = 1;
    n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 20) == ERR_TIMEOUT && n == 0);
    before = mock_frame_count();
    assert(PassThruClose(device) == 0);
    assert(mock_frame_count() == before + 1); /* exact teardown was sent */

    start(); poll_missing_after = 1; read_wait = 1;
    n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 20) == ERR_TIMEOUT && n == 0);
    before = mock_frame_count();
    clock_ns += 1001000000;
    assert(config() == ERR_FAILED && mock_frame_count() == before);
    stop();
    start(); assert(config() == 0); stop();

    start(); drop_op = 9; read_wait = 1;
    n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 7) == ERR_TIMEOUT && n == 0);
    uint8_t stale_ack[8] = {2, 0, 0x0a, 0};
    queue_status(stale_ack);
    n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 0) == ERR_FAILED && n == 0);
    before = mock_frame_count(); n = 1;
    assert(PassThruWriteMsgs(channel, messages, &n, 20) == ERR_FAILED && n == 0);
    assert(mock_frame_count() == before);
    stop();
    puts("PASS: delayed and split poll replies retain ordering; missing reply poisons after age cap");
}
static void test_pending_poll_age_boundary(void)
{
    /* The reply is ready during application idle. Resolve it before testing
     * the send age, for both command and read callers. */
    for (int mode = 0; mode < 3; ++mode) {
        start(); delayed_poll_num = 1; delayed_poll_ms = 10;
        delayed_data = mode == 0; read_wait = 1;
        J2534_ULONG n = 1;
        assert(PassThruReadMsgs(channel, received, &n, 5) == ERR_TIMEOUT && n == 0);
        int before = mock_frame_count();
        clock_ns += 1500000000;
        if (mode == 0) {
            n = 1;
            assert(PassThruReadMsgs(channel, received, &n, 0) == 0 && n == 1);
            assert(received[0].DataSize == 5 && mock_frame_count() == before);
        } else if (mode == 1) {
            n = 1;
            assert(PassThruReadMsgs(channel, received, &n, 0) == ERR_BUFFER_EMPTY && n == 0);
            assert(mock_frame_count() == before);
        } else {
            n = 1;
            assert(PassThruWriteMsgs(channel, messages, &n, 50) == 0 && n == 1);
            assert(mock_frame_count() == before + 1);
        }
        stop();
    }

    /* All bytes can be buffered even when the first transport read returns
     * only one byte. The non-waiting probe must finish that frame. */
    start(); delayed_poll_num = 1; delayed_poll_ms = 10;
    delayed_data = 1; read_wait = 1;
    J2534_ULONG n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 5) == ERR_TIMEOUT && n == 0);
    assert(delayed_len > 0);
    queue_bytes(delayed_frame, delayed_len);
    delayed_len = delayed_pos = 0;
    chunk = 1;
    int before = mock_frame_count();
    clock_ns += 1500000000;
    n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 0) == 0 && n == 1);
    assert(received[0].DataSize == 5 && mock_frame_count() == before);
    stop();

    /* A missing reply consumes only the poll's remaining age, even when the
     * read caller grants far more time. No second poll may be sent. */
    start(); poll_missing_after = 1; read_wait = 1;
    n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 5) == ERR_TIMEOUT && n == 0);
    before = mock_frame_count();
    uint64_t began = clock_ns;
    n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 5000) == ERR_FAILED && n == 0);
    assert(clock_ns - began == 996000000 && mock_frame_count() == before);
    n = 1;
    assert(PassThruWriteMsgs(channel, messages, &n, 20) == ERR_FAILED && n == 0);
    assert(mock_frame_count() == before);
    stop();
    puts("PASS: buffered aged replies survive idle; missing polls stop at the age bound");
}
static void test_fragments_bounds_and_overflow(void)
{
    start(); chunk = 1;
    mock_rx_record(extra_data, 64, 0); J2534_ULONG n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 0) == 0 && n == 1);
    stop();
    start();
    uint8_t inner[80] = {14, 0, 9}, frame[MVCI_MAX_FRAME];
    int len = mvci_frame_enc(mock_key(), inner, 16, frame, sizeof frame);
    queue_bytes(frame, 4); n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 0) == ERR_BUFFER_EMPTY && n == 0);
    queue_bytes(frame + 4, len - 4); n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 0) == 0 && n == 1 && received[0].DataSize == 5);
    stop();
    start(); async_count = 1; async_len = 64; n = 1;
    assert(PassThruWriteMsgs(channel, messages, &n, 10) == 0 && n == 1);
    n = 1; assert(PassThruReadMsgs(channel, received, &n, 0) == 0 && n == 1 && received[0].DataSize == 64);
    stop();
    start(); async_count = 17; async_len = 64; n = 1;
    assert(PassThruWriteMsgs(channel, messages, &n, 10) == ERR_BUFFER_OVERFLOW && n == 0);
    n = 1; assert(PassThruReadMsgs(channel, received, &n, 0) == ERR_BUFFER_OVERFLOW && n == 0);
    n = 16; assert(PassThruReadMsgs(channel, received, &n, 0) == 0 && n == 16);
    n = 1; assert(PassThruReadMsgs(channel, received, &n, 0) == ERR_FAILED && n == 0);
    stop();
    start(); memset(inner, 0, sizeof inner); inner[0] = 74; inner[2] = 9;
    len = mvci_frame_enc(mock_key(), inner, sizeof inner, frame, sizeof frame);
    queue_bytes(frame, len); n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 0) == ERR_FAILED && n == 0);
    stop();
    puts("PASS: fragmented input, full admitted async capacity, explicit bounded-queue loss and oversize refusal");
}

static void loopback_off(void)
{
    SCONFIG one = {LOOPBACK, 0};
    SCONFIG_LIST list = {1, &one};
    assert(PassThruIoctl(channel, SET_CONFIG, &list, NULL) == 0);
}

static void test_bounded_nonblocking_stream(void)
{
    /* Echoes, empty statuses, and their mixture all use one public budget.
     * There is no I/O error or empty transport read to terminate these calls. */
    for (int stream = 1; stream <= 3; ++stream) {
        start(); loopback_off();
        rx_stream = stream;
        max_read_timeout = 0;
        immediate_ns = 1000000;
        int before = mock_frame_count();
        J2534_ULONG n = 1;
        assert(PassThruReadMsgs(channel, received, &n, 0) == ERR_BUFFER_EMPTY && n == 0);
        assert(stream_generated > 0 && stream_generated <= (int)MVCI_PIPE_READ_BUDGET);
        assert(max_read_timeout == 0 && mock_frame_count() == before);
        rx_stream = 0; immediate_ns = 0;
        mock_rx_record(extra_data, 5, 0); n = 1;
        assert(PassThruReadMsgs(channel, received, &n, 0) == 0 && n == 1);
        assert(config() == 0); /* Valid traffic never poisons command correlation. */
        stop();
    }

    start(); loopback_off();
    const uint8_t first[] = {0x48, 1}, last[] = {0x48, 2};
    mock_rx_record(first, sizeof first, 0);
    for (unsigned i = 1; i < MVCI_PIPE_READ_BUDGET; ++i) {
        if (i % 2) mock_rx_record(extra_data, 6, 2);
        else {
            const uint8_t empty[8] = {2, 0, 9, 0x10};
            queue_status(empty);
        }
    }
    mock_rx_record(last, sizeof last, 0);
    J2534_ULONG n = 2;
    assert(PassThruReadMsgs(channel, received, &n, 0) == 0 && n == 1);
    assert(received[0].DataSize == sizeof first && memcmp(received[0].Data, first, sizeof first) == 0);
    n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 0) == 0 && n == 1);
    assert(received[0].DataSize == sizeof last && memcmp(received[0].Data, last, sizeof last) == 0);
    n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 0) == ERR_BUFFER_EMPTY && n == 0);
    stop();
    puts("PASS: bounded zero-timeout echo/status streams, partial counts and unconsumed record retention");
}

static void test_immediate_read_deadline(void)
{
    const uint8_t first[] = {0x48, 0x6b, 0x10, 0x41, 1};
    const uint8_t second[] = {0x48, 0x6b, 0x10, 0x41, 2};
    /* Expire after only the frame length, then after the complete wire body.
     * Both must retain bytes and deliver the same frame once on the next call. */
    for (int timeout = 1; timeout <= 3; timeout += 2) {
        start();
        mock_rx_record(first, sizeof first, 0);
        mock_rx_record(second, sizeof second, 0);
        immediate_ns = 2000000;
        immediate_reads = max_read_timeout = 0;
        int before = mock_frame_count();
        uint64_t began = clock_ns;
        memset(received, 0xa5, sizeof received);
        PASSTHRU_MSG unchanged = received[0];
        J2534_ULONG n = 1;
        assert(PassThruReadMsgs(channel, received, &n, timeout) == ERR_TIMEOUT && n == 0);
        assert(clock_ns - began == (uint64_t)(timeout + 1) * 1000000);
        assert(immediate_reads == (timeout + 1) / 2 && max_read_timeout == 0);
        assert(mock_frame_count() == before && memcmp(received, &unchanged, sizeof unchanged) == 0);
        immediate_ns = 0; n = 2;
        assert(PassThruReadMsgs(channel, received, &n, 0) == 0 && n == 2);
        assert(received[0].DataSize == sizeof first && memcmp(received[0].Data, first, sizeof first) == 0);
        assert(received[1].DataSize == sizeof second && memcmp(received[1].Data, second, sizeof second) == 0);
        n = 1;
        assert(PassThruReadMsgs(channel, received, &n, 0) == ERR_BUFFER_EMPTY && n == 0);
        assert(config() == 0);
        stop();
    }

    start();
    mock_rx_record(first, sizeof first, 0);
    mock_rx_record(second, sizeof second, 0);
    immediate_ns = 250000;
    J2534_ULONG n = 2;
    assert(PassThruReadMsgs(channel, received, &n, 1) == ERR_TIMEOUT && n == 1);
    assert(received[0].DataSize == sizeof first && memcmp(received[0].Data, first, sizeof first) == 0);
    immediate_ns = 0; n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 0) == 0 && n == 1);
    assert(received[0].DataSize == sizeof second && memcmp(received[0].Data, second, sizeof second) == 0);
    stop();
    puts("PASS: one caller deadline covers immediate I/O; late complete/partial records survive without false counts");
}
static void test_lock_deadline(void)
{
    start(); lock_busy = 1;
    uint64_t began = clock_ns;
    int before = mock_frame_count(); J2534_ULONG n = 1;
    assert(PassThruReadMsgs(channel, received, &n, 0) == ERR_TIMEOUT && n == 0);
    assert(clock_ns == began);
    n = 1;
    assert(PassThruWriteMsgs(channel, messages, &n, 7) == ERR_TIMEOUT && n == 0);
    assert(clock_ns - began == 7000000 && mock_frame_count() == before);
    stop();
    puts("PASS: waiting for the I/O owner consumes the same budget; zero-timeout never waits");
}
int main(void)
{
    memset(extra_data, 0x48, sizeof extra_data);
    test_ack_results();
    test_stale_ack_and_timeout();
    test_write_batch_and_deadline();
    test_read_deadlines_and_errors();
    test_pending_poll_across_public_deadlines();
    test_pending_poll_age_boundary();
    test_fragments_bounds_and_overflow();
    test_bounded_nonblocking_stream();
    test_immediate_read_deadline();
    test_lock_deadline();
    puts("pipeline: all checks passed; skips=0 (offline only)");
    return 0;
}
