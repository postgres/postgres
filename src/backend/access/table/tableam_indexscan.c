/*-------------------------------------------------------------------------
 *
 * tableam_indexscan.c
 *	  Helpers for table AM index scan callbacks.
 *
 *
 * Table AMs can use these functions to implement xs_getnext_slot callbacks.
 * See access/tableam.h.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/table/tableam_indexscan.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/tableam_indexscan.h"
#include "utils/builtins.h"

/*
 * Copy all name columns stored as cstrings back into NAMEDATALEN bytes of
 * xs_name_cstring_buf.
 *
 * Called by tableam_index_fill_ios_slot (kept out of line, since it's needed
 * only by index-only scans of indexes on "name" columns).
 */
pg_attribute_cold void
tableam_index_fill_ios_names(IndexScanDesc scan, TupleTableSlot *slot)
{
	for (int idx = 0; idx < scan->xs_name_cstring_count; idx++)
	{
		int			attnum = scan->xs_name_cstring_attnums[idx];
		Name		name;

		/* skip null Datums */
		if (slot->tts_isnull[attnum])
			continue;

		/* use namestrcpy to zero-pad all trailing bytes */
		name = (Name) (scan->xs_name_cstring_buf + idx * NAMEDATALEN);
		namestrcpy(name, DatumGetCString(slot->tts_values[attnum]));
		slot->tts_values[attnum] = NameGetDatum(name);
	}
}
