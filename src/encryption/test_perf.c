#include "encryption/enc_aes.h"

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

unsigned char hardcoded_key[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
unsigned char hardcoded_iv[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

unsigned char hardcoded_iv2[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };

char* data = 
"0123456789abcdef"
"ABCDEFGHIJKLMNOP";
int data_len = 32;

unsigned char output[64];
int out_len;

unsigned char output2[64];
int out2_len;

int main(int argc, char** argv)
{
	const unsigned long long full = 100000;

	unsigned long long blockSize = atoi(argv[1]) / 16; // in 16 bytes

	const unsigned long long innerIters = 8192 / blockSize / 16;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvla"
	unsigned char output2[blockSize * 16];
#pragma GCC diagnostic pop

	unsigned long long sum =  0;

	AesInit();

	fprintf(stdout, "Using bs/16 %llu %s\n", blockSize, argv[1]);
	fprintf(stdout, "All: %llu \n", full * innerIters);


	for(unsigned long long j = 0; j < full; ++j) {
		if(j % 1000 == 0) fprintf(stdout, ".");
		fflush(stdout);
		for(unsigned long long i = 0;i < innerIters; ++i) {
			unsigned long long start = j * (8192/16) + i * blockSize;
			Aes128EncryptedZeroBlocks2(NULL, hardcoded_key, start, start + blockSize, output2);
			//Aes128EncryptedZeroBlocks(hardcoded_key, start, start + blockSize, output);
			for(int k=0;k<blockSize * 16;++k) sum += output2[k];

			//assert(memcmp(output, output2, 16) == 0);
		}
	}

	fprintf(stderr, "%llu", sum);

	return 0;
}
