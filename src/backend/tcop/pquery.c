/*-------------------------------------------------------------------------
 *
 * pquery.c
 *	  POSTGRES process query command code
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/pquery.c,v 1.35 2000/06/28 03:32:22 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/command.h"
#include "executor/execdefs.h"
#include "executor/executor.h"
#include "tcop/pquery.h"
#include "utils/ps_status.h"

static char *CreateOperationTag(int operationType);


/* ----------------------------------------------------------------
 *		CreateQueryDesc
 * ----------------------------------------------------------------
 */
QueryDesc  *
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

	/* ----------------
	 *	create a new executor state
	 * ----------------
	 */
	state = makeNode(EState);

	/* ----------------
	 *	initialize the Executor State structure
	 * ----------------
	 */
	state->es_direction = ForwardScanDirection;
	state->es_range_table = NIL;

	state->es_into_relation_descriptor = NULL;
	state->es_result_relation_info = NULL;

	state->es_param_list_info = NULL;
	state->es_param_exec_vals = NULL;

	state->es_BaseId = 0;
	state->es_tupleTable = NULL;

	state->es_junkFilter = NULL;

	/* ----------------
	 *	return the executor state structure
	 * ----------------
	 */
	return state;
}

/* ----------------------------------------------------------------
 *		CreateOperationTag
 *
 *		utility to get a string representation of the
 *		query operation.
 * ----------------------------------------------------------------
 */
static char *
CreateOperationTag(int operationType)
{
	char	   *tag;

	switch (operationType)
	{
		case CMD_SELECT:
			tag = "SELECT";
			break;
		case CMD_INSERT:
			tag = "INSERT";
			break;
		case CMD_DELETE:
			tag = "DELETE";
			break;
		case CMD_UPDATE:
			tag = "UPDATE";
			break;
		default:
			elog(DEBUG, "CreateOperationTag: unknown operation type %d",
				 operationType);
			tag = "???";
			break;
	}

	return tag;
}

/* ----------------
 *		PreparePortal
 * ----------------
 */
Portal
PreparePortal(char *portalName)
{
	Portal		portal;

	/* ----------------
	 *	 Check for already-in-use portal name.
	 * ----------------
	 */
	portal = GetPortalByName(portalName);
	if (PortalIsValid(portal))
	{
		/* XXX Should we raise an error rather than closing the old portal? */
		elog(NOTICE, "Closing pre-existing portal \"%s\"",
			 portalName);
		PortalDrop(&portal);
	}

	/* ----------------
	 *	 Create the new portal and make its memory context active.
	 * ----------------
	 */
	portal = CreatePortal(portalName);

	MemoryContextSwitchTo(PortalGetHeapMemory(portal));

	return portal;
}


/* ----------------------------------------------------------------
 *		ProcessQuery
 *
 *		Execute a plan, the non-parallel version
 * ----------------------------------------------------------------
 */
void
ProcessQuery(Query *parsetree,
			 Plan *plan,
			 CommandDest dest)
{
	int			operation = parsetree->commandType;
	char	   *tag;
	bool		isRetrieveIntoPortal;
	bool		isRetrieveIntoRelation;
	Portal		portal = NULL;
	char	   *intoName = NULL;
	QueryDesc  *queryDesc;
	EState	   *state;
	TupleDesc	attinfo;

	set_ps_display(tag = CreateOperationTag(operation));

	/* ----------------
	 *	initialize portal/into relation status
	 * ----------------
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

	/* ----------------
	 *	If retrieving into a portal, set up the portal and copy
	 *	the parsetree and plan into its memory context.
	 * ----------------
	 */
	if (isRetrieveIntoPortal)
	{
		portal = PreparePortal(intoName);
		/* CurrentMemoryContext is now pointing to portal's context */
		parsetree = copyObject(parsetree);
		plan = copyObject(plan);
	}

	/* ----------------
	 *	Now we can create the QueryDesc object (this is also in
	 *	the portal context, if portal retrieve).
	 * ----------------
	 */
	queryDesc = CreateQueryDesc(parsetree, plan, dest);

	/* ----------------
	 *	When performing a retrieve into, we override the normal
	 *	communication destination during the processing of the
	 *	the query.	This only affects the tuple-output function
	 *	- the correct destination will still see BeginCommand()
	 *	and EndCommand() messages.
	 * ----------------
	 */
	if (isRetrieveIntoRelation)
		queryDesc->dest = (int) None;

	/* ----------------
	 *	create a default executor state..
	 * ----------------
	 */
	state = CreateExecutorState();

	/* ----------------
	 *	call ExecStart to prepare the plan for execution
	 * ----------------
	 */
	attinfo = ExecutorStart(queryDesc, state);

	/* ----------------
	 *	 report the query's result type information
	 *	 back to the front end or to whatever destination
	 *	 we're dealing with.
	 * ----------------
	 */
	BeginCommand(NULL,
				 operation,
				 attinfo,
				 isRetrieveIntoRelation,
				 isRetrieveIntoPortal,
				 tag,
				 dest);

	/* ----------------
	 *	If retrieve into portal, stop now; we do not run the plan
	 *	until a FETCH command is received.
	 * ----------------
	 */
	if (isRetrieveIntoPortal)
	{
		PortalSetQuery(portal,
					   queryDesc,
					   attinfo,
					   state,
					   PortalCleanup);

		MemoryContextSwitchTo(TransactionCommandContext);

		EndCommand(tag, dest);
		return;
	}

	/* ----------------
	 *	 Now we get to the important call to ExecutorRun() where we
	 *	 actually run the plan..
	 * ----------------
	 */
	ExecutorRun(queryDesc, state, EXEC_RUN,
				parsetree->limitOffset, parsetree->limitCount);

	/* save infos for EndCommand */
	UpdateCommandInfo(operation, state->es_lastoid, state->es_processed);

	/* ----------------
	 *	 Now, we close down all the scans and free allocated resources.
	 * ----------------
	 */
	ExecutorEnd(queryDesc, state);

	/* ----------------
	 *	Notify the destination of end of processing.
	 * ----------------
	 */
	EndCommand(tag, dest);
}
