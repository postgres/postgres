/*-------------------------------------------------------------------------
 *
 * prepare.c
 *	  Prepareable SQL statements via PREPARE, EXECUTE and DEALLOCATE
 *
 * This module also implements storage of prepared statements that are
 * accessed via the extended FE/BE query protocol.
 *
 *
 * Copyright (c) 2002-2006, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/prepare.c,v 1.66.2.1 2007/04/26 23:24:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/prepare.h"
#include "miscadmin.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/memutils.h"


/*
 * The hash table in which prepared queries are stored. This is
 * per-backend: query plans are not shared between backends.
 * The keys for this hash table are the arguments to PREPARE and EXECUTE
 * (statement names); the entries are PreparedStatement structs.
 */
static HTAB *prepared_queries = NULL;

static void InitQueryHashTable(void);
static ParamListInfo EvaluateParams(EState *estate,
			   List *params, List *argtypes);
static Datum build_regtype_array(List *oid_list);

/*
 * Implements the 'PREPARE' utility statement.
 */
void
PrepareQuery(PrepareStmt *stmt)
{
	const char *commandTag;
	Query	   *query;
	List	   *query_list,
			   *plan_list;

	/*
	 * Disallow empty-string statement name (conflicts with protocol-level
	 * unnamed statement).
	 */
	if (!stmt->name || stmt->name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PSTATEMENT_DEFINITION),
				 errmsg("invalid statement name: must not be empty")));

	switch (stmt->query->commandType)
	{
		case CMD_SELECT:
			commandTag = "SELECT";
			break;
		case CMD_INSERT:
			commandTag = "INSERT";
			break;
		case CMD_UPDATE:
			commandTag = "UPDATE";
			break;
		case CMD_DELETE:
			commandTag = "DELETE";
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PSTATEMENT_DEFINITION),
					 errmsg("utility statements cannot be prepared")));
			commandTag = NULL;	/* keep compiler quiet */
			break;
	}

	/*
	 * Parse analysis is already done, but we must still rewrite and plan the
	 * query.
	 */

	/*
	 * Because the planner is not cool about not scribbling on its input, we
	 * make a preliminary copy of the source querytree.  This prevents
	 * problems in the case that the PREPARE is in a portal or plpgsql
	 * function and is executed repeatedly.  (See also the same hack in
	 * DECLARE CURSOR and EXPLAIN.)  XXX the planner really shouldn't modify
	 * its input ... FIXME someday.
	 */
	query = copyObject(stmt->query);

	/* Rewrite the query. The result could be 0, 1, or many queries. */
	AcquireRewriteLocks(query);
	query_list = QueryRewrite(query);

	/* Generate plans for queries.	Snapshot is already set. */
	plan_list = pg_plan_queries(query_list, NULL, false);

	/*
	 * Save the results.  We don't have the query string for this PREPARE, but
	 * we do have the string we got from the client, so use that.
	 */
	StorePreparedStatement(stmt->name,
						   debug_query_string,
						   commandTag,
						   query_list,
						   plan_list,
						   stmt->argtype_oids,
						   true);
}

/*
 * Implements the 'EXECUTE' utility statement.
 */
void
ExecuteQuery(ExecuteStmt *stmt, ParamListInfo params,
			 DestReceiver *dest, char *completionTag)
{
	PreparedStatement *entry;
	char	   *query_string;
	List	   *query_list,
			   *plan_list;
	MemoryContext qcontext;
	ParamListInfo paramLI = NULL;
	EState	   *estate = NULL;
	Portal		portal;

	/* Look it up in the hash table */
	entry = FetchPreparedStatement(stmt->name, true);

	query_string = entry->query_string;
	query_list = entry->query_list;
	plan_list = entry->plan_list;
	qcontext = entry->context;

	Assert(list_length(query_list) == list_length(plan_list));

	/* Evaluate parameters, if any */
	if (entry->argtype_list != NIL)
	{
		/*
		 * Need an EState to evaluate parameters; must not delete it till end
		 * of query, in case parameters are pass-by-reference.
		 */
		estate = CreateExecutorState();
		estate->es_param_list_info = params;
		paramLI = EvaluateParams(estate, stmt->params, entry->argtype_list);
	}

	/* Create a new portal to run the query in */
	portal = CreateNewPortal();
	/* Don't display the portal in pg_cursors, it is for internal use only */
	portal->visible = false;

	/*
	 * For CREATE TABLE / AS EXECUTE, make a copy of the stored query so that
	 * we can modify its destination (yech, but this has always been ugly).
	 * For regular EXECUTE we can just use the stored query where it sits,
	 * since the executor is read-only.
	 */
	if (stmt->into)
	{
		MemoryContext oldContext;
		Query	   *query;

		oldContext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));

		if (query_string)
			query_string = pstrdup(query_string);
		query_list = copyObject(query_list);
		plan_list = copyObject(plan_list);
		qcontext = PortalGetHeapMemory(portal);

		if (list_length(query_list) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("prepared statement is not a SELECT")));
		query = (Query *) linitial(query_list);
		if (query->commandType != CMD_SELECT)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("prepared statement is not a SELECT")));
		query->into = copyObject(stmt->into);
		query->intoOptions = copyObject(stmt->intoOptions);
		query->intoOnCommit = stmt->into_on_commit;
		if (stmt->into_tbl_space)
			query->intoTableSpaceName = pstrdup(stmt->into_tbl_space);

		MemoryContextSwitchTo(oldContext);
	}

	PortalDefineQuery(portal,
					  NULL,
					  query_string,
					  entry->commandTag,
					  query_list,
					  plan_list,
					  qcontext);

	/*
	 * Run the portal to completion.
	 */
	PortalStart(portal, paramLI, ActiveSnapshot);

	(void) PortalRun(portal, FETCH_ALL, dest, dest, completionTag);

	PortalDrop(portal, false);

	if (estate)
		FreeExecutorState(estate);

	/* No need to pfree other memory, MemoryContext will be reset */
}

/*
 * Evaluates a list of parameters, using the given executor state. It
 * requires a list of the parameter expressions themselves, and a list of
 * their types. It returns a filled-in ParamListInfo -- this can later
 * be passed to CreateQueryDesc(), which allows the executor to make use
 * of the parameters during query execution.
 */
static ParamListInfo
EvaluateParams(EState *estate, List *params, List *argtypes)
{
	int			nargs = list_length(argtypes);
	ParamListInfo paramLI;
	List	   *exprstates;
	ListCell   *le,
			   *la;
	int			i = 0;

	/* Parser should have caught this error, but check for safety */
	if (list_length(params) != nargs)
		elog(ERROR, "wrong number of arguments");

	if (nargs == 0)
		return NULL;

	exprstates = (List *) ExecPrepareExpr((Expr *) params, estate);

	/* sizeof(ParamListInfoData) includes the first array element */
	paramLI = (ParamListInfo) palloc(sizeof(ParamListInfoData) +
									 (nargs - 1) *sizeof(ParamExternData));
	paramLI->numParams = nargs;

	forboth(le, exprstates, la, argtypes)
	{
		ExprState  *n = lfirst(le);
		ParamExternData *prm = &paramLI->params[i];

		prm->ptype = lfirst_oid(la);
		prm->pflags = 0;
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
 * the specified key. A copy of the data is made in a memory context belonging
 * to the hash entry, so the caller can dispose of their copy.
 *
 * Exception: commandTag is presumed to be a pointer to a constant string,
 * or possibly NULL, so it need not be copied.	Note that commandTag should
 * be NULL only if the original query (before rewriting) was empty.
 */
void
StorePreparedStatement(const char *stmt_name,
					   const char *query_string,
					   const char *commandTag,
					   List *query_list,
					   List *plan_list,
					   List *argtype_list,
					   bool from_sql)
{
	PreparedStatement *entry;
	MemoryContext oldcxt,
				entrycxt;
	char	   *qstring;
	bool		found;

	/* Initialize the hash table, if necessary */
	if (!prepared_queries)
		InitQueryHashTable();

	/* Check for pre-existing entry of same name */
	hash_search(prepared_queries, stmt_name, HASH_FIND, &found);

	if (found)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_PSTATEMENT),
				 errmsg("prepared statement \"%s\" already exists",
						stmt_name)));

	/* Make a permanent memory context for the hashtable entry */
	entrycxt = AllocSetContextCreate(TopMemoryContext,
									 stmt_name,
									 ALLOCSET_SMALL_MINSIZE,
									 ALLOCSET_SMALL_INITSIZE,
									 ALLOCSET_SMALL_MAXSIZE);

	oldcxt = MemoryContextSwitchTo(entrycxt);

	/*
	 * We need to copy the data so that it is stored in the correct memory
	 * context.  Do this before making hashtable entry, so that an
	 * out-of-memory failure only wastes memory and doesn't leave us with an
	 * incomplete (ie corrupt) hashtable entry.
	 */
	qstring = query_string ? pstrdup(query_string) : NULL;
	query_list = (List *) copyObject(query_list);
	plan_list = (List *) copyObject(plan_list);
	argtype_list = list_copy(argtype_list);

	/* Now we can add entry to hash table */
	entry = (PreparedStatement *) hash_search(prepared_queries,
											  stmt_name,
											  HASH_ENTER,
											  &found);

	/* Shouldn't get a duplicate entry */
	if (found)
		elog(ERROR, "duplicate prepared statement \"%s\"",
			 stmt_name);

	/* Fill in the hash table entry with copied data */
	entry->query_string = qstring;
	entry->commandTag = commandTag;
	entry->query_list = query_list;
	entry->plan_list = plan_list;
	entry->argtype_list = argtype_list;
	entry->context = entrycxt;
	entry->prepare_time = GetCurrentStatementStartTimestamp();
	entry->from_sql = from_sql;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Lookup an existing query in the hash table. If the query does not
 * actually exist, throw ereport(ERROR) or return NULL per second parameter.
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
 * Look up a prepared statement given the name (giving error if not found).
 * If found, return the list of argument type OIDs.
 */
List *
FetchPreparedStatementParams(const char *stmt_name)
{
	PreparedStatement *entry;

	entry = FetchPreparedStatement(stmt_name, true);

	return entry->argtype_list;
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
	Query	   *query;

	switch (ChoosePortalStrategy(stmt->query_list))
	{
		case PORTAL_ONE_SELECT:
			query = (Query *) linitial(stmt->query_list);
			return ExecCleanTypeFromTL(query->targetList, false);

		case PORTAL_ONE_RETURNING:
			query = PortalListGetPrimaryQuery(stmt->query_list);
			return ExecCleanTypeFromTL(query->returningList, false);

		case PORTAL_UTIL_SELECT:
			query = (Query *) linitial(stmt->query_list);
			return UtilityTupleDescriptor(query->utilityStmt);

		case PORTAL_MULTI_QUERY:
			/* will not return tuples */
			break;
	}
	return NULL;
}

/*
 * Given a prepared statement, determine whether it will return tuples.
 *
 * Note: this is used rather than just testing the result of
 * FetchPreparedStatementResultDesc() because that routine can fail if
 * invoked in an aborted transaction.  This one is safe to use in any
 * context.  Be sure to keep the two routines in sync!
 */
bool
PreparedStatementReturnsTuples(PreparedStatement *stmt)
{
	switch (ChoosePortalStrategy(stmt->query_list))
	{
		case PORTAL_ONE_SELECT:
		case PORTAL_ONE_RETURNING:
		case PORTAL_UTIL_SELECT:
			return true;

		case PORTAL_MULTI_QUERY:
			/* will not return tuples */
			break;
	}
	return false;
}

/*
 * Given a prepared statement that returns tuples, extract the query
 * targetlist.	Returns NIL if the statement doesn't have a determinable
 * targetlist.
 *
 * Note: do not modify the result.
 *
 * XXX be careful to keep this in sync with FetchPortalTargetList,
 * and with UtilityReturnsTuples.
 */
List *
FetchPreparedStatementTargetList(PreparedStatement *stmt)
{
	PortalStrategy strategy = ChoosePortalStrategy(stmt->query_list);

	if (strategy == PORTAL_ONE_SELECT)
		return ((Query *) linitial(stmt->query_list))->targetList;
	if (strategy == PORTAL_ONE_RETURNING)
		return (PortalListGetPrimaryQuery(stmt->query_list))->returningList;
	if (strategy == PORTAL_UTIL_SELECT)
	{
		Node	   *utilityStmt;

		utilityStmt = ((Query *) linitial(stmt->query_list))->utilityStmt;
		switch (nodeTag(utilityStmt))
		{
			case T_FetchStmt:
				{
					FetchStmt  *substmt = (FetchStmt *) utilityStmt;
					Portal		subportal;

					Assert(!substmt->ismove);
					subportal = GetPortalByName(substmt->portalname);
					Assert(PortalIsValid(subportal));
					return FetchPortalTargetList(subportal);
				}

			case T_ExecuteStmt:
				{
					ExecuteStmt *substmt = (ExecuteStmt *) utilityStmt;
					PreparedStatement *entry;

					Assert(!substmt->into);
					entry = FetchPreparedStatement(substmt->name, true);
					return FetchPreparedStatementTargetList(entry);
				}

			default:
				break;
		}
	}
	return NIL;
}

/*
 * Implements the 'DEALLOCATE' utility statement: deletes the
 * specified plan from storage.
 */
void
DeallocateQuery(DeallocateStmt *stmt)
{
	DropPreparedStatement(stmt->name, true);
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
		/* Drop any open portals that depend on this prepared statement */
		Assert(MemoryContextIsValid(entry->context));
		DropDependentPortals(entry->context);

		/* Flush the context holding the subsidiary data */
		MemoryContextDelete(entry->context);

		/* Now we can remove the hash table entry */
		hash_search(prepared_queries, entry->stmt_name, HASH_REMOVE, NULL);
	}
}

/*
 * Implements the 'EXPLAIN EXECUTE' utility statement.
 */
void
ExplainExecuteQuery(ExplainStmt *stmt, ParamListInfo params,
					TupOutputState *tstate)
{
	ExecuteStmt *execstmt = (ExecuteStmt *) stmt->query->utilityStmt;
	PreparedStatement *entry;
	ListCell   *q,
			   *p;
	List	   *query_list,
			   *plan_list;
	ParamListInfo paramLI = NULL;
	EState	   *estate = NULL;

	/* explain.c should only call me for EXECUTE stmt */
	Assert(execstmt && IsA(execstmt, ExecuteStmt));

	/* Look it up in the hash table */
	entry = FetchPreparedStatement(execstmt->name, true);

	query_list = entry->query_list;
	plan_list = entry->plan_list;

	Assert(list_length(query_list) == list_length(plan_list));

	/* Evaluate parameters, if any */
	if (entry->argtype_list != NIL)
	{
		/*
		 * Need an EState to evaluate parameters; must not delete it till end
		 * of query, in case parameters are pass-by-reference.
		 */
		estate = CreateExecutorState();
		estate->es_param_list_info = params;
		paramLI = EvaluateParams(estate, execstmt->params,
								 entry->argtype_list);
	}

	/* Explain each query */
	forboth(q, query_list, p, plan_list)
	{
		Query	   *query = (Query *) lfirst(q);
		Plan	   *plan = (Plan *) lfirst(p);
		bool		is_last_query;

		is_last_query = (lnext(p) == NULL);

		if (query->commandType == CMD_UTILITY)
		{
			if (query->utilityStmt && IsA(query->utilityStmt, NotifyStmt))
				do_text_output_oneline(tstate, "NOTIFY");
			else
				do_text_output_oneline(tstate, "UTILITY");
		}
		else
		{
			QueryDesc  *qdesc;

			if (execstmt->into)
			{
				if (query->commandType != CMD_SELECT)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("prepared statement is not a SELECT")));

				/* Copy the query so we can modify it */
				query = copyObject(query);

				query->into = execstmt->into;
			}

			/*
			 * Update snapshot command ID to ensure this query sees results of
			 * any previously executed queries.  (It's a bit cheesy to modify
			 * ActiveSnapshot without making a copy, but for the limited ways
			 * in which EXPLAIN can be invoked, I think it's OK, because the
			 * active snapshot shouldn't be shared with anything else anyway.)
			 */
			ActiveSnapshot->curcid = GetCurrentCommandId();

			/* Create a QueryDesc requesting no output */
			qdesc = CreateQueryDesc(query, plan,
									ActiveSnapshot, InvalidSnapshot,
									None_Receiver,
									paramLI, stmt->analyze);

			ExplainOnePlan(qdesc, stmt, tstate);
		}

		/* No need for CommandCounterIncrement, as ExplainOnePlan did it */

		/* put a blank line between plans */
		if (!is_last_query)
			do_text_output_oneline(tstate, "");
	}

	if (estate)
		FreeExecutorState(estate);
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
	 * build tupdesc for result tuples. This must match the definition of
	 * the pg_prepared_statements view in system_views.sql
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
	tupstore = tuplestore_begin_heap(true, false, work_mem);

	/* hash table might be uninitialized */
	if (prepared_queries)
	{
		HASH_SEQ_STATUS hash_seq;
		PreparedStatement *prep_stmt;

		hash_seq_init(&hash_seq, prepared_queries);
		while ((prep_stmt = hash_seq_search(&hash_seq)) != NULL)
		{
			HeapTuple	tuple;
			Datum		values[5];
			bool		nulls[5];

			/* generate junk in short-term context */
			MemoryContextSwitchTo(oldcontext);

			MemSet(nulls, 0, sizeof(nulls));

			values[0] = DirectFunctionCall1(textin,
									  CStringGetDatum(prep_stmt->stmt_name));

			if (prep_stmt->query_string == NULL)
				nulls[1] = true;
			else
				values[1] = DirectFunctionCall1(textin,
								   CStringGetDatum(prep_stmt->query_string));

			values[2] = TimestampTzGetDatum(prep_stmt->prepare_time);
			values[3] = build_regtype_array(prep_stmt->argtype_list);
			values[4] = BoolGetDatum(prep_stmt->from_sql);

			tuple = heap_form_tuple(tupdesc, values, nulls);

			/* switch to appropriate context while storing the tuple */
			MemoryContextSwitchTo(per_query_ctx);
			tuplestore_puttuple(tupstore, tuple);
		}
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	MemoryContextSwitchTo(oldcontext);

	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	return (Datum) 0;
}

/*
 * This utility function takes a List of Oids, and returns a Datum
 * pointing to a one-dimensional Postgres array of regtypes. The empty
 * list is returned as a zero-element array, not NULL.
 */
static Datum
build_regtype_array(List *oid_list)
{
	ListCell   *lc;
	int			len;
	int			i;
	Datum	   *tmp_ary;
	ArrayType  *result;

	len = list_length(oid_list);
	tmp_ary = (Datum *) palloc(len * sizeof(Datum));

	i = 0;
	foreach(lc, oid_list)
	{
		Oid			oid;
		Datum		oid_str;

		oid = lfirst_oid(lc);
		oid_str = DirectFunctionCall1(oidout, ObjectIdGetDatum(oid));
		tmp_ary[i++] = DirectFunctionCall1(regtypein, oid_str);
	}

	/* XXX: this hardcodes assumptions about the regtype type */
	result = construct_array(tmp_ary, len, REGTYPEOID, 4, true, 'i');
	return PointerGetDatum(result);
}
