/* des_backend_fault_test.c — status handling of the Windows CNG and macOS
 * CommonCrypto DES backends, run on any host.
 *
 * src/des.c is compiled into this file with a fake backend header that can
 * fail each call. This proves that every backend failure reaches the caller as
 * -1, that the buffer is cleared, and that the checked calls feed
 * mvci_frame_enc/mvci_frame_decrypt. It does NOT prove CNG or CommonCrypto
 * behavior on Windows or macOS: those runtimes stay UNPROVEN offline.
 *
 * Build one executable per backend (no hardware, no arguments):
 *   cc -std=gnu11 -Iinclude -Isrc -Itest/fake/win -DMVCI_DES_USE_CNG \
 *      test/des_backend_fault_test.c src/serial.c test/mock_io.c \
 *      -o <dir>/des_cng_fault_test -lcrypto -lpthread
 *   cc -std=gnu11 -Iinclude -Isrc -Itest/fake/cc -DMVCI_DES_USE_COMMONCRYPTO \
 *      test/des_backend_fault_test.c src/serial.c test/mock_io.c \
 *      -o <dir>/des_commoncrypto_fault_test -lcrypto -lpthread
 */
#include "../src/des.c"

#include <stdio.h>

static int g_fail, g_pass;
#define CHECK(cond, name) do { \
    if (cond) g_pass++; else { g_fail++; printf("[FAIL] %s (line %d)\n", name, __LINE__); } \
} while (0)

/* captured vendor vectors (mvci_test.c, KEY_NEW) */
static const uint8_t KEY[8]   = { 0x13,0xc4,0x6c,0x2b,0xba,0x61,0x65,0xa1 };
static const uint8_t PLAIN[16] = { 0x0e,0x00,0x0e,0x02,0x04,0x00,0x00,0x00,
                                   0x07,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
static const uint8_t CIPHER[16] = { 0x6f,0xa4,0xaf,0x0b,0x4b,0x42,0xd6,0x95,
                                    0xc8,0x6c,0x26,0xf5,0x7b,0x63,0xd4,0x94 };

static int all_zero(const uint8_t *p, size_t n)
{ for (size_t i = 0; i < n; i++) if (p[i]) return 0; return 1; }

#ifdef MVCI_DES_USE_CNG
#  define BACKEND "CNG"
#  define NFAULTS 4
static const char *const fault_name[NFAULTS] = {
    "provider open", "chaining-mode property", "key generation", "encrypt/decrypt status" };
static void clear_faults(void) { fake_cng.fail = F_NONE; fake_cng.short_done = 0; }
static void arm(int i) { fake_cng.fail = (enum fake_cng_op)(F_OPEN + i); }
static void arm_short(void) { fake_cng.short_done = 1; }
static void reset_provider(void)
{ if (g_alg) { BCryptCloseAlgorithmProvider(g_alg, 0); g_alg = NULL; } }
static int leaked(void) { return fake_cng.keys_open != 0; }
#define MAX_BLOCKS_OVER ((size_t)0x20000000u)           /* 2^32 bytes: ULONG overflow */
#else
#  define BACKEND "CommonCrypto"
#  define NFAULTS 3
static const char *const fault_name[NFAULTS] = {
    "kCCParamError", "kCCBufferTooSmall", "unknown negative status" };
static void clear_faults(void) { fake_cc.status = 0; fake_cc.short_moved = 0; }
static void arm(int i)
{ static const CCCryptorStatus s[NFAULTS] = { kCCParamError, kCCBufferTooSmall, -9999 };
  fake_cc.status = s[i]; }
static void arm_short(void) { fake_cc.short_moved = 1; }
static void reset_provider(void) {}
static int leaked(void) { return 0; }
#define MAX_BLOCKS_OVER (SIZE_MAX / 8 + 1)
#endif

int main(void)
{
    uint8_t b[16];
    printf("=== DES backend fault test (%s fake; no hardware) ===\n", BACKEND);

    /* success: vendor vectors, both directions */
    reset_provider(); clear_faults();
    memcpy(b, PLAIN, 16);
    CHECK(mvci_des_encrypt_checked(KEY, b, 2) == 0 && !memcmp(b, CIPHER, 16), "encrypt vendor vector");
    CHECK(mvci_des_decrypt_checked(KEY, b, 2) == 0 && !memcmp(b, PLAIN, 16), "decrypt round trip");
    memcpy(b, PLAIN, 8); memcpy(b + 8, PLAIN, 8);
    CHECK(mvci_des_encrypt_checked(KEY, b, 2) == 0 && !memcmp(b, b + 8, 8), "ECB: equal blocks stay equal");
#ifdef MVCI_DES_USE_CNG
    CHECK(fake_cng.setprop_calls == 1 && !wcscmp(fake_cng.prop_name, BCRYPT_CHAINING_MODE) &&
          !wcscmp(fake_cng.prop_value, BCRYPT_CHAIN_MODE_ECB), "provider is set to ECB once");
    CHECK(!leaked() && fake_cng.algs_open == 1, "no key handle leaked on success");
#else
    CHECK(fake_cc.bad_args == 0, "ECB/DES/no-padding arguments");
#endif

    /* every backend failure -> -1, buffer cleared, no leak, backend recovers */
    for (int f = 0; f < NFAULTS; f++) {
        for (int dir = 0; dir < 2; dir++) {
            char name[96];
            reset_provider(); clear_faults(); arm(f);
            memcpy(b, PLAIN, 16);
            int rc = dir ? mvci_des_decrypt_checked(KEY, b, 2) : mvci_des_encrypt_checked(KEY, b, 2);
            snprintf(name, sizeof name, "%s %s fails -> -1", fault_name[f], dir ? "decrypt" : "encrypt");
            CHECK(rc == -1, name);
            snprintf(name, sizeof name, "%s %s clears the buffer", fault_name[f], dir ? "decrypt" : "encrypt");
            CHECK(all_zero(b, 16), name);
            CHECK(!leaked(), "no key handle leaked on failure");
#ifdef MVCI_DES_USE_CNG
            if (f == 1) CHECK(fake_cng.algs_open == 0 && g_alg == NULL, "failed ECB provider is closed, not cached");
#endif
            clear_faults();
            memcpy(b, PLAIN, 16);
            CHECK(mvci_des_encrypt_checked(KEY, b, 2) == 0 && !memcmp(b, CIPHER, 16), "backend recovers after a fault");
        }
    }

    /* a cached provider still reports later call failures (CNG: key/encrypt) */
    clear_faults();
    memcpy(b, PLAIN, 16);
    arm(NFAULTS - 1);
    CHECK(mvci_des_encrypt_checked(KEY, b, 2) == -1 && all_zero(b, 16), "failure with warm provider");
    clear_faults();

    /* short result */
    memcpy(b, PLAIN, 16); arm_short();
    CHECK(mvci_des_encrypt_checked(KEY, b, 2) == -1 && all_zero(b, 16), "short encrypt result -> -1, cleared");
    memcpy(b, CIPHER, 16);
    CHECK(mvci_des_decrypt_checked(KEY, b, 2) == -1 && all_zero(b, 16), "short decrypt result -> -1, cleared");
    clear_faults();

    /* argument errors leave the buffer alone and never reach the backend */
    memcpy(b, PLAIN, 16);
    CHECK(mvci_des_encrypt_checked(KEY, b, 0) == 0 && !memcmp(b, PLAIN, 16), "zero blocks is a no-op");
    CHECK(mvci_des_encrypt_checked(NULL, b, 2) == -1 && !memcmp(b, PLAIN, 16), "NULL key refused");
    CHECK(mvci_des_decrypt_checked(KEY, NULL, 2) == -1, "NULL buffer refused");
    CHECK(mvci_des_encrypt_checked(KEY, b, MAX_BLOCKS_OVER) == -1 && !memcmp(b, PLAIN, 16),
          "length beyond the backend limit refused untouched");

    /* the void wrappers also leave no plaintext after a failure */
    memcpy(b, PLAIN, 16); arm(NFAULTS - 1);
    mvci_des_encrypt(KEY, b, 2);
    CHECK(all_zero(b, 16), "void encrypt wrapper clears on failure");
    clear_faults();

    /* codec: a backend failure fails the frame, with no plaintext on the wire */
    {
        uint8_t out[MVCI_MAX_FRAME], inner[16], frame[MVCI_MAX_FRAME];
        int n = mvci_frame_enc(KEY, PLAIN, 16, frame, sizeof frame);
        CHECK(n == 19 && !memcmp(frame + 2, CIPHER, 16), "frame_enc matches vendor ciphertext");
        arm(NFAULTS - 1);
        memset(out, 0xAA, sizeof out);
        CHECK(mvci_frame_enc(KEY, PLAIN, 16, out, sizeof out) == -1 &&
              memchr(out, 0x0e, sizeof out) == NULL, "frame_enc fails and writes no plaintext frame");
        CHECK(mvci_frame_decrypt(KEY, frame, (size_t)n, inner, sizeof inner) == -1, "frame_decrypt fails");
        clear_faults();
        CHECK(mvci_frame_decrypt(KEY, frame, (size_t)n, inner, sizeof inner) == 16 &&
              !memcmp(inner, PLAIN, 16), "frame_decrypt recovers");
    }

    printf("%d passed, %d failed, 0 skipped\n", g_pass, g_fail);
    return g_fail != 0;
}
