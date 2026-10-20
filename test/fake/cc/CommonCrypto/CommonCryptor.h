/* Host-side stand-in for <CommonCrypto/CommonCryptor.h> with scripted faults.
 * DES is real (OpenSSL). It checks status handling, not CommonCrypto itself. */
#ifndef FAKE_COMMONCRYPTOR_H
#define FAKE_COMMONCRYPTOR_H

#include <openssl/des.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int32_t  CCCryptorStatus;
typedef uint32_t CCOperation, CCAlgorithm, CCOptions;
enum { kCCSuccess = 0, kCCParamError = -4300, kCCBufferTooSmall = -4301 };
enum { kCCEncrypt = 0, kCCDecrypt = 1 };
enum { kCCAlgorithmDES = 1 };
enum { kCCOptionECBMode = 2 };
#define kCCKeySizeDES 8

static struct {
    CCCryptorStatus status;      /* returned when nonzero (data is not touched) */
    int  short_moved;
    int  calls, bad_args;
} fake_cc;

static CCCryptorStatus CCCrypt(CCOperation op, CCAlgorithm alg, CCOptions opts,
                               const void *key, size_t keylen, const void *iv,
                               const void *in, size_t inlen, void *out, size_t outlen,
                               size_t *moved)
{
    fake_cc.calls++;
    (void)iv;
    /* ECB, no padding flag: whole blocks only; in place is allowed */
    if (alg != kCCAlgorithmDES || opts != kCCOptionECBMode || keylen != 8 ||
        (op != kCCEncrypt && op != kCCDecrypt) || inlen % 8 || outlen < inlen) {
        fake_cc.bad_args++;
        return kCCParamError;
    }
    if (fake_cc.status) return fake_cc.status;
    DES_cblock kb; DES_key_schedule ks;
    memcpy(kb, key, 8);
    DES_set_key_unchecked(&kb, &ks);
    for (size_t i = 0; i < inlen; i += 8)
        DES_ecb_encrypt((DES_cblock *)((const uint8_t *)in + i),
                        (DES_cblock *)((uint8_t *)out + i), &ks,
                        op == kCCEncrypt ? DES_ENCRYPT : DES_DECRYPT);
    *moved = fake_cc.short_moved ? inlen - 8 : inlen;
    return kCCSuccess;
}

#endif
