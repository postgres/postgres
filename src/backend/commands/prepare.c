/*-------------------------------------------------------------------------
 *
 * prepare.c
 *	  Prepareable SQL statements via PREPARE, EXECUTE and DEALLOCATE
 *
 * Copyright (c) 2002, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/prepare.c,v 1.13 2003/02/02 23:46:38 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/explain.h"
#include "commands/prepare.h"
#include "executor/executor.h"
#include "utils/guc.h"
#include "optimizer/planner.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"


#define HASH_KEY_LEN NAMEDATALEN

/* All the data we need to remember about a stored query */
typedef struct
{
	/* dynahash.c requires key to be first field */
	char		key[HASH_KEY_LEN];
	List	   *query_list;		/* list of queries */
	List	   *plan_list;		/* list of plans */
	List	   *argtype_list;	/* list of parameter type OIDs */
	MemoryContext context;		/* context containing this query */
} QueryHashEntry;

/*
 * The hash table in which prepared queries are stored. This is
 * per-backend: query plans are not shared between backends.
 * The keys for this hash table are the arguments to PREPARE
 * and EXECUTE ("plan names"); the entries are QueryHashEntry structs.
 */
static HTAB *prepared_queries = NULL;

static void InitQueryHashTable(void);
static void StoreQuery(const char *stmt_name, List *query_list,
					   List *plan_list, List *argtype_list);
static QueryHashEntry *FetchQuery(const char *plan_name);
static ParamListInfo EvaluateParams(EState *estate,
									List *params, List *argtypes);

/*
 * Implements the 'PREPARE' utility statement.
 */
void
PrepareQuery(PrepareStmt *stmt)
{
	List	   *plan_list = NIL;
	List	   *query_list,
			   *query_list_item;

	if (!stmt->name)
		elog(ERROR, "No statement name given");

	if (stmt->query->commandType == CMD_UTILITY)
		elog(ERROR, "Utility statements cannot be prepared");

	/* Rewrite the query. The result could be 0, 1, or many queries. */
	query_list = QueryRewrite(stmt->query);

	foreach(query_list_item, query_list)
	{
		Query	   *query = (Query *) lfirst(query_list_item);
		Plan	   *plan;

		plan = pg_plan_query(query);

		plan_list = lappend(plan_list, plan);
	}

	StoreQuery(stmt->name, query_list, plan_list, stmt->argtype_oids);
}

/*
 * Implements the 'EXECUTE' utility statement.
 */
void
ExecuteQuery(ExecuteStmt *stmt, CommandDest outputDest)
{
	QueryHashEntry *entry;
	List	   *l,
			   *query_list,
			   *plan_list;
	ParamListInfo paramLI = NULL;
	EState	   *estate = NULL;

	/* Look it up in the hash table */
	entry = FetchQuery(stmt->name);

	query_list = entry->query_list;
	plan_list = entry->plan_list;

	Assert(length(query_list) == length(plan_list));

	/* Evaluate parameters, if any */
	if (entry->argtype_list != NIL)
	{
		/*
		 * Need an EState to evaluate parameters; must not delete it
		 * till end of query, in case parameters are pass-by-reference.
		 */
		estate = CreateExecutorState();
		paramLI = EvaluateParams(estate, stmt->params, entry->argtype_list);
	}

	/* Execute each query */
	foreach(l, query_list)
	{
		Query	  *query = (Query *) lfirst(l);
		Plan	  *plan = (Plan *) lfirst(plan_list);
		bool		is_last_query;

		plan_list = lnext(plan_list);
		is_last_query = (plan_list == NIL);

		if (query->commandType == CMD_UTILITY)
			ProcessUtility(query->utilityStmt, outputDest, NULL);
		else
		{
			QueryDesc  *qdesc;

			if (log_executor_stats)
				ResetUsage();

			qdesc = CreateQueryDesc(query, plan, outputDest, NULL,
									paramLI, false);

			if (stmt->into)
			{
				if (qdesc->operation != CMD_SELECT)
					elog(ERROR, "INTO clause specified for non-SELECT query");

				query->into = stmt->into;
				qdesc->dest = None;
			}

			ExecutorStart(qdesc);

			ExecutorRun(qdesc, ForwardScanDirection, 0L);

			ExecutorEnd(qdesc);

			FreeQueryDesc(qdesc);

			if (log_executor_stats)
				ShowUsage("EXECUTOR STATISTICS");
		}

		/*
		 * If we're processing multiple queries, we need to increment the
		 * command counter between them. For the last query, there's no
		 * need to do this, it's done automatically.
		 */
		if (!is_last_query)
			CommandCounterIncrement();
	}

	if (estate)
		FreeExecutorState(estate);

	/* No need to pfree other memory, MemoryContext will be reset */
}

/*
 * Evaluates a list of parameters, using the given executor state. It
 * requires a list of the parameter values themselves, and a list of
 * their types. It returns a filled-in ParamListInfo -- this can later
 * be passed to CreateQueryDesc(), which allows the executor to make use
 * of the parameters during query execution.
 */
static ParamListInfo
EvaluateParams(EState *estate, List *params, List *argtypes)
{
	int				nargs = length(argtypes);
	ParamListInfo	paramLI;
	List		   *exprstates;
	List		   *l;
	int				i = 0;

	/* Parser should have caught this error, but check anyway */
	if (length(params) != nargs)
		elog(ERROR, "EvaluateParams: wrong number of arguments");

	exprstates = (List *) ExecPrepareExpr((Expr *) params, estate);

	paramLI = (ParamListInfo)
		palloc0((nargs + 1) * sizeof(ParamListInfoData));

	foreach(l, exprstates)
	{
		ExprState  *n = lfirst(l);
		bool		isNull;

		paramLI[i].value = ExecEvalExprSwitchContext(n,
													 GetPerTupleExprContext(estate),
													 &isNull,
													 NULL);
		paramLI[i].kind = PARAM_NUM;
		paramLI[i].id = i + 1;
		paramLI[i].isnull = isNull;

		i++;
	}

	paramLI[i].kind = PARAM_INVALID;

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

	hash_ctl.keysize = HASH_KEY_LEN;
	hash_ctl.entrysize = sizeof(QueryHashEntry);

	prepared_queries = hash_create("Prepared Queries",
								   32,
								   &hash_ctl,
								   HASH_ELEM);

	if (!prepared_queries)
		elog(ERROR, "InitQueryHashTable: unable to create hash table");
}

/*
 * Store all the data pertaining to a query in the hash table using
 * the specified key. A copy of the data is made in a memory context belonging
 * to the hash entry, so the caller can dispose of their copy.
 */
static void
StoreQuery(const char *stmt_name, List *query_list,
		   List *plan_list, List *argtype_list)
{
	QueryHashEntry *entry;
	MemoryContext oldcxt,
				entrycxt;
	char		key[HASH_KEY_LEN];
	bool		found;

	/* Initialize the hash table, if necessary */
	if (!prepared_queries)
		InitQueryHashTable();

	/* Check for pre-existing entry of same name */
	/* See notes in FetchQuery */
	MemSet(key, 0, sizeof(key));
	strncpy(key, stmt_name, sizeof(key));

	hash_search(prepared_queries, key, HASH_FIND, &found);

	if (found)
		elog(ERROR, "Prepared statement with name \"%s\" already exists",
			 stmt_name);

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
	 * out-of-memory failure only wastes memory and doesn't leave us with
	 * an incomplete (ie corrupt) hashtable entry.
	 */
	query_list = (List *) copyObject(query_list);
	plan_list = (List *) copyObject(plan_list);
	argtype_list = listCopy(argtype_list);

	/* Now we can add entry to hash table */
	entry = (QueryHashEntry *) hash_search(prepared_queries,
										   key,
										   HASH_ENTER,
										   &found);

	/* Shouldn't get a failure, nor a duplicate entry */
	if (!entry || found)
		elog(ERROR, "Unable to store prepared statement \"%s\"!",
			 stmt_name);

	/* Fill in the hash table entry with copied data */
	entry->query_list = query_list;
	entry->plan_list = plan_list;
	entry->argtype_list = argtype_list;
	entry->context = entrycxt;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Lookup an existing query in the hash table. If the query does not
 * actually exist, an elog(ERROR) is thrown.
 */
static QueryHashEntry *
FetchQuery(const char *plan_name)
{
	char		key[HASH_KEY_LEN];
	QueryHashEntry *entry;

	/*
	 * If the hash table hasn't been initialized, it can't be storing
	 * anything, therefore it couldn't possibly store our plan.
	 */
	if (!prepared_queries)
		elog(ERROR, "Prepared statement with name \"%s\" does not exist",
			 plan_name);

	/*
	 * We can't just use the statement name as supplied by the user: the
	 * hash package is picky enough that it needs to be NULL-padded out to
	 * the appropriate length to work correctly.
	 */
	MemSet(key, 0, sizeof(key));
	strncpy(key, plan_name, sizeof(key));

	entry = (QueryHashEntry *) hash_search(prepared_queries,
										   key,
										   HASH_FIND,
										   NULL);

	if (!entry)
		elog(ERROR, "Prepared statement with name \"%s\" does not exist",
			 plan_name);

	return entry;
}

/*
 * Given a plan name, look up the stored plan (giving error if not found).
 * If found, return the list of argument type OIDs.
 */
List *
FetchQueryParams(const char *plan_name)
{
	QueryHashEntry *entry;

	entry = FetchQuery(plan_name);

	return entry->argtype_list;
}

/*
 * Implements the 'DEALLOCATE' utility statement: deletes the
 * specified plan from storage.
 */
void
DeallocateQuery(DeallocateStmt *stmt)
{
	QueryHashEntry *entry;

	/* Find the query's hash table entry */
	entry = FetchQuery(stmt->name);

	/* Flush the context holding the subsidiary data */
	Assert(MemoryContextIsValid(entry->context));
	MemoryContextDelete(entry->context);

	/* Now we can remove the hash table entry */
	hash_search(prepared_queries, entry->key, HASH_REMOVE, NULL);
}

/*
 * Implements the 'EXPLAIN EXECUTE' utility statement.
 */
void
ExplainExecuteQuery(ExplainStmt *stmt, TupOutputState *tstate)
{
	ExecuteStmt	   *execstmt = (ExecuteStmt *) stmt->query->utilityStmt;
	QueryHashEntry *entry;
	List	   *l,
			   *query_list,
			   *plan_list;
	ParamListInfo paramLI = NULL;
	EState	   *estate = NULL;

	/* explain.c should only call me for EXECUTE stmt */
	Assert(execstmt && IsA(execstmt, ExecuteStmt));

	/* Look it up in the hash table */
	entry = FetchQuery(execstmt->name);

	query_list = entry->query_list;
	plan_list = entry->plan_list;

	Assert(length(query_list) == length(plan_list));

	/* Evaluate parameters, if any */
	if (entry->argtype_list != NIL)
	{
		/*
		 * Need an EState to evaluate parameters; must not delete it
		 * till end of query, in case parameters are pass-by-reference.
		 */
		estate = CreateExecutorState();
		paramLI = EvaluateParams(estate, execstmt->params,
								 entry->argtype_list);
	}

	/* Explain each query */
	foreach(l, query_list)
	{
		Query	  *query = (Query *) lfirst(l);
		Plan	  *plan = (Plan *) lfirst(plan_list);
		bool		is_last_query;

		plan_list = lnext(plan_list);
		is_last_query = (plan_list == NIL);

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

			/* Create a QueryDesc requesting no output */
			qdesc = CreateQueryDesc(query, plan, None, NULL,
									paramLI, stmt->analyze);

			if (execstmt->into)
			{
				if (qdesc->operation != CMD_SELECT)
					elog(ERROR, "INTO clause specified for non-SELECT query");

				query->into = execstmt->into;
				qdesc->dest = None;
			}

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
