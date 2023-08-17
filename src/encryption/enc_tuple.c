#include "access/pg_tde_defines.h"
#define ENCRYPTION_DEBUG 1
#define FULL_TUPLE_ENCRYPTION 0

#include "postgres.h"

#include "encryption/enc_tuple.h"
#include "storage/bufmgr.h"

// ================================================================
// ACTUAL ENCRYPTION/DECRYPTION FUNCTIONS
// ================================================================

// t_data and out have to be different addresses without overlap!
static void PGTdeDecryptTupInternal(Oid tableOid, BlockNumber bn, Page page, HeapTupleHeader t_data, char* out, unsigned from, unsigned to)
{
	const int encryptionKey = tableOid;
	const unsigned long offset = (char*)t_data - (char*)page;
	const char realKey = (char)(encryptionKey + offset + bn);

#if ENCRYPTION_DEBUG
	fprintf(stderr, " ---- DECRYPTING (O: %i, L: %u, K:0x%02hhX) ----\n", encryptionKey, to - from, realKey & 0xFF);
#endif
	for(unsigned i = from; i < to; ++i) {
		const char v = ((char*)(t_data))[i];
#if ENCRYPTION_DEBUG
	    fprintf(stderr, " >> 0x%02hhX 0x%02hhX\n", v & 0xFF, (v ^ realKey) & 0xFF);
#endif
		out[i] = v ^ realKey;
	}
}

// t_data and out have to be different addresses without overlap!
static void PGTdeEncryptTupInternal(Oid tableOid, BlockNumber bn, char* page, char* t_data, char* out, unsigned from, unsigned to) 
{
	int encryptionKey = tableOid;
	const unsigned long offset = out - page; // TODO: we are assuming that we output to the page
	char realKey = (char)(encryptionKey + offset + bn);

#if ENCRYPTION_DEBUG
	fprintf(stderr, " ---- ENCRYPTING (O: %i, L: %u - K: 0x%02hhX) ----\n", encryptionKey, to - from, realKey & 0xFF);
#endif
	for(unsigned i = from; i < to; ++i) {
		const char v = ((char*)(t_data))[i];
#if ENCRYPTION_DEBUG
	    fprintf(stderr, " >> 0x%02hhX 0x%02hhX\n", v & 0xFF, (v ^ realKey) & 0xFF);
#endif
		out[i] = v ^ realKey;
	}
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

	// Most of the time we can't decrypt in place, so we allocate some memory... and leek it for now :(
	if(allocNew)
	{
		newPtr = malloc(tuple->t_len);
		memcpy(newPtr, tuple->t_data, tuple->t_len);
	}

	PGTdeDecryptTupInternal(tuple->t_tableOid, bn, page, tuple->t_data, newPtr, from, to);

	if(allocNew)
	{
		tuple->t_data = (HeapTupleHeader)newPtr;
	}
}

void PGTdeDecryptTupHeaderTo(Oid tableOid, BlockNumber bn, Page page, HeapTupleHeader in, HeapTupleHeader out)
{
#if FULL_TUPLE_ENCRYPTION
	PGTdeDecryptTupInternal(tableOid, bn, page, t_data, (char*)in, (char*)out, 0, sizeof(HeapTupleHeader));
#endif
}

void PGTdeDecryptTupFull(BlockNumber bn, Page page, HeapTuple tuple) 
{
#if FULL_TUPLE_ENCRYPTION
	PGTdeDecryptTupInternal2(bn, page, tuple, 0, tuple->t_len, true);
#endif
}

static void PGTdeDecryptTupDataOnly(BlockNumber bn, Page page, HeapTuple tuple) 
{
#if !FULL_TUPLE_ENCRYPTION
	PGTdeDecryptTupInternal2(bn, page, tuple, sizeof(HeapTupleHeaderData), tuple->t_len, true);
#endif
}


void PGTdeEncryptTupHeaderTo(Oid tableOid, BlockNumber bn, char* page, HeapTupleHeader in, HeapTupleHeader out) 
{
#if FULL_TUPLE_ENCRYPTION
	PGTdeEncryptTupInternal(tableOid, bn, page, (char*)t_data, (char*)out 0, 0);
#endif
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

#if FULL_TUPLE_ENCRYPTION
	PGTdeEncryptTupInternal(oid, bn, page, item, toAddr, 0, size);
#else
	PGTdeEncryptTupInternal(oid, bn, page, item, toAddr, headerSize, size);
#endif

	return off;
}

TupleTableSlot *
PGTdeExecStoreBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{
#if !FULL_TUPLE_ENCRYPTION
	Page pageHeader;

	pageHeader = BufferGetPage(buffer);
	PGTdeDecryptTupDataOnly(BufferGetBlockNumber(buffer), pageHeader, tuple);
#endif

	return  ExecStoreBufferHeapTuple(tuple, slot, buffer);
}

TupleTableSlot *
PGTdeExecStorePinnedBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{
#if !FULL_TUPLE_ENCRYPTION
	Page pageHeader;

	pageHeader = BufferGetPage(buffer);
	PGTdeDecryptTupDataOnly(BufferGetBlockNumber(buffer), pageHeader, tuple);
#endif 

	return  ExecStorePinnedBufferHeapTuple(tuple, slot, buffer);
}
