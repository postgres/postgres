/*-------------------------------------------------------------------------
 *
 * pquery.c
 *	  POSTGRES process query command code
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/pquery.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "commands/prepare.h"
#include "executor/tstoreReceiver.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"


/*
 * ActivePortal is the currently executing Portal (the most closely nested,
 * if there are several).
 */
Portal		ActivePortal = NULL;


static void ProcessQuery(PlannedStmt *plan,
			 const char *sourceText,
			 ParamListInfo params,
			 DestReceiver *dest,
			 char *completionTag);
static void FillPortalStore(Portal portal, bool isTopLevel);
static uint32 RunFromStore(Portal portal, ScanDirection direction, long count,
			 DestReceiver *dest);
static long PortalRunSelect(Portal portal, bool forward, long count,
				DestReceiver *dest);
static void PortalRunUtility(Portal portal, Node *utilityStmt, bool isTopLevel,
				 DestReceiver *dest, char *completionTag);
static void PortalRunMulti(Portal portal, bool isTopLevel,
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
CreateQueryDesc(PlannedStmt *plannedstmt,
				const char *sourceText,
				Snapshot snapshot,
				Snapshot crosscheck_snapshot,
				DestReceiver *dest,
				ParamListInfo params,
				int instrument_options)
{
	QueryDesc  *qd = (QueryDesc *) palloc(sizeof(QueryDesc));

	qd->operation = plannedstmt->commandType;	/* operation */
	qd->plannedstmt = plannedstmt;		/* plan */
	qd->utilitystmt = plannedstmt->utilityStmt; /* in case DECLARE CURSOR */
	qd->sourceText = sourceText;	/* query text */
	qd->snapshot = RegisterSnapshot(snapshot);	/* snapshot */
	/* RI check snapshot */
	qd->crosscheck_snapshot = RegisterSnapshot(crosscheck_snapshot);
	qd->dest = dest;			/* output dest */
	qd->params = params;		/* parameter values passed into query */
	qd->instrument_options = instrument_options;		/* instrumentation
														 * wanted? */

	/* null these fields until set by ExecutorStart */
	qd->tupDesc = NULL;
	qd->estate = NULL;
	qd->planstate = NULL;
	qd->totaltime = NULL;

	return qd;
}

/*
 * CreateUtilityQueryDesc
 */
QueryDesc *
CreateUtilityQueryDesc(Node *utilitystmt,
					   const char *sourceText,
					   Snapshot snapshot,
					   DestReceiver *dest,
					   ParamListInfo params)
{
	QueryDesc  *qd = (QueryDesc *) palloc(sizeof(QueryDesc));

	qd->operation = CMD_UTILITY;	/* operation */
	qd->plannedstmt = NULL;
	qd->utilitystmt = utilitystmt;		/* utility command */
	qd->sourceText = sourceText;	/* query text */
	qd->snapshot = RegisterSnapshot(snapshot);	/* snapshot */
	qd->crosscheck_snapshot = InvalidSnapshot;	/* RI check snapshot */
	qd->dest = dest;			/* output dest */
	qd->params = params;		/* parameter values passed into query */
	qd->instrument_options = false;		/* uninteresting for utilities */

	/* null these fields until set by ExecutorStart */
	qd->tupDesc = NULL;
	qd->estate = NULL;
	qd->planstate = NULL;
	qd->totaltime = NULL;

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

	/* forget our snapshots */
	UnregisterSnapshot(qdesc->snapshot);
	UnregisterSnapshot(qdesc->crosscheck_snapshot);

	/* Only the QueryDesc itself need be freed */
	pfree(qdesc);
}


/*
 * ProcessQuery
 *		Execute a single plannable query within a PORTAL_MULTI_QUERY,
 *		PORTAL_ONE_RETURNING, or PORTAL_ONE_MOD_WITH portal
 *
 *	plan: the plan tree for the query
 *	sourceText: the source text of the query
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
static void
ProcessQuery(PlannedStmt *plan,
			 const char *sourceText,
			 ParamListInfo params,
			 DestReceiver *dest,
			 char *completionTag)
{
	QueryDesc  *queryDesc;

	elog(DEBUG3, "ProcessQuery");

	/*
	 * Create the QueryDesc object
	 */
	queryDesc = CreateQueryDesc(plan, sourceText,
								GetActiveSnapshot(), InvalidSnapshot,
								dest, params, 0);

	/*
	 * Call ExecutorStart to prepare the plan for execution
	 */
	ExecutorStart(queryDesc, 0);

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

		switch (queryDesc->operation)
		{
			case CMD_SELECT:
				snprintf(completionTag, COMPLETION_TAG_BUFSIZE,
						 "SELECT %u", queryDesc->estate->es_processed);
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
	ExecutorFinish(queryDesc);
	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);
}

/*
 * ChoosePortalStrategy
 *		Select portal execution strategy given the intended statement list.
 *
 * The list elements can be Querys, PlannedStmts, or utility statements.
 * That's more general than portals need, but plancache.c uses this too.
 *
 * See the comments in portal.h.
 */
PortalStrategy
ChoosePortalStrategy(List *stmts)
{
	int			nSetTag;
	ListCell   *lc;

	/*
	 * PORTAL_ONE_SELECT and PORTAL_UTIL_SELECT need only consider the
	 * single-statement case, since there are no rewrite rules that can add
	 * auxiliary queries to a SELECT or a utility command. PORTAL_ONE_MOD_WITH
	 * likewise allows only one top-level statement.
	 */
	if (list_length(stmts) == 1)
	{
		Node	   *stmt = (Node *) linitial(stmts);

		if (IsA(stmt, Query))
		{
			Query	   *query = (Query *) stmt;

			if (query->canSetTag)
			{
				if (query->commandType == CMD_SELECT &&
					query->utilityStmt == NULL)
				{
					if (query->hasModifyingCTE)
						return PORTAL_ONE_MOD_WITH;
					else
						return PORTAL_ONE_SELECT;
				}
				if (query->commandType == CMD_UTILITY &&
					query->utilityStmt != NULL)
				{
					if (UtilityReturnsTuples(query->utilityStmt))
						return PORTAL_UTIL_SELECT;
					/* it can't be ONE_RETURNING, so give up */
					return PORTAL_MULTI_QUERY;
				}
			}
		}
		else if (IsA(stmt, PlannedStmt))
		{
			PlannedStmt *pstmt = (PlannedStmt *) stmt;

			if (pstmt->canSetTag)
			{
				if (pstmt->commandType == CMD_SELECT &&
					pstmt->utilityStmt == NULL)
				{
					if (pstmt->hasModifyingCTE)
						return PORTAL_ONE_MOD_WITH;
					else
						return PORTAL_ONE_SELECT;
				}
			}
		}
		else
		{
			/* must be a utility command; assume it's canSetTag */
			if (UtilityReturnsTuples(stmt))
				return PORTAL_UTIL_SELECT;
			/* it can't be ONE_RETURNING, so give up */
			return PORTAL_MULTI_QUERY;
		}
	}

	/*
	 * PORTAL_ONE_RETURNING has to allow auxiliary queries added by rewrite.
	 * Choose PORTAL_ONE_RETURNING if there is exactly one canSetTag query and
	 * it has a RETURNING list.
	 */
	nSetTag = 0;
	foreach(lc, stmts)
	{
		Node	   *stmt = (Node *) lfirst(lc);

		if (IsA(stmt, Query))
		{
			Query	   *query = (Query *) stmt;

			if (query->canSetTag)
			{
				if (++nSetTag > 1)
					return PORTAL_MULTI_QUERY;	/* no need to look further */
				if (query->returningList == NIL)
					return PORTAL_MULTI_QUERY;	/* no need to look further */
			}
		}
		else if (IsA(stmt, PlannedStmt))
		{
			PlannedStmt *pstmt = (PlannedStmt *) stmt;

			if (pstmt->canSetTag)
			{
				if (++nSetTag > 1)
					return PORTAL_MULTI_QUERY;	/* no need to look further */
				if (!pstmt->hasReturning)
					return PORTAL_MULTI_QUERY;	/* no need to look further */
			}
		}
		/* otherwise, utility command, assumed not canSetTag */
	}
	if (nSetTag == 1)
		return PORTAL_ONE_RETURNING;

	/* Else, it's the general case... */
	return PORTAL_MULTI_QUERY;
}

/*
 * FetchPortalTargetList
 *		Given a portal that returns tuples, extract the query targetlist.
 *		Returns NIL if the portal doesn't have a determinable targetlist.
 *
 * Note: do not modify the result.
 */
List *
FetchPortalTargetList(Portal portal)
{
	/* no point in looking if we determined it doesn't return tuples */
	if (portal->strategy == PORTAL_MULTI_QUERY)
		return NIL;
	/* get the primary statement and find out what it returns */
	return FetchStatementTargetList(PortalGetPrimaryStmt(portal));
}

/*
 * FetchStatementTargetList
 *		Given a statement that returns tuples, extract the query targetlist.
 *		Returns NIL if the statement doesn't have a determinable targetlist.
 *
 * This can be applied to a Query, a PlannedStmt, or a utility statement.
 * That's more general than portals need, but plancache.c uses this too.
 *
 * Note: do not modify the result.
 *
 * XXX be careful to keep this in sync with UtilityReturnsTuples.
 */
List *
FetchStatementTargetList(Node *stmt)
{
	if (stmt == NULL)
		return NIL;
	if (IsA(stmt, Query))
	{
		Query	   *query = (Query *) stmt;

		if (query->commandType == CMD_UTILITY &&
			query->utilityStmt != NULL)
		{
			/* transfer attention to utility statement */
			stmt = query->utilityStmt;
		}
		else
		{
			if (query->commandType == CMD_SELECT &&
				query->utilityStmt == NULL)
				return query->targetList;
			if (query->returningList)
				return query->returningList;
			return NIL;
		}
	}
	if (IsA(stmt, PlannedStmt))
	{
		PlannedStmt *pstmt = (PlannedStmt *) stmt;

		if (pstmt->commandType == CMD_SELECT &&
			pstmt->utilityStmt == NULL)
			return pstmt->planTree->targetlist;
		if (pstmt->hasReturning)
			return pstmt->planTree->targetlist;
		return NIL;
	}
	if (IsA(stmt, FetchStmt))
	{
		FetchStmt  *fstmt = (FetchStmt *) stmt;
		Portal		subportal;

		Assert(!fstmt->ismove);
		subportal = GetPortalByName(fstmt->portalname);
		Assert(PortalIsValid(subportal));
		return FetchPortalTargetList(subportal);
	}
	if (IsA(stmt, ExecuteStmt))
	{
		ExecuteStmt *estmt = (ExecuteStmt *) stmt;
		PreparedStatement *entry;

		entry = FetchPreparedStatement(estmt->name, true);
		return FetchPreparedStatementTargetList(entry);
	}
	return NIL;
}

/*
 * PortalStart
 *		Prepare a portal for execution.
 *
 * Caller must already have created the portal, done PortalDefineQuery(),
 * and adjusted portal options if needed.
 *
 * If parameters are needed by the query, they must be passed in "params"
 * (caller is responsible for giving them appropriate lifetime).
 *
 * The caller can also provide an initial set of "eflags" to be passed to
 * ExecutorStart (but note these can be modified internally, and they are
 * currently only honored for PORTAL_ONE_SELECT portals).  Most callers
 * should simply pass zero.
 *
 * The caller can optionally pass a snapshot to be used; pass InvalidSnapshot
 * for the normal behavior of setting a new snapshot.  This parameter is
 * presently ignored for non-PORTAL_ONE_SELECT portals (it's only intended
 * to be used for cursors).
 *
 * On return, portal is ready to accept PortalRun() calls, and the result
 * tupdesc (if any) is known.
 */
void
PortalStart(Portal portal, ParamListInfo params,
			int eflags, Snapshot snapshot)
{
	Portal		saveActivePortal;
	ResourceOwner saveResourceOwner;
	MemoryContext savePortalContext;
	MemoryContext oldContext;
	QueryDesc  *queryDesc;
	int			myeflags;

	AssertArg(PortalIsValid(portal));
	AssertState(portal->status == PORTAL_DEFINED);

	/*
	 * Set up global portal context pointers.
	 */
	saveActivePortal = ActivePortal;
	saveResourceOwner = CurrentResourceOwner;
	savePortalContext = PortalContext;
	PG_TRY();
	{
		ActivePortal = portal;
		if (portal->resowner)
			CurrentResourceOwner = portal->resowner;
		PortalContext = PortalGetHeapMemory(portal);

		oldContext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));

		/* Must remember portal param list, if any */
		portal->portalParams = params;

		/*
		 * Determine the portal execution strategy
		 */
		portal->strategy = ChoosePortalStrategy(portal->stmts);

		/*
		 * Fire her up according to the strategy
		 */
		switch (portal->strategy)
		{
			case PORTAL_ONE_SELECT:

				/* Must set snapshot before starting executor. */
				if (snapshot)
					PushActiveSnapshot(snapshot);
				else
					PushActiveSnapshot(GetTransactionSnapshot());

				/*
				 * Create QueryDesc in portal's context; for the moment, set
				 * the destination to DestNone.
				 */
				queryDesc = CreateQueryDesc((PlannedStmt *) linitial(portal->stmts),
											portal->sourceText,
											GetActiveSnapshot(),
											InvalidSnapshot,
											None_Receiver,
											params,
											0);

				/*
				 * If it's a scrollable cursor, executor needs to support
				 * REWIND and backwards scan, as well as whatever the caller
				 * might've asked for.
				 */
				if (portal->cursorOptions & CURSOR_OPT_SCROLL)
					myeflags = eflags | EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD;
				else
					myeflags = eflags;

				/*
				 * Call ExecutorStart to prepare the plan for execution
				 */
				ExecutorStart(queryDesc, myeflags);

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
				portal->atEnd = false;	/* allow fetches */
				portal->portalPos = 0;
				portal->posOverflow = false;

				PopActiveSnapshot();
				break;

			case PORTAL_ONE_RETURNING:
			case PORTAL_ONE_MOD_WITH:

				/*
				 * We don't start the executor until we are told to run the
				 * portal.  We do need to set up the result tupdesc.
				 */
				{
					PlannedStmt *pstmt;

					pstmt = (PlannedStmt *) PortalGetPrimaryStmt(portal);
					Assert(IsA(pstmt, PlannedStmt));
					portal->tupDesc =
						ExecCleanTypeFromTL(pstmt->planTree->targetlist,
											false);
				}

				/*
				 * Reset cursor position data to "start of query"
				 */
				portal->atStart = true;
				portal->atEnd = false;	/* allow fetches */
				portal->portalPos = 0;
				portal->posOverflow = false;
				break;

			case PORTAL_UTIL_SELECT:

				/*
				 * We don't set snapshot here, because PortalRunUtility will
				 * take care of it if needed.
				 */
				{
					Node	   *ustmt = PortalGetPrimaryStmt(portal);

					Assert(!IsA(ustmt, PlannedStmt));
					portal->tupDesc = UtilityTupleDescriptor(ustmt);
				}

				/*
				 * Reset cursor position data to "start of query"
				 */
				portal->atStart = true;
				portal->atEnd = false;	/* allow fetches */
				portal->portalPos = 0;
				portal->posOverflow = false;
				break;

			case PORTAL_MULTI_QUERY:
				/* Need do nothing now */
				portal->tupDesc = NULL;
				break;
		}
	}
	PG_CATCH();
	{
		/* Uncaught error while executing portal: mark it dead */
		MarkPortalFailed(portal);

		/* Restore global vars and propagate error */
		ActivePortal = saveActivePortal;
		CurrentResourceOwner = saveResourceOwner;
		PortalContext = savePortalContext;

		PG_RE_THROW();
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldContext);

	ActivePortal = saveActivePortal;
	CurrentResourceOwner = saveResourceOwner;
	PortalContext = savePortalContext;

	portal->status = PORTAL_READY;
}

/*
 * PortalSetResultFormat
 *		Select the format codes for a portal's output.
 *
 * This must be run after PortalStart for a portal that will be read by
 * a DestRemote or DestRemoteExecute destination.  It is not presently needed
 * for other destination types.
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
	portal->formats = (int16 *)
		MemoryContextAlloc(PortalGetHeapMemory(portal),
						   natts * sizeof(int16));
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
 * isTopLevel: true if query is being executed at backend "top level"
 * (that is, directly from a client command message)
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
PortalRun(Portal portal, long count, bool isTopLevel,
		  DestReceiver *dest, DestReceiver *altdest,
		  char *completionTag)
{
	bool		result;
	uint32		nprocessed;
	ResourceOwner saveTopTransactionResourceOwner;
	MemoryContext saveTopTransactionContext;
	Portal		saveActivePortal;
	ResourceOwner saveResourceOwner;
	MemoryContext savePortalContext;
	MemoryContext saveMemoryContext;

	AssertArg(PortalIsValid(portal));

	TRACE_POSTGRESQL_QUERY_EXECUTE_START();

	/* Initialize completion tag to empty string */
	if (completionTag)
		completionTag[0] = '\0';

	if (log_executor_stats && portal->strategy != PORTAL_MULTI_QUERY)
	{
		elog(DEBUG3, "PortalRun");
		/* PORTAL_MULTI_QUERY logs its own stats per query */
		ResetUsage();
	}

	/*
	 * Check for improper portal use, and mark portal active.
	 */
	MarkPortalActive(portal);

	/*
	 * Set up global portal context pointers.
	 *
	 * We have to play a special game here to support utility commands like
	 * VACUUM and CLUSTER, which internally start and commit transactions.
	 * When we are called to execute such a command, CurrentResourceOwner will
	 * be pointing to the TopTransactionResourceOwner --- which will be
	 * destroyed and replaced in the course of the internal commit and
	 * restart.  So we need to be prepared to restore it as pointing to the
	 * exit-time TopTransactionResourceOwner.  (Ain't that ugly?  This idea of
	 * internally starting whole new transactions is not good.)
	 * CurrentMemoryContext has a similar problem, but the other pointers we
	 * save here will be NULL or pointing to longer-lived objects.
	 */
	saveTopTransactionResourceOwner = TopTransactionResourceOwner;
	saveTopTransactionContext = TopTransactionContext;
	saveActivePortal = ActivePortal;
	saveResourceOwner = CurrentResourceOwner;
	savePortalContext = PortalContext;
	saveMemoryContext = CurrentMemoryContext;
	PG_TRY();
	{
		ActivePortal = portal;
		if (portal->resowner)
			CurrentResourceOwner = portal->resowner;
		PortalContext = PortalGetHeapMemory(portal);

		MemoryContextSwitchTo(PortalContext);

		switch (portal->strategy)
		{
			case PORTAL_ONE_SELECT:
			case PORTAL_ONE_RETURNING:
			case PORTAL_ONE_MOD_WITH:
			case PORTAL_UTIL_SELECT:

				/*
				 * If we have not yet run the command, do so, storing its
				 * results in the portal's tuplestore.  But we don't do that
				 * for the PORTAL_ONE_SELECT case.
				 */
				if (portal->strategy != PORTAL_ONE_SELECT && !portal->holdStore)
					FillPortalStore(portal, isTopLevel);

				/*
				 * Now fetch desired portion of results.
				 */
				nprocessed = PortalRunSelect(portal, true, count, dest);

				/*
				 * If the portal result contains a command tag and the caller
				 * gave us a pointer to store it, copy it. Patch the "SELECT"
				 * tag to also provide the rowcount.
				 */
				if (completionTag && portal->commandTag)
				{
					if (strcmp(portal->commandTag, "SELECT") == 0)
						snprintf(completionTag, COMPLETION_TAG_BUFSIZE,
								 "SELECT %u", nprocessed);
					else
						strcpy(completionTag, portal->commandTag);
				}

				/* Mark portal not active */
				portal->status = PORTAL_READY;

				/*
				 * Since it's a forward fetch, say DONE iff atEnd is now true.
				 */
				result = portal->atEnd;
				break;

			case PORTAL_MULTI_QUERY:
				PortalRunMulti(portal, isTopLevel,
							   dest, altdest, completionTag);

				/* Prevent portal's commands from being re-executed */
				MarkPortalDone(portal);

				/* Always complete at end of RunMulti */
				result = true;
				break;

			default:
				elog(ERROR, "unrecognized portal strategy: %d",
					 (int) portal->strategy);
				result = false; /* keep compiler quiet */
				break;
		}
	}
	PG_CATCH();
	{
		/* Uncaught error while executing portal: mark it dead */
		MarkPortalFailed(portal);

		/* Restore global vars and propagate error */
		if (saveMemoryContext == saveTopTransactionContext)
			MemoryContextSwitchTo(TopTransactionContext);
		else
			MemoryContextSwitchTo(saveMemoryContext);
		ActivePortal = saveActivePortal;
		if (saveResourceOwner == saveTopTransactionResourceOwner)
			CurrentResourceOwner = TopTransactionResourceOwner;
		else
			CurrentResourceOwner = saveResourceOwner;
		PortalContext = savePortalContext;

		PG_RE_THROW();
	}
	PG_END_TRY();

	if (saveMemoryContext == saveTopTransactionContext)
		MemoryContextSwitchTo(TopTransactionContext);
	else
		MemoryContextSwitchTo(saveMemoryContext);
	ActivePortal = saveActivePortal;
	if (saveResourceOwner == saveTopTransactionResourceOwner)
		CurrentResourceOwner = TopTransactionResourceOwner;
	else
		CurrentResourceOwner = saveResourceOwner;
	PortalContext = savePortalContext;

	if (log_executor_stats && portal->strategy != PORTAL_MULTI_QUERY)
		ShowUsage("EXECUTOR STATISTICS");

	TRACE_POSTGRESQL_QUERY_EXECUTE_DONE();

	return result;
}

/*
 * PortalRunSelect
 *		Execute a portal's query in PORTAL_ONE_SELECT mode, and also
 *		when fetching from a completed holdStore in PORTAL_ONE_RETURNING,
 *		PORTAL_ONE_MOD_WITH, and PORTAL_UTIL_SELECT cases.
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
	 * NB: queryDesc will be NULL if we are fetching from a held cursor or a
	 * completed utility query; can't use it in that path.
	 */
	queryDesc = PortalGetQueryDesc(portal);

	/* Caller messed up if we have neither a ready query nor held data. */
	Assert(queryDesc || portal->holdStore);

	/*
	 * Force the queryDesc destination to the right thing.  This supports
	 * MOVE, for example, which will pass in dest = DestNone.  This is okay to
	 * change as long as we do it on every fetch.  (The Executor must not
	 * assume that dest never changes.)
	 */
	if (queryDesc)
		queryDesc->dest = dest;

	/*
	 * Determine which direction to go in, and check to see if we're already
	 * at the end of the available tuples in that direction.  If so, set the
	 * direction to NoMovement to avoid trying to fetch any tuples.  (This
	 * check exists because not all plan node types are robust about being
	 * called again if they've already returned NULL once.)  Then call the
	 * executor (we must not skip this, because the destination needs to see a
	 * setup and shutdown even if no tuples are available).  Finally, update
	 * the portal position state depending on the number of tuples that were
	 * retrieved.
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
			PushActiveSnapshot(queryDesc->snapshot);
			ExecutorRun(queryDesc, direction, count);
			nprocessed = queryDesc->estate->es_processed;
			PopActiveSnapshot();
		}

		if (!ScanDirectionIsNoMovement(direction))
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
			PushActiveSnapshot(queryDesc->snapshot);
			ExecutorRun(queryDesc, direction, count);
			nprocessed = queryDesc->estate->es_processed;
			PopActiveSnapshot();
		}

		if (!ScanDirectionIsNoMovement(direction))
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
 * FillPortalStore
 *		Run the query and load result tuples into the portal's tuple store.
 *
 * This is used for PORTAL_ONE_RETURNING, PORTAL_ONE_MOD_WITH, and
 * PORTAL_UTIL_SELECT cases only.
 */
static void
FillPortalStore(Portal portal, bool isTopLevel)
{
	DestReceiver *treceiver;
	char		completionTag[COMPLETION_TAG_BUFSIZE];

	PortalCreateHoldStore(portal);
	treceiver = CreateDestReceiver(DestTuplestore);
	SetTuplestoreDestReceiverParams(treceiver,
									portal->holdStore,
									portal->holdContext,
									false);

	completionTag[0] = '\0';

	switch (portal->strategy)
	{
		case PORTAL_ONE_RETURNING:
		case PORTAL_ONE_MOD_WITH:

			/*
			 * Run the portal to completion just as for the default
			 * MULTI_QUERY case, but send the primary query's output to the
			 * tuplestore. Auxiliary query outputs are discarded.
			 */
			PortalRunMulti(portal, isTopLevel,
						   treceiver, None_Receiver, completionTag);
			break;

		case PORTAL_UTIL_SELECT:
			PortalRunUtility(portal, (Node *) linitial(portal->stmts),
							 isTopLevel, treceiver, completionTag);
			break;

		default:
			elog(ERROR, "unsupported portal strategy: %d",
				 (int) portal->strategy);
			break;
	}

	/* Override default completion tag with actual command result */
	if (completionTag[0] != '\0')
		portal->commandTag = pstrdup(completionTag);

	(*treceiver->rDestroy) (treceiver);
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
	TupleTableSlot *slot;

	slot = MakeSingleTupleTableSlot(portal->tupDesc);

	(*dest->rStartup) (dest, CMD_SELECT, portal->tupDesc);

	if (ScanDirectionIsNoMovement(direction))
	{
		/* do nothing except start/stop the destination */
	}
	else
	{
		bool		forward = ScanDirectionIsForward(direction);

		for (;;)
		{
			MemoryContext oldcontext;
			bool		ok;

			oldcontext = MemoryContextSwitchTo(portal->holdContext);

			ok = tuplestore_gettupleslot(portal->holdStore, forward, false,
										 slot);

			MemoryContextSwitchTo(oldcontext);

			if (!ok)
				break;

			(*dest->receiveSlot) (slot, dest);

			ExecClearTuple(slot);

			/*
			 * check our tuple count.. if we've processed the proper number
			 * then quit, else loop again and process more tuples. Zero count
			 * means no limit.
			 */
			current_tuple_count++;
			if (count && count == current_tuple_count)
				break;
		}
	}

	(*dest->rShutdown) (dest);

	ExecDropSingleTupleTableSlot(slot);

	return (uint32) current_tuple_count;
}

/*
 * PortalRunUtility
 *		Execute a utility statement inside a portal.
 */
static void
PortalRunUtility(Portal portal, Node *utilityStmt, bool isTopLevel,
				 DestReceiver *dest, char *completionTag)
{
	bool		active_snapshot_set;

	elog(DEBUG3, "ProcessUtility");

	/*
	 * Set snapshot if utility stmt needs one.  Most reliable way to do this
	 * seems to be to enumerate those that do not need one; this is a short
	 * list.  Transaction control, LOCK, and SET must *not* set a snapshot
	 * since they need to be executable at the start of a transaction-snapshot
	 * mode transaction without freezing a snapshot.  By extension we allow
	 * SHOW not to set a snapshot.  The other stmts listed are just efficiency
	 * hacks.  Beware of listing anything that can modify the database --- if,
	 * say, it has to update an index with expressions that invoke
	 * user-defined functions, then it had better have a snapshot.
	 */
	if (!(IsA(utilityStmt, TransactionStmt) ||
		  IsA(utilityStmt, LockStmt) ||
		  IsA(utilityStmt, VariableSetStmt) ||
		  IsA(utilityStmt, VariableShowStmt) ||
		  IsA(utilityStmt, ConstraintsSetStmt) ||
	/* efficiency hacks from here down */
		  IsA(utilityStmt, FetchStmt) ||
		  IsA(utilityStmt, ListenStmt) ||
		  IsA(utilityStmt, NotifyStmt) ||
		  IsA(utilityStmt, UnlistenStmt) ||
		  IsA(utilityStmt, CheckPointStmt)))
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		active_snapshot_set = true;
	}
	else
		active_snapshot_set = false;

	ProcessUtility(utilityStmt,
				   portal->sourceText,
			   isTopLevel ? PROCESS_UTILITY_TOPLEVEL : PROCESS_UTILITY_QUERY,
				   portal->portalParams,
				   dest,
				   completionTag);

	/* Some utility statements may change context on us */
	MemoryContextSwitchTo(PortalGetHeapMemory(portal));

	/*
	 * Some utility commands may pop the ActiveSnapshot stack from under us,
	 * so we only pop the stack if we actually see a snapshot set.  Note that
	 * the set of utility commands that do this must be the same set
	 * disallowed to run inside a transaction; otherwise, we could be popping
	 * a snapshot that belongs to some other operation.
	 */
	if (active_snapshot_set && ActiveSnapshotSet())
		PopActiveSnapshot();
}

/*
 * PortalRunMulti
 *		Execute a portal's queries in the general case (multi queries
 *		or non-SELECT-like queries)
 */
static void
PortalRunMulti(Portal portal, bool isTopLevel,
			   DestReceiver *dest, DestReceiver *altdest,
			   char *completionTag)
{
	bool		active_snapshot_set = false;
	ListCell   *stmtlist_item;

	/*
	 * If the destination is DestRemoteExecute, change to DestNone.  The
	 * reason is that the client won't be expecting any tuples, and indeed has
	 * no way to know what they are, since there is no provision for Describe
	 * to send a RowDescription message when this portal execution strategy is
	 * in effect.  This presently will only affect SELECT commands added to
	 * non-SELECT queries by rewrite rules: such commands will be executed,
	 * but the results will be discarded unless you use "simple Query"
	 * protocol.
	 */
	if (dest->mydest == DestRemoteExecute)
		dest = None_Receiver;
	if (altdest->mydest == DestRemoteExecute)
		altdest = None_Receiver;

	/*
	 * Loop to handle the individual queries generated from a single parsetree
	 * by analysis and rewrite.
	 */
	foreach(stmtlist_item, portal->stmts)
	{
		Node	   *stmt = (Node *) lfirst(stmtlist_item);

		/*
		 * If we got a cancel signal in prior command, quit
		 */
		CHECK_FOR_INTERRUPTS();

		if (IsA(stmt, PlannedStmt) &&
			((PlannedStmt *) stmt)->utilityStmt == NULL)
		{
			/*
			 * process a plannable query.
			 */
			PlannedStmt *pstmt = (PlannedStmt *) stmt;

			TRACE_POSTGRESQL_QUERY_EXECUTE_START();

			if (log_executor_stats)
				ResetUsage();

			/*
			 * Must always have a snapshot for plannable queries.  First time
			 * through, take a new snapshot; for subsequent queries in the
			 * same portal, just update the snapshot's copy of the command
			 * counter.
			 */
			if (!active_snapshot_set)
			{
				PushActiveSnapshot(GetTransactionSnapshot());
				active_snapshot_set = true;
			}
			else
				UpdateActiveSnapshotCommandId();

			if (pstmt->canSetTag)
			{
				/* statement can set tag string */
				ProcessQuery(pstmt,
							 portal->sourceText,
							 portal->portalParams,
							 dest, completionTag);
			}
			else
			{
				/* stmt added by rewrite cannot set tag */
				ProcessQuery(pstmt,
							 portal->sourceText,
							 portal->portalParams,
							 altdest, NULL);
			}

			if (log_executor_stats)
				ShowUsage("EXECUTOR STATISTICS");

			TRACE_POSTGRESQL_QUERY_EXECUTE_DONE();
		}
		else
		{
			/*
			 * process utility functions (create, destroy, etc..)
			 *
			 * These are assumed canSetTag if they're the only stmt in the
			 * portal.
			 *
			 * We must not set a snapshot here for utility commands (if one is
			 * needed, PortalRunUtility will do it).  If a utility command is
			 * alone in a portal then everything's fine.  The only case where
			 * a utility command can be part of a longer list is that rules
			 * are allowed to include NotifyStmt.  NotifyStmt doesn't care
			 * whether it has a snapshot or not, so we just leave the current
			 * snapshot alone if we have one.
			 */
			if (list_length(portal->stmts) == 1)
			{
				Assert(!active_snapshot_set);
				/* statement can set tag string */
				PortalRunUtility(portal, stmt, isTopLevel,
								 dest, completionTag);
			}
			else
			{
				Assert(IsA(stmt, NotifyStmt));
				/* stmt added by rewrite cannot set tag */
				PortalRunUtility(portal, stmt, isTopLevel,
								 altdest, NULL);
			}
		}

		/*
		 * Increment command counter between queries, but not after the last
		 * one.
		 */
		if (lnext(stmtlist_item) != NULL)
			CommandCounterIncrement();

		/*
		 * Clear subsidiary contexts to recover temporary memory.
		 */
		Assert(PortalGetHeapMemory(portal) == CurrentMemoryContext);

		MemoryContextDeleteChildren(PortalGetHeapMemory(portal));
	}

	/* Pop the snapshot if we pushed one. */
	if (active_snapshot_set)
		PopActiveSnapshot();

	/*
	 * If a command completion tag was supplied, use it.  Otherwise use the
	 * portal's commandTag as the default completion tag.
	 *
	 * Exception: Clients expect INSERT/UPDATE/DELETE tags to have counts, so
	 * fake them with zeros.  This can happen with DO INSTEAD rules if there
	 * is no replacement query of the same type as the original.  We print "0
	 * 0" here because technically there is no query of the matching tag type,
	 * and printing a non-zero count for a different query type seems wrong,
	 * e.g.  an INSERT that does an UPDATE instead should not print "0 1" if
	 * one row was updated.  See QueryRewrite(), step 3, for details.
	 */
	if (completionTag && completionTag[0] == '\0')
	{
		if (portal->commandTag)
			strcpy(completionTag, portal->commandTag);
		if (strcmp(completionTag, "SELECT") == 0)
			sprintf(completionTag, "SELECT 0 0");
		else if (strcmp(completionTag, "INSERT") == 0)
			strcpy(completionTag, "INSERT 0 0");
		else if (strcmp(completionTag, "UPDATE") == 0)
			strcpy(completionTag, "UPDATE 0");
		else if (strcmp(completionTag, "DELETE") == 0)
			strcpy(completionTag, "DELETE 0");
	}
}

/*
 * PortalRunFetch
 *		Variant form of PortalRun that supports SQL FETCH directions.
 *
 * Note: we presently assume that no callers of this want isTopLevel = true.
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
	Portal		saveActivePortal;
	ResourceOwner saveResourceOwner;
	MemoryContext savePortalContext;
	MemoryContext oldContext;

	AssertArg(PortalIsValid(portal));

	/*
	 * Check for improper portal use, and mark portal active.
	 */
	MarkPortalActive(portal);

	/*
	 * Set up global portal context pointers.
	 */
	saveActivePortal = ActivePortal;
	saveResourceOwner = CurrentResourceOwner;
	savePortalContext = PortalContext;
	PG_TRY();
	{
		ActivePortal = portal;
		if (portal->resowner)
			CurrentResourceOwner = portal->resowner;
		PortalContext = PortalGetHeapMemory(portal);

		oldContext = MemoryContextSwitchTo(PortalContext);

		switch (portal->strategy)
		{
			case PORTAL_ONE_SELECT:
				result = DoPortalRunFetch(portal, fdirection, count, dest);
				break;

			case PORTAL_ONE_RETURNING:
			case PORTAL_ONE_MOD_WITH:
			case PORTAL_UTIL_SELECT:

				/*
				 * If we have not yet run the command, do so, storing its
				 * results in the portal's tuplestore.
				 */
				if (!portal->holdStore)
					FillPortalStore(portal, false /* isTopLevel */ );

				/*
				 * Now fetch desired portion of results.
				 */
				result = DoPortalRunFetch(portal, fdirection, count, dest);
				break;

			default:
				elog(ERROR, "unsupported portal strategy");
				result = 0;		/* keep compiler quiet */
				break;
		}
	}
	PG_CATCH();
	{
		/* Uncaught error while executing portal: mark it dead */
		MarkPortalFailed(portal);

		/* Restore global vars and propagate error */
		ActivePortal = saveActivePortal;
		CurrentResourceOwner = saveResourceOwner;
		PortalContext = savePortalContext;

		PG_RE_THROW();
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldContext);

	/* Mark portal not active */
	portal->status = PORTAL_READY;

	ActivePortal = saveActivePortal;
	CurrentResourceOwner = saveResourceOwner;
	PortalContext = savePortalContext;

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

	Assert(portal->strategy == PORTAL_ONE_SELECT ||
		   portal->strategy == PORTAL_ONE_RETURNING ||
		   portal->strategy == PORTAL_ONE_MOD_WITH ||
		   portal->strategy == PORTAL_UTIL_SELECT);

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
				 * Definition: Rewind to start, advance count-1 rows, return
				 * next row (if any).  In practice, if the goal is less than
				 * halfway back to the start, it's better to scan from where
				 * we are.  In any case, we arrange to fetch the target row
				 * going forwards.
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
				 * return prior row (if any).  We could optimize this if we
				 * knew in advance where the end was, but typically we won't.
				 * (Is it worth considering case where count > half of size of
				 * query?  We could rewind once we know the size ...)
				 */
				PortalRunSelect(portal, true, FETCH_ALL, None_Receiver);
				if (count < -1)
					PortalRunSelect(portal, false, -count - 1, None_Receiver);
				return PortalRunSelect(portal, false, 1L, dest);
			}
			else
			{
				/* count == 0 */
				/* Rewind to start, return zero rows */
				DoPortalRewind(portal);
				return PortalRunSelect(portal, true, 0L, dest);
			}
			break;
		case FETCH_RELATIVE:
			if (count > 0)
			{
				/*
				 * Definition: advance count-1 rows, return next row (if any).
				 */
				if (count > 1)
					PortalRunSelect(portal, true, count - 1, None_Receiver);
				return PortalRunSelect(portal, true, 1L, dest);
			}
			else if (count < 0)
			{
				/*
				 * Definition: back up abs(count)-1 rows, return prior row (if
				 * any).
				 */
				if (count < -1)
					PortalRunSelect(portal, false, -count - 1, None_Receiver);
				return PortalRunSelect(portal, false, 1L, dest);
			}
			else
			{
				/* count == 0 */
				/* Same as FETCH FORWARD 0, so fall out of switch */
				fdirection = FETCH_FORWARD;
			}
			break;
		default:
			elog(ERROR, "bogus direction");
			break;
	}

	/*
	 * Get here with fdirection == FETCH_FORWARD or FETCH_BACKWARD, and count
	 * >= 0.
	 */
	forward = (fdirection == FETCH_FORWARD);

	/*
	 * Zero count means to re-fetch the current row, if any (per SQL)
	 */
	if (count == 0)
	{
		bool		on_row;

		/* Are we sitting on a row? */
		on_row = (!portal->atStart && !portal->atEnd);

		if (dest->mydest == DestNone)
		{
			/* MOVE 0 returns 0/1 based on if FETCH 0 would return a row */
			return on_row ? 1L : 0L;
		}
		else
		{
			/*
			 * If we are sitting on a row, back up one so we can re-fetch it.
			 * If we are not sitting on a row, we still have to start up and
			 * shut down the executor so that the destination is initialized
			 * and shut down correctly; so keep going.  To PortalRunSelect,
			 * count == 0 means we will retrieve no row.
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
	if (!forward && count == FETCH_ALL && dest->mydest == DestNone)
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
	QueryDesc  *queryDesc;

	/* Rewind holdStore, if we have one */
	if (portal->holdStore)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(portal->holdContext);
		tuplestore_rescan(portal->holdStore);
		MemoryContextSwitchTo(oldcontext);
	}

	/* Rewind executor, if active */
	queryDesc = PortalGetQueryDesc(portal);
	if (queryDesc)
	{
		PushActiveSnapshot(queryDesc->snapshot);
		ExecutorRewind(queryDesc);
		PopActiveSnapshot();
	}

	portal->atStart = true;
	portal->atEnd = false;
	portal->portalPos = 0;
	portal->posOverflow = false;
}
