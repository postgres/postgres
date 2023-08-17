
#pragma once

#include <stdint.h>

void AesInit(void);


// Output size: blockCount * 16 bytes 
void Aes128EncryptedZeroBlocks(unsigned char* key, uint64_t blockNumber1, uint64_t blockNumber2, unsigned char* out);

// Only used for testing
void AesEncrypt(unsigned char* key, unsigned char* iv, unsigned char* in, int in_len, unsigned char* out, int* out_len);
void AesDecrypt(unsigned char* key, unsigned char* iv, unsigned char* in, int in_len, unsigned char* out, int* out_len);
