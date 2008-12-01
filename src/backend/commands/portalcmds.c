/*-------------------------------------------------------------------------
 *
 * portalcmds.c
 *	  Utility commands affecting portals (that is, SQL cursor commands)
 *
 * Note: see also tcop/pquery.c, which implements portal operations for
 * the FE/BE protocol.	This module uses pquery.c for some operations.
 * And both modules depend on utils/mmgr/portalmem.c, which controls
 * storage management for portals (but doesn't run any queries in them).
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/portalcmds.c,v 1.57.2.2 2008/12/01 17:06:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "access/xact.h"
#include "commands/portalcmds.h"
#include "executor/executor.h"
#include "executor/tstoreReceiver.h"
#include "optimizer/planner.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"


/*
 * PerformCursorOpen
 *		Execute SQL DECLARE CURSOR command.
 */
void
PerformCursorOpen(DeclareCursorStmt *stmt, ParamListInfo params)
{
	List	   *rewritten;
	Query	   *query;
	Plan	   *plan;
	Portal		portal;
	MemoryContext oldContext;

	/*
	 * Disallow empty-string cursor name (conflicts with protocol-level
	 * unnamed portal).
	 */
	if (!stmt->portalname || stmt->portalname[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_NAME),
				 errmsg("invalid cursor name: must not be empty")));

	/*
	 * If this is a non-holdable cursor, we require that this statement has
	 * been executed inside a transaction block (or else, it would have no
	 * user-visible effect).
	 */
	if (!(stmt->options & CURSOR_OPT_HOLD))
		RequireTransactionChain((void *) stmt, "DECLARE CURSOR");

	/*
	 * Because the planner is not cool about not scribbling on its input, we
	 * make a preliminary copy of the source querytree.  This prevents
	 * problems in the case that the DECLARE CURSOR is in a portal and is
	 * executed repeatedly.  XXX the planner really shouldn't modify its input
	 * ... FIXME someday.
	 */
	query = copyObject(stmt->query);

	/*
	 * The query has been through parse analysis, but not rewriting or
	 * planning as yet.  Note that the grammar ensured we have a SELECT query,
	 * so we are not expecting rule rewriting to do anything strange.
	 */
	AcquireRewriteLocks(query);
	rewritten = QueryRewrite(query);
	if (list_length(rewritten) != 1 || !IsA(linitial(rewritten), Query))
		elog(ERROR, "unexpected rewrite result");
	query = (Query *) linitial(rewritten);
	if (query->commandType != CMD_SELECT)
		elog(ERROR, "unexpected rewrite result");

	if (query->into)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_DEFINITION),
				 errmsg("DECLARE CURSOR may not specify INTO")));
	if (query->rowMarks != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			  errmsg("DECLARE CURSOR ... FOR UPDATE/SHARE is not supported"),
				 errdetail("Cursors must be READ ONLY.")));

	plan = planner(query, true, stmt->options, params);

	/*
	 * Create a portal and copy the query and plan into its memory context.
	 */
	portal = CreatePortal(stmt->portalname, false, false);

	oldContext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));

	query = copyObject(query);
	plan = copyObject(plan);

	/*
	 * XXX: debug_query_string is wrong here: the user might have submitted
	 * multiple semicolon delimited queries.
	 */
	PortalDefineQuery(portal,
					  NULL,
					  debug_query_string ? pstrdup(debug_query_string) : NULL,
					  "SELECT", /* cursor's query is always a SELECT */
					  list_make1(query),
					  list_make1(plan),
					  PortalGetHeapMemory(portal));

	/*
	 * Also copy the outer portal's parameter list into the inner portal's
	 * memory context.	We want to pass down the parameter values in case we
	 * had a command like DECLARE c CURSOR FOR SELECT ... WHERE foo = $1 This
	 * will have been parsed using the outer parameter set and the parameter
	 * value needs to be preserved for use when the cursor is executed.
	 */
	params = copyParamList(params);

	MemoryContextSwitchTo(oldContext);

	/*
	 * Set up options for portal.
	 *
	 * If the user didn't specify a SCROLL type, allow or disallow scrolling
	 * based on whether it would require any additional runtime overhead to do
	 * so.
	 */
	portal->cursorOptions = stmt->options;
	if (!(portal->cursorOptions & (CURSOR_OPT_SCROLL | CURSOR_OPT_NO_SCROLL)))
	{
		if (ExecSupportsBackwardScan(plan))
			portal->cursorOptions |= CURSOR_OPT_SCROLL;
		else
			portal->cursorOptions |= CURSOR_OPT_NO_SCROLL;
	}

	/*
	 * Start execution, inserting parameters if any.
	 */
	PortalStart(portal, params, ActiveSnapshot);

	Assert(portal->strategy == PORTAL_ONE_SELECT);

	/*
	 * We're done; the query won't actually be run until PerformPortalFetch is
	 * called.
	 */
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
				   DestReceiver *dest,
				   char *completionTag)
{
	Portal		portal;
	long		nprocessed;

	/*
	 * Disallow empty-string cursor name (conflicts with protocol-level
	 * unnamed portal).
	 */
	if (!stmt->portalname || stmt->portalname[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_NAME),
				 errmsg("invalid cursor name: must not be empty")));

	/* get the portal from the portal name */
	portal = GetPortalByName(stmt->portalname);
	if (!PortalIsValid(portal))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", stmt->portalname)));
		return;					/* keep compiler happy */
	}

	/* Adjust dest if needed.  MOVE wants destination DestNone */
	if (stmt->ismove)
		dest = None_Receiver;

	/* Do it */
	nprocessed = PortalRunFetch(portal,
								stmt->direction,
								stmt->howMany,
								dest);

	/* Return command status if wanted */
	if (completionTag)
		snprintf(completionTag, COMPLETION_TAG_BUFSIZE, "%s %ld",
				 stmt->ismove ? "MOVE" : "FETCH",
				 nprocessed);
}

/*
 * PerformPortalClose
 *		Close a cursor.
 */
void
PerformPortalClose(const char *name)
{
	Portal		portal;

	/*
	 * Disallow empty-string cursor name (conflicts with protocol-level
	 * unnamed portal).
	 */
	if (!name || name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_NAME),
				 errmsg("invalid cursor name: must not be empty")));

	/*
	 * get the portal from the portal name
	 */
	portal = GetPortalByName(name);
	if (!PortalIsValid(portal))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", name)));
		return;					/* keep compiler happy */
	}

	/*
	 * Note: PortalCleanup is called as a side-effect
	 */
	PortalDrop(portal, false);
}

/*
 * PortalCleanup
 *
 * Clean up a portal when it's dropped.  This is the standard cleanup hook
 * for portals.
 */
void
PortalCleanup(Portal portal)
{
	QueryDesc  *queryDesc;

	/*
	 * sanity checks
	 */
	AssertArg(PortalIsValid(portal));
	AssertArg(portal->cleanup == PortalCleanup);

	/*
	 * Shut down executor, if still running.  We skip this during error abort,
	 * since other mechanisms will take care of releasing executor resources,
	 * and we can't be sure that ExecutorEnd itself wouldn't fail.
	 */
	queryDesc = PortalGetQueryDesc(portal);
	if (queryDesc)
	{
		portal->queryDesc = NULL;
		if (portal->status != PORTAL_FAILED)
		{
			ResourceOwner saveResourceOwner;

			/* We must make the portal's resource owner current */
			saveResourceOwner = CurrentResourceOwner;
			PG_TRY();
			{
				CurrentResourceOwner = portal->resowner;
				/* we do not need AfterTriggerEndQuery() here */
				ExecutorEnd(queryDesc);
			}
			PG_CATCH();
			{
				/* Ensure CurrentResourceOwner is restored on error */
				CurrentResourceOwner = saveResourceOwner;
				PG_RE_THROW();
			}
			PG_END_TRY();
			CurrentResourceOwner = saveResourceOwner;
		}
	}
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
	QueryDesc  *queryDesc = PortalGetQueryDesc(portal);
	Portal		saveActivePortal;
	Snapshot	saveActiveSnapshot;
	ResourceOwner saveResourceOwner;
	MemoryContext savePortalContext;
	MemoryContext saveQueryContext;
	MemoryContext oldcxt;

	/*
	 * If we're preserving a holdable portal, we had better be inside the
	 * transaction that originally created it.
	 */
	Assert(portal->createSubid != InvalidSubTransactionId);
	Assert(queryDesc != NULL);

	/*
	 * Caller must have created the tuplestore already.
	 */
	Assert(portal->holdContext != NULL);
	Assert(portal->holdStore != NULL);

	/*
	 * Before closing down the executor, we must copy the tupdesc into
	 * long-term memory, since it was created in executor memory.
	 */
	oldcxt = MemoryContextSwitchTo(portal->holdContext);

	portal->tupDesc = CreateTupleDescCopy(portal->tupDesc);

	MemoryContextSwitchTo(oldcxt);

	/*
	 * Check for improper portal use, and mark portal active.
	 */
	if (portal->status != PORTAL_READY)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("portal \"%s\" cannot be run", portal->name)));
	portal->status = PORTAL_ACTIVE;

	/*
	 * Set up global portal context pointers.
	 */
	saveActivePortal = ActivePortal;
	saveActiveSnapshot = ActiveSnapshot;
	saveResourceOwner = CurrentResourceOwner;
	savePortalContext = PortalContext;
	saveQueryContext = QueryContext;
	PG_TRY();
	{
		ActivePortal = portal;
		ActiveSnapshot = queryDesc->snapshot;
		CurrentResourceOwner = portal->resowner;
		PortalContext = PortalGetHeapMemory(portal);
		QueryContext = portal->queryContext;

		MemoryContextSwitchTo(PortalContext);

		/*
		 * Rewind the executor: we need to store the entire result set in the
		 * tuplestore, so that subsequent backward FETCHs can be processed.
		 */
		ExecutorRewind(queryDesc);

		/*
		 * Change the destination to output to the tuplestore.  Note we
		 * tell the tuplestore receiver to detoast all data passed through it.
		 */
		queryDesc->dest = CreateDestReceiver(DestTuplestore, portal);
		SetTuplestoreDestReceiverDeToast(queryDesc->dest, true);

		/* Fetch the result set into the tuplestore */
		ExecutorRun(queryDesc, ForwardScanDirection, 0L);

		(*queryDesc->dest->rDestroy) (queryDesc->dest);
		queryDesc->dest = NULL;

		/*
		 * Now shut down the inner executor.
		 */
		portal->queryDesc = NULL;		/* prevent double shutdown */
		/* we do not need AfterTriggerEndQuery() here */
		ExecutorEnd(queryDesc);

		/*
		 * Set the position in the result set: ideally, this could be
		 * implemented by just skipping straight to the tuple # that we need
		 * to be at, but the tuplestore API doesn't support that. So we start
		 * at the beginning of the tuplestore and iterate through it until we
		 * reach where we need to be.  FIXME someday?  (Fortunately, the
		 * typical case is that we're supposed to be at or near the start
		 * of the result set, so this isn't as bad as it sounds.)
		 */
		MemoryContextSwitchTo(portal->holdContext);

		if (portal->atEnd)
		{
			/* we can handle this case even if posOverflow */
			while (tuplestore_advance(portal->holdStore, true))
				/* continue */ ;
		}
		else
		{
			long		store_pos;

			if (portal->posOverflow)	/* oops, cannot trust portalPos */
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("could not reposition held cursor")));

			tuplestore_rescan(portal->holdStore);

			for (store_pos = 0; store_pos < portal->portalPos; store_pos++)
			{
				if (!tuplestore_advance(portal->holdStore, true))
					elog(ERROR, "unexpected end of tuple stream");
			}
		}
	}
	PG_CATCH();
	{
		/* Uncaught error while executing portal: mark it dead */
		portal->status = PORTAL_FAILED;

		/* Restore global vars and propagate error */
		ActivePortal = saveActivePortal;
		ActiveSnapshot = saveActiveSnapshot;
		CurrentResourceOwner = saveResourceOwner;
		PortalContext = savePortalContext;
		QueryContext = saveQueryContext;

		PG_RE_THROW();
	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldcxt);

	/* Mark portal not active */
	portal->status = PORTAL_READY;

	ActivePortal = saveActivePortal;
	ActiveSnapshot = saveActiveSnapshot;
	CurrentResourceOwner = saveResourceOwner;
	PortalContext = savePortalContext;
	QueryContext = saveQueryContext;

	/*
	 * We can now release any subsidiary memory of the portal's heap context;
	 * we'll never use it again.  The executor already dropped its context,
	 * but this will clean up anything that glommed onto the portal's heap via
	 * PortalContext.
	 */
	MemoryContextDeleteChildren(PortalGetHeapMemory(portal));
}
