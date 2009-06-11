/*-------------------------------------------------------------------------
 *
 * functions.c
 *	  Execution of SQL-language functions
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/functions.c,v 1.135 2009/06/11 17:25:38 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/functions.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/*
 * Specialized DestReceiver for collecting query output in a SQL function
 */
typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	Tuplestorestate *tstore;	/* where to put result tuples */
	MemoryContext cxt;			/* context containing tstore */
	JunkFilter *filter;			/* filter to convert tuple type */
} DR_sqlfunction;

/*
 * We have an execution_state record for each query in a function.	Each
 * record contains a plantree for its query.  If the query is currently in
 * F_EXEC_RUN state then there's a QueryDesc too.
 */
typedef enum
{
	F_EXEC_START, F_EXEC_RUN, F_EXEC_DONE
} ExecStatus;

typedef struct execution_state
{
	struct execution_state *next;
	ExecStatus	status;
	bool		setsResult;		/* true if this query produces func's result */
	bool		lazyEval;		/* true if should fetch one row at a time */
	Node	   *stmt;			/* PlannedStmt or utility statement */
	QueryDesc  *qd;				/* null unless status == RUN */
} execution_state;


/*
 * An SQLFunctionCache record is built during the first call,
 * and linked to from the fn_extra field of the FmgrInfo struct.
 *
 * Note that currently this has only the lifespan of the calling query.
 * Someday we might want to consider caching the parse/plan results longer
 * than that.
 */
typedef struct
{
	char	   *src;			/* function body text (for error msgs) */

	Oid		   *argtypes;		/* resolved types of arguments */
	Oid			rettype;		/* actual return type */
	int16		typlen;			/* length of the return type */
	bool		typbyval;		/* true if return type is pass by value */
	bool		returnsSet;		/* true if returning multiple rows */
	bool		returnsTuple;	/* true if returning whole tuple result */
	bool		shutdown_reg;	/* true if registered shutdown callback */
	bool		readonly_func;	/* true to run in "read only" mode */
	bool		lazyEval;		/* true if using lazyEval for result query */

	ParamListInfo paramLI;		/* Param list representing current args */

	Tuplestorestate *tstore;	/* where we accumulate result tuples */

	JunkFilter *junkFilter;		/* will be NULL if function returns VOID */

	/* head of linked list of execution_state records */
	execution_state *func_state;
} SQLFunctionCache;

typedef SQLFunctionCache *SQLFunctionCachePtr;


/* non-export function prototypes */
static execution_state *init_execution_state(List *queryTree_list,
					 SQLFunctionCachePtr fcache,
					 bool lazyEvalOK);
static void init_sql_fcache(FmgrInfo *finfo, bool lazyEvalOK);
static void postquel_start(execution_state *es, SQLFunctionCachePtr fcache);
static bool postquel_getnext(execution_state *es, SQLFunctionCachePtr fcache);
static void postquel_end(execution_state *es);
static void postquel_sub_params(SQLFunctionCachePtr fcache,
					FunctionCallInfo fcinfo);
static Datum postquel_get_single_result(TupleTableSlot *slot,
						   FunctionCallInfo fcinfo,
						   SQLFunctionCachePtr fcache,
						   MemoryContext resultcontext);
static void sql_exec_error_callback(void *arg);
static void ShutdownSQLFunction(Datum arg);
static void sqlfunction_startup(DestReceiver *self, int operation, TupleDesc typeinfo);
static void sqlfunction_receive(TupleTableSlot *slot, DestReceiver *self);
static void sqlfunction_shutdown(DestReceiver *self);
static void sqlfunction_destroy(DestReceiver *self);


/* Set up the list of per-query execution_state records for a SQL function */
static execution_state *
init_execution_state(List *queryTree_list,
					 SQLFunctionCachePtr fcache,
					 bool lazyEvalOK)
{
	execution_state *firstes = NULL;
	execution_state *preves = NULL;
	execution_state *lasttages = NULL;
	ListCell   *qtl_item;

	foreach(qtl_item, queryTree_list)
	{
		Query	   *queryTree = (Query *) lfirst(qtl_item);
		Node	   *stmt;
		execution_state *newes;

		Assert(IsA(queryTree, Query));

		if (queryTree->commandType == CMD_UTILITY)
			stmt = queryTree->utilityStmt;
		else
			stmt = (Node *) pg_plan_query(queryTree, 0, NULL);

		/* Precheck all commands for validity in a function */
		if (IsA(stmt, TransactionStmt))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/* translator: %s is a SQL statement name */
					 errmsg("%s is not allowed in a SQL function",
							CreateCommandTag(stmt))));

		if (fcache->readonly_func && !CommandIsReadOnly(stmt))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/* translator: %s is a SQL statement name */
					 errmsg("%s is not allowed in a non-volatile function",
							CreateCommandTag(stmt))));

		newes = (execution_state *) palloc(sizeof(execution_state));
		if (preves)
			preves->next = newes;
		else
			firstes = newes;

		newes->next = NULL;
		newes->status = F_EXEC_START;
		newes->setsResult = false;		/* might change below */
		newes->lazyEval = false;	/* might change below */
		newes->stmt = stmt;
		newes->qd = NULL;

		if (queryTree->canSetTag)
			lasttages = newes;

		preves = newes;
	}

	/*
	 * Mark the last canSetTag query as delivering the function result; then,
	 * if it is a plain SELECT, mark it for lazy evaluation. If it's not a
	 * SELECT we must always run it to completion.
	 *
	 * Note: at some point we might add additional criteria for whether to use
	 * lazy eval.  However, we should prefer to use it whenever the function
	 * doesn't return set, since fetching more than one row is useless in that
	 * case.
	 *
	 * Note: don't set setsResult if the function returns VOID, as evidenced
	 * by not having made a junkfilter.  This ensures we'll throw away any
	 * output from a utility statement that check_sql_fn_retval deemed to not
	 * have output.
	 */
	if (lasttages && fcache->junkFilter)
	{
		lasttages->setsResult = true;
		if (lazyEvalOK &&
			IsA(lasttages->stmt, PlannedStmt))
		{
			PlannedStmt *ps = (PlannedStmt *) lasttages->stmt;

			if (ps->commandType == CMD_SELECT &&
				ps->utilityStmt == NULL &&
				ps->intoClause == NULL)
				fcache->lazyEval = lasttages->lazyEval = true;
		}
	}

	return firstes;
}

/* Initialize the SQLFunctionCache for a SQL function */
static void
init_sql_fcache(FmgrInfo *finfo, bool lazyEvalOK)
{
	Oid			foid = finfo->fn_oid;
	Oid			rettype;
	HeapTuple	procedureTuple;
	Form_pg_proc procedureStruct;
	SQLFunctionCachePtr fcache;
	Oid		   *argOidVect;
	int			nargs;
	List	   *queryTree_list;
	Datum		tmp;
	bool		isNull;

	fcache = (SQLFunctionCachePtr) palloc0(sizeof(SQLFunctionCache));

	/*
	 * get the procedure tuple corresponding to the given function Oid
	 */
	procedureTuple = SearchSysCache(PROCOID,
									ObjectIdGetDatum(foid),
									0, 0, 0);
	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "cache lookup failed for function %u", foid);
	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	/*
	 * get the result type from the procedure tuple, and check for polymorphic
	 * result type; if so, find out the actual result type.
	 */
	rettype = procedureStruct->prorettype;

	if (IsPolymorphicType(rettype))
	{
		rettype = get_fn_expr_rettype(finfo);
		if (rettype == InvalidOid)		/* this probably should not happen */
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("could not determine actual result type for function declared to return type %s",
							format_type_be(procedureStruct->prorettype))));
	}

	fcache->rettype = rettype;

	/* Fetch the typlen and byval info for the result type */
	get_typlenbyval(rettype, &fcache->typlen, &fcache->typbyval);

	/* Remember whether we're returning setof something */
	fcache->returnsSet = procedureStruct->proretset;

	/* Remember if function is STABLE/IMMUTABLE */
	fcache->readonly_func =
		(procedureStruct->provolatile != PROVOLATILE_VOLATILE);

	/*
	 * We need the actual argument types to pass to the parser.
	 */
	nargs = procedureStruct->pronargs;
	if (nargs > 0)
	{
		int			argnum;

		argOidVect = (Oid *) palloc(nargs * sizeof(Oid));
		memcpy(argOidVect,
			   procedureStruct->proargtypes.values,
			   nargs * sizeof(Oid));
		/* Resolve any polymorphic argument types */
		for (argnum = 0; argnum < nargs; argnum++)
		{
			Oid			argtype = argOidVect[argnum];

			if (IsPolymorphicType(argtype))
			{
				argtype = get_fn_expr_argtype(finfo, argnum);
				if (argtype == InvalidOid)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("could not determine actual type of argument declared %s",
									format_type_be(argOidVect[argnum]))));
				argOidVect[argnum] = argtype;
			}
		}
	}
	else
		argOidVect = NULL;
	fcache->argtypes = argOidVect;

	/*
	 * And of course we need the function body text.
	 */
	tmp = SysCacheGetAttr(PROCOID,
						  procedureTuple,
						  Anum_pg_proc_prosrc,
						  &isNull);
	if (isNull)
		elog(ERROR, "null prosrc for function %u", foid);
	fcache->src = TextDatumGetCString(tmp);

	/*
	 * Parse and rewrite the queries in the function text.
	 */
	queryTree_list = pg_parse_and_rewrite(fcache->src, argOidVect, nargs);

	/*
	 * Check that the function returns the type it claims to.  Although in
	 * simple cases this was already done when the function was defined, we
	 * have to recheck because database objects used in the function's queries
	 * might have changed type.  We'd have to do it anyway if the function had
	 * any polymorphic arguments.
	 *
	 * Note: we set fcache->returnsTuple according to whether we are returning
	 * the whole tuple result or just a single column.	In the latter case we
	 * clear returnsTuple because we need not act different from the scalar
	 * result case, even if it's a rowtype column.  (However, we have to force
	 * lazy eval mode in that case; otherwise we'd need extra code to expand
	 * the rowtype column into multiple columns, since we have no way to
	 * notify the caller that it should do that.)
	 *
	 * check_sql_fn_retval will also construct a JunkFilter we can use to
	 * coerce the returned rowtype to the desired form (unless the result type
	 * is VOID, in which case there's nothing to coerce to).
	 */
	fcache->returnsTuple = check_sql_fn_retval(foid,
											   rettype,
											   queryTree_list,
											   false,
											   &fcache->junkFilter);

	if (fcache->returnsTuple)
	{
		/* Make sure output rowtype is properly blessed */
		BlessTupleDesc(fcache->junkFilter->jf_resultSlot->tts_tupleDescriptor);
	}
	else if (fcache->returnsSet && type_is_rowtype(fcache->rettype))
	{
		/*
		 * Returning rowtype as if it were scalar --- materialize won't work.
		 * Right now it's sufficient to override any caller preference for
		 * materialize mode, but to add more smarts in init_execution_state
		 * about this, we'd probably need a three-way flag instead of bool.
		 */
		lazyEvalOK = true;
	}

	/* Finally, plan the queries */
	fcache->func_state = init_execution_state(queryTree_list,
											  fcache,
											  lazyEvalOK);

	ReleaseSysCache(procedureTuple);

	finfo->fn_extra = (void *) fcache;
}

/* Start up execution of one execution_state node */
static void
postquel_start(execution_state *es, SQLFunctionCachePtr fcache)
{
	Snapshot	snapshot;
	DestReceiver *dest;

	Assert(es->qd == NULL);

	/*
	 * In a read-only function, use the surrounding query's snapshot;
	 * otherwise take a new snapshot for each query.  The snapshot should
	 * include a fresh command ID so that all work to date in this transaction
	 * is visible.
	 */
	if (fcache->readonly_func)
		snapshot = GetActiveSnapshot();
	else
	{
		CommandCounterIncrement();
		snapshot = GetTransactionSnapshot();
	}

	/*
	 * If this query produces the function result, send its output to the
	 * tuplestore; else discard any output.
	 */
	if (es->setsResult)
	{
		DR_sqlfunction *myState;

		dest = CreateDestReceiver(DestSQLFunction);
		/* pass down the needed info to the dest receiver routines */
		myState = (DR_sqlfunction *) dest;
		Assert(myState->pub.mydest == DestSQLFunction);
		myState->tstore = fcache->tstore;
		myState->cxt = CurrentMemoryContext;
		myState->filter = fcache->junkFilter;
	}
	else
		dest = None_Receiver;

	if (IsA(es->stmt, PlannedStmt))
		es->qd = CreateQueryDesc((PlannedStmt *) es->stmt,
								 fcache->src,
								 snapshot, InvalidSnapshot,
								 dest,
								 fcache->paramLI, false);
	else
		es->qd = CreateUtilityQueryDesc(es->stmt,
										fcache->src,
										snapshot,
										dest,
										fcache->paramLI);

	/* We assume we don't need to set up ActiveSnapshot for ExecutorStart */

	/* Utility commands don't need Executor. */
	if (es->qd->utilitystmt == NULL)
	{
		/*
		 * Only set up to collect queued triggers if it's not a SELECT. This
		 * isn't just an optimization, but is necessary in case a SELECT
		 * returns multiple rows to caller --- we mustn't exit from the
		 * function execution with a stacked AfterTrigger level still active.
		 */
		if (es->qd->operation != CMD_SELECT)
			AfterTriggerBeginQuery();
		ExecutorStart(es->qd, 0);
	}

	es->status = F_EXEC_RUN;
}

/* Run one execution_state; either to completion or to first result row */
/* Returns true if we ran to completion */
static bool
postquel_getnext(execution_state *es, SQLFunctionCachePtr fcache)
{
	bool		result;

	/* Make our snapshot the active one for any called functions */
	PushActiveSnapshot(es->qd->snapshot);

	if (es->qd->utilitystmt)
	{
		/* ProcessUtility needs the PlannedStmt for DECLARE CURSOR */
		ProcessUtility((es->qd->plannedstmt ?
						(Node *) es->qd->plannedstmt :
						es->qd->utilitystmt),
					   fcache->src,
					   es->qd->params,
					   false,	/* not top level */
					   es->qd->dest,
					   NULL);
		result = true;			/* never stops early */
	}
	else
	{
		/* Run regular commands to completion unless lazyEval */
		long		count = (es->lazyEval) ? 1L : 0L;

		ExecutorRun(es->qd, ForwardScanDirection, count);

		/*
		 * If we requested run to completion OR there was no tuple returned,
		 * command must be complete.
		 */
		result = (count == 0L || es->qd->estate->es_processed == 0);
	}

	PopActiveSnapshot();

	return result;
}

/* Shut down execution of one execution_state node */
static void
postquel_end(execution_state *es)
{
	/* mark status done to ensure we don't do ExecutorEnd twice */
	es->status = F_EXEC_DONE;

	/* Utility commands don't need Executor. */
	if (es->qd->utilitystmt == NULL)
	{
		/* Make our snapshot the active one for any called functions */
		PushActiveSnapshot(es->qd->snapshot);

		if (es->qd->operation != CMD_SELECT)
			AfterTriggerEndQuery(es->qd->estate);
		ExecutorEnd(es->qd);

		PopActiveSnapshot();
	}

	(*es->qd->dest->rDestroy) (es->qd->dest);

	FreeQueryDesc(es->qd);
	es->qd = NULL;
}

/* Build ParamListInfo array representing current arguments */
static void
postquel_sub_params(SQLFunctionCachePtr fcache,
					FunctionCallInfo fcinfo)
{
	int			nargs = fcinfo->nargs;

	if (nargs > 0)
	{
		ParamListInfo paramLI;
		int			i;

		if (fcache->paramLI == NULL)
		{
			/* sizeof(ParamListInfoData) includes the first array element */
			paramLI = (ParamListInfo) palloc(sizeof(ParamListInfoData) +
									   (nargs - 1) *sizeof(ParamExternData));
			paramLI->numParams = nargs;
			fcache->paramLI = paramLI;
		}
		else
		{
			paramLI = fcache->paramLI;
			Assert(paramLI->numParams == nargs);
		}

		for (i = 0; i < nargs; i++)
		{
			ParamExternData *prm = &paramLI->params[i];

			prm->value = fcinfo->arg[i];
			prm->isnull = fcinfo->argnull[i];
			prm->pflags = 0;
			prm->ptype = fcache->argtypes[i];
		}
	}
	else
		fcache->paramLI = NULL;
}

/*
 * Extract the SQL function's value from a single result row.  This is used
 * both for scalar (non-set) functions and for each row of a lazy-eval set
 * result.
 */
static Datum
postquel_get_single_result(TupleTableSlot *slot,
						   FunctionCallInfo fcinfo,
						   SQLFunctionCachePtr fcache,
						   MemoryContext resultcontext)
{
	Datum		value;
	MemoryContext oldcontext;

	/*
	 * Set up to return the function value.  For pass-by-reference datatypes,
	 * be sure to allocate the result in resultcontext, not the current memory
	 * context (which has query lifespan).	We can't leave the data in the
	 * TupleTableSlot because we intend to clear the slot before returning.
	 */
	oldcontext = MemoryContextSwitchTo(resultcontext);

	if (fcache->returnsTuple)
	{
		/* We must return the whole tuple as a Datum. */
		fcinfo->isnull = false;
		value = ExecFetchSlotTupleDatum(slot);
		value = datumCopy(value, fcache->typbyval, fcache->typlen);
	}
	else
	{
		/*
		 * Returning a scalar, which we have to extract from the first column
		 * of the SELECT result, and then copy into result context if needed.
		 */
		value = slot_getattr(slot, 1, &(fcinfo->isnull));

		if (!fcinfo->isnull)
			value = datumCopy(value, fcache->typbyval, fcache->typlen);
	}

	MemoryContextSwitchTo(oldcontext);

	return value;
}

/*
 * fmgr_sql: function call manager for SQL functions
 */
Datum
fmgr_sql(PG_FUNCTION_ARGS)
{
	MemoryContext oldcontext;
	SQLFunctionCachePtr fcache;
	ErrorContextCallback sqlerrcontext;
	bool		randomAccess;
	bool		lazyEvalOK;
	execution_state *es;
	TupleTableSlot *slot;
	Datum		result;

	/*
	 * Switch to context in which the fcache lives.  This ensures that
	 * parsetrees, plans, etc, will have sufficient lifetime.  The
	 * sub-executor is responsible for deleting per-tuple information.
	 */
	oldcontext = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

	/*
	 * Setup error traceback support for ereport()
	 */
	sqlerrcontext.callback = sql_exec_error_callback;
	sqlerrcontext.arg = fcinfo->flinfo;
	sqlerrcontext.previous = error_context_stack;
	error_context_stack = &sqlerrcontext;

	/* Check call context */
	if (fcinfo->flinfo->fn_retset)
	{
		ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

		/*
		 * For simplicity, we require callers to support both set eval modes.
		 * There are cases where we must use one or must use the other, and
		 * it's not really worthwhile to postpone the check till we know.
		 * But note we do not require caller to provide an expectedDesc.
		 */
		if (!rsi || !IsA(rsi, ReturnSetInfo) ||
			(rsi->allowedModes & SFRM_ValuePerCall) == 0 ||
			(rsi->allowedModes & SFRM_Materialize) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that cannot accept a set")));
		randomAccess = rsi->allowedModes & SFRM_Materialize_Random;
		lazyEvalOK = !(rsi->allowedModes & SFRM_Materialize_Preferred);
	}
	else
	{
		randomAccess = false;
		lazyEvalOK = true;
	}

	/*
	 * Initialize fcache (build plans) if first time through.
	 */
	fcache = (SQLFunctionCachePtr) fcinfo->flinfo->fn_extra;
	if (fcache == NULL)
	{
		init_sql_fcache(fcinfo->flinfo, lazyEvalOK);
		fcache = (SQLFunctionCachePtr) fcinfo->flinfo->fn_extra;
	}
	es = fcache->func_state;

	/*
	 * Convert params to appropriate format if starting a fresh execution. (If
	 * continuing execution, we can re-use prior params.)
	 */
	if (es && es->status == F_EXEC_START)
		postquel_sub_params(fcache, fcinfo);

	/*
	 * Build tuplestore to hold results, if we don't have one already. Note
	 * it's in the query-lifespan context.
	 */
	if (!fcache->tstore)
		fcache->tstore = tuplestore_begin_heap(randomAccess, false, work_mem);

	/*
	 * Find first unfinished query in function.
	 */
	while (es && es->status == F_EXEC_DONE)
		es = es->next;

	/*
	 * Execute each command in the function one after another until we either
	 * run out of commands or get a result row from a lazily-evaluated SELECT.
	 */
	while (es)
	{
		bool		completed;

		if (es->status == F_EXEC_START)
			postquel_start(es, fcache);

		completed = postquel_getnext(es, fcache);

		/*
		 * If we ran the command to completion, we can shut it down now. Any
		 * row(s) we need to return are safely stashed in the tuplestore, and
		 * we want to be sure that, for example, AFTER triggers get fired
		 * before we return anything.  Also, if the function doesn't return
		 * set, we can shut it down anyway because it must be a SELECT and we
		 * don't care about fetching any more result rows.
		 */
		if (completed || !fcache->returnsSet)
			postquel_end(es);

		/*
		 * Break from loop if we didn't shut down (implying we got a
		 * lazily-evaluated row).  Otherwise we'll press on till the whole
		 * function is done, relying on the tuplestore to keep hold of the
		 * data to eventually be returned.	This is necessary since an
		 * INSERT/UPDATE/DELETE RETURNING that sets the result might be
		 * followed by additional rule-inserted commands, and we want to
		 * finish doing all those commands before we return anything.
		 */
		if (es->status != F_EXEC_DONE)
			break;
		es = es->next;
	}

	/*
	 * The tuplestore now contains whatever row(s) we are supposed to return.
	 */
	if (fcache->returnsSet)
	{
		ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

		if (es)
		{
			/*
			 * If we stopped short of being done, we must have a lazy-eval
			 * row.
			 */
			Assert(es->lazyEval);
			/* Re-use the junkfilter's output slot to fetch back the tuple */
			Assert(fcache->junkFilter);
			slot = fcache->junkFilter->jf_resultSlot;
			if (!tuplestore_gettupleslot(fcache->tstore, true, false, slot))
				elog(ERROR, "failed to fetch lazy-eval tuple");
			/* Extract the result as a datum, and copy out from the slot */
			result = postquel_get_single_result(slot, fcinfo,
												fcache, oldcontext);
			/* Clear the tuplestore, but keep it for next time */
			/* NB: this might delete the slot's content, but we don't care */
			tuplestore_clear(fcache->tstore);

			/*
			 * Let caller know we're not finished.
			 */
			rsi->isDone = ExprMultipleResult;

			/*
			 * Ensure we will get shut down cleanly if the exprcontext is not
			 * run to completion.
			 */
			if (!fcache->shutdown_reg)
			{
				RegisterExprContextCallback(rsi->econtext,
											ShutdownSQLFunction,
											PointerGetDatum(fcache));
				fcache->shutdown_reg = true;
			}
		}
		else if (fcache->lazyEval)
		{
			/*
			 * We are done with a lazy evaluation.	Clean up.
			 */
			tuplestore_clear(fcache->tstore);

			/*
			 * Let caller know we're finished.
			 */
			rsi->isDone = ExprEndResult;

			fcinfo->isnull = true;
			result = (Datum) 0;

			/* Deregister shutdown callback, if we made one */
			if (fcache->shutdown_reg)
			{
				UnregisterExprContextCallback(rsi->econtext,
											  ShutdownSQLFunction,
											  PointerGetDatum(fcache));
				fcache->shutdown_reg = false;
			}
		}
		else
		{
			/*
			 * We are done with a non-lazy evaluation.	Return whatever is in
			 * the tuplestore.	(It is now caller's responsibility to free the
			 * tuplestore when done.)
			 */
			rsi->returnMode = SFRM_Materialize;
			rsi->setResult = fcache->tstore;
			fcache->tstore = NULL;
			/* must copy desc because execQual will free it */
			if (fcache->junkFilter)
				rsi->setDesc = CreateTupleDescCopy(fcache->junkFilter->jf_cleanTupType);

			fcinfo->isnull = true;
			result = (Datum) 0;

			/* Deregister shutdown callback, if we made one */
			if (fcache->shutdown_reg)
			{
				UnregisterExprContextCallback(rsi->econtext,
											  ShutdownSQLFunction,
											  PointerGetDatum(fcache));
				fcache->shutdown_reg = false;
			}
		}
	}
	else
	{
		/*
		 * Non-set function.  If we got a row, return it; else return NULL.
		 */
		if (fcache->junkFilter)
		{
			/* Re-use the junkfilter's output slot to fetch back the tuple */
			slot = fcache->junkFilter->jf_resultSlot;
			if (tuplestore_gettupleslot(fcache->tstore, true, false, slot))
				result = postquel_get_single_result(slot, fcinfo,
													fcache, oldcontext);
			else
			{
				fcinfo->isnull = true;
				result = (Datum) 0;
			}
		}
		else
		{
			/* Should only get here for VOID functions */
			Assert(fcache->rettype == VOIDOID);
			fcinfo->isnull = true;
			result = (Datum) 0;
		}

		/* Clear the tuplestore, but keep it for next time */
		tuplestore_clear(fcache->tstore);
	}

	/*
	 * If we've gone through every command in the function, we are done. Reset
	 * the execution states to start over again on next call.
	 */
	if (es == NULL)
	{
		es = fcache->func_state;
		while (es)
		{
			es->status = F_EXEC_START;
			es = es->next;
		}
	}

	error_context_stack = sqlerrcontext.previous;

	MemoryContextSwitchTo(oldcontext);

	return result;
}


/*
 * error context callback to let us supply a call-stack traceback
 */
static void
sql_exec_error_callback(void *arg)
{
	FmgrInfo   *flinfo = (FmgrInfo *) arg;
	SQLFunctionCachePtr fcache = (SQLFunctionCachePtr) flinfo->fn_extra;
	HeapTuple	func_tuple;
	Form_pg_proc functup;
	char	   *fn_name;
	int			syntaxerrposition;

	/* Need access to function's pg_proc tuple */
	func_tuple = SearchSysCache(PROCOID,
								ObjectIdGetDatum(flinfo->fn_oid),
								0, 0, 0);
	if (!HeapTupleIsValid(func_tuple))
		return;					/* shouldn't happen */
	functup = (Form_pg_proc) GETSTRUCT(func_tuple);
	fn_name = NameStr(functup->proname);

	/*
	 * If there is a syntax error position, convert to internal syntax error
	 */
	syntaxerrposition = geterrposition();
	if (syntaxerrposition > 0)
	{
		bool		isnull;
		Datum		tmp;
		char	   *prosrc;

		tmp = SysCacheGetAttr(PROCOID, func_tuple, Anum_pg_proc_prosrc,
							  &isnull);
		if (isnull)
			elog(ERROR, "null prosrc");
		prosrc = TextDatumGetCString(tmp);
		errposition(0);
		internalerrposition(syntaxerrposition);
		internalerrquery(prosrc);
		pfree(prosrc);
	}

	/*
	 * Try to determine where in the function we failed.  If there is a query
	 * with non-null QueryDesc, finger it.	(We check this rather than looking
	 * for F_EXEC_RUN state, so that errors during ExecutorStart or
	 * ExecutorEnd are blamed on the appropriate query; see postquel_start and
	 * postquel_end.)
	 */
	if (fcache)
	{
		execution_state *es;
		int			query_num;

		es = fcache->func_state;
		query_num = 1;
		while (es)
		{
			if (es->qd)
			{
				errcontext("SQL function \"%s\" statement %d",
						   fn_name, query_num);
				break;
			}
			es = es->next;
			query_num++;
		}
		if (es == NULL)
		{
			/*
			 * couldn't identify a running query; might be function entry,
			 * function exit, or between queries.
			 */
			errcontext("SQL function \"%s\"", fn_name);
		}
	}
	else
	{
		/* must have failed during init_sql_fcache() */
		errcontext("SQL function \"%s\" during startup", fn_name);
	}

	ReleaseSysCache(func_tuple);
}


/*
 * callback function in case a function-returning-set needs to be shut down
 * before it has been run to completion
 */
static void
ShutdownSQLFunction(Datum arg)
{
	SQLFunctionCachePtr fcache = (SQLFunctionCachePtr) DatumGetPointer(arg);
	execution_state *es = fcache->func_state;

	while (es != NULL)
	{
		/* Shut down anything still running */
		if (es->status == F_EXEC_RUN)
			postquel_end(es);
		/* Reset states to START in case we're called again */
		es->status = F_EXEC_START;
		es = es->next;
	}

	/* Release tuplestore if we have one */
	if (fcache->tstore)
		tuplestore_end(fcache->tstore);
	fcache->tstore = NULL;

	/* execUtils will deregister the callback... */
	fcache->shutdown_reg = false;
}


/*
 * check_sql_fn_retval() -- check return value of a list of sql parse trees.
 *
 * The return value of a sql function is the value returned by the last
 * canSetTag query in the function.  We do some ad-hoc type checking here
 * to be sure that the user is returning the type he claims.  There are
 * also a couple of strange-looking features to assist callers in dealing
 * with allowed special cases, such as binary-compatible result types.
 *
 * For a polymorphic function the passed rettype must be the actual resolved
 * output type of the function; we should never see a polymorphic pseudotype
 * such as ANYELEMENT as rettype.  (This means we can't check the type during
 * function definition of a polymorphic function.)
 *
 * This function returns true if the sql function returns the entire tuple
 * result of its final statement, and false otherwise.	Note that because we
 * allow "SELECT rowtype_expression", this may be false even when the declared
 * function return type is a rowtype.
 *
 * If insertRelabels is true, then binary-compatible cases are dealt with
 * by actually inserting RelabelType nodes into the output targetlist;
 * obviously the caller must pass a parsetree that it's okay to modify in this
 * case.
 *
 * If junkFilter isn't NULL, then *junkFilter is set to a JunkFilter defined
 * to convert the function's tuple result to the correct output tuple type.
 * Exception: if the function is defined to return VOID then *junkFilter is
 * set to NULL.
 */
bool
check_sql_fn_retval(Oid func_id, Oid rettype, List *queryTreeList,
					bool insertRelabels,
					JunkFilter **junkFilter)
{
	Query	   *parse;
	List	   *tlist;
	int			tlistlen;
	char		fn_typtype;
	Oid			restype;
	ListCell   *lc;

	AssertArg(!IsPolymorphicType(rettype));

	if (junkFilter)
		*junkFilter = NULL;		/* initialize in case of VOID result */

	/*
	 * Find the last canSetTag query in the list.  This isn't necessarily the
	 * last parsetree, because rule rewriting can insert queries after what
	 * the user wrote.
	 */
	parse = NULL;
	foreach(lc, queryTreeList)
	{
		Query	   *q = (Query *) lfirst(lc);

		if (q->canSetTag)
			parse = q;
	}

	/*
	 * If it's a plain SELECT, it returns whatever the targetlist says.
	 * Otherwise, if it's INSERT/UPDATE/DELETE with RETURNING, it returns
	 * that. Otherwise, the function return type must be VOID.
	 *
	 * Note: eventually replace this test with QueryReturnsTuples?	We'd need
	 * a more general method of determining the output type, though.  Also, it
	 * seems too dangerous to consider FETCH or EXECUTE as returning a
	 * determinable rowtype, since they depend on relatively short-lived
	 * entities.
	 */
	if (parse &&
		parse->commandType == CMD_SELECT &&
		parse->utilityStmt == NULL &&
		parse->intoClause == NULL)
	{
		tlist = parse->targetList;
	}
	else if (parse &&
			 (parse->commandType == CMD_INSERT ||
			  parse->commandType == CMD_UPDATE ||
			  parse->commandType == CMD_DELETE) &&
			 parse->returningList)
	{
		tlist = parse->returningList;
	}
	else
	{
		/* Empty function body, or last statement is a utility command */
		if (rettype != VOIDOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
			 errmsg("return type mismatch in function declared to return %s",
					format_type_be(rettype)),
					 errdetail("Function's final statement must be SELECT or INSERT/UPDATE/DELETE RETURNING.")));
		return false;
	}

	/*
	 * OK, check that the targetlist returns something matching the declared
	 * type.  (We used to insist that the declared type not be VOID in this
	 * case, but that makes it hard to write a void function that exits after
	 * calling another void function.  Instead, we insist that the tlist
	 * return void ... so void is treated as if it were a scalar type below.)
	 */

	/*
	 * Count the non-junk entries in the result targetlist.
	 */
	tlistlen = ExecCleanTargetListLength(tlist);

	fn_typtype = get_typtype(rettype);

	if (fn_typtype == TYPTYPE_BASE ||
		fn_typtype == TYPTYPE_DOMAIN ||
		fn_typtype == TYPTYPE_ENUM ||
		rettype == VOIDOID)
	{
		/*
		 * For scalar-type returns, the target list must have exactly one
		 * non-junk entry, and its type must agree with what the user
		 * declared; except we allow binary-compatible types too.
		 */
		TargetEntry *tle;

		if (tlistlen != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
			 errmsg("return type mismatch in function declared to return %s",
					format_type_be(rettype)),
			  errdetail("Final statement must return exactly one column.")));

		/* We assume here that non-junk TLEs must come first in tlists */
		tle = (TargetEntry *) linitial(tlist);
		Assert(!tle->resjunk);

		restype = exprType((Node *) tle->expr);
		if (!IsBinaryCoercible(restype, rettype))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
			 errmsg("return type mismatch in function declared to return %s",
					format_type_be(rettype)),
					 errdetail("Actual return type is %s.",
							   format_type_be(restype))));
		if (insertRelabels && restype != rettype)
			tle->expr = (Expr *) makeRelabelType(tle->expr,
												 rettype,
												 -1,
												 COERCE_DONTCARE);

		/* Set up junk filter if needed */
		if (junkFilter)
			*junkFilter = ExecInitJunkFilter(tlist, false, NULL);
	}
	else if (fn_typtype == TYPTYPE_COMPOSITE || rettype == RECORDOID)
	{
		/* Returns a rowtype */
		TupleDesc	tupdesc;
		int			tupnatts;	/* physical number of columns in tuple */
		int			tuplogcols; /* # of nondeleted columns in tuple */
		int			colindex;	/* physical column index */

		/*
		 * If the target list is of length 1, and the type of the varnode in
		 * the target list matches the declared return type, this is okay.
		 * This can happen, for example, where the body of the function is
		 * 'SELECT func2()', where func2 has the same composite return type as
		 * the function that's calling it.
		 */
		if (tlistlen == 1)
		{
			TargetEntry *tle = (TargetEntry *) linitial(tlist);

			Assert(!tle->resjunk);
			restype = exprType((Node *) tle->expr);
			if (IsBinaryCoercible(restype, rettype))
			{
				if (insertRelabels && restype != rettype)
					tle->expr = (Expr *) makeRelabelType(tle->expr,
														 rettype,
														 -1,
														 COERCE_DONTCARE);
				/* Set up junk filter if needed */
				if (junkFilter)
					*junkFilter = ExecInitJunkFilter(tlist, false, NULL);
				return false;	/* NOT returning whole tuple */
			}
		}

		/* Is the rowtype fixed, or determined only at runtime? */
		if (get_func_result_type(func_id, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		{
			/*
			 * Assume we are returning the whole tuple. Crosschecking against
			 * what the caller expects will happen at runtime.
			 */
			if (junkFilter)
				*junkFilter = ExecInitJunkFilter(tlist, false, NULL);
			return true;
		}
		Assert(tupdesc);

		/*
		 * Verify that the targetlist matches the return tuple type. We scan
		 * the non-deleted attributes to ensure that they match the datatypes
		 * of the non-resjunk columns.
		 */
		tupnatts = tupdesc->natts;
		tuplogcols = 0;			/* we'll count nondeleted cols as we go */
		colindex = 0;

		foreach(lc, tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			Form_pg_attribute attr;
			Oid			tletype;
			Oid			atttype;

			if (tle->resjunk)
				continue;

			do
			{
				colindex++;
				if (colindex > tupnatts)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("return type mismatch in function declared to return %s",
									format_type_be(rettype)),
					errdetail("Final statement returns too many columns.")));
				attr = tupdesc->attrs[colindex - 1];
			} while (attr->attisdropped);
			tuplogcols++;

			tletype = exprType((Node *) tle->expr);
			atttype = attr->atttypid;
			if (!IsBinaryCoercible(tletype, atttype))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("return type mismatch in function declared to return %s",
								format_type_be(rettype)),
						 errdetail("Final statement returns %s instead of %s at column %d.",
								   format_type_be(tletype),
								   format_type_be(atttype),
								   tuplogcols)));
			if (insertRelabels && tletype != atttype)
				tle->expr = (Expr *) makeRelabelType(tle->expr,
													 atttype,
													 -1,
													 COERCE_DONTCARE);
		}

		for (;;)
		{
			colindex++;
			if (colindex > tupnatts)
				break;
			if (!tupdesc->attrs[colindex - 1]->attisdropped)
				tuplogcols++;
		}

		if (tlistlen != tuplogcols)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
			 errmsg("return type mismatch in function declared to return %s",
					format_type_be(rettype)),
					 errdetail("Final statement returns too few columns.")));

		/* Set up junk filter if needed */
		if (junkFilter)
			*junkFilter = ExecInitJunkFilterConversion(tlist,
												CreateTupleDescCopy(tupdesc),
													   NULL);

		/* Report that we are returning entire tuple result */
		return true;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("return type %s is not supported for SQL functions",
						format_type_be(rettype))));

	return false;
}


/*
 * CreateSQLFunctionDestReceiver -- create a suitable DestReceiver object
 */
DestReceiver *
CreateSQLFunctionDestReceiver(void)
{
	DR_sqlfunction *self = (DR_sqlfunction *) palloc0(sizeof(DR_sqlfunction));

	self->pub.receiveSlot = sqlfunction_receive;
	self->pub.rStartup = sqlfunction_startup;
	self->pub.rShutdown = sqlfunction_shutdown;
	self->pub.rDestroy = sqlfunction_destroy;
	self->pub.mydest = DestSQLFunction;

	/* private fields will be set by postquel_start */

	return (DestReceiver *) self;
}

/*
 * sqlfunction_startup --- executor startup
 */
static void
sqlfunction_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	/* no-op */
}

/*
 * sqlfunction_receive --- receive one tuple
 */
static void
sqlfunction_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_sqlfunction *myState = (DR_sqlfunction *) self;
	MemoryContext oldcxt;

	/* Filter tuple as needed */
	slot = ExecFilterJunk(myState->filter, slot);

	/* Store the filtered tuple into the tuplestore */
	oldcxt = MemoryContextSwitchTo(myState->cxt);
	tuplestore_puttupleslot(myState->tstore, slot);
	MemoryContextSwitchTo(oldcxt);
}

/*
 * sqlfunction_shutdown --- executor end
 */
static void
sqlfunction_shutdown(DestReceiver *self)
{
	/* no-op */
}

/*
 * sqlfunction_destroy --- release DestReceiver object
 */
static void
sqlfunction_destroy(DestReceiver *self)
{
	pfree(self);
}
