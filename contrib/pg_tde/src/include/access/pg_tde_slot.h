/*-------------------------------------------------------------------------
 *
 * tdeheap_slot.h
 *	  TupleSlot implementation for TDE
 *
 * src/include/access/pg_tde_slot.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_SLOT_H
#define PG_TDE_SLOT_H


#include "postgres.h"
#include "executor/tuptable.h"
#include "access/pg_tde_tdemap.h"
#include "utils/relcache.h"

extern PGDLLIMPORT const TupleTableSlotOps TTSOpsTDEBufferHeapTuple;

#define TTS_IS_TDE_BUFFERTUPLE(slot) ((slot)->tts_ops == &TTSOpsTDEBufferHeapTuple)

extern TupleTableSlot *PGTdeExecStorePinnedBufferHeapTuple(Relation rel,
														   HeapTuple tuple,
														   TupleTableSlot *slot,
														   Buffer buffer);
extern TupleTableSlot *PGTdeExecStoreBufferHeapTuple(Relation rel,
													 HeapTuple tuple,
													 TupleTableSlot *slot,
													 Buffer buffer);

#endif							/* PG_TDE_SLOT_H */
