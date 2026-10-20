/* mvci_des.c — DES-ECB backend.
 *
 *   Linux  : OpenSSL libcrypto (DES_ecb_encrypt)
 *   macOS  : CommonCrypto (CCCrypt, kCCAlgorithmDES) — built into the OS,
 *            no external dependency.
 *   Windows: CNG / BCrypt (BCRYPT_DES_ALGORITHM, ECB) — built into Windows,
 *            no external dependency.
 *
 * All expose the same stateless API declared in mvci/serial.h:
 *   int  mvci_des_encrypt_checked(const uint8_t key[8], uint8_t *buf, size_t nblocks);
 *   int  mvci_des_decrypt_checked(const uint8_t key[8], uint8_t *buf, size_t nblocks);
 * The checked calls return 0 on success and -1 when the backend fails; on
 * failure the buffer is cleared, so plaintext is never left where a caller
 * could transmit it. The void mvci_des_encrypt/decrypt calls are kept as
 * wrappers with the same clear-on-failure behavior.
 *
 * The backend is chosen from the platform. A build may force one with
 * MVCI_DES_USE_CNG / MVCI_DES_USE_COMMONCRYPTO / MVCI_DES_USE_OPENSSL.
 */

#include <mvci/serial.h>
#include <stdint.h>
#include <string.h>

#if !defined(MVCI_DES_USE_CNG) && !defined(MVCI_DES_USE_COMMONCRYPTO) && \
    !defined(MVCI_DES_USE_OPENSSL)
#  if defined(_WIN32)
#    define MVCI_DES_USE_CNG 1
#  elif defined(__APPLE__)
#    define MVCI_DES_USE_COMMONCRYPTO 1
#  else
#    define MVCI_DES_USE_OPENSSL 1
#  endif
#endif

/* Backend entry point: 0 on success, -1 on any backend failure. The front
 * end below has already checked the arguments and des_max_blocks. */
static int des_backend(const uint8_t key[8], uint8_t *buf, size_t nblocks, int enc);

/* ====================================================================== */
#if defined(MVCI_DES_USE_CNG)
/* ------------------------------- Windows CNG ------------------------- */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wchar.h>
#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

/* Cache the algorithm provider (ECB) once; key handles are per-call. */
static BCRYPT_ALG_HANDLE g_alg = NULL;

static BCRYPT_ALG_HANDLE des_alg(void)
{
    /* Reads and publication use the same interlocked synchronization. The
     * per-context I/O lock does not protect this process-wide cache. */
    BCRYPT_ALG_HANDLE winner = (BCRYPT_ALG_HANDLE)
        InterlockedCompareExchangePointer((PVOID volatile *)&g_alg, NULL, NULL);
    if (winner) return winner;
    BCRYPT_ALG_HANDLE a = NULL;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&a, BCRYPT_DES_ALGORITHM, NULL, 0)))
        return NULL;
    if (!NT_SUCCESS(BCryptSetProperty(a, BCRYPT_CHAINING_MODE,
                      (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                      (ULONG)((wcslen(BCRYPT_CHAIN_MODE_ECB) + 1) * sizeof(WCHAR)), 0))) {
        BCryptCloseAlgorithmProvider(a, 0);
        return NULL;
    }
    winner = (BCRYPT_ALG_HANDLE)
        InterlockedCompareExchangePointer((PVOID volatile *)&g_alg, a, NULL);
    if (winner) {
        BCryptCloseAlgorithmProvider(a, 0);
        return winner;
    }
    return a;
}

#define DES_MAX_BLOCKS (0xFFFFFFFFu / 8)               /* CNG lengths are ULONG */

static int des_backend(const uint8_t key[8], uint8_t *buf, size_t nblocks, int enc)
{
    BCRYPT_ALG_HANDLE alg = des_alg();
    if (!alg) return -1;

    BCRYPT_KEY_HANDLE k = NULL;
    if (!NT_SUCCESS(BCryptGenerateSymmetricKey(alg, &k, NULL, 0, (PUCHAR)key, 8, 0)))
        return -1;

    ULONG done = 0;
    ULONG len = (ULONG)(nblocks * 8);
    NTSTATUS st;
    if (enc)
        st = BCryptEncrypt(k, buf, len, NULL, NULL, 0, buf, len, &done, 0);
    else
        st = BCryptDecrypt(k, buf, len, NULL, NULL, 0, buf, len, &done, 0);

    BCryptDestroyKey(k);
    return (NT_SUCCESS(st) && done == len) ? 0 : -1;
}

/* ====================================================================== */
#elif defined(MVCI_DES_USE_COMMONCRYPTO)
/* ---------------------------- macOS CommonCrypto --------------------- */
#include <CommonCrypto/CommonCryptor.h>

/* ECB, no padding. CommonCrypto processes the whole buffer in one call and
 * tolerates in-place operation (dataOut == dataIn) for the one-shot CCCrypt. */
static int des_backend(const uint8_t key[8], uint8_t *buf, size_t nblocks, int enc)
{
    size_t len = nblocks * 8, moved = 0;
    CCCryptorStatus st = CCCrypt(enc ? kCCEncrypt : kCCDecrypt,
                                 kCCAlgorithmDES, kCCOptionECBMode,
                                 key, kCCKeySizeDES, NULL, buf, len, buf, len, &moved);
    return (st == kCCSuccess && moved == len) ? 0 : -1;
}

/* ====================================================================== */
#else
/* ------------------------------- OpenSSL ----------------------------- */
#include <openssl/des.h>

/* The legacy DES_* calls return no status; only the arguments can be wrong. */
static int des_backend(const uint8_t key[8], uint8_t *buf, size_t nblocks, int enc)
{
    DES_cblock k;
    DES_key_schedule ks;
    memcpy(k, key, 8);
    DES_set_key_unchecked(&k, &ks);
    for (size_t i = 0; i < nblocks; i++)
        DES_ecb_encrypt((DES_cblock *)(buf + i * 8),
                        (DES_cblock *)(buf + i * 8), &ks,
                        enc ? DES_ENCRYPT : DES_DECRYPT);
    return 0;
}

#endif

#ifndef DES_MAX_BLOCKS
#define DES_MAX_BLOCKS (SIZE_MAX / 8)
#endif

/* ====================================================================== */
static int des_checked(const uint8_t key[8], uint8_t *buf, size_t nblocks, int enc)
{
    if (nblocks == 0) return 0;
    if (!key || !buf || nblocks > DES_MAX_BLOCKS) return -1;     /* buffer untouched */
    if (des_backend(key, buf, nblocks, enc) != 0) {
        memset(buf, 0, nblocks * 8);                   /* no plaintext left behind */
        return -1;
    }
    return 0;
}

int mvci_des_encrypt_checked(const uint8_t key[8], uint8_t *buf, size_t nblocks)
{ return des_checked(key, buf, nblocks, 1); }

int mvci_des_decrypt_checked(const uint8_t key[8], uint8_t *buf, size_t nblocks)
{ return des_checked(key, buf, nblocks, 0); }

void mvci_des_encrypt(const uint8_t key[8], uint8_t *buf, size_t nblocks)
{ (void)mvci_des_encrypt_checked(key, buf, nblocks); }

void mvci_des_decrypt(const uint8_t key[8], uint8_t *buf, size_t nblocks)
{ (void)mvci_des_decrypt_checked(key, buf, nblocks); }
