#include "access/pg_tde_defines.h"
#define ENCRYPTION_DEBUG 1
#define FULL_TUPLE_ENCRYPTION 0

#include "postgres.h"

#include "encryption/enc_tuple.h"
#include "storage/bufmgr.h"

static void PGTdeDecryptTupInternal(Oid tableOid, Page page, HeapTupleHeader t_data, char* out, unsigned from, unsigned to)
{
	const int encryptionKey = tableOid;
	const unsigned long offset = (char*)t_data - (char*)page;
	const char realKey = (char)(encryptionKey + offset);

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

// Assumtions:
// t_data is set
// t_len is valid (at most the actual length ; less is okay for partial)
// t_tableOid is set
static void PGTdeDecryptTupInternal2(Page page, HeapTuple tuple, unsigned from, unsigned to, bool allocNew)
{
	char* newPtr = (char*)tuple->t_data;

	// Most of the time we can't decrypt in place, so we allocate some memory... and leek it for now :(
	if(allocNew)
	{
		newPtr = malloc(tuple->t_len);
		memcpy(newPtr, tuple->t_data, tuple->t_len);
	}

	PGTdeDecryptTupInternal(tuple->t_tableOid, page, tuple->t_data, newPtr, from, to);

	if(allocNew)
	{
		tuple->t_data = (HeapTupleHeader)newPtr;
	}
}

void PGTdeDecryptTupHeaderTo(Oid tableOid, Page page, HeapTupleHeader in, HeapTupleHeader out)
{
#if FULL_TUPLE_ENCRYPTION
	PGTdeDecryptTupInternal(tableOid, page, t_data, (char*)in, (char*)out, 0, sizeof(HeapTupleHeader));
#endif
}

void PGTdeDecryptTupFull(Page page, HeapTuple tuple) 
{
#if FULL_TUPLE_ENCRYPTION
	PGTdeDecryptTupInternal2(page, tuple, 0, tuple->t_len, true);
#endif
}

static void PGTdeDecryptTupDataOnly(Page page, HeapTuple tuple) 
{
#if !FULL_TUPLE_ENCRYPTION
	PGTdeDecryptTupInternal2(page, tuple, sizeof(HeapTupleHeaderData), tuple->t_len, true);
#endif
}

static void PGTdeEncryptTupInternal(Oid tableOid, char* page, char* t_data, char* out, unsigned from, unsigned to) 
{
	int encryptionKey = tableOid;
	const unsigned long offset = t_data - page;
	char realKey = (char)(encryptionKey + offset);

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

void PGTdeEncryptTupHeaderTo(Oid tableOid, char* page, HeapTupleHeader in, HeapTupleHeader out) 
{
#if FULL_TUPLE_ENCRYPTION
	PGTdeEncryptTupInternal(tableOid, page, (char*)t_data, (char*)out 0, 0);
#endif
}

OffsetNumber
PGTdePageAddItemExtended(Oid oid,
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

	char* addr = ((char*)phdr) + phdr->pd_upper;

#if FULL_TUPLE_ENCRYPTION
	PGTdeEncryptTupInternal(oid, page, addr, addr, 0, size);
#else
	PGTdeEncryptTupInternal(oid, page, addr, addr, headerSize, size);
#endif

	return off;
}

TupleTableSlot *
PGTdeExecStoreBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{
#if !FULL_TUPLE_ENCRYPTION
	Page pageHeader;

	pageHeader = BufferGetPage( buffer);
	PGTdeDecryptTupDataOnly(pageHeader, tuple);
#endif

	return  ExecStoreBufferHeapTuple(tuple, slot, buffer);
}

TupleTableSlot *
PGTdeExecStorePinnedBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{
#if !FULL_TUPLE_ENCRYPTION
	Page pageHeader;

	pageHeader = BufferGetPage(buffer);
	PGTdeDecryptTupDataOnly(pageHeader, tuple);
#endif 

	return  ExecStorePinnedBufferHeapTuple(tuple, slot, buffer);
}
