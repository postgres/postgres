/*-------------------------------------------------------------------------
 *
 * functions.c
 *	  Execution of SQL-language functions
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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
#include "utils/lsyscache.h"
#include "utils/memutils.h"
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
	PlannedStmt *stmt;			/* plan for this query */
	QueryDesc  *qd;				/* null unless status == RUN */
} execution_state;


/*
 * An SQLFunctionCache record is built during the first call,
 * and linked to from the fn_extra field of the FmgrInfo struct.
 *
 * Note that currently this has only the lifespan of the calling query.
 * Someday we should rewrite this code to use plancache.c to save parse/plan
 * results for longer than that.
 *
 * Physically, though, the data has the lifespan of the FmgrInfo that's used
 * to call the function, and there are cases (particularly with indexes)
 * where the FmgrInfo might survive across transactions.  We cannot assume
 * that the parse/plan trees are good for longer than the (sub)transaction in
 * which parsing was done, so we must mark the record with the LXID/subxid of
 * its creation time, and regenerate everything if that's obsolete.  To avoid
 * memory leakage when we do have to regenerate things, all the data is kept
 * in a sub-context of the FmgrInfo's fn_mcxt.
 */
typedef struct
{
	char	   *fname;			/* function name (for error msgs) */
	char	   *src;			/* function body text (for error msgs) */

	SQLFunctionParseInfoPtr pinfo;	/* data for parser callback hooks */

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

	MemoryContext fcontext;		/* memory context holding this struct and all
								 * subsidiary data */

	LocalTransactionId lxid;	/* lxid in which cache was made */
	SubTransactionId subxid;	/* subxid in which cache was made */
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
static List *init_execution_state(List *queryTree_list,
								  SQLFunctionCachePtr fcache,
								  bool lazyEvalOK);
static void init_sql_fcache(FunctionCallInfo fcinfo, Oid collation, bool lazyEvalOK);
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
static bool coerce_fn_result_column(TargetEntry *src_tle,
									Oid res_type, int32 res_typmod,
									bool tlist_is_modifiable,
									List **upper_tlist,
									bool *upper_tlist_nontrivial);
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
	pstate->p_ref_hook_state = (void *) pinfo;
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
	Assert(IsA(field1, String));
	name1 = strVal(field1);
	if (nnames > 1)
	{
		subfield = (Node *) lsecond(cref->fields);
		Assert(IsA(subfield, String));
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
		List	   *qtlist = lfirst_node(List, lc1);
		execution_state *firstes = NULL;
		execution_state *preves = NULL;
		ListCell   *lc2;

		foreach(lc2, qtlist)
		{
			Query	   *queryTree = lfirst_node(Query, lc2);
			PlannedStmt *stmt;
			execution_state *newes;

			/* Plan the query if needed */
			if (queryTree->commandType == CMD_UTILITY)
			{
				/* Utility commands require no planning. */
				stmt = makeNode(PlannedStmt);
				stmt->commandType = CMD_UTILITY;
				stmt->canSetTag = queryTree->canSetTag;
				stmt->utilityStmt = queryTree->utilityStmt;
				stmt->stmt_location = queryTree->stmt_location;
				stmt->stmt_len = queryTree->stmt_len;
			}
			else
				stmt = pg_plan_query(queryTree,
									 fcache->src,
									 CURSOR_OPT_PARALLEL_OK,
									 NULL);

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

			if (fcache->readonly_func && !CommandIsReadOnly(stmt))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				/* translator: %s is a SQL statement name */
						 errmsg("%s is not allowed in a non-volatile function",
								CreateCommandName((Node *) stmt))));

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
	 * output from the last statement in such a function.
	 */
	if (lasttages && fcache->junkFilter)
	{
		lasttages->setsResult = true;
		if (lazyEvalOK &&
			lasttages->stmt->commandType == CMD_SELECT &&
			!lasttages->stmt->hasModifyingCTE)
			fcache->lazyEval = lasttages->lazyEval = true;
	}

	return eslist;
}

/*
 * Initialize the SQLFunctionCache for a SQL function
 */
static void
init_sql_fcache(FunctionCallInfo fcinfo, Oid collation, bool lazyEvalOK)
{
	FmgrInfo   *finfo = fcinfo->flinfo;
	Oid			foid = finfo->fn_oid;
	MemoryContext fcontext;
	MemoryContext oldcontext;
	Oid			rettype;
	TupleDesc	rettupdesc;
	HeapTuple	procedureTuple;
	Form_pg_proc procedureStruct;
	SQLFunctionCachePtr fcache;
	List	   *queryTree_list;
	List	   *resulttlist;
	ListCell   *lc;
	Datum		tmp;
	bool		isNull;

	/*
	 * Create memory context that holds all the SQLFunctionCache data.  It
	 * must be a child of whatever context holds the FmgrInfo.
	 */
	fcontext = AllocSetContextCreate(finfo->fn_mcxt,
									 "SQL function",
									 ALLOCSET_DEFAULT_SIZES);

	oldcontext = MemoryContextSwitchTo(fcontext);

	/*
	 * Create the struct proper, link it to fcontext and fn_extra.  Once this
	 * is done, we'll be able to recover the memory after failure, even if the
	 * FmgrInfo is long-lived.
	 */
	fcache = (SQLFunctionCachePtr) palloc0(sizeof(SQLFunctionCache));
	fcache->fcontext = fcontext;
	finfo->fn_extra = (void *) fcache;

	/*
	 * get the procedure tuple corresponding to the given function Oid
	 */
	procedureTuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(foid));
	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "cache lookup failed for function %u", foid);
	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	/*
	 * copy function name immediately for use by error reporting callback, and
	 * for use as memory context identifier
	 */
	fcache->fname = pstrdup(NameStr(procedureStruct->proname));
	MemoryContextSetIdentifier(fcontext, fcache->fname);

	/*
	 * Resolve any polymorphism, obtaining the actual result type, and the
	 * corresponding tupdesc if it's a rowtype.
	 */
	(void) get_call_result_type(fcinfo, &rettype, &rettupdesc);

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

	/* If we have prosqlbody, pay attention to that not prosrc. */
	tmp = SysCacheGetAttr(PROCOID,
						  procedureTuple,
						  Anum_pg_proc_prosqlbody,
						  &isNull);

	/*
	 * Parse and rewrite the queries in the function text.  Use sublists to
	 * keep track of the original query boundaries.
	 *
	 * Note: since parsing and planning is done in fcontext, we will generate
	 * a lot of cruft that lives as long as the fcache does.  This is annoying
	 * but we'll not worry about it until the module is rewritten to use
	 * plancache.c.
	 */
	queryTree_list = NIL;
	if (!isNull)
	{
		Node	   *n;
		List	   *stored_query_list;

		n = stringToNode(TextDatumGetCString(tmp));
		if (IsA(n, List))
			stored_query_list = linitial_node(List, castNode(List, n));
		else
			stored_query_list = list_make1(n);

		foreach(lc, stored_query_list)
		{
			Query	   *parsetree = lfirst_node(Query, lc);
			List	   *queryTree_sublist;

			AcquireRewriteLocks(parsetree, true, false);
			queryTree_sublist = pg_rewrite_query(parsetree);
			queryTree_list = lappend(queryTree_list, queryTree_sublist);
		}
	}
	else
	{
		List	   *raw_parsetree_list;

		raw_parsetree_list = pg_parse_query(fcache->src);

		foreach(lc, raw_parsetree_list)
		{
			RawStmt    *parsetree = lfirst_node(RawStmt, lc);
			List	   *queryTree_sublist;

			queryTree_sublist = pg_analyze_and_rewrite_withcb(parsetree,
															  fcache->src,
															  (ParserSetupHook) sql_fn_parser_setup,
															  fcache->pinfo,
															  NULL);
			queryTree_list = lappend(queryTree_list, queryTree_sublist);
		}
	}

	/*
	 * Check that there are no statements we don't want to allow.
	 */
	check_sql_fn_statements(queryTree_list);

	/*
	 * Check that the function returns the type it claims to.  Although in
	 * simple cases this was already done when the function was defined, we
	 * have to recheck because database objects used in the function's queries
	 * might have changed type.  We'd have to recheck anyway if the function
	 * had any polymorphic arguments.  Moreover, check_sql_fn_retval takes
	 * care of injecting any required column type coercions.  (But we don't
	 * ask it to insert nulls for dropped columns; the junkfilter handles
	 * that.)
	 *
	 * Note: we set fcache->returnsTuple according to whether we are returning
	 * the whole tuple result or just a single column.  In the latter case we
	 * clear returnsTuple because we need not act different from the scalar
	 * result case, even if it's a rowtype column.  (However, we have to force
	 * lazy eval mode in that case; otherwise we'd need extra code to expand
	 * the rowtype column into multiple columns, since we have no way to
	 * notify the caller that it should do that.)
	 */
	fcache->returnsTuple = check_sql_fn_retval(queryTree_list,
											   rettype,
											   rettupdesc,
											   false,
											   &resulttlist);

	/*
	 * Construct a JunkFilter we can use to coerce the returned rowtype to the
	 * desired form, unless the result type is VOID, in which case there's
	 * nothing to coerce to.  (XXX Frequently, the JunkFilter isn't doing
	 * anything very interesting, but much of this module expects it to be
	 * there anyway.)
	 */
	if (rettype != VOIDOID)
	{
		TupleTableSlot *slot = MakeSingleTupleTableSlot(NULL,
														&TTSOpsMinimalTuple);

		/*
		 * If the result is composite, *and* we are returning the whole tuple
		 * result, we need to insert nulls for any dropped columns.  In the
		 * single-column-result case, there might be dropped columns within
		 * the composite column value, but it's not our problem here.  There
		 * should be no resjunk entries in resulttlist, so in the second case
		 * the JunkFilter is certainly a no-op.
		 */
		if (rettupdesc && fcache->returnsTuple)
			fcache->junkFilter = ExecInitJunkFilterConversion(resulttlist,
															  rettupdesc,
															  slot);
		else
			fcache->junkFilter = ExecInitJunkFilter(resulttlist, slot);
	}

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

	/* Mark fcache with time of creation to show it's valid */
	fcache->lxid = MyProc->lxid;
	fcache->subxid = GetCurrentSubTransactionId();

	ReleaseSysCache(procedureTuple);

	MemoryContextSwitchTo(oldcontext);
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

	es->qd = CreateQueryDesc(es->stmt,
							 fcache->src,
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
}

/* Run one execution_state; either to completion or to first result row */
/* Returns true if we ran to completion */
static bool
postquel_getnext(execution_state *es, SQLFunctionCachePtr fcache)
{
	bool		result;

	if (es->qd->operation == CMD_UTILITY)
	{
		ProcessUtility(es->qd->plannedstmt,
					   fcache->src,
					   false,
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

		ExecutorRun(es->qd, ForwardScanDirection, count, !fcache->returnsSet || !es->lazyEval);

		/*
		 * If we requested run to completion OR there was no tuple returned,
		 * command must be complete.
		 */
		result = (count == 0 || es->qd->estate->es_processed == 0);
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
	if (es->qd->operation != CMD_UTILITY)
	{
		ExecutorFinish(es->qd);
		ExecutorEnd(es->qd);
	}

	es->qd->dest->rDestroy(es->qd->dest);

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
		Oid		   *argtypes = fcache->pinfo->argtypes;

		if (fcache->paramLI == NULL)
		{
			paramLI = makeParamList(nargs);
			fcache->paramLI = paramLI;
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
													get_typlen(argtypes[i]));
			prm->pflags = 0;
			prm->ptype = argtypes[i];
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
		value = ExecFetchSlotHeapTupleDatum(slot);
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
	SQLFunctionCachePtr fcache;
	ErrorContextCallback sqlerrcontext;
	MemoryContext oldcontext;
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
	 * Initialize fcache (build plans) if first time through; or re-initialize
	 * if the cache is stale.
	 */
	fcache = (SQLFunctionCachePtr) fcinfo->flinfo->fn_extra;

	if (fcache != NULL)
	{
		if (fcache->lxid != MyProc->lxid ||
			!SubTransactionIsActive(fcache->subxid))
		{
			/* It's stale; unlink and delete */
			fcinfo->flinfo->fn_extra = NULL;
			MemoryContextDelete(fcache->fcontext);
			fcache = NULL;
		}
	}

	if (fcache == NULL)
	{
		init_sql_fcache(fcinfo, PG_GET_COLLATION(), lazyEvalOK);
		fcache = (SQLFunctionCachePtr) fcinfo->flinfo->fn_extra;
	}

	/*
	 * Switch to context in which the fcache lives.  This ensures that our
	 * tuplestore etc will have sufficient lifetime.  The sub-executor is
	 * responsible for deleting per-tuple information.  (XXX in the case of a
	 * long-lived FmgrInfo, this policy represents more memory leakage, but
	 * it's not entirely clear where to keep stuff instead.)
	 */
	oldcontext = MemoryContextSwitchTo(fcache->fcontext);

	/*
	 * Find first unfinished query in function, and note whether it's the
	 * first query.
	 */
	eslist = fcache->func_state;
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
			eslc = lnext(eslist, eslc);
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
			/* Should only get here for VOID functions and procedures */
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
		ListCell   *lc2;

		foreach(lc2, sublist)
		{
			Query	   *query = lfirst_node(Query, lc2);

			/*
			 * Disallow calling procedures with output arguments.  The current
			 * implementation would just throw the output values away, unless
			 * the statement is the last one.  Per SQL standard, we should
			 * assign the output values by name.  By disallowing this here, we
			 * preserve an opportunity for future improvement.
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
 *
 * If resultTargetList isn't NULL, then *resultTargetList is set to the
 * targetlist that defines the final statement's result.  Exception: if the
 * function is defined to return VOID then *resultTargetList is set to NIL.
 */
bool
check_sql_fn_retval(List *queryTreeLists,
					Oid rettype, TupleDesc rettupdesc,
					bool insertDroppedCols,
					List **resultTargetList)
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

	if (resultTargetList)
		*resultTargetList = NIL;	/* initialize in case of VOID result */

	/*
	 * If it's declared to return VOID, we don't care what's in the function.
	 * (This takes care of the procedure case, as well.)
	 */
	if (rettype == VOIDOID)
		return false;

	/*
	 * Find the last canSetTag query in the function body (which is presented
	 * to us as a list of sublists of Query nodes).  This isn't necessarily
	 * the last parsetree, because rule rewriting can insert queries after
	 * what the user wrote.  Note that it might not even be in the last
	 * sublist, for example if the last query rewrites to DO INSTEAD NOTHING.
	 * (It might not be unreasonable to throw an error in such a case, but
	 * this is the historical behavior and it doesn't seem worth changing.)
	 */
	parse = NULL;
	parse_cell = NULL;
	foreach(lc, queryTreeLists)
	{
		List	   *sublist = lfirst_node(List, lc);
		ListCell   *lc2;

		foreach(lc2, sublist)
		{
			Query	   *q = lfirst_node(Query, lc2);

			if (q->canSetTag)
			{
				parse = q;
				parse_cell = lc2;
			}
		}
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
		parse->commandType == CMD_SELECT)
	{
		tlist = parse->targetList;
		/* tlist is modifiable unless it's a dummy in a setop query */
		tlist_is_modifiable = (parse->setOperations == NULL);
	}
	else if (parse &&
			 (parse->commandType == CMD_INSERT ||
			  parse->commandType == CMD_UPDATE ||
			  parse->commandType == CMD_DELETE) &&
			 parse->returningList)
	{
		tlist = parse->returningList;
		/* returningList can always be modified */
		tlist_is_modifiable = true;
	}
	else
	{
		/* Empty function body, or last statement is a utility command */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("return type mismatch in function declared to return %s",
						format_type_be(rettype)),
				 errdetail("Function's final statement must be SELECT or INSERT/UPDATE/DELETE RETURNING.")));
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
		 */
		if (tlistlen == 1)
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
		{
			/* Return tlist if requested */
			if (resultTargetList)
				*resultTargetList = tlist;
			return true;
		}

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
			if (!TupleDescAttr(rettupdesc, colindex - 1)->attisdropped)
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

		/* Replace original query in the correct element of the query list */
		lfirst(parse_cell) = newquery;
	}

	/* Return tlist (possibly modified) if requested */
	if (resultTargetList)
		*resultTargetList = upper_tlist;

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

	/* Filter tuple as needed */
	slot = ExecFilterJunk(myState->filter, slot);

	/* Store the filtered tuple into the tuplestore */
	tuplestore_puttupleslot(myState->tstore, slot);

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
