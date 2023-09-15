
#pragma once

#include <stdint.h>

void AesInit(void);


// Output size: blockCount * 16 bytes 
void Aes128EncryptedZeroBlocks(const unsigned char* key, uint64_t blockNumber1, uint64_t blockNumber2, unsigned char* out);

// Only used for testing
void AesEncrypt(const unsigned char* key, const unsigned char* iv, const unsigned char* in, int in_len, unsigned char* out, int* out_len);
void AesDecrypt(const unsigned char* key, const unsigned char* iv, const unsigned char* in, int in_len, unsigned char* out, int* out_len);
