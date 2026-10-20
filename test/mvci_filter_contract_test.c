/* Offline public filter/IOCTL contracts. Link with serial.c, passthru.c,
 * des.c, libcrypto/pthread and --wrap=clock_gettime --wrap=nanosleep.
 * The transport below is scripted; no hardware backend is linked.
 */
#include <mvci/j2534.h>
#include <mvci/serial.h>
#include "compat.h"
#include <stdio.h>
#include <string.h>

static uint64_t virtual_ns = 1000000000;
int __wrap_clock_gettime(clockid_t id, struct timespec *t)
{
    (void)id;
    t->tv_sec = (time_t)(virtual_ns / 1000000000);
    t->tv_nsec = (long)(virtual_ns % 1000000000);
    return 0;
}
int __wrap_nanosleep(const struct timespec *t, struct timespec *remaining)
{
    (void)remaining;
    virtual_ns += (uint64_t)t->tv_sec * 1000000000 + (uint64_t)t->tv_nsec;
    return 0;
}

/* Reuse the existing cable script. Add deterministic transport failures,
 * without changing its reply bytes or requiring a new shared mock API. */
#define mvci_io_write_timeout scripted_write_timeout
#define mvci_io_write scripted_write
#define mvci_io_read scripted_read
#define mvci_io_purge_rx scripted_purge
#include "mock_io.c"
#undef mvci_io_write
#undef mvci_io_write_timeout
#undef mvci_io_read
#undef mvci_io_purge_rx

static uint8_t fail_opcode;
static int fail_as_reply;
static unsigned fail_after, purges;
int mvci_io_write(mvci_io_t *io, const uint8_t *buf, int n)
{
    uint8_t inner[MVCI_MAX_INNER];
    int r = mvci_frame_decrypt(mock_key(), buf, (size_t)n, inner, sizeof inner);
    if (fail_after && r >= 3 && inner[2] == fail_opcode && --fail_after == 0) {
        if (!fail_as_reply) return -1;
        int result = scripted_write(io, buf, n);
        scripted_purge(io);
        uint8_t rejected[8] = {2, 0, inner[2], 8};
        queue_status(rejected);
        return result;
    }
    return scripted_write(io, buf, n);
}
int mvci_io_write_timeout(mvci_io_t *io, const uint8_t *buf, int n, int timeout_ms)
{
    (void)timeout_ms;
    return mvci_io_write(io, buf, n);
}
int mvci_io_read(mvci_io_t *io, uint8_t *buf, int n, int timeout_ms)
{
    int r = scripted_read(io, buf, n, timeout_ms);
    if (r == 0 && timeout_ms > 0) virtual_ns += (uint64_t)timeout_ms * 1000000;
    return r;
}
void mvci_io_purge_rx(mvci_io_t *io)
{
    purges++;
    scripted_purge(io);
}

static unsigned checks, failures;
#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while (0)

static J2534_ULONG open_channel(J2534_ULONG *dev)
{
    J2534_ULONG ch = 0;
    CHECK(PassThruOpen((void *)"mock", dev) == STATUS_NOERROR);
    CHECK(PassThruConnect(*dev, ISO9141, 0, 10400, &ch) == STATUS_NOERROR);
    return ch;
}
static PASSTHRU_MSG descriptor(uint8_t value)
{
    PASSTHRU_MSG msg = {0};
    msg.ProtocolID = ISO9141;
    msg.DataSize = 1;
    msg.Data[0] = value;
    return msg;
}
static J2534_LONG add_filter(J2534_ULONG ch, J2534_ULONG *fid)
{
    PASSTHRU_MSG mask = descriptor(0x80), pattern = descriptor(0);
    return PassThruStartMsgFilter(ch, PASS_FILTER, &mask, &pattern, NULL, fid);
}
static int frame_inner(int idx, uint8_t *inner)
{
    uint8_t frame[MVCI_MAX_FRAME];
    int n = mock_frame_bytes(idx, frame, sizeof frame);
    if (n < 0) return -1;
    return mvci_frame_decrypt(mock_key(), frame, (size_t)n, inner, MVCI_MAX_INNER);
}
static uint32_t filter_handle(int idx, uint8_t opcode)
{
    uint8_t inner[MVCI_MAX_INNER] = {0};
    CHECK(frame_inner(idx, inner) >= 11);
    CHECK(inner[2] == opcode);
    CHECK(get_u32(inner + 3) == ISO9141);
    return get_u32(inner + 7);
}
static void refuse_filter(J2534_ULONG ch, J2534_ULONG type,
                          PASSTHRU_MSG *mask, PASSTHRU_MSG *pattern,
                          PASSTHRU_MSG *flow, J2534_LONG expected)
{
    J2534_ULONG fid = 0xdeadbeef;
    int before = mock_frame_count();
    CHECK(PassThruStartMsgFilter(ch, type, mask, pattern, flow, &fid) == expected);
    CHECK(fid == 0xdeadbeef);
    CHECK(mock_frame_count() == before);
}
static void test_filter_admission(void)
{
    J2534_ULONG dev = 0, ch = open_channel(&dev), fid = 0;
    PASSTHRU_MSG mask = descriptor(0x80), pattern = descriptor(0), flow = descriptor(1);
    refuse_filter(ch, BLOCK_FILTER, &mask, &pattern, NULL, ERR_NOT_SUPPORTED);
    refuse_filter(ch, FLOW_CONTROL_FILTER, &mask, &pattern, NULL, ERR_NOT_SUPPORTED);
    refuse_filter(ch, FLOW_CONTROL_FILTER, &mask, &pattern, &flow, ERR_NOT_SUPPORTED);
    refuse_filter(ch, 0xffffffff, &mask, &pattern, NULL, ERR_NOT_SUPPORTED);
    refuse_filter(ch, PASS_FILTER, &mask, &pattern, &flow, ERR_INVALID_MSG);
    refuse_filter(ch, PASS_FILTER, NULL, &pattern, NULL, ERR_NULL_PARAMETER);
    refuse_filter(ch, PASS_FILTER, &mask, NULL, NULL, ERR_NULL_PARAMETER);
    int before = mock_frame_count();
    CHECK(PassThruStartMsgFilter(ch, PASS_FILTER, &mask, &pattern, NULL, NULL) == ERR_NULL_PARAMETER);
    CHECK(mock_frame_count() == before);
    mask.DataSize = pattern.DataSize = 0;
    refuse_filter(ch, PASS_FILTER, &mask, &pattern, NULL, ERR_INVALID_MSG);
    mask.DataSize = 1;
    refuse_filter(ch, PASS_FILTER, &mask, &pattern, NULL, ERR_INVALID_MSG);
    pattern.DataSize = 2;
    refuse_filter(ch, PASS_FILTER, &mask, &pattern, NULL, ERR_INVALID_MSG);
    mask.DataSize = 2;
    refuse_filter(ch, PASS_FILTER, &mask, &pattern, NULL, ERR_NOT_SUPPORTED);
    mask.DataSize = pattern.DataSize = 0xffffffff;
    refuse_filter(ch, PASS_FILTER, &mask, &pattern, NULL, ERR_NOT_SUPPORTED);
    mask.DataSize = pattern.DataSize = 1;
    mask.ProtocolID = CAN;
    refuse_filter(ch, PASS_FILTER, &mask, &pattern, NULL, ERR_MSG_PROTOCOL_ID);
    pattern.ProtocolID = CAN;
    refuse_filter(ch, PASS_FILTER, &mask, &pattern, NULL, ERR_MSG_PROTOCOL_ID);
    mask.ProtocolID = pattern.ProtocolID = ISO9141;
    mask.TxFlags = 1;
    refuse_filter(ch, PASS_FILTER, &mask, &pattern, NULL, ERR_INVALID_FLAGS);
    mask.TxFlags = 0; pattern.TxFlags = 1;
    refuse_filter(ch, PASS_FILTER, &mask, &pattern, NULL, ERR_INVALID_FLAGS);
    pattern.TxFlags = 0;
    fail_opcode = 0x0b; fail_after = 1;
    refuse_filter(ch, PASS_FILTER, &mask, &pattern, NULL, ERR_FAILED);
    /* A failed write can leave a partial command on the cable. The new
     * pipeline must refuse subsequent commands until a fresh context. */
    refuse_filter(ch, PASS_FILTER, &mask, &pattern, NULL, ERR_FAILED);
    CHECK(PassThruClose(dev) == STATUS_NOERROR);
    ch = open_channel(&dev);
    CHECK(add_filter(ch, &fid) == STATUS_NOERROR && fid == 1);
    uint8_t inner[MVCI_MAX_INNER];
    CHECK(frame_inner(mock_frame_count() - 1, inner) >= 17);
    CHECK(get_u32(inner + 11) == PASS_FILTER && inner[15] == 0x80 && inner[16] == 0);
    CHECK(PassThruStopMsgFilter(ch, fid) == STATUS_NOERROR);
    /* Preserve the original caller's zero-initialized descriptor form. */
    mask.ProtocolID = pattern.ProtocolID = 0;
    CHECK(PassThruStartMsgFilter(ch, PASS_FILTER, &mask, &pattern, NULL, &fid) == STATUS_NOERROR);
    CHECK(PassThruClose(dev) == STATUS_NOERROR);
}
static void test_reusable_capacity_and_device_handles(void)
{
    J2534_ULONG dev = 0, ch = open_channel(&dev), ids[32], replacement = 0;
    for (unsigned i = 0; i < 32; i++) CHECK(add_filter(ch, &ids[i]) == STATUS_NOERROR);
    int before = mock_frame_count();
    CHECK(add_filter(ch, &replacement) == ERR_EXCEEDED_LIMIT && replacement == 0);
    CHECK(mock_frame_count() == before);
    uint32_t original_handle = filter_handle(3 + 7, 0x0b);
    CHECK(PassThruStopMsgFilter(ch, ids[7]) == STATUS_NOERROR);
    CHECK(filter_handle(mock_frame_count() - 1, 0x0c) == original_handle);
    CHECK(add_filter(ch, &replacement) == STATUS_NOERROR && replacement != ids[7]);
    CHECK(filter_handle(mock_frame_count() - 1, 0x0b) == original_handle);
    before = mock_frame_count();
    CHECK(PassThruStopMsgFilter(ch, ids[7]) == ERR_INVALID_FILTER_ID);
    CHECK(mock_frame_count() == before);
    CHECK(PassThruIoctl(ch, CLEAR_MSG_FILTERS, NULL, NULL) == STATUS_NOERROR);
    CHECK(mock_frame_count() == before + 32);
    before = mock_frame_count();
    CHECK(PassThruStopMsgFilter(ch, replacement) == ERR_INVALID_FILTER_ID);
    CHECK(PassThruIoctl(ch, CLEAR_MSG_FILTERS, NULL, NULL) == STATUS_NOERROR);
    CHECK(mock_frame_count() == before);
    /* More than 32 start/stop cycles must not exhaust an empty table. */
    for (unsigned i = 0; i < 33; i++) {
        CHECK(add_filter(ch, &replacement) == STATUS_NOERROR &&
              PassThruStopMsgFilter(ch, replacement) == STATUS_NOERROR);
    }
    CHECK(PassThruClose(dev) == STATUS_NOERROR);
}
static void test_failed_stop_and_partial_clear(void)
{
    J2534_ULONG dev = 0, ch = open_channel(&dev), ids[3];
    for (unsigned i = 0; i < 3; i++) CHECK(add_filter(ch, &ids[i]) == STATUS_NOERROR);
    fail_as_reply = 1;
    fail_opcode = 0x0c; fail_after = 1;
    CHECK(PassThruStopMsgFilter(ch, ids[0]) == ERR_FAILED);
    /* First stop succeeds; second fails. Only the first entry is removed. */
    fail_after = 2;
    int before = mock_frame_count();
    CHECK(PassThruIoctl(ch, CLEAR_MSG_FILTERS, NULL, NULL) == ERR_FAILED);
    CHECK(mock_frame_count() == before + 2);
    CHECK(filter_handle(before + 1, 0x0c) == 0x000e7a02);
    CHECK(filter_handle(before, 0x0c) == 0x000e7a01);
    char error[128];
    CHECK(PassThruGetLastError(error) == STATUS_NOERROR);
    CHECK(strstr(error, "remaining") != NULL);
    before = mock_frame_count();
    CHECK(PassThruStopMsgFilter(ch, ids[0]) == ERR_INVALID_FILTER_ID);
    CHECK(mock_frame_count() == before);
    CHECK(PassThruIoctl(ch, CLEAR_MSG_FILTERS, NULL, NULL) == STATUS_NOERROR);
    CHECK(mock_frame_count() == before + 2);
    CHECK(filter_handle(before, 0x0c) == 0x000e7a02);
    CHECK(filter_handle(before + 1, 0x0c) == 0x000e7a03);
    for (unsigned i = 0; i < 3; i++) CHECK(PassThruStopMsgFilter(ch, ids[i]) == ERR_INVALID_FILTER_ID);
    CHECK(PassThruClose(dev) == STATUS_NOERROR);
}
static void test_channel_and_device_scope(void)
{
    static const J2534_ULONG channel_ioctls[] = {
        GET_CONFIG, SET_CONFIG, FAST_INIT, FIVE_BAUD_INIT, CLEAR_MSG_FILTERS,
        CLEAR_PERIODIC_MSGS, CLEAR_RX_BUFFER, CLEAR_TX_BUFFER
    };
    J2534_ULONG dev = 0, ch = open_channel(&dev), fid = 0;
    CHECK(add_filter(ch, &fid) == STATUS_NOERROR);
    int before = mock_frame_count();
    for (unsigned i = 0; i < sizeof channel_ioctls / sizeof channel_ioctls[0]; i++) {
        CHECK(PassThruIoctl(dev, channel_ioctls[i], NULL, NULL) == ERR_INVALID_CHANNEL_ID);
        CHECK(PassThruIoctl(0xffffffff, channel_ioctls[i], NULL, NULL) == ERR_INVALID_CHANNEL_ID);
    }
    CHECK(PassThruIoctl(dev, READ_VBATT, NULL, NULL) == ERR_NOT_SUPPORTED);
    CHECK(PassThruIoctl(dev, READ_PROG_VOLTAGE, NULL, NULL) == ERR_NOT_SUPPORTED);
    CHECK(PassThruIoctl(ch, READ_VBATT, NULL, NULL) == ERR_INVALID_DEVICE_ID);
    CHECK(PassThruIoctl(ch, READ_PROG_VOLTAGE, NULL, NULL) == ERR_INVALID_DEVICE_ID);
    CHECK(PassThruIoctl(0, READ_VBATT, NULL, NULL) == ERR_INVALID_DEVICE_ID);
    CHECK(mock_frame_count() == before);
    CHECK(PassThruDisconnect(ch) == STATUS_NOERROR);
    before = mock_frame_count();
    for (unsigned i = 0; i < sizeof channel_ioctls / sizeof channel_ioctls[0]; i++)
        CHECK(PassThruIoctl(ch, channel_ioctls[i], NULL, NULL) == ERR_INVALID_CHANNEL_ID);
    CHECK(add_filter(ch, &fid) == ERR_INVALID_CHANNEL_ID);
    CHECK(PassThruStopMsgFilter(ch, fid) == ERR_INVALID_CHANNEL_ID);
    CHECK(mock_frame_count() == before);
    CHECK(PassThruClose(dev) == STATUS_NOERROR);
    CHECK(PassThruIoctl(dev, READ_PROG_VOLTAGE, NULL, NULL) == ERR_INVALID_DEVICE_ID);
    CHECK(PassThruIoctl(ch, CLEAR_MSG_FILTERS, NULL, NULL) == ERR_INVALID_CHANNEL_ID);
}
static void test_device_filter_maps_are_separate(void)
{
    J2534_ULONG dev1 = 0, ch1 = open_channel(&dev1), first = 0, second = 0;
    CHECK(add_filter(ch1, &first) == STATUS_NOERROR);
    CHECK(add_filter(ch1, &second) == STATUS_NOERROR);
    J2534_ULONG dev2 = 0, ch2 = open_channel(&dev2), other = 0;
    CHECK(dev1 != dev2 && ch1 != ch2);
    CHECK(add_filter(ch2, &other) == STATUS_NOERROR);
    int before = mock_frame_count();
    CHECK(PassThruStopMsgFilter(ch2, second) == ERR_INVALID_FILTER_ID);
    CHECK(mock_frame_count() == before);
    CHECK(PassThruIoctl(ch2, CLEAR_MSG_FILTERS, NULL, NULL) == STATUS_NOERROR);
    CHECK(PassThruStopMsgFilter(ch1, second) == STATUS_NOERROR);
    CHECK(PassThruStopMsgFilter(ch1, first) == STATUS_NOERROR);
    CHECK(PassThruClose(dev2) == STATUS_NOERROR);
    CHECK(PassThruClose(dev1) == STATUS_NOERROR);
}
static void test_rx_tx_and_periodic_clear(void)
{
    J2534_ULONG dev = 0, ch = open_channel(&dev);
    const uint8_t payload[] = {0x48,0x6b,0x10,0x41,0x0c,0x12,0x34};
    mock_rx_record(payload, sizeof payload, 0);
    SCONFIG config = {P3_MIN, 100};
    SCONFIG_LIST list = {1, &config};
    CHECK(PassThruIoctl(ch, SET_CONFIG, &list, NULL) == STATUS_NOERROR);
    mock_rx_record(payload, sizeof payload, 0);
    unsigned old_purges = purges;
    int before = mock_frame_count();
    CHECK(PassThruIoctl(ch, CLEAR_RX_BUFFER, NULL, NULL) == STATUS_NOERROR);
    CHECK(mock_frame_count() == before && purges == old_purges + 1);
    PASSTHRU_MSG msg = {0};
    J2534_ULONG n = 1;
    CHECK(PassThruReadMsgs(ch, &msg, &n, 0) == ERR_BUFFER_EMPTY && n == 0);
    msg = descriptor(0x01); n = 1;
    CHECK(PassThruWriteMsgs(ch, &msg, &n, 1000) == STATUS_NOERROR);
    before = mock_frame_count(); old_purges = purges;
    CHECK(PassThruIoctl(ch, CLEAR_TX_BUFFER, NULL, NULL) == ERR_NOT_SUPPORTED);
    CHECK(mock_frame_count() == before && purges == old_purges);
    CHECK(PassThruIoctl(ch, CLEAR_PERIODIC_MSGS, NULL, NULL) == STATUS_NOERROR);
    uint8_t inner[MVCI_MAX_INNER];
    CHECK(frame_inner(mock_frame_count() - 1, inner) >= 8);
    CHECK(inner[2] == 0x0e && inner[3] == 9 && get_u32(inner + 4) == ISO9141);
    fail_as_reply = 1;
    fail_opcode = 0x0e; fail_after = 1;
    CHECK(PassThruIoctl(ch, CLEAR_PERIODIC_MSGS, NULL, NULL) == ERR_FAILED);
    /* Timing read-back must remain unsupported even after an ACKed SET. */
    config.Value = 0xdeadbeef;
    before = mock_frame_count();
    CHECK(PassThruIoctl(ch, GET_CONFIG, &list, NULL) == ERR_NOT_SUPPORTED);
    CHECK(config.Value == 0xdeadbeef && mock_frame_count() == before);
    CHECK(PassThruClose(dev) == STATUS_NOERROR);
}
int main(void)
{
    test_filter_admission();
    test_reusable_capacity_and_device_handles();
    test_failed_stop_and_partial_clear();
    test_channel_and_device_scope();
    test_device_filter_maps_are_separate();
    test_rx_tx_and_periodic_clear();
    printf("filter/IOCTL contracts: %u checks, %u failures, 0 skips\n", checks, failures);
    return failures ? 1 : 0;
}
