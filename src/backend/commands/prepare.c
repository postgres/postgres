/*-------------------------------------------------------------------------
 *
 * prepare.c
 *	  Prepareable SQL statements via PREPARE, EXECUTE and DEALLOCATE
 *
 * This module also implements storage of prepared statements that are
 * accessed via the extended FE/BE query protocol.
 *
 *
 * Copyright (c) 2002-2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/prepare.c,v 1.23.4.3 2005/12/14 17:06:59 tgl Exp $
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
	 * Parse analysis is already done, but we must still rewrite and plan
	 * the query.
	 */

	/*
	 * Because the planner is not cool about not scribbling on its input,
	 * we make a preliminary copy of the source querytree.  This prevents
	 * problems in the case that the PREPARE is in a portal or plpgsql
	 * function and is executed repeatedly.  (See also the same hack in
	 * DECLARE CURSOR and EXPLAIN.)  XXX the planner really shouldn't
	 * modify its input ... FIXME someday.
	 */
	query = copyObject(stmt->query);

	/* Rewrite the query. The result could be 0, 1, or many queries. */
	query_list = QueryRewrite(query);

	/* Generate plans for queries.	Snapshot is already set. */
	plan_list = pg_plan_queries(query_list, false);

	/* Save the results. */
	StorePreparedStatement(stmt->name,
						   NULL,	/* text form not available */
						   commandTag,
						   query_list,
						   plan_list,
						   stmt->argtype_oids);
}

/*
 * Implements the 'EXECUTE' utility statement.
 */
void
ExecuteQuery(ExecuteStmt *stmt, DestReceiver *dest)
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

	Assert(length(query_list) == length(plan_list));

	/* Evaluate parameters, if any */
	if (entry->argtype_list != NIL)
	{
		/*
		 * Need an EState to evaluate parameters; must not delete it till
		 * end of query, in case parameters are pass-by-reference.
		 */
		estate = CreateExecutorState();
		paramLI = EvaluateParams(estate, stmt->params, entry->argtype_list);
	}

	/*
	 * Create a new portal to run the query in
	 */
	portal = CreateNewPortal();

	/*
	 * For CREATE TABLE / AS EXECUTE, make a copy of the stored query so
	 * that we can modify its destination (yech, but this has always been
	 * ugly).  For regular EXECUTE we can just use the stored query where
	 * it sits, since the executor is read-only.
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

		if (length(query_list) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("prepared statement is not a SELECT")));
		query = (Query *) lfirst(query_list);
		if (query->commandType != CMD_SELECT)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("prepared statement is not a SELECT")));
		query->into = copyObject(stmt->into);

		MemoryContextSwitchTo(oldContext);
	}

	PortalDefineQuery(portal,
					  query_string,
					  entry->commandTag,
					  query_list,
					  plan_list,
					  qcontext);

	/*
	 * Run the portal to completion.
	 */
	PortalStart(portal, paramLI);

	(void) PortalRun(portal, FETCH_ALL, dest, dest, NULL);

	PortalDrop(portal, false);

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
	int			nargs = length(argtypes);
	ParamListInfo paramLI;
	List	   *exprstates;
	List	   *l;
	int			i = 0;

	/* Parser should have caught this error, but check for safety */
	if (length(params) != nargs)
		elog(ERROR, "wrong number of arguments");

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

	hash_ctl.keysize = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(PreparedStatement);

	prepared_queries = hash_create("Prepared Queries",
								   32,
								   &hash_ctl,
								   HASH_ELEM);

	if (!prepared_queries)
		elog(ERROR, "could not create hash table");
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
					   List *argtype_list)
{
	PreparedStatement *entry;
	MemoryContext oldcxt,
				entrycxt;
	char	   *qstring;
	char		key[NAMEDATALEN];
	bool		found;

	/* Initialize the hash table, if necessary */
	if (!prepared_queries)
		InitQueryHashTable();

	/* Check for pre-existing entry of same name */
	/* See notes in FetchPreparedStatement */
	StrNCpy(key, stmt_name, sizeof(key));

	hash_search(prepared_queries, key, HASH_FIND, &found);

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
	 * out-of-memory failure only wastes memory and doesn't leave us with
	 * an incomplete (ie corrupt) hashtable entry.
	 */
	qstring = query_string ? pstrdup(query_string) : (char *) NULL;
	query_list = (List *) copyObject(query_list);
	plan_list = (List *) copyObject(plan_list);
	argtype_list = listCopy(argtype_list);

	/* Now we can add entry to hash table */
	entry = (PreparedStatement *) hash_search(prepared_queries,
											  key,
											  HASH_ENTER,
											  &found);

	/* Shouldn't get a failure, nor a duplicate entry */
	if (!entry || found)
		elog(ERROR, "could not store prepared statement \"%s\"",
			 stmt_name);

	/* Fill in the hash table entry with copied data */
	entry->query_string = qstring;
	entry->commandTag = commandTag;
	entry->query_list = query_list;
	entry->plan_list = plan_list;
	entry->argtype_list = argtype_list;
	entry->context = entrycxt;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Lookup an existing query in the hash table. If the query does not
 * actually exist, throw ereport(ERROR) or return NULL per second parameter.
 */
PreparedStatement *
FetchPreparedStatement(const char *stmt_name, bool throwError)
{
	char		key[NAMEDATALEN];
	PreparedStatement *entry;

	/*
	 * If the hash table hasn't been initialized, it can't be storing
	 * anything, therefore it couldn't possibly store our plan.
	 */
	if (prepared_queries)
	{
		/*
		 * We can't just use the statement name as supplied by the user:
		 * the hash package is picky enough that it needs to be
		 * NULL-padded out to the appropriate length to work correctly.
		 */
		StrNCpy(key, stmt_name, sizeof(key));

		entry = (PreparedStatement *) hash_search(prepared_queries,
												  key,
												  HASH_FIND,
												  NULL);
	}
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
			query = (Query *) lfirst(stmt->query_list);
			return ExecCleanTypeFromTL(query->targetList, false);

		case PORTAL_UTIL_SELECT:
			query = (Query *) lfirst(stmt->query_list);
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
		case PORTAL_UTIL_SELECT:
			return true;

		case PORTAL_MULTI_QUERY:
			/* will not return tuples */
			break;
	}
	return false;
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
ExplainExecuteQuery(ExplainStmt *stmt, TupOutputState *tstate)
{
	ExecuteStmt *execstmt = (ExecuteStmt *) stmt->query->utilityStmt;
	PreparedStatement *entry;
	List	   *l,
			   *query_list,
			   *plan_list;
	ParamListInfo paramLI = NULL;
	EState	   *estate = NULL;

	/* explain.c should only call me for EXECUTE stmt */
	Assert(execstmt && IsA(execstmt, ExecuteStmt));

	/* Look it up in the hash table */
	entry = FetchPreparedStatement(execstmt->name, true);

	query_list = entry->query_list;
	plan_list = entry->plan_list;

	Assert(length(query_list) == length(plan_list));

	/* Evaluate parameters, if any */
	if (entry->argtype_list != NIL)
	{
		/*
		 * Need an EState to evaluate parameters; must not delete it till
		 * end of query, in case parameters are pass-by-reference.
		 */
		estate = CreateExecutorState();
		paramLI = EvaluateParams(estate, execstmt->params,
								 entry->argtype_list);
	}

	/* Explain each query */
	foreach(l, query_list)
	{
		Query	   *query = (Query *) lfirst(l);
		Plan	   *plan = (Plan *) lfirst(plan_list);
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

			/* Create a QueryDesc requesting no output */
			qdesc = CreateQueryDesc(query, plan, None_Receiver,
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
