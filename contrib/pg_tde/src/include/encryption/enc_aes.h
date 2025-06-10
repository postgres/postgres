/*
 * AES Encryption / Decryption routines using OpenSSL
 */

#ifndef ENC_AES_H
#define ENC_AES_H

#include <stdint.h>

extern void AesInit(void);
extern void AesEncrypt(const unsigned char *key, const unsigned char *iv, const unsigned char *in, int in_len, unsigned char *out);
extern void AesDecrypt(const unsigned char *key, const unsigned char *iv, const unsigned char *in, int in_len, unsigned char *out);
extern void AesGcmEncrypt(const unsigned char *key, const unsigned char *iv, int iv_len, const unsigned char *aad, int aad_len, const unsigned char *in, int in_len, unsigned char *out, unsigned char *tag, int tag_len);
extern bool AesGcmDecrypt(const unsigned char *key, const unsigned char *iv, int iv_len, const unsigned char *aad, int aad_len, const unsigned char *in, int in_len, unsigned char *out, unsigned char *tag, int tag_len);
extern void AesCtrEncryptedZeroBlocks(void *ctxPtr, const unsigned char *key, const char *iv_prefix, uint64_t blockNumber1, uint64_t blockNumber2, unsigned char *out);

#endif							/* ENC_AES_H */
