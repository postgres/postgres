
#pragma once

#include "storage/bufpage.h"
#include "executor/tuptable.h"

// Used by both data only and full tuple encryption
OffsetNumber
PGTdePageAddItemExtended(Oid oid, Page page,
					Item item,
					Size size,
					OffsetNumber offsetNumber,
					int flags);

// These 6 functions are only used with full tuple encryption, including headers
// Without FULL_TUPLE_ENCRYPTION = 1, they default to NOP
void PGTdeDecryptTupFull(Page page, HeapTuple tuple);
void PGTdeDecryptTupPartial(Page page, HeapTuple tuple, unsigned from, unsigned to);
// Inplace is used for special cases where we have critical section
void PGTdeDecryptTupPartialInplace(Page page, HeapTuple tuple, unsigned from, unsigned to);
void PGTdeDecryptTupInplace(Oid tableOid, Page page, HeapTupleHeader t_data, unsigned from, unsigned to);
void PGTdeDecryptTupTo(Oid tableOid, Page page, HeapTupleHeader t_data, char* out, unsigned from, unsigned to);
void PGTdeEncryptTupInplace(Oid tableOid, char* page, HeapTupleHeader t_data, unsigned from, unsigned to);


// These 2 are only used by data only encryption
TupleTableSlot *
PGTdeExecStoreBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer);
TupleTableSlot *
PGTdeExecStorePinnedBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer);
