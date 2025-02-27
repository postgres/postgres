/*-------------------------------------------------------------------------
 *
 * prepare.c
 *	  Prepareable SQL statements via PREPARE, EXECUTE and DEALLOCATE
 *
 * This module also implements storage of prepared statements that are
 * accessed via the extended FE/BE query protocol.
 *
 *
 * Copyright (c) 2002-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/commands/prepare.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/createas.h"
#include "commands/explain_format.h"
#include "commands/prepare.h"
#include "funcapi.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_type.h"
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
static ParamListInfo EvaluateParams(ParseState *pstate,
									PreparedStatement *pstmt, List *params,
									EState *estate);
static Datum build_regtype_array(Oid *param_types, int num_params);

/*
 * Implements the 'PREPARE' utility statement.
 */
void
PrepareQuery(ParseState *pstate, PrepareStmt *stmt,
			 int stmt_location, int stmt_len)
{
	RawStmt    *rawstmt;
	CachedPlanSource *plansource;
	Oid		   *argtypes = NULL;
	int			nargs;
	List	   *query_list;

	/*
	 * Disallow empty-string statement name (conflicts with protocol-level
	 * unnamed statement).
	 */
	if (!stmt->name || stmt->name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PSTATEMENT_DEFINITION),
				 errmsg("invalid statement name: must not be empty")));

	/*
	 * Need to wrap the contained statement in a RawStmt node to pass it to
	 * parse analysis.
	 */
	rawstmt = makeNode(RawStmt);
	rawstmt->stmt = stmt->query;
	rawstmt->stmt_location = stmt_location;
	rawstmt->stmt_len = stmt_len;

	/*
	 * Create the CachedPlanSource before we do parse analysis, since it needs
	 * to see the unmodified raw parse tree.
	 */
	plansource = CreateCachedPlan(rawstmt, pstate->p_sourcetext,
								  CreateCommandTag(stmt->query));

	/* Transform list of TypeNames to array of type OIDs */
	nargs = list_length(stmt->argtypes);

	if (nargs)
	{
		int			i;
		ListCell   *l;

		argtypes = palloc_array(Oid, nargs);
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
	 * Rewrite the query. The result could be 0, 1, or many queries.
	 */
	query_list = pg_analyze_and_rewrite_varparams(rawstmt, pstate->p_sourcetext,
												  &argtypes, &nargs, NULL);

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
 */
void
ExecuteQuery(ParseState *pstate,
			 ExecuteStmt *stmt, IntoClause *intoClause,
			 ParamListInfo params,
			 DestReceiver *dest, QueryCompletion *qc)
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
		paramLI = EvaluateParams(pstate, entry, stmt->params, estate);
	}

	/* Create a new portal to run the query in */
	portal = CreateNewPortal();
	/* Don't display the portal in pg_cursors, it is for internal use only */
	portal->visible = false;

	/* Copy the plan's saved query string into the portal's memory */
	query_string = MemoryContextStrdup(portal->portalContext,
									   entry->plansource->query_string);

	/* Replan if needed, and increment plan refcount for portal */
	cplan = GetCachedPlan(entry->plansource, paramLI, NULL, NULL);
	plan_list = cplan->stmt_list;

	/*
	 * DO NOT add any logic that could possibly throw an error between
	 * GetCachedPlan and PortalDefineQuery, or you'll leak the plan refcount.
	 */
	PortalDefineQuery(portal,
					  NULL,
					  query_string,
					  entry->plansource->commandTag,
					  plan_list,
					  cplan,
					  entry->plansource);

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
		pstmt = linitial_node(PlannedStmt, plan_list);
		if (pstmt->commandType != CMD_SELECT)
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

	/*
	 * Run the portal as appropriate.
	 */
	PortalStart(portal, paramLI, eflags, GetActiveSnapshot());

	(void) PortalRun(portal, count, false, dest, dest, qc);

	PortalDrop(portal, false);

	if (estate)
		FreeExecutorState(estate);

	/* No need to pfree other memory, MemoryContext will be reset */
}

/*
 * EvaluateParams: evaluate a list of parameters.
 *
 * pstate: parse state
 * pstmt: statement we are getting parameters for.
 * params: list of given parameter expressions (raw parser output!)
 * estate: executor state to use.
 *
 * Returns a filled-in ParamListInfo -- this can later be passed to
 * CreateQueryDesc(), which allows the executor to make use of the parameters
 * during query execution.
 */
static ParamListInfo
EvaluateParams(ParseState *pstate, PreparedStatement *pstmt, List *params,
			   EState *estate)
{
	Oid		   *param_types = pstmt->plansource->param_types;
	int			num_params = pstmt->plansource->num_params;
	int			nparams = list_length(params);
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
	params = copyObject(params);

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
					 errhint("You will need to rewrite or cast the expression."),
					 parser_errposition(pstate, exprLocation(lfirst(l)))));

		/* Take care of collations in the finished expression. */
		assign_expr_collations(pstate, expr);

		lfirst(l) = expr;
		i++;
	}

	/* Prepare the expressions for execution */
	exprstates = ExecPrepareExprList(params, estate);

	paramLI = makeParamList(num_params);

	i = 0;
	foreach(l, exprstates)
	{
		ExprState  *n = (ExprState *) lfirst(l);
		ParamExternData *prm = &paramLI->params[i];

		prm->ptype = param_types[i];
		prm->pflags = PARAM_FLAG_CONST;
		prm->value = ExecEvalExprSwitchContext(n,
											   GetPerTupleExprContext(estate),
											   &prm->isnull);

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

	hash_ctl.keysize = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(PreparedStatement);

	prepared_queries = hash_create("Prepared Queries",
								   32,
								   &hash_ctl,
								   HASH_ELEM | HASH_STRINGS);
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
	tlist = CachedPlanGetTargetList(stmt->plansource, NULL);

	/* Copy into caller's context in case plan gets invalidated */
	return copyObject(tlist);
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
 * Note: the passed-in pstate's queryString is that of the EXPLAIN EXECUTE,
 * not the original PREPARE; we get the latter string from the plancache.
 */
void
ExplainExecuteQuery(ExecuteStmt *execstmt, IntoClause *into, ExplainState *es,
					ParseState *pstate, ParamListInfo params)
{
	PreparedStatement *entry;
	const char *query_string;
	CachedPlan *cplan;
	List	   *plan_list;
	ListCell   *p;
	ParamListInfo paramLI = NULL;
	EState	   *estate = NULL;
	instr_time	planstart;
	instr_time	planduration;
	BufferUsage bufusage_start,
				bufusage;
	MemoryContextCounters mem_counters;
	MemoryContext planner_ctx = NULL;
	MemoryContext saved_ctx = NULL;
	int			query_index = 0;

	if (es->memory)
	{
		/* See ExplainOneQuery about this */
		Assert(IsA(CurrentMemoryContext, AllocSetContext));
		planner_ctx = AllocSetContextCreate(CurrentMemoryContext,
											"explain analyze planner context",
											ALLOCSET_DEFAULT_SIZES);
		saved_ctx = MemoryContextSwitchTo(planner_ctx);
	}

	if (es->buffers)
		bufusage_start = pgBufferUsage;
	INSTR_TIME_SET_CURRENT(planstart);

	/* Look it up in the hash table */
	entry = FetchPreparedStatement(execstmt->name, true);

	/* Shouldn't find a non-fixed-result cached plan */
	if (!entry->plansource->fixed_result)
		elog(ERROR, "EXPLAIN EXECUTE does not support variable-result cached plans");

	query_string = entry->plansource->query_string;

	/* Evaluate parameters, if any */
	if (entry->plansource->num_params)
	{
		ParseState *pstate_params;

		pstate_params = make_parsestate(NULL);
		pstate_params->p_sourcetext = pstate->p_sourcetext;

		/*
		 * Need an EState to evaluate parameters; must not delete it till end
		 * of query, in case parameters are pass-by-reference.  Note that the
		 * passed-in "params" could possibly be referenced in the parameter
		 * expressions.
		 */
		estate = CreateExecutorState();
		estate->es_param_list_info = params;

		paramLI = EvaluateParams(pstate_params, entry, execstmt->params, estate);
	}

	/* Replan if needed, and acquire a transient refcount */
	cplan = GetCachedPlan(entry->plansource, paramLI,
						  CurrentResourceOwner, pstate->p_queryEnv);

	INSTR_TIME_SET_CURRENT(planduration);
	INSTR_TIME_SUBTRACT(planduration, planstart);

	if (es->memory)
	{
		MemoryContextSwitchTo(saved_ctx);
		MemoryContextMemConsumed(planner_ctx, &mem_counters);
	}

	/* calc differences of buffer counters. */
	if (es->buffers)
	{
		memset(&bufusage, 0, sizeof(BufferUsage));
		BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);
	}

	plan_list = cplan->stmt_list;

	/* Explain each query */
	foreach(p, plan_list)
	{
		PlannedStmt *pstmt = lfirst_node(PlannedStmt, p);

		if (pstmt->commandType != CMD_UTILITY)
			ExplainOnePlan(pstmt, cplan, entry->plansource, query_index,
						   into, es, query_string, paramLI, pstate->p_queryEnv,
						   &planduration, (es->buffers ? &bufusage : NULL),
						   es->memory ? &mem_counters : NULL);
		else
			ExplainOneUtility(pstmt->utilityStmt, into, es, pstate, paramLI);

		/* No need for CommandCounterIncrement, as ExplainOnePlan did it */

		/* Separate plans with an appropriate separator */
		if (lnext(plan_list, p) != NULL)
			ExplainSeparatePlans(es);

		query_index++;
	}

	if (estate)
		FreeExecutorState(estate);

	ReleaseCachedPlan(cplan, CurrentResourceOwner);
}

/*
 * This set returning function reads all the prepared statements and
 * returns a set of (name, statement, prepare_time, param_types, from_sql,
 * generic_plans, custom_plans).
 */
Datum
pg_prepared_statement(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/*
	 * We put all the tuples into a tuplestore in one scan of the hashtable.
	 * This avoids any issue of the hashtable possibly changing between calls.
	 */
	InitMaterializedSRF(fcinfo, 0);

	/* hash table might be uninitialized */
	if (prepared_queries)
	{
		HASH_SEQ_STATUS hash_seq;
		PreparedStatement *prep_stmt;

		hash_seq_init(&hash_seq, prepared_queries);
		while ((prep_stmt = hash_seq_search(&hash_seq)) != NULL)
		{
			TupleDesc	result_desc;
			Datum		values[8];
			bool		nulls[8] = {0};

			result_desc = prep_stmt->plansource->resultDesc;

			values[0] = CStringGetTextDatum(prep_stmt->stmt_name);
			values[1] = CStringGetTextDatum(prep_stmt->plansource->query_string);
			values[2] = TimestampTzGetDatum(prep_stmt->prepare_time);
			values[3] = build_regtype_array(prep_stmt->plansource->param_types,
											prep_stmt->plansource->num_params);
			if (result_desc)
			{
				Oid		   *result_types;

				result_types = palloc_array(Oid, result_desc->natts);
				for (int i = 0; i < result_desc->natts; i++)
					result_types[i] = TupleDescAttr(result_desc, i)->atttypid;
				values[4] = build_regtype_array(result_types, result_desc->natts);
			}
			else
			{
				/* no result descriptor (for example, DML statement) */
				nulls[4] = true;
			}
			values[5] = BoolGetDatum(prep_stmt->from_sql);
			values[6] = Int64GetDatumFast(prep_stmt->plansource->num_generic_plans);
			values[7] = Int64GetDatumFast(prep_stmt->plansource->num_custom_plans);

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}
	}

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

	tmp_ary = palloc_array(Datum, num_params);

	for (i = 0; i < num_params; i++)
		tmp_ary[i] = ObjectIdGetDatum(param_types[i]);

	result = construct_array_builtin(tmp_ary, num_params, REGTYPEOID);
	return PointerGetDatum(result);
}
