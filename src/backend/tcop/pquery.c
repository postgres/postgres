/*-------------------------------------------------------------------------
 *
 * pquery.c
 *	  POSTGRES process query command code
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/pquery.c,v 1.47 2002/02/26 22:47:09 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/command.h"
#include "executor/execdefs.h"
#include "executor/executor.h"
#include "tcop/pquery.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"


/* ----------------------------------------------------------------
 *		CreateQueryDesc
 * ----------------------------------------------------------------
 */
QueryDesc *
CreateQueryDesc(Query *parsetree,
				Plan *plantree,
				CommandDest dest)
{
	QueryDesc  *qd = (QueryDesc *) palloc(sizeof(QueryDesc));

	qd->operation = parsetree->commandType;		/* operation */
	qd->parsetree = parsetree;	/* parse tree */
	qd->plantree = plantree;	/* plan */
	qd->dest = dest;			/* output dest */
	return qd;
}

/* ----------------------------------------------------------------
 *		CreateExecutorState
 *
 *		Note: this may someday take parameters -cim 9/18/89
 * ----------------------------------------------------------------
 */
EState *
CreateExecutorState(void)
{
	EState	   *state;

	/*
	 * create a new executor state
	 */
	state = makeNode(EState);

	/*
	 * initialize the Executor State structure
	 */
	state->es_direction = ForwardScanDirection;
	state->es_range_table = NIL;

	state->es_result_relations = NULL;
	state->es_num_result_relations = 0;
	state->es_result_relation_info = NULL;

	state->es_junkFilter = NULL;

	state->es_into_relation_descriptor = NULL;

	state->es_param_list_info = NULL;
	state->es_param_exec_vals = NULL;

	state->es_tupleTable = NULL;

	state->es_query_cxt = CurrentMemoryContext;

	state->es_per_tuple_exprcontext = NULL;

	/*
	 * return the executor state structure
	 */
	return state;
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
		elog(NOTICE, "Closing pre-existing portal \"%s\"",
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
	bool		isRetrieveIntoPortal;
	bool		isRetrieveIntoRelation;
	char	   *intoName = NULL;
	Portal		portal = NULL;
	MemoryContext oldContext = NULL;
	QueryDesc  *queryDesc;
	EState	   *state;
	TupleDesc	attinfo;

	/*
	 * initialize portal/into relation status
	 */
	isRetrieveIntoPortal = false;
	isRetrieveIntoRelation = false;

	if (operation == CMD_SELECT)
	{
		if (parsetree->isPortal)
		{
			isRetrieveIntoPortal = true;
			intoName = parsetree->into;
			if (parsetree->isBinary)
			{
				/*
				 * For internal format portals, we change Remote
				 * (externalized form) to RemoteInternal (internalized
				 * form)
				 */
				dest = RemoteInternal;
			}
		}
		else if (parsetree->into != NULL)
		{
			/* select into table */
			isRetrieveIntoRelation = true;
		}
	}

	/*
	 * If retrieving into a portal, set up the portal and copy the
	 * parsetree and plan into its memory context.
	 */
	if (isRetrieveIntoPortal)
	{
		portal = PreparePortal(intoName);
		oldContext = MemoryContextSwitchTo(PortalGetHeapMemory(portal));
		parsetree = copyObject(parsetree);
		plan = copyObject(plan);

		/*
		 * We stay in portal's memory context for now, so that query desc,
		 * EState, and plan startup info are also allocated in the portal
		 * context.
		 */
	}

	/*
	 * Now we can create the QueryDesc object.
	 */
	queryDesc = CreateQueryDesc(parsetree, plan, dest);

	/*
	 * When performing a retrieve into, we override the normal
	 * communication destination during the processing of the the query.
	 * This only affects the tuple-output function - the correct
	 * destination will still see the BeginCommand() call.
	 */
	if (isRetrieveIntoRelation)
		queryDesc->dest = None;

	/*
	 * create a default executor state.
	 */
	state = CreateExecutorState();

	/*
	 * call ExecStart to prepare the plan for execution
	 */
	attinfo = ExecutorStart(queryDesc, state);

	/*
	 * report the query's result type information back to the front end or
	 * to whatever destination we're dealing with.
	 */
	BeginCommand(NULL,
				 operation,
				 attinfo,
				 isRetrieveIntoRelation,
				 isRetrieveIntoPortal,
				 NULL,			/* not used */
				 dest);

	/*
	 * If retrieve into portal, stop now; we do not run the plan until a
	 * FETCH command is received.
	 */
	if (isRetrieveIntoPortal)
	{
		PortalSetQuery(portal,
					   queryDesc,
					   attinfo,
					   state,
					   PortalCleanup);

		/* Now we can return to caller's memory context. */
		MemoryContextSwitchTo(oldContext);

		/* Set completion tag.  SQL calls this operation DECLARE CURSOR */
		if (completionTag)
			strcpy(completionTag, "DECLARE");

		return;
	}

	/*
	 * Now we get to the important call to ExecutorRun() where we actually
	 * run the plan..
	 */
	ExecutorRun(queryDesc, state, EXEC_RUN, 0L);

	/*
	 * Build command completion status string, if caller wants one.
	 */
	if (completionTag)
	{
		Oid		lastOid;

		switch (operation)
		{
			case CMD_SELECT:
				strcpy(completionTag, "SELECT");
				break;
			case CMD_INSERT:
				if (state->es_processed == 1)
					lastOid = state->es_lastoid;
				else
					lastOid = InvalidOid;
				snprintf(completionTag, COMPLETION_TAG_BUFSIZE,
						 "INSERT %u %u", lastOid, state->es_processed);
				break;
			case CMD_UPDATE:
				snprintf(completionTag, COMPLETION_TAG_BUFSIZE,
						 "UPDATE %u", state->es_processed);
				break;
			case CMD_DELETE:
				snprintf(completionTag, COMPLETION_TAG_BUFSIZE,
						 "DELETE %u", state->es_processed);
				break;
			default:
				strcpy(completionTag, "???");
				break;
		}
	}

	/*
	 * Now, we close down all the scans and free allocated resources.
	 */
	ExecutorEnd(queryDesc, state);
}
