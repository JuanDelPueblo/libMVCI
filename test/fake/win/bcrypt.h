/* Host-side stand-in for <bcrypt.h> with scripted faults. DES is real
 * (OpenSSL), so a success run is checked against captured vectors. Included
 * only by the test translation unit that also includes src/des.c. */
#ifndef FAKE_BCRYPT_H
#define FAKE_BCRYPT_H

#include <windows.h>
#include <openssl/des.h>
#include <stdlib.h>
#include <string.h>

#define BCRYPT_DES_ALGORITHM     L"DES"
#define BCRYPT_CHAINING_MODE     L"ChainingMode"
#define BCRYPT_CHAIN_MODE_ECB    L"ChainingModeECB"
#define NT_FAIL                  ((NTSTATUS)0xC0000001)

typedef struct fake_alg { int ecb; } *BCRYPT_ALG_HANDLE;
typedef struct fake_key { uint8_t key[8]; } *BCRYPT_KEY_HANDLE;

enum fake_cng_op { F_NONE, F_OPEN, F_SETPROP, F_GENKEY, F_CRYPT };

static struct {
    enum fake_cng_op fail;       /* which call returns a failure status */
    int    short_done;           /* report fewer bytes than asked */
    int    algs_open, keys_open, crypt_calls, setprop_calls;
    wchar_t prop_name[32], prop_value[32];
} fake_cng;

static NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE *h, const WCHAR *id,
                                            const WCHAR *impl, ULONG flags)
{
    (void)impl; (void)flags;
    if (fake_cng.fail == F_OPEN || wcscmp(id, BCRYPT_DES_ALGORITHM) != 0) return NT_FAIL;
    *h = calloc(1, sizeof **h);
    fake_cng.algs_open++;
    return 0;
}

static NTSTATUS BCryptSetProperty(BCRYPT_ALG_HANDLE a, const WCHAR *name, PUCHAR value,
                                  ULONG size, ULONG flags)
{
    (void)flags;
    fake_cng.setprop_calls++;
    if (fake_cng.fail == F_SETPROP) return NT_FAIL;
    /* size is in bytes of the platform WCHAR and includes the terminator */
    if (size != (wcslen((const WCHAR *)value) + 1) * sizeof(WCHAR)) return NT_FAIL;
    wcsncpy(fake_cng.prop_name, name, 31);
    wcsncpy(fake_cng.prop_value, (const WCHAR *)value, 31);
    a->ecb = wcscmp(name, BCRYPT_CHAINING_MODE) == 0 &&
             wcscmp((const WCHAR *)value, BCRYPT_CHAIN_MODE_ECB) == 0;
    return 0;
}

static NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE a, ULONG flags)
{ (void)flags; free(a); fake_cng.algs_open--; return 0; }

static NTSTATUS BCryptGenerateSymmetricKey(BCRYPT_ALG_HANDLE a, BCRYPT_KEY_HANDLE *k,
                                           PUCHAR obj, ULONG objlen, PUCHAR secret,
                                           ULONG secretlen, ULONG flags)
{
    (void)obj; (void)objlen; (void)flags;
    if (fake_cng.fail == F_GENKEY || secretlen != 8) return NT_FAIL;
    *k = calloc(1, sizeof **k);
    memcpy((*k)->key, secret, 8);
    fake_cng.keys_open++;
    (void)a;
    return 0;
}

static NTSTATUS BCryptDestroyKey(BCRYPT_KEY_HANDLE k)
{ free(k); fake_cng.keys_open--; return 0; }

static NTSTATUS fake_crypt(BCRYPT_KEY_HANDLE k, PUCHAR in, ULONG inlen, PUCHAR out,
                           ULONG outlen, ULONG *done, int enc)
{
    fake_cng.crypt_calls++;
    if (fake_cng.fail == F_CRYPT) return NT_FAIL;
    if (inlen % 8 || outlen < inlen) return NT_FAIL;
    DES_cblock kb; DES_key_schedule ks;
    memcpy(kb, k->key, 8);
    DES_set_key_unchecked(&kb, &ks);
    for (ULONG i = 0; i < inlen; i += 8)
        DES_ecb_encrypt((DES_cblock *)(in + i), (DES_cblock *)(out + i), &ks,
                        enc ? DES_ENCRYPT : DES_DECRYPT);
    *done = fake_cng.short_done ? inlen - 8 : inlen;
    return 0;
}

static NTSTATUS BCryptEncrypt(BCRYPT_KEY_HANDLE k, PUCHAR in, ULONG inlen, void *pad,
                              PUCHAR iv, ULONG ivlen, PUCHAR out, ULONG outlen,
                              ULONG *done, ULONG flags)
{
    (void)pad; (void)iv; (void)ivlen;
    if (flags != 0) return NT_FAIL;                    /* no padding */
    return fake_crypt(k, in, inlen, out, outlen, done, 1);
}

static NTSTATUS BCryptDecrypt(BCRYPT_KEY_HANDLE k, PUCHAR in, ULONG inlen, void *pad,
                              PUCHAR iv, ULONG ivlen, PUCHAR out, ULONG outlen,
                              ULONG *done, ULONG flags)
{
    (void)pad; (void)iv; (void)ivlen;
    if (flags != 0) return NT_FAIL;
    return fake_crypt(k, in, inlen, out, outlen, done, 0);
}

#endif
