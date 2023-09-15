
#include "encryption/enc_aes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/err.h>

/* Implementation notes
 * =====================
 *
 * AES-CTR in a nutshell:
 * * Uses a counter, 0 for the first block, 1 for the next block, ...
 * * Encrypts the counter using AES
 * * XORs the data to the encrypted counter
 *
 * In the case of OpenSSL, counter is passed on as the IV. IV = complete 0s is the start, IV =  1 is the next, ...
 *
 * Since we want to encrypt/decrypt arbitrary bytes within blocks, we use it the following way:
 * 1. Retrieve/pass key to OpenSSL
 * 2. Calculate block number, pass it as IV
 * 3. Run encryption code to encrypt a completely zero block
 * 4. Encrypt or decrypt by XORing the data to the encrypted zero block
 *
 * Improvement note: 1-3 could be cached somewhere to significantly speed things up.
 */


const EVP_CIPHER* cipher;
int cipher_block_size;

void AesInit(void)
{
	static int initialized = 0;

	if(!initialized) {
		// Not sure if we need this 2, does postgres already call them somewhere?
		OpenSSL_add_all_algorithms();
		ERR_load_crypto_strings();
	
		cipher = EVP_get_cipherbyname("aes-128-ctr");
		cipher_block_size = EVP_CIPHER_block_size(cipher); // == buffer size

		initialized = 1;
	}
}

// TODO: a few things could be optimized in this. It's good enough for a prototype.
static void AesRun(int enc, const unsigned char* key, const unsigned char* iv, const unsigned char* in, int in_len, unsigned char* out, int* out_len)
{
	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	EVP_CIPHER_CTX_init(ctx);

	if(EVP_CipherInit_ex(ctx, cipher, NULL, key, iv, enc) == 0) {
		fprintf(stderr, "ERROR: EVP_CipherInit_ex failed. OpenSSL error: %s\n", ERR_error_string(ERR_get_error(), NULL));
		goto cleanup;
	}

	if(EVP_CipherUpdate(ctx, out, out_len, in, in_len) == 0) {
		fprintf(stderr, "ERROR: EVP_CipherUpdate failed. OpenSSL error: %s\n", ERR_error_string(ERR_get_error(), NULL));
		goto cleanup;
	}

	int f_len; // todo: what to do with this?
			   // TODO: this isn't good at all, could overwrite out
	if(EVP_CipherFinal_ex(ctx, out, &f_len) == 0) {
		fprintf(stderr, "ERROR: EVP_CipherFinal_ex failed. OpenSSL error: %s\n", ERR_error_string(ERR_get_error(), NULL));
		goto cleanup;
	}
	assert(f_len == 0);

cleanup:
 	EVP_CIPHER_CTX_cleanup(ctx);
 	EVP_CIPHER_CTX_free(ctx);
}

void AesEncrypt(const unsigned char* key, const unsigned char* iv, const unsigned char* in, int in_len, unsigned char* out, int* out_len)
{
	AesRun(1, key, iv, in, in_len, out, out_len);
}

void AesDecrypt(const unsigned char* key, const unsigned char* iv, const unsigned char* in, int in_len, unsigned char* out, int* out_len)
{
	AesRun(0, key, iv, in, in_len, out, out_len);
}

void Aes128EncryptedZeroBlocks(const unsigned char* key, uint64_t blockNumber1, uint64_t blockNumber2, unsigned char* out)
{
	assert(blockNumber2 >= blockNumber1);
	unsigned char iv[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	// NOT memcpy: this is endian independent, and it's also how OpenSSL expects it
	for(int i =0; i<8;++i) {
		iv[15-i] = (blockNumber1 >> (8*i)) & 0xFF;
	}

	unsigned dataLen = (blockNumber2 - blockNumber1 + 1) * 16;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvla"
	unsigned char data[dataLen];
#pragma GCC diagnostic pop
	memset(data, 0, dataLen);

	int outLen;
	AesRun(1, key, iv, data, dataLen, out, &outLen);
	assert(outLen == dataLen);
}
