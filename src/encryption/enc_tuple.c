#include "pg_tde_defines.h"

#include "postgres.h"
#include "utils/memutils.h"

#include "access/pg_tde_tdemap.h"
#include "encryption/enc_tuple.h"
#include "encryption/enc_aes.h"
#include "storage/bufmgr.h"
#include "keyring/keyring_api.h"

// ================================================================
// ACTUAL ENCRYPTION/DECRYPTION FUNCTIONS
// ================================================================

// t_data and out have to be different addresses without overlap!
// The only difference between enc and dec is how we calculate offsetInPage
void PGTdeCryptTupInternal(Oid tableOid, BlockNumber bn, unsigned long offsetInPage, char* t_data, char* out, unsigned from, unsigned to, RelKeysData* keys)
{
	const uint64_t offsetInFile = (bn * BLCKSZ) + offsetInPage;

	const uint64_t aesBlockNumber1 = offsetInFile / 16;
	const uint64_t aesBlockNumber2 = (offsetInFile + (to-from) + 15) / 16;
	const uint64_t aesBlockOffset = offsetInFile % 16;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvla"
	unsigned char encKey[16 * (aesBlockNumber2 - aesBlockNumber1 + 1)];
#pragma GCC diagnostic pop

	AesInit(); // TODO: where to move this?

	// TODO: verify key length!
	Aes128EncryptedZeroBlocks2(&(keys->internal_key[0].ctx), keys->internal_key[0].key, aesBlockNumber1, aesBlockNumber2, encKey);

#if ENCRYPTION_DEBUG
	fprintf(stderr, " ---- (Oid: %i, Offset: %lu Len: %u, AesBlock: %lu, BlockOffset: %lu) ----\n", tableOid, offsetInPage, to - from, aesBlockNumber1, aesBlockOffset);
#endif
	for(unsigned i = 0; i < to - from; ++i) {
		const char v = ((char*)(t_data))[i + from];
		char realKey = encKey[aesBlockOffset + i];
#if ENCRYPTION_DEBUG > 1
	    fprintf(stderr, " >> 0x%02hhX 0x%02hhX\n", v & 0xFF, (v ^ realKey) & 0xFF);
#endif
		out[i + from] = v ^ realKey;
	}
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
PGTdeExecStoreBufferHeapTuple(RelFileLocator rel, HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{
	Page pageHeader;

	RelKeysData *keys = GetRelationKeys(rel);

	pageHeader = BufferGetPage(buffer);
	PGTdeDecryptTupData(BufferGetBlockNumber(buffer), pageHeader, tuple, keys);

	return  ExecStoreBufferHeapTuple(tuple, slot, buffer);
}

TupleTableSlot *
PGTdeExecStorePinnedBufferHeapTuple(RelFileLocator rel, HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{
	Page pageHeader;

	RelKeysData *keys = GetRelationKeys(rel);

	pageHeader = BufferGetPage(buffer);
	PGTdeDecryptTupData(BufferGetBlockNumber(buffer), pageHeader, tuple, keys);

	return  ExecStorePinnedBufferHeapTuple(tuple, slot, buffer);
}
