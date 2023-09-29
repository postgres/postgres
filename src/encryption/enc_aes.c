
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
 * * Encrypts the counter using AES-ECB
 * * XORs the data to the encrypted counter
 *
 * In our implementation, we want random access into any 16 byte part of the encrypted datafile.
 * This is doable with OpenSSL and directly using AES-CTR, by passing the offset in the correct format as IV.
 * Unfortunately this requires reinitializing the OpenSSL context for every seek, and that's a costly operation.
 * Initialization and then decryption of 8192 bytes takes just double the time of initialization and deecryption
 * of 16 bytes.
 *
 * To mitigate this, we reimplement AES-CTR using AES-ECB:
 * * We only initialize one ECB context per encryption key (e.g. table), and store this context
 * * When a new block is requested, we use this stored context to encrypt the position information
 * * And then XOR it with the data
 *
 * This is still not as fast as using 8k blocks, but already 2 orders of magnitude better than direct CTR with 
 * 16 byte blocks.
 */


const EVP_CIPHER* cipher;
const EVP_CIPHER* cipher2;
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
														   //
		cipher2 = EVP_get_cipherbyname("aes-128-ecb");

		initialized = 1;
	}
}

// TODO: a few things could be optimized in this. It's good enough for a prototype.
static void AesRun2(EVP_CIPHER_CTX** ctxPtr, int enc, const unsigned char* key, const unsigned char* iv, const unsigned char* in, int in_len, unsigned char* out, int* out_len)
{
	if (*ctxPtr == NULL)
	{
		*ctxPtr = EVP_CIPHER_CTX_new();
		EVP_CIPHER_CTX_init(*ctxPtr);

		if(EVP_CipherInit_ex(*ctxPtr, cipher2, NULL, key, iv, enc) == 0) {
			fprintf(stderr, "ERROR: EVP_CipherInit_ex failed. OpenSSL error: %s\n", ERR_error_string(ERR_get_error(), NULL));
			return;
		}
	}

	if(EVP_CipherUpdate(*ctxPtr, out, out_len, in, in_len) == 0) {
		fprintf(stderr, "ERROR: EVP_CipherUpdate failed. OpenSSL error: %s\n", ERR_error_string(ERR_get_error(), NULL));
		return;
	}
}

static void AesRun(int enc, const unsigned char* key, const unsigned char* iv, const unsigned char* in, int in_len, unsigned char* out, int* out_len)
{
	EVP_CIPHER_CTX* ctx = NULL;
	int f_len; // todo: what to do with this?
	ctx = EVP_CIPHER_CTX_new();
	EVP_CIPHER_CTX_init(ctx);


	if(EVP_CipherInit_ex(ctx, cipher, NULL, key, iv, enc) == 0) {
		fprintf(stderr, "ERROR: EVP_CipherInit_ex failed. OpenSSL error: %s\n", ERR_error_string(ERR_get_error(), NULL));
		goto cleanup;
	}

	if(EVP_CipherUpdate(ctx, out, out_len, in, in_len) == 0) {
		fprintf(stderr, "ERROR: EVP_CipherUpdate failed. OpenSSL error: %s\n", ERR_error_string(ERR_get_error(), NULL));
		goto cleanup;
	}

	if(EVP_CipherFinal_ex(ctx, out, &f_len) == 0) {
		fprintf(stderr, "ERROR: EVP_CipherFinal_ex failed. OpenSSL error: %s\n", ERR_error_string(ERR_get_error(), NULL));
		goto cleanup;
	}
	assert(f_len == 0);

cleanup:
 	EVP_CIPHER_CTX_cleanup(ctx);
 	EVP_CIPHER_CTX_free(ctx);
}

void Aes128EncryptedZeroBlocks(const unsigned char* key, uint64_t blockNumber1, uint64_t blockNumber2, unsigned char* out)
{
	unsigned char iv[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	unsigned dataLen = (blockNumber2 - blockNumber1 + 1) * 16;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvla"
	unsigned char data[dataLen];
#pragma GCC diagnostic pop
	int outLen;

	assert(blockNumber2 >= blockNumber1);

	// NOT memcpy: this is endian independent, and it's also how OpenSSL expects it
	for(int i =0; i<8;++i) {
		iv[15-i] = (blockNumber1 >> (8*i)) & 0xFF;
	}

	memset(data, 0, dataLen);

	AesRun(1, key, iv, data, dataLen, out, &outLen);
	assert(outLen == dataLen);
}

void AesEncrypt(const unsigned char* key, const unsigned char* iv, const unsigned char* in, int in_len, unsigned char* out, int* out_len)
{
	AesRun(1, key, iv, in, in_len, out, out_len);
}

void AesDecrypt(const unsigned char* key, const unsigned char* iv, const unsigned char* in, int in_len, unsigned char* out, int* out_len)
{
	AesRun(0, key, iv, in, in_len, out, out_len);
}

void Aes128EncryptedZeroBlocks2(void* ctxPtr, const unsigned char* key, uint64_t blockNumber1, uint64_t blockNumber2, unsigned char* out)
{
	unsigned char iv[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	unsigned dataLen = (blockNumber2 - blockNumber1 + 1) * 16;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvla"
	unsigned char data[dataLen];
#pragma GCC diagnostic pop
	int outLen;

	assert(blockNumber2 >= blockNumber1);

	memset(data, 0, dataLen);
	for(int j=blockNumber1;j<blockNumber2;++j)
	{
		for(int i =0; i<8;++i) {
			data[16*(j-blockNumber1)+15-i] = (j >> (8*i)) & 0xFF;
		}
	}

	AesRun2(ctxPtr, 1, key, iv, data, dataLen, out, &outLen);
	assert(outLen == dataLen);
}
