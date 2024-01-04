/*--------------------------------------------------------------------------
 *
 * test_copy_callbacks.c
 *		Code for testing COPY callbacks.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/test_copy_callbacks/test_copy_callbacks.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/table.h"
#include "commands/copy.h"
#include "fmgr.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

static void
to_cb(void *data, int len)
{
	ereport(NOTICE,
			(errmsg("COPY TO callback called with data \"%s\" and length %d",
					(char *) data, len)));
}

PG_FUNCTION_INFO_V1(test_copy_to_callback);
Datum
test_copy_to_callback(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	CopyToState cstate;
	int64		processed;

	cstate = BeginCopyTo(NULL, rel, NULL, RelationGetRelid(rel), NULL, false,
						 to_cb, NIL, NIL);
	processed = DoCopyTo(cstate);
	EndCopyTo(cstate);

	ereport(NOTICE, (errmsg("COPY TO callback has processed %lld rows",
							(long long) processed)));

	table_close(rel, NoLock);

	PG_RETURN_VOID();
}
