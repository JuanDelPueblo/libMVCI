/* Offline public-API regression: scripted transport and virtual time only.
 * Build with serial.c passthru.c des.c, NOT io.c or a separate mock_io.c.
 * -Wl,--wrap=clock_gettime,--wrap=nanosleep,--wrap=pthread_mutex_trylock
 * supplies deterministic time and lock contention. */
#include <mvci/j2534.h>
#include <mvci/serial.h>
#include "io.h"
#include "compat.h"
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
    if (chunk && n > chunk) n = chunk;
    int r = scripted_base_read(io, buf, n, timeout_ms);
    if (!r && read_wait) clock_ns += (uint64_t)timeout_ms * 1000000;
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
    lock_busy = init_success = 0;
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
    assert(PassThruReadMsgs(channel, received, &n, 0) == ERR_FAILED && n == 0);
    stop();
    puts("PASS: zero-timeout drains, partial timeout, corrupt frame, provider loss and unknown/missing poll result");
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
    test_fragments_bounds_and_overflow();
    test_lock_deadline();
    puts("pipeline: all checks passed; skips=0 (offline only)");
    return 0;
}
