/* mvci_seq_test.c — offline J2534 provider sequence-equivalence test.
 *
 * Runs the evidenced Corolla sequences through the real PassThru* provider
 * against an in-memory scripted device (test/mock_io.c) and asserts the
 * exact emitted adapter-command sequence. Any extra adapter command —
 * notably a background 05 00 09 06 keepalive — fails the comparison.
 *
 * Also drives oversized inputs through the public PassThruIoctl path and
 * requires fail-closed returns with no extra wire traffic.
 *
 * No hardware, no vehicle, no network. Built by the CMake `mvci_seq_test`
 * target; runs under ctest as `mvci_seqtest`.
 */

#include <mvci/serial.h>
#include <mvci/j2534.h>

#include <stdio.h>
#include <string.h>

const uint8_t *mock_key(void);
int mock_frame_count(void);
int mock_frame_bytes(int idx, uint8_t *out, int cap);

static int g_pass, g_fail;

static void ok(int cond, const char *name)
{
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (cond) g_pass++;
    else g_fail++;
}

/* Compare logged frame idx against exp[explen]. Plaintext handshake frames
 * compare raw; encrypted frames are decrypted first (full padded length). */
static void expect_frame(int idx, const uint8_t *exp, int explen, const char *name)
{
    uint8_t raw[300];
    int n = mock_frame_bytes(idx, raw, sizeof raw);
    int good = 0;
    if (n == 3 || n == 12) {
        good = (n == explen && memcmp(raw, exp, (size_t)n) == 0);
    } else if (n > 0) {
        uint8_t inner[MVCI_MAX_INNER];
        int il = mvci_frame_decrypt(mock_key(), raw, n, inner, sizeof inner);
        good = (il == explen && memcmp(inner, exp, (size_t)il) == 0);
    }
    if (!good) {
        printf("[FAIL] %s (frame %d)\n", name, idx);
        printf("    expected");
        for (int i = 0; i < explen; i++) printf(" %02x", exp[i]);
        printf("\n");
        if (n > 0) {
            printf("    raw     ");
            for (int i = 0; i < n; i++) printf(" %02x", raw[i]);
            printf("\n");
        }
        g_fail++;
    } else {
        printf("[PASS] %s\n", name);
        g_pass++;
    }
}

static const uint8_t RESETB[] = { 0x03, 0x00, 0x03 };
static const uint8_t IDENT[]  = { 0x0c, 0x00, 0x07, 0x00, 0x01,
                                  'M', 'V', 'C', 'I', '-', 'T', 0x62 };
static const uint8_t CONN0[]  = { 0x0d, 0x00, 0x07, 0x03, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0xa0, 0x28, 0x00, 0x00, 0x00 };
static const uint8_t CONN12[] = { 0x0d, 0x00, 0x07, 0x03, 0x00, 0x00, 0x00,
                                  0x00, 0x12, 0x00, 0x00, 0xa0, 0x28, 0x00, 0x00, 0x00 };
static const uint8_t FB33[]   = { 0x07, 0x00, 0x0e, 0x04, 0x03, 0x00, 0x00, 0x00, 0x33,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t FILT1[]  = { 0x10, 0x00, 0x0b, 0x03, 0x00, 0x00, 0x00,
                                  0x01, 0x7a, 0x0e, 0x00, 0x01, 0x00, 0x00, 0x00,
                                  0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t FILT2[]  = { 0x10, 0x00, 0x0b, 0x03, 0x00, 0x00, 0x00,
                                  0x02, 0x7a, 0x0e, 0x00, 0x01, 0x00, 0x00, 0x00,
                                  0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t PIN15[]  = { 0x09, 0x00, 0x0d, 0x0f, 0x00, 0x00, 0x00,
                                  0xfe, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t SC_DR[]  = { 0x0e, 0x00, 0x0e, 0x02, 0x03, 0x00, 0x00, 0x00,
                                  0x01, 0x00, 0x00, 0x00, 0x80, 0x25, 0x00, 0x00 };
static const uint8_t SC_P4[]  = { 0x0e, 0x00, 0x0e, 0x02, 0x03, 0x00, 0x00, 0x00,
                                  0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t SC_TI[]  = { 0x0e, 0x00, 0x0e, 0x02, 0x03, 0x00, 0x00, 0x00,
                                  0x14, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00, 0x00 };
static const uint8_t SC_TW[]  = { 0x0e, 0x00, 0x0e, 0x02, 0x03, 0x00, 0x00, 0x00,
                                  0x15, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00 };
static const uint8_t FAST10[] = { 0x10, 0x00, 0x0e, 0x05, 0x03, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x13, 0x00, 0x01, 0x00, 0x27, 0x6f,
                                  0x57, 0xbc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t STOP1[]  = { 0x09, 0x00, 0x0c, 0x03, 0x00, 0x00, 0x00,
                                  0x01, 0x7a, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t STOP2[]  = { 0x09, 0x00, 0x0c, 0x03, 0x00, 0x00, 0x00,
                                  0x02, 0x7a, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t DISC[]   = { 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };

static const uint8_t FAST10REQ[] = { 0x00, 0x00, 0x13, 0x00, 0x01,
                                     0x00, 0x27, 0x6f, 0x57, 0xbc };

static J2534_ULONG open_dev(void)
{
    J2534_ULONG dev = 0;
    if (PassThruOpen((void *)"mock", &dev) != STATUS_NOERROR) return 0;
    return dev;
}

static J2534_ULONG connect_ch(J2534_ULONG dev, J2534_ULONG flags)
{
    J2534_ULONG ch = 0;
    if (PassThruConnect(dev, ISO9141, flags, 10400, &ch) != STATUS_NOERROR) return 0;
    return ch;
}

static J2534_LONG add_filter(J2534_ULONG ch, uint8_t mask, uint8_t pattern, J2534_ULONG *fid)
{
    PASSTHRU_MSG m, p;
    memset(&m, 0, sizeof m);
    memset(&p, 0, sizeof p);
    m.DataSize = 1;
    m.Data[0] = mask;
    p.DataSize = 1;
    p.Data[0] = pattern;
    return PassThruStartMsgFilter(ch, PASS_FILTER, &m, &p, NULL, fid);
}

static J2534_LONG set_one(J2534_ULONG ch, uint32_t param, uint32_t value)
{
    SCONFIG sc;
    SCONFIG_LIST sl;
    sc.Parameter = param;
    sc.Value = value;
    sl.NumOfParams = 1;
    sl.ConfigPtr = &sc;
    return PassThruIoctl(ch, SET_CONFIG, &sl, NULL);
}

/* Sequence A shape: Open, Connect(flags 0), FIVE_BAUD_INIT(0x33).
 * The 2000 ms pre-init idle must emit no adapter command. */
static void test_five_baud_idle(void)
{
    puts("--- five-baud idle emits no extra command ---");
    J2534_ULONG dev = open_dev();
    ok(dev != 0, "open mock device");
    J2534_ULONG ch = connect_ch(dev, 0);
    ok(ch != 0, "connect ISO9141 flags 0");

    uint8_t a33 = 0x33, rbuf[32];
    SBYTE_ARRAY in, out;
    in.NumOfBytes = 1;
    in.BytePtr = &a33;
    out.NumOfBytes = sizeof rbuf;
    out.BytePtr = rbuf;
    ok(PassThruIoctl(ch, FIVE_BAUD_INIT, &in, &out) == ERR_DEVICE_NOT_CONNECTED,
       "five-baud returns ERR_DEVICE_NOT_CONNECTED");

    ok(mock_frame_count() == 4, "exactly 4 frames on the wire");
    expect_frame(0, RESETB, sizeof RESETB, "frame 0 reset");
    expect_frame(1, IDENT, sizeof IDENT, "frame 1 identify");
    expect_frame(2, CONN0, sizeof CONN0, "frame 2 connect");
    expect_frame(3, FB33, sizeof FB33, "frame 3 five-baud(0x33)");

    ok(PassThruDisconnect(ch) == STATUS_NOERROR, "disconnect");
    ok(PassThruClose(dev) == STATUS_NOERROR, "close");
    ok(mock_frame_count() == 5, "teardown adds only the disconnect frame");
    expect_frame(4, DISC, sizeof DISC, "frame 4 disconnect");
}

/* Sequence B shape: the full T255 programming entry plus teardown. */
static void test_programming_entry(void)
{
    puts("--- programming entry emits no extra command ---");
    J2534_ULONG dev = open_dev();
    ok(dev != 0, "open mock device");
    J2534_ULONG ch = connect_ch(dev, 0x1200);
    ok(ch != 0, "connect ISO9141 flags 0x1200");

    J2534_ULONG f1 = 0, f2 = 0;
    ok(add_filter(ch, 0x80, 0x00, &f1) == STATUS_NOERROR && f1 == 1, "filter 80/00 id 1");
    ok(add_filter(ch, 0x80, 0x80, &f2) == STATUS_NOERROR && f2 == 2, "filter 80/80 id 2");
    ok(PassThruSetProgrammingVoltage(dev, 15, 0xFFFFFFFEu) == STATUS_NOERROR,
       "pin 15 SHORT_TO_GROUND");
    ok(set_one(ch, DATA_RATE, 9600) == STATUS_NOERROR, "sconfig DATA_RATE");
    ok(set_one(ch, P4_MIN, 0) == STATUS_NOERROR, "sconfig P4_MIN");
    ok(set_one(ch, TINIL, 70) == STATUS_NOERROR, "sconfig TINIL");
    ok(set_one(ch, TWUP, 80) == STATUS_NOERROR, "sconfig TWUP");

    PASSTHRU_MSG fi, fo;
    memset(&fi, 0, sizeof fi);
    memset(&fo, 0, sizeof fo);
    memcpy(fi.Data, FAST10REQ, sizeof FAST10REQ);
    fi.DataSize = sizeof FAST10REQ;
    ok(PassThruIoctl(ch, FAST_INIT, &fi, &fo) == ERR_TIMEOUT, "fast_init returns ERR_TIMEOUT");
    ok(fo.DataSize == 0, "fast_init timeout carries zero bytes");

    ok(mock_frame_count() == 11, "exactly 11 frames before teardown");
    expect_frame(0, RESETB, sizeof RESETB, "frame 0 reset");
    expect_frame(1, IDENT, sizeof IDENT, "frame 1 identify");
    expect_frame(2, CONN12, sizeof CONN12, "frame 2 connect");
    expect_frame(3, FILT1, sizeof FILT1, "frame 3 filter 80/00");
    expect_frame(4, FILT2, sizeof FILT2, "frame 4 filter 80/80");
    expect_frame(5, PIN15, sizeof PIN15, "frame 5 pin 15 ground");
    expect_frame(6, SC_DR, sizeof SC_DR, "frame 6 DATA_RATE");
    expect_frame(7, SC_P4, sizeof SC_P4, "frame 7 P4_MIN");
    expect_frame(8, SC_TI, sizeof SC_TI, "frame 8 TINIL");
    expect_frame(9, SC_TW, sizeof SC_TW, "frame 9 TWUP");
    expect_frame(10, FAST10, sizeof FAST10, "frame 10 fast_init");

    ok(PassThruStopMsgFilter(ch, f2) == STATUS_NOERROR, "stop filter 2");
    ok(PassThruStopMsgFilter(ch, f1) == STATUS_NOERROR, "stop filter 1");
    ok(PassThruStopMsgFilter(ch, f1) == ERR_INVALID_FILTER_ID, "double stop rejected");
    ok(PassThruStopMsgFilter(ch, 99) == ERR_INVALID_FILTER_ID, "unknown filter rejected");
    ok(PassThruDisconnect(ch) == STATUS_NOERROR, "disconnect");
    ok(PassThruClose(dev) == STATUS_NOERROR, "close");
    ok(mock_frame_count() == 14, "teardown adds only stop/disconnect frames");
    expect_frame(11, STOP2, sizeof STOP2, "frame 11 stop filter 2");
    expect_frame(12, STOP1, sizeof STOP1, "frame 12 stop filter 1");
    expect_frame(13, DISC, sizeof DISC, "frame 13 disconnect");
}

/* Oversized inputs through the public PassThruIoctl path fail closed with
 * no wire traffic; an in-range large input follows the normal path. */
static void test_oversize_public_path(void)
{
    puts("--- oversized inputs fail closed on the public path ---");
    J2534_ULONG dev = open_dev();
    ok(dev != 0, "open mock device");
    J2534_ULONG ch = connect_ch(dev, 0);
    ok(ch != 0, "connect ISO9141 flags 0");
    ok(mock_frame_count() == 3, "open+connect emit 3 frames");

    uint8_t big[300], rbuf[32];
    memset(big, 0x41, sizeof big);
    SBYTE_ARRAY in, out;
    in.NumOfBytes = sizeof big;
    in.BytePtr = big;
    out.NumOfBytes = sizeof rbuf;
    out.BytePtr = rbuf;
    ok(PassThruIoctl(ch, FIVE_BAUD_INIT, &in, &out) == ERR_FAILED,
       "300-byte five-baud rejected with ERR_FAILED");
    ok(mock_frame_count() == 3, "rejected five-baud emits no frame");

    PASSTHRU_MSG fi, fo;
    memset(&fi, 0, sizeof fi);
    memset(&fo, 0, sizeof fo);
    memset(fi.Data, 0x42, 300);
    fi.DataSize = 300;
    ok(PassThruIoctl(ch, FAST_INIT, &fi, &fo) == ERR_FAILED,
       "300-byte fast_init rejected with ERR_FAILED");
    ok(mock_frame_count() == 3, "rejected fast_init emits no frame");

    PASSTHRU_MSG wm;
    J2534_ULONG nmsg = 1;
    memset(&wm, 0, sizeof wm);
    wm.DataSize = 65;
    ok(PassThruWriteMsgs(ch, &wm, &nmsg, 0) == ERR_INVALID_MSG,
       "65-byte write rejected with ERR_INVALID_MSG");
    ok(mock_frame_count() == 3, "rejected write emits no frame");

    /* 100 address bytes sit inside the constructor's supported range, so the
     * command goes out normally (and the mock answers disconnected). */
    uint8_t addr100[100];
    for (int i = 0; i < 100; i++) addr100[i] = (uint8_t)i;
    in.NumOfBytes = sizeof addr100;
    in.BytePtr = addr100;
    out.NumOfBytes = sizeof rbuf;
    out.BytePtr = rbuf;
    ok(PassThruIoctl(ch, FIVE_BAUD_INIT, &in, &out) == ERR_DEVICE_NOT_CONNECTED,
       "100-byte five-baud follows the normal path");
    ok(mock_frame_count() == 4, "in-range five-baud emits one frame");
    {
        uint8_t raw[300];
        uint8_t inner[MVCI_MAX_INNER];
        int n = mock_frame_bytes(3, raw, sizeof raw);
        int il = (n > 0) ? mvci_frame_decrypt(mock_key(), raw, n, inner, sizeof inner) : -1;
        int good = (il == 112 && inner[0] == 0x6a && inner[1] == 0x00 &&
                    inner[2] == 0x0e && inner[3] == 0x04 &&
                    memcmp(inner + 8, addr100, 100) == 0);
        ok(good, "100-byte five-baud inner shape intact");
    }

    ok(PassThruDisconnect(ch) == STATUS_NOERROR, "disconnect");
    ok(PassThruClose(dev) == STATUS_NOERROR, "close");
}

int main(void)
{
    puts("=== seq-test (mock device, exact wire sequences) ===");
    test_five_baud_idle();
    test_programming_entry();
    test_oversize_public_path();
    printf("\nseq-test: %d passed, %d failed\n\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
