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
 *	  $Header: /cvsroot/pgsql/src/backend/tcop/pquery.c,v 1.44 2001/03/22 06:16:17 momjian Exp $
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
		PortalDrop(&portal);
	}

	/*
	 * Create the new portal.
	 */
	portal = CreatePortal(portalName);

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
	char	   *intoName = NULL;
	Portal		portal = NULL;
	MemoryContext oldContext = NULL;
	QueryDesc  *queryDesc;
	EState	   *state;
	TupleDesc	attinfo;

	set_ps_display(tag = CreateOperationTag(operation));

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
	 * destination will still see BeginCommand() and EndCommand()
	 * messages.
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
				 tag,
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

		EndCommand(tag, dest);

		return;
	}

	/*
	 * Now we get to the important call to ExecutorRun() where we actually
	 * run the plan..
	 */
	ExecutorRun(queryDesc, state, EXEC_RUN, 0L);

	/* save infos for EndCommand */
	UpdateCommandInfo(operation, state->es_lastoid, state->es_processed);

	/*
	 * Now, we close down all the scans and free allocated resources.
	 */
	ExecutorEnd(queryDesc, state);

	/*
	 * Notify the destination of end of processing.
	 */
	EndCommand(tag, dest);
}
