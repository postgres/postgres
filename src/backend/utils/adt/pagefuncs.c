/*-------------------------------------------------------------------------
 *
 * pagefuncs.c
 *	  Functions for features related to relation pages.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/pagefuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relation.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

static void check_one_relation(TupleDesc tupdesc, Tuplestorestate *tupstore,
							   Oid relid, ForkNumber single_forknum);
static void check_relation_fork(TupleDesc tupdesc, Tuplestorestate *tupstore,
								Relation relation, ForkNumber forknum);

/*
 * callback arguments for check_pages_error_callback()
 */
typedef struct CheckPagesErrorInfo
{
	char	   *path;
	BlockNumber blkno;
} CheckPagesErrorInfo;

/*
 * Error callback specific to check_relation_fork().
 */
static void
check_pages_error_callback(void *arg)
{
	CheckPagesErrorInfo *errinfo = (CheckPagesErrorInfo *) arg;

	errcontext("while checking page %u of path %s",
			   errinfo->blkno, errinfo->path);
}

/*
 * pg_relation_check_pages
 *
 * Check the state of all the pages for one or more fork types in the given
 * relation.
 */
Datum
pg_relation_check_pages(PG_FUNCTION_ARGS)
{
	Oid			relid;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	ForkNumber	forknum;

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/* handle arguments */
	if (PG_ARGISNULL(0))
	{
		/* Just leave if nothing is defined */
		PG_RETURN_VOID();
	}

	/* By default all the forks of a relation are checked */
	if (PG_ARGISNULL(1))
		forknum = InvalidForkNumber;
	else
	{
		const char *forkname = TextDatumGetCString(PG_GETARG_TEXT_PP(1));

		forknum = forkname_to_number(forkname);
	}

	relid = PG_GETARG_OID(0);

	check_one_relation(tupdesc, tupstore, relid, forknum);
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 * Perform the check on a single relation, possibly filtered with a single
 * fork.  This function will check if the given relation exists or not, as
 * a relation could be dropped after checking for the list of relations and
 * before getting here, and we don't want to error out in this case.
 */
static void
check_one_relation(TupleDesc tupdesc, Tuplestorestate *tupstore,
				   Oid relid, ForkNumber single_forknum)
{
	Relation	relation;
	ForkNumber	forknum;

	/* Check if relation exists. leaving if there is no such relation */
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
		return;

	relation = relation_open(relid, AccessShareLock);

	/*
	 * Sanity checks, returning no results if not supported.  Temporary
	 * relations and relations without storage are out of scope.
	 */
	if (!RELKIND_HAS_STORAGE(relation->rd_rel->relkind) ||
		relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
	{
		relation_close(relation, AccessShareLock);
		return;
	}

	RelationOpenSmgr(relation);

	for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
	{
		if (single_forknum != InvalidForkNumber && single_forknum != forknum)
			continue;

		if (smgrexists(relation->rd_smgr, forknum))
			check_relation_fork(tupdesc, tupstore, relation, forknum);
	}

	relation_close(relation, AccessShareLock);
}

/*
 * For a given relation and fork, do the real work of iterating over all pages
 * and doing the check.  Caller must hold an AccessShareLock lock on the given
 * relation.
 */
static void
check_relation_fork(TupleDesc tupdesc, Tuplestorestate *tupstore,
					Relation relation, ForkNumber forknum)
{
	BlockNumber blkno,
				nblocks;
	SMgrRelation smgr = relation->rd_smgr;
	char	   *path;
	CheckPagesErrorInfo errinfo;
	ErrorContextCallback errcallback;

	/* Number of output arguments in the SRF */
#define PG_CHECK_RELATION_COLS			2

	Assert(CheckRelationLockedByMe(relation, AccessShareLock, true));

	/*
	 * We remember the number of blocks here.  Since caller must hold a lock
	 * on the relation, we know that it won't be truncated while we are
	 * iterating over the blocks.  Any block added after this function started
	 * will not be checked.
	 */
	nblocks = RelationGetNumberOfBlocksInFork(relation, forknum);

	path = relpathbackend(smgr->smgr_rnode.node,
						  smgr->smgr_rnode.backend,
						  forknum);

	/*
	 * Error context to print some information about blocks and relations
	 * impacted by corruptions.
	 */
	errinfo.path = pstrdup(path);
	errinfo.blkno = 0;
	errcallback.callback = check_pages_error_callback;
	errcallback.arg = (void *) &errinfo;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	for (blkno = 0; blkno < nblocks; blkno++)
	{
		Datum		values[PG_CHECK_RELATION_COLS];
		bool		nulls[PG_CHECK_RELATION_COLS];
		int			i = 0;

		/* Update block number for the error context */
		errinfo.blkno = blkno;

		CHECK_FOR_INTERRUPTS();

		/* Check the given buffer */
		if (CheckBuffer(smgr, forknum, blkno))
			continue;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[i++] = CStringGetTextDatum(path);
		values[i++] = Int64GetDatum((int64) blkno);

		Assert(i == PG_CHECK_RELATION_COLS);

		/* Save the corrupted blocks in the tuplestore. */
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);

		pfree(path);
	}

	/* Pop the error context stack */
	error_context_stack = errcallback.previous;
}
