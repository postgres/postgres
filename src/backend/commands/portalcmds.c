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
 *	  $Header: /cvsroot/pgsql/src/backend/commands/portalcmds.c,v 1.2 2002/05/21 22:05:54 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/portalcmds.h"
#include "executor/executor.h"


/*
 * PortalCleanup
 */
void
PortalCleanup(Portal portal)
{
	MemoryContext oldcontext;

	/*
	 * sanity checks
	 */
	AssertArg(PortalIsValid(portal));
	AssertArg(portal->cleanup == PortalCleanup);

	/*
	 * set proper portal-executor context before calling ExecMain.
	 */
	oldcontext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));

	/*
	 * tell the executor to shutdown the query
	 */
	ExecutorEnd(PortalGetQueryDesc(portal), PortalGetState(portal));

	/*
	 * switch back to previous context
	 */
	MemoryContextSwitchTo(oldcontext);
}


/*
 * PerformPortalFetch
 *
 *	name: name of portal
 *	forward: forward or backward fetch?
 *	count: # of tuples to fetch (0 implies all)
 *	dest: where to send results
 *	completionTag: points to a buffer of size COMPLETION_TAG_BUFSIZE
 *		in which to store a command completion status string.
 *
 * completionTag may be NULL if caller doesn't want a status string.
 */
void
PerformPortalFetch(char *name,
				   bool forward,
				   int count,
				   CommandDest dest,
				   char *completionTag)
{
	Portal		portal;
	QueryDesc  *queryDesc;
	EState	   *estate;
	MemoryContext oldcontext;
	ScanDirection direction;
	bool		temp_desc = false;

	/* initialize completion status in case of early exit */
	if (completionTag)
		strcpy(completionTag, (dest == None) ? "MOVE 0" : "FETCH 0");

	/*
	 * sanity checks
	 */
	if (name == NULL)
	{
		elog(WARNING, "PerformPortalFetch: missing portal name");
		return;
	}

	/*
	 * get the portal from the portal name
	 */
	portal = GetPortalByName(name);
	if (!PortalIsValid(portal))
	{
		elog(WARNING, "PerformPortalFetch: portal \"%s\" not found",
			 name);
		return;
	}

	/*
	 * switch into the portal context
	 */
	oldcontext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));

	queryDesc = PortalGetQueryDesc(portal);
	estate = PortalGetState(portal);

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
		QueryDesc  *qdesc = (QueryDesc *) palloc(sizeof(QueryDesc));

		memcpy(qdesc, queryDesc, sizeof(QueryDesc));
		qdesc->dest = dest;
		queryDesc = qdesc;
		temp_desc = true;
	}

	/*
	 * Determine which direction to go in, and check to see if we're
	 * already at the end of the available tuples in that direction.  If
	 * so, set the direction to NoMovement to avoid trying to fetch any
	 * tuples.  (This check exists because not all plan node types
	 * are robust about being called again if they've already returned
	 * NULL once.)  Then call the executor (we must not skip this, because
	 * the destination needs to see a setup and shutdown even if no tuples
	 * are available).  Finally, update the atStart/atEnd state depending
	 * on the number of tuples that were retrieved.
	 */
	if (forward)
	{
		if (portal->atEnd)
			direction = NoMovementScanDirection;
		else
			direction = ForwardScanDirection;

		ExecutorRun(queryDesc, estate, direction, (long) count);

		if (estate->es_processed > 0)
			portal->atStart = false; /* OK to back up now */
		if (count <= 0 || (int) estate->es_processed < count)
			portal->atEnd = true;	/* we retrieved 'em all */
	}
	else
	{
		if (portal->atStart)
			direction = NoMovementScanDirection;
		else
			direction = BackwardScanDirection;

		ExecutorRun(queryDesc, estate, direction, (long) count);

		if (estate->es_processed > 0)
			portal->atEnd = false;	/* OK to go forward now */
		if (count <= 0 || (int) estate->es_processed < count)
			portal->atStart = true; /* we retrieved 'em all */
	}

	/* Return command status if wanted */
	if (completionTag)
		snprintf(completionTag, COMPLETION_TAG_BUFSIZE, "%s %u",
				 (dest == None) ? "MOVE" : "FETCH",
				 estate->es_processed);

	/*
	 * Clean up and switch back to old context.
	 */
	if (temp_desc)
		pfree(queryDesc);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * PerformPortalClose
 */
void
PerformPortalClose(char *name, CommandDest dest)
{
	Portal		portal;

	/*
	 * sanity checks
	 */
	if (name == NULL)
	{
		elog(WARNING, "PerformPortalClose: missing portal name");
		return;
	}

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
	PortalDrop(portal);
}
