/*-------------------------------------------------------------------------
 *
 * execMain.c
 *	  top level executor interface routines
 *
 * INTERFACE ROUTINES
 *	ExecutorStart()
 *	ExecutorRun()
 *	ExecutorFinish()
 *	ExecutorEnd()
 *
 *	These four procedures are the external interface to the executor.
 *	In each case, the query descriptor is required as an argument.
 *
 *	ExecutorStart must be called at the beginning of execution of any
 *	query plan and ExecutorEnd must always be called at the end of
 *	execution of a plan (unless it is aborted due to error).
 *
 *	ExecutorRun accepts direction and count arguments that specify whether
 *	the plan is to be executed forwards, backwards, and for how many tuples.
 *	In some cases ExecutorRun may be called multiple times to process all
 *	the tuples for a plan.  It is also acceptable to stop short of executing
 *	the whole plan (but only if it is a SELECT).
 *
 *	ExecutorFinish must be called after the final ExecutorRun call and
 *	before ExecutorEnd.  This can be omitted only in case of EXPLAIN,
 *	which should also omit ExecutorRun.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execMain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/matview.h"
#include "commands/trigger.h"
#include "executor/execdebug.h"
#include "foreign/fdwapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "parser/parsetree.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/tqual.h"


/* Hooks for plugins to get control in ExecutorStart/Run/Finish/End */
ExecutorStart_hook_type ExecutorStart_hook = NULL;
ExecutorRun_hook_type ExecutorRun_hook = NULL;
ExecutorFinish_hook_type ExecutorFinish_hook = NULL;
ExecutorEnd_hook_type ExecutorEnd_hook = NULL;

/* Hook for plugin to get control in ExecCheckRTPerms() */
ExecutorCheckPerms_hook_type ExecutorCheckPerms_hook = NULL;

/* decls for local routines only used within this module */
static void InitPlan(QueryDesc *queryDesc, int eflags);
static void CheckValidRowMarkRel(Relation rel, RowMarkType markType);
static void ExecPostprocessPlan(EState *estate);
static void ExecEndPlan(PlanState *planstate, EState *estate);
static void ExecutePlan(EState *estate, PlanState *planstate,
			bool use_parallel_mode,
			CmdType operation,
			bool sendTuples,
			long numberTuples,
			ScanDirection direction,
			DestReceiver *dest);
static bool ExecCheckRTEPerms(RangeTblEntry *rte);
static bool ExecCheckRTEPermsModified(Oid relOid, Oid userid,
						  Bitmapset *modifiedCols,
						  AclMode requiredPerms);
static void ExecCheckXactReadOnly(PlannedStmt *plannedstmt);
static char *ExecBuildSlotValueDescription(Oid reloid,
							  TupleTableSlot *slot,
							  TupleDesc tupdesc,
							  Bitmapset *modifiedCols,
							  int maxfieldlen);
static void EvalPlanQualStart(EPQState *epqstate, EState *parentestate,
				  Plan *planTree);

/*
 * Note that GetUpdatedColumns() also exists in commands/trigger.c.  There does
 * not appear to be any good header to put it into, given the structures that
 * it uses, so we let them be duplicated.  Be sure to update both if one needs
 * to be changed, however.
 */
#define GetInsertedColumns(relinfo, estate) \
	(rt_fetch((relinfo)->ri_RangeTableIndex, (estate)->es_range_table)->insertedCols)
#define GetUpdatedColumns(relinfo, estate) \
	(rt_fetch((relinfo)->ri_RangeTableIndex, (estate)->es_range_table)->updatedCols)

/* end of local decls */


/* ----------------------------------------------------------------
 *		ExecutorStart
 *
 *		This routine must be called at the beginning of any execution of any
 *		query plan
 *
 * Takes a QueryDesc previously created by CreateQueryDesc (which is separate
 * only because some places use QueryDescs for utility commands).  The tupDesc
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
	 *
	 * Don't allow writes in parallel mode.  Supporting UPDATE and DELETE
	 * would require (a) storing the combocid hash in shared memory, rather
	 * than synchronizing it just once at the start of parallelism, and (b) an
	 * alternative to heap_update()'s reliance on xmax for mutual exclusion.
	 * INSERT may have no such troubles, but we forbid it to simplify the
	 * checks.
	 *
	 * We have lower-level defenses in CommandCounterIncrement and elsewhere
	 * against performing unsafe operations in parallel mode, but this gives a
	 * more user-friendly error message.
	 */
	if ((XactReadOnly || IsInParallelMode()) &&
		!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		ExecCheckXactReadOnly(queryDesc->plannedstmt);

	/*
	 * Build EState, switch into per-query memory context for startup.
	 */
	estate = CreateExecutorState();
	queryDesc->estate = estate;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/*
	 * Fill in external parameters, if any, from queryDesc; and allocate
	 * workspace for internal parameters
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

			/*
			 * SELECT FOR [KEY] UPDATE/SHARE and modifying CTEs need to mark
			 * tuples
			 */
			if (queryDesc->plannedstmt->rowMarks != NIL ||
				queryDesc->plannedstmt->hasModifyingCTE)
				estate->es_output_cid = GetCurrentCommandId(true);

			/*
			 * A SELECT without modifying CTEs can't possibly queue triggers,
			 * so force skip-triggers mode. This is just a marginal efficiency
			 * hack, since AfterTriggerBeginQuery/AfterTriggerEndQuery aren't
			 * all that expensive, but we might as well do it.
			 */
			if (!queryDesc->plannedstmt->hasModifyingCTE)
				eflags |= EXEC_FLAG_SKIP_TRIGGERS;
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
	estate->es_top_eflags = eflags;
	estate->es_instrument = queryDesc->instrument_options;

	/*
	 * Initialize the plan state tree
	 */
	InitPlan(queryDesc, eflags);

	/*
	 * Set up an AFTER-trigger statement context, unless told not to, or
	 * unless it's EXPLAIN-only mode (when ExecutorFinish won't be called).
	 */
	if (!(eflags & (EXEC_FLAG_SKIP_TRIGGERS | EXEC_FLAG_EXPLAIN_ONLY)))
		AfterTriggerBeginQuery();

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
 *		completion.  Also note that the count limit is only applied to
 *		retrieved tuples, not for instance to those inserted/updated/deleted
 *		by a ModifyTable plan node.
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
	Assert(!(estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY));

	/*
	 * Switch into per-query memory context
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/* Allow instrumentation of Executor overall runtime */
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
					queryDesc->plannedstmt->parallelModeNeeded,
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
 *		ExecutorFinish
 *
 *		This routine must be called after the last ExecutorRun call.
 *		It performs cleanup such as firing AFTER triggers.  It is
 *		separate from ExecutorEnd because EXPLAIN ANALYZE needs to
 *		include these actions in the total runtime.
 *
 *		We provide a function hook variable that lets loadable plugins
 *		get control when ExecutorFinish is called.  Such a plugin would
 *		normally call standard_ExecutorFinish().
 *
 * ----------------------------------------------------------------
 */
void
ExecutorFinish(QueryDesc *queryDesc)
{
	if (ExecutorFinish_hook)
		(*ExecutorFinish_hook) (queryDesc);
	else
		standard_ExecutorFinish(queryDesc);
}

void
standard_ExecutorFinish(QueryDesc *queryDesc)
{
	EState	   *estate;
	MemoryContext oldcontext;

	/* sanity checks */
	Assert(queryDesc != NULL);

	estate = queryDesc->estate;

	Assert(estate != NULL);
	Assert(!(estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY));

	/* This should be run once and only once per Executor instance */
	Assert(!estate->es_finished);

	/* Switch into per-query memory context */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/* Allow instrumentation of Executor overall runtime */
	if (queryDesc->totaltime)
		InstrStartNode(queryDesc->totaltime);

	/* Run ModifyTable nodes to completion */
	ExecPostprocessPlan(estate);

	/* Execute queued AFTER triggers, unless told not to */
	if (!(estate->es_top_eflags & EXEC_FLAG_SKIP_TRIGGERS))
		AfterTriggerEndQuery(estate);

	if (queryDesc->totaltime)
		InstrStopNode(queryDesc->totaltime, 0);

	MemoryContextSwitchTo(oldcontext);

	estate->es_finished = true;
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
	 * Check that ExecutorFinish was called, unless in EXPLAIN-only mode. This
	 * Assert is needed because ExecutorFinish is new as of 9.1, and callers
	 * might forget to call it.
	 */
	Assert(estate->es_finished ||
		   (estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY));

	/*
	 * Switch into per-query memory context to run ExecEndPlan
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	ExecEndPlan(queryDesc->planstate, estate);

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
	ExecReScan(queryDesc->planstate);

	MemoryContextSwitchTo(oldcontext);
}


/*
 * ExecCheckRTPerms
 *		Check access permissions for all relations listed in a range table.
 *
 * Returns true if permissions are adequate.  Otherwise, throws an appropriate
 * error if ereport_on_violation is true, or simply returns false otherwise.
 *
 * Note that this does NOT address row level security policies (aka: RLS).  If
 * rows will be returned to the user as a result of this permission check
 * passing, then RLS also needs to be consulted (and check_enable_rls()).
 *
 * See rewrite/rowsecurity.c.
 */
bool
ExecCheckRTPerms(List *rangeTable, bool ereport_on_violation)
{
	ListCell   *l;
	bool		result = true;

	foreach(l, rangeTable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		result = ExecCheckRTEPerms(rte);
		if (!result)
		{
			Assert(rte->rtekind == RTE_RELATION);
			if (ereport_on_violation)
				aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_CLASS,
							   get_rel_name(rte->relid));
			return false;
		}
	}

	if (ExecutorCheckPerms_hook)
		result = (*ExecutorCheckPerms_hook) (rangeTable,
											 ereport_on_violation);
	return result;
}

/*
 * ExecCheckRTEPerms
 *		Check access permissions for a single RTE.
 */
static bool
ExecCheckRTEPerms(RangeTblEntry *rte)
{
	AclMode		requiredPerms;
	AclMode		relPerms;
	AclMode		remainingPerms;
	Oid			relOid;
	Oid			userid;

	/*
	 * Only plain-relation RTEs need to be checked here.  Function RTEs are
	 * checked by init_fcache when the function is prepared for execution.
	 * Join, subquery, and special RTEs need no checks.
	 */
	if (rte->rtekind != RTE_RELATION)
		return true;

	/*
	 * No work if requiredPerms is empty.
	 */
	requiredPerms = rte->requiredPerms;
	if (requiredPerms == 0)
		return true;

	relOid = rte->relid;

	/*
	 * userid to check as: current user unless we have a setuid indication.
	 *
	 * Note: GetUserId() is presently fast enough that there's no harm in
	 * calling it separately for each RTE.  If that stops being true, we could
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
		int			col = -1;

		/*
		 * If we lack any permissions that exist only as relation permissions,
		 * we can fail straight away.
		 */
		if (remainingPerms & ~(ACL_SELECT | ACL_INSERT | ACL_UPDATE))
			return false;

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
					return false;
			}

			while ((col = bms_next_member(rte->selectedCols, col)) >= 0)
			{
				/* bit #s are offset by FirstLowInvalidHeapAttributeNumber */
				AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;

				if (attno == InvalidAttrNumber)
				{
					/* Whole-row reference, must have priv on all cols */
					if (pg_attribute_aclcheck_all(relOid, userid, ACL_SELECT,
												  ACLMASK_ALL) != ACLCHECK_OK)
						return false;
				}
				else
				{
					if (pg_attribute_aclcheck(relOid, attno, userid,
											  ACL_SELECT) != ACLCHECK_OK)
						return false;
				}
			}
		}

		/*
		 * Basically the same for the mod columns, for both INSERT and UPDATE
		 * privilege as specified by remainingPerms.
		 */
		if (remainingPerms & ACL_INSERT && !ExecCheckRTEPermsModified(relOid,
																	  userid,
														   rte->insertedCols,
																 ACL_INSERT))
			return false;

		if (remainingPerms & ACL_UPDATE && !ExecCheckRTEPermsModified(relOid,
																	  userid,
															rte->updatedCols,
																 ACL_UPDATE))
			return false;
	}
	return true;
}

/*
 * ExecCheckRTEPermsModified
 *		Check INSERT or UPDATE access permissions for a single RTE (these
 *		are processed uniformly).
 */
static bool
ExecCheckRTEPermsModified(Oid relOid, Oid userid, Bitmapset *modifiedCols,
						  AclMode requiredPerms)
{
	int			col = -1;

	/*
	 * When the query doesn't explicitly update any columns, allow the query
	 * if we have permission on any column of the rel.  This is to handle
	 * SELECT FOR UPDATE as well as possible corner cases in UPDATE.
	 */
	if (bms_is_empty(modifiedCols))
	{
		if (pg_attribute_aclcheck_all(relOid, userid, requiredPerms,
									  ACLMASK_ANY) != ACLCHECK_OK)
			return false;
	}

	while ((col = bms_next_member(modifiedCols, col)) >= 0)
	{
		/* bit #s are offset by FirstLowInvalidHeapAttributeNumber */
		AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;

		if (attno == InvalidAttrNumber)
		{
			/* whole-row reference can't happen here */
			elog(ERROR, "whole-row update is not implemented");
		}
		else
		{
			if (pg_attribute_aclcheck(relOid, attno, userid,
									  requiredPerms) != ACLCHECK_OK)
				return false;
		}
	}
	return true;
}

/*
 * Check that the query does not imply any writes to non-temp tables;
 * unless we're in parallel mode, in which case don't even allow writes
 * to temp tables.
 *
 * Note: in a Hot Standby slave this would need to reject writes to temp
 * tables just as we do in parallel mode; but an HS slave can't have created
 * any temp tables in the first place, so no need to check that.
 */
static void
ExecCheckXactReadOnly(PlannedStmt *plannedstmt)
{
	ListCell   *l;

	/*
	 * Fail if write permissions are requested in parallel mode for table
	 * (temp or non-temp), otherwise fail for any non-temp table.
	 */
	foreach(l, plannedstmt->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		if (rte->rtekind != RTE_RELATION)
			continue;

		if ((rte->requiredPerms & (~ACL_SELECT)) == 0)
			continue;

		if (isTempNamespace(get_rel_namespace(rte->relid)))
			continue;

		PreventCommandIfReadOnly(CreateCommandTag((Node *) plannedstmt));
	}

	if (plannedstmt->commandType != CMD_SELECT || plannedstmt->hasModifyingCTE)
		PreventCommandIfParallelMode(CreateCommandTag((Node *) plannedstmt));
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
	ExecCheckRTPerms(rangeTable, true);

	/*
	 * initialize the node's execution state
	 */
	estate->es_range_table = rangeTable;
	estate->es_plannedstmt = plannedstmt;

	/*
	 * initialize result relation stuff, and open/lock the result rels.
	 *
	 * We must do this before initializing the plan tree, else we might try to
	 * do a lock upgrade if a result rel is also a source rel.
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
	 * Similarly, we have to lock relations selected FOR [KEY] UPDATE/SHARE
	 * before we initialize the plan tree, else we'd be risking lock upgrades.
	 * While we are at it, build the ExecRowMark list.
	 */
	estate->es_rowMarks = NIL;
	foreach(l, plannedstmt->rowMarks)
	{
		PlanRowMark *rc = (PlanRowMark *) lfirst(l);
		Oid			relid;
		Relation	relation;
		ExecRowMark *erm;

		/* ignore "parent" rowmarks; they are irrelevant at runtime */
		if (rc->isParent)
			continue;

		/* get relation's OID (will produce InvalidOid if subquery) */
		relid = getrelid(rc->rti, rangeTable);

		/*
		 * If you change the conditions under which rel locks are acquired
		 * here, be sure to adjust ExecOpenScanRelation to match.
		 */
		switch (rc->markType)
		{
			case ROW_MARK_EXCLUSIVE:
			case ROW_MARK_NOKEYEXCLUSIVE:
			case ROW_MARK_SHARE:
			case ROW_MARK_KEYSHARE:
				relation = heap_open(relid, RowShareLock);
				break;
			case ROW_MARK_REFERENCE:
				relation = heap_open(relid, AccessShareLock);
				break;
			case ROW_MARK_COPY:
				/* no physical table access is required */
				relation = NULL;
				break;
			default:
				elog(ERROR, "unrecognized markType: %d", rc->markType);
				relation = NULL;	/* keep compiler quiet */
				break;
		}

		/* Check that relation is a legal target for marking */
		if (relation)
			CheckValidRowMarkRel(relation, rc->markType);

		erm = (ExecRowMark *) palloc(sizeof(ExecRowMark));
		erm->relation = relation;
		erm->relid = relid;
		erm->rti = rc->rti;
		erm->prti = rc->prti;
		erm->rowmarkId = rc->rowmarkId;
		erm->markType = rc->markType;
		erm->strength = rc->strength;
		erm->waitPolicy = rc->waitPolicy;
		erm->ermActive = false;
		ItemPointerSetInvalid(&(erm->curCtid));
		erm->ermExtra = NULL;
		estate->es_rowMarks = lappend(estate->es_rowMarks, erm);
	}

	/*
	 * Initialize the executor's tuple table to empty.
	 */
	estate->es_tupleTable = NIL;
	estate->es_trig_tuple_slot = NULL;
	estate->es_trig_oldtup_slot = NULL;
	estate->es_trig_newtup_slot = NULL;

	/* mark EvalPlanQual not active */
	estate->es_epqTuple = NULL;
	estate->es_epqTupleSet = NULL;
	estate->es_epqScanDone = NULL;

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
		sp_eflags = eflags
			& (EXEC_FLAG_EXPLAIN_ONLY | EXEC_FLAG_WITH_NO_DATA);
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
	 * Get the tuple descriptor describing the type of tuples to return.
	 */
	tupType = ExecGetResultType(planstate);

	/*
	 * Initialize the junk filter if needed.  SELECT queries need a filter if
	 * there are any junk attrs in the top-level tlist.
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
		}
	}

	queryDesc->tupDesc = tupType;
	queryDesc->planstate = planstate;
}

/*
 * Check that a proposed result relation is a legal target for the operation
 *
 * Generally the parser and/or planner should have noticed any such mistake
 * already, but let's make sure.
 *
 * Note: when changing this function, you probably also need to look at
 * CheckValidRowMarkRel.
 */
void
CheckValidResultRel(Relation resultRel, CmdType operation)
{
	TriggerDesc *trigDesc = resultRel->trigdesc;
	FdwRoutine *fdwroutine;

	switch (resultRel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
			/* OK */
			break;
		case RELKIND_SEQUENCE:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change sequence \"%s\"",
							RelationGetRelationName(resultRel))));
			break;
		case RELKIND_TOASTVALUE:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change TOAST relation \"%s\"",
							RelationGetRelationName(resultRel))));
			break;
		case RELKIND_VIEW:

			/*
			 * Okay only if there's a suitable INSTEAD OF trigger.  Messages
			 * here should match rewriteHandler.c's rewriteTargetView, except
			 * that we omit errdetail because we haven't got the information
			 * handy (and given that we really shouldn't get here anyway, it's
			 * not worth great exertion to get).
			 */
			switch (operation)
			{
				case CMD_INSERT:
					if (!trigDesc || !trigDesc->trig_insert_instead_row)
						ereport(ERROR,
						  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						   errmsg("cannot insert into view \"%s\"",
								  RelationGetRelationName(resultRel)),
						   errhint("To enable inserting into the view, provide an INSTEAD OF INSERT trigger or an unconditional ON INSERT DO INSTEAD rule.")));
					break;
				case CMD_UPDATE:
					if (!trigDesc || !trigDesc->trig_update_instead_row)
						ereport(ERROR,
						  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						   errmsg("cannot update view \"%s\"",
								  RelationGetRelationName(resultRel)),
						   errhint("To enable updating the view, provide an INSTEAD OF UPDATE trigger or an unconditional ON UPDATE DO INSTEAD rule.")));
					break;
				case CMD_DELETE:
					if (!trigDesc || !trigDesc->trig_delete_instead_row)
						ereport(ERROR,
						  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						   errmsg("cannot delete from view \"%s\"",
								  RelationGetRelationName(resultRel)),
						   errhint("To enable deleting from the view, provide an INSTEAD OF DELETE trigger or an unconditional ON DELETE DO INSTEAD rule.")));
					break;
				default:
					elog(ERROR, "unrecognized CmdType: %d", (int) operation);
					break;
			}
			break;
		case RELKIND_MATVIEW:
			if (!MatViewIncrementalMaintenanceIsEnabled())
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot change materialized view \"%s\"",
								RelationGetRelationName(resultRel))));
			break;
		case RELKIND_FOREIGN_TABLE:
			/* Okay only if the FDW supports it */
			fdwroutine = GetFdwRoutineForRelation(resultRel, false);
			switch (operation)
			{
				case CMD_INSERT:
					if (fdwroutine->ExecForeignInsert == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("cannot insert into foreign table \"%s\"",
								   RelationGetRelationName(resultRel))));
					if (fdwroutine->IsForeignRelUpdatable != NULL &&
						(fdwroutine->IsForeignRelUpdatable(resultRel) & (1 << CMD_INSERT)) == 0)
						ereport(ERROR,
						  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("foreign table \"%s\" does not allow inserts",
							   RelationGetRelationName(resultRel))));
					break;
				case CMD_UPDATE:
					if (fdwroutine->ExecForeignUpdate == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot update foreign table \"%s\"",
										RelationGetRelationName(resultRel))));
					if (fdwroutine->IsForeignRelUpdatable != NULL &&
						(fdwroutine->IsForeignRelUpdatable(resultRel) & (1 << CMD_UPDATE)) == 0)
						ereport(ERROR,
						  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("foreign table \"%s\" does not allow updates",
							   RelationGetRelationName(resultRel))));
					break;
				case CMD_DELETE:
					if (fdwroutine->ExecForeignDelete == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("cannot delete from foreign table \"%s\"",
								   RelationGetRelationName(resultRel))));
					if (fdwroutine->IsForeignRelUpdatable != NULL &&
						(fdwroutine->IsForeignRelUpdatable(resultRel) & (1 << CMD_DELETE)) == 0)
						ereport(ERROR,
						  (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("foreign table \"%s\" does not allow deletes",
							   RelationGetRelationName(resultRel))));
					break;
				default:
					elog(ERROR, "unrecognized CmdType: %d", (int) operation);
					break;
			}
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot change relation \"%s\"",
							RelationGetRelationName(resultRel))));
			break;
	}
}

/*
 * Check that a proposed rowmark target relation is a legal target
 *
 * In most cases parser and/or planner should have noticed this already, but
 * they don't cover all cases.
 */
static void
CheckValidRowMarkRel(Relation rel, RowMarkType markType)
{
	FdwRoutine *fdwroutine;

	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
			/* OK */
			break;
		case RELKIND_SEQUENCE:
			/* Must disallow this because we don't vacuum sequences */
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot lock rows in sequence \"%s\"",
							RelationGetRelationName(rel))));
			break;
		case RELKIND_TOASTVALUE:
			/* We could allow this, but there seems no good reason to */
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot lock rows in TOAST relation \"%s\"",
							RelationGetRelationName(rel))));
			break;
		case RELKIND_VIEW:
			/* Should not get here; planner should have expanded the view */
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot lock rows in view \"%s\"",
							RelationGetRelationName(rel))));
			break;
		case RELKIND_MATVIEW:
			/* Allow referencing a matview, but not actual locking clauses */
			if (markType != ROW_MARK_REFERENCE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					   errmsg("cannot lock rows in materialized view \"%s\"",
							  RelationGetRelationName(rel))));
			break;
		case RELKIND_FOREIGN_TABLE:
			/* Okay only if the FDW supports it */
			fdwroutine = GetFdwRoutineForRelation(rel, false);
			if (fdwroutine->RefetchForeignRow == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot lock rows in foreign table \"%s\"",
								RelationGetRelationName(rel))));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot lock rows in relation \"%s\"",
							RelationGetRelationName(rel))));
			break;
	}
}

/*
 * Initialize ResultRelInfo data for one result relation
 *
 * Caution: before Postgres 9.1, this function included the relkind checking
 * that's now in CheckValidResultRel, and it also did ExecOpenIndices if
 * appropriate.  Be sure callers cover those needs.
 */
void
InitResultRelInfo(ResultRelInfo *resultRelInfo,
				  Relation resultRelationDesc,
				  Index resultRelationIndex,
				  int instrument_options)
{
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
		resultRelInfo->ri_TrigWhenExprs = (List **)
			palloc0(n * sizeof(List *));
		if (instrument_options)
			resultRelInfo->ri_TrigInstrument = InstrAlloc(n, instrument_options);
	}
	else
	{
		resultRelInfo->ri_TrigFunctions = NULL;
		resultRelInfo->ri_TrigWhenExprs = NULL;
		resultRelInfo->ri_TrigInstrument = NULL;
	}
	if (resultRelationDesc->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		resultRelInfo->ri_FdwRoutine = GetFdwRoutineForRelation(resultRelationDesc, true);
	else
		resultRelInfo->ri_FdwRoutine = NULL;
	resultRelInfo->ri_FdwState = NULL;
	resultRelInfo->ri_ConstraintExprs = NULL;
	resultRelInfo->ri_junkFilter = NULL;
	resultRelInfo->ri_projectReturning = NULL;
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
 * be processed in the context of the outer query.  For efficiency's sake,
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
	 * event got queued, so we need take no new lock here.  Also, we need not
	 * recheck the relkind, so no need for CheckValidResultRel.
	 */
	rel = heap_open(relid, NoLock);

	/*
	 * Make the new entry in the right context.
	 */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);
	rInfo = makeNode(ResultRelInfo);
	InitResultRelInfo(rInfo,
					  rel,
					  0,		/* dummy rangetable index */
					  estate->es_instrument);
	estate->es_trig_target_relations =
		lappend(estate->es_trig_target_relations, rInfo);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Currently, we don't need any index information in ResultRelInfos used
	 * only for triggers, so no need to call ExecOpenIndices.
	 */

	return rInfo;
}

/*
 *		ExecContextForcesOids
 *
 * This is pretty grotty: when doing INSERT, UPDATE, or CREATE TABLE AS,
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
 * CREATE TABLE AS is even uglier, because we don't have the target relation's
 * descriptor available when this code runs; we have to look aside at the
 * flags passed to ExecutorStart().
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

	if (planstate->state->es_top_eflags & EXEC_FLAG_WITH_OIDS)
	{
		*hasoids = true;
		return true;
	}
	if (planstate->state->es_top_eflags & EXEC_FLAG_WITHOUT_OIDS)
	{
		*hasoids = false;
		return true;
	}

	return false;
}

/* ----------------------------------------------------------------
 *		ExecPostprocessPlan
 *
 *		Give plan nodes a final chance to execute before shutdown
 * ----------------------------------------------------------------
 */
static void
ExecPostprocessPlan(EState *estate)
{
	ListCell   *lc;

	/*
	 * Make sure nodes run forward.
	 */
	estate->es_direction = ForwardScanDirection;

	/*
	 * Run any secondary ModifyTable nodes to completion, in case the main
	 * query did not fetch all rows from them.  (We do this to ensure that
	 * such nodes have predictable results.)
	 */
	foreach(lc, estate->es_auxmodifytables)
	{
		PlanState  *ps = (PlanState *) lfirst(lc);

		for (;;)
		{
			TupleTableSlot *slot;

			/* Reset the per-output-tuple exprcontext each time */
			ResetPerTupleExprContext(estate);

			slot = ExecProcNode(ps);

			if (TupIsNull(slot))
				break;
		}
	}
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
	 * releasing buffer pins and tupdesc refcounts; there's no need to pfree
	 * the TupleTableSlots, since the containing memory context is about to go
	 * away anyway.
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
	 * close any relations selected FOR [KEY] UPDATE/SHARE, again keeping
	 * locks
	 */
	foreach(l, estate->es_rowMarks)
	{
		ExecRowMark *erm = (ExecRowMark *) lfirst(l);

		if (erm->relation)
			heap_close(erm->relation, NoLock);
	}
}

/* ----------------------------------------------------------------
 *		ExecutePlan
 *
 *		Processes the query plan until we have retrieved 'numberTuples' tuples,
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
			bool use_parallel_mode,
			CmdType operation,
			bool sendTuples,
			long numberTuples,
			ScanDirection direction,
			DestReceiver *dest)
{
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
	 * If a tuple count was supplied, we must force the plan to run without
	 * parallelism, because we might exit early.
	 */
	if (numberTuples != 0)
		use_parallel_mode = false;

	/*
	 * If a tuple count was supplied, we must force the plan to run without
	 * parallelism, because we might exit early.
	 */
	if (use_parallel_mode)
		EnterParallelMode();

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
		slot = ExecProcNode(planstate);

		/*
		 * if the tuple is null, then we assume there is nothing more to
		 * process so we just end the loop...
		 */
		if (TupIsNull(slot))
		{
			/* Allow nodes to release or shut down resources. */
			(void) ExecShutdownNode(planstate);
			break;
		}

		/*
		 * If we have a junk filter, then project a new tuple with the junk
		 * removed.
		 *
		 * Store this new "clean" tuple in the junkfilter's resultSlot.
		 * (Formerly, we stored it back over the "dirty" tuple, which is WRONG
		 * because that tuple slot has the wrong descriptor.)
		 */
		if (estate->es_junkFilter != NULL)
			slot = ExecFilterJunk(estate->es_junkFilter, slot);

		/*
		 * If we are supposed to send the tuple somewhere, do so. (In
		 * practice, this is probably always the case at this point.)
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

	if (use_parallel_mode)
		ExitParallelMode();
}


/*
 * ExecRelCheck --- check that tuple meets constraints for result relation
 *
 * Returns NULL if OK, else name of failed check constraint
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
		 * NOTE: SQL specifies that a NULL result from a constraint expression
		 * is not to be treated as a failure.  Therefore, tell ExecQual to
		 * return TRUE for NULL.
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
	TupleDesc	tupdesc = RelationGetDescr(rel);
	TupleConstr *constr = tupdesc->constr;
	Bitmapset  *modifiedCols;
	Bitmapset  *insertedCols;
	Bitmapset  *updatedCols;

	Assert(constr);

	if (constr->has_not_null)
	{
		int			natts = tupdesc->natts;
		int			attrChk;

		for (attrChk = 1; attrChk <= natts; attrChk++)
		{
			if (tupdesc->attrs[attrChk - 1]->attnotnull &&
				slot_attisnull(slot, attrChk))
			{
				char	   *val_desc;

				insertedCols = GetInsertedColumns(resultRelInfo, estate);
				updatedCols = GetUpdatedColumns(resultRelInfo, estate);
				modifiedCols = bms_union(insertedCols, updatedCols);
				val_desc = ExecBuildSlotValueDescription(RelationGetRelid(rel),
														 slot,
														 tupdesc,
														 modifiedCols,
														 64);

				ereport(ERROR,
						(errcode(ERRCODE_NOT_NULL_VIOLATION),
						 errmsg("null value in column \"%s\" violates not-null constraint",
							  NameStr(tupdesc->attrs[attrChk - 1]->attname)),
						 val_desc ? errdetail("Failing row contains %s.", val_desc) : 0,
						 errtablecol(rel, attrChk)));
			}
		}
	}

	if (constr->num_check > 0)
	{
		const char *failed;

		if ((failed = ExecRelCheck(resultRelInfo, slot, estate)) != NULL)
		{
			char	   *val_desc;

			insertedCols = GetInsertedColumns(resultRelInfo, estate);
			updatedCols = GetUpdatedColumns(resultRelInfo, estate);
			modifiedCols = bms_union(insertedCols, updatedCols);
			val_desc = ExecBuildSlotValueDescription(RelationGetRelid(rel),
													 slot,
													 tupdesc,
													 modifiedCols,
													 64);
			ereport(ERROR,
					(errcode(ERRCODE_CHECK_VIOLATION),
					 errmsg("new row for relation \"%s\" violates check constraint \"%s\"",
							RelationGetRelationName(rel), failed),
			  val_desc ? errdetail("Failing row contains %s.", val_desc) : 0,
					 errtableconstraint(rel, failed)));
		}
	}
}

/*
 * ExecWithCheckOptions -- check that tuple satisfies any WITH CHECK OPTIONs
 * of the specified kind.
 *
 * Note that this needs to be called multiple times to ensure that all kinds of
 * WITH CHECK OPTIONs are handled (both those from views which have the WITH
 * CHECK OPTION set and from row level security policies).  See ExecInsert()
 * and ExecUpdate().
 */
void
ExecWithCheckOptions(WCOKind kind, ResultRelInfo *resultRelInfo,
					 TupleTableSlot *slot, EState *estate)
{
	Relation	rel = resultRelInfo->ri_RelationDesc;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	ExprContext *econtext;
	ListCell   *l1,
			   *l2;

	/*
	 * We will use the EState's per-tuple context for evaluating constraint
	 * expressions (creating it if it's not already there).
	 */
	econtext = GetPerTupleExprContext(estate);

	/* Arrange for econtext's scan tuple to be the tuple under test */
	econtext->ecxt_scantuple = slot;

	/* Check each of the constraints */
	forboth(l1, resultRelInfo->ri_WithCheckOptions,
			l2, resultRelInfo->ri_WithCheckOptionExprs)
	{
		WithCheckOption *wco = (WithCheckOption *) lfirst(l1);
		ExprState  *wcoExpr = (ExprState *) lfirst(l2);

		/*
		 * Skip any WCOs which are not the kind we are looking for at this
		 * time.
		 */
		if (wco->kind != kind)
			continue;

		/*
		 * WITH CHECK OPTION checks are intended to ensure that the new tuple
		 * is visible (in the case of a view) or that it passes the
		 * 'with-check' policy (in the case of row security). If the qual
		 * evaluates to NULL or FALSE, then the new tuple won't be included in
		 * the view or doesn't pass the 'with-check' policy for the table.  We
		 * need ExecQual to return FALSE for NULL to handle the view case (the
		 * opposite of what we do above for CHECK constraints).
		 */
		if (!ExecQual((List *) wcoExpr, econtext, false))
		{
			char	   *val_desc;
			Bitmapset  *modifiedCols;
			Bitmapset  *insertedCols;
			Bitmapset  *updatedCols;

			switch (wco->kind)
			{
					/*
					 * For WITH CHECK OPTIONs coming from views, we might be
					 * able to provide the details on the row, depending on
					 * the permissions on the relation (that is, if the user
					 * could view it directly anyway).  For RLS violations, we
					 * don't include the data since we don't know if the user
					 * should be able to view the tuple as as that depends on
					 * the USING policy.
					 */
				case WCO_VIEW_CHECK:
					insertedCols = GetInsertedColumns(resultRelInfo, estate);
					updatedCols = GetUpdatedColumns(resultRelInfo, estate);
					modifiedCols = bms_union(insertedCols, updatedCols);
					val_desc = ExecBuildSlotValueDescription(RelationGetRelid(rel),
															 slot,
															 tupdesc,
															 modifiedCols,
															 64);

					ereport(ERROR,
							(errcode(ERRCODE_WITH_CHECK_OPTION_VIOLATION),
					  errmsg("new row violates check option for view \"%s\"",
							 wco->relname),
							 val_desc ? errdetail("Failing row contains %s.",
												  val_desc) : 0));
					break;
				case WCO_RLS_INSERT_CHECK:
				case WCO_RLS_UPDATE_CHECK:
					if (wco->polname != NULL)
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("new row violates row-level security policy \"%s\" for table \"%s\"",
									wco->polname, wco->relname)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("new row violates row-level security policy for table \"%s\"",
									wco->relname)));
					break;
				case WCO_RLS_CONFLICT_CHECK:
					if (wco->polname != NULL)
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("new row violates row-level security policy \"%s\" (USING expression) for table \"%s\"",
									wco->polname, wco->relname)));
					else
						ereport(ERROR,
								(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("new row violates row-level security policy (USING expression) for table \"%s\"",
									wco->relname)));
					break;
				default:
					elog(ERROR, "unrecognized WCO kind: %u", wco->kind);
					break;
			}
		}
	}
}

/*
 * ExecBuildSlotValueDescription -- construct a string representing a tuple
 *
 * This is intentionally very similar to BuildIndexValueDescription, but
 * unlike that function, we truncate long field values (to at most maxfieldlen
 * bytes).  That seems necessary here since heap field values could be very
 * long, whereas index entries typically aren't so wide.
 *
 * Also, unlike the case with index entries, we need to be prepared to ignore
 * dropped columns.  We used to use the slot's tuple descriptor to decode the
 * data, but the slot's descriptor doesn't identify dropped columns, so we
 * now need to be passed the relation's descriptor.
 *
 * Note that, like BuildIndexValueDescription, if the user does not have
 * permission to view any of the columns involved, a NULL is returned.  Unlike
 * BuildIndexValueDescription, if the user has access to view a subset of the
 * column involved, that subset will be returned with a key identifying which
 * columns they are.
 */
static char *
ExecBuildSlotValueDescription(Oid reloid,
							  TupleTableSlot *slot,
							  TupleDesc tupdesc,
							  Bitmapset *modifiedCols,
							  int maxfieldlen)
{
	StringInfoData buf;
	StringInfoData collist;
	bool		write_comma = false;
	bool		write_comma_collist = false;
	int			i;
	AclResult	aclresult;
	bool		table_perm = false;
	bool		any_perm = false;

	/*
	 * Check if RLS is enabled and should be active for the relation; if so,
	 * then don't return anything.  Otherwise, go through normal permission
	 * checks.
	 */
	if (check_enable_rls(reloid, InvalidOid, true) == RLS_ENABLED)
		return NULL;

	initStringInfo(&buf);

	appendStringInfoChar(&buf, '(');

	/*
	 * Check if the user has permissions to see the row.  Table-level SELECT
	 * allows access to all columns.  If the user does not have table-level
	 * SELECT then we check each column and include those the user has SELECT
	 * rights on.  Additionally, we always include columns the user provided
	 * data for.
	 */
	aclresult = pg_class_aclcheck(reloid, GetUserId(), ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
	{
		/* Set up the buffer for the column list */
		initStringInfo(&collist);
		appendStringInfoChar(&collist, '(');
	}
	else
		table_perm = any_perm = true;

	/* Make sure the tuple is fully deconstructed */
	slot_getallattrs(slot);

	for (i = 0; i < tupdesc->natts; i++)
	{
		bool		column_perm = false;
		char	   *val;
		int			vallen;

		/* ignore dropped columns */
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		if (!table_perm)
		{
			/*
			 * No table-level SELECT, so need to make sure they either have
			 * SELECT rights on the column or that they have provided the data
			 * for the column.  If not, omit this column from the error
			 * message.
			 */
			aclresult = pg_attribute_aclcheck(reloid, tupdesc->attrs[i]->attnum,
											  GetUserId(), ACL_SELECT);
			if (bms_is_member(tupdesc->attrs[i]->attnum - FirstLowInvalidHeapAttributeNumber,
							  modifiedCols) || aclresult == ACLCHECK_OK)
			{
				column_perm = any_perm = true;

				if (write_comma_collist)
					appendStringInfoString(&collist, ", ");
				else
					write_comma_collist = true;

				appendStringInfoString(&collist, NameStr(tupdesc->attrs[i]->attname));
			}
		}

		if (table_perm || column_perm)
		{
			if (slot->tts_isnull[i])
				val = "null";
			else
			{
				Oid			foutoid;
				bool		typisvarlena;

				getTypeOutputInfo(tupdesc->attrs[i]->atttypid,
								  &foutoid, &typisvarlena);
				val = OidOutputFunctionCall(foutoid, slot->tts_values[i]);
			}

			if (write_comma)
				appendStringInfoString(&buf, ", ");
			else
				write_comma = true;

			/* truncate if needed */
			vallen = strlen(val);
			if (vallen <= maxfieldlen)
				appendStringInfoString(&buf, val);
			else
			{
				vallen = pg_mbcliplen(val, vallen, maxfieldlen);
				appendBinaryStringInfo(&buf, val, vallen);
				appendStringInfoString(&buf, "...");
			}
		}
	}

	/* If we end up with zero columns being returned, then return NULL. */
	if (!any_perm)
		return NULL;

	appendStringInfoChar(&buf, ')');

	if (!table_perm)
	{
		appendStringInfoString(&collist, ") = ");
		appendStringInfoString(&collist, buf.data);

		return collist.data;
	}

	return buf.data;
}


/*
 * ExecUpdateLockMode -- find the appropriate UPDATE tuple lock mode for a
 * given ResultRelInfo
 */
LockTupleMode
ExecUpdateLockMode(EState *estate, ResultRelInfo *relinfo)
{
	Bitmapset  *keyCols;
	Bitmapset  *updatedCols;

	/*
	 * Compute lock mode to use.  If columns that are part of the key have not
	 * been modified, then we can use a weaker lock, allowing for better
	 * concurrency.
	 */
	updatedCols = GetUpdatedColumns(relinfo, estate);
	keyCols = RelationGetIndexAttrBitmap(relinfo->ri_RelationDesc,
										 INDEX_ATTR_BITMAP_KEY);

	if (bms_overlap(keyCols, updatedCols))
		return LockTupleExclusive;

	return LockTupleNoKeyExclusive;
}

/*
 * ExecFindRowMark -- find the ExecRowMark struct for given rangetable index
 *
 * If no such struct, either return NULL or throw error depending on missing_ok
 */
ExecRowMark *
ExecFindRowMark(EState *estate, Index rti, bool missing_ok)
{
	ListCell   *lc;

	foreach(lc, estate->es_rowMarks)
	{
		ExecRowMark *erm = (ExecRowMark *) lfirst(lc);

		if (erm->rti == rti)
			return erm;
	}
	if (!missing_ok)
		elog(ERROR, "failed to find ExecRowMark for rangetable index %u", rti);
	return NULL;
}

/*
 * ExecBuildAuxRowMark -- create an ExecAuxRowMark struct
 *
 * Inputs are the underlying ExecRowMark struct and the targetlist of the
 * input plan node (not planstate node!).  We need the latter to find out
 * the column numbers of the resjunk columns.
 */
ExecAuxRowMark *
ExecBuildAuxRowMark(ExecRowMark *erm, List *targetlist)
{
	ExecAuxRowMark *aerm = (ExecAuxRowMark *) palloc0(sizeof(ExecAuxRowMark));
	char		resname[32];

	aerm->rowmark = erm;

	/* Look up the resjunk columns associated with this rowmark */
	if (erm->markType != ROW_MARK_COPY)
	{
		/* need ctid for all methods other than COPY */
		snprintf(resname, sizeof(resname), "ctid%u", erm->rowmarkId);
		aerm->ctidAttNo = ExecFindJunkAttributeInTlist(targetlist,
													   resname);
		if (!AttributeNumberIsValid(aerm->ctidAttNo))
			elog(ERROR, "could not find junk %s column", resname);
	}
	else
	{
		/* need wholerow if COPY */
		snprintf(resname, sizeof(resname), "wholerow%u", erm->rowmarkId);
		aerm->wholeAttNo = ExecFindJunkAttributeInTlist(targetlist,
														resname);
		if (!AttributeNumberIsValid(aerm->wholeAttNo))
			elog(ERROR, "could not find junk %s column", resname);
	}

	/* if child rel, need tableoid */
	if (erm->rti != erm->prti)
	{
		snprintf(resname, sizeof(resname), "tableoid%u", erm->rowmarkId);
		aerm->toidAttNo = ExecFindJunkAttributeInTlist(targetlist,
													   resname);
		if (!AttributeNumberIsValid(aerm->toidAttNo))
			elog(ERROR, "could not find junk %s column", resname);
	}

	return aerm;
}


/*
 * EvalPlanQual logic --- recheck modified tuple(s) to see if we want to
 * process the updated version under READ COMMITTED rules.
 *
 * See backend/executor/README for some info about how this works.
 */


/*
 * Check a modified tuple to see if we want to process its updated version
 * under READ COMMITTED rules.
 *
 *	estate - outer executor state data
 *	epqstate - state for EvalPlanQual rechecking
 *	relation - table containing tuple
 *	rti - rangetable index of table containing tuple
 *	lockmode - requested tuple lock mode
 *	*tid - t_ctid from the outdated tuple (ie, next updated version)
 *	priorXmax - t_xmax from the outdated tuple
 *
 * *tid is also an output parameter: it's modified to hold the TID of the
 * latest version of the tuple (note this may be changed even on failure)
 *
 * Returns a slot containing the new candidate update/delete tuple, or
 * NULL if we determine we shouldn't process the row.
 *
 * Note: properly, lockmode should be declared as enum LockTupleMode,
 * but we use "int" to avoid having to include heapam.h in executor.h.
 */
TupleTableSlot *
EvalPlanQual(EState *estate, EPQState *epqstate,
			 Relation relation, Index rti, int lockmode,
			 ItemPointer tid, TransactionId priorXmax)
{
	TupleTableSlot *slot;
	HeapTuple	copyTuple;

	Assert(rti > 0);

	/*
	 * Get and lock the updated version of the row; if fail, return NULL.
	 */
	copyTuple = EvalPlanQualFetch(estate, relation, lockmode, LockWaitBlock,
								  tid, priorXmax);

	if (copyTuple == NULL)
		return NULL;

	/*
	 * For UPDATE/DELETE we have to return tid of actual row we're executing
	 * PQ for.
	 */
	*tid = copyTuple->t_self;

	/*
	 * Need to run a recheck subquery.  Initialize or reinitialize EPQ state.
	 */
	EvalPlanQualBegin(epqstate, estate);

	/*
	 * Free old test tuple, if any, and store new tuple where relation's scan
	 * node will see it
	 */
	EvalPlanQualSetTuple(epqstate, rti, copyTuple);

	/*
	 * Fetch any non-locked source rows
	 */
	EvalPlanQualFetchRowMarks(epqstate);

	/*
	 * Run the EPQ query.  We assume it will return at most one tuple.
	 */
	slot = EvalPlanQualNext(epqstate);

	/*
	 * If we got a tuple, force the slot to materialize the tuple so that it
	 * is not dependent on any local state in the EPQ query (in particular,
	 * it's highly likely that the slot contains references to any pass-by-ref
	 * datums that may be present in copyTuple).  As with the next step, this
	 * is to guard against early re-use of the EPQ query.
	 */
	if (!TupIsNull(slot))
		(void) ExecMaterializeSlot(slot);

	/*
	 * Clear out the test tuple.  This is needed in case the EPQ query is
	 * re-used to test a tuple for a different relation.  (Not clear that can
	 * really happen, but let's be safe.)
	 */
	EvalPlanQualSetTuple(epqstate, rti, NULL);

	return slot;
}

/*
 * Fetch a copy of the newest version of an outdated tuple
 *
 *	estate - executor state data
 *	relation - table containing tuple
 *	lockmode - requested tuple lock mode
 *	wait_policy - requested lock wait policy
 *	*tid - t_ctid from the outdated tuple (ie, next updated version)
 *	priorXmax - t_xmax from the outdated tuple
 *
 * Returns a palloc'd copy of the newest tuple version, or NULL if we find
 * that there is no newest version (ie, the row was deleted not updated).
 * We also return NULL if the tuple is locked and the wait policy is to skip
 * such tuples.
 *
 * If successful, we have locked the newest tuple version, so caller does not
 * need to worry about it changing anymore.
 *
 * Note: properly, lockmode should be declared as enum LockTupleMode,
 * but we use "int" to avoid having to include heapam.h in executor.h.
 */
HeapTuple
EvalPlanQualFetch(EState *estate, Relation relation, int lockmode,
				  LockWaitPolicy wait_policy,
				  ItemPointer tid, TransactionId priorXmax)
{
	HeapTuple	copyTuple = NULL;
	HeapTupleData tuple;
	SnapshotData SnapshotDirty;

	/*
	 * fetch target tuple
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
			HTSU_Result test;
			HeapUpdateFailureData hufd;

			/*
			 * If xmin isn't what we're expecting, the slot must have been
			 * recycled and reused for an unrelated tuple.  This implies that
			 * the latest version of the row was deleted, so we need do
			 * nothing.  (Should be safe to examine xmin without getting
			 * buffer's content lock.  We assume reading a TransactionId to be
			 * atomic, and Xmin never changes in an existing tuple, except to
			 * invalid or frozen, and neither of those can match priorXmax.)
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
			 * wait for its commit/abort, or die trying.
			 */
			if (TransactionIdIsValid(SnapshotDirty.xmax))
			{
				ReleaseBuffer(buffer);
				switch (wait_policy)
				{
					case LockWaitBlock:
						XactLockTableWait(SnapshotDirty.xmax,
										  relation, &tuple.t_self,
										  XLTW_FetchUpdated);
						break;
					case LockWaitSkip:
						if (!ConditionalXactLockTableWait(SnapshotDirty.xmax))
							return NULL;		/* skip instead of waiting */
						break;
					case LockWaitError:
						if (!ConditionalXactLockTableWait(SnapshotDirty.xmax))
							ereport(ERROR,
									(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
									 errmsg("could not obtain lock on row in relation \"%s\"",
										RelationGetRelationName(relation))));
						break;
				}
				continue;		/* loop back to repeat heap_fetch */
			}

			/*
			 * If tuple was inserted by our own transaction, we have to check
			 * cmin against es_output_cid: cmin >= current CID means our
			 * command cannot see the tuple, so we should ignore it. Otherwise
			 * heap_lock_tuple() will throw an error, and so would any later
			 * attempt to update or delete the tuple.  (We need not check cmax
			 * because HeapTupleSatisfiesDirty will consider a tuple deleted
			 * by our transaction dead, regardless of cmax.) We just checked
			 * that priorXmax == xmin, so we can test that variable instead of
			 * doing HeapTupleHeaderGetXmin again.
			 */
			if (TransactionIdIsCurrentTransactionId(priorXmax) &&
				HeapTupleHeaderGetCmin(tuple.t_data) >= estate->es_output_cid)
			{
				ReleaseBuffer(buffer);
				return NULL;
			}

			/*
			 * This is a live tuple, so now try to lock it.
			 */
			test = heap_lock_tuple(relation, &tuple,
								   estate->es_output_cid,
								   lockmode, wait_policy,
								   false, &buffer, &hufd);
			/* We now have two pins on the buffer, get rid of one */
			ReleaseBuffer(buffer);

			switch (test)
			{
				case HeapTupleSelfUpdated:

					/*
					 * The target tuple was already updated or deleted by the
					 * current command, or by a later command in the current
					 * transaction.  We *must* ignore the tuple in the former
					 * case, so as to avoid the "Halloween problem" of
					 * repeated update attempts.  In the latter case it might
					 * be sensible to fetch the updated tuple instead, but
					 * doing so would require changing heap_update and
					 * heap_delete to not complain about updating "invisible"
					 * tuples, which seems pretty scary (heap_lock_tuple will
					 * not complain, but few callers expect
					 * HeapTupleInvisible, and we're not one of them).  So for
					 * now, treat the tuple as deleted and do not process.
					 */
					ReleaseBuffer(buffer);
					return NULL;

				case HeapTupleMayBeUpdated:
					/* successfully locked */
					break;

				case HeapTupleUpdated:
					ReleaseBuffer(buffer);
					if (IsolationUsesXactSnapshot())
						ereport(ERROR,
								(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
								 errmsg("could not serialize access due to concurrent update")));

					/* Should not encounter speculative tuple on recheck */
					Assert(!HeapTupleHeaderIsSpeculative(tuple.t_data));
					if (!ItemPointerEquals(&hufd.ctid, &tuple.t_self))
					{
						/* it was updated, so look at the updated version */
						tuple.t_self = hufd.ctid;
						/* updated row should have xmin matching this xmax */
						priorXmax = hufd.xmax;
						continue;
					}
					/* tuple was deleted, so give up */
					return NULL;

				case HeapTupleWouldBlock:
					ReleaseBuffer(buffer);
					return NULL;

				case HeapTupleInvisible:
					elog(ERROR, "attempted to lock invisible tuple");

				default:
					ReleaseBuffer(buffer);
					elog(ERROR, "unrecognized heap_lock_tuple status: %u",
						 test);
					return NULL;	/* keep compiler quiet */
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
		 * process.
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
		priorXmax = HeapTupleHeaderGetUpdateXid(tuple.t_data);
		ReleaseBuffer(buffer);
		/* loop back to fetch next in chain */
	}

	/*
	 * Return the copied tuple
	 */
	return copyTuple;
}

/*
 * EvalPlanQualInit -- initialize during creation of a plan state node
 * that might need to invoke EPQ processing.
 *
 * Note: subplan/auxrowmarks can be NULL/NIL if they will be set later
 * with EvalPlanQualSetPlan.
 */
void
EvalPlanQualInit(EPQState *epqstate, EState *estate,
				 Plan *subplan, List *auxrowmarks, int epqParam)
{
	/* Mark the EPQ state inactive */
	epqstate->estate = NULL;
	epqstate->planstate = NULL;
	epqstate->origslot = NULL;
	/* ... and remember data that EvalPlanQualBegin will need */
	epqstate->plan = subplan;
	epqstate->arowMarks = auxrowmarks;
	epqstate->epqParam = epqParam;
}

/*
 * EvalPlanQualSetPlan -- set or change subplan of an EPQState.
 *
 * We need this so that ModifyTable can deal with multiple subplans.
 */
void
EvalPlanQualSetPlan(EPQState *epqstate, Plan *subplan, List *auxrowmarks)
{
	/* If we have a live EPQ query, shut it down */
	EvalPlanQualEnd(epqstate);
	/* And set/change the plan pointer */
	epqstate->plan = subplan;
	/* The rowmarks depend on the plan, too */
	epqstate->arowMarks = auxrowmarks;
}

/*
 * Install one test tuple into EPQ state, or clear test tuple if tuple == NULL
 *
 * NB: passed tuple must be palloc'd; it may get freed later
 */
void
EvalPlanQualSetTuple(EPQState *epqstate, Index rti, HeapTuple tuple)
{
	EState	   *estate = epqstate->estate;

	Assert(rti > 0);

	/*
	 * free old test tuple, if any, and store new tuple where relation's scan
	 * node will see it
	 */
	if (estate->es_epqTuple[rti - 1] != NULL)
		heap_freetuple(estate->es_epqTuple[rti - 1]);
	estate->es_epqTuple[rti - 1] = tuple;
	estate->es_epqTupleSet[rti - 1] = true;
}

/*
 * Fetch back the current test tuple (if any) for the specified RTI
 */
HeapTuple
EvalPlanQualGetTuple(EPQState *epqstate, Index rti)
{
	EState	   *estate = epqstate->estate;

	Assert(rti > 0);

	return estate->es_epqTuple[rti - 1];
}

/*
 * Fetch the current row values for any non-locked relations that need
 * to be scanned by an EvalPlanQual operation.  origslot must have been set
 * to contain the current result row (top-level row) that we need to recheck.
 */
void
EvalPlanQualFetchRowMarks(EPQState *epqstate)
{
	ListCell   *l;

	Assert(epqstate->origslot != NULL);

	foreach(l, epqstate->arowMarks)
	{
		ExecAuxRowMark *aerm = (ExecAuxRowMark *) lfirst(l);
		ExecRowMark *erm = aerm->rowmark;
		Datum		datum;
		bool		isNull;
		HeapTupleData tuple;

		if (RowMarkRequiresRowShareLock(erm->markType))
			elog(ERROR, "EvalPlanQual doesn't support locking rowmarks");

		/* clear any leftover test tuple for this rel */
		EvalPlanQualSetTuple(epqstate, erm->rti, NULL);

		/* if child rel, must check whether it produced this row */
		if (erm->rti != erm->prti)
		{
			Oid			tableoid;

			datum = ExecGetJunkAttribute(epqstate->origslot,
										 aerm->toidAttNo,
										 &isNull);
			/* non-locked rels could be on the inside of outer joins */
			if (isNull)
				continue;
			tableoid = DatumGetObjectId(datum);

			Assert(OidIsValid(erm->relid));
			if (tableoid != erm->relid)
			{
				/* this child is inactive right now */
				continue;
			}
		}

		if (erm->markType == ROW_MARK_REFERENCE)
		{
			HeapTuple	copyTuple;

			Assert(erm->relation != NULL);

			/* fetch the tuple's ctid */
			datum = ExecGetJunkAttribute(epqstate->origslot,
										 aerm->ctidAttNo,
										 &isNull);
			/* non-locked rels could be on the inside of outer joins */
			if (isNull)
				continue;

			/* fetch requests on foreign tables must be passed to their FDW */
			if (erm->relation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
			{
				FdwRoutine *fdwroutine;
				bool		updated = false;

				fdwroutine = GetFdwRoutineForRelation(erm->relation, false);
				/* this should have been checked already, but let's be safe */
				if (fdwroutine->RefetchForeignRow == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						   errmsg("cannot lock rows in foreign table \"%s\"",
								  RelationGetRelationName(erm->relation))));
				copyTuple = fdwroutine->RefetchForeignRow(epqstate->estate,
														  erm,
														  datum,
														  &updated);
				if (copyTuple == NULL)
					elog(ERROR, "failed to fetch tuple for EvalPlanQual recheck");

				/*
				 * Ideally we'd insist on updated == false here, but that
				 * assumes that FDWs can track that exactly, which they might
				 * not be able to.  So just ignore the flag.
				 */
			}
			else
			{
				/* ordinary table, fetch the tuple */
				Buffer		buffer;

				tuple.t_self = *((ItemPointer) DatumGetPointer(datum));
				if (!heap_fetch(erm->relation, SnapshotAny, &tuple, &buffer,
								false, NULL))
					elog(ERROR, "failed to fetch tuple for EvalPlanQual recheck");

				/* successful, copy tuple */
				copyTuple = heap_copytuple(&tuple);
				ReleaseBuffer(buffer);
			}

			/* store tuple */
			EvalPlanQualSetTuple(epqstate, erm->rti, copyTuple);
		}
		else
		{
			HeapTupleHeader td;

			Assert(erm->markType == ROW_MARK_COPY);

			/* fetch the whole-row Var for the relation */
			datum = ExecGetJunkAttribute(epqstate->origslot,
										 aerm->wholeAttNo,
										 &isNull);
			/* non-locked rels could be on the inside of outer joins */
			if (isNull)
				continue;
			td = DatumGetHeapTupleHeader(datum);

			/* build a temporary HeapTuple control structure */
			tuple.t_len = HeapTupleHeaderGetDatumLength(td);
			tuple.t_data = td;
			/* relation might be a foreign table, if so provide tableoid */
			tuple.t_tableOid = erm->relid;
			/* also copy t_ctid in case there's valid data there */
			tuple.t_self = td->t_ctid;

			/* copy and store tuple */
			EvalPlanQualSetTuple(epqstate, erm->rti,
								 heap_copytuple(&tuple));
		}
	}
}

/*
 * Fetch the next row (if any) from EvalPlanQual testing
 *
 * (In practice, there should never be more than one row...)
 */
TupleTableSlot *
EvalPlanQualNext(EPQState *epqstate)
{
	MemoryContext oldcontext;
	TupleTableSlot *slot;

	oldcontext = MemoryContextSwitchTo(epqstate->estate->es_query_cxt);
	slot = ExecProcNode(epqstate->planstate);
	MemoryContextSwitchTo(oldcontext);

	return slot;
}

/*
 * Initialize or reset an EvalPlanQual state tree
 */
void
EvalPlanQualBegin(EPQState *epqstate, EState *parentestate)
{
	EState	   *estate = epqstate->estate;

	if (estate == NULL)
	{
		/* First time through, so create a child EState */
		EvalPlanQualStart(epqstate, parentestate, epqstate->plan);
	}
	else
	{
		/*
		 * We already have a suitable child EPQ tree, so just reset it.
		 */
		int			rtsize = list_length(parentestate->es_range_table);
		PlanState  *planstate = epqstate->planstate;

		MemSet(estate->es_epqScanDone, 0, rtsize * sizeof(bool));

		/* Recopy current values of parent parameters */
		if (parentestate->es_plannedstmt->nParamExec > 0)
		{
			int			i = parentestate->es_plannedstmt->nParamExec;

			while (--i >= 0)
			{
				/* copy value if any, but not execPlan link */
				estate->es_param_exec_vals[i].value =
					parentestate->es_param_exec_vals[i].value;
				estate->es_param_exec_vals[i].isnull =
					parentestate->es_param_exec_vals[i].isnull;
			}
		}

		/*
		 * Mark child plan tree as needing rescan at all scan nodes.  The
		 * first ExecProcNode will take care of actually doing the rescan.
		 */
		planstate->chgParam = bms_add_member(planstate->chgParam,
											 epqstate->epqParam);
	}
}

/*
 * Start execution of an EvalPlanQual plan tree.
 *
 * This is a cut-down version of ExecutorStart(): we copy some state from
 * the top-level estate rather than initializing it fresh.
 */
static void
EvalPlanQualStart(EPQState *epqstate, EState *parentestate, Plan *planTree)
{
	EState	   *estate;
	int			rtsize;
	MemoryContext oldcontext;
	ListCell   *l;

	rtsize = list_length(parentestate->es_range_table);

	epqstate->estate = estate = CreateExecutorState();

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/*
	 * Child EPQ EStates share the parent's copy of unchanging state such as
	 * the snapshot, rangetable, result-rel info, and external Param info.
	 * They need their own copies of local state, including a tuple table,
	 * es_param_exec_vals, etc.
	 *
	 * The ResultRelInfo array management is trickier than it looks.  We
	 * create a fresh array for the child but copy all the content from the
	 * parent.  This is because it's okay for the child to share any
	 * per-relation state the parent has already created --- but if the child
	 * sets up any ResultRelInfo fields, such as its own junkfilter, that
	 * state must *not* propagate back to the parent.  (For one thing, the
	 * pointed-to data is in a memory context that won't last long enough.)
	 */
	estate->es_direction = ForwardScanDirection;
	estate->es_snapshot = parentestate->es_snapshot;
	estate->es_crosscheck_snapshot = parentestate->es_crosscheck_snapshot;
	estate->es_range_table = parentestate->es_range_table;
	estate->es_plannedstmt = parentestate->es_plannedstmt;
	estate->es_junkFilter = parentestate->es_junkFilter;
	estate->es_output_cid = parentestate->es_output_cid;
	if (parentestate->es_num_result_relations > 0)
	{
		int			numResultRelations = parentestate->es_num_result_relations;
		ResultRelInfo *resultRelInfos;

		resultRelInfos = (ResultRelInfo *)
			palloc(numResultRelations * sizeof(ResultRelInfo));
		memcpy(resultRelInfos, parentestate->es_result_relations,
			   numResultRelations * sizeof(ResultRelInfo));
		estate->es_result_relations = resultRelInfos;
		estate->es_num_result_relations = numResultRelations;
	}
	/* es_result_relation_info must NOT be copied */
	/* es_trig_target_relations must NOT be copied */
	estate->es_rowMarks = parentestate->es_rowMarks;
	estate->es_top_eflags = parentestate->es_top_eflags;
	estate->es_instrument = parentestate->es_instrument;
	/* es_auxmodifytables must NOT be copied */

	/*
	 * The external param list is simply shared from parent.  The internal
	 * param workspace has to be local state, but we copy the initial values
	 * from the parent, so as to have access to any param values that were
	 * already set from other parts of the parent's plan tree.
	 */
	estate->es_param_list_info = parentestate->es_param_list_info;
	if (parentestate->es_plannedstmt->nParamExec > 0)
	{
		int			i = parentestate->es_plannedstmt->nParamExec;

		estate->es_param_exec_vals = (ParamExecData *)
			palloc0(i * sizeof(ParamExecData));
		while (--i >= 0)
		{
			/* copy value if any, but not execPlan link */
			estate->es_param_exec_vals[i].value =
				parentestate->es_param_exec_vals[i].value;
			estate->es_param_exec_vals[i].isnull =
				parentestate->es_param_exec_vals[i].isnull;
		}
	}

	/*
	 * Each EState must have its own es_epqScanDone state, but if we have
	 * nested EPQ checks they should share es_epqTuple arrays.  This allows
	 * sub-rechecks to inherit the values being examined by an outer recheck.
	 */
	estate->es_epqScanDone = (bool *) palloc0(rtsize * sizeof(bool));
	if (parentestate->es_epqTuple != NULL)
	{
		estate->es_epqTuple = parentestate->es_epqTuple;
		estate->es_epqTupleSet = parentestate->es_epqTupleSet;
	}
	else
	{
		estate->es_epqTuple = (HeapTuple *)
			palloc0(rtsize * sizeof(HeapTuple));
		estate->es_epqTupleSet = (bool *)
			palloc0(rtsize * sizeof(bool));
	}

	/*
	 * Each estate also has its own tuple table.
	 */
	estate->es_tupleTable = NIL;

	/*
	 * Initialize private state information for each SubPlan.  We must do this
	 * before running ExecInitNode on the main query tree, since
	 * ExecInitSubPlan expects to be able to find these entries. Some of the
	 * SubPlans might not be used in the part of the plan tree we intend to
	 * run, but since it's not easy to tell which, we just initialize them
	 * all.
	 */
	Assert(estate->es_subplanstates == NIL);
	foreach(l, parentestate->es_plannedstmt->subplans)
	{
		Plan	   *subplan = (Plan *) lfirst(l);
		PlanState  *subplanstate;

		subplanstate = ExecInitNode(subplan, estate, 0);
		estate->es_subplanstates = lappend(estate->es_subplanstates,
										   subplanstate);
	}

	/*
	 * Initialize the private state information for all the nodes in the part
	 * of the plan tree we need to run.  This opens files, allocates storage
	 * and leaves us ready to start processing tuples.
	 */
	epqstate->planstate = ExecInitNode(planTree, estate, 0);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * EvalPlanQualEnd -- shut down at termination of parent plan state node,
 * or if we are done with the current EPQ child.
 *
 * This is a cut-down version of ExecutorEnd(); basically we want to do most
 * of the normal cleanup, but *not* close result relations (which we are
 * just sharing from the outer query).  We do, however, have to close any
 * trigger target relations that got opened, since those are not shared.
 * (There probably shouldn't be any of the latter, but just in case...)
 */
void
EvalPlanQualEnd(EPQState *epqstate)
{
	EState	   *estate = epqstate->estate;
	MemoryContext oldcontext;
	ListCell   *l;

	if (estate == NULL)
		return;					/* idle, so nothing to do */

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	ExecEndNode(epqstate->planstate);

	foreach(l, estate->es_subplanstates)
	{
		PlanState  *subplanstate = (PlanState *) lfirst(l);

		ExecEndNode(subplanstate);
	}

	/* throw away the per-estate tuple table */
	ExecResetTupleTable(estate->es_tupleTable, false);

	/* close any trigger target relations attached to this EState */
	foreach(l, estate->es_trig_target_relations)
	{
		ResultRelInfo *resultRelInfo = (ResultRelInfo *) lfirst(l);

		/* Close indices and then the relation itself */
		ExecCloseIndices(resultRelInfo);
		heap_close(resultRelInfo->ri_RelationDesc, NoLock);
	}

	MemoryContextSwitchTo(oldcontext);

	FreeExecutorState(estate);

	/* Mark EPQState idle */
	epqstate->estate = NULL;
	epqstate->planstate = NULL;
	epqstate->origslot = NULL;
}
