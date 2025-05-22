/*-------------------------------------------------------------------------
 *
 * functions.c
 *	  Execution of SQL-language functions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/functions.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/functions.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_func.h"
#include "rewrite/rewriteHandler.h"
#include "storage/proc.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/funccache.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/plancache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/*
 * Specialized DestReceiver for collecting query output in a SQL function
 */
typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	Tuplestorestate *tstore;	/* where to put result tuples, or NULL */
	JunkFilter *filter;			/* filter to convert tuple type */
} DR_sqlfunction;

/*
 * We have an execution_state record for each query in a function.  Each
 * record references a plantree for its query.  If the query is currently in
 * F_EXEC_RUN state then there's a QueryDesc too.
 *
 * The "next" fields chain together all the execution_state records generated
 * from a single original parsetree.  (There will only be more than one in
 * case of rule expansion of the original parsetree.)  The chain structure is
 * quite vestigial at this point, because we allocate the records in an array
 * for ease of memory management.  But we'll get rid of it some other day.
 */
typedef enum
{
	F_EXEC_START, F_EXEC_RUN, F_EXEC_DONE,
} ExecStatus;

typedef struct execution_state
{
	struct execution_state *next;
	ExecStatus	status;
	bool		setsResult;		/* true if this query produces func's result */
	bool		lazyEval;		/* true if should fetch one row at a time */
	PlannedStmt *stmt;			/* plan for this query */
	QueryDesc  *qd;				/* null unless status == RUN */
} execution_state;


/*
 * Data associated with a SQL-language function is kept in two main
 * data structures:
 *
 * 1. SQLFunctionHashEntry is a long-lived (potentially session-lifespan)
 * struct that holds all the info we need out of the function's pg_proc row.
 * In addition it holds pointers to CachedPlanSource(s) that manage creation
 * of plans for the query(s) within the function.  A SQLFunctionHashEntry is
 * potentially shared across multiple concurrent executions of the function,
 * so it must contain no execution-specific state; but its use_count must
 * reflect the number of SQLFunctionCache structs pointing at it.
 * If the function's pg_proc row is updated, we throw away and regenerate
 * the SQLFunctionHashEntry and subsidiary data.  (Also note that if the
 * function is polymorphic or used as a trigger, there is a separate
 * SQLFunctionHashEntry for each usage, so that we need consider only one
 * set of relevant data types.)  The struct itself is in memory managed by
 * funccache.c, and its subsidiary data is kept in one of two contexts:
 *	* pcontext ("parse context") holds the raw parse trees or Query trees
 *	  that we read from the pg_proc row.  These will be converted to
 *	  CachedPlanSources as they are needed.  Once the last one is converted,
 *	  pcontext can be freed.
 *	* hcontext ("hash context") holds everything else belonging to the
 *	  SQLFunctionHashEntry.
 *
 * 2. SQLFunctionCache is subsidiary data for a single FmgrInfo struct.
 * It is pointed to by the fn_extra field of the FmgrInfo struct, and is
 * always allocated in the FmgrInfo's fn_mcxt.  It holds a reference to
 * the CachedPlan for the current query, and other execution-specific data.
 * A few subsidiary items such as the ParamListInfo object are also kept
 * directly in fn_mcxt (which is also called fcontext here).  But most
 * subsidiary data is in jfcontext or subcontext.
 */

typedef struct SQLFunctionHashEntry
{
	CachedFunction cfunc;		/* fields managed by funccache.c */

	char	   *fname;			/* function name (for error msgs) */
	char	   *src;			/* function body text (for error msgs) */

	SQLFunctionParseInfoPtr pinfo;	/* data for parser callback hooks */
	int16	   *argtyplen;		/* lengths of the input argument types */

	Oid			rettype;		/* actual return type */
	int16		typlen;			/* length of the return type */
	bool		typbyval;		/* true if return type is pass by value */
	bool		returnsSet;		/* true if returning multiple rows */
	bool		returnsTuple;	/* true if returning whole tuple result */
	bool		readonly_func;	/* true to run in "read only" mode */
	char		prokind;		/* prokind from pg_proc row */

	TupleDesc	rettupdesc;		/* result tuple descriptor */

	List	   *source_list;	/* RawStmts or Queries read from pg_proc */
	int			num_queries;	/* original length of source_list */
	bool		raw_source;		/* true if source_list contains RawStmts */

	List	   *plansource_list;	/* CachedPlanSources for fn's queries */

	MemoryContext pcontext;		/* memory context holding source_list */
	MemoryContext hcontext;		/* memory context holding all else */
} SQLFunctionHashEntry;

typedef struct SQLFunctionCache
{
	SQLFunctionHashEntry *func; /* associated SQLFunctionHashEntry */

	bool		lazyEvalOK;		/* true if lazyEval is safe */
	bool		shutdown_reg;	/* true if registered shutdown callback */
	bool		lazyEval;		/* true if using lazyEval for result query */
	bool		randomAccess;	/* true if tstore needs random access */
	bool		ownSubcontext;	/* is subcontext really a separate context? */

	ParamListInfo paramLI;		/* Param list representing current args */

	Tuplestorestate *tstore;	/* where we accumulate result for a SRF */
	MemoryContext tscontext;	/* memory context that tstore should be in */

	JunkFilter *junkFilter;		/* will be NULL if function returns VOID */
	int			jf_generation;	/* tracks whether junkFilter is up-to-date */

	/*
	 * While executing a particular query within the function, cplan is the
	 * CachedPlan we've obtained for that query, and eslist is a chain of
	 * execution_state records for the individual plans within the CachedPlan.
	 * If eslist is not NULL at entry to fmgr_sql, then we are resuming
	 * execution of a lazyEval-mode set-returning function.
	 *
	 * next_query_index is the 0-based index of the next CachedPlanSource to
	 * get a CachedPlan from.
	 */
	CachedPlan *cplan;			/* Plan for current query, if any */
	ResourceOwner cowner;		/* CachedPlan is registered with this owner */
	int			next_query_index;	/* index of next CachedPlanSource to run */

	execution_state *eslist;	/* chain of execution_state records */
	execution_state *esarray;	/* storage for eslist */
	int			esarray_len;	/* allocated length of esarray[] */

	/* if positive, this is the 1-based index of the query we're processing */
	int			error_query_index;

	MemoryContext fcontext;		/* memory context holding this struct and all
								 * subsidiary data */
	MemoryContext jfcontext;	/* subsidiary memory context holding
								 * junkFilter, result slot, and related data */
	MemoryContext subcontext;	/* subsidiary memory context for sub-executor */

	/* Callback to release our use-count on the SQLFunctionHashEntry */
	MemoryContextCallback mcb;
} SQLFunctionCache;

typedef SQLFunctionCache *SQLFunctionCachePtr;


/* non-export function prototypes */
static Node *sql_fn_param_ref(ParseState *pstate, ParamRef *pref);
static Node *sql_fn_post_column_ref(ParseState *pstate,
									ColumnRef *cref, Node *var);
static Node *sql_fn_make_param(SQLFunctionParseInfoPtr pinfo,
							   int paramno, int location);
static Node *sql_fn_resolve_param_name(SQLFunctionParseInfoPtr pinfo,
									   const char *paramname, int location);
static SQLFunctionCache *init_sql_fcache(FunctionCallInfo fcinfo,
										 bool lazyEvalOK);
static bool init_execution_state(SQLFunctionCachePtr fcache);
static void prepare_next_query(SQLFunctionHashEntry *func);
static void sql_compile_callback(FunctionCallInfo fcinfo,
								 HeapTuple procedureTuple,
								 const CachedFunctionHashKey *hashkey,
								 CachedFunction *cfunc,
								 bool forValidator);
static void sql_delete_callback(CachedFunction *cfunc);
static void sql_postrewrite_callback(List *querytree_list, void *arg);
static void postquel_start(execution_state *es, SQLFunctionCachePtr fcache);
static bool postquel_getnext(execution_state *es, SQLFunctionCachePtr fcache);
static void postquel_end(execution_state *es, SQLFunctionCachePtr fcache);
static void postquel_sub_params(SQLFunctionCachePtr fcache,
								FunctionCallInfo fcinfo);
static Datum postquel_get_single_result(TupleTableSlot *slot,
										FunctionCallInfo fcinfo,
										SQLFunctionCachePtr fcache);
static void sql_compile_error_callback(void *arg);
static void sql_exec_error_callback(void *arg);
static void ShutdownSQLFunction(Datum arg);
static void RemoveSQLFunctionCache(void *arg);
static void check_sql_fn_statement(List *queryTreeList);
static bool check_sql_stmt_retval(List *queryTreeList,
								  Oid rettype, TupleDesc rettupdesc,
								  char prokind, bool insertDroppedCols);
static bool coerce_fn_result_column(TargetEntry *src_tle,
									Oid res_type, int32 res_typmod,
									bool tlist_is_modifiable,
									List **upper_tlist,
									bool *upper_tlist_nontrivial);
static List *get_sql_fn_result_tlist(List *queryTreeList);
static void sqlfunction_startup(DestReceiver *self, int operation, TupleDesc typeinfo);
static bool sqlfunction_receive(TupleTableSlot *slot, DestReceiver *self);
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

	/* Function's name (only) can be used to qualify argument names */
	pinfo->fname = pstrdup(NameStr(procedureStruct->proname));

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

	/*
	 * Collect names of arguments, too, if any
	 */
	if (nargs > 0)
	{
		Datum		proargnames;
		Datum		proargmodes;
		int			n_arg_names;
		bool		isNull;

		proargnames = SysCacheGetAttr(PROCNAMEARGSNSP, procedureTuple,
									  Anum_pg_proc_proargnames,
									  &isNull);
		if (isNull)
			proargnames = PointerGetDatum(NULL);	/* just to be sure */

		proargmodes = SysCacheGetAttr(PROCNAMEARGSNSP, procedureTuple,
									  Anum_pg_proc_proargmodes,
									  &isNull);
		if (isNull)
			proargmodes = PointerGetDatum(NULL);	/* just to be sure */

		n_arg_names = get_func_input_arg_names(proargnames, proargmodes,
											   &pinfo->argnames);

		/* Paranoia: ignore the result if too few array entries */
		if (n_arg_names < nargs)
			pinfo->argnames = NULL;
	}
	else
		pinfo->argnames = NULL;

	return pinfo;
}

/*
 * Parser setup hook for parsing a SQL function body.
 */
void
sql_fn_parser_setup(struct ParseState *pstate, SQLFunctionParseInfoPtr pinfo)
{
	pstate->p_pre_columnref_hook = NULL;
	pstate->p_post_columnref_hook = sql_fn_post_column_ref;
	pstate->p_paramref_hook = sql_fn_param_ref;
	/* no need to use p_coerce_param_hook */
	pstate->p_ref_hook_state = pinfo;
}

/*
 * sql_fn_post_column_ref		parser callback for ColumnRefs
 */
static Node *
sql_fn_post_column_ref(ParseState *pstate, ColumnRef *cref, Node *var)
{
	SQLFunctionParseInfoPtr pinfo = (SQLFunctionParseInfoPtr) pstate->p_ref_hook_state;
	int			nnames;
	Node	   *field1;
	Node	   *subfield = NULL;
	const char *name1;
	const char *name2 = NULL;
	Node	   *param;

	/*
	 * Never override a table-column reference.  This corresponds to
	 * considering the parameter names to appear in a scope outside the
	 * individual SQL commands, which is what we want.
	 */
	if (var != NULL)
		return NULL;

	/*----------
	 * The allowed syntaxes are:
	 *
	 * A		A = parameter name
	 * A.B		A = function name, B = parameter name
	 *			OR: A = record-typed parameter name, B = field name
	 *			(the first possibility takes precedence)
	 * A.B.C	A = function name, B = record-typed parameter name,
	 *			C = field name
	 * A.*		Whole-row reference to composite parameter A.
	 * A.B.*	Same, with A = function name, B = parameter name
	 *
	 * Here, it's sufficient to ignore the "*" in the last two cases --- the
	 * main parser will take care of expanding the whole-row reference.
	 *----------
	 */
	nnames = list_length(cref->fields);

	if (nnames > 3)
		return NULL;

	if (IsA(llast(cref->fields), A_Star))
		nnames--;

	field1 = (Node *) linitial(cref->fields);
	name1 = strVal(field1);
	if (nnames > 1)
	{
		subfield = (Node *) lsecond(cref->fields);
		name2 = strVal(subfield);
	}

	if (nnames == 3)
	{
		/*
		 * Three-part name: if the first part doesn't match the function name,
		 * we can fail immediately. Otherwise, look up the second part, and
		 * take the third part to be a field reference.
		 */
		if (strcmp(name1, pinfo->fname) != 0)
			return NULL;

		param = sql_fn_resolve_param_name(pinfo, name2, cref->location);

		subfield = (Node *) lthird(cref->fields);
		Assert(IsA(subfield, String));
	}
	else if (nnames == 2 && strcmp(name1, pinfo->fname) == 0)
	{
		/*
		 * Two-part name with first part matching function name: first see if
		 * second part matches any parameter name.
		 */
		param = sql_fn_resolve_param_name(pinfo, name2, cref->location);

		if (param)
		{
			/* Yes, so this is a parameter reference, no subfield */
			subfield = NULL;
		}
		else
		{
			/* No, so try to match as parameter name and subfield */
			param = sql_fn_resolve_param_name(pinfo, name1, cref->location);
		}
	}
	else
	{
		/* Single name, or parameter name followed by subfield */
		param = sql_fn_resolve_param_name(pinfo, name1, cref->location);
	}

	if (!param)
		return NULL;			/* No match */

	if (subfield)
	{
		/*
		 * Must be a reference to a field of a composite parameter; otherwise
		 * ParseFuncOrColumn will return NULL, and we'll fail back at the
		 * caller.
		 */
		param = ParseFuncOrColumn(pstate,
								  list_make1(subfield),
								  list_make1(param),
								  pstate->p_last_srf,
								  NULL,
								  false,
								  cref->location);
	}

	return param;
}

/*
 * sql_fn_param_ref		parser callback for ParamRefs ($n symbols)
 */
static Node *
sql_fn_param_ref(ParseState *pstate, ParamRef *pref)
{
	SQLFunctionParseInfoPtr pinfo = (SQLFunctionParseInfoPtr) pstate->p_ref_hook_state;
	int			paramno = pref->number;

	/* Check parameter number is valid */
	if (paramno <= 0 || paramno > pinfo->nargs)
		return NULL;			/* unknown parameter number */

	return sql_fn_make_param(pinfo, paramno, pref->location);
}

/*
 * sql_fn_make_param		construct a Param node for the given paramno
 */
static Node *
sql_fn_make_param(SQLFunctionParseInfoPtr pinfo,
				  int paramno, int location)
{
	Param	   *param;

	param = makeNode(Param);
	param->paramkind = PARAM_EXTERN;
	param->paramid = paramno;
	param->paramtype = pinfo->argtypes[paramno - 1];
	param->paramtypmod = -1;
	param->paramcollid = get_typcollation(param->paramtype);
	param->location = location;

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
 * Search for a function parameter of the given name; if there is one,
 * construct and return a Param node for it.  If not, return NULL.
 * Helper function for sql_fn_post_column_ref.
 */
static Node *
sql_fn_resolve_param_name(SQLFunctionParseInfoPtr pinfo,
						  const char *paramname, int location)
{
	int			i;

	if (pinfo->argnames == NULL)
		return NULL;

	for (i = 0; i < pinfo->nargs; i++)
	{
		if (pinfo->argnames[i] && strcmp(pinfo->argnames[i], paramname) == 0)
			return sql_fn_make_param(pinfo, i + 1, location);
	}

	return NULL;
}

/*
 * Initialize the SQLFunctionCache for a SQL function
 */
static SQLFunctionCache *
init_sql_fcache(FunctionCallInfo fcinfo, bool lazyEvalOK)
{
	FmgrInfo   *finfo = fcinfo->flinfo;
	SQLFunctionHashEntry *func;
	SQLFunctionCache *fcache;

	/*
	 * If this is the first execution for this FmgrInfo, set up a cache struct
	 * (initially containing null pointers).  The cache must live as long as
	 * the FmgrInfo, so it goes in fn_mcxt.  Also set up a memory context
	 * callback that will be invoked when fn_mcxt is deleted.
	 */
	fcache = finfo->fn_extra;
	if (fcache == NULL)
	{
		fcache = (SQLFunctionCache *)
			MemoryContextAllocZero(finfo->fn_mcxt, sizeof(SQLFunctionCache));
		fcache->fcontext = finfo->fn_mcxt;
		fcache->mcb.func = RemoveSQLFunctionCache;
		fcache->mcb.arg = fcache;
		MemoryContextRegisterResetCallback(finfo->fn_mcxt, &fcache->mcb);
		finfo->fn_extra = fcache;
	}

	/*
	 * If we are resuming execution of a set-returning function, just keep
	 * using the same cache.  We do not ask funccache.c to re-validate the
	 * SQLFunctionHashEntry: we want to run to completion using the function's
	 * initial definition.
	 */
	if (fcache->eslist != NULL)
	{
		Assert(fcache->func != NULL);
		return fcache;
	}

	/*
	 * Look up, or re-validate, the long-lived hash entry.  Make the hash key
	 * depend on the result of get_call_result_type() when that's composite,
	 * so that we can safely assume that we'll build a new hash entry if the
	 * composite rowtype changes.
	 */
	func = (SQLFunctionHashEntry *)
		cached_function_compile(fcinfo,
								(CachedFunction *) fcache->func,
								sql_compile_callback,
								sql_delete_callback,
								sizeof(SQLFunctionHashEntry),
								true,
								false);

	/*
	 * Install the hash pointer in the SQLFunctionCache, and increment its use
	 * count to reflect that.  If cached_function_compile gave us back a
	 * different hash entry than we were using before, we must decrement that
	 * one's use count.
	 */
	if (func != fcache->func)
	{
		if (fcache->func != NULL)
		{
			Assert(fcache->func->cfunc.use_count > 0);
			fcache->func->cfunc.use_count--;
		}
		fcache->func = func;
		func->cfunc.use_count++;
		/* Assume we need to rebuild the junkFilter */
		fcache->junkFilter = NULL;
	}

	/*
	 * We're beginning a new execution of the function, so convert params to
	 * appropriate format.
	 */
	postquel_sub_params(fcache, fcinfo);

	/* Also reset lazyEval state for the new execution. */
	fcache->lazyEvalOK = lazyEvalOK;
	fcache->lazyEval = false;

	/* Also reset data about where we are in the function. */
	fcache->eslist = NULL;
	fcache->next_query_index = 0;
	fcache->error_query_index = 0;

	return fcache;
}

/*
 * Set up the per-query execution_state records for the next query within
 * the SQL function.
 *
 * Returns true if successful, false if there are no more queries.
 */
static bool
init_execution_state(SQLFunctionCachePtr fcache)
{
	CachedPlanSource *plansource;
	execution_state *preves = NULL;
	execution_state *lasttages = NULL;
	int			nstmts;
	ListCell   *lc;

	/*
	 * Clean up after previous query, if there was one.
	 */
	if (fcache->cplan)
	{
		ReleaseCachedPlan(fcache->cplan, fcache->cowner);
		fcache->cplan = NULL;
	}
	fcache->eslist = NULL;

	/*
	 * Get the next CachedPlanSource, or stop if there are no more.  We might
	 * need to create the next CachedPlanSource; if so, advance
	 * error_query_index first, so that errors detected in prepare_next_query
	 * are blamed on the right statement.
	 */
	if (fcache->next_query_index >= list_length(fcache->func->plansource_list))
	{
		if (fcache->next_query_index >= fcache->func->num_queries)
			return false;
		fcache->error_query_index++;
		prepare_next_query(fcache->func);
	}
	else
		fcache->error_query_index++;

	plansource = (CachedPlanSource *) list_nth(fcache->func->plansource_list,
											   fcache->next_query_index);
	fcache->next_query_index++;

	/*
	 * Generate plans for the query or queries within this CachedPlanSource.
	 * Register the CachedPlan with the current resource owner.  (Saving
	 * cowner here is mostly paranoia, but this way we needn't assume that
	 * CurrentResourceOwner will be the same when ShutdownSQLFunction runs.)
	 */
	fcache->cowner = CurrentResourceOwner;
	fcache->cplan = GetCachedPlan(plansource,
								  fcache->paramLI,
								  fcache->cowner,
								  NULL);

	/*
	 * If necessary, make esarray[] bigger to hold the needed state.
	 */
	nstmts = list_length(fcache->cplan->stmt_list);
	if (nstmts > fcache->esarray_len)
	{
		if (fcache->esarray == NULL)
			fcache->esarray = (execution_state *)
				MemoryContextAlloc(fcache->fcontext,
								   sizeof(execution_state) * nstmts);
		else
			fcache->esarray = repalloc_array(fcache->esarray,
											 execution_state, nstmts);
		fcache->esarray_len = nstmts;
	}

	/*
	 * Build execution_state list to match the number of contained plans.
	 */
	foreach(lc, fcache->cplan->stmt_list)
	{
		PlannedStmt *stmt = lfirst_node(PlannedStmt, lc);
		execution_state *newes;

		/*
		 * Precheck all commands for validity in a function.  This should
		 * generally match the restrictions spi.c applies.
		 */
		if (stmt->commandType == CMD_UTILITY)
		{
			if (IsA(stmt->utilityStmt, CopyStmt) &&
				((CopyStmt *) stmt->utilityStmt)->filename == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot COPY to/from client in an SQL function")));

			if (IsA(stmt->utilityStmt, TransactionStmt))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				/* translator: %s is a SQL statement name */
						 errmsg("%s is not allowed in an SQL function",
								CreateCommandName(stmt->utilityStmt))));
		}

		if (fcache->func->readonly_func && !CommandIsReadOnly(stmt))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/* translator: %s is a SQL statement name */
					 errmsg("%s is not allowed in a non-volatile function",
							CreateCommandName((Node *) stmt))));

		/* OK, build the execution_state for this query */
		newes = &fcache->esarray[foreach_current_index(lc)];
		if (preves)
			preves->next = newes;
		else
			fcache->eslist = newes;

		newes->next = NULL;
		newes->status = F_EXEC_START;
		newes->setsResult = false;	/* might change below */
		newes->lazyEval = false;	/* might change below */
		newes->stmt = stmt;
		newes->qd = NULL;

		if (stmt->canSetTag)
			lasttages = newes;

		preves = newes;
	}

	/*
	 * If this isn't the last CachedPlanSource, we're done here.  Otherwise,
	 * we need to prepare information about how to return the results.
	 */
	if (fcache->next_query_index < fcache->func->num_queries)
		return true;

	/*
	 * Construct a JunkFilter we can use to coerce the returned rowtype to the
	 * desired form, unless the result type is VOID, in which case there's
	 * nothing to coerce to.  (XXX Frequently, the JunkFilter isn't doing
	 * anything very interesting, but much of this module expects it to be
	 * there anyway.)
	 *
	 * Normally we can re-use the JunkFilter across executions, but if the
	 * plan for the last CachedPlanSource changed, we'd better rebuild it.
	 *
	 * The JunkFilter, its result slot, and its tupledesc are kept in a
	 * subsidiary memory context so that we can free them easily when needed.
	 */
	if (fcache->func->rettype != VOIDOID &&
		(fcache->junkFilter == NULL ||
		 fcache->jf_generation != fcache->cplan->generation))
	{
		TupleTableSlot *slot;
		List	   *resulttlist;
		MemoryContext oldcontext;

		/* Create or reset the jfcontext */
		if (fcache->jfcontext == NULL)
			fcache->jfcontext = AllocSetContextCreate(fcache->fcontext,
													  "SQL function junkfilter",
													  ALLOCSET_SMALL_SIZES);
		else
			MemoryContextReset(fcache->jfcontext);
		oldcontext = MemoryContextSwitchTo(fcache->jfcontext);

		slot = MakeSingleTupleTableSlot(NULL, &TTSOpsMinimalTuple);

		/*
		 * Re-fetch the (possibly modified) output tlist of the final
		 * statement.  By this point, we should have thrown an error if there
		 * is not one.
		 */
		resulttlist = get_sql_fn_result_tlist(plansource->query_list);

		/*
		 * If the result is composite, *and* we are returning the whole tuple
		 * result, we need to insert nulls for any dropped columns.  In the
		 * single-column-result case, there might be dropped columns within
		 * the composite column value, but it's not our problem here.  There
		 * should be no resjunk entries in resulttlist, so in the second case
		 * the JunkFilter is certainly a no-op.
		 */
		if (fcache->func->rettupdesc && fcache->func->returnsTuple)
			fcache->junkFilter = ExecInitJunkFilterConversion(resulttlist,
															  fcache->func->rettupdesc,
															  slot);
		else
			fcache->junkFilter = ExecInitJunkFilter(resulttlist, slot);

		/*
		 * The resulttlist tree belongs to the plancache and might disappear
		 * underneath us due to plancache invalidation.  While we could
		 * forestall that by copying it, that'd just be a waste of cycles,
		 * because the junkfilter doesn't need it anymore.  (It'd only be used
		 * by ExecFindJunkAttribute(), which we don't use here.)  To ensure
		 * there's not a dangling pointer laying about, clear the junkFilter's
		 * pointer.
		 */
		fcache->junkFilter->jf_targetList = NIL;

		/* Make sure output rowtype is properly blessed */
		if (fcache->func->returnsTuple)
			BlessTupleDesc(fcache->junkFilter->jf_resultSlot->tts_tupleDescriptor);

		/* Mark the JunkFilter as up-to-date */
		fcache->jf_generation = fcache->cplan->generation;

		MemoryContextSwitchTo(oldcontext);
	}

	if (fcache->func->returnsSet &&
		!fcache->func->returnsTuple &&
		type_is_rowtype(fcache->func->rettype))
	{
		/*
		 * Returning rowtype as if it were scalar --- materialize won't work.
		 * Right now it's sufficient to override any caller preference for
		 * materialize mode, but this might need more work in future.
		 */
		fcache->lazyEvalOK = true;
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
	 * output from the last statement in such a function.
	 */
	if (lasttages && fcache->junkFilter)
	{
		lasttages->setsResult = true;
		if (fcache->lazyEvalOK &&
			lasttages->stmt->commandType == CMD_SELECT &&
			!lasttages->stmt->hasModifyingCTE)
			fcache->lazyEval = lasttages->lazyEval = true;
	}

	return true;
}

/*
 * Convert the SQL function's next query from source form (RawStmt or Query)
 * into a CachedPlanSource.  If it's the last query, also determine whether
 * the function returnsTuple.
 */
static void
prepare_next_query(SQLFunctionHashEntry *func)
{
	int			qindex;
	bool		islast;
	CachedPlanSource *plansource;
	List	   *queryTree_list;
	MemoryContext oldcontext;

	/* Which query should we process? */
	qindex = list_length(func->plansource_list);
	Assert(qindex < func->num_queries); /* else caller error */
	islast = (qindex + 1 >= func->num_queries);

	/*
	 * Parse and/or rewrite the query, creating a CachedPlanSource that holds
	 * a copy of the original parsetree.  Note fine point: we make a copy of
	 * each original parsetree to ensure that the source_list in pcontext
	 * remains unmodified during parse analysis and rewrite.  This is normally
	 * unnecessary, but we have to do it in case an error is raised during
	 * parse analysis.  Otherwise, a fresh attempt to execute the function
	 * will arrive back here and try to work from a corrupted source_list.
	 */
	if (!func->raw_source)
	{
		/* Source queries are already parse-analyzed */
		Query	   *parsetree = list_nth_node(Query, func->source_list, qindex);

		parsetree = copyObject(parsetree);
		plansource = CreateCachedPlanForQuery(parsetree,
											  func->src,
											  CreateCommandTag((Node *) parsetree));
		AcquireRewriteLocks(parsetree, true, false);
		queryTree_list = pg_rewrite_query(parsetree);
	}
	else
	{
		/* Source queries are raw parsetrees */
		RawStmt    *parsetree = list_nth_node(RawStmt, func->source_list, qindex);

		parsetree = copyObject(parsetree);
		plansource = CreateCachedPlan(parsetree,
									  func->src,
									  CreateCommandTag(parsetree->stmt));
		queryTree_list = pg_analyze_and_rewrite_withcb(parsetree,
													   func->src,
													   (ParserSetupHook) sql_fn_parser_setup,
													   func->pinfo,
													   NULL);
	}

	/*
	 * Check that there are no statements we don't want to allow.
	 */
	check_sql_fn_statement(queryTree_list);

	/*
	 * If this is the last query, check that the function returns the type it
	 * claims to.  Although in simple cases this was already done when the
	 * function was defined, we have to recheck because database objects used
	 * in the function's queries might have changed type.  We'd have to
	 * recheck anyway if the function had any polymorphic arguments. Moreover,
	 * check_sql_stmt_retval takes care of injecting any required column type
	 * coercions.  (But we don't ask it to insert nulls for dropped columns;
	 * the junkfilter handles that.)
	 *
	 * Note: we set func->returnsTuple according to whether we are returning
	 * the whole tuple result or just a single column.  In the latter case we
	 * clear returnsTuple because we need not act different from the scalar
	 * result case, even if it's a rowtype column.  (However, we have to force
	 * lazy eval mode in that case; otherwise we'd need extra code to expand
	 * the rowtype column into multiple columns, since we have no way to
	 * notify the caller that it should do that.)
	 */
	if (islast)
		func->returnsTuple = check_sql_stmt_retval(queryTree_list,
												   func->rettype,
												   func->rettupdesc,
												   func->prokind,
												   false);

	/*
	 * Now that check_sql_stmt_retval has done its thing, we can complete plan
	 * cache entry creation.
	 */
	CompleteCachedPlan(plansource,
					   queryTree_list,
					   NULL,
					   NULL,
					   0,
					   (ParserSetupHook) sql_fn_parser_setup,
					   func->pinfo,
					   CURSOR_OPT_PARALLEL_OK | CURSOR_OPT_NO_SCROLL,
					   false);

	/*
	 * Install post-rewrite hook.  Its arg is the hash entry if this is the
	 * last statement, else NULL.
	 */
	SetPostRewriteHook(plansource,
					   sql_postrewrite_callback,
					   islast ? func : NULL);

	/*
	 * While the CachedPlanSources can take care of themselves, our List
	 * pointing to them had better be in the hcontext.
	 */
	oldcontext = MemoryContextSwitchTo(func->hcontext);
	func->plansource_list = lappend(func->plansource_list, plansource);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * As soon as we've linked the CachedPlanSource into the list, mark it as
	 * "saved".
	 */
	SaveCachedPlan(plansource);

	/*
	 * Finally, if this was the last statement, we can flush the pcontext with
	 * the original query trees; they're all safely copied into
	 * CachedPlanSources now.
	 */
	if (islast)
	{
		func->source_list = NIL;	/* avoid dangling pointer */
		MemoryContextDelete(func->pcontext);
		func->pcontext = NULL;
	}
}

/*
 * Fill a new SQLFunctionHashEntry.
 *
 * The passed-in "cfunc" struct is expected to be zeroes, except
 * for the CachedFunction fields, which we don't touch here.
 *
 * We expect to be called in a short-lived memory context (typically a
 * query's per-tuple context).  Data that is to be part of the hash entry
 * must be copied into the hcontext or pcontext as appropriate.
 */
static void
sql_compile_callback(FunctionCallInfo fcinfo,
					 HeapTuple procedureTuple,
					 const CachedFunctionHashKey *hashkey,
					 CachedFunction *cfunc,
					 bool forValidator)
{
	SQLFunctionHashEntry *func = (SQLFunctionHashEntry *) cfunc;
	Form_pg_proc procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);
	ErrorContextCallback comperrcontext;
	MemoryContext hcontext;
	MemoryContext pcontext;
	MemoryContext oldcontext = CurrentMemoryContext;
	Oid			rettype;
	TupleDesc	rettupdesc;
	Datum		tmp;
	bool		isNull;
	List	   *source_list;

	/*
	 * Setup error traceback support for ereport() during compile.  (This is
	 * mainly useful for reporting parse errors from pg_parse_query.)
	 */
	comperrcontext.callback = sql_compile_error_callback;
	comperrcontext.arg = func;
	comperrcontext.previous = error_context_stack;
	error_context_stack = &comperrcontext;

	/*
	 * Create the hash entry's memory context.  For now it's a child of the
	 * caller's context, so that it will go away if we fail partway through.
	 */
	hcontext = AllocSetContextCreate(CurrentMemoryContext,
									 "SQL function",
									 ALLOCSET_SMALL_SIZES);

	/*
	 * Create the not-as-long-lived pcontext.  We make this a child of
	 * hcontext so that it doesn't require separate deletion.
	 */
	pcontext = AllocSetContextCreate(hcontext,
									 "SQL function parse trees",
									 ALLOCSET_SMALL_SIZES);
	func->pcontext = pcontext;

	/*
	 * copy function name immediately for use by error reporting callback, and
	 * for use as memory context identifier
	 */
	func->fname = MemoryContextStrdup(hcontext,
									  NameStr(procedureStruct->proname));
	MemoryContextSetIdentifier(hcontext, func->fname);

	/*
	 * Resolve any polymorphism, obtaining the actual result type, and the
	 * corresponding tupdesc if it's a rowtype.
	 */
	(void) get_call_result_type(fcinfo, &rettype, &rettupdesc);

	func->rettype = rettype;
	if (rettupdesc)
	{
		MemoryContextSwitchTo(hcontext);
		func->rettupdesc = CreateTupleDescCopy(rettupdesc);
		MemoryContextSwitchTo(oldcontext);
	}

	/* Fetch the typlen and byval info for the result type */
	get_typlenbyval(rettype, &func->typlen, &func->typbyval);

	/* Remember whether we're returning setof something */
	func->returnsSet = procedureStruct->proretset;

	/* Remember if function is STABLE/IMMUTABLE */
	func->readonly_func =
		(procedureStruct->provolatile != PROVOLATILE_VOLATILE);

	/* Remember routine kind */
	func->prokind = procedureStruct->prokind;

	/*
	 * We need the actual argument types to pass to the parser.  Also make
	 * sure that parameter symbols are considered to have the function's
	 * resolved input collation.
	 */
	MemoryContextSwitchTo(hcontext);
	func->pinfo = prepare_sql_fn_parse_info(procedureTuple,
											fcinfo->flinfo->fn_expr,
											PG_GET_COLLATION());
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Now that we have the resolved argument types, collect their typlens for
	 * use in postquel_sub_params.
	 */
	func->argtyplen = (int16 *)
		MemoryContextAlloc(hcontext, func->pinfo->nargs * sizeof(int16));
	for (int i = 0; i < func->pinfo->nargs; i++)
		func->argtyplen[i] = get_typlen(func->pinfo->argtypes[i]);

	/*
	 * And of course we need the function body text.
	 */
	tmp = SysCacheGetAttrNotNull(PROCOID, procedureTuple, Anum_pg_proc_prosrc);
	func->src = MemoryContextStrdup(hcontext,
									TextDatumGetCString(tmp));

	/* If we have prosqlbody, pay attention to that not prosrc. */
	tmp = SysCacheGetAttr(PROCOID,
						  procedureTuple,
						  Anum_pg_proc_prosqlbody,
						  &isNull);
	if (!isNull)
	{
		/* Source queries are already parse-analyzed */
		Node	   *n;

		n = stringToNode(TextDatumGetCString(tmp));
		if (IsA(n, List))
			source_list = linitial_node(List, castNode(List, n));
		else
			source_list = list_make1(n);
		func->raw_source = false;
	}
	else
	{
		/* Source queries are raw parsetrees */
		source_list = pg_parse_query(func->src);
		func->raw_source = true;
	}

	/*
	 * Note: we must save the number of queries so that we'll still remember
	 * how many there are after we discard source_list.
	 */
	func->num_queries = list_length(source_list);

	/*
	 * Edge case: empty function body is OK only if it returns VOID.  Normally
	 * we validate that the last statement returns the right thing in
	 * check_sql_stmt_retval, but we'll never reach that if there's no last
	 * statement.
	 */
	if (func->num_queries == 0 && rettype != VOIDOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("return type mismatch in function declared to return %s",
						format_type_be(rettype)),
				 errdetail("Function's final statement must be SELECT or INSERT/UPDATE/DELETE/MERGE RETURNING.")));

	/* Save the source trees in pcontext for now. */
	MemoryContextSwitchTo(pcontext);
	func->source_list = copyObject(source_list);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * We now have a fully valid hash entry, so reparent hcontext under
	 * CacheMemoryContext to make all the subsidiary data long-lived, and only
	 * then install the hcontext link so that sql_delete_callback will know to
	 * delete it.
	 */
	MemoryContextSetParent(hcontext, CacheMemoryContext);
	func->hcontext = hcontext;

	error_context_stack = comperrcontext.previous;
}

/*
 * Deletion callback used by funccache.c.
 *
 * Free any free-able subsidiary data of cfunc, but not the
 * struct CachedFunction itself.
 */
static void
sql_delete_callback(CachedFunction *cfunc)
{
	SQLFunctionHashEntry *func = (SQLFunctionHashEntry *) cfunc;
	ListCell   *lc;

	/* Release the CachedPlanSources */
	foreach(lc, func->plansource_list)
	{
		CachedPlanSource *plansource = (CachedPlanSource *) lfirst(lc);

		DropCachedPlan(plansource);
	}
	func->plansource_list = NIL;

	/*
	 * If we have an hcontext, free it, thereby getting rid of all subsidiary
	 * data.  (If we still have a pcontext, this gets rid of that too.)
	 */
	if (func->hcontext)
		MemoryContextDelete(func->hcontext);
	func->hcontext = NULL;
}

/*
 * Post-rewrite callback used by plancache.c.
 *
 * This must match the processing that prepare_next_query() does between
 * rewriting and calling CompleteCachedPlan().
 */
static void
sql_postrewrite_callback(List *querytree_list, void *arg)
{
	/*
	 * Check that there are no statements we don't want to allow.  (Presently,
	 * there's no real point in this because the result can't change from what
	 * we saw originally.  But it's cheap and maybe someday it will matter.)
	 */
	check_sql_fn_statement(querytree_list);

	/*
	 * If this is the last query, we must re-do what check_sql_stmt_retval did
	 * to its targetlist.  Also check that returnsTuple didn't change (it
	 * probably cannot, but be cautious).
	 */
	if (arg != NULL)
	{
		SQLFunctionHashEntry *func = (SQLFunctionHashEntry *) arg;
		bool		returnsTuple;

		returnsTuple = check_sql_stmt_retval(querytree_list,
											 func->rettype,
											 func->rettupdesc,
											 func->prokind,
											 false);
		if (returnsTuple != func->returnsTuple)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cached plan must not change result type")));
	}
}

/* Start up execution of one execution_state node */
static void
postquel_start(execution_state *es, SQLFunctionCachePtr fcache)
{
	DestReceiver *dest;
	MemoryContext oldcontext = CurrentMemoryContext;

	Assert(es->qd == NULL);

	/* Caller should have ensured a suitable snapshot is active */
	Assert(ActiveSnapshotSet());

	/*
	 * In lazyEval mode for a SRF, we must run the sub-executor in a child of
	 * fcontext, so that it can survive across multiple calls to fmgr_sql.
	 * (XXX in the case of a long-lived FmgrInfo, this policy potentially
	 * causes memory leakage, but it's not very clear where we could keep
	 * stuff instead.  Fortunately, there are few if any cases where
	 * set-returning functions are invoked via FmgrInfos that would outlive
	 * the calling query.)  Otherwise, we're going to run it to completion
	 * before exiting fmgr_sql, so it can perfectly well run in the caller's
	 * context.
	 */
	if (es->lazyEval && fcache->func->returnsSet)
	{
		fcache->subcontext = AllocSetContextCreate(fcache->fcontext,
												   "SQL function execution",
												   ALLOCSET_DEFAULT_SIZES);
		fcache->ownSubcontext = true;
	}
	else if (es->stmt->commandType == CMD_UTILITY)
	{
		/*
		 * The code path using a sub-executor is pretty good about cleaning up
		 * cruft, since the executor will make its own sub-context.  We don't
		 * really need an additional layer of sub-context in that case.
		 * However, if this is a utility statement, it won't make its own
		 * sub-context, so it seems advisable to make one that we can free on
		 * completion.
		 */
		fcache->subcontext = AllocSetContextCreate(CurrentMemoryContext,
												   "SQL function execution",
												   ALLOCSET_DEFAULT_SIZES);
		fcache->ownSubcontext = true;
	}
	else
	{
		fcache->subcontext = CurrentMemoryContext;
		fcache->ownSubcontext = false;
	}

	/*
	 * Build a tuplestore if needed, that is if it's a set-returning function
	 * and we're producing the function result without using lazyEval mode.
	 */
	if (es->setsResult)
	{
		Assert(fcache->tstore == NULL);
		if (fcache->func->returnsSet && !es->lazyEval)
		{
			MemoryContextSwitchTo(fcache->tscontext);
			fcache->tstore = tuplestore_begin_heap(fcache->randomAccess,
												   false, work_mem);
		}
	}

	/* Switch into the selected subcontext (might be a no-op) */
	MemoryContextSwitchTo(fcache->subcontext);

	/*
	 * If this query produces the function result, collect its output using
	 * our custom DestReceiver; else discard any output.
	 */
	if (es->setsResult)
	{
		DR_sqlfunction *myState;

		dest = CreateDestReceiver(DestSQLFunction);
		/* pass down the needed info to the dest receiver routines */
		myState = (DR_sqlfunction *) dest;
		Assert(myState->pub.mydest == DestSQLFunction);
		myState->tstore = fcache->tstore;	/* might be NULL */
		myState->filter = fcache->junkFilter;

		/* Make very sure the junkfilter's result slot is empty */
		ExecClearTuple(fcache->junkFilter->jf_resultSlot);
	}
	else
		dest = None_Receiver;

	es->qd = CreateQueryDesc(es->stmt,
							 fcache->func->src,
							 GetActiveSnapshot(),
							 InvalidSnapshot,
							 dest,
							 fcache->paramLI,
							 es->qd ? es->qd->queryEnv : NULL,
							 0);

	/* Utility commands don't need Executor. */
	if (es->qd->operation != CMD_UTILITY)
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

	MemoryContextSwitchTo(oldcontext);
}

/* Run one execution_state; either to completion or to first result row */
/* Returns true if we ran to completion */
static bool
postquel_getnext(execution_state *es, SQLFunctionCachePtr fcache)
{
	bool		result;
	MemoryContext oldcontext;

	/* Run the sub-executor in subcontext */
	oldcontext = MemoryContextSwitchTo(fcache->subcontext);

	if (es->qd->operation == CMD_UTILITY)
	{
		ProcessUtility(es->qd->plannedstmt,
					   fcache->func->src,
					   true,	/* protect function cache's parsetree */
					   PROCESS_UTILITY_QUERY,
					   es->qd->params,
					   es->qd->queryEnv,
					   es->qd->dest,
					   NULL);
		result = true;			/* never stops early */
	}
	else
	{
		/* Run regular commands to completion unless lazyEval */
		uint64		count = (es->lazyEval) ? 1 : 0;

		ExecutorRun(es->qd, ForwardScanDirection, count);

		/*
		 * If we requested run to completion OR there was no tuple returned,
		 * command must be complete.
		 */
		result = (count == 0 || es->qd->estate->es_processed == 0);
	}

	MemoryContextSwitchTo(oldcontext);

	return result;
}

/* Shut down execution of one execution_state node */
static void
postquel_end(execution_state *es, SQLFunctionCachePtr fcache)
{
	MemoryContext oldcontext;

	/* Run the sub-executor in subcontext */
	oldcontext = MemoryContextSwitchTo(fcache->subcontext);

	/* mark status done to ensure we don't do ExecutorEnd twice */
	es->status = F_EXEC_DONE;

	/* Utility commands don't need Executor. */
	if (es->qd->operation != CMD_UTILITY)
	{
		ExecutorFinish(es->qd);
		ExecutorEnd(es->qd);
	}

	es->qd->dest->rDestroy(es->qd->dest);

	FreeQueryDesc(es->qd);
	es->qd = NULL;

	MemoryContextSwitchTo(oldcontext);

	/* Delete the subcontext, if it's actually a separate context */
	if (fcache->ownSubcontext)
		MemoryContextDelete(fcache->subcontext);
	fcache->subcontext = NULL;
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
		Oid		   *argtypes = fcache->func->pinfo->argtypes;
		int16	   *argtyplen = fcache->func->argtyplen;

		if (fcache->paramLI == NULL)
		{
			/* First time through: build a persistent ParamListInfo struct */
			MemoryContext oldcontext;

			oldcontext = MemoryContextSwitchTo(fcache->fcontext);
			paramLI = makeParamList(nargs);
			fcache->paramLI = paramLI;
			MemoryContextSwitchTo(oldcontext);
		}
		else
		{
			paramLI = fcache->paramLI;
			Assert(paramLI->numParams == nargs);
		}

		for (int i = 0; i < nargs; i++)
		{
			ParamExternData *prm = &paramLI->params[i];

			/*
			 * If an incoming parameter value is a R/W expanded datum, we
			 * force it to R/O.  We'd be perfectly entitled to scribble on it,
			 * but the problem is that if the parameter is referenced more
			 * than once in the function, earlier references might mutate the
			 * value seen by later references, which won't do at all.  We
			 * could do better if we could be sure of the number of Param
			 * nodes in the function's plans; but we might not have planned
			 * all the statements yet, nor do we have plan tree walker
			 * infrastructure.  (Examining the parse trees is not good enough,
			 * because of possible function inlining during planning.)
			 */
			prm->isnull = fcinfo->args[i].isnull;
			prm->value = MakeExpandedObjectReadOnly(fcinfo->args[i].value,
													prm->isnull,
													argtyplen[i]);
			/* Allow the value to be substituted into custom plans */
			prm->pflags = PARAM_FLAG_CONST;
			prm->ptype = argtypes[i];
		}
	}
	else
		fcache->paramLI = NULL;
}

/*
 * Extract the SQL function's value from a single result row.  This is used
 * both for scalar (non-set) functions and for each row of a lazy-eval set
 * result.  We expect the current memory context to be that of the caller
 * of fmgr_sql.
 */
static Datum
postquel_get_single_result(TupleTableSlot *slot,
						   FunctionCallInfo fcinfo,
						   SQLFunctionCachePtr fcache)
{
	Datum		value;

	/*
	 * Set up to return the function value.  For pass-by-reference datatypes,
	 * be sure to copy the result into the current context.  We can't leave
	 * the data in the TupleTableSlot because we must clear the slot before
	 * returning.
	 */
	if (fcache->func->returnsTuple)
	{
		/* We must return the whole tuple as a Datum. */
		fcinfo->isnull = false;
		value = ExecFetchSlotHeapTupleDatum(slot);
	}
	else
	{
		/*
		 * Returning a scalar, which we have to extract from the first column
		 * of the SELECT result, and then copy into current context if needed.
		 */
		value = slot_getattr(slot, 1, &(fcinfo->isnull));

		if (!fcinfo->isnull)
			value = datumCopy(value, fcache->func->typbyval, fcache->func->typlen);
	}

	/* Clear the slot for next time */
	ExecClearTuple(slot);

	return value;
}

/*
 * fmgr_sql: function call manager for SQL functions
 */
Datum
fmgr_sql(PG_FUNCTION_ARGS)
{
	SQLFunctionCachePtr fcache;
	ErrorContextCallback sqlerrcontext;
	MemoryContext tscontext;
	bool		randomAccess;
	bool		lazyEvalOK;
	bool		pushed_snapshot;
	execution_state *es;
	TupleTableSlot *slot;
	Datum		result;

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
		/* tuplestore, if used, must have query lifespan */
		tscontext = rsi->econtext->ecxt_per_query_memory;
	}
	else
	{
		randomAccess = false;
		lazyEvalOK = true;
		/* we won't need a tuplestore */
		tscontext = NULL;
	}

	/*
	 * Initialize fcache if starting a fresh execution.
	 */
	fcache = init_sql_fcache(fcinfo, lazyEvalOK);

	/* Remember info that we might need later to construct tuplestore */
	fcache->tscontext = tscontext;
	fcache->randomAccess = randomAccess;

	/*
	 * Now we can set up error traceback support for ereport()
	 */
	sqlerrcontext.callback = sql_exec_error_callback;
	sqlerrcontext.arg = fcache;
	sqlerrcontext.previous = error_context_stack;
	error_context_stack = &sqlerrcontext;

	/*
	 * Find first unfinished execution_state.  If none, advance to the next
	 * query in function.
	 */
	do
	{
		es = fcache->eslist;
		while (es && es->status == F_EXEC_DONE)
			es = es->next;
		if (es)
			break;
	} while (init_execution_state(fcache));

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
			if (!fcache->func->readonly_func)
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
		else if (!fcache->func->readonly_func && !pushed_snapshot)
		{
			/* Re-establish active snapshot when re-entering function */
			PushActiveSnapshot(es->qd->snapshot);
			pushed_snapshot = true;
		}

		completed = postquel_getnext(es, fcache);

		/*
		 * If we ran the command to completion, we can shut it down now. Any
		 * row(s) we need to return are safely stashed in the result slot or
		 * tuplestore, and we want to be sure that, for example, AFTER
		 * triggers get fired before we return anything.  Also, if the
		 * function doesn't return set, we can shut it down anyway because it
		 * must be a SELECT and we don't care about fetching any more result
		 * rows.
		 */
		if (completed || !fcache->func->returnsSet)
			postquel_end(es, fcache);

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
		 * Advance to next execution_state, and perhaps next query.
		 */
		es = es->next;
		while (!es)
		{
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

			if (!init_execution_state(fcache))
				break;			/* end of function */

			es = fcache->eslist;
		}
	}

	/*
	 * The result slot or tuplestore now contains whatever row(s) we are
	 * supposed to return.
	 */
	if (fcache->func->returnsSet)
	{
		ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;

		if (es)
		{
			/*
			 * If we stopped short of being done, we must have a lazy-eval
			 * row.
			 */
			Assert(es->lazyEval);
			/* The junkfilter's result slot contains the query result tuple */
			Assert(fcache->junkFilter);
			slot = fcache->junkFilter->jf_resultSlot;
			Assert(!TTS_EMPTY(slot));
			/* Extract the result as a datum, and copy out from the slot */
			result = postquel_get_single_result(slot, fcinfo, fcache);

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
			 * We are done with a lazy evaluation.  Let caller know we're
			 * finished.
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
			 *
			 * Note an edge case: we could get here without having made a
			 * tuplestore if the function is declared to return SETOF VOID.
			 * ExecMakeTableFunctionResult will cope with null setResult.
			 */
			Assert(fcache->tstore || fcache->func->rettype == VOIDOID);
			rsi->returnMode = SFRM_Materialize;
			rsi->setResult = fcache->tstore;
			fcache->tstore = NULL;
			/* must copy desc because execSRF.c will free it */
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
			/* The junkfilter's result slot contains the query result tuple */
			slot = fcache->junkFilter->jf_resultSlot;
			if (!TTS_EMPTY(slot))
				result = postquel_get_single_result(slot, fcinfo, fcache);
			else
			{
				fcinfo->isnull = true;
				result = (Datum) 0;
			}
		}
		else
		{
			/* Should only get here for VOID functions and procedures */
			Assert(fcache->func->rettype == VOIDOID);
			fcinfo->isnull = true;
			result = (Datum) 0;
		}
	}

	/* Pop snapshot if we have pushed one */
	if (pushed_snapshot)
		PopActiveSnapshot();

	/*
	 * If we've gone through every command in the function, we are done. Reset
	 * state to start over again on next call.
	 */
	if (es == NULL)
		fcache->eslist = NULL;

	error_context_stack = sqlerrcontext.previous;

	return result;
}


/*
 * error context callback to let us supply a traceback during compile
 */
static void
sql_compile_error_callback(void *arg)
{
	SQLFunctionHashEntry *func = (SQLFunctionHashEntry *) arg;
	int			syntaxerrposition;

	/*
	 * We can do nothing useful if sql_compile_callback() didn't get as far as
	 * copying the function name
	 */
	if (func->fname == NULL)
		return;

	/*
	 * If there is a syntax error position, convert to internal syntax error
	 */
	syntaxerrposition = geterrposition();
	if (syntaxerrposition > 0 && func->src != NULL)
	{
		errposition(0);
		internalerrposition(syntaxerrposition);
		internalerrquery(func->src);
	}

	/*
	 * sql_compile_callback() doesn't do any per-query processing, so just
	 * report the context as "during startup".
	 */
	errcontext("SQL function \"%s\" during startup", func->fname);
}

/*
 * error context callback to let us supply a call-stack traceback at runtime
 */
static void
sql_exec_error_callback(void *arg)
{
	SQLFunctionCachePtr fcache = (SQLFunctionCachePtr) arg;
	int			syntaxerrposition;

	/*
	 * If there is a syntax error position, convert to internal syntax error
	 */
	syntaxerrposition = geterrposition();
	if (syntaxerrposition > 0 && fcache->func->src != NULL)
	{
		errposition(0);
		internalerrposition(syntaxerrposition);
		internalerrquery(fcache->func->src);
	}

	/*
	 * If we failed while executing an identifiable query within the function,
	 * report that.  Otherwise say it was "during startup".
	 */
	if (fcache->error_query_index > 0)
		errcontext("SQL function \"%s\" statement %d",
				   fcache->func->fname, fcache->error_query_index);
	else
		errcontext("SQL function \"%s\" during startup", fcache->func->fname);
}


/*
 * ExprContext callback function
 *
 * We register this in the active ExprContext while a set-returning SQL
 * function is running, in case the function needs to be shut down before it
 * has been run to completion.  Note that this will not be called during an
 * error abort, but we don't need it because transaction abort will take care
 * of releasing executor resources.
 */
static void
ShutdownSQLFunction(Datum arg)
{
	SQLFunctionCachePtr fcache = (SQLFunctionCachePtr) DatumGetPointer(arg);
	execution_state *es;

	es = fcache->eslist;
	while (es)
	{
		/* Shut down anything still running */
		if (es->status == F_EXEC_RUN)
		{
			/* Re-establish active snapshot for any called functions */
			if (!fcache->func->readonly_func)
				PushActiveSnapshot(es->qd->snapshot);

			postquel_end(es, fcache);

			if (!fcache->func->readonly_func)
				PopActiveSnapshot();
		}
		es = es->next;
	}
	fcache->eslist = NULL;

	/* Release tuplestore if we have one */
	if (fcache->tstore)
		tuplestore_end(fcache->tstore);
	fcache->tstore = NULL;

	/* Release CachedPlan if we have one */
	if (fcache->cplan)
		ReleaseCachedPlan(fcache->cplan, fcache->cowner);
	fcache->cplan = NULL;

	/* execUtils will deregister the callback... */
	fcache->shutdown_reg = false;
}

/*
 * MemoryContext callback function
 *
 * We register this in the memory context that contains a SQLFunctionCache
 * struct.  When the memory context is reset or deleted, we release the
 * reference count (if any) that the cache holds on the long-lived hash entry.
 * Note that this will happen even during error aborts.
 */
static void
RemoveSQLFunctionCache(void *arg)
{
	SQLFunctionCache *fcache = (SQLFunctionCache *) arg;

	/* Release reference count on SQLFunctionHashEntry */
	if (fcache->func != NULL)
	{
		Assert(fcache->func->cfunc.use_count > 0);
		fcache->func->cfunc.use_count--;
		/* This should be unnecessary, but let's just be sure: */
		fcache->func = NULL;
	}
}

/*
 * check_sql_fn_statements
 *
 * Check statements in an SQL function.  Error out if there is anything that
 * is not acceptable.
 */
void
check_sql_fn_statements(List *queryTreeLists)
{
	ListCell   *lc;

	/* We are given a list of sublists of Queries */
	foreach(lc, queryTreeLists)
	{
		List	   *sublist = lfirst_node(List, lc);

		check_sql_fn_statement(sublist);
	}
}

/*
 * As above, for a single sublist of Queries.
 */
static void
check_sql_fn_statement(List *queryTreeList)
{
	ListCell   *lc;

	foreach(lc, queryTreeList)
	{
		Query	   *query = lfirst_node(Query, lc);

		/*
		 * Disallow calling procedures with output arguments.  The current
		 * implementation would just throw the output values away, unless the
		 * statement is the last one.  Per SQL standard, we should assign the
		 * output values by name.  By disallowing this here, we preserve an
		 * opportunity for future improvement.
		 */
		if (query->commandType == CMD_UTILITY &&
			IsA(query->utilityStmt, CallStmt))
		{
			CallStmt   *stmt = (CallStmt *) query->utilityStmt;

			if (stmt->outargs != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("calling procedures with output arguments is not supported in SQL functions")));
		}
	}
}

/*
 * check_sql_fn_retval()
 *		Check return value of a list of lists of sql parse trees.
 *
 * The return value of a sql function is the value returned by the last
 * canSetTag query in the function.  We do some ad-hoc type checking and
 * coercion here to ensure that the function returns what it's supposed to.
 * Note that we may actually modify the last query to make it match!
 *
 * This function returns true if the sql function returns the entire tuple
 * result of its final statement, or false if it returns just the first column
 * result of that statement.  It throws an error if the final statement doesn't
 * return the right type at all.
 *
 * Note that because we allow "SELECT rowtype_expression", the result can be
 * false even when the declared function return type is a rowtype.
 *
 * For a polymorphic function the passed rettype must be the actual resolved
 * output type of the function.  (This means we can't check the type during
 * function definition of a polymorphic function.)  If we do see a polymorphic
 * rettype we'll throw an error, saying it is not a supported rettype.
 *
 * If the function returns composite, the passed rettupdesc should describe
 * the expected output.  If rettupdesc is NULL, we can't verify that the
 * output matches; that should only happen in fmgr_sql_validator(), or when
 * the function returns RECORD and the caller doesn't actually care which
 * composite type it is.
 *
 * (Typically, rettype and rettupdesc are computed by get_call_result_type
 * or a sibling function.)
 *
 * In addition to coercing individual output columns, we can modify the
 * output to include dummy NULL columns for any dropped columns appearing
 * in rettupdesc.  This is done only if the caller asks for it.
 */
bool
check_sql_fn_retval(List *queryTreeLists,
					Oid rettype, TupleDesc rettupdesc,
					char prokind,
					bool insertDroppedCols)
{
	List	   *queryTreeList;

	/*
	 * We consider only the last sublist of Query nodes, so that only the last
	 * original statement is a candidate to produce the result.  This is a
	 * change from pre-v18 versions, which would back up to the last statement
	 * that includes a canSetTag query, thus ignoring any ending statement(s)
	 * that rewrite to DO INSTEAD NOTHING.  That behavior was undocumented and
	 * there seems no good reason for it, except that it was an artifact of
	 * the original coding.
	 *
	 * If the function body is completely empty, handle that the same as if
	 * the last query had rewritten to nothing.
	 */
	if (queryTreeLists != NIL)
		queryTreeList = llast_node(List, queryTreeLists);
	else
		queryTreeList = NIL;

	return check_sql_stmt_retval(queryTreeList,
								 rettype, rettupdesc,
								 prokind, insertDroppedCols);
}

/*
 * As for check_sql_fn_retval, but we are given just the last query's
 * rewritten-queries list.
 */
static bool
check_sql_stmt_retval(List *queryTreeList,
					  Oid rettype, TupleDesc rettupdesc,
					  char prokind, bool insertDroppedCols)
{
	bool		is_tuple_result = false;
	Query	   *parse;
	ListCell   *parse_cell;
	List	   *tlist;
	int			tlistlen;
	bool		tlist_is_modifiable;
	char		fn_typtype;
	List	   *upper_tlist = NIL;
	bool		upper_tlist_nontrivial = false;
	ListCell   *lc;

	/*
	 * If it's declared to return VOID, we don't care what's in the function.
	 * (This takes care of procedures with no output parameters, as well.)
	 */
	if (rettype == VOIDOID)
		return false;

	/*
	 * Find the last canSetTag query in the list of Query nodes.  This isn't
	 * necessarily the last parsetree, because rule rewriting can insert
	 * queries after what the user wrote.
	 */
	parse = NULL;
	parse_cell = NULL;
	foreach(lc, queryTreeList)
	{
		Query	   *q = lfirst_node(Query, lc);

		if (q->canSetTag)
		{
			parse = q;
			parse_cell = lc;
		}
	}

	/*
	 * If it's a plain SELECT, it returns whatever the targetlist says.
	 * Otherwise, if it's INSERT/UPDATE/DELETE/MERGE with RETURNING, it
	 * returns that. Otherwise, the function return type must be VOID.
	 *
	 * Note: eventually replace this test with QueryReturnsTuples?	We'd need
	 * a more general method of determining the output type, though.  Also, it
	 * seems too dangerous to consider FETCH or EXECUTE as returning a
	 * determinable rowtype, since they depend on relatively short-lived
	 * entities.
	 */
	if (parse &&
		parse->commandType == CMD_SELECT)
	{
		tlist = parse->targetList;
		/* tlist is modifiable unless it's a dummy in a setop query */
		tlist_is_modifiable = (parse->setOperations == NULL);
	}
	else if (parse &&
			 (parse->commandType == CMD_INSERT ||
			  parse->commandType == CMD_UPDATE ||
			  parse->commandType == CMD_DELETE ||
			  parse->commandType == CMD_MERGE) &&
			 parse->returningList)
	{
		tlist = parse->returningList;
		/* returningList can always be modified */
		tlist_is_modifiable = true;
	}
	else
	{
		/* Last statement is a utility command, or it rewrote to nothing */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("return type mismatch in function declared to return %s",
						format_type_be(rettype)),
				 errdetail("Function's final statement must be SELECT or INSERT/UPDATE/DELETE/MERGE RETURNING.")));
		return false;			/* keep compiler quiet */
	}

	/*
	 * OK, check that the targetlist returns something matching the declared
	 * type, and modify it if necessary.  If possible, we insert any coercion
	 * steps right into the final statement's targetlist.  However, that might
	 * risk changes in the statement's semantics --- we can't safely change
	 * the output type of a grouping column, for instance.  In such cases we
	 * handle coercions by inserting an extra level of Query that effectively
	 * just does a projection.
	 */

	/*
	 * Count the non-junk entries in the result targetlist.
	 */
	tlistlen = ExecCleanTargetListLength(tlist);

	fn_typtype = get_typtype(rettype);

	if (fn_typtype == TYPTYPE_BASE ||
		fn_typtype == TYPTYPE_DOMAIN ||
		fn_typtype == TYPTYPE_ENUM ||
		fn_typtype == TYPTYPE_RANGE ||
		fn_typtype == TYPTYPE_MULTIRANGE)
	{
		/*
		 * For scalar-type returns, the target list must have exactly one
		 * non-junk entry, and its type must be coercible to rettype.
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

		if (!coerce_fn_result_column(tle, rettype, -1,
									 tlist_is_modifiable,
									 &upper_tlist,
									 &upper_tlist_nontrivial))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("return type mismatch in function declared to return %s",
							format_type_be(rettype)),
					 errdetail("Actual return type is %s.",
							   format_type_be(exprType((Node *) tle->expr)))));
	}
	else if (fn_typtype == TYPTYPE_COMPOSITE || rettype == RECORDOID)
	{
		/*
		 * Returns a rowtype.
		 *
		 * Note that we will not consider a domain over composite to be a
		 * "rowtype" return type; it goes through the scalar case above.  This
		 * is because we only provide column-by-column implicit casting, and
		 * will not cast the complete record result.  So the only way to
		 * produce a domain-over-composite result is to compute it as an
		 * explicit single-column result.  The single-composite-column code
		 * path just below could handle such cases, but it won't be reached.
		 */
		int			tupnatts;	/* physical number of columns in tuple */
		int			tuplogcols; /* # of nondeleted columns in tuple */
		int			colindex;	/* physical column index */

		/*
		 * If the target list has one non-junk entry, and that expression has
		 * or can be coerced to the declared return type, take it as the
		 * result.  This allows, for example, 'SELECT func2()', where func2
		 * has the same composite return type as the function that's calling
		 * it.  This provision creates some ambiguity --- maybe the expression
		 * was meant to be the lone field of the composite result --- but it
		 * works well enough as long as we don't get too enthusiastic about
		 * inventing coercions from scalar to composite types.
		 *
		 * XXX Note that if rettype is RECORD and the expression is of a named
		 * composite type, or vice versa, this coercion will succeed, whether
		 * or not the record type really matches.  For the moment we rely on
		 * runtime type checking to catch any discrepancy, but it'd be nice to
		 * do better at parse time.
		 *
		 * We must *not* do this for a procedure, however.  Procedures with
		 * output parameter(s) have rettype RECORD, and the CALL code expects
		 * to get results corresponding to the list of output parameters, even
		 * when there's just one parameter that's composite.
		 */
		if (tlistlen == 1 && prokind != PROKIND_PROCEDURE)
		{
			TargetEntry *tle = (TargetEntry *) linitial(tlist);

			Assert(!tle->resjunk);
			if (coerce_fn_result_column(tle, rettype, -1,
										tlist_is_modifiable,
										&upper_tlist,
										&upper_tlist_nontrivial))
			{
				/* Note that we're NOT setting is_tuple_result */
				goto tlist_coercion_finished;
			}
		}

		/*
		 * If the caller didn't provide an expected tupdesc, we can't do any
		 * further checking.  Assume we're returning the whole tuple.
		 */
		if (rettupdesc == NULL)
			return true;

		/*
		 * Verify that the targetlist matches the return tuple type.  We scan
		 * the non-resjunk columns, and coerce them if necessary to match the
		 * datatypes of the non-deleted attributes.  For deleted attributes,
		 * insert NULL result columns if the caller asked for that.
		 */
		tupnatts = rettupdesc->natts;
		tuplogcols = 0;			/* we'll count nondeleted cols as we go */
		colindex = 0;

		foreach(lc, tlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			Form_pg_attribute attr;

			/* resjunk columns can simply be ignored */
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
				attr = TupleDescAttr(rettupdesc, colindex - 1);
				if (attr->attisdropped && insertDroppedCols)
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
					upper_tlist = lappend(upper_tlist,
										  makeTargetEntry(null_expr,
														  list_length(upper_tlist) + 1,
														  NULL,
														  false));
					upper_tlist_nontrivial = true;
				}
			} while (attr->attisdropped);
			tuplogcols++;

			if (!coerce_fn_result_column(tle,
										 attr->atttypid, attr->atttypmod,
										 tlist_is_modifiable,
										 &upper_tlist,
										 &upper_tlist_nontrivial))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("return type mismatch in function declared to return %s",
								format_type_be(rettype)),
						 errdetail("Final statement returns %s instead of %s at column %d.",
								   format_type_be(exprType((Node *) tle->expr)),
								   format_type_be(attr->atttypid),
								   tuplogcols)));
		}

		/* remaining columns in rettupdesc had better all be dropped */
		for (colindex++; colindex <= tupnatts; colindex++)
		{
			if (!TupleDescCompactAttr(rettupdesc, colindex - 1)->attisdropped)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("return type mismatch in function declared to return %s",
								format_type_be(rettype)),
						 errdetail("Final statement returns too few columns.")));
			if (insertDroppedCols)
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
				upper_tlist = lappend(upper_tlist,
									  makeTargetEntry(null_expr,
													  list_length(upper_tlist) + 1,
													  NULL,
													  false));
				upper_tlist_nontrivial = true;
			}
		}

		/* Report that we are returning entire tuple result */
		is_tuple_result = true;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("return type %s is not supported for SQL functions",
						format_type_be(rettype))));

tlist_coercion_finished:

	/*
	 * If necessary, modify the final Query by injecting an extra Query level
	 * that just performs a projection.  (It'd be dubious to do this to a
	 * non-SELECT query, but we never have to; RETURNING lists can always be
	 * modified in-place.)
	 */
	if (upper_tlist_nontrivial)
	{
		Query	   *newquery;
		List	   *colnames;
		RangeTblEntry *rte;
		RangeTblRef *rtr;

		Assert(parse->commandType == CMD_SELECT);

		/* Most of the upper Query struct can be left as zeroes/nulls */
		newquery = makeNode(Query);
		newquery->commandType = CMD_SELECT;
		newquery->querySource = parse->querySource;
		newquery->canSetTag = true;
		newquery->targetList = upper_tlist;

		/* We need a moderately realistic colnames list for the subquery RTE */
		colnames = NIL;
		foreach(lc, parse->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);

			if (tle->resjunk)
				continue;
			colnames = lappend(colnames,
							   makeString(tle->resname ? tle->resname : ""));
		}

		/* Build a suitable RTE for the subquery */
		rte = makeNode(RangeTblEntry);
		rte->rtekind = RTE_SUBQUERY;
		rte->subquery = parse;
		rte->eref = rte->alias = makeAlias("*SELECT*", colnames);
		rte->lateral = false;
		rte->inh = false;
		rte->inFromCl = true;
		newquery->rtable = list_make1(rte);

		rtr = makeNode(RangeTblRef);
		rtr->rtindex = 1;
		newquery->jointree = makeFromExpr(list_make1(rtr), NULL);

		/*
		 * Make sure the new query is marked as having row security if the
		 * original one does.
		 */
		newquery->hasRowSecurity = parse->hasRowSecurity;

		/* Replace original query in the correct element of the query list */
		lfirst(parse_cell) = newquery;
	}

	return is_tuple_result;
}

/*
 * Process one function result column for check_sql_fn_retval
 *
 * Coerce the output value to the required type/typmod, and add a column
 * to *upper_tlist for it.  Set *upper_tlist_nontrivial to true if we
 * add an upper tlist item that's not just a Var.
 *
 * Returns true if OK, false if could not coerce to required type
 * (in which case, no changes have been made)
 */
static bool
coerce_fn_result_column(TargetEntry *src_tle,
						Oid res_type,
						int32 res_typmod,
						bool tlist_is_modifiable,
						List **upper_tlist,
						bool *upper_tlist_nontrivial)
{
	TargetEntry *new_tle;
	Expr	   *new_tle_expr;
	Node	   *cast_result;

	/*
	 * If the TLE has a sortgroupref marking, don't change it, as it probably
	 * is referenced by ORDER BY, DISTINCT, etc, and changing its type would
	 * break query semantics.  Otherwise, it's safe to modify in-place unless
	 * the query as a whole has issues with that.
	 */
	if (tlist_is_modifiable && src_tle->ressortgroupref == 0)
	{
		/* OK to modify src_tle in place, if necessary */
		cast_result = coerce_to_target_type(NULL,
											(Node *) src_tle->expr,
											exprType((Node *) src_tle->expr),
											res_type, res_typmod,
											COERCION_ASSIGNMENT,
											COERCE_IMPLICIT_CAST,
											-1);
		if (cast_result == NULL)
			return false;
		assign_expr_collations(NULL, cast_result);
		src_tle->expr = (Expr *) cast_result;
		/* Make a Var referencing the possibly-modified TLE */
		new_tle_expr = (Expr *) makeVarFromTargetEntry(1, src_tle);
	}
	else
	{
		/* Any casting must happen in the upper tlist */
		Var		   *var = makeVarFromTargetEntry(1, src_tle);

		cast_result = coerce_to_target_type(NULL,
											(Node *) var,
											var->vartype,
											res_type, res_typmod,
											COERCION_ASSIGNMENT,
											COERCE_IMPLICIT_CAST,
											-1);
		if (cast_result == NULL)
			return false;
		assign_expr_collations(NULL, cast_result);
		/* Did the coercion actually do anything? */
		if (cast_result != (Node *) var)
			*upper_tlist_nontrivial = true;
		new_tle_expr = (Expr *) cast_result;
	}
	new_tle = makeTargetEntry(new_tle_expr,
							  list_length(*upper_tlist) + 1,
							  src_tle->resname, false);
	*upper_tlist = lappend(*upper_tlist, new_tle);
	return true;
}

/*
 * Extract the targetlist of the last canSetTag query in the given list
 * of parsed-and-rewritten Queries.  Returns NIL if there is none.
 */
static List *
get_sql_fn_result_tlist(List *queryTreeList)
{
	Query	   *parse = NULL;
	ListCell   *lc;

	foreach(lc, queryTreeList)
	{
		Query	   *q = lfirst_node(Query, lc);

		if (q->canSetTag)
			parse = q;
	}
	if (parse &&
		parse->commandType == CMD_SELECT)
		return parse->targetList;
	else if (parse &&
			 (parse->commandType == CMD_INSERT ||
			  parse->commandType == CMD_UPDATE ||
			  parse->commandType == CMD_DELETE ||
			  parse->commandType == CMD_MERGE) &&
			 parse->returningList)
		return parse->returningList;
	else
		return NIL;
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
static bool
sqlfunction_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_sqlfunction *myState = (DR_sqlfunction *) self;

	if (myState->tstore)
	{
		/* We are collecting all of a set result into the tuplestore */

		/* Filter tuple as needed */
		slot = ExecFilterJunk(myState->filter, slot);

		/* Store the filtered tuple into the tuplestore */
		tuplestore_puttupleslot(myState->tstore, slot);
	}
	else
	{
		/*
		 * We only want the first tuple, which we'll save in the junkfilter's
		 * result slot.  Ignore any additional tuples passed.
		 */
		if (TTS_EMPTY(myState->filter->jf_resultSlot))
		{
			/* Filter tuple as needed */
			slot = ExecFilterJunk(myState->filter, slot);
			Assert(slot == myState->filter->jf_resultSlot);

			/* Materialize the slot so it preserves pass-by-ref values */
			ExecMaterializeSlot(slot);
		}
	}

	return true;
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
