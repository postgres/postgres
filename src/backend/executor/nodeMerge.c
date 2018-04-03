/*-------------------------------------------------------------------------
 *
 * nodeMerge.c
 *	  routines to handle Merge nodes relating to the MERGE command
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeMerge.c
 *
 *-------------------------------------------------------------------------
 */


#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "commands/trigger.h"
#include "executor/execPartition.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "executor/nodeMerge.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tqual.h"


/*
 * Check and execute the first qualifying MATCHED action. The current target
 * tuple is identified by tupleid.
 *
 * We start from the first WHEN MATCHED action and check if the WHEN AND quals
 * pass, if any. If the WHEN AND quals for the first action do not pass, we
 * check the second, then the third and so on. If we reach to the end, no
 * action is taken and we return true, indicating that no further action is
 * required for this tuple.
 *
 * If we do find a qualifying action, then we attempt to execute the action.
 *
 * If the tuple is concurrently updated, EvalPlanQual is run with the updated
 * tuple to recheck the join quals. Note that the additional quals associated
 * with individual actions are evaluated separately by the MERGE code, while
 * EvalPlanQual checks for the join quals. If EvalPlanQual tells us that the
 * updated tuple still passes the join quals, then we restart from the first
 * action to look for a qualifying action. Otherwise, we return false meaning
 * that a NOT MATCHED action must now be executed for the current source tuple.
 */
static bool
ExecMergeMatched(ModifyTableState *mtstate, EState *estate,
				 TupleTableSlot *slot, JunkFilter *junkfilter,
				 ItemPointer tupleid)
{
	ExprContext *econtext = mtstate->ps.ps_ExprContext;
	bool		isNull;
	List	   *mergeMatchedActionStates = NIL;
	HeapUpdateFailureData hufd;
	bool		tuple_updated,
				tuple_deleted;
	Buffer		buffer;
	HeapTupleData tuple;
	EPQState   *epqstate = &mtstate->mt_epqstate;
	ResultRelInfo *saved_resultRelInfo;
	ResultRelInfo *resultRelInfo = estate->es_result_relation_info;
	ListCell   *l;
	TupleTableSlot *saved_slot = slot;

	if (mtstate->mt_partition_tuple_routing)
	{
		Datum		datum;
		Oid			tableoid = InvalidOid;
		int         leaf_part_index;
		PartitionTupleRouting *proute = mtstate->mt_partition_tuple_routing;

		/*
		 * In case of partitioned table, we fetch the tableoid while performing
		 * MATCHED MERGE action.
		 */
		datum = ExecGetJunkAttribute(slot, junkfilter->jf_otherJunkAttNo,
				&isNull);
		Assert(!isNull);
		tableoid = DatumGetObjectId(datum);

		/*
		 * If we're dealing with a MATCHED tuple, then tableoid must have been
		 * set correctly. In case of partitioned table, we must now fetch the
		 * correct result relation corresponding to the child table emitting
		 * the matching target row. For normal table, there is just one result
		 * relation and it must be the one emitting the matching row.
		 */
		leaf_part_index = ExecFindPartitionByOid(proute, tableoid);

		resultRelInfo = proute->partitions[leaf_part_index];
		if (resultRelInfo == NULL)
		{
			resultRelInfo = ExecInitPartitionInfo(mtstate,
					mtstate->resultRelInfo,
					proute, estate, leaf_part_index);
			Assert(resultRelInfo != NULL);
		}
	}

	/*
	 * Save the current information and work with the correct result relation.
	 */
	saved_resultRelInfo = resultRelInfo;
	estate->es_result_relation_info = resultRelInfo;

	/*
	 * And get the correct action lists.
	 */
	mergeMatchedActionStates =
		resultRelInfo->ri_mergeState->matchedActionStates;

	/*
	 * If there are not WHEN MATCHED actions, we are done.
	 */
	if (mergeMatchedActionStates == NIL)
		return true;

	/*
	 * Make tuple and any needed join variables available to ExecQual and
	 * ExecProject. The target's existing tuple is installed in the scantuple.
	 * Again, this target relation's slot is required only in the case of a
	 * MATCHED tuple and UPDATE/DELETE actions.
	 */
	if (mtstate->mt_partition_tuple_routing)
		ExecSetSlotDescriptor(mtstate->mt_existing,
				resultRelInfo->ri_RelationDesc->rd_att);
	econtext->ecxt_scantuple = mtstate->mt_existing;
	econtext->ecxt_innertuple = slot;
	econtext->ecxt_outertuple = NULL;

lmerge_matched:;
	slot = saved_slot;

	/*
	 * UPDATE/DELETE is only invoked for matched rows. And we must have found
	 * the tupleid of the target row in that case. We fetch using SnapshotAny
	 * because we might get called again after EvalPlanQual returns us a new
	 * tuple. This tuple may not be visible to our MVCC snapshot.
	 */
	Assert(tupleid != NULL);

	tuple.t_self = *tupleid;
	if (!heap_fetch(resultRelInfo->ri_RelationDesc, SnapshotAny, &tuple,
					&buffer, true, NULL))
		elog(ERROR, "Failed to fetch the target tuple");

	/* Store target's existing tuple in the state's dedicated slot */
	ExecStoreTuple(&tuple, mtstate->mt_existing, buffer, false);

	foreach(l, mergeMatchedActionStates)
	{
		MergeActionState *action = (MergeActionState *) lfirst(l);

		/*
		 * Test condition, if any
		 *
		 * In the absence of a condition we perform the action unconditionally
		 * (no need to check separately since ExecQual() will return true if
		 * there are no conditions to evaluate).
		 */
		if (!ExecQual(action->whenqual, econtext))
			continue;

		/*
		 * Check if the existing target tuple meet the USING checks of
		 * UPDATE/DELETE RLS policies. If those checks fail, we throw an
		 * error.
		 *
		 * The WITH CHECK quals are applied in ExecUpdate() and hence we need
		 * not do anything special to handle them.
		 *
		 * NOTE: We must do this after WHEN quals are evaluated so that we
		 * check policies only when they matter.
		 */
		if (resultRelInfo->ri_WithCheckOptions)
		{
			ExecWithCheckOptions(action->commandType == CMD_UPDATE ?
								 WCO_RLS_MERGE_UPDATE_CHECK : WCO_RLS_MERGE_DELETE_CHECK,
								 resultRelInfo,
								 mtstate->mt_existing,
								 mtstate->ps.state);
		}

		/* Perform stated action */
		switch (action->commandType)
		{
			case CMD_UPDATE:

				/*
				 * We set up the projection earlier, so all we do here is
				 * Project, no need for any other tasks prior to the
				 * ExecUpdate.
				 */
				if (mtstate->mt_partition_tuple_routing)
					ExecSetSlotDescriptor(mtstate->mt_mergeproj, action->tupDesc);
				ExecProject(action->proj);

				/*
				 * We don't call ExecFilterJunk() because the projected tuple
				 * using the UPDATE action's targetlist doesn't have a junk
				 * attribute.
				 */
				slot = ExecUpdate(mtstate, tupleid, NULL,
								  mtstate->mt_mergeproj,
								  slot, epqstate, estate,
								  &tuple_updated, &hufd,
								  action, mtstate->canSetTag);
				break;

			case CMD_DELETE:
				/* Nothing to Project for a DELETE action */
				slot = ExecDelete(mtstate, tupleid, NULL,
								  slot, epqstate, estate,
								  &tuple_deleted, false, &hufd, action,
								  mtstate->canSetTag);

				break;

			default:
				elog(ERROR, "unknown action in MERGE WHEN MATCHED clause");

		}

		/*
		 * Check for any concurrent update/delete operation which may have
		 * prevented our update/delete. We also check for situations where we
		 * might be trying to update/delete the same tuple twice.
		 */
		if ((action->commandType == CMD_UPDATE && !tuple_updated) ||
			(action->commandType == CMD_DELETE && !tuple_deleted))

		{
			switch (hufd.result)
			{
				case HeapTupleMayBeUpdated:
					break;
				case HeapTupleInvisible:

					/*
					 * This state should never be reached since the underlying
					 * JOIN runs with a MVCC snapshot and should only return
					 * rows visible to us.
					 */
					elog(ERROR, "unexpected invisible tuple");
					break;

				case HeapTupleSelfUpdated:

					/*
					 * SQLStandard disallows this for MERGE.
					 */
					if (TransactionIdIsCurrentTransactionId(hufd.xmax))
						ereport(ERROR,
								(errcode(ERRCODE_CARDINALITY_VIOLATION),
								 errmsg("MERGE command cannot affect row a second time"),
								 errhint("Ensure that not more than one source row matches any one target row")));
					/* This shouldn't happen */
					elog(ERROR, "attempted to update or delete invisible tuple");
					break;

				case HeapTupleUpdated:

					/*
					 * The target tuple was concurrently updated/deleted by
					 * some other transaction.
					 *
					 * If the current tuple is that last tuple in the update
					 * chain, then we know that the tuple was concurrently
					 * deleted. Just return and let the caller try NOT MATCHED
					 * actions.
					 *
					 * If the current tuple was concurrently updated, then we
					 * must run the EvalPlanQual() with the new version of the
					 * tuple. If EvalPlanQual() does not return a tuple then
					 * we switch to the NOT MATCHED list of actions.
					 * If it does return a tuple and the join qual is
					 * still satisfied, then we just need to recheck the
					 * MATCHED actions, starting from the top, and execute the
					 * first qualifying action.
					 */
					if (!ItemPointerEquals(tupleid, &hufd.ctid))
					{
						TupleTableSlot *epqslot;

						/*
						 * Since we generate a JOIN query with a target table
						 * RTE different than the result relation RTE, we must
						 * pass in the RTI of the relation used in the join
						 * query and not the one from result relation.
						 */
						Assert(resultRelInfo->ri_mergeTargetRTI > 0);
						epqslot = EvalPlanQual(estate,
											   epqstate,
											   resultRelInfo->ri_RelationDesc,
											   GetEPQRangeTableIndex(resultRelInfo),
											   LockTupleExclusive,
											   &hufd.ctid,
											   hufd.xmax);

						if (!TupIsNull(epqslot))
						{
							(void) ExecGetJunkAttribute(epqslot,
														resultRelInfo->ri_junkFilter->jf_junkAttNo,
														&isNull);

							/*
							 * A non-NULL ctid means that we are still dealing
							 * with MATCHED case. But we must retry from the
							 * start with the updated tuple to ensure that the
							 * first qualifying WHEN MATCHED action is
							 * executed.
							 *
							 * We don't use the new slot returned by
							 * EvalPlanQual because we anyways re-install the
							 * new target tuple in econtext->ecxt_scantuple
							 * before re-evaluating WHEN AND conditions and
							 * re-projecting the update targetlists. The
							 * source side tuple does not change and hence we
							 * can safely continue to use the old slot.
							 */
							if (!isNull)
							{
								/*
								 * Must update *tupleid to the TID of the
								 * newer tuple found in the update chain.
								 */
								*tupleid = hufd.ctid;
								ReleaseBuffer(buffer);
								goto lmerge_matched;
							}
						}
					}

					/*
					 * Tell the caller about the updated TID, restore the
					 * state back and return.
					 */
					*tupleid = hufd.ctid;
					estate->es_result_relation_info = saved_resultRelInfo;
					ReleaseBuffer(buffer);
					return false;

				default:
					break;

			}
		}

		if (action->commandType == CMD_UPDATE && tuple_updated)
			InstrCountFiltered2(&mtstate->ps, 1);
		if (action->commandType == CMD_DELETE && tuple_deleted)
			InstrCountFiltered3(&mtstate->ps, 1);

		/*
		 * We've activated one of the WHEN clauses, so we don't search
		 * further. This is required behaviour, not an optimization.
		 */
		estate->es_result_relation_info = saved_resultRelInfo;
		break;
	}

	ReleaseBuffer(buffer);

	/*
	 * Successfully executed an action or no qualifying action was found.
	 */
	return true;
}

/*
 * Execute the first qualifying NOT MATCHED action.
 */
static void
ExecMergeNotMatched(ModifyTableState *mtstate, EState *estate,
					TupleTableSlot *slot)
{
	PartitionTupleRouting *proute = mtstate->mt_partition_tuple_routing;
	ExprContext *econtext = mtstate->ps.ps_ExprContext;
	List	   *mergeNotMatchedActionStates = NIL;
	ResultRelInfo *resultRelInfo;
	ListCell   *l;
	TupleTableSlot	*myslot;

	/*
	 * We are dealing with NOT MATCHED tuple. Since for MERGE, partition tree
	 * is not expanded for the result relation, we continue to work with the
	 * currently active result relation, which should be of the root of the
	 * partition tree.
	 */
	resultRelInfo = mtstate->resultRelInfo;

	/*
	 * For INSERT actions, root relation's merge action is OK since the
	 * INSERT's targetlist and the WHEN conditions can only refer to the
	 * source relation and hence it does not matter which result relation we
	 * work with.
	 */
	mergeNotMatchedActionStates =
		resultRelInfo->ri_mergeState->notMatchedActionStates;

	/*
	 * Make source tuple available to ExecQual and ExecProject. We don't need
	 * the target tuple since the WHEN quals and the targetlist can't refer to
	 * the target columns.
	 */
	econtext->ecxt_scantuple = NULL;
	econtext->ecxt_innertuple = slot;
	econtext->ecxt_outertuple = NULL;

	foreach(l, mergeNotMatchedActionStates)
	{
		MergeActionState *action = (MergeActionState *) lfirst(l);

		/*
		 * Test condition, if any
		 *
		 * In the absence of a condition we perform the action unconditionally
		 * (no need to check separately since ExecQual() will return true if
		 * there are no conditions to evaluate).
		 */
		if (!ExecQual(action->whenqual, econtext))
			continue;

		/* Perform stated action */
		switch (action->commandType)
		{
			case CMD_INSERT:

				/*
				 * We set up the projection earlier, so all we do here is
				 * Project, no need for any other tasks prior to the
				 * ExecInsert.
				 */
				if (mtstate->mt_partition_tuple_routing)
					ExecSetSlotDescriptor(mtstate->mt_mergeproj, action->tupDesc);
				ExecProject(action->proj);

				/*
				 * ExecPrepareTupleRouting may modify the passed-in slot. Hence
				 * pass a local reference so that action->slot is not modified.
				 */
				myslot = mtstate->mt_mergeproj;

				/* Prepare for tuple routing if needed. */
				if (proute)
					myslot = ExecPrepareTupleRouting(mtstate, estate, proute,
												   resultRelInfo, myslot);
				slot = ExecInsert(mtstate, myslot, slot,
								  estate, action,
								  mtstate->canSetTag);
				/* Revert ExecPrepareTupleRouting's state change. */
				if (proute)
					estate->es_result_relation_info = resultRelInfo;
				InstrCountFiltered1(&mtstate->ps, 1);
				break;
			case CMD_NOTHING:
				/* Do Nothing */
				break;
			default:
				elog(ERROR, "unknown action in MERGE WHEN NOT MATCHED clause");
		}

		break;
	}
}

/*
 * Perform MERGE.
 */
void
ExecMerge(ModifyTableState *mtstate, EState *estate, TupleTableSlot *slot,
		  JunkFilter *junkfilter, ResultRelInfo *resultRelInfo)
{
	ExprContext *econtext = mtstate->ps.ps_ExprContext;
	ItemPointer tupleid;
	ItemPointerData tuple_ctid;
	bool		matched = false;
	char		relkind;
	Datum		datum;
	bool		isNull;

	relkind = resultRelInfo->ri_RelationDesc->rd_rel->relkind;
	Assert(relkind == RELKIND_RELATION ||
		   relkind == RELKIND_PARTITIONED_TABLE);

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous cycle.
	 */
	ResetExprContext(econtext);

	/*
	 * We run a JOIN between the target relation and the source relation to
	 * find a set of candidate source rows that has matching row in the target
	 * table and a set of candidate source rows that does not have matching
	 * row in the target table. If the join returns us a tuple with target
	 * relation's tid set, that implies that the join found a matching row for
	 * the given source tuple. This case triggers the WHEN MATCHED clause of
	 * the MERGE. Whereas a NULL in the target relation's ctid column
	 * indicates a NOT MATCHED case.
	 */
	datum = ExecGetJunkAttribute(slot, junkfilter->jf_junkAttNo, &isNull);

	if (!isNull)
	{
		matched = true;
		tupleid = (ItemPointer) DatumGetPointer(datum);
		tuple_ctid = *tupleid;	/* be sure we don't free ctid!! */
		tupleid = &tuple_ctid;
	}
	else
	{
		matched = false;
		tupleid = NULL;			/* we don't need it for INSERT actions */
	}

	/*
	 * If we are dealing with a WHEN MATCHED case, we execute the first action
	 * for which the additional WHEN MATCHED AND quals pass. If an action
	 * without quals is found, that action is executed.
	 *
	 * Similarly, if we are dealing with WHEN NOT MATCHED case, we look at the
	 * given WHEN NOT MATCHED actions in sequence until one passes.
	 *
	 * Things get interesting in case of concurrent update/delete of the
	 * target tuple. Such concurrent update/delete is detected while we are
	 * executing a WHEN MATCHED action.
	 *
	 * A concurrent update can:
	 *
	 * 1. modify the target tuple so that it no longer satisfies the
	 * additional quals attached to the current WHEN MATCHED action OR
	 *
	 * In this case, we are still dealing with a WHEN MATCHED case, but
	 * we should recheck the list of WHEN MATCHED actions and choose the first
	 * one that satisfies the new target tuple.
	 *
	 * 2. modify the target tuple so that the join quals no longer pass and
	 * hence the source tuple no longer has a match.
	 *
	 * In the second case, the source tuple no longer matches the target tuple,
	 * so we now instead find a qualifying WHEN NOT MATCHED action to execute.
	 *
	 * A concurrent delete, changes a WHEN MATCHED case to WHEN NOT MATCHED.
	 *
	 * ExecMergeMatched takes care of following the update chain and
	 * re-finding the qualifying WHEN MATCHED action, as long as the updated
	 * target tuple still satisfies the join quals i.e. it still remains a
	 * WHEN MATCHED case. If the tuple gets deleted or the join quals fail, it
	 * returns and we try ExecMergeNotMatched. Given that ExecMergeMatched
	 * always make progress by following the update chain and we never switch
	 * from ExecMergeNotMatched to ExecMergeMatched, there is no risk of a
	 * livelock.
	 */
	if (matched)
		matched = ExecMergeMatched(mtstate, estate, slot, junkfilter, tupleid);

	/*
	 * Either we were dealing with a NOT MATCHED tuple or ExecMergeNotMatched()
	 * returned "false", indicating the previously MATCHED tuple is no longer a
	 * matching tuple.
	 */
	if (!matched)
		ExecMergeNotMatched(mtstate, estate, slot);
}
