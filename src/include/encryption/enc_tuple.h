
#pragma once

#include "utils/rel.h"

#include "storage/bufpage.h"
#include "executor/tuptable.h"
#include "executor/tuptable.h"
#include "access/pg_tde_tdemap.h"

/* TODO: clean up external interface. Now are too much of similar functions */
void PGTdeCryptTupInternal(Oid tableOid, BlockNumber bn, unsigned long offsetInPage, char* t_data, char* out, unsigned from, unsigned to, RelKeysData* keys);
void PGTdeEncryptTupInternal(Oid tableOid, BlockNumber bn, char* page, char* t_data, char* out, unsigned from, unsigned to, RelKeysData* keys);
void PGTdeDecryptTupInternal(Oid tableOid, BlockNumber bn, Page page, HeapTupleHeader t_data, char* out, unsigned from, unsigned to, RelKeysData* keys);
void PGTdeDecryptTupData(BlockNumber bn, Page page, HeapTuple tuple, RelKeysData* keys);

/* A wrapper to encrypt a tuple before adding it to the buffer */
OffsetNumber
PGTdePageAddItemExtended(RelFileLocator rel, Oid oid, BlockNumber bn, Page page,
					Item item,
					Size size,
					OffsetNumber offsetNumber,
					int flags);

/* Wrapper functions for reading decrypted tuple into a given slot */
TupleTableSlot *
PGTdeExecStoreBufferHeapTuple(RelFileLocator rel, HeapTuple tuple, TupleTableSlot *slot, Buffer buffer);
TupleTableSlot *
PGTdeExecStorePinnedBufferHeapTuple(RelFileLocator rel, HeapTuple tuple, TupleTableSlot *slot, Buffer buffer);
