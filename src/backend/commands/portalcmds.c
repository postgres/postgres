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
 *	  $Header: /cvsroot/pgsql/src/backend/commands/portalcmds.c,v 1.7 2002/12/30 15:31:47 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "commands/portalcmds.h"
#include "executor/executor.h"


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

	/*
	 * tell the executor to shutdown the query
	 */
	ExecutorEnd(PortalGetQueryDesc(portal));

	/*
	 * This should be unnecessary since the querydesc should be in the
	 * portal's memory context, but do it anyway for symmetry.
	 */
	FreeQueryDesc(PortalGetQueryDesc(portal));
}


/*
 * PerformPortalFetch
 *
 *	name: name of portal
 *	forward: forward or backward fetch?
 *	count: # of tuples to fetch
 *	dest: where to send results
 *	completionTag: points to a buffer of size COMPLETION_TAG_BUFSIZE
 *		in which to store a command completion status string.
 *
 * completionTag may be NULL if caller doesn't want a status string.
 */
void
PerformPortalFetch(char *name,
				   bool forward,
				   long count,
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

	/* If zero count, handle specially */
	if (count == 0)
	{
		bool on_row = false;

		/* Are we sitting on a row? */
		oldcontext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));
		queryDesc = PortalGetQueryDesc(portal);
		estate = queryDesc->estate;
		if (portal->atStart == false && portal->atEnd == false)
			on_row = true;
		MemoryContextSwitchTo(oldcontext);

		if (dest == None)
		{
			/* MOVE 0 returns 0/1 based on if FETCH 0 would return a row */
			if (completionTag && on_row)
				strcpy(completionTag, "MOVE 1");
			return;
		}
		else
		{
			/* If we are not on a row, FETCH 0 returns nothing */
			if (!on_row)
				return;

			/* Since we are sitting on a row, return the row */
			/* Back up so we can reread the row */
			PerformPortalFetch(name, false /* backward */, 1,
							   None, /* throw away output */
							   NULL /* do not modify the command tag */);

			/* Set up to fetch one row */
			count = 1;
			forward = true;
		}
	}

	/* Internally, zero count processes all portal rows */
	if (count == LONG_MAX)
		count = 0;

	/*
	 * switch into the portal context
	 */
	oldcontext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));

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
	 * tuples.	(This check exists because not all plan node types are
	 * robust about being called again if they've already returned NULL
	 * once.)  Then call the executor (we must not skip this, because the
	 * destination needs to see a setup and shutdown even if no tuples are
	 * available).	Finally, update the atStart/atEnd state depending on
	 * the number of tuples that were retrieved.
	 */
	if (forward)
	{
		if (portal->atEnd)
			direction = NoMovementScanDirection;
		else
			direction = ForwardScanDirection;

		ExecutorRun(queryDesc, direction, (long) count);

		if (estate->es_processed > 0)
			portal->atStart = false;	/* OK to back up now */
		if (count <= 0 || (int) estate->es_processed < count)
			portal->atEnd = true;		/* we retrieved 'em all */
	}
	else
	{
		if (portal->atStart)
			direction = NoMovementScanDirection;
		else
			direction = BackwardScanDirection;

		ExecutorRun(queryDesc, direction, (long) count);

		if (estate->es_processed > 0)
			portal->atEnd = false;		/* OK to go forward now */
		if (count <= 0 || (int) estate->es_processed < count)
			portal->atStart = true;		/* we retrieved 'em all */
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
