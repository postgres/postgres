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
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/pquery.c,v 1.59 2003/03/10 03:53:51 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "tcop/pquery.h"


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
			dest = None;
		}
	}

	/*
	 * Create the QueryDesc object
	 */
	queryDesc = CreateQueryDesc(parsetree, plan, dest, NULL, NULL, false);

	/*
	 * Call ExecStart to prepare the plan for execution
	 */
	ExecutorStart(queryDesc);

	/*
	 * And run the plan.
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
