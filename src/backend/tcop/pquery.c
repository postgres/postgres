/*-------------------------------------------------------------------------
 *
 * pquery.c
 *	  POSTGRES process query command code
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/pquery.c,v 1.73.2.1 2004/03/05 00:21:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"
#include "miscadmin.h"
#include "tcop/tcopprot.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/memutils.h"


static uint32 RunFromStore(Portal portal, ScanDirection direction, long count,
			 DestReceiver *dest);
static long PortalRunSelect(Portal portal, bool forward, long count,
				DestReceiver *dest);
static void PortalRunUtility(Portal portal, Query *query,
				 DestReceiver *dest, char *completionTag);
static void PortalRunMulti(Portal portal,
			   DestReceiver *dest, DestReceiver *altdest,
			   char *completionTag);
static long DoPortalRunFetch(Portal portal,
				 FetchDirection fdirection,
				 long count,
				 DestReceiver *dest);
static void DoPortalRewind(Portal portal);


/*
 * CreateQueryDesc
 */
QueryDesc *
CreateQueryDesc(Query *parsetree,
				Plan *plantree,
				DestReceiver *dest,
				ParamListInfo params,
				bool doInstrument)
{
	QueryDesc  *qd = (QueryDesc *) palloc(sizeof(QueryDesc));

	qd->operation = parsetree->commandType;		/* operation */
	qd->parsetree = parsetree;	/* parse tree */
	qd->plantree = plantree;	/* plan */
	qd->dest = dest;			/* output dest */
	qd->params = params;		/* parameter values passed into query */
	qd->doInstrument = doInstrument;	/* instrumentation wanted? */

	/* null these fields until set by ExecutorStart */
	qd->tupDesc = NULL;
	qd->estate = NULL;
	qd->planstate = NULL;

	return qd;
}

/*
 * FreeQueryDesc
 */
void
FreeQueryDesc(QueryDesc *qdesc)
{
	/* Can't be a live query */
	Assert(qdesc->estate == NULL);
	/* Only the QueryDesc itself need be freed */
	pfree(qdesc);
}


/*
 * ProcessQuery
 *		Execute a single query
 *
 *	parsetree: the query tree
 *	plan: the plan tree for the query
 *	params: any parameters needed
 *	dest: where to send results
 *	completionTag: points to a buffer of size COMPLETION_TAG_BUFSIZE
 *		in which to store a command completion status string.
 *
 * completionTag may be NULL if caller doesn't want a status string.
 *
 * Must be called in a memory context that will be reset or deleted on
 * error; otherwise the executor's memory usage will be leaked.
 */
void
ProcessQuery(Query *parsetree,
			 Plan *plan,
			 ParamListInfo params,
			 DestReceiver *dest,
			 char *completionTag)
{
	int			operation = parsetree->commandType;
	QueryDesc  *queryDesc;

	/*
	 * Check for special-case destinations
	 */
	if (operation == CMD_SELECT)
	{
		if (parsetree->into != NULL)
		{
			/*
			 * SELECT INTO table (a/k/a CREATE AS ... SELECT).
			 *
			 * Override the normal communication destination; execMain.c
			 * special-cases this case.  (Perhaps would be cleaner to have
			 * an additional destination type?)
			 */
			dest = None_Receiver;
		}
	}

	/*
	 * Create the QueryDesc object
	 */
	queryDesc = CreateQueryDesc(parsetree, plan, dest, params, false);

	/*
	 * Call ExecStart to prepare the plan for execution
	 */
	ExecutorStart(queryDesc, false, false);

	/*
	 * Run the plan to completion.
	 */
	ExecutorRun(queryDesc, ForwardScanDirection, 0L);

	/*
	 * Build command completion status string, if caller wants one.
	 */
	if (completionTag)
	{
		Oid			lastOid;

		switch (operation)
		{
			case CMD_SELECT:
				strcpy(completionTag, "SELECT");
				break;
			case CMD_INSERT:
				if (queryDesc->estate->es_processed == 1)
					lastOid = queryDesc->estate->es_lastoid;
				else
					lastOid = InvalidOid;
				snprintf(completionTag, COMPLETION_TAG_BUFSIZE,
				"INSERT %u %u", lastOid, queryDesc->estate->es_processed);
				break;
			case CMD_UPDATE:
				snprintf(completionTag, COMPLETION_TAG_BUFSIZE,
						 "UPDATE %u", queryDesc->estate->es_processed);
				break;
			case CMD_DELETE:
				snprintf(completionTag, COMPLETION_TAG_BUFSIZE,
						 "DELETE %u", queryDesc->estate->es_processed);
				break;
			default:
				strcpy(completionTag, "???");
				break;
		}
	}

	/*
	 * Now, we close down all the scans and free allocated resources.
	 */
	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);
}

/*
 * ChoosePortalStrategy
 *		Select portal execution strategy given the intended query list.
 *
 * See the comments in portal.h.
 */
PortalStrategy
ChoosePortalStrategy(List *parseTrees)
{
	PortalStrategy strategy;

	strategy = PORTAL_MULTI_QUERY;		/* default assumption */

	if (length(parseTrees) == 1)
	{
		Query	   *query = (Query *) lfirst(parseTrees);

		if (query->commandType == CMD_SELECT &&
			query->canSetTag &&
			query->into == NULL)
			strategy = PORTAL_ONE_SELECT;
		else if (query->commandType == CMD_UTILITY &&
				 query->canSetTag &&
				 query->utilityStmt != NULL)
		{
			if (UtilityReturnsTuples(query->utilityStmt))
				strategy = PORTAL_UTIL_SELECT;
		}
	}
	return strategy;
}

/*
 * PortalStart
 *		Prepare a portal for execution.
 *
 * Caller must already have created the portal, done PortalDefineQuery(),
 * and adjusted portal options if needed.  If parameters are needed by
 * the query, they must be passed in here (caller is responsible for
 * giving them appropriate lifetime).
 *
 * On return, portal is ready to accept PortalRun() calls, and the result
 * tupdesc (if any) is known.
 */
void
PortalStart(Portal portal, ParamListInfo params)
{
	MemoryContext oldContext;
	QueryDesc  *queryDesc;

	AssertArg(PortalIsValid(portal));
	AssertState(portal->queryContext != NULL);	/* query defined? */
	AssertState(!portal->portalReady);	/* else extra PortalStart */

	oldContext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));

	/* Must remember portal param list, if any */
	portal->portalParams = params;

	/*
	 * Determine the portal execution strategy
	 */
	portal->strategy = ChoosePortalStrategy(portal->parseTrees);

	/*
	 * Fire her up according to the strategy
	 */
	switch (portal->strategy)
	{
		case PORTAL_ONE_SELECT:

			/*
			 * Must set query snapshot before starting executor.
			 */
			SetQuerySnapshot();

			/*
			 * Create QueryDesc in portal's context; for the moment, set
			 * the destination to None.
			 */
			queryDesc = CreateQueryDesc((Query *) lfirst(portal->parseTrees),
									  (Plan *) lfirst(portal->planTrees),
										None_Receiver,
										params,
										false);

			/*
			 * Call ExecStart to prepare the plan for execution
			 */
			ExecutorStart(queryDesc, false, false);

			/*
			 * This tells PortalCleanup to shut down the executor
			 */
			portal->queryDesc = queryDesc;

			/*
			 * Remember tuple descriptor (computed by ExecutorStart)
			 */
			portal->tupDesc = queryDesc->tupDesc;

			/*
			 * Reset cursor position data to "start of query"
			 */
			portal->atStart = true;
			portal->atEnd = false;		/* allow fetches */
			portal->portalPos = 0;
			portal->posOverflow = false;
			break;

		case PORTAL_UTIL_SELECT:

			/*
			 * We don't set query snapshot here, because PortalRunUtility
			 * will take care of it.
			 */
			portal->tupDesc =
				UtilityTupleDescriptor(((Query *) lfirst(portal->parseTrees))->utilityStmt);

			/*
			 * Reset cursor position data to "start of query"
			 */
			portal->atStart = true;
			portal->atEnd = false;		/* allow fetches */
			portal->portalPos = 0;
			portal->posOverflow = false;
			break;

		case PORTAL_MULTI_QUERY:
			/* Need do nothing now */
			portal->tupDesc = NULL;
			break;
	}

	MemoryContextSwitchTo(oldContext);

	portal->portalReady = true;
}

/*
 * PortalSetResultFormat
 *		Select the format codes for a portal's output.
 *
 * This must be run after PortalStart for a portal that will be read by
 * a Remote or RemoteExecute destination.  It is not presently needed for
 * other destination types.
 *
 * formats[] is the client format request, as per Bind message conventions.
 */
void
PortalSetResultFormat(Portal portal, int nFormats, int16 *formats)
{
	int			natts;
	int			i;

	/* Do nothing if portal won't return tuples */
	if (portal->tupDesc == NULL)
		return;
	natts = portal->tupDesc->natts;
	/* +1 avoids palloc(0) if no columns */
	portal->formats = (int16 *)
		MemoryContextAlloc(PortalGetHeapMemory(portal),
						   (natts + 1) * sizeof(int16));
	if (nFormats > 1)
	{
		/* format specified for each column */
		if (nFormats != natts)
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("bind message has %d result formats but query has %d columns",
							nFormats, natts)));
		memcpy(portal->formats, formats, natts * sizeof(int16));
	}
	else if (nFormats > 0)
	{
		/* single format specified, use for all columns */
		int16		format1 = formats[0];

		for (i = 0; i < natts; i++)
			portal->formats[i] = format1;
	}
	else
	{
		/* use default format for all columns */
		for (i = 0; i < natts; i++)
			portal->formats[i] = 0;
	}
}

/*
 * PortalRun
 *		Run a portal's query or queries.
 *
 * count <= 0 is interpreted as a no-op: the destination gets started up
 * and shut down, but nothing else happens.  Also, count == FETCH_ALL is
 * interpreted as "all rows".  Note that count is ignored in multi-query
 * situations, where we always run the portal to completion.
 *
 * dest: where to send output of primary (canSetTag) query
 *
 * altdest: where to send output of non-primary queries
 *
 * completionTag: points to a buffer of size COMPLETION_TAG_BUFSIZE
 *		in which to store a command completion status string.
 *		May be NULL if caller doesn't want a status string.
 *
 * Returns TRUE if the portal's execution is complete, FALSE if it was
 * suspended due to exhaustion of the count parameter.
 */
bool
PortalRun(Portal portal, long count,
		  DestReceiver *dest, DestReceiver *altdest,
		  char *completionTag)
{
	bool		result;
	MemoryContext savePortalContext;
	MemoryContext saveQueryContext;
	MemoryContext oldContext;

	AssertArg(PortalIsValid(portal));
	AssertState(portal->portalReady);	/* else no PortalStart */

	/* Initialize completion tag to empty string */
	if (completionTag)
		completionTag[0] = '\0';

	if (portal->strategy != PORTAL_MULTI_QUERY)
	{
		ereport(DEBUG3,
			(errmsg_internal("PortalRun")));
		/* PORTAL_MULTI_QUERY logs its own stats per query */
		if (log_executor_stats)
			ResetUsage();
	}
	
	if (log_executor_stats && portal->strategy != PORTAL_MULTI_QUERY)

	/*
	 * Check for improper portal use, and mark portal active.
	 */
	if (portal->portalDone)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		   errmsg("portal \"%s\" cannot be run anymore", portal->name)));
	if (portal->portalActive)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("portal \"%s\" already active", portal->name)));
	portal->portalActive = true;

	/*
	 * Set global portal context pointers.
	 */
	savePortalContext = PortalContext;
	PortalContext = PortalGetHeapMemory(portal);
	saveQueryContext = QueryContext;
	QueryContext = portal->queryContext;

	oldContext = MemoryContextSwitchTo(PortalContext);

	switch (portal->strategy)
	{
		case PORTAL_ONE_SELECT:
			(void) PortalRunSelect(portal, true, count, dest);
			/* we know the query is supposed to set the tag */
			if (completionTag && portal->commandTag)
				strcpy(completionTag, portal->commandTag);

			/*
			 * Since it's a forward fetch, say DONE iff atEnd is now true.
			 */
			result = portal->atEnd;
			break;

		case PORTAL_UTIL_SELECT:

			/*
			 * If we have not yet run the utility statement, do so,
			 * storing its results in the portal's tuplestore.
			 */
			if (!portal->portalUtilReady)
			{
				DestReceiver *treceiver;

				PortalCreateHoldStore(portal);
				treceiver = CreateDestReceiver(Tuplestore, portal);
				PortalRunUtility(portal, lfirst(portal->parseTrees),
								 treceiver, NULL);
				(*treceiver->rDestroy) (treceiver);
				portal->portalUtilReady = true;
			}

			/*
			 * Now fetch desired portion of results.
			 */
			(void) PortalRunSelect(portal, true, count, dest);

			/*
			 * We know the query is supposed to set the tag; we assume
			 * only the default tag is needed.
			 */
			if (completionTag && portal->commandTag)
				strcpy(completionTag, portal->commandTag);

			/*
			 * Since it's a forward fetch, say DONE iff atEnd is now true.
			 */
			result = portal->atEnd;
			break;

		case PORTAL_MULTI_QUERY:
			PortalRunMulti(portal, dest, altdest, completionTag);
			/* Always complete at end of RunMulti */
			result = true;
			break;

		default:
			elog(ERROR, "unrecognized portal strategy: %d",
				 (int) portal->strategy);
			result = false;		/* keep compiler quiet */
			break;
	}

	MemoryContextSwitchTo(oldContext);

	/* Mark portal not active */
	portal->portalActive = false;

	PortalContext = savePortalContext;
	QueryContext = saveQueryContext;

	if (log_executor_stats && portal->strategy != PORTAL_MULTI_QUERY)
		ShowUsage("EXECUTOR STATISTICS");

	return result;
}

/*
 * PortalRunSelect
 *		Execute a portal's query in SELECT cases (also UTIL_SELECT).
 *
 * This handles simple N-rows-forward-or-backward cases.  For more complex
 * nonsequential access to a portal, see PortalRunFetch.
 *
 * count <= 0 is interpreted as a no-op: the destination gets started up
 * and shut down, but nothing else happens.  Also, count == FETCH_ALL is
 * interpreted as "all rows".
 *
 * Caller must already have validated the Portal and done appropriate
 * setup (cf. PortalRun).
 *
 * Returns number of rows processed (suitable for use in result tag)
 */
static long
PortalRunSelect(Portal portal,
				bool forward,
				long count,
				DestReceiver *dest)
{
	QueryDesc  *queryDesc;
	ScanDirection direction;
	uint32		nprocessed;

	/*
	 * NB: queryDesc will be NULL if we are fetching from a held cursor or
	 * a completed utility query; can't use it in that path.
	 */
	queryDesc = PortalGetQueryDesc(portal);

	/* Caller messed up if we have neither a ready query nor held data. */
	Assert(queryDesc || portal->holdStore);

	/*
	 * Force the queryDesc destination to the right thing.	This supports
	 * MOVE, for example, which will pass in dest = None.  This is okay to
	 * change as long as we do it on every fetch.  (The Executor must not
	 * assume that dest never changes.)
	 */
	if (queryDesc)
		queryDesc->dest = dest;

	/*
	 * Determine which direction to go in, and check to see if we're
	 * already at the end of the available tuples in that direction.  If
	 * so, set the direction to NoMovement to avoid trying to fetch any
	 * tuples.	(This check exists because not all plan node types are
	 * robust about being called again if they've already returned NULL
	 * once.)  Then call the executor (we must not skip this, because the
	 * destination needs to see a setup and shutdown even if no tuples are
	 * available).	Finally, update the portal position state depending on
	 * the number of tuples that were retrieved.
	 */
	if (forward)
	{
		if (portal->atEnd || count <= 0)
			direction = NoMovementScanDirection;
		else
			direction = ForwardScanDirection;

		/* In the executor, zero count processes all rows */
		if (count == FETCH_ALL)
			count = 0;

		if (portal->holdStore)
			nprocessed = RunFromStore(portal, direction, count, dest);
		else
		{
			ExecutorRun(queryDesc, direction, count);
			nprocessed = queryDesc->estate->es_processed;
		}

		if (direction != NoMovementScanDirection)
		{
			long		oldPos;

			if (nprocessed > 0)
				portal->atStart = false;		/* OK to go backward now */
			if (count == 0 ||
				(unsigned long) nprocessed < (unsigned long) count)
				portal->atEnd = true;	/* we retrieved 'em all */
			oldPos = portal->portalPos;
			portal->portalPos += nprocessed;
			/* portalPos doesn't advance when we fall off the end */
			if (portal->portalPos < oldPos)
				portal->posOverflow = true;
		}
	}
	else
	{
		if (portal->cursorOptions & CURSOR_OPT_NO_SCROLL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cursor can only scan forward"),
					 errhint("Declare it with SCROLL option to enable backward scan.")));

		if (portal->atStart || count <= 0)
			direction = NoMovementScanDirection;
		else
			direction = BackwardScanDirection;

		/* In the executor, zero count processes all rows */
		if (count == FETCH_ALL)
			count = 0;

		if (portal->holdStore)
			nprocessed = RunFromStore(portal, direction, count, dest);
		else
		{
			ExecutorRun(queryDesc, direction, count);
			nprocessed = queryDesc->estate->es_processed;
		}

		if (direction != NoMovementScanDirection)
		{
			if (nprocessed > 0 && portal->atEnd)
			{
				portal->atEnd = false;	/* OK to go forward now */
				portal->portalPos++;	/* adjust for endpoint case */
			}
			if (count == 0 ||
				(unsigned long) nprocessed < (unsigned long) count)
			{
				portal->atStart = true; /* we retrieved 'em all */
				portal->portalPos = 0;
				portal->posOverflow = false;
			}
			else
			{
				long		oldPos;

				oldPos = portal->portalPos;
				portal->portalPos -= nprocessed;
				if (portal->portalPos > oldPos ||
					portal->portalPos <= 0)
					portal->posOverflow = true;
			}
		}
	}

	return nprocessed;
}

/*
 * RunFromStore
 *		Fetch tuples from the portal's tuple store.
 *
 * Calling conventions are similar to ExecutorRun, except that we
 * do not depend on having a queryDesc or estate.  Therefore we return the
 * number of tuples processed as the result, not in estate->es_processed.
 *
 * One difference from ExecutorRun is that the destination receiver functions
 * are run in the caller's memory context (since we have no estate).  Watch
 * out for memory leaks.
 */
static uint32
RunFromStore(Portal portal, ScanDirection direction, long count,
			 DestReceiver *dest)
{
	long		current_tuple_count = 0;

	(*dest->rStartup) (dest, CMD_SELECT, portal->tupDesc);

	if (direction == NoMovementScanDirection)
	{
		/* do nothing except start/stop the destination */
	}
	else
	{
		bool		forward = (direction == ForwardScanDirection);

		for (;;)
		{
			MemoryContext oldcontext;
			HeapTuple	tup;
			bool		should_free;

			oldcontext = MemoryContextSwitchTo(portal->holdContext);

			tup = tuplestore_getheaptuple(portal->holdStore, forward,
										  &should_free);

			MemoryContextSwitchTo(oldcontext);

			if (tup == NULL)
				break;

			(*dest->receiveTuple) (tup, portal->tupDesc, dest);

			if (should_free)
				pfree(tup);

			/*
			 * check our tuple count.. if we've processed the proper
			 * number then quit, else loop again and process more tuples.
			 * Zero count means no limit.
			 */
			current_tuple_count++;
			if (count && count == current_tuple_count)
				break;
		}
	}

	(*dest->rShutdown) (dest);

	return (uint32) current_tuple_count;
}

/*
 * PortalRunUtility
 *		Execute a utility statement inside a portal.
 */
static void
PortalRunUtility(Portal portal, Query *query,
				 DestReceiver *dest, char *completionTag)
{
	Node	   *utilityStmt = query->utilityStmt;

	ereport(DEBUG3,
			(errmsg_internal("ProcessUtility")));

	/*
	 * Set snapshot if utility stmt needs one.	Most reliable way to do
	 * this seems to be to enumerate those that do not need one; this is a
	 * short list.	Transaction control, LOCK, and SET must *not* set a
	 * snapshot since they need to be executable at the start of a
	 * serializable transaction without freezing a snapshot.  By extension
	 * we allow SHOW not to set a snapshot.  The other stmts listed are
	 * just efficiency hacks.  Beware of listing anything that can modify
	 * the database --- if, say, it has to update an index with
	 * expressions that invoke user-defined functions, then it had better
	 * have a snapshot.
	 */
	if (!(IsA(utilityStmt, TransactionStmt) ||
		  IsA(utilityStmt, LockStmt) ||
		  IsA(utilityStmt, VariableSetStmt) ||
		  IsA(utilityStmt, VariableShowStmt) ||
		  IsA(utilityStmt, VariableResetStmt) ||
		  IsA(utilityStmt, ConstraintsSetStmt) ||
	/* efficiency hacks from here down */
		  IsA(utilityStmt, FetchStmt) ||
		  IsA(utilityStmt, ListenStmt) ||
		  IsA(utilityStmt, NotifyStmt) ||
		  IsA(utilityStmt, UnlistenStmt) ||
		  IsA(utilityStmt, CheckPointStmt)))
		SetQuerySnapshot();

	if (query->canSetTag)
	{
		/* utility statement can override default tag string */
		ProcessUtility(utilityStmt, dest, completionTag);
		if (completionTag && completionTag[0] == '\0' && portal->commandTag)
			strcpy(completionTag, portal->commandTag);	/* use the default */
	}
	else
	{
		/* utility added by rewrite cannot set tag */
		ProcessUtility(utilityStmt, dest, NULL);
	}

	/* Some utility statements may change context on us */
	MemoryContextSwitchTo(PortalGetHeapMemory(portal));
}

/*
 * PortalRunMulti
 *		Execute a portal's queries in the general case (multi queries).
 */
static void
PortalRunMulti(Portal portal,
			   DestReceiver *dest, DestReceiver *altdest,
			   char *completionTag)
{
	List	   *plantree_list = portal->planTrees;
	List	   *querylist_item;

	/*
	 * If the destination is RemoteExecute, change to None.  The reason is
	 * that the client won't be expecting any tuples, and indeed has no
	 * way to know what they are, since there is no provision for Describe
	 * to send a RowDescription message when this portal execution
	 * strategy is in effect.  This presently will only affect SELECT
	 * commands added to non-SELECT queries by rewrite rules: such
	 * commands will be executed, but the results will be discarded unless
	 * you use "simple Query" protocol.
	 */
	if (dest->mydest == RemoteExecute)
		dest = None_Receiver;
	if (altdest->mydest == RemoteExecute)
		altdest = None_Receiver;

	/*
	 * Loop to handle the individual queries generated from a single
	 * parsetree by analysis and rewrite.
	 */
	foreach(querylist_item, portal->parseTrees)
	{
		Query	   *query = (Query *) lfirst(querylist_item);
		Plan	   *plan = (Plan *) lfirst(plantree_list);

		plantree_list = lnext(plantree_list);

		/*
		 * If we got a cancel signal in prior command, quit
		 */
		CHECK_FOR_INTERRUPTS();

		if (query->commandType == CMD_UTILITY)
		{
			/*
			 * process utility functions (create, destroy, etc..)
			 */
			Assert(plan == NULL);

			PortalRunUtility(portal, query,
							 query->canSetTag ? dest : altdest,
							 completionTag);
		}
		else
		{
			/*
			 * process a plannable query.
			 */
			ereport(DEBUG3,
					(errmsg_internal("ProcessQuery")));

			/* Must always set snapshot for plannable queries */
			SetQuerySnapshot();

			/*
			 * execute the plan
			 */
			if (log_executor_stats)
				ResetUsage();

			if (query->canSetTag)
			{
				/* statement can set tag string */
				ProcessQuery(query, plan,
							 portal->portalParams,
							 dest, completionTag);
			}
			else
			{
				/* stmt added by rewrite cannot set tag */
				ProcessQuery(query, plan,
							 portal->portalParams,
							 altdest, NULL);
			}

			if (log_executor_stats)
				ShowUsage("EXECUTOR STATISTICS");
		}

		/*
		 * Increment command counter between queries, but not after the
		 * last one.
		 */
		if (plantree_list != NIL)
			CommandCounterIncrement();

		/*
		 * Clear subsidiary contexts to recover temporary memory.
		 */
		Assert(PortalGetHeapMemory(portal) == CurrentMemoryContext);

		MemoryContextDeleteChildren(PortalGetHeapMemory(portal));
	}

	/*
	 * If a command completion tag was supplied, use it.  Otherwise use
	 * the portal's commandTag as the default completion tag.
	 *
	 * Exception: clients will expect INSERT/UPDATE/DELETE tags to have
	 * counts, so fake something up if necessary.  (This could happen if
	 * the original query was replaced by a DO INSTEAD rule.)
	 */
	if (completionTag && completionTag[0] == '\0')
	{
		if (portal->commandTag)
			strcpy(completionTag, portal->commandTag);
		if (strcmp(completionTag, "INSERT") == 0)
			strcpy(completionTag, "INSERT 0 0");
		else if (strcmp(completionTag, "UPDATE") == 0)
			strcpy(completionTag, "UPDATE 0");
		else if (strcmp(completionTag, "DELETE") == 0)
			strcpy(completionTag, "DELETE 0");
	}

	/* Prevent portal's commands from being re-executed */
	portal->portalDone = true;
}

/*
 * PortalRunFetch
 *		Variant form of PortalRun that supports SQL FETCH directions.
 *
 * Returns number of rows processed (suitable for use in result tag)
 */
long
PortalRunFetch(Portal portal,
			   FetchDirection fdirection,
			   long count,
			   DestReceiver *dest)
{
	long		result;
	MemoryContext savePortalContext;
	MemoryContext saveQueryContext;
	MemoryContext oldContext;

	AssertArg(PortalIsValid(portal));
	AssertState(portal->portalReady);	/* else no PortalStart */

	/*
	 * Check for improper portal use, and mark portal active.
	 */
	if (portal->portalDone)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		   errmsg("portal \"%s\" cannot be run anymore", portal->name)));
	if (portal->portalActive)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("portal \"%s\" already active", portal->name)));
	portal->portalActive = true;

	/*
	 * Set global portal context pointers.
	 */
	savePortalContext = PortalContext;
	PortalContext = PortalGetHeapMemory(portal);
	saveQueryContext = QueryContext;
	QueryContext = portal->queryContext;

	oldContext = MemoryContextSwitchTo(PortalContext);

	switch (portal->strategy)
	{
		case PORTAL_ONE_SELECT:
			result = DoPortalRunFetch(portal, fdirection, count, dest);
			break;

		default:
			elog(ERROR, "unsupported portal strategy");
			result = 0;			/* keep compiler quiet */
			break;
	}

	MemoryContextSwitchTo(oldContext);

	/* Mark portal not active */
	portal->portalActive = false;

	PortalContext = savePortalContext;
	QueryContext = saveQueryContext;

	return result;
}

/*
 * DoPortalRunFetch
 *		Guts of PortalRunFetch --- the portal context is already set up
 *
 * Returns number of rows processed (suitable for use in result tag)
 */
static long
DoPortalRunFetch(Portal portal,
				 FetchDirection fdirection,
				 long count,
				 DestReceiver *dest)
{
	bool		forward;

	Assert(portal->strategy == PORTAL_ONE_SELECT);

	switch (fdirection)
	{
		case FETCH_FORWARD:
			if (count < 0)
			{
				fdirection = FETCH_BACKWARD;
				count = -count;
			}
			/* fall out of switch to share code with FETCH_BACKWARD */
			break;
		case FETCH_BACKWARD:
			if (count < 0)
			{
				fdirection = FETCH_FORWARD;
				count = -count;
			}
			/* fall out of switch to share code with FETCH_FORWARD */
			break;
		case FETCH_ABSOLUTE:
			if (count > 0)
			{
				/*
				 * Definition: Rewind to start, advance count-1 rows,
				 * return next row (if any).  In practice, if the goal is
				 * less than halfway back to the start, it's better to
				 * scan from where we are.	In any case, we arrange to
				 * fetch the target row going forwards.
				 */
				if (portal->posOverflow || portal->portalPos == LONG_MAX ||
					count - 1 <= portal->portalPos / 2)
				{
					DoPortalRewind(portal);
					if (count > 1)
						PortalRunSelect(portal, true, count - 1,
										None_Receiver);
				}
				else
				{
					long		pos = portal->portalPos;

					if (portal->atEnd)
						pos++;	/* need one extra fetch if off end */
					if (count <= pos)
						PortalRunSelect(portal, false, pos - count + 1,
										None_Receiver);
					else if (count > pos + 1)
						PortalRunSelect(portal, true, count - pos - 1,
										None_Receiver);
				}
				return PortalRunSelect(portal, true, 1L, dest);
			}
			else if (count < 0)
			{
				/*
				 * Definition: Advance to end, back up abs(count)-1 rows,
				 * return prior row (if any).  We could optimize this if
				 * we knew in advance where the end was, but typically we
				 * won't. (Is it worth considering case where count > half
				 * of size of query?  We could rewind once we know the
				 * size ...)
				 */
				PortalRunSelect(portal, true, FETCH_ALL, None_Receiver);
				if (count < -1)
					PortalRunSelect(portal, false, -count - 1, None_Receiver);
				return PortalRunSelect(portal, false, 1L, dest);
			}
			else
/* count == 0 */
			{
				/* Rewind to start, return zero rows */
				DoPortalRewind(portal);
				return PortalRunSelect(portal, true, 0L, dest);
			}
			break;
		case FETCH_RELATIVE:
			if (count > 0)
			{
				/*
				 * Definition: advance count-1 rows, return next row (if
				 * any).
				 */
				if (count > 1)
					PortalRunSelect(portal, true, count - 1, None_Receiver);
				return PortalRunSelect(portal, true, 1L, dest);
			}
			else if (count < 0)
			{
				/*
				 * Definition: back up abs(count)-1 rows, return prior row
				 * (if any).
				 */
				if (count < -1)
					PortalRunSelect(portal, false, -count - 1, None_Receiver);
				return PortalRunSelect(portal, false, 1L, dest);
			}
			else
/* count == 0 */
			{
				/* Same as FETCH FORWARD 0, so fall out of switch */
				fdirection = FETCH_FORWARD;
			}
			break;
		default:
			elog(ERROR, "bogus direction");
			break;
	}

	/*
	 * Get here with fdirection == FETCH_FORWARD or FETCH_BACKWARD, and
	 * count >= 0.
	 */
	forward = (fdirection == FETCH_FORWARD);

	/*
	 * Zero count means to re-fetch the current row, if any (per SQL92)
	 */
	if (count == 0)
	{
		bool		on_row;

		/* Are we sitting on a row? */
		on_row = (!portal->atStart && !portal->atEnd);

		if (dest->mydest == None)
		{
			/* MOVE 0 returns 0/1 based on if FETCH 0 would return a row */
			return on_row ? 1L : 0L;
		}
		else
		{
			/*
			 * If we are sitting on a row, back up one so we can re-fetch
			 * it. If we are not sitting on a row, we still have to start
			 * up and shut down the executor so that the destination is
			 * initialized and shut down correctly; so keep going.	To
			 * PortalRunSelect, count == 0 means we will retrieve no row.
			 */
			if (on_row)
			{
				PortalRunSelect(portal, false, 1L, None_Receiver);
				/* Set up to fetch one row forward */
				count = 1;
				forward = true;
			}
		}
	}

	/*
	 * Optimize MOVE BACKWARD ALL into a Rewind.
	 */
	if (!forward && count == FETCH_ALL && dest->mydest == None)
	{
		long		result = portal->portalPos;

		if (result > 0 && !portal->atEnd)
			result--;
		DoPortalRewind(portal);
		/* result is bogus if pos had overflowed, but it's best we can do */
		return result;
	}

	return PortalRunSelect(portal, forward, count, dest);
}

/*
 * DoPortalRewind - rewind a Portal to starting point
 */
static void
DoPortalRewind(Portal portal)
{
	if (portal->holdStore)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(portal->holdContext);
		tuplestore_rescan(portal->holdStore);
		MemoryContextSwitchTo(oldcontext);
	}
	if (PortalGetQueryDesc(portal))
		ExecutorRewind(PortalGetQueryDesc(portal));

	portal->atStart = true;
	portal->atEnd = false;
	portal->portalPos = 0;
	portal->posOverflow = false;
}
