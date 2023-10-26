#include "pg_tde_defines.h"

#include "postgres.h"
#include "utils/memutils.h"

#include "access/pg_tde_tdemap.h"
#include "encryption/enc_tuple.h"
#include "encryption/enc_aes.h"
#include "storage/bufmgr.h"
#include "keyring/keyring_api.h"



/* ================================================================
 * ACTUAL ENCRYPTION/DECRYPTION FUNCTIONS
 * ================================================================
 *
 * data and out have to be different addresses without overlap!
 * start_offset: is the absolute location of start of data in the file
 * The only difference between enc and dec is how we calculate offsetInPage
 */

void
pg_tde_crypt(uint64 start_offset, const char* data, uint32 data_len, char* out, RelKeysData* keys, const char* context)
{
    uint64 aes_start_block = start_offset / AES_BLOCK_SIZE;
    uint64 aes_end_block = (start_offset + data_len + (AES_BLOCK_SIZE -1)) / AES_BLOCK_SIZE;
    uint64 aes_block_no = start_offset % AES_BLOCK_SIZE;
	unsigned char encKey[MAX_AES_ENC_BATCH_KEY_SIZE];
	uint32 batch_no = 0;

	/* do max NUM_AES_BLOCKS_IN_BATCH blocks at a time */
	for (uint64 batch_start_block = aes_start_block; batch_start_block < aes_end_block; batch_start_block += NUM_AES_BLOCKS_IN_BATCH)
	{
		uint64 batch_end_block = Min(batch_start_block + NUM_AES_BLOCKS_IN_BATCH, aes_end_block);

	    Aes128EncryptedZeroBlocks2(&(keys->internal_key[0].ctx), keys->internal_key[0].key, batch_start_block, batch_end_block, encKey);
#ifdef ENCRYPTION_DEBUG
		ereport(LOG,
			(errmsg("%s: Batch-No:%d Start offset: %lu Data_Len: %u, batch_start_block: %lu, batch_end_block: %lu",
				context?context:"", batch_no, start_offset, data_len, batch_start_block, batch_end_block)));
#endif

		for(uint32 i = 0; i < DATA_BYTES_PER_AES_BATCH; ++i)
		{
			uint32 data_index = i + (batch_no * DATA_BYTES_PER_AES_BATCH);
			uint32	enc_key_index = i + (batch_no > 0 ? 0 : aes_block_no);
			if (data_index >= data_len)
				break;

	        out[data_index] = data[data_index] ^ encKey[enc_key_index];
		}
		batch_no++;
	}
}
/*
 * pg_tde_move_encrypted_data:
 * decrypts and encrypts data in one go
*/
void
pg_tde_move_encrypted_data(uint64 read_start_offset, const char* read_data,
				uint64 write_start_offset, char* write_data,
				uint32 data_len, RelKeysData* keys, const char* context)
{
    uint64 read_aes_start_block = read_start_offset / AES_BLOCK_SIZE;
    uint64 read_aes_end_block = (read_start_offset + data_len + (AES_BLOCK_SIZE -1)) / AES_BLOCK_SIZE;
    uint64 read_aes_block_no = read_start_offset % AES_BLOCK_SIZE;
	unsigned char read_encKey[MAX_AES_ENC_BATCH_KEY_SIZE];

    uint64 write_aes_start_block = write_start_offset / AES_BLOCK_SIZE;
    uint64 write_aes_end_block = (write_start_offset + data_len + (AES_BLOCK_SIZE -1)) / AES_BLOCK_SIZE;
    uint64 write_aes_block_no = write_start_offset % AES_BLOCK_SIZE;
	unsigned char write_encKey[MAX_AES_ENC_BATCH_KEY_SIZE];

	uint64 read_batch_start_block, write_batch_start_block;
	uint32 batch_no = 0;

	/* do max NUM_AES_BLOCKS_IN_BATCH blocks at a time */
	for (read_batch_start_block = read_aes_start_block, write_batch_start_block = write_aes_start_block;
			read_batch_start_block < read_aes_end_block && write_batch_start_block < write_aes_end_block;
			read_batch_start_block += NUM_AES_BLOCKS_IN_BATCH, write_batch_start_block += NUM_AES_BLOCKS_IN_BATCH)
	{
		uint64 read_batch_end_block = Min(read_batch_start_block + NUM_AES_BLOCKS_IN_BATCH, read_aes_end_block);
		uint64 write_batch_end_block = Min(write_batch_start_block + NUM_AES_BLOCKS_IN_BATCH, write_aes_end_block);

		Aes128EncryptedZeroBlocks2(&(keys->internal_key[0].ctx), keys->internal_key[0].key, read_batch_start_block, read_batch_end_block, read_encKey);
		Aes128EncryptedZeroBlocks2(&(keys->internal_key[0].ctx), keys->internal_key[0].key, write_batch_start_block, write_batch_end_block, write_encKey);

		for(uint32 i = 0; i < DATA_BYTES_PER_AES_BATCH; ++i)
		{
			uint32 data_index = i + (batch_no * DATA_BYTES_PER_AES_BATCH);
			uint32	read_enc_key_index = i + (batch_no > 0 ? 0 : read_aes_block_no);
			uint32	write_enc_key_index = i + (batch_no > 0 ? 0 : write_aes_block_no);
			char decrypted_byte;

			if (data_index >= data_len)
				break;
	        decrypted_byte = read_data[data_index] ^ read_encKey[read_enc_key_index];
			write_data[data_index] = decrypted_byte ^ write_encKey[write_enc_key_index];
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
