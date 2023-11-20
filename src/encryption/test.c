#include "encryption/enc_aes.h"

#include <stdio.h>
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

int main()
{
	AesInit();




	fprintf(stderr, "Testing full E-D\n");
	fprintf(stderr, "------------------------\n");
	fprintf(stderr, "Input: %s\n", data);

	AesEncrypt(hardcoded_key, hardcoded_iv, (unsigned char*)data, 32, output, &out_len);
	AesDecrypt(hardcoded_key, hardcoded_iv, output, 32, output2, &out2_len);

	fprintf(stderr, "E-D: %s\n", (char*)output2);



	fprintf(stderr, "\nTesting partial D, decrypting only second block\n");
	fprintf(stderr, "------------------------\n");

	memset(output2, 0, 32);
	AesDecrypt(hardcoded_key, hardcoded_iv2, output+16, 16, output2, &out2_len);

	fprintf(stderr, "-D: %s\n", (char*)output2);

	fprintf(stderr, "\nTesting D using xor, decrypting only first block\n");
	fprintf(stderr, "------------------------\n");

	Aes128EncryptedZeroBlocks(NULL, hardcoded_key, 0, 0, output2);
	for(int i = 0; i < 16; ++i) {
		char c = output2[i] ^ output[i];
		fprintf(stderr, "%c", c);
	}
	fprintf(stderr, "\n");

	fprintf(stderr, "\nTesting D using xor, decrypting only second block\n");
	fprintf(stderr, "------------------------\n");

	Aes128EncryptedZeroBlocks(NULL, hardcoded_key, 1, 1, output2);
	for(int i = 0; i < 16; ++i) {
		char c = output2[i] ^ output[i+16];
		fprintf(stderr, "%c", c);
	}
	fprintf(stderr, "\n");

	return 0;
}
