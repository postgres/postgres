#include "access/pg_tde_defines.h"
#define ENCRYPTION_DEBUG 1

#include "postgres.h"

#include "encryption/enc_tuple.h"
#include "storage/bufmgr.h"

void PGTdeDecryptTupFull(Page page, HeapTuple tuple) 
{
}

// Assumtions:
// t_data is set
// t_len is valid (at most the actual length ; less is okay for partial)
// t_tableOid is set
static void PGTdeDecryptTupDataOnly(Page page, HeapTuple tuple) 
{
	// We can't decrypt in place, so we allocate some memory... and leek it for now :(
	
	char* newPtr = malloc(tuple->t_len);
	int encryptionKey = tuple->t_tableOid;
	const unsigned long headerSize = sizeof(HeapTupleHeaderData);
	const unsigned long offset = ((char*)tuple->t_data) + headerSize - page;
	char realKey = (char)(encryptionKey + offset);

	memcpy(newPtr, tuple->t_data, tuple->t_len);
	tuple->t_data = (HeapTupleHeader)newPtr;

#if ENCRYPTION_DEBUG
	fprintf(stderr, " ---- DECRYPTING (L: %lu, K:0x%02hhX) ----\n", tuple->t_len - headerSize, realKey & 0xFF);
#endif
	for(char* i = (char*)tuple->t_data + headerSize; i < ((char*)tuple->t_data) + tuple->t_len; ++i) {
#if ENCRYPTION_DEBUG
	    fprintf(stderr, " >> 0x%02hhX 0x%02hhX\n", *i & 0xFF, (*i ^ realKey) & 0xFF);
#endif
		*i ^= realKey;
	}
}

static void PGTdeEncryptTupDataOnly(Oid tableOid, unsigned long offset, char* data, unsigned long len) 
{
	int encryptionKey = tableOid;
	char realKey = (char)(encryptionKey + offset);

#if ENCRYPTION_DEBUG
	fprintf(stderr, " ---- ENCRYPTING (%lu - 0x%02hhX) ----\n", len, realKey & 0xFF);
#endif
	for(char* i = data; i < data + len; ++i) {
#if ENCRYPTION_DEBUG
	    fprintf(stderr, " >> 0x%02hhX 0x%02hhX\n", *i & 0xFF, (*i ^ realKey) & 0xFF);
#endif
		*i ^= realKey;
	}
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

	PGTdeEncryptTupDataOnly(oid, phdr->pd_upper + headerSize, ((char*)phdr) + phdr->pd_upper + headerSize, size - headerSize);

	return off;
}

TupleTableSlot *
PGTdeExecStoreBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{
	Page pageHeader;

	pageHeader = BufferGetPage( buffer);
	PGTdeDecryptTupDataOnly(pageHeader, tuple);

	return  ExecStoreBufferHeapTuple(tuple, slot, buffer);
}

TupleTableSlot *
PGTdeExecStorePinnedBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{
	Page pageHeader;

	pageHeader = BufferGetPage(buffer);
	PGTdeDecryptTupDataOnly(pageHeader, tuple);

	return  ExecStorePinnedBufferHeapTuple(tuple, slot, buffer);
}
