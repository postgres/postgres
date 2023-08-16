#include "access/pg_tde_defines.h"
#define ENCRYPTION_DEBUG 1

#include "postgres.h"

#include "encryption/enc_tuple.h"

void PGTdeDecryptTupFull(Page page, HeapTuple tuple) 
{
}

OffsetNumber
PGTdePageAddItemExtended(Page page,
					Item item,
					Size size,
					OffsetNumber offsetNumber,
					int flags)
{
	OffsetNumber off;
	PageHeader	phdr = (PageHeader) page;
	off = PageAddItemExtended(page,item,size,offsetNumber,flags);

	
#if ENCRYPTION_DEBUG
	fprintf(stderr, " ---- ENCRYPTING (%lu - %lu) ----\n", size, sizeof(HeapTupleHeaderData));
#endif
	unsigned long headerSize = sizeof(HeapTupleHeaderData);
	for(OffsetNumber i = phdr->pd_upper + headerSize; i < phdr->pd_upper+size; ++i) {
		char* ptr = ((char*)page) + i;
#if ENCRYPTION_DEBUG
	    fprintf(stderr, " >> %x %x\n", *ptr, *ptr ^ 17);
#endif
		*((char*)(page) + i) ^= 17;
	}

	return off;
}

TupleTableSlot *
PGTdeExecStoreBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{
	Page pageHeader;
	OffsetNumber offnum;

	//pageHeader = BufferGetPage(buffer);

#if ENCRYPTION_DEBUG
	fprintf(stderr, " ---- DECRYPTING ----\n");
#endif
	unsigned long headerSize = sizeof(HeapTupleHeaderData);
	for(char* i = (char*)tuple->t_data + headerSize; i < ((char*)tuple->t_data) + tuple->t_len; ++i) {
#if ENCRYPTION_DEBUG
	    fprintf(stderr, " >> %x %x\n", *i, *i ^ 17);
#endif
		*i ^= 17;
	}

	//HeapTuple enc_tuple = PGTdeDecrypt(tuple);
	return  ExecStoreBufferHeapTuple(tuple, slot, buffer);
}

TupleTableSlot *
PGTdeExecStorePinnedBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer)
{
	Page pageHeader;
	OffsetNumber offnum;

#if ENCRYPTION_DEBUG
	fprintf(stderr, " ---- DECRYPTING? ----\n");
#endif
	unsigned long headerSize = sizeof(HeapTupleHeaderData);
	for(char* i = (char*)tuple->t_data + headerSize; i < ((char*)tuple->t_data) + tuple->t_len; ++i) {
#if ENCRYPTION_DEBUG
	    fprintf(stderr, " >> %x %x\n", *i, *i ^ 17);
#endif
	//*i ^= 17;
	}

	//HeapTuple enc_tuple = PGTdeDecrypt(tuple);
	return  ExecStorePinnedBufferHeapTuple(tuple, slot, buffer);
}
