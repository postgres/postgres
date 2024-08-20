/*--------------------------------------------------------------------------
 *
 * test_multixact.c
 *		Support code for multixact testing
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/test_slru/test_multixact.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/multixact.h"
#include "access/xact.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"

PG_FUNCTION_INFO_V1(test_create_multixact);
PG_FUNCTION_INFO_V1(test_read_multixact);

/*
 * Produces multixact with 2 current xids
 */
Datum
test_create_multixact(PG_FUNCTION_ARGS)
{
	MultiXactId id;

	MultiXactIdSetOldestMember();
	id = MultiXactIdCreate(GetCurrentTransactionId(), MultiXactStatusUpdate,
						   GetCurrentTransactionId(), MultiXactStatusForShare);
	PG_RETURN_TRANSACTIONID(id);
}

/*
 * Reads given multixact after running an injection point. Discards local cache
 * to make a real read.  Tailored for multixact testing.
 */
Datum
test_read_multixact(PG_FUNCTION_ARGS)
{
	MultiXactId id = PG_GETARG_TRANSACTIONID(0);
	MultiXactMember *members;

	INJECTION_POINT("test-multixact-read");
	/* discard caches */
	AtEOXact_MultiXact();

	if (GetMultiXactIdMembers(id, &members, false, false) == -1)
		elog(ERROR, "MultiXactId not found");

	PG_RETURN_VOID();
}
