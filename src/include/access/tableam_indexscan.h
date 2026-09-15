/*-------------------------------------------------------------------------
 *
 * tableam_indexscan.h
 *	  Helpers for table AM index scan callbacks.
 *
 * Table AMs can use these functions to implement xs_getnext_slot callbacks.
 * See access/tableam.h.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tableam_indexscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLEAM_INDEXSCAN_H
#define TABLEAM_INDEXSCAN_H

#include "access/amapi.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "executor/tuptable.h"
#include "pgstat.h"
#include "utils/rel.h"

extern pg_attribute_cold void tableam_index_fill_ios_names(IndexScanDesc scan,
														   TupleTableSlot *slot);


/*
 * Fetch the next matching TID for the scan (or the first) through the
 * amgettuple interface.
 *
 * Returns true when a TID was found (the index AM will have saved it in
 * scan->xs_heaptid), or false when we're out of index entries.
 */
static inline bool
tableam_index_getnext_tid(IndexScanDesc scan, ScanDirection direction)
{
	bool		found;

	found = scan->indexRelation->rd_indam->amgettuple(scan, direction);

	/* Reset kill flag immediately for safety */
	scan->kill_prior_tuple = false;
	scan->xs_heap_continue = false;

	/* If we're out of index entries, we're done */
	if (!found)
		return false;

	Assert(ItemPointerIsValid(&scan->xs_heaptid));

	pgstat_count_index_tuples(scan->indexRelation, 1);

	return true;
}

/*
 * Fill an index-only scan's result slot from the data the index AM returned.
 *
 * The data is provided in either HeapTuple (xs_hitup) or IndexTuple (xs_itup)
 * format.  An index AM may fill both, in which case the heap format is used,
 * since it's a bit cheaper to fill a slot from.
 *
 * Called by table AMs from their xs_getnext_slot callbacks.
 */
static inline void
tableam_index_fill_ios_slot(IndexScanDesc scan, TupleTableSlot *slot)
{
	ExecClearTuple(slot);

	/*
	 * We must deform the tuple using the tupdesc the index AM formed it with
	 * (xs_hitupdesc or xs_itupdesc), not the slot's tupdesc.  The datums
	 * returned by the index AM must be binary compatible, but the descriptors
	 * may align each column differently in certain rare cases. (Actually,
	 * btree's "name" opclass stores cstring tuples that _aren't_ even binary
	 * compatible, in the strictest sense.  tableam_index_fill_ios_names
	 * handles that.)
	 */
	if (scan->xs_hitup)
	{
		Assert(slot->tts_tupleDescriptor->natts == scan->xs_hitupdesc->natts);

		heap_deform_tuple(scan->xs_hitup, scan->xs_hitupdesc,
						  slot->tts_values, slot->tts_isnull);
	}
	else if (scan->xs_itup)
	{
		Assert(slot->tts_tupleDescriptor->natts == scan->xs_itupdesc->natts);

		index_deform_tuple(scan->xs_itup, scan->xs_itupdesc,
						   slot->tts_values, slot->tts_isnull);

		/*
		 * Copy all name columns stored as cstrings back into NAMEDATALEN
		 * bytes of xs_name_cstring_buf.  We mark this branch as unlikely as
		 * generally "name" is used only for the system catalogs and this
		 * would have to be a user query running on those or some other user
		 * table with an index on a name column.
		 */
		if (unlikely(scan->xs_name_cstring_attnums != NULL))
			tableam_index_fill_ios_names(scan, slot);
	}
	else
		elog(ERROR, "no data returned for index-only scan");

	ExecStoreVirtualTuple(slot);
}

#endif							/* TABLEAM_INDEXSCAN_H */
