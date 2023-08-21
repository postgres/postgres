
#pragma once

#include "storage/bufpage.h"
#include "executor/tuptable.h"

/* A wrapper to encrypt a tuple before adding it to the buffer */
OffsetNumber
PGTdePageAddItemExtended(Oid oid, BlockNumber bn, Page page,
					Item item,
					Size size,
					OffsetNumber offsetNumber,
					int flags);

/* Wrapper functions for reading decrypted tuple into a given slot */
TupleTableSlot *
PGTdeExecStoreBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer);
TupleTableSlot *
PGTdeExecStorePinnedBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer);
