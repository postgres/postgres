#include "pg_tde_defines.h"

#include "postgres.h"
#include "utils/memutils.h"

#include "access/pg_tde_tdemap.h"
#include "encryption/enc_tuple.h"
#include "encryption/enc_aes.h"
#include "storage/bufmgr.h"
#include "keyring/keyring_api.h"



/* 
 * ================================================================
 * ACTUAL ENCRYPTION/DECRYPTION FUNCTIONS
 * ================================================================
 */

/* 
 * pg_tde_crypt:
 * Encrypts/decrypts `data` with a given `keys`. The result is written to `out`.
 * start_offset: is the absolute location of start of data in the file.
 */
void
pg_tde_crypt(uint64 start_offset, const char* data, uint32 data_len, char* out, RelKeysData* keys, const char* context)
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

		Aes128EncryptedZeroBlocks(&(keys->internal_key[0].ctx), keys->internal_key[0].key, batch_start_block, batch_end_block, enc_key);
#ifdef ENCRYPTION_DEBUG
		ereport(LOG,
			(errmsg("%s: Batch-No:%d Start offset: %lu Data_Len: %u, batch_start_block: %lu, batch_end_block: %lu",
				context?context:"", batch_no, start_offset, data_len, batch_start_block, batch_end_block)));
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
			 * 		encKeys: [...][0 1 2 3  4  5][0  1  2  3  4  5]
			 * so the 10th data byte is encoded with the 4th byte of the 2nd enc_key etc.
			 * We need this shift so each byte will be coded the same despite
			 * the initial offset.
			 * Let's see the same data but sent to the func starting from the offset 0:
			 * 		data:    [0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16]
			 * 		encKeys: [0 1 2 3 4 5][0 1 2 3 4 5][ 0 1  2  3  4  5]
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
 * pg_tde_move_encrypted_data:
 * Re-encrypts data in one go
*/
void
pg_tde_move_encrypted_data(uint64 read_start_offset, const char* read_data,
				uint64 write_start_offset, char* write_data,
				uint32 data_len, RelKeysData* keys, const char* context)
{
	uint32	batch_len;
	uint32	bytes_left;
	uint32	curr_offset = 0;
	char 	decrypted[DATA_BYTES_PER_AES_BATCH];

	for (bytes_left = data_len; bytes_left > 0; bytes_left -= batch_len)
	{
		batch_len = Min(Min(DATA_BYTES_PER_AES_BATCH, data_len), bytes_left);
		
		pg_tde_crypt(read_start_offset + curr_offset, read_data + curr_offset, batch_len, decrypted, keys, context);
		pg_tde_crypt(write_start_offset + curr_offset, decrypted, batch_len, write_data + curr_offset, keys, context);
		curr_offset += batch_len;
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
pg_tde_crypt_tuple(BlockNumber bn, Page page, HeapTuple tuple, HeapTuple out_tuple, RelKeysData* keys, const char* context)
{
    uint32 data_len = tuple->t_len - tuple->t_data->t_hoff;
    uint64 tuple_offset_in_page = (char*)tuple->t_data - (char*)page;
    uint64 tuple_offset_in_file = (bn * BLCKSZ) + tuple_offset_in_page;
    char *tup_data = (char*)tuple->t_data + tuple->t_data->t_hoff;
    char *out_data = (char*)out_tuple->t_data + out_tuple->t_data->t_hoff;

#ifdef ENCRYPTION_DEBUG
    ereport(LOG,
        (errmsg("%s: table Oid: %u block no: %u data size: %u, tuple offset in file: %lu",
                context?context:"", tuple->t_tableOid, bn,
                data_len, tuple_offset_in_file)));
#endif
    pg_tde_crypt(tuple_offset_in_file, tup_data, data_len, out_data, keys, context);
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

	char* toAddr = ((char*)phdr) + phdr->pd_upper + header_size;
	char* data = item + header_size;
	uint64 offset_in_page = ((char*)phdr) + phdr->pd_upper - (char*)page;
	uint32	data_len = size - header_size;

	RelKeysData *keys = GetRelationKeys(rel);

	PG_TDE_ENCRYPT_PAGE_ITEM(bn, offset_in_page, data, data_len, toAddr, keys);
	return off;
}

TupleTableSlot *
PGTdeExecStoreBufferHeapTuple(Relation rel, HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{

    if (rel->rd_rel->relkind != RELKIND_TOASTVALUE)
    {
		MemoryContext oldContext;
        Page pageHeader;
		HeapTuple	decrypted_tuple;
        RelKeysData *keys = GetRelationKeys(rel->rd_locator);
        pageHeader = BufferGetPage(buffer);

		oldContext = MemoryContextSwitchTo(slot->tts_mcxt);
		decrypted_tuple = heap_copytuple(tuple);
		MemoryContextSwitchTo(oldContext);
		PG_TDE_DECRYPT_TUPLE_EX(BufferGetBlockNumber(buffer), pageHeader, tuple, decrypted_tuple, keys, "ExecStoreBuffer");
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
        Page pageHeader;
		HeapTuple	decrypted_tuple;
        RelKeysData *keys = GetRelationKeys(rel->rd_locator);
        pageHeader = BufferGetPage(buffer);

		oldContext = MemoryContextSwitchTo(slot->tts_mcxt);
		decrypted_tuple = heap_copytuple(tuple);
		MemoryContextSwitchTo(oldContext);

		PG_TDE_DECRYPT_TUPLE_EX(BufferGetBlockNumber(buffer), pageHeader, tuple, decrypted_tuple, keys, "ExecStoreBuffer");
		/* TODO: revisit this */
		tuple->t_data = decrypted_tuple->t_data;
    }
	return  ExecStorePinnedBufferHeapTuple(tuple, slot, buffer);
}
