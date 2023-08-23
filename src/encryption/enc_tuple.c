#include "access/pg_tde_defines.h"
#define ENCRYPTION_DEBUG 1

#include "postgres.h"
#include "utils/memutils.h"

#include "encryption/enc_tuple.h"
#include "encryption/enc_aes.h"
#include "storage/bufmgr.h"

// TODO: use real keys
static unsigned char hardcodedKey[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };

// ================================================================
// ACTUAL ENCRYPTION/DECRYPTION FUNCTIONS
// ================================================================

// t_data and out have to be different addresses without overlap!
// The only difference between enc and dec is how we calculate offsetInPage
static void PGTdeCryptTupInternal(Oid tableOid, BlockNumber bn, unsigned long offsetInPage, char* t_data, char* out, unsigned from, unsigned to)
{
	AesInit(); // TODO: where to move this?

	const uint64_t offsetInFile = (bn * BLCKSZ) + offsetInPage;

	const uint64_t aesBlockNumber1 = offsetInFile / 16;
	const uint64_t aesBlockNumber2 = (offsetInFile + (to-from) + 15) / 16;
	const uint64_t aesBlockOffset = offsetInFile % 16;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvla"
	unsigned char encKey[16 * (aesBlockNumber2 - aesBlockNumber1 + 1)];
#pragma GCC diagnostic pop
	Aes128EncryptedZeroBlocks(hardcodedKey, aesBlockNumber1, aesBlockNumber2, encKey);

#if ENCRYPTION_DEBUG
	fprintf(stderr, " ---- (Oid: %i, Len: %u, AesBlock: %lu, BlockOffset: %lu) ----\n", tableOid, to - from, aesBlockNumber1, aesBlockOffset);
#endif
	for(unsigned i = 0; i < to - from; ++i) {
		const char v = ((char*)(t_data))[i + from];
		char realKey = encKey[aesBlockOffset + i];
#if ENCRYPTION_DEBUG
	    fprintf(stderr, " >> 0x%02hhX 0x%02hhX\n", v & 0xFF, (v ^ realKey) & 0xFF);
#endif
		out[i + from] = v ^ realKey;
	}
}

static void PGTdeDecryptTupInternal(Oid tableOid, BlockNumber bn, Page page, HeapTupleHeader t_data, char* out, unsigned from, unsigned to)
{
#if ENCRYPTION_DEBUG
	fprintf(stderr, " >> DECRYPTING ");
#endif
	const unsigned long offsetInPage = (char*)t_data - (char*)page;
	PGTdeCryptTupInternal(tableOid, bn, offsetInPage, (char*)t_data, out, from, to);
}

// t_data and out have to be different addresses without overlap!
static void PGTdeEncryptTupInternal(Oid tableOid, BlockNumber bn, char* page, char* t_data, char* out, unsigned from, unsigned to) 
{
#if ENCRYPTION_DEBUG
	fprintf(stderr, " >> ENCRYPTING ");
#endif
	const unsigned long offsetInPage = out - page;
	PGTdeCryptTupInternal(tableOid, bn, offsetInPage, t_data, out, from, to);
}

// ================================================================
// HELPER FUNCTIONS FOR ENCRYPTION
// ================================================================

// Assumtions:
// t_data is set
// t_len is valid (at most the actual length ; less is okay for partial)
// t_tableOid is set
static void PGTdeDecryptTupInternal2(BlockNumber bn, Page page, HeapTuple tuple, unsigned from, unsigned to, bool allocNew)
{
	char* newPtr = (char*)tuple->t_data;

	if(allocNew)
	{
		MemoryContext oldctx = MemoryContextSwitchTo(CurTransactionContext);

		newPtr = palloc(tuple->t_len);
		memcpy(newPtr, tuple->t_data, tuple->t_len);

		MemoryContextSwitchTo(oldctx);
	}

	PGTdeDecryptTupInternal(tuple->t_tableOid, bn, page, tuple->t_data, newPtr, from, to);

	if(allocNew)
	{
		tuple->t_data = (HeapTupleHeader)newPtr;
	}
}

static void PGTdeDecryptTupData(BlockNumber bn, Page page, HeapTuple tuple) 
{
	PGTdeDecryptTupInternal2(bn, page, tuple, sizeof(HeapTupleHeaderData), tuple->t_len, true);
}

OffsetNumber
PGTdePageAddItemExtended(Oid oid,
					BlockNumber bn, 
					Page page,
					Item item,
					Size size,
					OffsetNumber offsetNumber,
					int flags)
{
	OffsetNumber off;
	PageHeader	phdr = (PageHeader) page;
	unsigned long headerSize = sizeof(HeapTupleHeaderData);

	off = PageAddItemExtended(page,item,size,offsetNumber,flags);

	char* toAddr = ((char*)phdr) + phdr->pd_upper;


	PGTdeEncryptTupInternal(oid, bn, page, item, toAddr, headerSize, size);

	return off;
}

TupleTableSlot *
PGTdeExecStoreBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{
	Page pageHeader;

	pageHeader = BufferGetPage(buffer);
	PGTdeDecryptTupData(BufferGetBlockNumber(buffer), pageHeader, tuple);

	return  ExecStoreBufferHeapTuple(tuple, slot, buffer);
}

TupleTableSlot *
PGTdeExecStorePinnedBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{
	Page pageHeader;

	pageHeader = BufferGetPage(buffer);
	PGTdeDecryptTupData(BufferGetBlockNumber(buffer), pageHeader, tuple);

	return  ExecStorePinnedBufferHeapTuple(tuple, slot, buffer);
}
