
#pragma once

#include "storage/bufpage.h"
#include "executor/tuptable.h"

OffsetNumber
PGTdePageAddItemExtended(Page page,
					Item item,
					Size size,
					OffsetNumber offsetNumber,
					int flags);

// TODO: Empty for now
void PGTdeDecryptTupFull(Page page, HeapTuple tuple);

TupleTableSlot *
PGTdeExecStoreBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer);

TupleTableSlot *
PGTdeExecStorePinnedBufferHeapTuple(HeapTuple tuple, TupleTableSlot *slot, Buffer buffer);
