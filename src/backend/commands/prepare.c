/*-------------------------------------------------------------------------
 *
 * prepare.c
 *	  Prepareable SQL statements via PREPARE, EXECUTE and DEALLOCATE
 *
 * Copyright (c) 2002, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/prepare.c,v 1.5 2002/11/10 07:25:13 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

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
static void RunQuery(QueryDesc *qdesc, EState *state);


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

	/* Look it up in the hash table */
	entry = FetchQuery(stmt->name);

	/* Make working copies the executor can safely scribble on */
	query_list = (List *) copyObject(entry->query_list);
	plan_list = (List *) copyObject(entry->plan_list);

	Assert(length(query_list) == length(plan_list));

	/* Evaluate parameters, if any */
	if (entry->argtype_list != NIL)
	{
		int			nargs = length(entry->argtype_list);
		int			i = 0;
		ExprContext *econtext = MakeExprContext(NULL, CurrentMemoryContext);

		/* Parser should have caught this error, but check */
		if (nargs != length(stmt->params))
			elog(ERROR, "ExecuteQuery: wrong number of arguments");

		paramLI = (ParamListInfo) palloc0((nargs + 1) * sizeof(ParamListInfoData));

		foreach(l, stmt->params)
		{
			Node	   *n = lfirst(l);
			bool		isNull;

			paramLI[i].value = ExecEvalExprSwitchContext(n,
														 econtext,
														 &isNull,
														 NULL);
			paramLI[i].kind = PARAM_NUM;
			paramLI[i].id = i + 1;
			paramLI[i].isnull = isNull;

			i++;
		}
		paramLI[i].kind = PARAM_INVALID;
	}

	/* Execute each query */
	foreach(l, query_list)
	{
		Query	   *query = lfirst(l);
		Plan	   *plan = lfirst(plan_list);
		bool		is_last_query;

		plan_list = lnext(plan_list);
		is_last_query = (plan_list == NIL);

		if (query->commandType == CMD_UTILITY)
			ProcessUtility(query->utilityStmt, outputDest, NULL);
		else
		{
			QueryDesc  *qdesc;
			EState	   *state;

			if (Show_executor_stats)
				ResetUsage();

			qdesc = CreateQueryDesc(query, plan, outputDest, NULL);
			state = CreateExecutorState();

			state->es_param_list_info = paramLI;

			if (stmt->into)
			{
				if (qdesc->operation != CMD_SELECT)
					elog(ERROR, "INTO clause specified for non-SELECT query");

				query->into = stmt->into;
				qdesc->dest = None;
			}

			RunQuery(qdesc, state);

			if (Show_executor_stats)
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

	/* No need to pfree memory, MemoryContext will be reset */
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
StoreQuery(const char *stmt_name, List *query_list, List *plan_list,
		   List *argtype_list)
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
									 1024,
									 1024,
									 ALLOCSET_DEFAULT_MAXSIZE);

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

	/* Shouldn't get a failure, nor duplicate entry */
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
 * Lookup an existing query in the hash table.
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
 * Actually execute a prepared query.
 */
static void
RunQuery(QueryDesc *qdesc, EState *state)
{
	TupleDesc	tupdesc;

	tupdesc = ExecutorStart(qdesc, state);

	ExecutorRun(qdesc, state, state->es_direction, 0L);

	ExecutorEnd(qdesc, state);
}

/*
 * Implements the 'DEALLOCATE' utility statement: deletes the
 * specified plan from storage.
 *
 * The initial part of this routine is identical to FetchQuery(),
 * but we repeat the coding because we need to use the key twice.
 */
void
DeallocateQuery(DeallocateStmt *stmt)
{
	char		key[HASH_KEY_LEN];
	QueryHashEntry *entry;

	/*
	 * If the hash table hasn't been initialized, it can't be storing
	 * anything, therefore it couldn't possibly store our plan.
	 */
	if (!prepared_queries)
		elog(ERROR, "Prepared statement with name \"%s\" does not exist",
			 stmt->name);

	/*
	 * We can't just use the statement name as supplied by the user: the
	 * hash package is picky enough that it needs to be NULL-padded out to
	 * the appropriate length to work correctly.
	 */
	MemSet(key, 0, sizeof(key));
	strncpy(key, stmt->name, sizeof(key));

	/*
	 * First lookup the entry, so we can release all the subsidiary memory
	 * it has allocated (when it's removed, hash_search() will return a
	 * dangling pointer, so it needs to be done prior to HASH_REMOVE).
	 * This requires an extra hash-table lookup, but DEALLOCATE isn't
	 * exactly a performance bottleneck.
	 */
	entry = (QueryHashEntry *) hash_search(prepared_queries,
										   key,
										   HASH_FIND,
										   NULL);

	if (!entry)
		elog(ERROR, "Prepared statement with name \"%s\" does not exist",
			 stmt->name);

	/* Flush the context holding the subsidiary data */
	Assert(MemoryContextIsValid(entry->context));
	MemoryContextDelete(entry->context);

	/* Now we can remove the hash table entry */
	hash_search(prepared_queries, key, HASH_REMOVE, NULL);
}
