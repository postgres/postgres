/*-------------------------------------------------------------------------
 *
 * functions.c
 *	  Execution of SQL-language functions
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/functions.c
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
 * We have an execution_state record for each query in a function.  Each
 * record contains a plantree for its query.  If the query is currently in
 * F_EXEC_RUN state then there's a QueryDesc too.
 *
 * The "next" fields chain together all the execution_state records generated
 * from a single original parsetree.  (There will only be more than one in
 * case of rule expansion of the original parsetree.)
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
	char	   *fname;			/* function name (for error msgs) */
	char	   *src;			/* function body text (for error msgs) */

	SQLFunctionParseInfoPtr pinfo;		/* data for parser callback hooks */

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

	/*
	 * func_state is a List of execution_state records, each of which is the
	 * first for its original parsetree, with any additional records chained
	 * to it via the "next" fields.  This sublist structure is needed to keep
	 * track of where the original query boundaries are.
	 */
	List	   *func_state;
} SQLFunctionCache;

typedef SQLFunctionCache *SQLFunctionCachePtr;

/*
 * Data structure needed by the parser callback hooks to resolve parameter
 * references during parsing of a SQL function's body.  This is separate from
 * SQLFunctionCache since we sometimes do parsing separately from execution.
 */
typedef struct SQLFunctionParseInfo
{
	Oid		   *argtypes;		/* resolved types of input arguments */
	int			nargs;			/* number of input arguments */
	Oid			collation;		/* function's input collation, if known */
}	SQLFunctionParseInfo;


/* non-export function prototypes */
static Node *sql_fn_param_ref(ParseState *pstate, ParamRef *pref);
static List *init_execution_state(List *queryTree_list,
					 SQLFunctionCachePtr fcache,
					 bool lazyEvalOK);
static void init_sql_fcache(FmgrInfo *finfo, Oid collation, bool lazyEvalOK);
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


/*
 * Prepare the SQLFunctionParseInfo struct for parsing a SQL function body
 *
 * This includes resolving actual types of polymorphic arguments.
 *
 * call_expr can be passed as NULL, but then we will fail if there are any
 * polymorphic arguments.
 */
SQLFunctionParseInfoPtr
prepare_sql_fn_parse_info(HeapTuple procedureTuple,
						  Node *call_expr,
						  Oid inputCollation)
{
	SQLFunctionParseInfoPtr pinfo;
	Form_pg_proc procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);
	int			nargs;

	pinfo = (SQLFunctionParseInfoPtr) palloc0(sizeof(SQLFunctionParseInfo));

	/* Save the function's input collation */
	pinfo->collation = inputCollation;

	/*
	 * Copy input argument types from the pg_proc entry, then resolve any
	 * polymorphic types.
	 */
	pinfo->nargs = nargs = procedureStruct->pronargs;
	if (nargs > 0)
	{
		Oid		   *argOidVect;
		int			argnum;

		argOidVect = (Oid *) palloc(nargs * sizeof(Oid));
		memcpy(argOidVect,
			   procedureStruct->proargtypes.values,
			   nargs * sizeof(Oid));

		for (argnum = 0; argnum < nargs; argnum++)
		{
			Oid			argtype = argOidVect[argnum];

			if (IsPolymorphicType(argtype))
			{
				argtype = get_call_expr_argtype(call_expr, argnum);
				if (argtype == InvalidOid)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("could not determine actual type of argument declared %s",
									format_type_be(argOidVect[argnum]))));
				argOidVect[argnum] = argtype;
			}
		}

		pinfo->argtypes = argOidVect;
	}

	return pinfo;
}

/*
 * Parser setup hook for parsing a SQL function body.
 */
void
sql_fn_parser_setup(struct ParseState *pstate, SQLFunctionParseInfoPtr pinfo)
{
	/* Later we might use these hooks to support parameter names */
	pstate->p_pre_columnref_hook = NULL;
	pstate->p_post_columnref_hook = NULL;
	pstate->p_paramref_hook = sql_fn_param_ref;
	/* no need to use p_coerce_param_hook */
	pstate->p_ref_hook_state = (void *) pinfo;
}

/*
 * sql_fn_param_ref		parser callback for ParamRefs ($n symbols)
 */
static Node *
sql_fn_param_ref(ParseState *pstate, ParamRef *pref)
{
	SQLFunctionParseInfoPtr pinfo = (SQLFunctionParseInfoPtr) pstate->p_ref_hook_state;
	int			paramno = pref->number;
	Param	   *param;

	/* Check parameter number is valid */
	if (paramno <= 0 || paramno > pinfo->nargs)
		return NULL;			/* unknown parameter number */

	param = makeNode(Param);
	param->paramkind = PARAM_EXTERN;
	param->paramid = paramno;
	param->paramtype = pinfo->argtypes[paramno - 1];
	param->paramtypmod = -1;
	param->paramcollid = get_typcollation(param->paramtype);
	param->location = pref->location;

	/*
	 * If we have a function input collation, allow it to override the
	 * type-derived collation for parameter symbols.  (XXX perhaps this should
	 * not happen if the type collation is not default?)
	 */
	if (OidIsValid(pinfo->collation) && OidIsValid(param->paramcollid))
		param->paramcollid = pinfo->collation;

	return (Node *) param;
}

/*
 * Set up the per-query execution_state records for a SQL function.
 *
 * The input is a List of Lists of parsed and rewritten, but not planned,
 * querytrees.  The sublist structure denotes the original query boundaries.
 */
static List *
init_execution_state(List *queryTree_list,
					 SQLFunctionCachePtr fcache,
					 bool lazyEvalOK)
{
	List	   *eslist = NIL;
	execution_state *lasttages = NULL;
	ListCell   *lc1;

	foreach(lc1, queryTree_list)
	{
		List	   *qtlist = (List *) lfirst(lc1);
		execution_state *firstes = NULL;
		execution_state *preves = NULL;
		ListCell   *lc2;

		foreach(lc2, qtlist)
		{
			Query	   *queryTree = (Query *) lfirst(lc2);
			Node	   *stmt;
			execution_state *newes;

			Assert(IsA(queryTree, Query));

			/* Plan the query if needed */
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

			/* OK, build the execution_state for this query */
			newes = (execution_state *) palloc(sizeof(execution_state));
			if (preves)
				preves->next = newes;
			else
				firstes = newes;

			newes->next = NULL;
			newes->status = F_EXEC_START;
			newes->setsResult = false;	/* might change below */
			newes->lazyEval = false;	/* might change below */
			newes->stmt = stmt;
			newes->qd = NULL;

			if (queryTree->canSetTag)
				lasttages = newes;

			preves = newes;
		}

		eslist = lappend(eslist, firstes);
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
				ps->intoClause == NULL &&
				!ps->hasModifyingCTE)
				fcache->lazyEval = lasttages->lazyEval = true;
		}
	}

	return eslist;
}

/*
 * Initialize the SQLFunctionCache for a SQL function
 */
static void
init_sql_fcache(FmgrInfo *finfo, Oid collation, bool lazyEvalOK)
{
	Oid			foid = finfo->fn_oid;
	Oid			rettype;
	HeapTuple	procedureTuple;
	Form_pg_proc procedureStruct;
	SQLFunctionCachePtr fcache;
	List	   *raw_parsetree_list;
	List	   *queryTree_list;
	List	   *flat_query_list;
	ListCell   *lc;
	Datum		tmp;
	bool		isNull;

	fcache = (SQLFunctionCachePtr) palloc0(sizeof(SQLFunctionCache));
	finfo->fn_extra = (void *) fcache;

	/*
	 * get the procedure tuple corresponding to the given function Oid
	 */
	procedureTuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(foid));
	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "cache lookup failed for function %u", foid);
	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	/*
	 * copy function name immediately for use by error reporting callback
	 */
	fcache->fname = pstrdup(NameStr(procedureStruct->proname));

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
	 * We need the actual argument types to pass to the parser.  Also make
	 * sure that parameter symbols are considered to have the function's
	 * resolved input collation.
	 */
	fcache->pinfo = prepare_sql_fn_parse_info(procedureTuple,
											  finfo->fn_expr,
											  collation);

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
	 * Parse and rewrite the queries in the function text.  Use sublists to
	 * keep track of the original query boundaries.  But we also build a
	 * "flat" list of the rewritten queries to pass to check_sql_fn_retval.
	 * This is because the last canSetTag query determines the result type
	 * independently of query boundaries --- and it might not be in the last
	 * sublist, for example if the last query rewrites to DO INSTEAD NOTHING.
	 * (It might not be unreasonable to throw an error in such a case, but
	 * this is the historical behavior and it doesn't seem worth changing.)
	 */
	raw_parsetree_list = pg_parse_query(fcache->src);

	queryTree_list = NIL;
	flat_query_list = NIL;
	foreach(lc, raw_parsetree_list)
	{
		Node	   *parsetree = (Node *) lfirst(lc);
		List	   *queryTree_sublist;

		queryTree_sublist = pg_analyze_and_rewrite_params(parsetree,
														  fcache->src,
									   (ParserSetupHook) sql_fn_parser_setup,
														  fcache->pinfo);
		queryTree_list = lappend(queryTree_list, queryTree_sublist);
		flat_query_list = list_concat(flat_query_list,
									  list_copy(queryTree_sublist));
	}

	/*
	 * Check that the function returns the type it claims to.  Although in
	 * simple cases this was already done when the function was defined, we
	 * have to recheck because database objects used in the function's queries
	 * might have changed type.  We'd have to do it anyway if the function had
	 * any polymorphic arguments.
	 *
	 * Note: we set fcache->returnsTuple according to whether we are returning
	 * the whole tuple result or just a single column.  In the latter case we
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
											   flat_query_list,
											   NULL,
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
}

/* Start up execution of one execution_state node */
static void
postquel_start(execution_state *es, SQLFunctionCachePtr fcache)
{
	DestReceiver *dest;

	Assert(es->qd == NULL);

	/* Caller should have ensured a suitable snapshot is active */
	Assert(ActiveSnapshotSet());

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
								 GetActiveSnapshot(),
								 InvalidSnapshot,
								 dest,
								 fcache->paramLI, 0);
	else
		es->qd = CreateUtilityQueryDesc(es->stmt,
										fcache->src,
										GetActiveSnapshot(),
										dest,
										fcache->paramLI);

	/* Utility commands don't need Executor. */
	if (es->qd->utilitystmt == NULL)
	{
		/*
		 * In lazyEval mode, do not let the executor set up an AfterTrigger
		 * context.  This is necessary not just an optimization, because we
		 * mustn't exit from the function execution with a stacked
		 * AfterTrigger level still active.  We are careful not to select
		 * lazyEval mode for any statement that could possibly queue triggers.
		 */
		int			eflags;

		if (es->lazyEval)
			eflags = EXEC_FLAG_SKIP_TRIGGERS;
		else
			eflags = 0;			/* default run-to-completion flags */
		ExecutorStart(es->qd, eflags);
	}

	es->status = F_EXEC_RUN;
}

/* Run one execution_state; either to completion or to first result row */
/* Returns true if we ran to completion */
static bool
postquel_getnext(execution_state *es, SQLFunctionCachePtr fcache)
{
	bool		result;

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
		ExecutorFinish(es->qd);
		ExecutorEnd(es->qd);
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
									  (nargs - 1) * sizeof(ParamExternData));
			/* we have static list of params, so no hooks needed */
			paramLI->paramFetch = NULL;
			paramLI->paramFetchArg = NULL;
			paramLI->parserSetup = NULL;
			paramLI->parserSetupArg = NULL;
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
			prm->ptype = fcache->pinfo->argtypes[i];
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
	 * context (which has query lifespan).  We can't leave the data in the
	 * TupleTableSlot because we intend to clear the slot before returning.
	 */
	oldcontext = MemoryContextSwitchTo(resultcontext);

	if (fcache->returnsTuple)
	{
		/* We must return the whole tuple as a Datum. */
		fcinfo->isnull = false;
		value = ExecFetchSlotTupleDatum(slot);
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
	bool		is_first;
	bool		pushed_snapshot;
	execution_state *es;
	TupleTableSlot *slot;
	Datum		result;
	List	   *eslist;
	ListCell   *eslc;

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
		 * it's not really worthwhile to postpone the check till we know. But
		 * note we do not require caller to provide an expectedDesc.
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
		init_sql_fcache(fcinfo->flinfo, PG_GET_COLLATION(), lazyEvalOK);
		fcache = (SQLFunctionCachePtr) fcinfo->flinfo->fn_extra;
	}
	eslist = fcache->func_state;

	/*
	 * Find first unfinished query in function, and note whether it's the
	 * first query.
	 */
	es = NULL;
	is_first = true;
	foreach(eslc, eslist)
	{
		es = (execution_state *) lfirst(eslc);

		while (es && es->status == F_EXEC_DONE)
		{
			is_first = false;
			es = es->next;
		}

		if (es)
			break;
	}

	/*
	 * Convert params to appropriate format if starting a fresh execution. (If
	 * continuing execution, we can re-use prior params.)
	 */
	if (is_first && es && es->status == F_EXEC_START)
		postquel_sub_params(fcache, fcinfo);

	/*
	 * Build tuplestore to hold results, if we don't have one already. Note
	 * it's in the query-lifespan context.
	 */
	if (!fcache->tstore)
		fcache->tstore = tuplestore_begin_heap(randomAccess, false, work_mem);

	/*
	 * Execute each command in the function one after another until we either
	 * run out of commands or get a result row from a lazily-evaluated SELECT.
	 *
	 * Notes about snapshot management:
	 *
	 * In a read-only function, we just use the surrounding query's snapshot.
	 *
	 * In a non-read-only function, we rely on the fact that we'll never
	 * suspend execution between queries of the function: the only reason to
	 * suspend execution before completion is if we are returning a row from a
	 * lazily-evaluated SELECT.  So, when first entering this loop, we'll
	 * either start a new query (and push a fresh snapshot) or re-establish
	 * the active snapshot from the existing query descriptor.  If we need to
	 * start a new query in a subsequent execution of the loop, either we need
	 * a fresh snapshot (and pushed_snapshot is false) or the existing
	 * snapshot is on the active stack and we can just bump its command ID.
	 */
	pushed_snapshot = false;
	while (es)
	{
		bool		completed;

		if (es->status == F_EXEC_START)
		{
			/*
			 * If not read-only, be sure to advance the command counter for
			 * each command, so that all work to date in this transaction is
			 * visible.  Take a new snapshot if we don't have one yet,
			 * otherwise just bump the command ID in the existing snapshot.
			 */
			if (!fcache->readonly_func)
			{
				CommandCounterIncrement();
				if (!pushed_snapshot)
				{
					PushActiveSnapshot(GetTransactionSnapshot());
					pushed_snapshot = true;
				}
				else
					UpdateActiveSnapshotCommandId();
			}

			postquel_start(es, fcache);
		}
		else if (!fcache->readonly_func && !pushed_snapshot)
		{
			/* Re-establish active snapshot when re-entering function */
			PushActiveSnapshot(es->qd->snapshot);
			pushed_snapshot = true;
		}

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
		 * data to eventually be returned.  This is necessary since an
		 * INSERT/UPDATE/DELETE RETURNING that sets the result might be
		 * followed by additional rule-inserted commands, and we want to
		 * finish doing all those commands before we return anything.
		 */
		if (es->status != F_EXEC_DONE)
			break;

		/*
		 * Advance to next execution_state, which might be in the next list.
		 */
		es = es->next;
		while (!es)
		{
			eslc = lnext(eslc);
			if (!eslc)
				break;			/* end of function */

			es = (execution_state *) lfirst(eslc);

			/*
			 * Flush the current snapshot so that we will take a new one for
			 * the new query list.  This ensures that new snaps are taken at
			 * original-query boundaries, matching the behavior of interactive
			 * execution.
			 */
			if (pushed_snapshot)
			{
				PopActiveSnapshot();
				pushed_snapshot = false;
			}
		}
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
			 * We are done with a lazy evaluation.  Clean up.
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
			 * We are done with a non-lazy evaluation.  Return whatever is in
			 * the tuplestore.  (It is now caller's responsibility to free the
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

	/* Pop snapshot if we have pushed one */
	if (pushed_snapshot)
		PopActiveSnapshot();

	/*
	 * If we've gone through every command in the function, we are done. Reset
	 * the execution states to start over again on next call.
	 */
	if (es == NULL)
	{
		foreach(eslc, fcache->func_state)
		{
			es = (execution_state *) lfirst(eslc);
			while (es)
			{
				es->status = F_EXEC_START;
				es = es->next;
			}
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
	int			syntaxerrposition;

	/*
	 * We can do nothing useful if init_sql_fcache() didn't get as far as
	 * saving the function name
	 */
	if (fcache == NULL || fcache->fname == NULL)
		return;

	/*
	 * If there is a syntax error position, convert to internal syntax error
	 */
	syntaxerrposition = geterrposition();
	if (syntaxerrposition > 0 && fcache->src != NULL)
	{
		errposition(0);
		internalerrposition(syntaxerrposition);
		internalerrquery(fcache->src);
	}

	/*
	 * Try to determine where in the function we failed.  If there is a query
	 * with non-null QueryDesc, finger it.  (We check this rather than looking
	 * for F_EXEC_RUN state, so that errors during ExecutorStart or
	 * ExecutorEnd are blamed on the appropriate query; see postquel_start and
	 * postquel_end.)
	 */
	if (fcache->func_state)
	{
		execution_state *es;
		int			query_num;
		ListCell   *lc;

		es = NULL;
		query_num = 1;
		foreach(lc, fcache->func_state)
		{
			es = (execution_state *) lfirst(lc);
			while (es)
			{
				if (es->qd)
				{
					errcontext("SQL function \"%s\" statement %d",
							   fcache->fname, query_num);
					break;
				}
				es = es->next;
			}
			if (es)
				break;
			query_num++;
		}
		if (es == NULL)
		{
			/*
			 * couldn't identify a running query; might be function entry,
			 * function exit, or between queries.
			 */
			errcontext("SQL function \"%s\"", fcache->fname);
		}
	}
	else
	{
		/*
		 * Assume we failed during init_sql_fcache().  (It's possible that the
		 * function actually has an empty body, but in that case we may as
		 * well report all errors as being "during startup".)
		 */
		errcontext("SQL function \"%s\" during startup", fcache->fname);
	}
}


/*
 * callback function in case a function-returning-set needs to be shut down
 * before it has been run to completion
 */
static void
ShutdownSQLFunction(Datum arg)
{
	SQLFunctionCachePtr fcache = (SQLFunctionCachePtr) DatumGetPointer(arg);
	execution_state *es;
	ListCell   *lc;

	foreach(lc, fcache->func_state)
	{
		es = (execution_state *) lfirst(lc);
		while (es)
		{
			/* Shut down anything still running */
			if (es->status == F_EXEC_RUN)
			{
				/* Re-establish active snapshot for any called functions */
				if (!fcache->readonly_func)
					PushActiveSnapshot(es->qd->snapshot);

				postquel_end(es);

				if (!fcache->readonly_func)
					PopActiveSnapshot();
			}

			/* Reset states to START in case we're called again */
			es->status = F_EXEC_START;
			es = es->next;
		}
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
 * result of its final statement, or false if it returns just the first column
 * result of that statement.  It throws an error if the final statement doesn't
 * return the right type at all.
 *
 * Note that because we allow "SELECT rowtype_expression", the result can be
 * false even when the declared function return type is a rowtype.
 *
 * If modifyTargetList isn't NULL, the function will modify the final
 * statement's targetlist in two cases:
 * (1) if the tlist returns values that are binary-coercible to the expected
 * type rather than being exactly the expected type.  RelabelType nodes will
 * be inserted to make the result types match exactly.
 * (2) if there are dropped columns in the declared result rowtype.  NULL
 * output columns will be inserted in the tlist to match them.
 * (Obviously the caller must pass a parsetree that is okay to modify when
 * using this flag.)  Note that this flag does not affect whether the tlist is
 * considered to be a legal match to the result type, only how we react to
 * allowed not-exact-match cases.  *modifyTargetList will be set true iff
 * we had to make any "dangerous" changes that could modify the semantics of
 * the statement.  If it is set true, the caller should not use the modified
 * statement, but for simplicity we apply the changes anyway.
 *
 * If junkFilter isn't NULL, then *junkFilter is set to a JunkFilter defined
 * to convert the function's tuple result to the correct output tuple type.
 * Exception: if the function is defined to return VOID then *junkFilter is
 * set to NULL.
 */
bool
check_sql_fn_retval(Oid func_id, Oid rettype, List *queryTreeList,
					bool *modifyTargetList,
					JunkFilter **junkFilter)
{
	Query	   *parse;
	List	  **tlist_ptr;
	List	   *tlist;
	int			tlistlen;
	char		fn_typtype;
	Oid			restype;
	ListCell   *lc;

	AssertArg(!IsPolymorphicType(rettype));

	if (modifyTargetList)
		*modifyTargetList = false;		/* initialize for no change */
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
		tlist_ptr = &parse->targetList;
		tlist = parse->targetList;
	}
	else if (parse &&
			 (parse->commandType == CMD_INSERT ||
			  parse->commandType == CMD_UPDATE ||
			  parse->commandType == CMD_DELETE) &&
			 parse->returningList)
	{
		tlist_ptr = &parse->returningList;
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
		if (modifyTargetList && restype != rettype)
		{
			tle->expr = (Expr *) makeRelabelType(tle->expr,
												 rettype,
												 -1,
												 get_typcollation(rettype),
												 COERCE_DONTCARE);
			/* Relabel is dangerous if TLE is a sort/group or setop column */
			if (tle->ressortgroupref != 0 || parse->setOperations)
				*modifyTargetList = true;
		}

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
		List	   *newtlist;	/* new non-junk tlist entries */
		List	   *junkattrs;	/* new junk tlist entries */

		/*
		 * If the target list is of length 1, and the type of the varnode in
		 * the target list matches the declared return type, this is okay.
		 * This can happen, for example, where the body of the function is
		 * 'SELECT func2()', where func2 has the same composite return type as
		 * the function that's calling it.
		 *
		 * XXX Note that if rettype is RECORD, the IsBinaryCoercible check
		 * will succeed for any composite restype.  For the moment we rely on
		 * runtime type checking to catch any discrepancy, but it'd be nice to
		 * do better at parse time.
		 */
		if (tlistlen == 1)
		{
			TargetEntry *tle = (TargetEntry *) linitial(tlist);

			Assert(!tle->resjunk);
			restype = exprType((Node *) tle->expr);
			if (IsBinaryCoercible(restype, rettype))
			{
				if (modifyTargetList && restype != rettype)
				{
					tle->expr = (Expr *) makeRelabelType(tle->expr,
														 rettype,
														 -1,
												   get_typcollation(rettype),
														 COERCE_DONTCARE);
					/* Relabel is dangerous if sort/group or setop column */
					if (tle->ressortgroupref != 0 || parse->setOperations)
						*modifyTargetList = true;
				}
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
		 * of the non-resjunk columns.  For deleted attributes, insert NULL
		 * result columns if the caller asked for that.
		 */
		tupnatts = tupdesc->natts;
		tuplogcols = 0;			/* we'll count nondeleted cols as we go */
		colindex = 0;
		newtlist = NIL;			/* these are only used if modifyTargetList */
		junkattrs = NIL;

		foreach(lc, tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			Form_pg_attribute attr;
			Oid			tletype;
			Oid			atttype;

			if (tle->resjunk)
			{
				if (modifyTargetList)
					junkattrs = lappend(junkattrs, tle);
				continue;
			}

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
				if (attr->attisdropped && modifyTargetList)
				{
					Expr	   *null_expr;

					/* The type of the null we insert isn't important */
					null_expr = (Expr *) makeConst(INT4OID,
												   -1,
												   InvalidOid,
												   sizeof(int32),
												   (Datum) 0,
												   true,		/* isnull */
												   true /* byval */ );
					newtlist = lappend(newtlist,
									   makeTargetEntry(null_expr,
													   colindex,
													   NULL,
													   false));
					/* NULL insertion is dangerous in a setop */
					if (parse->setOperations)
						*modifyTargetList = true;
				}
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
			if (modifyTargetList)
			{
				if (tletype != atttype)
				{
					tle->expr = (Expr *) makeRelabelType(tle->expr,
														 atttype,
														 -1,
												   get_typcollation(atttype),
														 COERCE_DONTCARE);
					/* Relabel is dangerous if sort/group or setop column */
					if (tle->ressortgroupref != 0 || parse->setOperations)
						*modifyTargetList = true;
				}
				tle->resno = colindex;
				newtlist = lappend(newtlist, tle);
			}
		}

		/* remaining columns in tupdesc had better all be dropped */
		for (colindex++; colindex <= tupnatts; colindex++)
		{
			if (!tupdesc->attrs[colindex - 1]->attisdropped)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("return type mismatch in function declared to return %s",
								format_type_be(rettype)),
					 errdetail("Final statement returns too few columns.")));
			if (modifyTargetList)
			{
				Expr	   *null_expr;

				/* The type of the null we insert isn't important */
				null_expr = (Expr *) makeConst(INT4OID,
											   -1,
											   InvalidOid,
											   sizeof(int32),
											   (Datum) 0,
											   true,	/* isnull */
											   true /* byval */ );
				newtlist = lappend(newtlist,
								   makeTargetEntry(null_expr,
												   colindex,
												   NULL,
												   false));
				/* NULL insertion is dangerous in a setop */
				if (parse->setOperations)
					*modifyTargetList = true;
			}
		}

		if (modifyTargetList)
		{
			/* ensure resjunk columns are numbered correctly */
			foreach(lc, junkattrs)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(lc);

				tle->resno = colindex++;
			}
			/* replace the tlist with the modified one */
			*tlist_ptr = list_concat(newtlist, junkattrs);
		}

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

	/* Filter tuple as needed */
	slot = ExecFilterJunk(myState->filter, slot);

	/* Store the filtered tuple into the tuplestore */
	tuplestore_puttupleslot(myState->tstore, slot);
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
