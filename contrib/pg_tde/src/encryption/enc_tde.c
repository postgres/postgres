#include "pg_tde_defines.h"

#include "postgres.h"

#include "access/pg_tde_tdemap.h"
#include "encryption/enc_tde.h"
#include "encryption/enc_aes.h"
#include "storage/bufmgr.h"
#include "keyring/keyring_api.h"

#define AES_BLOCK_SIZE 		        16
#define NUM_AES_BLOCKS_IN_BATCH     200
#define DATA_BYTES_PER_AES_BATCH    (NUM_AES_BLOCKS_IN_BATCH * AES_BLOCK_SIZE)

#ifdef ENCRYPTION_DEBUG
static void
iv_prefix_debug(const char *iv_prefix, char *out_hex)
{
	for (int i = 0; i < 16; ++i)
	{
		sprintf(out_hex + i * 2, "%02x", (int) *(iv_prefix + i));
	}
	out_hex[32] = 0;
}
#endif

/*
 * ================================================================
 * ACTUAL ENCRYPTION/DECRYPTION FUNCTIONS
 * ================================================================
 */

/*
 * pg_tde_crypt_simple:
 * Encrypts/decrypts `data` with a given `key`. The result is written to `out`.
 * start_offset: is the absolute location of start of data in the file.
 * This function assumes that everything is in a single block, and has an assertion ensuring this
 */
static void
pg_tde_crypt_simple(const char *iv_prefix, uint32 start_offset, const char *data, uint32 data_len, char *out, InternalKey *key, const char *context)
{
	const uint64 aes_start_block = start_offset / AES_BLOCK_SIZE;
	const uint64 aes_end_block = (start_offset + data_len + (AES_BLOCK_SIZE - 1)) / AES_BLOCK_SIZE;
	const uint64 aes_block_no = start_offset % AES_BLOCK_SIZE;

	unsigned char enc_key[DATA_BYTES_PER_AES_BATCH + AES_BLOCK_SIZE];

	Assert(aes_end_block - aes_start_block <= NUM_AES_BLOCKS_IN_BATCH + 1);

	Aes128EncryptedZeroBlocks(&key->ctx, key->key, iv_prefix, aes_start_block, aes_end_block, enc_key);

#ifdef ENCRYPTION_DEBUG
	{
		char		ivp_debug[33];

		iv_prefix_debug(iv_prefix, ivp_debug);
		ereport(LOG,
				(errmsg("%s: Start offset: %lu Data_Len: %u, aes_start_block: %lu, aes_end_block: %lu, IV prefix: %s",
						context ? context : "", start_offset, data_len, aes_start_block, aes_end_block, ivp_debug)));
	}
#endif

	for (uint32 i = 0; i < data_len; ++i)
	{
		out[i] = data[i] ^ enc_key[i + aes_block_no];
	}
}


/*
 * pg_tde_crypt_complex:
 * Encrypts/decrypts `data` with a given `key`. The result is written to `out`.
 * start_offset: is the absolute location of start of data in the file.
 * This is a generic function intended for large data, that do not fit into a single block
 */
static void
pg_tde_crypt_complex(const char *iv_prefix, uint32 start_offset, const char *data, uint32 data_len, char *out, InternalKey *key, const char *context)
{
	const uint64 aes_start_block = start_offset / AES_BLOCK_SIZE;
	const uint64 aes_end_block = (start_offset + data_len + (AES_BLOCK_SIZE - 1)) / AES_BLOCK_SIZE;
	const uint64 aes_block_no = start_offset % AES_BLOCK_SIZE;
	uint32		batch_no = 0;
	uint32		data_index = 0;
	uint64		batch_end_block;
	uint32		current_batch_bytes;
	unsigned char enc_key[DATA_BYTES_PER_AES_BATCH];

	/* do max NUM_AES_BLOCKS_IN_BATCH blocks at a time */
	for (uint64 batch_start_block = aes_start_block; batch_start_block < aes_end_block; batch_start_block += NUM_AES_BLOCKS_IN_BATCH)
	{
		batch_end_block = Min(batch_start_block + NUM_AES_BLOCKS_IN_BATCH, aes_end_block);

		Aes128EncryptedZeroBlocks(&key->ctx, key->key, iv_prefix, batch_start_block, batch_end_block, enc_key);
#ifdef ENCRYPTION_DEBUG
		{
			char		ivp_debug[33];

			iv_prefix_debug(iv_prefix, ivp_debug);
			ereport(LOG,
					(errmsg("%s: Batch-No:%d Start offset: %lu Data_Len: %u, batch_start_block: %lu, batch_end_block: %lu, IV prefix: %s",
							context ? context : "", batch_no, start_offset, data_len, batch_start_block, batch_end_block, ivp_debug)));
		}
#endif

		current_batch_bytes = ((batch_end_block - batch_start_block) * AES_BLOCK_SIZE)
			- (batch_no > 0 ? 0 : aes_block_no);	/* first batch skips
													 * `aes_block_no`-th bytes
													 * of enc_key */
		if ((data_index + current_batch_bytes) > data_len)
			current_batch_bytes = data_len - data_index;

		for (uint32 i = 0; i < current_batch_bytes; ++i)
		{
			/*
			 * As the size of enc_key always is a multiple of 16 we start from
			 * `aes_block_no`-th index of the enc_key[] so N-th will be
			 * crypted with the same enc_key byte despite what start_offset
			 * the function was called with. For example start_offset = 10;
			 * MAX_AES_ENC_BATCH_KEY_SIZE = 6: data:                 [10 11 12
			 * 13 14 15 16] encKey: [...][0 1 2 3  4  5][0  1  2  3  4  5] so
			 * the 10th data byte is encoded with the 4th byte of the 2nd
			 * enc_key etc. We need this shift so each byte will be coded the
			 * same despite the initial offset. Let's see the same data but
			 * sent to the func starting from the offset 0: data:    [0 1 2 3
			 * 4 5 6 7 8 9 10 11 12 13 14 15 16] encKey: [0 1 2 3 4 5][0 1 2 3
			 * 4 5][ 0 1  2  3  4  5] again, the 10th data byte is encoded
			 * with the 4th byte of the 2nd enc_key etc.
			 */
			uint32		enc_key_index = i + (batch_no > 0 ? 0 : aes_block_no);

			out[data_index] = data[data_index] ^ enc_key[enc_key_index];

			data_index++;
		}
		batch_no++;
	}
}

/*
 * pg_tde_crypt:
 * Encrypts/decrypts `data` with a given `key`. The result is written to `out`.
 * start_offset: is the absolute location of start of data in the file.
 * This function simply selects between the two above variations based on the data length
 */
void
pg_tde_crypt(const char *iv_prefix, uint32 start_offset, const char *data, uint32 data_len, char *out, InternalKey *key, const char *context)
{
	if (data_len >= DATA_BYTES_PER_AES_BATCH)
	{
		pg_tde_crypt_complex(iv_prefix, start_offset, data, data_len, out, key, context);
	}
	else
	{
		pg_tde_crypt_simple(iv_prefix, start_offset, data, data_len, out, key, context);
	}
}

#ifndef FRONTEND
/*
 * Provide a simple interface to encrypt a given key.
 *
 * The function pallocs and updates the p_enc_rel_key_data along with key bytes. The memory
 * is allocated in the current memory context as this key should be ephemeral with a very
 * short lifespan until it is written to disk.
 */
void
AesEncryptKey(const TDEPrincipalKey *principal_key, Oid dbOid, InternalKey *rel_key_data, InternalKey **p_enc_rel_key_data, size_t *enc_key_bytes)
{
	unsigned char iv[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

	/* Ensure we are getting a valid pointer here */
	Assert(principal_key);

	memcpy(iv, &dbOid, sizeof(Oid));

	*p_enc_rel_key_data = palloc_object(InternalKey);
	memcpy(*p_enc_rel_key_data, rel_key_data, sizeof(InternalKey));

	AesEncrypt(principal_key->keyData, iv, (unsigned char *) rel_key_data, INTERNAL_KEY_LEN, (unsigned char *) *p_enc_rel_key_data, (int *) enc_key_bytes);
}

#endif							/* FRONTEND */

/*
 * Provide a simple interface to decrypt a given key.
 *
 * The function pallocs and updates the p_rel_key_data along with key bytes. It's important
 * to note that memory is allocated in the TopMemoryContext so we expect this to be added
 * to our key cache.
 */
void
AesDecryptKey(const TDEPrincipalKey *principal_key, Oid dbOid, InternalKey **p_rel_key_data, InternalKey *enc_rel_key_data, size_t *key_bytes)
{
	unsigned char iv[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

	/* Ensure we are getting a valid pointer here */
	Assert(principal_key);

	memcpy(iv, &dbOid, sizeof(Oid));

	*p_rel_key_data = palloc_object(InternalKey);

	/* Fill in the structure */
	memcpy(*p_rel_key_data, enc_rel_key_data, sizeof(InternalKey));
	(*p_rel_key_data)->ctx = NULL;

	AesDecrypt(principal_key->keyData, iv, (unsigned char *) enc_rel_key_data, INTERNAL_KEY_LEN, (unsigned char *) *p_rel_key_data, (int *) key_bytes);
}
