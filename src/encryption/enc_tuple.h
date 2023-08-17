
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

// These 3 functions are only used with full tuple encryption, including headers
// Without FULL_TUPLE_ENCRYPTION = 1, they default to NOP
void PGTdeDecryptTupFull(Page page, HeapTuple tuple);
void PGTdeDecryptTupHeaderTo(Oid tableOid, Page page, HeapTupleHeader in, HeapTupleHeader out);
void PGTdeEncryptTupHeaderTo(Oid tableOid, char* page, HeapTupleHeader in, HeapTupleHeader out);


// These 2 are only used by data only encryption
TupleTableSlot *
PGTdeExecStoreBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer);
TupleTableSlot *
PGTdeExecStorePinnedBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer);
