/*-------------------------------------------------------------------------
 *
 * prepare.c
 *	  Prepareable SQL statements via PREPARE, EXECUTE and DEALLOCATE
 *
 * This module also implements storage of prepared statements that are
 * accessed via the extended FE/BE query protocol.
 *
 *
 * Copyright (c) 2002-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/commands/prepare.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/createas.h"
#include "commands/prepare.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"


/*
 * The hash table in which prepared queries are stored. This is
 * per-backend: query plans are not shared between backends.
 * The keys for this hash table are the arguments to PREPARE and EXECUTE
 * (statement names); the entries are PreparedStatement structs.
 */
static HTAB *prepared_queries = NULL;

static void InitQueryHashTable(void);
static ParamListInfo EvaluateParams(PreparedStatement *pstmt, List *params,
			   const char *queryString, EState *estate);
static Datum build_regtype_array(Oid *param_types, int num_params);

/*
 * Implements the 'PREPARE' utility statement.
 */
void
PrepareQuery(PrepareStmt *stmt, const char *queryString)
{
	CachedPlanSource *plansource;
	Oid		   *argtypes = NULL;
	int			nargs;
	Query	   *query;
	List	   *query_list;
	int			i;

	/*
	 * Disallow empty-string statement name (conflicts with protocol-level
	 * unnamed statement).
	 */
	if (!stmt->name || stmt->name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PSTATEMENT_DEFINITION),
				 errmsg("invalid statement name: must not be empty")));

	/*
	 * Create the CachedPlanSource before we do parse analysis, since it needs
	 * to see the unmodified raw parse tree.
	 */
	plansource = CreateCachedPlan(stmt->query, queryString,
								  CreateCommandTag(stmt->query));

	/* Transform list of TypeNames to array of type OIDs */
	nargs = list_length(stmt->argtypes);

	if (nargs)
	{
		ParseState *pstate;
		ListCell   *l;

		/*
		 * typenameTypeId wants a ParseState to carry the source query string.
		 * Is it worth refactoring its API to avoid this?
		 */
		pstate = make_parsestate(NULL);
		pstate->p_sourcetext = queryString;

		argtypes = (Oid *) palloc(nargs * sizeof(Oid));
		i = 0;

		foreach(l, stmt->argtypes)
		{
			TypeName   *tn = lfirst(l);
			Oid			toid = typenameTypeId(pstate, tn);

			argtypes[i++] = toid;
		}
	}

	/*
	 * Analyze the statement using these parameter types (any parameters
	 * passed in from above us will not be visible to it), allowing
	 * information about unknown parameters to be deduced from context.
	 *
	 * Because parse analysis scribbles on the raw querytree, we must make a
	 * copy to ensure we don't modify the passed-in tree.  FIXME someday.
	 */
	query = parse_analyze_varparams((Node *) copyObject(stmt->query),
									queryString,
									&argtypes, &nargs);

	/*
	 * Check that all parameter types were determined.
	 */
	for (i = 0; i < nargs; i++)
	{
		Oid			argtype = argtypes[i];

		if (argtype == InvalidOid || argtype == UNKNOWNOID)
			ereport(ERROR,
					(errcode(ERRCODE_INDETERMINATE_DATATYPE),
					 errmsg("could not determine data type of parameter $%d",
							i + 1)));
	}

	/*
	 * grammar only allows OptimizableStmt, so this check should be redundant
	 */
	switch (query->commandType)
	{
		case CMD_SELECT:
		case CMD_INSERT:
		case CMD_UPDATE:
		case CMD_DELETE:
			/* OK */
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PSTATEMENT_DEFINITION),
					 errmsg("utility statements cannot be prepared")));
			break;
	}

	/* Rewrite the query. The result could be 0, 1, or many queries. */
	query_list = QueryRewrite(query);

	/* Finish filling in the CachedPlanSource */
	CompleteCachedPlan(plansource,
					   query_list,
					   NULL,
					   argtypes,
					   nargs,
					   NULL,
					   NULL,
					   CURSOR_OPT_PARALLEL_OK,	/* allow parallel mode */
					   true);	/* fixed result */

	/*
	 * Save the results.
	 */
	StorePreparedStatement(stmt->name,
						   plansource,
						   true);
}

/*
 * ExecuteQuery --- implement the 'EXECUTE' utility statement.
 *
 * This code also supports CREATE TABLE ... AS EXECUTE.  That case is
 * indicated by passing a non-null intoClause.  The DestReceiver is already
 * set up correctly for CREATE TABLE AS, but we still have to make a few
 * other adjustments here.
 *
 * Note: this is one of very few places in the code that needs to deal with
 * two query strings at once.  The passed-in queryString is that of the
 * EXECUTE, which we might need for error reporting while processing the
 * parameter expressions.  The query_string that we copy from the plan
 * source is that of the original PREPARE.
 */
void
ExecuteQuery(ExecuteStmt *stmt, IntoClause *intoClause,
			 const char *queryString, ParamListInfo params,
			 DestReceiver *dest, char *completionTag)
{
	PreparedStatement *entry;
	CachedPlan *cplan;
	List	   *plan_list;
	ParamListInfo paramLI = NULL;
	EState	   *estate = NULL;
	Portal		portal;
	char	   *query_string;
	int			eflags;
	long		count;

	/* Look it up in the hash table */
	entry = FetchPreparedStatement(stmt->name, true);

	/* Shouldn't find a non-fixed-result cached plan */
	if (!entry->plansource->fixed_result)
		elog(ERROR, "EXECUTE does not support variable-result cached plans");

	/* Evaluate parameters, if any */
	if (entry->plansource->num_params > 0)
	{
		/*
		 * Need an EState to evaluate parameters; must not delete it till end
		 * of query, in case parameters are pass-by-reference.  Note that the
		 * passed-in "params" could possibly be referenced in the parameter
		 * expressions.
		 */
		estate = CreateExecutorState();
		estate->es_param_list_info = params;
		paramLI = EvaluateParams(entry, stmt->params,
								 queryString, estate);
	}

	/* Create a new portal to run the query in */
	portal = CreateNewPortal();
	/* Don't display the portal in pg_cursors, it is for internal use only */
	portal->visible = false;

	/* Copy the plan's saved query string into the portal's memory */
	query_string = MemoryContextStrdup(PortalGetHeapMemory(portal),
									   entry->plansource->query_string);

	/* Replan if needed, and increment plan refcount for portal */
	cplan = GetCachedPlan(entry->plansource, paramLI, false);
	plan_list = cplan->stmt_list;

	/*
	 * For CREATE TABLE ... AS EXECUTE, we must verify that the prepared
	 * statement is one that produces tuples.  Currently we insist that it be
	 * a plain old SELECT.  In future we might consider supporting other
	 * things such as INSERT ... RETURNING, but there are a couple of issues
	 * to be settled first, notably how WITH NO DATA should be handled in such
	 * a case (do we really want to suppress execution?) and how to pass down
	 * the OID-determining eflags (PortalStart won't handle them in such a
	 * case, and for that matter it's not clear the executor will either).
	 *
	 * For CREATE TABLE ... AS EXECUTE, we also have to ensure that the proper
	 * eflags and fetch count are passed to PortalStart/PortalRun.
	 */
	if (intoClause)
	{
		PlannedStmt *pstmt;

		if (list_length(plan_list) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("prepared statement is not a SELECT")));
		pstmt = (PlannedStmt *) linitial(plan_list);
		if (!IsA(pstmt, PlannedStmt) ||
			pstmt->commandType != CMD_SELECT ||
			pstmt->utilityStmt != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("prepared statement is not a SELECT")));

		/* Set appropriate eflags */
		eflags = GetIntoRelEFlags(intoClause);

		/* And tell PortalRun whether to run to completion or not */
		if (intoClause->skipData)
			count = 0;
		else
			count = FETCH_ALL;
	}
	else
	{
		/* Plain old EXECUTE */
		eflags = 0;
		count = FETCH_ALL;
	}

	PortalDefineQuery(portal,
					  NULL,
					  query_string,
					  entry->plansource->commandTag,
					  plan_list,
					  cplan);

	/*
	 * Run the portal as appropriate.
	 */
	PortalStart(portal, paramLI, eflags, GetActiveSnapshot());

	(void) PortalRun(portal, count, false, dest, dest, completionTag);

	PortalDrop(portal, false);

	if (estate)
		FreeExecutorState(estate);

	/* No need to pfree other memory, MemoryContext will be reset */
}

/*
 * EvaluateParams: evaluate a list of parameters.
 *
 * pstmt: statement we are getting parameters for.
 * params: list of given parameter expressions (raw parser output!)
 * queryString: source text for error messages.
 * estate: executor state to use.
 *
 * Returns a filled-in ParamListInfo -- this can later be passed to
 * CreateQueryDesc(), which allows the executor to make use of the parameters
 * during query execution.
 */
static ParamListInfo
EvaluateParams(PreparedStatement *pstmt, List *params,
			   const char *queryString, EState *estate)
{
	Oid		   *param_types = pstmt->plansource->param_types;
	int			num_params = pstmt->plansource->num_params;
	int			nparams = list_length(params);
	ParseState *pstate;
	ParamListInfo paramLI;
	List	   *exprstates;
	ListCell   *l;
	int			i;

	if (nparams != num_params)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
		   errmsg("wrong number of parameters for prepared statement \"%s\"",
				  pstmt->stmt_name),
				 errdetail("Expected %d parameters but got %d.",
						   num_params, nparams)));

	/* Quick exit if no parameters */
	if (num_params == 0)
		return NULL;

	/*
	 * We have to run parse analysis for the expressions.  Since the parser is
	 * not cool about scribbling on its input, copy first.
	 */
	params = (List *) copyObject(params);

	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;

	i = 0;
	foreach(l, params)
	{
		Node	   *expr = lfirst(l);
		Oid			expected_type_id = param_types[i];
		Oid			given_type_id;

		expr = transformExpr(pstate, expr, EXPR_KIND_EXECUTE_PARAMETER);

		given_type_id = exprType(expr);

		expr = coerce_to_target_type(pstate, expr, given_type_id,
									 expected_type_id, -1,
									 COERCION_ASSIGNMENT,
									 COERCE_IMPLICIT_CAST,
									 -1);

		if (expr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("parameter $%d of type %s cannot be coerced to the expected type %s",
							i + 1,
							format_type_be(given_type_id),
							format_type_be(expected_type_id)),
			   errhint("You will need to rewrite or cast the expression.")));

		/* Take care of collations in the finished expression. */
		assign_expr_collations(pstate, expr);

		lfirst(l) = expr;
		i++;
	}

	/* Prepare the expressions for execution */
	exprstates = (List *) ExecPrepareExpr((Expr *) params, estate);

	paramLI = (ParamListInfo)
		palloc(offsetof(ParamListInfoData, params) +
			   num_params * sizeof(ParamExternData));
	/* we have static list of params, so no hooks needed */
	paramLI->paramFetch = NULL;
	paramLI->paramFetchArg = NULL;
	paramLI->parserSetup = NULL;
	paramLI->parserSetupArg = NULL;
	paramLI->numParams = num_params;
	paramLI->paramMask = NULL;

	i = 0;
	foreach(l, exprstates)
	{
		ExprState  *n = lfirst(l);
		ParamExternData *prm = &paramLI->params[i];

		prm->ptype = param_types[i];
		prm->pflags = PARAM_FLAG_CONST;
		prm->value = ExecEvalExprSwitchContext(n,
											   GetPerTupleExprContext(estate),
											   &prm->isnull,
											   NULL);

		i++;
	}

	return paramLI;
}


/*
 * Initialize query hash table upon first use.
 */
static void
InitQueryHashTable(void)
{
	HASHCTL		hash_ctl;

	MemSet(&hash_ctl, 0, sizeof(hash_ctl));

	hash_ctl.keysize = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(PreparedStatement);

	prepared_queries = hash_create("Prepared Queries",
								   32,
								   &hash_ctl,
								   HASH_ELEM);
}

/*
 * Store all the data pertaining to a query in the hash table using
 * the specified key.  The passed CachedPlanSource should be "unsaved"
 * in case we get an error here; we'll save it once we've created the hash
 * table entry.
 */
void
StorePreparedStatement(const char *stmt_name,
					   CachedPlanSource *plansource,
					   bool from_sql)
{
	PreparedStatement *entry;
	TimestampTz cur_ts = GetCurrentStatementStartTimestamp();
	bool		found;

	/* Initialize the hash table, if necessary */
	if (!prepared_queries)
		InitQueryHashTable();

	/* Add entry to hash table */
	entry = (PreparedStatement *) hash_search(prepared_queries,
											  stmt_name,
											  HASH_ENTER,
											  &found);

	/* Shouldn't get a duplicate entry */
	if (found)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_PSTATEMENT),
				 errmsg("prepared statement \"%s\" already exists",
						stmt_name)));

	/* Fill in the hash table entry */
	entry->plansource = plansource;
	entry->from_sql = from_sql;
	entry->prepare_time = cur_ts;

	/* Now it's safe to move the CachedPlanSource to permanent memory */
	SaveCachedPlan(plansource);
}

/*
 * Lookup an existing query in the hash table. If the query does not
 * actually exist, throw ereport(ERROR) or return NULL per second parameter.
 *
 * Note: this does not force the referenced plancache entry to be valid,
 * since not all callers care.
 */
PreparedStatement *
FetchPreparedStatement(const char *stmt_name, bool throwError)
{
	PreparedStatement *entry;

	/*
	 * If the hash table hasn't been initialized, it can't be storing
	 * anything, therefore it couldn't possibly store our plan.
	 */
	if (prepared_queries)
		entry = (PreparedStatement *) hash_search(prepared_queries,
												  stmt_name,
												  HASH_FIND,
												  NULL);
	else
		entry = NULL;

	if (!entry && throwError)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
				 errmsg("prepared statement \"%s\" does not exist",
						stmt_name)));

	return entry;
}

/*
 * Given a prepared statement, determine the result tupledesc it will
 * produce.  Returns NULL if the execution will not return tuples.
 *
 * Note: the result is created or copied into current memory context.
 */
TupleDesc
FetchPreparedStatementResultDesc(PreparedStatement *stmt)
{
	/*
	 * Since we don't allow prepared statements' result tupdescs to change,
	 * there's no need to worry about revalidating the cached plan here.
	 */
	Assert(stmt->plansource->fixed_result);
	if (stmt->plansource->resultDesc)
		return CreateTupleDescCopy(stmt->plansource->resultDesc);
	else
		return NULL;
}

/*
 * Given a prepared statement that returns tuples, extract the query
 * targetlist.  Returns NIL if the statement doesn't have a determinable
 * targetlist.
 *
 * Note: this is pretty ugly, but since it's only used in corner cases like
 * Describe Statement on an EXECUTE command, we don't worry too much about
 * efficiency.
 */
List *
FetchPreparedStatementTargetList(PreparedStatement *stmt)
{
	List	   *tlist;

	/* Get the plan's primary targetlist */
	tlist = CachedPlanGetTargetList(stmt->plansource);

	/* Copy into caller's context in case plan gets invalidated */
	return (List *) copyObject(tlist);
}

/*
 * Implements the 'DEALLOCATE' utility statement: deletes the
 * specified plan from storage.
 */
void
DeallocateQuery(DeallocateStmt *stmt)
{
	if (stmt->name)
		DropPreparedStatement(stmt->name, true);
	else
		DropAllPreparedStatements();
}

/*
 * Internal version of DEALLOCATE
 *
 * If showError is false, dropping a nonexistent statement is a no-op.
 */
void
DropPreparedStatement(const char *stmt_name, bool showError)
{
	PreparedStatement *entry;

	/* Find the query's hash table entry; raise error if wanted */
	entry = FetchPreparedStatement(stmt_name, showError);

	if (entry)
	{
		/* Release the plancache entry */
		DropCachedPlan(entry->plansource);

		/* Now we can remove the hash table entry */
		hash_search(prepared_queries, entry->stmt_name, HASH_REMOVE, NULL);
	}
}

/*
 * Drop all cached statements.
 */
void
DropAllPreparedStatements(void)
{
	HASH_SEQ_STATUS seq;
	PreparedStatement *entry;

	/* nothing cached */
	if (!prepared_queries)
		return;

	/* walk over cache */
	hash_seq_init(&seq, prepared_queries);
	while ((entry = hash_seq_search(&seq)) != NULL)
	{
		/* Release the plancache entry */
		DropCachedPlan(entry->plansource);

		/* Now we can remove the hash table entry */
		hash_search(prepared_queries, entry->stmt_name, HASH_REMOVE, NULL);
	}
}

/*
 * Implements the 'EXPLAIN EXECUTE' utility statement.
 *
 * "into" is NULL unless we are doing EXPLAIN CREATE TABLE AS EXECUTE,
 * in which case executing the query should result in creating that table.
 *
 * Note: the passed-in queryString is that of the EXPLAIN EXECUTE,
 * not the original PREPARE; we get the latter string from the plancache.
 */
void
ExplainExecuteQuery(ExecuteStmt *execstmt, IntoClause *into, ExplainState *es,
					const char *queryString, ParamListInfo params)
{
	PreparedStatement *entry;
	const char *query_string;
	CachedPlan *cplan;
	List	   *plan_list;
	ListCell   *p;
	ParamListInfo paramLI = NULL;
	EState	   *estate = NULL;

	/* Look it up in the hash table */
	entry = FetchPreparedStatement(execstmt->name, true);

	/* Shouldn't find a non-fixed-result cached plan */
	if (!entry->plansource->fixed_result)
		elog(ERROR, "EXPLAIN EXECUTE does not support variable-result cached plans");

	query_string = entry->plansource->query_string;

	/* Evaluate parameters, if any */
	if (entry->plansource->num_params)
	{
		/*
		 * Need an EState to evaluate parameters; must not delete it till end
		 * of query, in case parameters are pass-by-reference.  Note that the
		 * passed-in "params" could possibly be referenced in the parameter
		 * expressions.
		 */
		estate = CreateExecutorState();
		estate->es_param_list_info = params;
		paramLI = EvaluateParams(entry, execstmt->params,
								 queryString, estate);
	}

	/* Replan if needed, and acquire a transient refcount */
	cplan = GetCachedPlan(entry->plansource, paramLI, true);

	plan_list = cplan->stmt_list;

	/* Explain each query */
	foreach(p, plan_list)
	{
		PlannedStmt *pstmt = (PlannedStmt *) lfirst(p);

		if (IsA(pstmt, PlannedStmt))
			ExplainOnePlan(pstmt, into, es, query_string, paramLI, NULL);
		else
			ExplainOneUtility((Node *) pstmt, into, es, query_string, paramLI);

		/* No need for CommandCounterIncrement, as ExplainOnePlan did it */

		/* Separate plans with an appropriate separator */
		if (lnext(p) != NULL)
			ExplainSeparatePlans(es);
	}

	if (estate)
		FreeExecutorState(estate);

	ReleaseCachedPlan(cplan, true);
}

/*
 * This set returning function reads all the prepared statements and
 * returns a set of (name, statement, prepare_time, param_types, from_sql).
 */
Datum
pg_prepared_statement(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* need to build tuplestore in query context */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/*
	 * build tupdesc for result tuples. This must match the definition of the
	 * pg_prepared_statements view in system_views.sql
	 */
	tupdesc = CreateTemplateTupleDesc(5, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "statement",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "prepare_time",
					   TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "parameter_types",
					   REGTYPEARRAYOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "from_sql",
					   BOOLOID, -1, 0);

	/*
	 * We put all the tuples into a tuplestore in one scan of the hashtable.
	 * This avoids any issue of the hashtable possibly changing between calls.
	 */
	tupstore =
		tuplestore_begin_heap(rsinfo->allowedModes & SFRM_Materialize_Random,
							  false, work_mem);

	/* generate junk in short-term context */
	MemoryContextSwitchTo(oldcontext);

	/* hash table might be uninitialized */
	if (prepared_queries)
	{
		HASH_SEQ_STATUS hash_seq;
		PreparedStatement *prep_stmt;

		hash_seq_init(&hash_seq, prepared_queries);
		while ((prep_stmt = hash_seq_search(&hash_seq)) != NULL)
		{
			Datum		values[5];
			bool		nulls[5];

			MemSet(nulls, 0, sizeof(nulls));

			values[0] = CStringGetTextDatum(prep_stmt->stmt_name);
			values[1] = CStringGetTextDatum(prep_stmt->plansource->query_string);
			values[2] = TimestampTzGetDatum(prep_stmt->prepare_time);
			values[3] = build_regtype_array(prep_stmt->plansource->param_types,
										  prep_stmt->plansource->num_params);
			values[4] = BoolGetDatum(prep_stmt->from_sql);

			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		}
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	return (Datum) 0;
}

/*
 * This utility function takes a C array of Oids, and returns a Datum
 * pointing to a one-dimensional Postgres array of regtypes. An empty
 * array is returned as a zero-element array, not NULL.
 */
static Datum
build_regtype_array(Oid *param_types, int num_params)
{
	Datum	   *tmp_ary;
	ArrayType  *result;
	int			i;

	tmp_ary = (Datum *) palloc(num_params * sizeof(Datum));

	for (i = 0; i < num_params; i++)
		tmp_ary[i] = ObjectIdGetDatum(param_types[i]);

	/* XXX: this hardcodes assumptions about the regtype type */
	result = construct_array(tmp_ary, num_params, REGTYPEOID, 4, true, 'i');
	return PointerGetDatum(result);
}
