#include "pg_tde_defines.h"

#include "postgres.h"
#include "utils/memutils.h"

#include "access/pg_tde_tdemap.h"
#include "encryption/enc_tde.h"
#include "encryption/enc_aes.h"
#include "storage/bufmgr.h"
#include "keyring/keyring_api.h"

#ifdef ENCRYPTION_DEBUG
static void iv_prefix_debug(const char* iv_prefix, char* out_hex)
{
	for(int i =0;i<16;++i) {
		sprintf(out_hex + i*2, "%02x", (int)*(iv_prefix + i));
	}
	out_hex[32] = 0;
}
#endif

static void
SetIVPrefix(ItemPointerData* ip, char* iv_prefix)
{
	/* We have up to 16 bytes for the entire IV
	 * The higher bytes (starting with 15) are used for the incrementing counter
	 * The lower bytes (in this case, 0..5) are used for the tuple identification
	 * Tuple identification is based on CTID, which currently is 48 bytes in
	 * postgres: 4 bytes for the block id and 2 bytes for the position id
	 */
	iv_prefix[0] = ip->ip_blkid.bi_hi / 256;
	iv_prefix[1] = ip->ip_blkid.bi_hi % 256;
	iv_prefix[2] = ip->ip_blkid.bi_lo / 256;
	iv_prefix[3] = ip->ip_blkid.bi_lo % 256;
	iv_prefix[4] = ip->ip_posid / 256;
	iv_prefix[5] = ip->ip_posid % 256;
}

/* 
 * ================================================================
 * ACTUAL ENCRYPTION/DECRYPTION FUNCTIONS
 * ================================================================
 */

/* 
 * pg_tde_crypt:
 * Encrypts/decrypts `data` with a given `key`. The result is written to `out`.
 * start_offset: is the absolute location of start of data in the file.
 */
void
pg_tde_crypt(const char* iv_prefix, uint32 start_offset, const char* data, uint32 data_len, char* out, RelKeyData* key, const char* context)
{
	uint64 aes_start_block = start_offset / AES_BLOCK_SIZE;
	uint64 aes_end_block = (start_offset + data_len + (AES_BLOCK_SIZE -1)) / AES_BLOCK_SIZE;
	uint64 aes_block_no = start_offset % AES_BLOCK_SIZE;
	uint32 batch_no = 0;
	uint32 data_index = 0;
	uint64 batch_end_block;
	uint32 current_batch_bytes;
	unsigned char enc_key[DATA_BYTES_PER_AES_BATCH];

	/* do max NUM_AES_BLOCKS_IN_BATCH blocks at a time */
	for (uint64 batch_start_block = aes_start_block; batch_start_block < aes_end_block; batch_start_block += NUM_AES_BLOCKS_IN_BATCH)
	{
		batch_end_block = Min(batch_start_block + NUM_AES_BLOCKS_IN_BATCH, aes_end_block);

		Aes128EncryptedZeroBlocks(&(key->internal_key.ctx), key->internal_key.key, iv_prefix, batch_start_block, batch_end_block, enc_key);
#ifdef ENCRYPTION_DEBUG
{
	char ivp_debug[33];
	iv_prefix_debug(iv_prefix, ivp_debug);
		ereport(LOG,
			(errmsg("%s: Batch-No:%d Start offset: %lu Data_Len: %u, batch_start_block: %lu, batch_end_block: %lu, IV prefix: %s",
				context?context:"", batch_no, start_offset, data_len, batch_start_block, batch_end_block, ivp_debug)));
}
#endif

		current_batch_bytes = ((batch_end_block - batch_start_block) * AES_BLOCK_SIZE)
										- (batch_no > 0 ? 0 : aes_block_no); /* first batch skips `aes_block_no`-th bytes of enc_key */
		if ((data_index + current_batch_bytes) > data_len)
			current_batch_bytes = data_len - data_index;

		for(uint32 i = 0; i < current_batch_bytes; ++i)
		{
			/* 
			 * As the size of enc_key always is a multiple of 16 we 
			 * start from `aes_block_no`-th index of the enc_key[]
			 * so N-th will be crypted with the same enc_key byte despite
			 * what start_offset the function was called with.
			 * For example start_offset = 10; MAX_AES_ENC_BATCH_KEY_SIZE = 6:
			 * 		data:                 [10 11 12 13 14 15 16]
			 * 		encKey: [...][0 1 2 3  4  5][0  1  2  3  4  5]
			 * so the 10th data byte is encoded with the 4th byte of the 2nd enc_key etc.
			 * We need this shift so each byte will be coded the same despite
			 * the initial offset.
			 * Let's see the same data but sent to the func starting from the offset 0:
			 * 		data:    [0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16]
			 * 		encKey: [0 1 2 3 4 5][0 1 2 3 4 5][ 0 1  2  3  4  5]
			 * again, the 10th data byte is encoded with the 4th byte of the 2nd enc_key etc.
			 */
			uint32  enc_key_index = i + (batch_no > 0 ? 0 : aes_block_no);
			out[data_index] = data[data_index] ^ enc_key[enc_key_index];

			data_index++;
		}
		batch_no++;
    }
}

/*
 * pg_tde_crypt_tuple:
 * Does the encryption/decryption of tuple data in place
 * page: Page containing the tuple, Used to calculate the offset of tuple in the page
 * tuple: HeapTuple to be encrypted/decrypted
 * out_tuple: to encrypt/decrypt into. If you want to do inplace encryption/decryption, pass tuple as out_tuple
 * context: Optional context message to be used in debug log
 * */
void
pg_tde_crypt_tuple(HeapTuple tuple, HeapTuple out_tuple, RelKeyData* key, const char* context)
{
	char iv_prefix[16] = {0};
	uint32 data_len = tuple->t_len - tuple->t_data->t_hoff;
	char *tup_data = (char*)tuple->t_data + tuple->t_data->t_hoff;
	char *out_data = (char*)out_tuple->t_data + out_tuple->t_data->t_hoff;

	SetIVPrefix(&tuple->t_self, iv_prefix);

#ifdef ENCRYPTION_DEBUG
    ereport(LOG,
        (errmsg("%s: table Oid: %u data size: %u",
                context?context:"", tuple->t_tableOid,
                data_len)));
#endif
    pg_tde_crypt(iv_prefix, 0, tup_data, data_len, out_data, key, context);
}


// ================================================================
// HELPER FUNCTIONS FOR ENCRYPTION
// ================================================================

OffsetNumber
PGTdePageAddItemExtended(RelFileLocator rel,
					Oid oid,
					BlockNumber bn, 
					Page page,
					Item item,
					Size size,
					OffsetNumber offsetNumber,
					int flags)
{
	OffsetNumber off = PageAddItemExtended(page,item,size,offsetNumber,flags);
	PageHeader	phdr = (PageHeader) page;
	unsigned long header_size = ((HeapTupleHeader)item)->t_hoff;
	char iv_prefix[16] = {0,};
	char* toAddr = ((char*)phdr) + phdr->pd_upper + header_size;
	char* data = item + header_size;
	uint32	data_len = size - header_size;
	/* ctid stored in item is incorrect (not set) at this point */
	ItemPointerData ip;
	RelKeyData *key = GetRelationKey(rel);

	ItemPointerSet(&ip, bn, off); 

	SetIVPrefix(&ip, iv_prefix);

	PG_TDE_ENCRYPT_PAGE_ITEM(iv_prefix, 0, data, data_len, toAddr, key);
	return off;
}

TupleTableSlot *
PGTdeExecStoreBufferHeapTuple(Relation rel, HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{

    if (rel->rd_rel->relkind != RELKIND_TOASTVALUE)
    {
		MemoryContext oldContext;
		HeapTuple	decrypted_tuple;
        RelKeyData *key = GetRelationKey(rel->rd_locator);

		oldContext = MemoryContextSwitchTo(slot->tts_mcxt);
		decrypted_tuple = heap_copytuple(tuple);
		MemoryContextSwitchTo(oldContext);
		PG_TDE_DECRYPT_TUPLE_EX(tuple, decrypted_tuple, key, "ExecStoreBuffer");
		/* TODO: revisit this */
		tuple->t_data = decrypted_tuple->t_data;
    }
	return  ExecStoreBufferHeapTuple(tuple, slot, buffer);
}

TupleTableSlot *
PGTdeExecStorePinnedBufferHeapTuple(Relation rel, HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{

    if (rel->rd_rel->relkind != RELKIND_TOASTVALUE)
    {
		MemoryContext oldContext;
		HeapTuple	decrypted_tuple;
        RelKeyData *key = GetRelationKey(rel->rd_locator);

		oldContext = MemoryContextSwitchTo(slot->tts_mcxt);
		decrypted_tuple = heap_copytuple(tuple);
		MemoryContextSwitchTo(oldContext);

		PG_TDE_DECRYPT_TUPLE_EX(tuple, decrypted_tuple, key, "ExecStoreBuffer");
		/* TODO: revisit this */
		tuple->t_data = decrypted_tuple->t_data;
    }
	return  ExecStorePinnedBufferHeapTuple(tuple, slot, buffer);
}

/*
 * Provide a simple interface to encrypt a given key.
 *
 * The function pallocs and updates the p_enc_rel_key_data along with key bytes. The memory
 * is allocated in the current memory context as this key should be ephemeral with a very
 * short lifespan until it is written to disk.
 */
void
AesEncryptKey(const keyInfo *master_key_info, RelKeyData *rel_key_data, RelKeyData **p_enc_rel_key_data, size_t *enc_key_bytes)
{
		unsigned char iv[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

        /* Ensure we are getting a valid pointer here */
        Assert(master_key_info);

        *p_enc_rel_key_data = (RelKeyData *) palloc(sizeof(RelKeyData));
        memcpy(*p_enc_rel_key_data, rel_key_data, sizeof(RelKeyData));

        AesEncrypt(master_key_info->data.data, iv, ((unsigned char*)&rel_key_data->internal_key), INTERNAL_KEY_LEN, ((unsigned char *)&(*p_enc_rel_key_data)->internal_key), (int *)enc_key_bytes);
}

/*
 * Provide a simple interface to decrypt a given key.
 *
 * The function pallocs and updates the p_rel_key_data along with key bytes. It's important
 * to note that memory is allocated in the TopMemoryContext so we expect this to be added
 * to our key cache.
 */
void
AesDecryptKey(const keyInfo *master_key_info, RelKeyData **p_rel_key_data, RelKeyData *enc_rel_key_data, size_t *key_bytes)
{
		unsigned char iv[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

        /* Ensure we are getting a valid pointer here */
        Assert(master_key_info);
        *p_rel_key_data = (RelKeyData *) MemoryContextAlloc(TopMemoryContext, sizeof(RelKeyData));

        /* Fill in the structure */
        memcpy(*p_rel_key_data, enc_rel_key_data, sizeof(RelKeyData));
		(*p_rel_key_data)->internal_key.ctx = NULL;

        AesDecrypt(master_key_info->data.data, iv, ((unsigned char*) &enc_rel_key_data->internal_key), INTERNAL_KEY_LEN, ((unsigned char *)&(*p_rel_key_data)->internal_key) , (int *)key_bytes);
}
