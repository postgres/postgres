/*-------------------------------------------------------------------------
 *
 * execMain.c
 *	  top level executor interface routines
 *
 * INTERFACE ROUTINES
 *	ExecutorStart()
 *	ExecutorRun()
 *	ExecutorEnd()
 *
 *	The old ExecutorMain() has been replaced by ExecutorStart(),
 *	ExecutorRun() and ExecutorEnd()
 *
 *	These three procedures are the external interfaces to the executor.
 *	In each case, the query descriptor is required as an argument.
 *
 *	ExecutorStart() must be called at the beginning of execution of any
 *	query plan and ExecutorEnd() should always be called at the end of
 *	execution of a plan.
 *
 *	ExecutorRun accepts direction and count arguments that specify whether
 *	the plan is to be executed forwards, backwards, and for how many tuples.
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/execMain.c,v 1.332 2009/10/10 01:43:47 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/toasting.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "executor/execdebug.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "parser/parse_clause.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/tqual.h"


/* Hooks for plugins to get control in ExecutorStart/Run/End() */
ExecutorStart_hook_type ExecutorStart_hook = NULL;
ExecutorRun_hook_type ExecutorRun_hook = NULL;
ExecutorEnd_hook_type ExecutorEnd_hook = NULL;

typedef struct evalPlanQual
{
	Index		rti;
	EState	   *estate;
	PlanState  *planstate;
	struct evalPlanQual *next;	/* stack of active PlanQual plans */
	struct evalPlanQual *free;	/* list of free PlanQual plans */
} evalPlanQual;

/* decls for local routines only used within this module */
static void InitPlan(QueryDesc *queryDesc, int eflags);
static void ExecEndPlan(PlanState *planstate, EState *estate);
static void ExecutePlan(EState *estate, PlanState *planstate,
			CmdType operation,
			bool sendTuples,
			long numberTuples,
			ScanDirection direction,
			DestReceiver *dest);
static TupleTableSlot *EvalPlanQualNext(EState *estate);
static void EndEvalPlanQual(EState *estate);
static void ExecCheckRTPerms(List *rangeTable);
static void ExecCheckRTEPerms(RangeTblEntry *rte);
static void ExecCheckXactReadOnly(PlannedStmt *plannedstmt);
static void EvalPlanQualStart(evalPlanQual *epq, EState *estate,
							  Plan *planTree, evalPlanQual *priorepq);
static void EvalPlanQualStop(evalPlanQual *epq);
static void OpenIntoRel(QueryDesc *queryDesc);
static void CloseIntoRel(QueryDesc *queryDesc);
static void intorel_startup(DestReceiver *self, int operation, TupleDesc typeinfo);
static void intorel_receive(TupleTableSlot *slot, DestReceiver *self);
static void intorel_shutdown(DestReceiver *self);
static void intorel_destroy(DestReceiver *self);

/* end of local decls */


/* ----------------------------------------------------------------
 *		ExecutorStart
 *
 *		This routine must be called at the beginning of any execution of any
 *		query plan
 *
 * Takes a QueryDesc previously created by CreateQueryDesc (it's not real
 * clear why we bother to separate the two functions, but...).	The tupDesc
 * field of the QueryDesc is filled in to describe the tuples that will be
 * returned, and the internal fields (estate and planstate) are set up.
 *
 * eflags contains flag bits as described in executor.h.
 *
 * NB: the CurrentMemoryContext when this is called will become the parent
 * of the per-query context used for this Executor invocation.
 *
 * We provide a function hook variable that lets loadable plugins
 * get control when ExecutorStart is called.  Such a plugin would
 * normally call standard_ExecutorStart().
 *
 * ----------------------------------------------------------------
 */
void
ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (ExecutorStart_hook)
		(*ExecutorStart_hook) (queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

void
standard_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* sanity checks: queryDesc must not be started already */
	Assert(queryDesc != NULL);
	Assert(queryDesc->estate == NULL);

	/*
	 * If the transaction is read-only, we need to check if any writes are
	 * planned to non-temporary tables.  EXPLAIN is considered read-only.
	 */
	if (XactReadOnly && !(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		ExecCheckXactReadOnly(queryDesc->plannedstmt);

	/*
	 * Build EState, switch into per-query memory context for startup.
	 */
	estate = CreateExecutorState();
	queryDesc->estate = estate;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/*
	 * Fill in parameters, if any, from queryDesc
	 */
	estate->es_param_list_info = queryDesc->params;

	if (queryDesc->plannedstmt->nParamExec > 0)
		estate->es_param_exec_vals = (ParamExecData *)
			palloc0(queryDesc->plannedstmt->nParamExec * sizeof(ParamExecData));

	/*
	 * If non-read-only query, set the command ID to mark output tuples with
	 */
	switch (queryDesc->operation)
	{
		case CMD_SELECT:
			/* SELECT INTO and SELECT FOR UPDATE/SHARE need to mark tuples */
			if (queryDesc->plannedstmt->intoClause != NULL ||
				queryDesc->plannedstmt->rowMarks != NIL)
				estate->es_output_cid = GetCurrentCommandId(true);
			break;

		case CMD_INSERT:
		case CMD_DELETE:
		case CMD_UPDATE:
			estate->es_output_cid = GetCurrentCommandId(true);
			break;

		default:
			elog(ERROR, "unrecognized operation code: %d",
				 (int) queryDesc->operation);
			break;
	}

	/*
	 * Copy other important information into the EState
	 */
	estate->es_snapshot = RegisterSnapshot(queryDesc->snapshot);
	estate->es_crosscheck_snapshot = RegisterSnapshot(queryDesc->crosscheck_snapshot);
	estate->es_instrument = queryDesc->doInstrument;

	/*
	 * Initialize the plan state tree
	 */
	InitPlan(queryDesc, eflags);

	MemoryContextSwitchTo(oldcontext);
}

/* ----------------------------------------------------------------
 *		ExecutorRun
 *
 *		This is the main routine of the executor module. It accepts
 *		the query descriptor from the traffic cop and executes the
 *		query plan.
 *
 *		ExecutorStart must have been called already.
 *
 *		If direction is NoMovementScanDirection then nothing is done
 *		except to start up/shut down the destination.  Otherwise,
 *		we retrieve up to 'count' tuples in the specified direction.
 *
 *		Note: count = 0 is interpreted as no portal limit, i.e., run to
 *		completion.
 *
 *		There is no return value, but output tuples (if any) are sent to
 *		the destination receiver specified in the QueryDesc; and the number
 *		of tuples processed at the top level can be found in
 *		estate->es_processed.
 *
 *		We provide a function hook variable that lets loadable plugins
 *		get control when ExecutorRun is called.  Such a plugin would
 *		normally call standard_ExecutorRun().
 *
 * ----------------------------------------------------------------
 */
void
ExecutorRun(QueryDesc *queryDesc,
			ScanDirection direction, long count)
{
	if (ExecutorRun_hook)
		(*ExecutorRun_hook) (queryDesc, direction, count);
	else
		standard_ExecutorRun(queryDesc, direction, count);
}

void
standard_ExecutorRun(QueryDesc *queryDesc,
					 ScanDirection direction, long count)
{
	EState	   *estate;
	CmdType		operation;
	DestReceiver *dest;
	bool		sendTuples;
	MemoryContext oldcontext;

	/* sanity checks */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);

	/*
	 * Switch into per-query memory context
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/* Allow instrumentation of ExecutorRun overall runtime */
	if (queryDesc->totaltime)
		InstrStartNode(queryDesc->totaltime);

	/*
	 * extract information from the query descriptor and the query feature.
	 */
	operation = queryDesc->operation;
	dest = queryDesc->dest;

	/*
	 * startup tuple receiver, if we will be emitting tuples
	 */
	estate->es_processed = 0;
	estate->es_lastoid = InvalidOid;

	sendTuples = (operation == CMD_SELECT ||
				  queryDesc->plannedstmt->hasReturning);

	if (sendTuples)
		(*dest->rStartup) (dest, operation, queryDesc->tupDesc);

	/*
	 * run plan
	 */
	if (!ScanDirectionIsNoMovement(direction))
		ExecutePlan(estate,
					queryDesc->planstate,
					operation,
					sendTuples,
					count,
					direction,
					dest);

	/*
	 * shutdown tuple receiver, if we started it
	 */
	if (sendTuples)
		(*dest->rShutdown) (dest);

	if (queryDesc->totaltime)
		InstrStopNode(queryDesc->totaltime, estate->es_processed);

	MemoryContextSwitchTo(oldcontext);
}

/* ----------------------------------------------------------------
 *		ExecutorEnd
 *
 *		This routine must be called at the end of execution of any
 *		query plan
 *
 *		We provide a function hook variable that lets loadable plugins
 *		get control when ExecutorEnd is called.  Such a plugin would
 *		normally call standard_ExecutorEnd().
 *
 * ----------------------------------------------------------------
 */
void
ExecutorEnd(QueryDesc *queryDesc)
{
	if (ExecutorEnd_hook)
		(*ExecutorEnd_hook) (queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

void
standard_ExecutorEnd(QueryDesc *queryDesc)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* sanity checks */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);

	/*
	 * Switch into per-query memory context to run ExecEndPlan
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	ExecEndPlan(queryDesc->planstate, estate);

	/*
	 * Close the SELECT INTO relation if any
	 */
	if (estate->es_select_into)
		CloseIntoRel(queryDesc);

	/* do away with our snapshots */
	UnregisterSnapshot(estate->es_snapshot);
	UnregisterSnapshot(estate->es_crosscheck_snapshot);

	/*
	 * Must switch out of context before destroying it
	 */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Release EState and per-query memory context.  This should release
	 * everything the executor has allocated.
	 */
	FreeExecutorState(estate);

	/* Reset queryDesc fields that no longer point to anything */
	queryDesc->tupDesc = NULL;
	queryDesc->estate = NULL;
	queryDesc->planstate = NULL;
	queryDesc->totaltime = NULL;
}

/* ----------------------------------------------------------------
 *		ExecutorRewind
 *
 *		This routine may be called on an open queryDesc to rewind it
 *		to the start.
 * ----------------------------------------------------------------
 */
void
ExecutorRewind(QueryDesc *queryDesc)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* sanity checks */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);

	/* It's probably not sensible to rescan updating queries */
	Assert(queryDesc->operation == CMD_SELECT);

	/*
	 * Switch into per-query memory context
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/*
	 * rescan plan
	 */
	ExecReScan(queryDesc->planstate, NULL);

	MemoryContextSwitchTo(oldcontext);
}


/*
 * ExecCheckRTPerms
 *		Check access permissions for all relations listed in a range table.
 */
static void
ExecCheckRTPerms(List *rangeTable)
{
	ListCell   *l;

	foreach(l, rangeTable)
	{
		ExecCheckRTEPerms((RangeTblEntry *) lfirst(l));
	}
}

/*
 * ExecCheckRTEPerms
 *		Check access permissions for a single RTE.
 */
static void
ExecCheckRTEPerms(RangeTblEntry *rte)
{
	AclMode		requiredPerms;
	AclMode		relPerms;
	AclMode		remainingPerms;
	Oid			relOid;
	Oid			userid;
	Bitmapset  *tmpset;
	int			col;

	/*
	 * Only plain-relation RTEs need to be checked here.  Function RTEs are
	 * checked by init_fcache when the function is prepared for execution.
	 * Join, subquery, and special RTEs need no checks.
	 */
	if (rte->rtekind != RTE_RELATION)
		return;

	/*
	 * No work if requiredPerms is empty.
	 */
	requiredPerms = rte->requiredPerms;
	if (requiredPerms == 0)
		return;

	relOid = rte->relid;

	/*
	 * userid to check as: current user unless we have a setuid indication.
	 *
	 * Note: GetUserId() is presently fast enough that there's no harm in
	 * calling it separately for each RTE.	If that stops being true, we could
	 * call it once in ExecCheckRTPerms and pass the userid down from there.
	 * But for now, no need for the extra clutter.
	 */
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/*
	 * We must have *all* the requiredPerms bits, but some of the bits can be
	 * satisfied from column-level rather than relation-level permissions.
	 * First, remove any bits that are satisfied by relation permissions.
	 */
	relPerms = pg_class_aclmask(relOid, userid, requiredPerms, ACLMASK_ALL);
	remainingPerms = requiredPerms & ~relPerms;
	if (remainingPerms != 0)
	{
		/*
		 * If we lack any permissions that exist only as relation permissions,
		 * we can fail straight away.
		 */
		if (remainingPerms & ~(ACL_SELECT | ACL_INSERT | ACL_UPDATE))
			aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_CLASS,
						   get_rel_name(relOid));

		/*
		 * Check to see if we have the needed privileges at column level.
		 *
		 * Note: failures just report a table-level error; it would be nicer
		 * to report a column-level error if we have some but not all of the
		 * column privileges.
		 */
		if (remainingPerms & ACL_SELECT)
		{
			/*
			 * When the query doesn't explicitly reference any columns (for
			 * example, SELECT COUNT(*) FROM table), allow the query if we
			 * have SELECT on any column of the rel, as per SQL spec.
			 */
			if (bms_is_empty(rte->selectedCols))
			{
				if (pg_attribute_aclcheck_all(relOid, userid, ACL_SELECT,
											  ACLMASK_ANY) != ACLCHECK_OK)
					aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_CLASS,
								   get_rel_name(relOid));
			}

			tmpset = bms_copy(rte->selectedCols);
			while ((col = bms_first_member(tmpset)) >= 0)
			{
				/* remove the column number offset */
				col += FirstLowInvalidHeapAttributeNumber;
				if (col == InvalidAttrNumber)
				{
					/* Whole-row reference, must have priv on all cols */
					if (pg_attribute_aclcheck_all(relOid, userid, ACL_SELECT,
												  ACLMASK_ALL) != ACLCHECK_OK)
						aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_CLASS,
									   get_rel_name(relOid));
				}
				else
				{
					if (pg_attribute_aclcheck(relOid, col, userid, ACL_SELECT)
						!= ACLCHECK_OK)
						aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_CLASS,
									   get_rel_name(relOid));
				}
			}
			bms_free(tmpset);
		}

		/*
		 * Basically the same for the mod columns, with either INSERT or
		 * UPDATE privilege as specified by remainingPerms.
		 */
		remainingPerms &= ~ACL_SELECT;
		if (remainingPerms != 0)
		{
			/*
			 * When the query doesn't explicitly change any columns, allow the
			 * query if we have permission on any column of the rel.  This is
			 * to handle SELECT FOR UPDATE as well as possible corner cases in
			 * INSERT and UPDATE.
			 */
			if (bms_is_empty(rte->modifiedCols))
			{
				if (pg_attribute_aclcheck_all(relOid, userid, remainingPerms,
											  ACLMASK_ANY) != ACLCHECK_OK)
					aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_CLASS,
								   get_rel_name(relOid));
			}

			tmpset = bms_copy(rte->modifiedCols);
			while ((col = bms_first_member(tmpset)) >= 0)
			{
				/* remove the column number offset */
				col += FirstLowInvalidHeapAttributeNumber;
				if (col == InvalidAttrNumber)
				{
					/* whole-row reference can't happen here */
					elog(ERROR, "whole-row update is not implemented");
				}
				else
				{
					if (pg_attribute_aclcheck(relOid, col, userid, remainingPerms)
						!= ACLCHECK_OK)
						aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_CLASS,
									   get_rel_name(relOid));
				}
			}
			bms_free(tmpset);
		}
	}
}

/*
 * Check that the query does not imply any writes to non-temp tables.
 */
static void
ExecCheckXactReadOnly(PlannedStmt *plannedstmt)
{
	ListCell   *l;

	/*
	 * CREATE TABLE AS or SELECT INTO?
	 *
	 * XXX should we allow this if the destination is temp?
	 */
	if (plannedstmt->intoClause != NULL)
		goto fail;

	/* Fail if write permissions are requested on any non-temp table */
	foreach(l, plannedstmt->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		if (rte->rtekind != RTE_RELATION)
			continue;

		if ((rte->requiredPerms & (~ACL_SELECT)) == 0)
			continue;

		if (isTempNamespace(get_rel_namespace(rte->relid)))
			continue;

		goto fail;
	}

	return;

fail:
	ereport(ERROR,
			(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
			 errmsg("transaction is read-only")));
}


/* ----------------------------------------------------------------
 *		InitPlan
 *
 *		Initializes the query plan: open files, allocate storage
 *		and start up the rule manager
 * ----------------------------------------------------------------
 */
static void
InitPlan(QueryDesc *queryDesc, int eflags)
{
	CmdType		operation = queryDesc->operation;
	PlannedStmt *plannedstmt = queryDesc->plannedstmt;
	Plan	   *plan = plannedstmt->planTree;
	List	   *rangeTable = plannedstmt->rtable;
	EState	   *estate = queryDesc->estate;
	PlanState  *planstate;
	TupleDesc	tupType;
	ListCell   *l;
	int			i;

	/*
	 * Do permissions checks
	 */
	ExecCheckRTPerms(rangeTable);

	/*
	 * initialize the node's execution state
	 */
	estate->es_range_table = rangeTable;

	/*
	 * initialize result relation stuff, and open/lock the result rels.
	 *
	 * We must do this before initializing the plan tree, else we might
	 * try to do a lock upgrade if a result rel is also a source rel.
	 */
	if (plannedstmt->resultRelations)
	{
		List	   *resultRelations = plannedstmt->resultRelations;
		int			numResultRelations = list_length(resultRelations);
		ResultRelInfo *resultRelInfos;
		ResultRelInfo *resultRelInfo;

		resultRelInfos = (ResultRelInfo *)
			palloc(numResultRelations * sizeof(ResultRelInfo));
		resultRelInfo = resultRelInfos;
		foreach(l, resultRelations)
		{
			Index		resultRelationIndex = lfirst_int(l);
			Oid			resultRelationOid;
			Relation	resultRelation;

			resultRelationOid = getrelid(resultRelationIndex, rangeTable);
			resultRelation = heap_open(resultRelationOid, RowExclusiveLock);
			InitResultRelInfo(resultRelInfo,
							  resultRelation,
							  resultRelationIndex,
							  operation,
							  estate->es_instrument);
			resultRelInfo++;
		}
		estate->es_result_relations = resultRelInfos;
		estate->es_num_result_relations = numResultRelations;
		/* es_result_relation_info is NULL except when within ModifyTable */
		estate->es_result_relation_info = NULL;
	}
	else
	{
		/*
		 * if no result relation, then set state appropriately
		 */
		estate->es_result_relations = NULL;
		estate->es_num_result_relations = 0;
		estate->es_result_relation_info = NULL;
	}

	/*
	 * Detect whether we're doing SELECT INTO.  If so, set the es_into_oids
	 * flag appropriately so that the plan tree will be initialized with the
	 * correct tuple descriptors.  (Other SELECT INTO stuff comes later.)
	 */
	estate->es_select_into = false;
	if (operation == CMD_SELECT && plannedstmt->intoClause != NULL)
	{
		estate->es_select_into = true;
		estate->es_into_oids = interpretOidsOption(plannedstmt->intoClause->options);
	}

	/*
	 * Have to lock relations selected FOR UPDATE/FOR SHARE before we
	 * initialize the plan tree, else we'd be doing a lock upgrade. While we
	 * are at it, build the ExecRowMark list.
	 */
	estate->es_rowMarks = NIL;
	foreach(l, plannedstmt->rowMarks)
	{
		RowMarkClause *rc = (RowMarkClause *) lfirst(l);
		Oid			relid;
		Relation	relation;
		ExecRowMark *erm;

		/* ignore "parent" rowmarks; they are irrelevant at runtime */
		if (rc->isParent)
			continue;

		relid = getrelid(rc->rti, rangeTable);
		relation = heap_open(relid, RowShareLock);
		erm = (ExecRowMark *) palloc(sizeof(ExecRowMark));
		erm->relation = relation;
		erm->rti = rc->rti;
		erm->prti = rc->prti;
		erm->forUpdate = rc->forUpdate;
		erm->noWait = rc->noWait;
		/* We'll locate the junk attrs below */
		erm->ctidAttNo = InvalidAttrNumber;
		erm->toidAttNo = InvalidAttrNumber;
		ItemPointerSetInvalid(&(erm->curCtid));
		estate->es_rowMarks = lappend(estate->es_rowMarks, erm);
	}

	/*
	 * Initialize the executor's tuple table to empty.
	 */
	estate->es_tupleTable = NIL;
	estate->es_trig_tuple_slot = NULL;

	/* mark EvalPlanQual not active */
	estate->es_plannedstmt = plannedstmt;
	estate->es_evalPlanQual = NULL;
	estate->es_evTupleNull = NULL;
	estate->es_evTuple = NULL;
	estate->es_useEvalPlan = false;

	/*
	 * Initialize private state information for each SubPlan.  We must do this
	 * before running ExecInitNode on the main query tree, since
	 * ExecInitSubPlan expects to be able to find these entries.
	 */
	Assert(estate->es_subplanstates == NIL);
	i = 1;						/* subplan indices count from 1 */
	foreach(l, plannedstmt->subplans)
	{
		Plan	   *subplan = (Plan *) lfirst(l);
		PlanState  *subplanstate;
		int			sp_eflags;

		/*
		 * A subplan will never need to do BACKWARD scan nor MARK/RESTORE. If
		 * it is a parameterless subplan (not initplan), we suggest that it be
		 * prepared to handle REWIND efficiently; otherwise there is no need.
		 */
		sp_eflags = eflags & EXEC_FLAG_EXPLAIN_ONLY;
		if (bms_is_member(i, plannedstmt->rewindPlanIDs))
			sp_eflags |= EXEC_FLAG_REWIND;

		subplanstate = ExecInitNode(subplan, estate, sp_eflags);

		estate->es_subplanstates = lappend(estate->es_subplanstates,
										   subplanstate);

		i++;
	}

	/*
	 * Initialize the private state information for all the nodes in the query
	 * tree.  This opens files, allocates storage and leaves us ready to start
	 * processing tuples.
	 */
	planstate = ExecInitNode(plan, estate, eflags);

	/*
	 * Get the tuple descriptor describing the type of tuples to return. (this
	 * is especially important if we are creating a relation with "SELECT
	 * INTO")
	 */
	tupType = ExecGetResultType(planstate);

	/*
	 * Initialize the junk filter if needed.  SELECT queries need a
	 * filter if there are any junk attrs in the top-level tlist.
	 */
	if (operation == CMD_SELECT)
	{
		bool		junk_filter_needed = false;
		ListCell   *tlist;

		foreach(tlist, plan->targetlist)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tlist);

			if (tle->resjunk)
			{
				junk_filter_needed = true;
				break;
			}
		}

		if (junk_filter_needed)
		{
			JunkFilter *j;

			j = ExecInitJunkFilter(planstate->plan->targetlist,
								   tupType->tdhasoid,
								   ExecInitExtraTupleSlot(estate));
			estate->es_junkFilter = j;

			/* Want to return the cleaned tuple type */
			tupType = j->jf_cleanTupType;

			/* For SELECT FOR UPDATE/SHARE, find the junk attrs now */
			foreach(l, estate->es_rowMarks)
			{
				ExecRowMark *erm = (ExecRowMark *) lfirst(l);
				char		resname[32];

				/* always need the ctid */
				snprintf(resname, sizeof(resname), "ctid%u",
						 erm->prti);
				erm->ctidAttNo = ExecFindJunkAttribute(j, resname);
				if (!AttributeNumberIsValid(erm->ctidAttNo))
					elog(ERROR, "could not find junk \"%s\" column",
						 resname);
				/* if child relation, need tableoid too */
				if (erm->rti != erm->prti)
				{
					snprintf(resname, sizeof(resname), "tableoid%u",
							 erm->prti);
					erm->toidAttNo = ExecFindJunkAttribute(j, resname);
					if (!AttributeNumberIsValid(erm->toidAttNo))
						elog(ERROR, "could not find junk \"%s\" column",
							 resname);
				}
			}
		}
		else
		{
			estate->es_junkFilter = NULL;
			if (estate->es_rowMarks)
				elog(ERROR, "SELECT FOR UPDATE/SHARE, but no junk columns");
		}
	}

	queryDesc->tupDesc = tupType;
	queryDesc->planstate = planstate;

	/*
	 * If doing SELECT INTO, initialize the "into" relation.  We must wait
	 * till now so we have the "clean" result tuple type to create the new
	 * table from.
	 *
	 * If EXPLAIN, skip creating the "into" relation.
	 */
	if (estate->es_select_into && !(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		OpenIntoRel(queryDesc);
}

/*
 * Initialize ResultRelInfo data for one result relation
 */
void
InitResultRelInfo(ResultRelInfo *resultRelInfo,
				  Relation resultRelationDesc,
				  Index resultRelationIndex,
				  CmdType operation,
				  bool doInstrument)
{
	/*
	 * Check valid relkind ... parser and/or planner should have noticed this
	 * already, but let's make sure.
	 */
	switch (resultRelationDesc->rd_rel->relkind)
	{
		case RELKIND_RELATION:
			/* OK */
			break;
		case RELKIND_SEQUENCE:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change sequence \"%s\"",
							RelationGetRelationName(resultRelationDesc))));
			break;
		case RELKIND_TOASTVALUE:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change TOAST relation \"%s\"",
							RelationGetRelationName(resultRelationDesc))));
			break;
		case RELKIND_VIEW:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change view \"%s\"",
							RelationGetRelationName(resultRelationDesc))));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change relation \"%s\"",
							RelationGetRelationName(resultRelationDesc))));
			break;
	}

	/* OK, fill in the node */
	MemSet(resultRelInfo, 0, sizeof(ResultRelInfo));
	resultRelInfo->type = T_ResultRelInfo;
	resultRelInfo->ri_RangeTableIndex = resultRelationIndex;
	resultRelInfo->ri_RelationDesc = resultRelationDesc;
	resultRelInfo->ri_NumIndices = 0;
	resultRelInfo->ri_IndexRelationDescs = NULL;
	resultRelInfo->ri_IndexRelationInfo = NULL;
	/* make a copy so as not to depend on relcache info not changing... */
	resultRelInfo->ri_TrigDesc = CopyTriggerDesc(resultRelationDesc->trigdesc);
	if (resultRelInfo->ri_TrigDesc)
	{
		int			n = resultRelInfo->ri_TrigDesc->numtriggers;

		resultRelInfo->ri_TrigFunctions = (FmgrInfo *)
			palloc0(n * sizeof(FmgrInfo));
		if (doInstrument)
			resultRelInfo->ri_TrigInstrument = InstrAlloc(n);
		else
			resultRelInfo->ri_TrigInstrument = NULL;
	}
	else
	{
		resultRelInfo->ri_TrigFunctions = NULL;
		resultRelInfo->ri_TrigInstrument = NULL;
	}
	resultRelInfo->ri_ConstraintExprs = NULL;
	resultRelInfo->ri_junkFilter = NULL;
	resultRelInfo->ri_projectReturning = NULL;

	/*
	 * If there are indices on the result relation, open them and save
	 * descriptors in the result relation info, so that we can add new index
	 * entries for the tuples we add/update.  We need not do this for a
	 * DELETE, however, since deletion doesn't affect indexes.
	 */
	if (resultRelationDesc->rd_rel->relhasindex &&
		operation != CMD_DELETE)
		ExecOpenIndices(resultRelInfo);
}

/*
 *		ExecGetTriggerResultRel
 *
 * Get a ResultRelInfo for a trigger target relation.  Most of the time,
 * triggers are fired on one of the result relations of the query, and so
 * we can just return a member of the es_result_relations array.  (Note: in
 * self-join situations there might be multiple members with the same OID;
 * if so it doesn't matter which one we pick.)  However, it is sometimes
 * necessary to fire triggers on other relations; this happens mainly when an
 * RI update trigger queues additional triggers on other relations, which will
 * be processed in the context of the outer query.	For efficiency's sake,
 * we want to have a ResultRelInfo for those triggers too; that can avoid
 * repeated re-opening of the relation.  (It also provides a way for EXPLAIN
 * ANALYZE to report the runtimes of such triggers.)  So we make additional
 * ResultRelInfo's as needed, and save them in es_trig_target_relations.
 */
ResultRelInfo *
ExecGetTriggerResultRel(EState *estate, Oid relid)
{
	ResultRelInfo *rInfo;
	int			nr;
	ListCell   *l;
	Relation	rel;
	MemoryContext oldcontext;

	/* First, search through the query result relations */
	rInfo = estate->es_result_relations;
	nr = estate->es_num_result_relations;
	while (nr > 0)
	{
		if (RelationGetRelid(rInfo->ri_RelationDesc) == relid)
			return rInfo;
		rInfo++;
		nr--;
	}
	/* Nope, but maybe we already made an extra ResultRelInfo for it */
	foreach(l, estate->es_trig_target_relations)
	{
		rInfo = (ResultRelInfo *) lfirst(l);
		if (RelationGetRelid(rInfo->ri_RelationDesc) == relid)
			return rInfo;
	}
	/* Nope, so we need a new one */

	/*
	 * Open the target relation's relcache entry.  We assume that an
	 * appropriate lock is still held by the backend from whenever the trigger
	 * event got queued, so we need take no new lock here.
	 */
	rel = heap_open(relid, NoLock);

	/*
	 * Make the new entry in the right context.  Currently, we don't need any
	 * index information in ResultRelInfos used only for triggers, so tell
	 * InitResultRelInfo it's a DELETE.
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);
	rInfo = makeNode(ResultRelInfo);
	InitResultRelInfo(rInfo,
					  rel,
					  0,		/* dummy rangetable index */
					  CMD_DELETE,
					  estate->es_instrument);
	estate->es_trig_target_relations =
		lappend(estate->es_trig_target_relations, rInfo);
	MemoryContextSwitchTo(oldcontext);

	return rInfo;
}

/*
 *		ExecContextForcesOids
 *
 * This is pretty grotty: when doing INSERT, UPDATE, or SELECT INTO,
 * we need to ensure that result tuples have space for an OID iff they are
 * going to be stored into a relation that has OIDs.  In other contexts
 * we are free to choose whether to leave space for OIDs in result tuples
 * (we generally don't want to, but we do if a physical-tlist optimization
 * is possible).  This routine checks the plan context and returns TRUE if the
 * choice is forced, FALSE if the choice is not forced.  In the TRUE case,
 * *hasoids is set to the required value.
 *
 * One reason this is ugly is that all plan nodes in the plan tree will emit
 * tuples with space for an OID, though we really only need the topmost node
 * to do so.  However, node types like Sort don't project new tuples but just
 * return their inputs, and in those cases the requirement propagates down
 * to the input node.  Eventually we might make this code smart enough to
 * recognize how far down the requirement really goes, but for now we just
 * make all plan nodes do the same thing if the top level forces the choice.
 *
 * We assume that if we are generating tuples for INSERT or UPDATE,
 * estate->es_result_relation_info is already set up to describe the target
 * relation.  Note that in an UPDATE that spans an inheritance tree, some of
 * the target relations may have OIDs and some not.  We have to make the
 * decisions on a per-relation basis as we initialize each of the subplans of
 * the ModifyTable node, so ModifyTable has to set es_result_relation_info
 * while initializing each subplan.
 *
 * SELECT INTO is even uglier, because we don't have the INTO relation's
 * descriptor available when this code runs; we have to look aside at a
 * flag set by InitPlan().
 */
bool
ExecContextForcesOids(PlanState *planstate, bool *hasoids)
{
	ResultRelInfo *ri = planstate->state->es_result_relation_info;

	if (ri != NULL)
	{
		Relation	rel = ri->ri_RelationDesc;

		if (rel != NULL)
		{
			*hasoids = rel->rd_rel->relhasoids;
			return true;
		}
	}

	if (planstate->state->es_select_into)
	{
		*hasoids = planstate->state->es_into_oids;
		return true;
	}

	return false;
}

/* ----------------------------------------------------------------
 *		ExecEndPlan
 *
 *		Cleans up the query plan -- closes files and frees up storage
 *
 * NOTE: we are no longer very worried about freeing storage per se
 * in this code; FreeExecutorState should be guaranteed to release all
 * memory that needs to be released.  What we are worried about doing
 * is closing relations and dropping buffer pins.  Thus, for example,
 * tuple tables must be cleared or dropped to ensure pins are released.
 * ----------------------------------------------------------------
 */
static void
ExecEndPlan(PlanState *planstate, EState *estate)
{
	ResultRelInfo *resultRelInfo;
	int			i;
	ListCell   *l;

	/*
	 * shut down any PlanQual processing we were doing
	 */
	if (estate->es_evalPlanQual != NULL)
		EndEvalPlanQual(estate);

	/*
	 * shut down the node-type-specific query processing
	 */
	ExecEndNode(planstate);

	/*
	 * for subplans too
	 */
	foreach(l, estate->es_subplanstates)
	{
		PlanState  *subplanstate = (PlanState *) lfirst(l);

		ExecEndNode(subplanstate);
	}

	/*
	 * destroy the executor's tuple table.  Actually we only care about
	 * releasing buffer pins and tupdesc refcounts; there's no need to
	 * pfree the TupleTableSlots, since the containing memory context
	 * is about to go away anyway.
	 */
	ExecResetTupleTable(estate->es_tupleTable, false);

	/*
	 * close the result relation(s) if any, but hold locks until xact commit.
	 */
	resultRelInfo = estate->es_result_relations;
	for (i = estate->es_num_result_relations; i > 0; i--)
	{
		/* Close indices and then the relation itself */
		ExecCloseIndices(resultRelInfo);
		heap_close(resultRelInfo->ri_RelationDesc, NoLock);
		resultRelInfo++;
	}

	/*
	 * likewise close any trigger target relations
	 */
	foreach(l, estate->es_trig_target_relations)
	{
		resultRelInfo = (ResultRelInfo *) lfirst(l);
		/* Close indices and then the relation itself */
		ExecCloseIndices(resultRelInfo);
		heap_close(resultRelInfo->ri_RelationDesc, NoLock);
	}

	/*
	 * close any relations selected FOR UPDATE/FOR SHARE, again keeping locks
	 */
	foreach(l, estate->es_rowMarks)
	{
		ExecRowMark *erm = lfirst(l);

		heap_close(erm->relation, NoLock);
	}
}

/* ----------------------------------------------------------------
 *		ExecutePlan
 *
 *		Processes the query plan until we have processed 'numberTuples' tuples,
 *		moving in the specified direction.
 *
 *		Runs to completion if numberTuples is 0
 *
 * Note: the ctid attribute is a 'junk' attribute that is removed before the
 * user can see it
 * ----------------------------------------------------------------
 */
static void
ExecutePlan(EState *estate,
			PlanState *planstate,
			CmdType operation,
			bool sendTuples,
			long numberTuples,
			ScanDirection direction,
			DestReceiver *dest)
{
	JunkFilter *junkfilter;
	TupleTableSlot *planSlot;
	TupleTableSlot *slot;
	long		current_tuple_count;

	/*
	 * initialize local variables
	 */
	current_tuple_count = 0;

	/*
	 * Set the direction.
	 */
	estate->es_direction = direction;

	/*
	 * Loop until we've processed the proper number of tuples from the plan.
	 */
	for (;;)
	{
		/* Reset the per-output-tuple exprcontext */
		ResetPerTupleExprContext(estate);

		/*
		 * Execute the plan and obtain a tuple
		 */
lnext:	;
		if (estate->es_useEvalPlan)
		{
			planSlot = EvalPlanQualNext(estate);
			if (TupIsNull(planSlot))
				planSlot = ExecProcNode(planstate);
		}
		else
			planSlot = ExecProcNode(planstate);

		/*
		 * if the tuple is null, then we assume there is nothing more to
		 * process so we just end the loop...
		 */
		if (TupIsNull(planSlot))
			break;
		slot = planSlot;

		/*
		 * If we have a junk filter, then project a new tuple with the junk
		 * removed.
		 *
		 * Store this new "clean" tuple in the junkfilter's resultSlot.
		 * (Formerly, we stored it back over the "dirty" tuple, which is WRONG
		 * because that tuple slot has the wrong descriptor.)
		 *
		 * But first, extract all the junk information we need.
		 */
		if ((junkfilter = estate->es_junkFilter) != NULL)
		{
			/*
			 * Process any FOR UPDATE or FOR SHARE locking requested.
			 */
			if (estate->es_rowMarks != NIL)
			{
				ListCell   *l;

		lmark:	;
				foreach(l, estate->es_rowMarks)
				{
					ExecRowMark *erm = lfirst(l);
					Datum		datum;
					bool		isNull;
					HeapTupleData tuple;
					Buffer		buffer;
					ItemPointerData update_ctid;
					TransactionId update_xmax;
					TupleTableSlot *newSlot;
					LockTupleMode lockmode;
					HTSU_Result test;

					/* if child rel, must check whether it produced this row */
					if (erm->rti != erm->prti)
					{
						Oid			tableoid;

						datum = ExecGetJunkAttribute(slot,
													 erm->toidAttNo,
													 &isNull);
						/* shouldn't ever get a null result... */
						if (isNull)
							elog(ERROR, "tableoid is NULL");
						tableoid = DatumGetObjectId(datum);

						if (tableoid != RelationGetRelid(erm->relation))
						{
							/* this child is inactive right now */
							ItemPointerSetInvalid(&(erm->curCtid));
							continue;
						}
					}

					/* okay, fetch the tuple by ctid */
					datum = ExecGetJunkAttribute(slot,
												 erm->ctidAttNo,
												 &isNull);
					/* shouldn't ever get a null result... */
					if (isNull)
						elog(ERROR, "ctid is NULL");
					tuple.t_self = *((ItemPointer) DatumGetPointer(datum));

					if (erm->forUpdate)
						lockmode = LockTupleExclusive;
					else
						lockmode = LockTupleShared;

					test = heap_lock_tuple(erm->relation, &tuple, &buffer,
										   &update_ctid, &update_xmax,
										   estate->es_output_cid,
										   lockmode, erm->noWait);
					ReleaseBuffer(buffer);
					switch (test)
					{
						case HeapTupleSelfUpdated:
							/* treat it as deleted; do not process */
							goto lnext;

						case HeapTupleMayBeUpdated:
							break;

						case HeapTupleUpdated:
							if (IsXactIsoLevelSerializable)
								ereport(ERROR,
								 (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
								  errmsg("could not serialize access due to concurrent update")));
							if (!ItemPointerEquals(&update_ctid,
												   &tuple.t_self))
							{
								/* updated, so look at updated version */
								newSlot = EvalPlanQual(estate,
													   erm->rti,
													   planstate,
													   &update_ctid,
													   update_xmax);
								if (!TupIsNull(newSlot))
								{
									slot = planSlot = newSlot;
									estate->es_useEvalPlan = true;
									goto lmark;
								}
							}

							/*
							 * if tuple was deleted or PlanQual failed for
							 * updated tuple - we must not return this tuple!
							 */
							goto lnext;

						default:
							elog(ERROR, "unrecognized heap_lock_tuple status: %u",
								 test);
					}

					/* Remember tuple TID for WHERE CURRENT OF */
					erm->curCtid = tuple.t_self;
				}
			}

			/*
			 * Create a new "clean" tuple with all junk attributes removed.
			 */
			slot = ExecFilterJunk(junkfilter, slot);
		}

		/*
		 * If we are supposed to send the tuple somewhere, do so.
		 * (In practice this is probably always the case at this point.)
		 */
		if (sendTuples)
			(*dest->receiveSlot) (slot, dest);

		/*
		 * Count tuples processed, if this is a SELECT.  (For other operation
		 * types, the ModifyTable plan node must count the appropriate
		 * events.)
		 */
		if (operation == CMD_SELECT)
			(estate->es_processed)++;

		/*
		 * check our tuple count.. if we've processed the proper number then
		 * quit, else loop again and process more tuples.  Zero numberTuples
		 * means no limit.
		 */
		current_tuple_count++;
		if (numberTuples && numberTuples == current_tuple_count)
			break;
	}
}


/*
 * ExecRelCheck --- check that tuple meets constraints for result relation
 */
static const char *
ExecRelCheck(ResultRelInfo *resultRelInfo,
			 TupleTableSlot *slot, EState *estate)
{
	Relation	rel = resultRelInfo->ri_RelationDesc;
	int			ncheck = rel->rd_att->constr->num_check;
	ConstrCheck *check = rel->rd_att->constr->check;
	ExprContext *econtext;
	MemoryContext oldContext;
	List	   *qual;
	int			i;

	/*
	 * If first time through for this result relation, build expression
	 * nodetrees for rel's constraint expressions.  Keep them in the per-query
	 * memory context so they'll survive throughout the query.
	 */
	if (resultRelInfo->ri_ConstraintExprs == NULL)
	{
		oldContext = MemoryContextSwitchTo(estate->es_query_cxt);
		resultRelInfo->ri_ConstraintExprs =
			(List **) palloc(ncheck * sizeof(List *));
		for (i = 0; i < ncheck; i++)
		{
			/* ExecQual wants implicit-AND form */
			qual = make_ands_implicit(stringToNode(check[i].ccbin));
			resultRelInfo->ri_ConstraintExprs[i] = (List *)
				ExecPrepareExpr((Expr *) qual, estate);
		}
		MemoryContextSwitchTo(oldContext);
	}

	/*
	 * We will use the EState's per-tuple context for evaluating constraint
	 * expressions (creating it if it's not already there).
	 */
	econtext = GetPerTupleExprContext(estate);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* And evaluate the constraints */
	for (i = 0; i < ncheck; i++)
	{
		qual = resultRelInfo->ri_ConstraintExprs[i];

		/*
		 * NOTE: SQL92 specifies that a NULL result from a constraint
		 * expression is not to be treated as a failure.  Therefore, tell
		 * ExecQual to return TRUE for NULL.
		 */
		if (!ExecQual(qual, econtext, true))
			return check[i].ccname;
	}

	/* NULL result means no error */
	return NULL;
}

void
ExecConstraints(ResultRelInfo *resultRelInfo,
				TupleTableSlot *slot, EState *estate)
{
	Relation	rel = resultRelInfo->ri_RelationDesc;
	TupleConstr *constr = rel->rd_att->constr;

	Assert(constr);

	if (constr->has_not_null)
	{
		int			natts = rel->rd_att->natts;
		int			attrChk;

		for (attrChk = 1; attrChk <= natts; attrChk++)
		{
			if (rel->rd_att->attrs[attrChk - 1]->attnotnull &&
				slot_attisnull(slot, attrChk))
				ereport(ERROR,
						(errcode(ERRCODE_NOT_NULL_VIOLATION),
						 errmsg("null value in column \"%s\" violates not-null constraint",
						NameStr(rel->rd_att->attrs[attrChk - 1]->attname))));
		}
	}

	if (constr->num_check > 0)
	{
		const char *failed;

		if ((failed = ExecRelCheck(resultRelInfo, slot, estate)) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_CHECK_VIOLATION),
					 errmsg("new row for relation \"%s\" violates check constraint \"%s\"",
							RelationGetRelationName(rel), failed)));
	}
}

/*
 * Check a modified tuple to see if we want to process its updated version
 * under READ COMMITTED rules.
 *
 * See backend/executor/README for some info about how this works.
 *
 *	estate - executor state data
 *	rti - rangetable index of table containing tuple
 *	subplanstate - portion of plan tree that needs to be re-evaluated
 *	*tid - t_ctid from the outdated tuple (ie, next updated version)
 *	priorXmax - t_xmax from the outdated tuple
 *
 * *tid is also an output parameter: it's modified to hold the TID of the
 * latest version of the tuple (note this may be changed even on failure)
 *
 * Returns a slot containing the new candidate update/delete tuple, or
 * NULL if we determine we shouldn't process the row.
 */
TupleTableSlot *
EvalPlanQual(EState *estate, Index rti,
			 PlanState *subplanstate,
			 ItemPointer tid, TransactionId priorXmax)
{
	evalPlanQual *epq;
	EState	   *epqstate;
	Relation	relation;
	HeapTupleData tuple;
	HeapTuple	copyTuple = NULL;
	SnapshotData SnapshotDirty;
	bool		endNode;

	Assert(rti != 0);

	/*
	 * find relation containing target tuple
	 */
	if (estate->es_result_relation_info != NULL &&
		estate->es_result_relation_info->ri_RangeTableIndex == rti)
		relation = estate->es_result_relation_info->ri_RelationDesc;
	else
	{
		ListCell   *l;

		relation = NULL;
		foreach(l, estate->es_rowMarks)
		{
			ExecRowMark *erm = lfirst(l);

			if (erm->rti == rti)
			{
				relation = erm->relation;
				break;
			}
		}
		if (relation == NULL)
			elog(ERROR, "could not find RowMark for RT index %u", rti);
	}

	/*
	 * fetch tid tuple
	 *
	 * Loop here to deal with updated or busy tuples
	 */
	InitDirtySnapshot(SnapshotDirty);
	tuple.t_self = *tid;
	for (;;)
	{
		Buffer		buffer;

		if (heap_fetch(relation, &SnapshotDirty, &tuple, &buffer, true, NULL))
		{
			/*
			 * If xmin isn't what we're expecting, the slot must have been
			 * recycled and reused for an unrelated tuple.	This implies that
			 * the latest version of the row was deleted, so we need do
			 * nothing.  (Should be safe to examine xmin without getting
			 * buffer's content lock, since xmin never changes in an existing
			 * tuple.)
			 */
			if (!TransactionIdEquals(HeapTupleHeaderGetXmin(tuple.t_data),
									 priorXmax))
			{
				ReleaseBuffer(buffer);
				return NULL;
			}

			/* otherwise xmin should not be dirty... */
			if (TransactionIdIsValid(SnapshotDirty.xmin))
				elog(ERROR, "t_xmin is uncommitted in tuple to be updated");

			/*
			 * If tuple is being updated by other transaction then we have to
			 * wait for its commit/abort.
			 */
			if (TransactionIdIsValid(SnapshotDirty.xmax))
			{
				ReleaseBuffer(buffer);
				XactLockTableWait(SnapshotDirty.xmax);
				continue;		/* loop back to repeat heap_fetch */
			}

			/*
			 * If tuple was inserted by our own transaction, we have to check
			 * cmin against es_output_cid: cmin >= current CID means our
			 * command cannot see the tuple, so we should ignore it.  Without
			 * this we are open to the "Halloween problem" of indefinitely
			 * re-updating the same tuple. (We need not check cmax because
			 * HeapTupleSatisfiesDirty will consider a tuple deleted by our
			 * transaction dead, regardless of cmax.)  We just checked that
			 * priorXmax == xmin, so we can test that variable instead of
			 * doing HeapTupleHeaderGetXmin again.
			 */
			if (TransactionIdIsCurrentTransactionId(priorXmax) &&
				HeapTupleHeaderGetCmin(tuple.t_data) >= estate->es_output_cid)
			{
				ReleaseBuffer(buffer);
				return NULL;
			}

			/*
			 * We got tuple - now copy it for use by recheck query.
			 */
			copyTuple = heap_copytuple(&tuple);
			ReleaseBuffer(buffer);
			break;
		}

		/*
		 * If the referenced slot was actually empty, the latest version of
		 * the row must have been deleted, so we need do nothing.
		 */
		if (tuple.t_data == NULL)
		{
			ReleaseBuffer(buffer);
			return NULL;
		}

		/*
		 * As above, if xmin isn't what we're expecting, do nothing.
		 */
		if (!TransactionIdEquals(HeapTupleHeaderGetXmin(tuple.t_data),
								 priorXmax))
		{
			ReleaseBuffer(buffer);
			return NULL;
		}

		/*
		 * If we get here, the tuple was found but failed SnapshotDirty.
		 * Assuming the xmin is either a committed xact or our own xact (as it
		 * certainly should be if we're trying to modify the tuple), this must
		 * mean that the row was updated or deleted by either a committed xact
		 * or our own xact.  If it was deleted, we can ignore it; if it was
		 * updated then chain up to the next version and repeat the whole
		 * test.
		 *
		 * As above, it should be safe to examine xmax and t_ctid without the
		 * buffer content lock, because they can't be changing.
		 */
		if (ItemPointerEquals(&tuple.t_self, &tuple.t_data->t_ctid))
		{
			/* deleted, so forget about it */
			ReleaseBuffer(buffer);
			return NULL;
		}

		/* updated, so look at the updated row */
		tuple.t_self = tuple.t_data->t_ctid;
		/* updated row should have xmin matching this xmax */
		priorXmax = HeapTupleHeaderGetXmax(tuple.t_data);
		ReleaseBuffer(buffer);
		/* loop back to fetch next in chain */
	}

	/*
	 * For UPDATE/DELETE we have to return tid of actual row we're executing
	 * PQ for.
	 */
	*tid = tuple.t_self;

	/*
	 * Need to run a recheck subquery.	Find or create a PQ stack entry.
	 */
	epq = estate->es_evalPlanQual;
	endNode = true;

	if (epq != NULL && epq->rti == 0)
	{
		/* Top PQ stack entry is idle, so re-use it */
		Assert(!(estate->es_useEvalPlan) && epq->next == NULL);
		epq->rti = rti;
		endNode = false;
	}

	/*
	 * If this is request for another RTE - Ra, - then we have to check wasn't
	 * PlanQual requested for Ra already and if so then Ra' row was updated
	 * again and we have to re-start old execution for Ra and forget all what
	 * we done after Ra was suspended. Cool? -:))
	 */
	if (epq != NULL && epq->rti != rti &&
		epq->estate->es_evTuple[rti - 1] != NULL)
	{
		do
		{
			evalPlanQual *oldepq;

			/* stop execution */
			EvalPlanQualStop(epq);
			/* pop previous PlanQual from the stack */
			oldepq = epq->next;
			Assert(oldepq && oldepq->rti != 0);
			/* push current PQ to freePQ stack */
			oldepq->free = epq;
			epq = oldepq;
			estate->es_evalPlanQual = epq;
		} while (epq->rti != rti);
	}

	/*
	 * If we are requested for another RTE then we have to suspend execution
	 * of current PlanQual and start execution for new one.
	 */
	if (epq == NULL || epq->rti != rti)
	{
		/* try to reuse plan used previously */
		evalPlanQual *newepq = (epq != NULL) ? epq->free : NULL;

		if (newepq == NULL)		/* first call or freePQ stack is empty */
		{
			newepq = (evalPlanQual *) palloc0(sizeof(evalPlanQual));
			newepq->free = NULL;
			newepq->estate = NULL;
			newepq->planstate = NULL;
		}
		else
		{
			/* recycle previously used PlanQual */
			Assert(newepq->estate == NULL);
			epq->free = NULL;
		}
		/* push current PQ to the stack */
		newepq->next = epq;
		epq = newepq;
		estate->es_evalPlanQual = epq;
		epq->rti = rti;
		endNode = false;
	}

	Assert(epq->rti == rti);

	/*
	 * Ok - we're requested for the same RTE.  Unfortunately we still have to
	 * end and restart execution of the plan, because ExecReScan wouldn't
	 * ensure that upper plan nodes would reset themselves.  We could make
	 * that work if insertion of the target tuple were integrated with the
	 * Param mechanism somehow, so that the upper plan nodes know that their
	 * children's outputs have changed.
	 *
	 * Note that the stack of free evalPlanQual nodes is quite useless at the
	 * moment, since it only saves us from pallocing/releasing the
	 * evalPlanQual nodes themselves.  But it will be useful once we implement
	 * ReScan instead of end/restart for re-using PlanQual nodes.
	 */
	if (endNode)
	{
		/* stop execution */
		EvalPlanQualStop(epq);
	}

	/*
	 * Initialize new recheck query.
	 *
	 * Note: if we were re-using PlanQual plans via ExecReScan, we'd need to
	 * instead copy down changeable state from the top plan (including
	 * es_result_relation_info) and reset locally changeable
	 * state in the epq (including es_param_exec_vals, es_evTupleNull).
	 */
	EvalPlanQualStart(epq, estate, subplanstate->plan, epq->next);

	/*
	 * free old RTE' tuple, if any, and store target tuple where relation's
	 * scan node will see it
	 */
	epqstate = epq->estate;
	if (epqstate->es_evTuple[rti - 1] != NULL)
		heap_freetuple(epqstate->es_evTuple[rti - 1]);
	epqstate->es_evTuple[rti - 1] = copyTuple;

	return EvalPlanQualNext(estate);
}

static TupleTableSlot *
EvalPlanQualNext(EState *estate)
{
	evalPlanQual *epq = estate->es_evalPlanQual;
	MemoryContext oldcontext;
	TupleTableSlot *slot;

	Assert(epq->rti != 0);

lpqnext:;
	oldcontext = MemoryContextSwitchTo(epq->estate->es_query_cxt);
	slot = ExecProcNode(epq->planstate);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * No more tuples for this PQ. Continue previous one.
	 */
	if (TupIsNull(slot))
	{
		evalPlanQual *oldepq;

		/* stop execution */
		EvalPlanQualStop(epq);
		/* pop old PQ from the stack */
		oldepq = epq->next;
		if (oldepq == NULL)
		{
			/* this is the first (oldest) PQ - mark as free */
			epq->rti = 0;
			estate->es_useEvalPlan = false;
			/* and continue Query execution */
			return NULL;
		}
		Assert(oldepq->rti != 0);
		/* push current PQ to freePQ stack */
		oldepq->free = epq;
		epq = oldepq;
		estate->es_evalPlanQual = epq;
		goto lpqnext;
	}

	return slot;
}

static void
EndEvalPlanQual(EState *estate)
{
	evalPlanQual *epq = estate->es_evalPlanQual;

	if (epq->rti == 0)			/* plans already shutdowned */
	{
		Assert(epq->next == NULL);
		return;
	}

	for (;;)
	{
		evalPlanQual *oldepq;

		/* stop execution */
		EvalPlanQualStop(epq);
		/* pop old PQ from the stack */
		oldepq = epq->next;
		if (oldepq == NULL)
		{
			/* this is the first (oldest) PQ - mark as free */
			epq->rti = 0;
			estate->es_useEvalPlan = false;
			break;
		}
		Assert(oldepq->rti != 0);
		/* push current PQ to freePQ stack */
		oldepq->free = epq;
		epq = oldepq;
		estate->es_evalPlanQual = epq;
	}
}

/*
 * Start execution of one level of PlanQual.
 *
 * This is a cut-down version of ExecutorStart(): we copy some state from
 * the top-level estate rather than initializing it fresh.
 */
static void
EvalPlanQualStart(evalPlanQual *epq, EState *estate, Plan *planTree,
				  evalPlanQual *priorepq)
{
	EState	   *epqstate;
	int			rtsize;
	MemoryContext oldcontext;
	ListCell   *l;

	rtsize = list_length(estate->es_range_table);

	epq->estate = epqstate = CreateExecutorState();

	oldcontext = MemoryContextSwitchTo(epqstate->es_query_cxt);

	/*
	 * The epqstates share the top query's copy of unchanging state such as
	 * the snapshot, rangetable, result-rel info, and external Param info.
	 * They need their own copies of local state, including a tuple table,
	 * es_param_exec_vals, etc.
	 */
	epqstate->es_direction = ForwardScanDirection;
	epqstate->es_snapshot = estate->es_snapshot;
	epqstate->es_crosscheck_snapshot = estate->es_crosscheck_snapshot;
	epqstate->es_range_table = estate->es_range_table;
	epqstate->es_output_cid = estate->es_output_cid;
	epqstate->es_result_relations = estate->es_result_relations;
	epqstate->es_num_result_relations = estate->es_num_result_relations;
	epqstate->es_result_relation_info = estate->es_result_relation_info;
	epqstate->es_junkFilter = estate->es_junkFilter;
	/* es_trig_target_relations must NOT be copied */
	epqstate->es_param_list_info = estate->es_param_list_info;
	if (estate->es_plannedstmt->nParamExec > 0)
		epqstate->es_param_exec_vals = (ParamExecData *)
			palloc0(estate->es_plannedstmt->nParamExec * sizeof(ParamExecData));
	epqstate->es_rowMarks = estate->es_rowMarks;
	epqstate->es_instrument = estate->es_instrument;
	epqstate->es_select_into = estate->es_select_into;
	epqstate->es_into_oids = estate->es_into_oids;
	epqstate->es_plannedstmt = estate->es_plannedstmt;

	/*
	 * Each epqstate must have its own es_evTupleNull state, but all the stack
	 * entries share es_evTuple state.	This allows sub-rechecks to inherit
	 * the value being examined by an outer recheck.
	 */
	epqstate->es_evTupleNull = (bool *) palloc0(rtsize * sizeof(bool));
	if (priorepq == NULL)
		/* first PQ stack entry */
		epqstate->es_evTuple = (HeapTuple *)
			palloc0(rtsize * sizeof(HeapTuple));
	else
		/* later stack entries share the same storage */
		epqstate->es_evTuple = priorepq->estate->es_evTuple;

	/*
	 * Each epqstate also has its own tuple table.
	 */
	epqstate->es_tupleTable = NIL;

	/*
	 * Initialize private state information for each SubPlan.  We must do this
	 * before running ExecInitNode on the main query tree, since
	 * ExecInitSubPlan expects to be able to find these entries.
	 * Some of the SubPlans might not be used in the part of the plan tree
	 * we intend to run, but since it's not easy to tell which, we just
	 * initialize them all.
	 */
	Assert(epqstate->es_subplanstates == NIL);
	foreach(l, estate->es_plannedstmt->subplans)
	{
		Plan	   *subplan = (Plan *) lfirst(l);
		PlanState  *subplanstate;

		subplanstate = ExecInitNode(subplan, epqstate, 0);

		epqstate->es_subplanstates = lappend(epqstate->es_subplanstates,
											 subplanstate);
	}

	/*
	 * Initialize the private state information for all the nodes in the
	 * part of the plan tree we need to run.  This opens files, allocates
	 * storage and leaves us ready to start processing tuples.
	 */
	epq->planstate = ExecInitNode(planTree, epqstate, 0);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * End execution of one level of PlanQual.
 *
 * This is a cut-down version of ExecutorEnd(); basically we want to do most
 * of the normal cleanup, but *not* close result relations (which we are
 * just sharing from the outer query).	We do, however, have to close any
 * trigger target relations that got opened, since those are not shared.
 */
static void
EvalPlanQualStop(evalPlanQual *epq)
{
	EState	   *epqstate = epq->estate;
	MemoryContext oldcontext;
	ListCell   *l;

	oldcontext = MemoryContextSwitchTo(epqstate->es_query_cxt);

	ExecEndNode(epq->planstate);

	foreach(l, epqstate->es_subplanstates)
	{
		PlanState  *subplanstate = (PlanState *) lfirst(l);

		ExecEndNode(subplanstate);
	}

	/* throw away the per-epqstate tuple table completely */
	ExecResetTupleTable(epqstate->es_tupleTable, true);
	epqstate->es_tupleTable = NIL;

	if (epqstate->es_evTuple[epq->rti - 1] != NULL)
	{
		heap_freetuple(epqstate->es_evTuple[epq->rti - 1]);
		epqstate->es_evTuple[epq->rti - 1] = NULL;
	}

	foreach(l, epqstate->es_trig_target_relations)
	{
		ResultRelInfo *resultRelInfo = (ResultRelInfo *) lfirst(l);

		/* Close indices and then the relation itself */
		ExecCloseIndices(resultRelInfo);
		heap_close(resultRelInfo->ri_RelationDesc, NoLock);
	}

	MemoryContextSwitchTo(oldcontext);

	FreeExecutorState(epqstate);

	epq->estate = NULL;
	epq->planstate = NULL;
}

/*
 * ExecGetActivePlanTree --- get the active PlanState tree from a QueryDesc
 *
 * Ordinarily this is just the one mentioned in the QueryDesc, but if we
 * are looking at a row returned by the EvalPlanQual machinery, we need
 * to look at the subsidiary state instead.
 */
PlanState *
ExecGetActivePlanTree(QueryDesc *queryDesc)
{
	EState	   *estate = queryDesc->estate;

	if (estate && estate->es_useEvalPlan && estate->es_evalPlanQual != NULL)
		return estate->es_evalPlanQual->planstate;
	else
		return queryDesc->planstate;
}


/*
 * Support for SELECT INTO (a/k/a CREATE TABLE AS)
 *
 * We implement SELECT INTO by diverting SELECT's normal output with
 * a specialized DestReceiver type.
 */

typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	EState	   *estate;			/* EState we are working with */
	Relation	rel;			/* Relation to write to */
	int			hi_options;		/* heap_insert performance options */
	BulkInsertState bistate;	/* bulk insert state */
} DR_intorel;

/*
 * OpenIntoRel --- actually create the SELECT INTO target relation
 *
 * This also replaces QueryDesc->dest with the special DestReceiver for
 * SELECT INTO.  We assume that the correct result tuple type has already
 * been placed in queryDesc->tupDesc.
 */
static void
OpenIntoRel(QueryDesc *queryDesc)
{
	IntoClause *into = queryDesc->plannedstmt->intoClause;
	EState	   *estate = queryDesc->estate;
	Relation	intoRelationDesc;
	char	   *intoName;
	Oid			namespaceId;
	Oid			tablespaceId;
	Datum		reloptions;
	AclResult	aclresult;
	Oid			intoRelationId;
	TupleDesc	tupdesc;
	DR_intorel *myState;
	static char *validnsps[] = HEAP_RELOPT_NAMESPACES;

	Assert(into);

	/*
	 * Check consistency of arguments
	 */
	if (into->onCommit != ONCOMMIT_NOOP && !into->rel->istemp)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("ON COMMIT can only be used on temporary tables")));

	/*
	 * Find namespace to create in, check its permissions
	 */
	intoName = into->rel->relname;
	namespaceId = RangeVarGetCreationNamespace(into->rel);

	aclresult = pg_namespace_aclcheck(namespaceId, GetUserId(),
									  ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(namespaceId));

	/*
	 * Select tablespace to use.  If not specified, use default tablespace
	 * (which may in turn default to database's default).
	 */
	if (into->tableSpaceName)
	{
		tablespaceId = get_tablespace_oid(into->tableSpaceName);
		if (!OidIsValid(tablespaceId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tablespace \"%s\" does not exist",
							into->tableSpaceName)));
	}
	else
	{
		tablespaceId = GetDefaultTablespace(into->rel->istemp);
		/* note InvalidOid is OK in this case */
	}

	/* Check permissions except when using the database's default space */
	if (OidIsValid(tablespaceId) && tablespaceId != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = pg_tablespace_aclcheck(tablespaceId, GetUserId(),
										   ACL_CREATE);

		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_TABLESPACE,
						   get_tablespace_name(tablespaceId));
	}

	/* Parse and validate any reloptions */
	reloptions = transformRelOptions((Datum) 0,
									 into->options,
									 NULL,
									 validnsps,
									 true,
									 false);
	(void) heap_reloptions(RELKIND_RELATION, reloptions, true);

	/* Copy the tupdesc because heap_create_with_catalog modifies it */
	tupdesc = CreateTupleDescCopy(queryDesc->tupDesc);

	/* Now we can actually create the new relation */
	intoRelationId = heap_create_with_catalog(intoName,
											  namespaceId,
											  tablespaceId,
											  InvalidOid,
											  InvalidOid,
											  GetUserId(),
											  tupdesc,
											  NIL,
											  RELKIND_RELATION,
											  false,
											  true,
											  0,
											  into->onCommit,
											  reloptions,
											  true,
											  allowSystemTableMods);

	FreeTupleDesc(tupdesc);

	/*
	 * Advance command counter so that the newly-created relation's catalog
	 * tuples will be visible to heap_open.
	 */
	CommandCounterIncrement();

	/*
	 * If necessary, create a TOAST table for the INTO relation. Note that
	 * AlterTableCreateToastTable ends with CommandCounterIncrement(), so that
	 * the TOAST table will be visible for insertion.
	 */
	reloptions = transformRelOptions((Datum) 0,
									 into->options,
									 "toast",
									 validnsps,
									 true,
									 false);

	(void) heap_reloptions(RELKIND_TOASTVALUE, reloptions, true);

	AlterTableCreateToastTable(intoRelationId, InvalidOid, reloptions, false);

	/*
	 * And open the constructed table for writing.
	 */
	intoRelationDesc = heap_open(intoRelationId, AccessExclusiveLock);

	/*
	 * Now replace the query's DestReceiver with one for SELECT INTO
	 */
	queryDesc->dest = CreateDestReceiver(DestIntoRel);
	myState = (DR_intorel *) queryDesc->dest;
	Assert(myState->pub.mydest == DestIntoRel);
	myState->estate = estate;
	myState->rel = intoRelationDesc;

	/*
	 * We can skip WAL-logging the insertions, unless PITR is in use.  We can
	 * skip the FSM in any case.
	 */
	myState->hi_options = HEAP_INSERT_SKIP_FSM |
		(XLogArchivingActive() ? 0 : HEAP_INSERT_SKIP_WAL);
	myState->bistate = GetBulkInsertState();

	/* Not using WAL requires rd_targblock be initially invalid */
	Assert(intoRelationDesc->rd_targblock == InvalidBlockNumber);
}

/*
 * CloseIntoRel --- clean up SELECT INTO at ExecutorEnd time
 */
static void
CloseIntoRel(QueryDesc *queryDesc)
{
	DR_intorel *myState = (DR_intorel *) queryDesc->dest;

	/* OpenIntoRel might never have gotten called */
	if (myState && myState->pub.mydest == DestIntoRel && myState->rel)
	{
		FreeBulkInsertState(myState->bistate);

		/* If we skipped using WAL, must heap_sync before commit */
		if (myState->hi_options & HEAP_INSERT_SKIP_WAL)
			heap_sync(myState->rel);

		/* close rel, but keep lock until commit */
		heap_close(myState->rel, NoLock);

		myState->rel = NULL;
	}
}

/*
 * CreateIntoRelDestReceiver -- create a suitable DestReceiver object
 */
DestReceiver *
CreateIntoRelDestReceiver(void)
{
	DR_intorel *self = (DR_intorel *) palloc0(sizeof(DR_intorel));

	self->pub.receiveSlot = intorel_receive;
	self->pub.rStartup = intorel_startup;
	self->pub.rShutdown = intorel_shutdown;
	self->pub.rDestroy = intorel_destroy;
	self->pub.mydest = DestIntoRel;

	/* private fields will be set by OpenIntoRel */

	return (DestReceiver *) self;
}

/*
 * intorel_startup --- executor startup
 */
static void
intorel_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	/* no-op */
}

/*
 * intorel_receive --- receive one tuple
 */
static void
intorel_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_intorel *myState = (DR_intorel *) self;
	HeapTuple	tuple;

	/*
	 * get the heap tuple out of the tuple table slot, making sure we have a
	 * writable copy
	 */
	tuple = ExecMaterializeSlot(slot);

	/*
	 * force assignment of new OID (see comments in ExecInsert)
	 */
	if (myState->rel->rd_rel->relhasoids)
		HeapTupleSetOid(tuple, InvalidOid);

	heap_insert(myState->rel,
				tuple,
				myState->estate->es_output_cid,
				myState->hi_options,
				myState->bistate);

	/* We know this is a newly created relation, so there are no indexes */
}

/*
 * intorel_shutdown --- executor end
 */
static void
intorel_shutdown(DestReceiver *self)
{
	/* no-op */
}

/*
 * intorel_destroy --- release DestReceiver object
 */
static void
intorel_destroy(DestReceiver *self)
{
	pfree(self);
}
