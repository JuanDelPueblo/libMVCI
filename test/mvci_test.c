/* mvci_test.c — cross-platform verification of the MVCI driver.
 *
 * Same source on Linux, macOS and Windows: it calls the mvci_serial session API,
 * which sits above the platform backends (termios/OpenSSL, termios/CommonCrypto,
 * or FTDI-D2XX/BCrypt), so one test program covers all three. It is built by the
 * CMake `mvci_test` target (see CMakeLists.txt).
 *
 * Run (self-test only, no hardware):   mvci_test
 * Run (also do a live session):        Linux:   ./mvci_test /dev/ttyUSB0
 *                                      macOS:   ./mvci_test /dev/cu.usbserial-XXXX
 *                                      Windows: mvci_test.exe M-VCI
 *
 * The self-test vectors are real bytes captured from MVCI32.dll, so a pass means
 * our encode/decode path is byte-for-byte identical to the vendor DLL.
 */

#include <mvci/serial.h>
#include <mvci/j2534.h>

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;

static void hex(const char *label, const uint8_t *b, int n)
{
    printf("    %-10s", label);
    for (int i = 0; i < n; i++) printf(" %02x", b[i]);
    printf("\n");
}

static void check(const char *name, const uint8_t *got, int gotn,
                  const uint8_t *exp, int expn)
{
    int ok = (gotn == expn) && (memcmp(got, exp, gotn) == 0);
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        hex("expected", exp, expn);
        hex("got", got, gotn);
        g_fail++;
    } else {
        g_pass++;
    }
}

/* old-session key (challenge b0 cb 49 68 07 45 c8 7f a9) */
static const uint8_t KEY_OLD[8] = { 0xb0,0xcb,0x49,0x68,0x07,0x45,0xc8,0x7f };
/* frida-session key (challenge 13 c4 6c 2b ba 61 65 a1 89) */
static const uint8_t KEY_NEW[8] = { 0x13,0xc4,0x6c,0x2b,0xba,0x61,0x65,0xa1 };

static void selftest(void)
{
    uint8_t out[MVCI_MAX_FRAME];
    int n;

    puts("=== self-test (captured vectors) ===");

    /* 1. reset frame */
    {
        static const uint8_t exp[] = { 0x03, 0x00, 0x03 };
        n = mvci_frame_plain(NULL, 0, out, sizeof out);
        check("reset frame", out, n, exp, sizeof exp);
    }

    /* 2. identify frame -> 0c 00 07 00 01 4d 56 43 49 2d 54 62  ('b'==xorsum) */
    {
        static const uint8_t ident[] = { 0x07,0x00,0x01,'M','V','C','I','-','T' };
        static const uint8_t exp[]   = { 0x0c,0x00,0x07,0x00,0x01,0x4d,0x56,0x43,
                                         0x49,0x2d,0x54,0x62 };
        n = mvci_frame_plain(ident, sizeof ident, out, sizeof out);
        check("identify frame", out, n, exp, sizeof exp);
    }

    /* 3. parse challenge -> recover key (and validate checksum) */
    {
        static const uint8_t chal[] = { 0x0e,0x00,0x09,0x00,0x01,
                                        0xb0,0xcb,0x49,0x68,0x07,0x45,0xc8,0x7f,0xa9 };
        const uint8_t *pl;
        int plen = mvci_frame_payload(chal, sizeof chal, &pl);
        int ok = (plen == 11) && (pl[0] == 0x09) && (memcmp(pl + 3, KEY_OLD, 8) == 0);
        printf("[%s] parse challenge -> key\n", ok ? "PASS" : "FAIL");
        if (ok) { g_pass++; hex("key", pl + 3, 8); } else g_fail++;
    }

    /* 4. SET_CONFIG(proto=4,param=7,value=0), KEY_OLD -> real wire frame */
    {
        static const uint8_t exp[] = { 0x13,0x00,0x1a,0x7c,0xef,0xa7,0x56,0x16,0x8c,0xbc,
                                       0x3f,0x7d,0x9a,0x06,0x8e,0x58,0x87,0xe6,0x24 };
        uint8_t inner[16];
        mvci_inner_set_config(4, 7, 0, inner);
        n = mvci_frame_enc(KEY_OLD, inner, sizeof inner, out, sizeof out);
        check("set_config(7,0) wire [old key]", out, n, exp, sizeof exp);
    }

    /* 5. SET_CONFIG(proto=4,param=7,value=1), KEY_OLD */
    {
        static const uint8_t exp[] = { 0x13,0x00,0x1a,0x7c,0xef,0xa7,0x56,0x16,0x8c,0xbc,
                                       0x0a,0xbd,0x17,0x8f,0xd9,0x48,0xcf,0x57,0x6b };
        uint8_t inner[16];
        mvci_inner_set_config(4, 7, 1, inner);
        n = mvci_frame_enc(KEY_OLD, inner, sizeof inner, out, sizeof out);
        check("set_config(7,1) wire [old key]", out, n, exp, sizeof exp);
    }

    /* 6. SET_CONFIG(proto=4,param=7,value=0), KEY_NEW (frida capture) */
    {
        /* DES(0e000e0204000000)=6fa4af0b4b42d695 ; DES(0700000000000000)=c86c26f57b63d494 */
        static const uint8_t body[] = { 0x6f,0xa4,0xaf,0x0b,0x4b,0x42,0xd6,0x95,
                                        0xc8,0x6c,0x26,0xf5,0x7b,0x63,0xd4,0x94 };
        uint8_t exp[MVCI_MAX_FRAME];
        int en = mvci_frame_plain(body, sizeof body, exp, sizeof exp);
        uint8_t inner[16];
        mvci_inner_set_config(4, 7, 0, inner);
        n = mvci_frame_enc(KEY_NEW, inner, sizeof inner, out, sizeof out);
        check("set_config(7,0) wire [new key]", out, n, exp, en);
    }

    /* 7. keepalive build (inner 05 00 09 06 00 00 00 00, KEY_OLD) */
    {
        static const uint8_t ka[8] = { 0x05,0x00,0x09,0x06,0x00,0x00,0x00,0x00 };
        static const uint8_t exp[] = { 0x0b,0x00,0x31,0x18,0x19,0x2b,0x97,0x53,
                                       0x24,0xce,0x3e };
        n = mvci_frame_enc(KEY_OLD, ka, sizeof ka, out, sizeof out);
        check("keepalive wire [old key]", out, n, exp, sizeof exp);
    }

    /* 8. decrypt a real response frame -> 02 00 0e 00 00 00 00 28 */
    {
        /* wire response (KEY_NEW): 0b 00 81 07 0e b3 d5 f4 ee a6 <xorsum> */
        uint8_t frame[MVCI_MAX_FRAME];
        static const uint8_t body[] = { 0x81,0x07,0x0e,0xb3,0xd5,0xf4,0xee,0xa6 };
        int fn = mvci_frame_plain(body, sizeof body, frame, sizeof frame);
        uint8_t inner[64];
        int il = mvci_frame_decrypt(KEY_NEW, frame, fn, inner, sizeof inner);
        static const uint8_t exp[] = { 0x02,0x00,0x0e,0x00,0x00,0x00,0x00,0x28 };
        check("decrypt response [new key]", inner, il < 0 ? 0 : il, exp, sizeof exp);
    }

    /* 9. DES round-trip identity */
    {
        uint8_t a[16], b[16];
        for (int i = 0; i < 16; i++) a[i] = b[i] = (uint8_t)(i * 7 + 1);
        mvci_des_encrypt(KEY_OLD, b, 2);
        mvci_des_decrypt(KEY_OLD, b, 2);
        check("DES encrypt/decrypt round-trip", b, 16, a, 16);
    }

    /* 10. reject corrupted checksum */
    {
        uint8_t frame[] = { 0x0b,0x00,0x31,0x18,0x19,0x2b,0x97,0x53,0x24,0xce,0x00 };
        const uint8_t *pl;
        int ok = (mvci_frame_payload(frame, sizeof frame, &pl) == -1);
        printf("[%s] reject bad checksum\n", ok ? "PASS" : "FAIL");
        ok ? g_pass++ : g_fail++;
    }

    /* ---- operational inner builders vs captured live plaintext ---- */
    {
        uint8_t in[32];
        static const uint8_t exp[] = { 0x0d,0x00,0x07,0x04,0,0,0,0,0x10,0,0,0xa0,0x28,0,0,0 };
        memset(in, 0, sizeof in);
        mvci_inner_connect(4, 4096, 10400, in);
        check("inner connect(ISO14230,4096,10400)", in, 16, exp, sizeof exp);
    }
    {
        uint8_t in[32];
        static const uint8_t exp[] = { 0x10,0,0x0b,0x04,0,0,0,0x8b,0x7a,0x0e,0,0x01,0,0,0,
                                       0xc0,0xc0,0,0,0,0,0,0,0 };
        memset(in, 0, sizeof in);
        mvci_inner_start_filter(4, 0x000e7a8b, 1, 0xc0, 0xc0, in);
        check("inner start_filter(c0/c0)", in, 24, exp, sizeof exp);
    }
    {
        uint8_t in[32];
        static const uint8_t init[] = { 0x81,0x19,0xf0,0x81 };
        static const uint8_t exp[]  = { 0x0a,0,0x0e,0x05,0x04,0,0,0,0x81,0x19,0xf0,0x81,0,0,0,0 };
        memset(in, 0, sizeof in);
        mvci_inner_fast_init(4, init, sizeof init, in);
        check("inner fast_init", in, 16, exp, sizeof exp);
    }
    {
        uint8_t in[32];
        static const uint8_t msg[] = { 0x82,0x19,0xf0,0x01,0x05 };
        static const uint8_t exp[] = { 0x0e,0,0x0a,0x04,0,0,0,0,0,0,0,0x82,0x19,0xf0,0x01,0x05 };
        memset(in, 0, sizeof in);
        mvci_inner_write_msg(4, msg, sizeof msg, in);
        check("inner write_msg(82 19 f0 01 05)", in, 16, exp, sizeof exp);
    }
    {
        static const uint8_t reply[] = { 0x0f,0,0x09,0,0,0,0,0,0,0,0,
                                         0x83,0xf0,0x19,0x41,0x0d,0,0,0,0,0,0,0,0 };
        static const uint8_t expmsg[] = { 0x83,0xf0,0x19,0x41,0x0d,0x00 };
        uint8_t msg[32], rx;
        int m = mvci_parse_read_reply(reply, sizeof reply, msg, sizeof msg, &rx);
        check("parse read reply -> msg bytes", msg, m < 0 ? 0 : m, expmsg, sizeof expmsg);
    }
    {
        /* full connect frame, KEY_NEW -> captured ciphertext + checksum */
        static const uint8_t cipher[] = { 0x5b,0x81,0x15,0x2c,0xcb,0xe7,0xd9,0xc2,
                                          0x47,0x5c,0xeb,0x5c,0x21,0x70,0xf0,0xdd };
        uint8_t exp[MVCI_MAX_FRAME];
        int en = mvci_frame_plain(cipher, sizeof cipher, exp, sizeof exp);
        uint8_t in[32];
        memset(in, 0, sizeof in);
        int il = mvci_inner_connect(4, 4096, 10400, in);
        n = mvci_frame_enc(KEY_NEW, in, il, out, sizeof out);
        check("connect full frame [new key]", out, n, exp, en);
    }

    /* ---- T256 Corolla provider parity (T254/T255 captured vectors) ---- */
    {
        uint8_t in[32];
        static const uint8_t exp4[] = { 0x0e,0x00,0x0e,0x02,0x04,0x00,0x00,0x00,
                                        0x01,0x00,0x00,0x00,0x80,0x25,0x00,0x00 };
        memset(in, 0, sizeof in);
        int il = mvci_inner_set_config(4, 1, 9600, in);
        check("set_config ISO14230 uses proto 4", in, il, exp4, sizeof exp4);
    }
    {
        uint8_t in[32];
        static const uint8_t exp3[] = { 0x0e,0x00,0x0e,0x02,0x03,0x00,0x00,0x00,
                                        0x01,0x00,0x00,0x00,0x80,0x25,0x00,0x00 };
        memset(in, 0, sizeof in);
        int il = mvci_inner_set_config(3, 1, 9600, in);
        check("set_config ISO9141 uses proto 3", in, il, exp3, sizeof exp3);
    }
    {
        uint8_t in[32];
        static const uint8_t e1[] = { 0x0e,0x00,0x0e,0x02,0x03,0x00,0x00,0x00,
                                      0x01,0x00,0x00,0x00,0x80,0x25,0x00,0x00 };
        static const uint8_t e2[] = { 0x0e,0x00,0x0e,0x02,0x03,0x00,0x00,0x00,
                                      0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
        static const uint8_t e3[] = { 0x0e,0x00,0x0e,0x02,0x03,0x00,0x00,0x00,
                                      0x14,0x00,0x00,0x00,0x46,0x00,0x00,0x00 };
        static const uint8_t e4[] = { 0x0e,0x00,0x0e,0x02,0x03,0x00,0x00,0x00,
                                      0x15,0x00,0x00,0x00,0x50,0x00,0x00,0x00 };
        memset(in, 0, sizeof in);
        mvci_inner_set_config(3, 1, 9600, in);
        check("t255 DATA_RATE=9600 proto3", in, 16, e1, sizeof e1);
        mvci_inner_set_config(3, 0x0c, 0, in);
        check("t255 P4_MIN=0 proto3", in, 16, e2, sizeof e2);
        mvci_inner_set_config(3, 0x14, 70, in);
        check("t255 TINIL=70 proto3", in, 16, e3, sizeof e3);
        mvci_inner_set_config(3, 0x15, 80, in);
        check("t255 TWUP=80 proto3", in, 16, e4, sizeof e4);
    }
    {
        uint8_t in[32];
        static const uint8_t exp[] = { 0x07,0x00,0x0e,0x04,0x03,0x00,0x00,0x00,0x33 };
        memset(in, 0, sizeof in);
        uint8_t a33 = 0x33;
        int il = mvci_inner_five_baud_init(3, &a33, 1, in);
        check("t254 five-baud(0x33) plaintext", in, il, exp, sizeof exp);
    }
    {
        uint8_t in[32];
        uint8_t a33 = 0x33;
        int il = mvci_inner_five_baud_init(3, &a33, 1, in);
        static const uint8_t exp[] = { 0x13,0x00,0x7b,0x37,0x94,0x41,0xd1,0x1b,0x1b,0xda,
                                       0x2a,0x9b,0x91,0x32,0x40,0x7d,0xe7,0xf4,0xbd };
        n = mvci_frame_enc(KEY_OLD, in, (size_t)il, out, sizeof out);
        check("t254 five-baud frame 550", out, n, exp, sizeof exp);
    }
    {
        uint8_t in[32];
        int il = mvci_inner_five_baud_init(3, NULL, 0, in);
        int ok = (il < 0);
        printf("[%s] five-baud empty rejected\n", ok ? "PASS" : "FAIL");
        ok ? g_pass++ : g_fail++;
    }
    {
        uint8_t in[32];
        static const uint8_t exp[] = { 0x09,0x00,0x0d,0x0f,0x00,0x00,0x00,0xfe,0xff,0xff,0xff };
        memset(in, 0, sizeof in);
        int il = mvci_inner_prog_voltage(15, 0xFFFFFFFEu, in);
        check("t255 pin15 plaintext", in, il, exp, sizeof exp);
    }
    {
        uint8_t in[32];
        mvci_inner_prog_voltage(15, 0xFFFFFFFEu, in);
        static const uint8_t exp[] = { 0x13,0x00,0x5e,0x5b,0x11,0x67,0xe9,0x3c,0xf5,0x2c,
                                       0xc4,0x8a,0xfa,0x9b,0x6a,0x54,0x40,0xab,0x96 };
        n = mvci_frame_enc(KEY_OLD, in, 11, out, sizeof out);
        check("t255 pin15 frame 336", out, n, exp, sizeof exp);
    }
    {
        uint8_t in[32];
        static const uint8_t init10[] = { 0x00,0x00,0x13,0x00,0x01,0x00,0x27,0x6f,0x57,0xbc };
        static const uint8_t expin[] = { 0x10,0x00,0x0e,0x05,0x03,0x00,0x00,0x00,
                                         0x00,0x00,0x13,0x00,0x01,0x00,0x27,0x6f,0x57,0xbc };
        memset(in, 0, sizeof in);
        int il = mvci_inner_fast_init(3, init10, sizeof init10, in);
        check("t255 fast_init plaintext", in, il, expin, sizeof expin);
        static const uint8_t exp[] = { 0x1b,0x00,0xbd,0x62,0xce,0x9b,0x65,0x83,0xd6,0xf0,
                                       0x59,0xf0,0xa9,0xda,0xa1,0x03,0x4e,0x31,0x8e,0xae,
                                       0x65,0xbd,0xaa,0x1c,0x54,0xe6,0xaa };
        n = mvci_frame_enc(KEY_OLD, in, (size_t)il, out, sizeof out);
        check("t255 fast_init frame 438", out, n, exp, sizeof exp);
    }
    {
        static const uint8_t disc5[] = { 0x02,0x00,0x0e,0x08,0x00,0x00,0x00,0x28 };
        static const uint8_t discF[] = { 0x02,0x00,0x0e,0x08,0x00,0x00,0x00,0xf0 };
        static const uint8_t okSet[] = { 0x02,0x00,0x0e,0x00,0x00,0x00,0x00,0x28 };
        int ok = mvci_five_baud_is_disconnected(disc5, 8)
              && !mvci_five_baud_is_disconnected(discF, 8)
              && !mvci_five_baud_is_disconnected(okSet, 8)
              && (ERR_DEVICE_NOT_CONNECTED == 8);
        printf("[%s] five-baud disconnected -> error 8\n", ok ? "PASS" : "FAIL");
        ok ? g_pass++ : g_fail++;
    }
    {
        static const uint8_t discF[] = { 0x02,0x00,0x0e,0x08,0x00,0x00,0x00,0xf0 };
        static const uint8_t disc5[] = { 0x02,0x00,0x0e,0x08,0x00,0x00,0x00,0x28 };
        int ok = mvci_fast_init_is_timeout(discF, 8)
              && !mvci_fast_init_is_timeout(disc5, 8)
              && (ERR_TIMEOUT == 9);
        printf("[%s] fast_init disconnected -> error 9\n", ok ? "PASS" : "FAIL");
        ok ? g_pass++ : g_fail++;
    }
    {
        int ok = mvci_prog_voltage_is_supported(15, 0xFFFFFFFEu)
              && !mvci_prog_voltage_is_supported(15, 0)
              && !mvci_prog_voltage_is_supported(15, 0xFFFFFFFFu)
              && !mvci_prog_voltage_is_supported(15, 5000)
              && !mvci_prog_voltage_is_supported(0, 0xFFFFFFFEu)
              && !mvci_prog_voltage_is_supported(16, 0xFFFFFFFEu);
        printf("[%s] prog voltage gate pin15 only\n", ok ? "PASS" : "FAIL");
        ok ? g_pass++ : g_fail++;
    }

    printf("\nself-test: %d passed, %d failed\n\n", g_pass, g_fail);
}

/* The 12 SET_CONFIG params the vendor app sends for ISO14230 (from the live log) */
static const uint32_t CFG[12][2] = {
    { 1, 9600 }, { 7, 40 }, { 10, 10 }, { 11, 10 }, { 19, 300 }, { 20, 35 },
    { 21, 50 }, { 14, 25 }, { 15, 20 }, { 16, 20 }, { 17, 25 }, { 18, 10 }
};

static void live(const char *port)
{
    printf("=== live test on %s ===\n", port);
    mvci_ctx_t *ctx = mvci_open(port);
    if (!ctx) { fprintf(stderr, "open failed\n"); return; }

    if (mvci_handshake(ctx) != 0) { fprintf(stderr, "handshake failed\n"); mvci_close(ctx); return; }
    hex("DES key", mvci_key(ctx), 8);

    printf("connect            : %s\n", mvci_connect(ctx, 4, 4096, 10400) == 0 ? "OK" : "FAIL");
    printf("filter c0/c0       : %s\n", mvci_start_filter(ctx, 0x000e7a8b, 0xc0, 0xc0) == 0 ? "OK" : "FAIL");
    printf("filter c0/80       : %s\n", mvci_start_filter(ctx, 0x000e7a9a, 0xc0, 0x80) == 0 ? "OK" : "FAIL");
    printf("filter c0/40       : %s\n", mvci_start_filter(ctx, 0x000e7aaa, 0xc0, 0x40) == 0 ? "OK" : "FAIL");

    int cfg_ok = 0;
    for (int i = 0; i < 12; i++)
        if (mvci_set_config(ctx, CFG[i][0], CFG[i][1]) == 0) cfg_ok++;
    printf("SET_CONFIG (12)    : %d/12 ACK\n", cfg_ok);

    mvci_clear_periodic(ctx);

    uint8_t init[4] = { 0x81, 0x19, 0xf0, 0x81 };
    uint8_t fi[16];
    int k = mvci_fast_init(ctx, init, sizeof init, fi, sizeof fi);
    if (k > 0) { printf("FAST_INIT          : OK -> "); for (int i = 0; i < k; i++) printf("%02x ", fi[i]); printf("\n"); }
    else        printf("FAST_INIT          : FAIL\n");

    /* read a few OBD PIDs from the SMT ECU (address 0x19) */
    struct { uint8_t pid; const char *name; } pids[] = {
        { 0x05, "ECT" }, { 0x0f, "IAT" }, { 0x0c, "RPM" }, { 0x0d, "VSS" }
    };
    for (unsigned i = 0; i < sizeof pids / sizeof pids[0]; i++) {
        uint8_t r[32];
        int m = mvci_obd_request(ctx, 0x19, pids[i].pid, r, sizeof r, 2000);
        printf("OBD %-3s (pid %02x)   : ", pids[i].name, pids[i].pid);
        if (m <= 0) { printf("no reply\n"); continue; }
        for (int j = 0; j < m; j++) printf("%02x ", r[j]);
        if (m >= 6) {
            double v = 0;
            switch (pids[i].pid) {
                case 0x05: case 0x0f: v = r[5] - 40.0; break;       /* deg C */
                case 0x0c: v = r[5] * 64 + (m > 6 ? r[6] / 4.0 : 0); break; /* rpm */
                case 0x0d: v = r[5]; break;                          /* km/h */
            }
            printf("  -> %.1f", v);
        }
        printf("\n");
    }

    printf("keepalive          : %s\n", mvci_keepalive(ctx) == 0 ? "OK" : "no reply");
    mvci_close(ctx);
}

int main(int argc, char **argv)
{
    selftest();
    if (argc > 1) {
        live(argv[1]);
    } else {
#ifdef _WIN32
        printf("(pass a port to run the live test, e.g. mvci_test M-VCI)\n");
#else
        printf("(pass a port to run the live test, e.g. ./mvci_test /dev/ttyUSB0)\n");
#endif
    }
    return g_fail ? 1 : 0;
}
