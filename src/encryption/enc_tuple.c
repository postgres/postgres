#include "pg_tde_defines.h"

#include "postgres.h"
#include "utils/memutils.h"

#include "access/pg_tde_tdemap.h"
#include "encryption/enc_tuple.h"
#include "encryption/enc_aes.h"
#include "storage/bufmgr.h"
#include "keyring/keyring_api.h"

#define AES_BLOCK_SIZE 16

/* ================================================================
 * ACTUAL ENCRYPTION/DECRYPTION FUNCTIONS
 * ================================================================
 *
 * data and out have to be different addresses without overlap!
 * start_offset: is the absolute location of start of data in the file
 * The only difference between enc and dec is how we calculate offsetInPage
 */

void
pg_tde_crypt(uint64_t start_offset, const char* data, uint32 data_len, char* out, RelKeysData* keys, const char* context)
{
    const uint64_t aes_start_block = start_offset / AES_BLOCK_SIZE;
    const uint64_t aes_end_block = (start_offset + data_len + (AES_BLOCK_SIZE -1)) / AES_BLOCK_SIZE;
    const uint64_t aes_block_no = start_offset % AES_BLOCK_SIZE;
    unsigned char* encKey;

    ereport(DEBUG2,
        (errmsg("%s: start_offset:%llu, data_len: %u data:%p",context?context:"", start_offset,data_len, data)));

    encKey = palloc(AES_BLOCK_SIZE * (aes_end_block - aes_start_block + 1));

    // TODO: verify key length!
    Aes128EncryptedZeroBlocks2(&(keys->internal_key[0].ctx), keys->internal_key[0].key, aes_start_block, aes_end_block, encKey);

#if ENCRYPTION_DEBUG
    /* While in active development, We are emmiting a LOG message for debug data when ENCRYPTION_DEBUG is enabled.*/
    ereport(LOG,
        (errmsg("%s: Start offset: %llu Data_Len: %u, AesBlock: %llu, BlockOffset: %llu", context?context:"", start_offset, data_len, aes_start_block, aes_block_no)));
#endif

    for(unsigned i = 0; i < data_len; ++i)
    {
#if ENCRYPTION_DEBUG > 1
        fprintf(stderr, " >> 0x%02hhX 0x%02hhX\n", v & 0xFF, (v ^ encKey[aes_block_no + i]) & 0xFF);
#endif
        out[i] = data[i] ^ encKey[aes_block_no + i];
    }
    pfree(encKey);
}

void PGTdeCryptTupInternal(Oid tableOid, BlockNumber bn, unsigned long offsetInPage, char* t_data, char* out, unsigned from, unsigned to, RelKeysData* keys)
{
    const uint64_t offsetInFile = (bn * BLCKSZ) + offsetInPage;
    pg_tde_crypt(offsetInFile, t_data + from, (to - from), out + from, keys, "CryptTupInternal");
}

void PGTdeDecryptTupInternal(Oid tableOid, BlockNumber bn, Page page, HeapTupleHeader t_data, char* out, unsigned from, unsigned to, RelKeysData* keys)
{
	const unsigned long offsetInPage = (char*)t_data - (char*)page;
#if ENCRYPTION_DEBUG
	fprintf(stderr, " >> DECRYPTING ");
#endif
	PGTdeCryptTupInternal(tableOid, bn, offsetInPage, (char*)t_data, out, from, to, keys);
}

// t_data and out have to be different addresses without overlap!
void PGTdeEncryptTupInternal(Oid tableOid, BlockNumber bn, char* page, char* t_data, char* out, unsigned from, unsigned to, RelKeysData* keys) 
{
	const unsigned long offsetInPage = out - page;
#if ENCRYPTION_DEBUG
	fprintf(stderr, " >> ENCRYPTING ");
#endif
	PGTdeCryptTupInternal(tableOid, bn, offsetInPage, t_data, out, from, to, keys);
}

// ================================================================
// HELPER FUNCTIONS FOR ENCRYPTION
// ================================================================

// Assumtions:
// t_data is set
// t_len is valid (at most the actual length ; less is okay for partial)
// t_tableOid is set
static void PGTdeDecryptTupInternal2(BlockNumber bn, Page page, HeapTuple tuple, unsigned from, unsigned to, bool allocNew, RelKeysData* keys)
{
	char* newPtr = (char*)tuple->t_data;

	if(allocNew)
	{
		MemoryContext oldctx = MemoryContextSwitchTo(CurTransactionContext);

		newPtr = palloc(tuple->t_len);
		memcpy(newPtr, tuple->t_data, tuple->t_len);

		MemoryContextSwitchTo(oldctx);
	}

	PGTdeDecryptTupInternal(tuple->t_tableOid, bn, page, tuple->t_data, newPtr, from, to, keys);

	if(allocNew)
	{
		tuple->t_data = (HeapTupleHeader)newPtr;
	}
}

void PGTdeDecryptTupData(BlockNumber bn, Page page, HeapTuple tuple, RelKeysData* keys) 
{
	PGTdeDecryptTupInternal2(bn, page, tuple, tuple->t_data->t_hoff, tuple->t_len, true, keys);
}

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
	unsigned long headerSize = ((HeapTupleHeader)item)->t_hoff;

	char* toAddr = ((char*)phdr) + phdr->pd_upper;

	RelKeysData *keys = GetRelationKeys(rel);

	PGTdeEncryptTupInternal(oid, bn, page, item, toAddr, headerSize, size, keys);

	return off;
}

TupleTableSlot *
PGTdeExecStoreBufferHeapTuple(Relation rel, HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{
    if (rel->rd_rel->relkind != RELKIND_TOASTVALUE)
    {
        Page pageHeader;
        RelKeysData *keys = GetRelationKeys(rel->rd_locator);
        pageHeader = BufferGetPage(buffer);
        PGTdeDecryptTupData(BufferGetBlockNumber(buffer), pageHeader, tuple, keys);
    }
	return  ExecStoreBufferHeapTuple(tuple, slot, buffer);
}

TupleTableSlot *
PGTdeExecStorePinnedBufferHeapTuple(Relation rel, HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{
    if (rel->rd_rel->relkind != RELKIND_TOASTVALUE)
    {
        Page pageHeader;
        RelKeysData *keys = GetRelationKeys(rel->rd_locator);
        pageHeader = BufferGetPage(buffer);
        PGTdeDecryptTupData(BufferGetBlockNumber(buffer), pageHeader, tuple, keys);
    }
	return  ExecStorePinnedBufferHeapTuple(tuple, slot, buffer);
}
