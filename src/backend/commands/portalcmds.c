/*-------------------------------------------------------------------------
 *
 * portalcmds.c
 *	  portal support code
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/portalcmds.c,v 1.11 2003/03/27 16:51:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "miscadmin.h"
#include "commands/portalcmds.h"
#include "executor/executor.h"
#include "optimizer/planner.h"
#include "rewrite/rewriteHandler.h"
#include "utils/memutils.h"

static long DoRelativeFetch(Portal portal,
							bool forward,
							long count,
							CommandDest dest);
static long DoRelativeStoreFetch(Portal portal,
								 bool forward,
								 long count,
								 CommandDest dest);
static void DoPortalRewind(Portal portal);
static Portal PreparePortal(DeclareCursorStmt *stmt);


/*
 * PerformCursorOpen
 *		Execute SQL DECLARE CURSOR command.
 */
void
PerformCursorOpen(DeclareCursorStmt *stmt, CommandDest dest)
{
	List	   *rewritten;
	Query	   *query;
	Plan	   *plan;
	Portal		portal;
	MemoryContext oldContext;
	char	   *cursorName;
	QueryDesc  *queryDesc;

	/*
	 * If this is a non-holdable cursor, we ensure that this statement
	 * has been executed inside a transaction block (or else, it would
	 * have no user-visible effect).
	 *
	 * XXX: surely there is a better way to check this?
	 */
	if (!(stmt->options & CURSOR_OPT_HOLD))
		RequireTransactionChain((void *) stmt, "DECLARE CURSOR");

	/*
	 * The query has been through parse analysis, but not rewriting or
	 * planning as yet.  Note that the grammar ensured we have a SELECT
	 * query, so we are not expecting rule rewriting to do anything strange.
	 */
	rewritten = QueryRewrite((Query *) stmt->query);
	if (length(rewritten) != 1 || !IsA(lfirst(rewritten), Query))
		elog(ERROR, "PerformCursorOpen: unexpected rewrite result");
	query = (Query *) lfirst(rewritten);
	if (query->commandType != CMD_SELECT)
		elog(ERROR, "PerformCursorOpen: unexpected rewrite result");

	if (query->into)
		elog(ERROR, "DECLARE CURSOR may not specify INTO");
	if (query->rowMarks != NIL)
		elog(ERROR, "DECLARE/UPDATE is not supported"
			 "\n\tCursors must be READ ONLY");

	plan = planner(query, true, stmt->options);

	/* If binary cursor, switch to alternate output format */
	if ((stmt->options & CURSOR_OPT_BINARY) && dest == Remote)
		dest = RemoteInternal;

	/*
	 * Create a portal and copy the query and plan into its memory context.
	 */
	portal = PreparePortal(stmt);

	oldContext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));
	query = copyObject(query);
	plan = copyObject(plan);

	/*
	 * Create the QueryDesc object in the portal context, too.
	 */
	cursorName = pstrdup(stmt->portalname);
	queryDesc = CreateQueryDesc(query, plan, dest, cursorName, NULL, false);

	/*
	 * call ExecStart to prepare the plan for execution
	 */
	ExecutorStart(queryDesc);

	/* Arrange to shut down the executor if portal is dropped */
	PortalSetQuery(portal, queryDesc, PortalCleanup);

	/*
	 * We're done; the query won't actually be run until PerformPortalFetch
	 * is called.
	 */
	MemoryContextSwitchTo(oldContext);
}

/*
 * PerformPortalFetch
 *		Execute SQL FETCH or MOVE command.
 *
 *	stmt: parsetree node for command
 *	dest: where to send results
 *	completionTag: points to a buffer of size COMPLETION_TAG_BUFSIZE
 *		in which to store a command completion status string.
 *
 * completionTag may be NULL if caller doesn't want a status string.
 */
void
PerformPortalFetch(FetchStmt *stmt,
				   CommandDest dest,
				   char *completionTag)
{
	Portal		portal;
	long		nprocessed;

	/* initialize completion status in case of early exit */
	if (completionTag)
		strcpy(completionTag, stmt->ismove ? "MOVE 0" : "FETCH 0");

	/* get the portal from the portal name */
	portal = GetPortalByName(stmt->portalname);
	if (!PortalIsValid(portal))
	{
		/* FIXME: shouldn't this be an ERROR? */
		elog(WARNING, "PerformPortalFetch: portal \"%s\" not found",
			 stmt->portalname);
		return;
	}

	/* Do it */
	nprocessed = DoPortalFetch(portal,
							   stmt->direction,
							   stmt->howMany,
							   stmt->ismove ? None : dest);

	/* Return command status if wanted */
	if (completionTag)
		snprintf(completionTag, COMPLETION_TAG_BUFSIZE, "%s %ld",
				 stmt->ismove ? "MOVE" : "FETCH",
				 nprocessed);
}

/*
 * DoPortalFetch
 *		Guts of PerformPortalFetch --- shared with SPI cursor operations.
 *		Caller must already have validated the Portal.
 *
 * Returns number of rows processed (suitable for use in result tag)
 */
long
DoPortalFetch(Portal portal,
			  FetchDirection fdirection,
			  long count,
			  CommandDest dest)
{
	bool		forward;

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
					count-1 <= portal->portalPos / 2)
				{
					DoPortalRewind(portal);
					if (count > 1)
						DoRelativeFetch(portal, true, count-1, None);
				}
				else
				{
					long		pos = portal->portalPos;

					if (portal->atEnd)
						pos++;	/* need one extra fetch if off end */
					if (count <= pos)
						DoRelativeFetch(portal, false, pos-count+1, None);
					else if (count > pos+1)
						DoRelativeFetch(portal, true, count-pos-1, None);
				}
				return DoRelativeFetch(portal, true, 1L, dest);
			}
			else if (count < 0)
			{
				/*
				 * Definition: Advance to end, back up abs(count)-1 rows,
				 * return prior row (if any).  We could optimize this if we
				 * knew in advance where the end was, but typically we won't.
				 * (Is it worth considering case where count > half of size
				 * of query?  We could rewind once we know the size ...)
				 */
				DoRelativeFetch(portal, true, FETCH_ALL, None);
				if (count < -1)
					DoRelativeFetch(portal, false, -count-1, None);
				return DoRelativeFetch(portal, false, 1L, dest);
			}
			else /* count == 0 */
			{
				/* Rewind to start, return zero rows */
				DoPortalRewind(portal);
				return DoRelativeFetch(portal, true, 0L, dest);
			}
			break;
		case FETCH_RELATIVE:
			if (count > 0)
			{
				/*
				 * Definition: advance count-1 rows, return next row (if any).
				 */
				if (count > 1)
					DoRelativeFetch(portal, true, count-1, None);
				return DoRelativeFetch(portal, true, 1L, dest);
			}
			else if (count < 0)
			{
				/*
				 * Definition: back up abs(count)-1 rows, return prior row
				 * (if any).
				 */
				if (count < -1)
					DoRelativeFetch(portal, false, -count-1, None);
				return DoRelativeFetch(portal, false, 1L, dest);
			}
			else /* count == 0 */
			{
				/* Same as FETCH FORWARD 0, so fall out of switch */
				fdirection = FETCH_FORWARD;
			}
			break;
		default:
			elog(ERROR, "DoPortalFetch: bogus direction");
			break;
	}

	/*
	 * Get here with fdirection == FETCH_FORWARD or FETCH_BACKWARD,
	 * and count >= 0.
	 */
	forward = (fdirection == FETCH_FORWARD);

	/*
	 * Zero count means to re-fetch the current row, if any (per SQL92)
	 */
	if (count == 0)
	{
		bool	on_row;

		/* Are we sitting on a row? */
		on_row = (!portal->atStart && !portal->atEnd);

		if (dest == None)
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
			 * and shut down correctly; so keep going.  To DoRelativeFetch,
			 * count == 0 means we will retrieve no row.
			 */
			if (on_row)
			{
				DoRelativeFetch(portal, false, 1L, None);
				/* Set up to fetch one row forward */
				count = 1;
				forward = true;
			}
		}
	}

	/*
	 * Optimize MOVE BACKWARD ALL into a Rewind.
	 */
	if (!forward && count == FETCH_ALL && dest == None)
	{
		long	result = portal->portalPos;

		if (result > 0 && !portal->atEnd)
			result--;
		DoPortalRewind(portal);
		/* result is bogus if pos had overflowed, but it's best we can do */
		return result;
	}

	return DoRelativeFetch(portal, forward, count, dest);
}

/*
 * DoRelativeFetch
 *		Do fetch for a simple N-rows-forward-or-backward case.
 *
 * count <= 0 is interpreted as a no-op: the destination gets started up
 * and shut down, but nothing else happens.  Also, count == FETCH_ALL is
 * interpreted as "all rows".
 *
 * Caller must already have validated the Portal.
 *
 * Returns number of rows processed (suitable for use in result tag)
 */
static long
DoRelativeFetch(Portal portal,
				bool forward,
				long count,
				CommandDest dest)
{
	QueryDesc  *queryDesc;
	EState	   *estate;
	ScanDirection direction;
	QueryDesc	temp_queryDesc;

	if (portal->holdStore)
		return DoRelativeStoreFetch(portal, forward, count, dest);

	queryDesc = PortalGetQueryDesc(portal);
	estate = queryDesc->estate;

	/*
	 * If the requested destination is not the same as the query's
	 * original destination, make a temporary QueryDesc with the proper
	 * destination.  This supports MOVE, for example, which will pass in
	 * dest = None.
	 *
	 * EXCEPTION: if the query's original dest is RemoteInternal (ie, it's a
	 * binary cursor) and the request is Remote, we do NOT override the
	 * original dest.  This is necessary since a FETCH command will pass
	 * dest = Remote, not knowing whether the cursor is binary or not.
	 */
	if (dest != queryDesc->dest &&
		!(queryDesc->dest == RemoteInternal && dest == Remote))
	{
		memcpy(&temp_queryDesc, queryDesc, sizeof(QueryDesc));
		temp_queryDesc.dest = dest;
		queryDesc = &temp_queryDesc;
	}

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

		ExecutorRun(queryDesc, direction, count);

		if (direction != NoMovementScanDirection)
		{
			long	oldPos;

			if (estate->es_processed > 0)
				portal->atStart = false;	/* OK to go backward now */
			if (count == 0 ||
				(unsigned long) estate->es_processed < (unsigned long) count)
				portal->atEnd = true;		/* we retrieved 'em all */
			oldPos = portal->portalPos;
			portal->portalPos += estate->es_processed;
			/* portalPos doesn't advance when we fall off the end */
			if (portal->portalPos < oldPos)
				portal->posOverflow = true;
		}
	}
	else
	{
		if (portal->scrollType == DISABLE_SCROLL)
			elog(ERROR, "Cursor can only scan forward"
				 "\n\tDeclare it with SCROLL option to enable backward scan");

		if (portal->atStart || count <= 0)
			direction = NoMovementScanDirection;
		else
			direction = BackwardScanDirection;

		/* In the executor, zero count processes all rows */
		if (count == FETCH_ALL)
			count = 0;

		ExecutorRun(queryDesc, direction, count);

		if (direction != NoMovementScanDirection)
		{
			if (estate->es_processed > 0 && portal->atEnd)
			{
				portal->atEnd = false;		/* OK to go forward now */
				portal->portalPos++;		/* adjust for endpoint case */
			}
			if (count == 0 ||
				(unsigned long) estate->es_processed < (unsigned long) count)
			{
				portal->atStart = true;		/* we retrieved 'em all */
				portal->portalPos = 0;
				portal->posOverflow = false;
			}
			else
			{
				long	oldPos;

				oldPos = portal->portalPos;
				portal->portalPos -= estate->es_processed;
				if (portal->portalPos > oldPos ||
					portal->portalPos <= 0)
					portal->posOverflow = true;
			}
		}
	}

	return estate->es_processed;
}

/*
 * DoRelativeStoreFetch
 *		Do fetch for a simple N-rows-forward-or-backward case, getting
 *		the results from the portal's tuple store.
 */
static long
DoRelativeStoreFetch(Portal portal,
					 bool forward,
					 long count,
					 CommandDest dest)
{
	DestReceiver *destfunc;
	QueryDesc *queryDesc = portal->queryDesc;
	long rows_fetched = 0;

	if (!forward && portal->scrollType == DISABLE_SCROLL)
			elog(ERROR, "Cursor can only scan forward"
				 "\n\tDeclare it with SCROLL option to enable backward scan");

	destfunc = DestToFunction(dest);
	(*destfunc->setup) (destfunc, queryDesc->operation,
						portal->name, queryDesc->tupDesc);

	for (;;)
	{
		HeapTuple tup;
		bool should_free;

		if (rows_fetched >= count)
			break;
		if (portal->atEnd && forward)
			break;
		if (portal->atStart && !forward)
			break;

		tup = tuplestore_getheaptuple(portal->holdStore, forward, &should_free);

		if (tup == NULL)
		{
			if (forward)
				portal->atEnd = true;
			else
				portal->atStart = true;

			break;
		}

		(*destfunc->receiveTuple) (tup, queryDesc->tupDesc, destfunc);

		rows_fetched++;
		if (forward)
			portal->portalPos++;
		else
			portal->portalPos--;

		if (forward && portal->atStart)
			portal->atStart = false;
		if (!forward && portal->atEnd)
			portal->atEnd = false;

		if (should_free)
			pfree(tup);
	}

	(*destfunc->cleanup) (destfunc);

	return rows_fetched;
}

/*
 * DoPortalRewind - rewind a Portal to starting point
 */
static void
DoPortalRewind(Portal portal)
{
	if (portal->holdStore)
		tuplestore_rescan(portal->holdStore);
	else
		ExecutorRewind(PortalGetQueryDesc(portal));

	portal->atStart = true;
	portal->atEnd = false;
	portal->portalPos = 0;
	portal->posOverflow = false;
}

/*
 * PerformPortalClose
 *		Close a cursor.
 */
void
PerformPortalClose(char *name)
{
	Portal		portal;

	/*
	 * get the portal from the portal name
	 */
	portal = GetPortalByName(name);
	if (!PortalIsValid(portal))
	{
		elog(WARNING, "PerformPortalClose: portal \"%s\" not found",
			 name);
		return;
	}

	/*
	 * Note: PortalCleanup is called as a side-effect
	 */
	PortalDrop(portal, false);
}

/*
 * PreparePortal
 *		Given a DECLARE CURSOR statement, returns the Portal data
 *		structure based on that statement that is used to manage the
 *		Portal internally. If a portal with specified name already
 *		exists, it is replaced.
 */
static Portal
PreparePortal(DeclareCursorStmt *stmt)
{
	Portal		portal;

	/*
	 * Check for already-in-use portal name.
	 */
	portal = GetPortalByName(stmt->portalname);
	if (PortalIsValid(portal))
	{
		/*
		 * XXX Should we raise an error rather than closing the old
		 * portal?
		 */
		elog(WARNING, "Closing pre-existing portal \"%s\"",
			 stmt->portalname);
		PortalDrop(portal, false);
	}

	/*
	 * Create the new portal.
	 */
	portal = CreatePortal(stmt->portalname);

	/*
	 * Modify the newly created portal based on the options specified in
	 * the DECLARE CURSOR statement.
	 */
	if (stmt->options & CURSOR_OPT_SCROLL)
		portal->scrollType = ENABLE_SCROLL;
	else if (stmt->options & CURSOR_OPT_NO_SCROLL)
		portal->scrollType = DISABLE_SCROLL;

	if (stmt->options & CURSOR_OPT_HOLD)
		portal->holdOpen = true;

	return portal;
}

/*
 * PortalCleanup
 *
 * Clean up a portal when it's dropped.  Since this mainly exists to run
 * ExecutorEnd(), it should not be set as the cleanup hook until we have
 * called ExecutorStart() on the portal's query.
 */
void
PortalCleanup(Portal portal)
{
	/*
	 * sanity checks
	 */
	AssertArg(PortalIsValid(portal));
	AssertArg(portal->cleanup == PortalCleanup);

	if (portal->holdStore)
		tuplestore_end(portal->holdStore);
	else
		ExecutorEnd(PortalGetQueryDesc(portal));

}

/*
 * PersistHoldablePortal
 *
 * Prepare the specified Portal for access outside of the current
 * transaction. When this function returns, all future accesses to the
 * portal must be done via the Tuplestore (not by invoking the
 * executor).
 */
void
PersistHoldablePortal(Portal portal)
{
	MemoryContext oldcxt;
	QueryDesc *queryDesc = PortalGetQueryDesc(portal);

	/*
	 * If we're preserving a holdable portal, we had better be
	 * inside the transaction that originally created it.
	 */
	Assert(portal->createXact == GetCurrentTransactionId());
	Assert(portal->holdStore == NULL);

	/*
	 * This context is used to store portal data that needs to persist
	 * between transactions.
	 */
	oldcxt = MemoryContextSwitchTo(portal->holdContext);

	/* XXX: Should SortMem be used for this? */
	portal->holdStore = tuplestore_begin_heap(true, true, SortMem);

	/* Set the destination to output to the tuplestore */
	queryDesc->dest = Tuplestore;

	/*
	 * Rewind the executor: we need to store the entire result set in
	 * the tuplestore, so that subsequent backward FETCHs can be
	 * processed.
	 */
	ExecutorRewind(queryDesc);

	/* Fetch the result set into the tuplestore */
	ExecutorRun(queryDesc, ForwardScanDirection, 0);

	/*
	 * Reset the position in the result set: ideally, this could be
	 * implemented by just skipping straight to the tuple # that we need
	 * to be at, but the tuplestore API doesn't support that. So we
	 * start at the beginning of the tuplestore and iterate through it
	 * until we reach where we need to be.
	 */
	if (!portal->atEnd)
	{
		int store_pos = 0;
		bool should_free;

		tuplestore_rescan(portal->holdStore);

		while (store_pos < portal->portalPos)
		{
			HeapTuple tmp = tuplestore_gettuple(portal->holdStore,
												true, &should_free);

			if (tmp == NULL)
				elog(ERROR,
					 "PersistHoldablePortal: unexpected end of tuple stream");

			store_pos++;

			/*
			 * This could probably be optimized by creating and then
			 * deleting a separate memory context for this series of
			 * operations.
			 */
			if (should_free)
				pfree(tmp);
		}
	}

	/*
	 * The current Portal structure contains some data that will be
	 * needed by the holdable cursor, but it has been allocated in a
	 * memory context that is not sufficiently long-lived: we need to
	 * copy it into the portal's long-term memory context.
	 */
	{
		TupleDesc tupDescCopy;
		QueryDesc *queryDescCopy;

		/*
		 * We need to use this order as ExecutorEnd invalidates the
		 * queryDesc's tuple descriptor
		 */
		tupDescCopy = CreateTupleDescCopy(queryDesc->tupDesc);

		ExecutorEnd(queryDesc);

		queryDescCopy = palloc(sizeof(*queryDescCopy));

		/*
		 * This doesn't copy all the dependant data in the QueryDesc,
		 * but that's okay -- the only complex field we need to keep is
		 * the query's tupledesc, which we've copied ourselves.
		 */
		memcpy(queryDescCopy, queryDesc, sizeof(*queryDesc));

		FreeQueryDesc(queryDesc);

		queryDescCopy->tupDesc = tupDescCopy;
		portal->queryDesc = queryDescCopy;
	}

	/*
	 * We no longer need the portal's short-term memory context.
	 */
	MemoryContextDelete(PortalGetHeapMemory(portal));

	PortalGetHeapMemory(portal) = NULL;
}
