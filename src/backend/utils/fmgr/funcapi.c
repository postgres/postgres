/*-------------------------------------------------------------------------
 *
 * funcapi.c
 *	  Utility and convenience functions for fmgr functions that return
 *	  sets and/or composite types.
 *
 * Copyright (c) 2002, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include "funcapi.h"
#include "catalog/pg_type.h"
#include "utils/syscache.h"

/*
 * init_MultiFuncCall
 * Create an empty FuncCallContext data structure
 * and do some other basic Multi-function call setup
 * and error checking
 */
FuncCallContext *
init_MultiFuncCall(PG_FUNCTION_ARGS)
{
	FuncCallContext *retval;

	/*
	 * Bail if we're called in the wrong context
	 */
	if (fcinfo->resultinfo == NULL || !IsA(fcinfo->resultinfo, ReturnSetInfo))
		elog(ERROR, "function called in context that does not accept a set result");

	if (fcinfo->flinfo->fn_extra == NULL)
	{
		/*
		 * First call
		 */
		MemoryContext oldcontext;

		/* switch to the appropriate memory context */
		oldcontext = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

		/*
		 * allocate space and zero it
		 */
		retval = (FuncCallContext *) palloc(sizeof(FuncCallContext));
		MemSet(retval, 0, sizeof(FuncCallContext));

		/*
		 * initialize the elements
		 */
		retval->call_cntr = 0;
		retval->max_calls = 0;
		retval->slot = NULL;
		retval->user_fctx = NULL;
		retval->attinmeta = NULL;
		retval->fmctx = fcinfo->flinfo->fn_mcxt;

		/*
		 * save the pointer for cross-call use
		 */
		fcinfo->flinfo->fn_extra = retval;

		/* back to the original memory context */
		MemoryContextSwitchTo(oldcontext);
	}
	else	/* second and subsequent calls */
	{
		elog(ERROR, "init_MultiFuncCall may not be called more than once");

		/* never reached, but keep compiler happy */
		retval = NULL;
	}

	return retval;
}

/*
 * per_MultiFuncCall
 * 
 * Do Multi-function per-call setup
 */
FuncCallContext *
per_MultiFuncCall(PG_FUNCTION_ARGS)
{
	FuncCallContext *retval = (FuncCallContext *) fcinfo->flinfo->fn_extra;

	/* make sure we start with a fresh slot */
	if(retval->slot != NULL)
		ExecClearTuple(retval->slot);

	return retval;
}

/*
 * end_MultiFuncCall
 * Clean up after init_MultiFuncCall
 */
void
end_MultiFuncCall(PG_FUNCTION_ARGS, FuncCallContext *funcctx)
{
	MemoryContext oldcontext;

	/* unbind from fcinfo */
	fcinfo->flinfo->fn_extra = NULL;

	/*
	 * Caller is responsible to free up memory for individual
	 * struct elements other than att_in_funcinfo and elements.
	 */
	oldcontext = MemoryContextSwitchTo(funcctx->fmctx);

	if (funcctx->attinmeta != NULL)
		pfree(funcctx->attinmeta);

	pfree(funcctx);

	MemoryContextSwitchTo(oldcontext);
}

void
get_type_metadata(Oid typeid, Oid *attinfuncid, Oid *attelem)
{
	HeapTuple		typeTuple;
	Form_pg_type	typtup;

	typeTuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(typeid),
							   0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "get_type_metadata: Cache lookup of type %u failed", typeid);

	typtup = (Form_pg_type) GETSTRUCT(typeTuple);

	*attinfuncid = typtup->typinput;
	*attelem = typtup->typelem;

	ReleaseSysCache(typeTuple);
}
