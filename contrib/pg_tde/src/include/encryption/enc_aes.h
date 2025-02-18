/*-------------------------------------------------------------------------
 *
 * end_aes.h
 *	  AES Encryption / Decryption routines using OpenSSL
 *
 * src/include/encryption/enc_aes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ENC_AES_H
#define ENC_AES_H

#include <stdint.h>

void		AesInit(void);
extern void Aes128EncryptedZeroBlocks(void *ctxPtr, const unsigned char *key, const char *iv_prefix, uint64_t blockNumber1, uint64_t blockNumber2, unsigned char *out);

/* Only used for testing */
extern void AesEncrypt(const unsigned char *key, const unsigned char *iv, const unsigned char *in, int in_len, unsigned char *out, int *out_len);
extern void AesDecrypt(const unsigned char *key, const unsigned char *iv, const unsigned char *in, int in_len, unsigned char *out, int *out_len);

#endif							/* ENC_AES_H */
