/*-------------------------------------------------------------------------
 *
 * multixactfuncs.c
 *	  Functions for accessing multixact-related data.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/multixactfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/multixact.h"
#include "funcapi.h"
#include "utils/builtins.h"

/*
 * pg_get_multixact_members
 *
 * Returns information about the MultiXactMembers of the specified
 * MultiXactId.
 */
Datum
pg_get_multixact_members(PG_FUNCTION_ARGS)
{
	typedef struct
	{
		MultiXactMember *members;
		int			nmembers;
		int			iter;
	} mxact;
	MultiXactId mxid = PG_GETARG_TRANSACTIONID(0);
	mxact	   *multi;
	FuncCallContext *funccxt;

	if (mxid < FirstMultiXactId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid MultiXactId: %u", mxid)));

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcxt;
		TupleDesc	tupdesc;

		funccxt = SRF_FIRSTCALL_INIT();
		oldcxt = MemoryContextSwitchTo(funccxt->multi_call_memory_ctx);

		multi = palloc(sizeof(mxact));
		/* no need to allow for old values here */
		multi->nmembers = GetMultiXactIdMembers(mxid, &multi->members, false,
												false);
		multi->iter = 0;

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funccxt->tuple_desc = tupdesc;
		funccxt->attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funccxt->user_fctx = multi;

		MemoryContextSwitchTo(oldcxt);
	}

	funccxt = SRF_PERCALL_SETUP();
	multi = (mxact *) funccxt->user_fctx;

	while (multi->iter < multi->nmembers)
	{
		HeapTuple	tuple;
		char	   *values[2];

		values[0] = psprintf("%u", multi->members[multi->iter].xid);
		values[1] = mxstatus_to_string(multi->members[multi->iter].status);

		tuple = BuildTupleFromCStrings(funccxt->attinmeta, values);

		multi->iter++;
		pfree(values[0]);
		SRF_RETURN_NEXT(funccxt, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funccxt);
}
