/*-------------------------------------------------------------------------
 *
 * pquery.c
 *	  POSTGRES process query command code
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/pquery.c,v 1.57 2002/12/05 15:50:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/portalcmds.h"
#include "executor/execdefs.h"
#include "executor/executor.h"
#include "tcop/pquery.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"


/*
 * CreateQueryDesc
 */
QueryDesc *
CreateQueryDesc(Query *parsetree,
				Plan *plantree,
				CommandDest dest,
				const char *portalName,
				ParamListInfo params,
				bool doInstrument)
{
	QueryDesc  *qd = (QueryDesc *) palloc(sizeof(QueryDesc));

	qd->operation = parsetree->commandType;		/* operation */
	qd->parsetree = parsetree;	/* parse tree */
	qd->plantree = plantree;	/* plan */
	qd->dest = dest;			/* output dest */
	qd->portalName = portalName;	/* name, if dest is a portal */
	qd->params = params;		/* parameter values passed into query */
	qd->doInstrument = doInstrument; /* instrumentation wanted? */

	/* null these fields until set by ExecutorStart */
	qd->tupDesc = NULL;
	qd->estate = NULL;
	qd->planstate = NULL;

	return qd;
}

/* ----------------
 *		PreparePortal
 * ----------------
 */
Portal
PreparePortal(char *portalName)
{
	Portal		portal;

	/*
	 * Check for already-in-use portal name.
	 */
	portal = GetPortalByName(portalName);
	if (PortalIsValid(portal))
	{
		/*
		 * XXX Should we raise an error rather than closing the old
		 * portal?
		 */
		elog(WARNING, "Closing pre-existing portal \"%s\"",
			 portalName);
		PortalDrop(portal);
	}

	/*
	 * Create the new portal.
	 */
	portal = CreatePortal(portalName);

	return portal;
}


/*
 * ProcessQuery
 *		Execute a query
 *
 *	parsetree: the query tree
 *	plan: the plan tree for the query
 *	dest: where to send results
 *	completionTag: points to a buffer of size COMPLETION_TAG_BUFSIZE
 *		in which to store a command completion status string.
 *
 * completionTag may be NULL if caller doesn't want a status string.
 */
void
ProcessQuery(Query *parsetree,
			 Plan *plan,
			 CommandDest dest,
			 char *completionTag)
{
	int			operation = parsetree->commandType;
	bool		isRetrieveIntoPortal = false;
	char	   *intoName = NULL;
	Portal		portal = NULL;
	MemoryContext oldContext = NULL;
	QueryDesc  *queryDesc;

	/*
	 * Check for special-case destinations
	 */
	if (operation == CMD_SELECT)
	{
		if (parsetree->isPortal)
		{
			isRetrieveIntoPortal = true;
			/* If binary portal, switch to alternate output format */
			if (dest == Remote && parsetree->isBinary)
				dest = RemoteInternal;
			/* Check for invalid context (must be in transaction block) */
			RequireTransactionChain((void *) parsetree, "DECLARE CURSOR");
		}
		else if (parsetree->into != NULL)
		{
			/*
			 * SELECT INTO table (a/k/a CREATE AS ... SELECT).
			 *
			 * Override the normal communication destination; execMain.c
			 * special-cases this case.  (Perhaps would be cleaner to have
			 * an additional destination type?)
			 */
			dest = None;
		}
	}

	/*
	 * If retrieving into a portal, set up the portal and copy the
	 * parsetree and plan into its memory context.
	 */
	if (isRetrieveIntoPortal)
	{
		intoName = parsetree->into->relname;
		portal = PreparePortal(intoName);
		oldContext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));
		parsetree = copyObject(parsetree);
		plan = copyObject(plan);
		intoName = parsetree->into->relname;	/* use copied name in
												 * QueryDesc */

		/*
		 * We stay in portal's memory context for now, so that query desc,
		 * exec state, and plan startup info are also allocated in the portal
		 * context.
		 */
	}

	/*
	 * Now we can create the QueryDesc object.
	 */
	queryDesc = CreateQueryDesc(parsetree, plan, dest, intoName, NULL, false);

	/*
	 * call ExecStart to prepare the plan for execution
	 */
	ExecutorStart(queryDesc);

	/*
	 * If retrieve into portal, stop now; we do not run the plan until a
	 * FETCH command is received.
	 */
	if (isRetrieveIntoPortal)
	{
		/* Arrange to shut down the executor if portal is dropped */
		PortalSetQuery(portal, queryDesc, PortalCleanup);

		/* Now we can return to caller's memory context. */
		MemoryContextSwitchTo(oldContext);

		/* Set completion tag.	SQL calls this operation DECLARE CURSOR */
		if (completionTag)
			strcpy(completionTag, "DECLARE CURSOR");

		return;
	}

	/*
	 * Now we get to the important call to ExecutorRun() where we actually
	 * run the plan..
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
}
