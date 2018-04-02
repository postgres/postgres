/*-------------------------------------------------------------------------
 *
 * nodeModifyTable.c
 *	  routines to handle ModifyTable nodes.
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeModifyTable.c
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *		ExecInitModifyTable - initialize the ModifyTable node
 *		ExecModifyTable		- retrieve the next tuple from the node
 *		ExecEndModifyTable	- shut down the ModifyTable node
 *		ExecReScanModifyTable - rescan the ModifyTable node
 *
 *	 NOTES
 *		Each ModifyTable node contains a list of one or more subplans,
 *		much like an Append node.  There is one subplan per result relation.
 *		The key reason for this is that in an inherited UPDATE command, each
 *		result relation could have a different schema (more or different
 *		columns) requiring a different plan tree to produce it.  In an
 *		inherited DELETE, all the subplans should produce the same output
 *		rowtype, but we might still find that different plans are appropriate
 *		for different child relations.
 *
 *		If the query specifies RETURNING, then the ModifyTable returns a
 *		RETURNING tuple after completing each row insert, update, or delete.
 *		It must be called again to continue the operation.  Without RETURNING,
 *		we just loop within the node until all the work is done, then
 *		return NULL.  This avoids useless call/return overhead.
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "commands/trigger.h"
#include "executor/execPartition.h"
#include "executor/executor.h"
#include "executor/nodeMerge.h"
#include "executor/nodeModifyTable.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tqual.h"


static bool ExecOnConflictUpdate(ModifyTableState *mtstate,
					 ResultRelInfo *resultRelInfo,
					 ItemPointer conflictTid,
					 TupleTableSlot *planSlot,
					 TupleTableSlot *excludedSlot,
					 EState *estate,
					 bool canSetTag,
					 TupleTableSlot **returning);
static ResultRelInfo *getTargetResultRelInfo(ModifyTableState *node);
static void ExecSetupChildParentMapForTcs(ModifyTableState *mtstate);
static void ExecSetupChildParentMapForSubplan(ModifyTableState *mtstate);
static TupleConversionMap *tupconv_map_for_subplan(ModifyTableState *node,
						int whichplan);

/* flags for mt_merge_subcommands */
#define MERGE_INSERT	0x01
#define MERGE_UPDATE	0x02
#define MERGE_DELETE	0x04

/*
 * Verify that the tuples to be produced by INSERT or UPDATE match the
 * target relation's rowtype
 *
 * We do this to guard against stale plans.  If plan invalidation is
 * functioning properly then we should never get a failure here, but better
 * safe than sorry.  Note that this is called after we have obtained lock
 * on the target rel, so the rowtype can't change underneath us.
 *
 * The plan output is represented by its targetlist, because that makes
 * handling the dropped-column case easier.
 */
static void
ExecCheckPlanOutput(Relation resultRel, List *targetList)
{
	TupleDesc	resultDesc = RelationGetDescr(resultRel);
	int			attno = 0;
	ListCell   *lc;

	foreach(lc, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		Form_pg_attribute attr;

		if (tle->resjunk)
			continue;			/* ignore junk tlist items */

		if (attno >= resultDesc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table row type and query-specified row type do not match"),
					 errdetail("Query has too many columns.")));
		attr = TupleDescAttr(resultDesc, attno);
		attno++;

		if (!attr->attisdropped)
		{
			/* Normal case: demand type match */
			if (exprType((Node *) tle->expr) != attr->atttypid)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail("Table has type %s at ordinal position %d, but query expects %s.",
								   format_type_be(attr->atttypid),
								   attno,
								   format_type_be(exprType((Node *) tle->expr)))));
		}
		else
		{
			/*
			 * For a dropped column, we can't check atttypid (it's likely 0).
			 * In any case the planner has most likely inserted an INT4 null.
			 * What we insist on is just *some* NULL constant.
			 */
			if (!IsA(tle->expr, Const) ||
				!((Const *) tle->expr)->constisnull)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail("Query provides a value for a dropped column at ordinal position %d.",
								   attno)));
		}
	}
	if (attno != resultDesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("table row type and query-specified row type do not match"),
				 errdetail("Query has too few columns.")));
}

/*
 * ExecProcessReturning --- evaluate a RETURNING list
 *
 * resultRelInfo: current result rel
 * tupleSlot: slot holding tuple actually inserted/updated/deleted
 * planSlot: slot holding tuple returned by top subplan node
 *
 * Note: If tupleSlot is NULL, the FDW should have already provided econtext's
 * scan tuple.
 *
 * Returns a slot holding the result tuple
 */
static TupleTableSlot *
ExecProcessReturning(ResultRelInfo *resultRelInfo,
					 TupleTableSlot *tupleSlot,
					 TupleTableSlot *planSlot)
{
	ProjectionInfo *projectReturning = resultRelInfo->ri_projectReturning;
	ExprContext *econtext = projectReturning->pi_exprContext;

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous cycle.
	 */
	ResetExprContext(econtext);

	/* Make tuple and any needed join variables available to ExecProject */
	if (tupleSlot)
		econtext->ecxt_scantuple = tupleSlot;
	else
	{
		HeapTuple	tuple;

		/*
		 * RETURNING expressions might reference the tableoid column, so
		 * initialize t_tableOid before evaluating them.
		 */
		Assert(!TupIsNull(econtext->ecxt_scantuple));
		tuple = ExecMaterializeSlot(econtext->ecxt_scantuple);
		tuple->t_tableOid = RelationGetRelid(resultRelInfo->ri_RelationDesc);
	}
	econtext->ecxt_outertuple = planSlot;

	/* Compute the RETURNING expressions */
	return ExecProject(projectReturning);
}

/*
 * ExecCheckHeapTupleVisible -- verify heap tuple is visible
 *
 * It would not be consistent with guarantees of the higher isolation levels to
 * proceed with avoiding insertion (taking speculative insertion's alternative
 * path) on the basis of another tuple that is not visible to MVCC snapshot.
 * Check for the need to raise a serialization failure, and do so as necessary.
 */
static void
ExecCheckHeapTupleVisible(EState *estate,
						  HeapTuple tuple,
						  Buffer buffer)
{
	if (!IsolationUsesXactSnapshot())
		return;

	/*
	 * We need buffer pin and lock to call HeapTupleSatisfiesVisibility.
	 * Caller should be holding pin, but not lock.
	 */
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	if (!HeapTupleSatisfiesVisibility(tuple, estate->es_snapshot, buffer))
	{
		/*
		 * We should not raise a serialization failure if the conflict is
		 * against a tuple inserted by our own transaction, even if it's not
		 * visible to our snapshot.  (This would happen, for example, if
		 * conflicting keys are proposed for insertion in a single command.)
		 */
		if (!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tuple->t_data)))
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("could not serialize access due to concurrent update")));
	}
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
}

/*
 * ExecCheckTIDVisible -- convenience variant of ExecCheckHeapTupleVisible()
 */
static void
ExecCheckTIDVisible(EState *estate,
					ResultRelInfo *relinfo,
					ItemPointer tid)
{
	Relation	rel = relinfo->ri_RelationDesc;
	Buffer		buffer;
	HeapTupleData tuple;

	/* Redundantly check isolation level */
	if (!IsolationUsesXactSnapshot())
		return;

	tuple.t_self = *tid;
	if (!heap_fetch(rel, SnapshotAny, &tuple, &buffer, false, NULL))
		elog(ERROR, "failed to fetch conflicting tuple for ON CONFLICT");
	ExecCheckHeapTupleVisible(estate, &tuple, buffer);
	ReleaseBuffer(buffer);
}

/* ----------------------------------------------------------------
 *		ExecInsert
 *
 *		For INSERT, we have to insert the tuple into the target relation
 *		and insert appropriate tuples into the index relations.
 *
 *		Returns RETURNING result if any, otherwise NULL.
 * ----------------------------------------------------------------
 */
extern TupleTableSlot *
ExecInsert(ModifyTableState *mtstate,
		   TupleTableSlot *slot,
		   TupleTableSlot *planSlot,
		   EState *estate,
		   MergeActionState *actionState,
		   bool canSetTag)
{
	HeapTuple	tuple;
	ResultRelInfo *resultRelInfo;
	Relation	resultRelationDesc;
	Oid			newId;
	List	   *recheckIndexes = NIL;
	TupleTableSlot *result = NULL;
	TransitionCaptureState *ar_insert_trig_tcs;
	ModifyTable *node = (ModifyTable *) mtstate->ps.plan;
	OnConflictAction onconflict = node->onConflictAction;

	/*
	 * get the heap tuple out of the tuple table slot, making sure we have a
	 * writable copy
	 */
	tuple = ExecMaterializeSlot(slot);

	/*
	 * get information on the (current) result relation
	 */
	resultRelInfo = estate->es_result_relation_info;
	resultRelationDesc = resultRelInfo->ri_RelationDesc;

	/*
	 * If the result relation has OIDs, force the tuple's OID to zero so that
	 * heap_insert will assign a fresh OID.  Usually the OID already will be
	 * zero at this point, but there are corner cases where the plan tree can
	 * return a tuple extracted literally from some table with the same
	 * rowtype.
	 *
	 * XXX if we ever wanted to allow users to assign their own OIDs to new
	 * rows, this'd be the place to do it.  For the moment, we make a point of
	 * doing this before calling triggers, so that a user-supplied trigger
	 * could hack the OID if desired.
	 */
	if (resultRelationDesc->rd_rel->relhasoids)
		HeapTupleSetOid(tuple, InvalidOid);

	/*
	 * BEFORE ROW INSERT Triggers.
	 *
	 * Note: We fire BEFORE ROW TRIGGERS for every attempted insertion in an
	 * INSERT ... ON CONFLICT statement.  We cannot check for constraint
	 * violations before firing these triggers, because they can change the
	 * values to insert.  Also, they can run arbitrary user-defined code with
	 * side-effects that we can't cancel by just not inserting the tuple.
	 */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_insert_before_row)
	{
		slot = ExecBRInsertTriggers(estate, resultRelInfo, slot);

		if (slot == NULL)		/* "do nothing" */
			return NULL;

		/* trigger might have changed tuple */
		tuple = ExecMaterializeSlot(slot);
	}

	/* INSTEAD OF ROW INSERT Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_insert_instead_row)
	{
		slot = ExecIRInsertTriggers(estate, resultRelInfo, slot);

		if (slot == NULL)		/* "do nothing" */
			return NULL;

		/* trigger might have changed tuple */
		tuple = ExecMaterializeSlot(slot);

		newId = InvalidOid;
	}
	else if (resultRelInfo->ri_FdwRoutine)
	{
		/*
		 * insert into foreign table: let the FDW do it
		 */
		slot = resultRelInfo->ri_FdwRoutine->ExecForeignInsert(estate,
															   resultRelInfo,
															   slot,
															   planSlot);

		if (slot == NULL)		/* "do nothing" */
			return NULL;

		/* FDW might have changed tuple */
		tuple = ExecMaterializeSlot(slot);

		/*
		 * AFTER ROW Triggers or RETURNING expressions might reference the
		 * tableoid column, so initialize t_tableOid before evaluating them.
		 */
		tuple->t_tableOid = RelationGetRelid(resultRelationDesc);

		newId = InvalidOid;
	}
	else
	{
		WCOKind		wco_kind;
		bool		check_partition_constr;

		/*
		 * We always check the partition constraint, including when the tuple
		 * got here via tuple-routing.  However we don't need to in the latter
		 * case if no BR trigger is defined on the partition.  Note that a BR
		 * trigger might modify the tuple such that the partition constraint
		 * is no longer satisfied, so we need to check in that case.
		 */
		check_partition_constr = (resultRelInfo->ri_PartitionCheck != NIL);

		/*
		 * Constraints might reference the tableoid column, so initialize
		 * t_tableOid before evaluating them.
		 */
		tuple->t_tableOid = RelationGetRelid(resultRelationDesc);

		/*
		 * Check any RLS WITH CHECK policies.
		 *
		 * Normally we should check INSERT policies. But if the insert is the
		 * result of a partition key update that moved the tuple to a new
		 * partition, we should instead check UPDATE policies, because we are
		 * executing policies defined on the target table, and not those
		 * defined on the child partitions.
		 *
		 * If we're running MERGE, we refer to the action that we're executing
		 * to know if we're doing an INSERT or UPDATE to a partition table.
		 */
		if (mtstate->operation == CMD_UPDATE)
			wco_kind = WCO_RLS_UPDATE_CHECK;
		else if (mtstate->operation == CMD_MERGE)
			wco_kind = (actionState->commandType == CMD_UPDATE) ?
				WCO_RLS_UPDATE_CHECK : WCO_RLS_INSERT_CHECK;
		else
			wco_kind = WCO_RLS_INSERT_CHECK;

		/*
		 * ExecWithCheckOptions() will skip any WCOs which are not of the kind
		 * we are looking for at this point.
		 */
		if (resultRelInfo->ri_WithCheckOptions != NIL)
			ExecWithCheckOptions(wco_kind, resultRelInfo, slot, estate);

		/*
		 * No need though if the tuple has been routed, and a BR trigger
		 * doesn't exist.
		 */
		if (resultRelInfo->ri_PartitionRoot != NULL &&
			!(resultRelInfo->ri_TrigDesc &&
			  resultRelInfo->ri_TrigDesc->trig_insert_before_row))
			check_partition_constr = false;

		/* Check the constraints of the tuple */
		if (resultRelationDesc->rd_att->constr || check_partition_constr)
			ExecConstraints(resultRelInfo, slot, estate, true);

		if (onconflict != ONCONFLICT_NONE && resultRelInfo->ri_NumIndices > 0)
		{
			/* Perform a speculative insertion. */
			uint32		specToken;
			ItemPointerData conflictTid;
			bool		specConflict;
			List	   *arbiterIndexes;

			arbiterIndexes = resultRelInfo->ri_onConflictArbiterIndexes;

			/*
			 * Do a non-conclusive check for conflicts first.
			 *
			 * We're not holding any locks yet, so this doesn't guarantee that
			 * the later insert won't conflict.  But it avoids leaving behind
			 * a lot of canceled speculative insertions, if you run a lot of
			 * INSERT ON CONFLICT statements that do conflict.
			 *
			 * We loop back here if we find a conflict below, either during
			 * the pre-check, or when we re-check after inserting the tuple
			 * speculatively.
			 */
	vlock:
			specConflict = false;
			if (!ExecCheckIndexConstraints(slot, estate, &conflictTid,
										   arbiterIndexes))
			{
				/* committed conflict tuple found */
				if (onconflict == ONCONFLICT_UPDATE)
				{
					/*
					 * In case of ON CONFLICT DO UPDATE, execute the UPDATE
					 * part.  Be prepared to retry if the UPDATE fails because
					 * of another concurrent UPDATE/DELETE to the conflict
					 * tuple.
					 */
					TupleTableSlot *returning = NULL;

					if (ExecOnConflictUpdate(mtstate, resultRelInfo,
											 &conflictTid, planSlot, slot,
											 estate, canSetTag, &returning))
					{
						InstrCountFiltered2(&mtstate->ps, 1);
						return returning;
					}
					else
						goto vlock;
				}
				else
				{
					/*
					 * In case of ON CONFLICT DO NOTHING, do nothing. However,
					 * verify that the tuple is visible to the executor's MVCC
					 * snapshot at higher isolation levels.
					 */
					Assert(onconflict == ONCONFLICT_NOTHING);
					ExecCheckTIDVisible(estate, resultRelInfo, &conflictTid);
					InstrCountFiltered2(&mtstate->ps, 1);
					return NULL;
				}
			}

			/*
			 * Before we start insertion proper, acquire our "speculative
			 * insertion lock".  Others can use that to wait for us to decide
			 * if we're going to go ahead with the insertion, instead of
			 * waiting for the whole transaction to complete.
			 */
			specToken = SpeculativeInsertionLockAcquire(GetCurrentTransactionId());
			HeapTupleHeaderSetSpeculativeToken(tuple->t_data, specToken);

			/* insert the tuple, with the speculative token */
			newId = heap_insert(resultRelationDesc, tuple,
								estate->es_output_cid,
								HEAP_INSERT_SPECULATIVE,
								NULL);

			/* insert index entries for tuple */
			recheckIndexes = ExecInsertIndexTuples(slot, &(tuple->t_self),
												   estate, true, &specConflict,
												   arbiterIndexes);

			/* adjust the tuple's state accordingly */
			if (!specConflict)
				heap_finish_speculative(resultRelationDesc, tuple);
			else
				heap_abort_speculative(resultRelationDesc, tuple);

			/*
			 * Wake up anyone waiting for our decision.  They will re-check
			 * the tuple, see that it's no longer speculative, and wait on our
			 * XID as if this was a regularly inserted tuple all along.  Or if
			 * we killed the tuple, they will see it's dead, and proceed as if
			 * the tuple never existed.
			 */
			SpeculativeInsertionLockRelease(GetCurrentTransactionId());

			/*
			 * If there was a conflict, start from the beginning.  We'll do
			 * the pre-check again, which will now find the conflicting tuple
			 * (unless it aborts before we get there).
			 */
			if (specConflict)
			{
				list_free(recheckIndexes);
				goto vlock;
			}

			/* Since there was no insertion conflict, we're done */
		}
		else
		{
			/*
			 * insert the tuple normally.
			 *
			 * Note: heap_insert returns the tid (location) of the new tuple
			 * in the t_self field.
			 */
			newId = heap_insert(resultRelationDesc, tuple,
								estate->es_output_cid,
								0, NULL);

			/* insert index entries for tuple */
			if (resultRelInfo->ri_NumIndices > 0)
				recheckIndexes = ExecInsertIndexTuples(slot, &(tuple->t_self),
													   estate, false, NULL,
													   NIL);
		}
	}

	if (canSetTag)
	{
		(estate->es_processed)++;
		estate->es_lastoid = newId;
		setLastTid(&(tuple->t_self));
	}

	/*
	 * If this insert is the result of a partition key update that moved the
	 * tuple to a new partition, put this row into the transition NEW TABLE,
	 * if there is one. We need to do this separately for DELETE and INSERT
	 * because they happen on different tables.
	 */
	ar_insert_trig_tcs = mtstate->mt_transition_capture;
	if (mtstate->operation == CMD_UPDATE && mtstate->mt_transition_capture
		&& mtstate->mt_transition_capture->tcs_update_new_table)
	{
		ExecARUpdateTriggers(estate, resultRelInfo, NULL,
							 NULL,
							 tuple,
							 NULL,
							 mtstate->mt_transition_capture);

		/*
		 * We've already captured the NEW TABLE row, so make sure any AR
		 * INSERT trigger fired below doesn't capture it again.
		 */
		ar_insert_trig_tcs = NULL;
	}

	/* AFTER ROW INSERT Triggers */
	ExecARInsertTriggers(estate, resultRelInfo, tuple, recheckIndexes,
						 ar_insert_trig_tcs);

	list_free(recheckIndexes);

	/*
	 * Check any WITH CHECK OPTION constraints from parent views.  We are
	 * required to do this after testing all constraints and uniqueness
	 * violations per the SQL spec, so we do it after actually inserting the
	 * record into the heap and all indexes.
	 *
	 * ExecWithCheckOptions will elog(ERROR) if a violation is found, so the
	 * tuple will never be seen, if it violates the WITH CHECK OPTION.
	 *
	 * ExecWithCheckOptions() will skip any WCOs which are not of the kind we
	 * are looking for at this point.
	 */
	if (resultRelInfo->ri_WithCheckOptions != NIL)
		ExecWithCheckOptions(WCO_VIEW_CHECK, resultRelInfo, slot, estate);

	/* Process RETURNING if present */
	if (resultRelInfo->ri_projectReturning)
		result = ExecProcessReturning(resultRelInfo, slot, planSlot);

	return result;
}

/* ----------------------------------------------------------------
 *		ExecDelete
 *
 *		DELETE is like UPDATE, except that we delete the tuple and no
 *		index modifications are needed.
 *
 *		When deleting from a table, tupleid identifies the tuple to
 *		delete and oldtuple is NULL.  When deleting from a view,
 *		oldtuple is passed to the INSTEAD OF triggers and identifies
 *		what to delete, and tupleid is invalid.  When deleting from a
 *		foreign table, tupleid is invalid; the FDW has to figure out
 *		which row to delete using data from the planSlot.  oldtuple is
 *		passed to foreign table triggers; it is NULL when the foreign
 *		table has no relevant triggers.
 *
 *		MERGE passes actionState of the action it's currently executing;
 *		regular DELETE passes NULL. This is used by ExecDelete to know if it's
 *		being called from MERGE or regular DELETE operation.
 *
 *		If the DELETE fails because the tuple is concurrently updated/deleted
 *		by this or some other transaction, hufdp is filled with the reason as
 *		well as other important information. Currently only MERGE needs this
 *		information.
 *
 *		Returns RETURNING result if any, otherwise NULL.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecDelete(ModifyTableState *mtstate,
		   ItemPointer tupleid,
		   HeapTuple oldtuple,
		   TupleTableSlot *planSlot,
		   EPQState *epqstate,
		   EState *estate,
		   bool *tupleDeleted,
		   bool processReturning,
		   HeapUpdateFailureData *hufdp,
		   MergeActionState *actionState,
		   bool canSetTag)
{
	ResultRelInfo *resultRelInfo;
	Relation	resultRelationDesc;
	HTSU_Result result;
	HeapUpdateFailureData hufd;
	TupleTableSlot *slot = NULL;
	TransitionCaptureState *ar_delete_trig_tcs;

	if (tupleDeleted)
		*tupleDeleted = false;

	/*
	 * Initialize hufdp. Since the caller is only interested in the failure
	 * status, initialize with the state that is used to indicate successful
	 * operation.
	 */
	if (hufdp)
		hufdp->result = HeapTupleMayBeUpdated;

	/*
	 * get information on the (current) result relation
	 */
	resultRelInfo = estate->es_result_relation_info;
	resultRelationDesc = resultRelInfo->ri_RelationDesc;

	/* BEFORE ROW DELETE Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_delete_before_row)
	{
		bool		dodelete;

		dodelete = ExecBRDeleteTriggers(estate, epqstate, resultRelInfo,
										tupleid, oldtuple, hufdp);

		if (!dodelete)			/* "do nothing" */
			return NULL;
	}

	/* INSTEAD OF ROW DELETE Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_delete_instead_row)
	{
		bool		dodelete;

		Assert(oldtuple != NULL);
		dodelete = ExecIRDeleteTriggers(estate, resultRelInfo, oldtuple);

		if (!dodelete)			/* "do nothing" */
			return NULL;
	}
	else if (resultRelInfo->ri_FdwRoutine)
	{
		HeapTuple	tuple;

		/*
		 * delete from foreign table: let the FDW do it
		 *
		 * We offer the trigger tuple slot as a place to store RETURNING data,
		 * although the FDW can return some other slot if it wants.  Set up
		 * the slot's tupdesc so the FDW doesn't need to do that for itself.
		 */
		slot = estate->es_trig_tuple_slot;
		if (slot->tts_tupleDescriptor != RelationGetDescr(resultRelationDesc))
			ExecSetSlotDescriptor(slot, RelationGetDescr(resultRelationDesc));

		slot = resultRelInfo->ri_FdwRoutine->ExecForeignDelete(estate,
															   resultRelInfo,
															   slot,
															   planSlot);

		if (slot == NULL)		/* "do nothing" */
			return NULL;

		/*
		 * RETURNING expressions might reference the tableoid column, so
		 * initialize t_tableOid before evaluating them.
		 */
		if (slot->tts_isempty)
			ExecStoreAllNullTuple(slot);
		tuple = ExecMaterializeSlot(slot);
		tuple->t_tableOid = RelationGetRelid(resultRelationDesc);
	}
	else
	{
		/*
		 * delete the tuple
		 *
		 * Note: if es_crosscheck_snapshot isn't InvalidSnapshot, we check
		 * that the row to be deleted is visible to that snapshot, and throw a
		 * can't-serialize error if not. This is a special-case behavior
		 * needed for referential integrity updates in transaction-snapshot
		 * mode transactions.
		 */
ldelete:;
		result = heap_delete(resultRelationDesc, tupleid,
							 estate->es_output_cid,
							 estate->es_crosscheck_snapshot,
							 true /* wait for commit */ ,
							 &hufd);

		/*
		 * Copy the necessary information, if the caller has asked for it. We
		 * must do this irrespective of whether the tuple was updated or
		 * deleted.
		 */
		if (hufdp)
			*hufdp = hufd;

		switch (result)
		{
			case HeapTupleSelfUpdated:

				/*
				 * The target tuple was already updated or deleted by the
				 * current command, or by a later command in the current
				 * transaction.  The former case is possible in a join DELETE
				 * where multiple tuples join to the same target tuple. This
				 * is somewhat questionable, but Postgres has always allowed
				 * it: we just ignore additional deletion attempts.
				 *
				 * The latter case arises if the tuple is modified by a
				 * command in a BEFORE trigger, or perhaps by a command in a
				 * volatile function used in the query.  In such situations we
				 * should not ignore the deletion, but it is equally unsafe to
				 * proceed.  We don't want to discard the original DELETE
				 * while keeping the triggered actions based on its deletion;
				 * and it would be no better to allow the original DELETE
				 * while discarding updates that it triggered.  The row update
				 * carries some information that might be important according
				 * to business rules; so throwing an error is the only safe
				 * course.
				 *
				 * If a trigger actually intends this type of interaction, it
				 * can re-execute the DELETE and then return NULL to cancel
				 * the outer delete.
				 */
				if (hufd.cmax != estate->es_output_cid)
					ereport(ERROR,
							(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
							 errmsg("tuple to be updated was already modified by an operation triggered by the current command"),
							 errhint("Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.")));

				/*
				 * Else, already deleted by self; nothing to do but inform
				 * MERGE about it anyways so that it can take necessary
				 * action.
				 */
				return NULL;

			case HeapTupleMayBeUpdated:
				break;

			case HeapTupleUpdated:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent update")));

				if (!ItemPointerEquals(tupleid, &hufd.ctid))
				{
					TupleTableSlot *epqslot;

					/*
					 * If we're executing MERGE, then the onus of running
					 * EvalPlanQual() and handling its outcome lies with the
					 * caller.
					 */
					if (actionState != NULL)
						return NULL;

					/* Normal DELETE path.  */
					epqslot = EvalPlanQual(estate,
										   epqstate,
										   resultRelationDesc,
										   GetEPQRangeTableIndex(resultRelInfo),
										   LockTupleExclusive,
										   &hufd.ctid,
										   hufd.xmax);
					if (!TupIsNull(epqslot))
					{
						*tupleid = hufd.ctid;
						goto ldelete;
					}
				}

				/*
				 * tuple already deleted; nothing to do. But MERGE might want
				 * to handle it differently. We've already filled-in hufdp
				 * with sufficient information for MERGE to look at.
				 */
				return NULL;

			default:
				elog(ERROR, "unrecognized heap_delete status: %u", result);
				return NULL;
		}

		/*
		 * Note: Normally one would think that we have to delete index tuples
		 * associated with the heap tuple now...
		 *
		 * ... but in POSTGRES, we have no need to do this because VACUUM will
		 * take care of it later.  We can't delete index tuples immediately
		 * anyway, since the tuple is still visible to other transactions.
		 */
	}

	if (canSetTag)
		(estate->es_processed)++;

	/* Tell caller that the delete actually happened. */
	if (tupleDeleted)
		*tupleDeleted = true;

	/*
	 * If this delete is the result of a partition key update that moved the
	 * tuple to a new partition, put this row into the transition OLD TABLE,
	 * if there is one. We need to do this separately for DELETE and INSERT
	 * because they happen on different tables.
	 */
	ar_delete_trig_tcs = mtstate->mt_transition_capture;
	if (mtstate->operation == CMD_UPDATE && mtstate->mt_transition_capture
		&& mtstate->mt_transition_capture->tcs_update_old_table)
	{
		ExecARUpdateTriggers(estate, resultRelInfo,
							 tupleid,
							 oldtuple,
							 NULL,
							 NULL,
							 mtstate->mt_transition_capture);

		/*
		 * We've already captured the NEW TABLE row, so make sure any AR
		 * DELETE trigger fired below doesn't capture it again.
		 */
		ar_delete_trig_tcs = NULL;
	}

	/* AFTER ROW DELETE Triggers */
	ExecARDeleteTriggers(estate, resultRelInfo, tupleid, oldtuple,
						 ar_delete_trig_tcs);

	/* Process RETURNING if present and if requested */
	if (processReturning && resultRelInfo->ri_projectReturning)
	{
		/*
		 * We have to put the target tuple into a slot, which means first we
		 * gotta fetch it.  We can use the trigger tuple slot.
		 */
		TupleTableSlot *rslot;
		HeapTupleData deltuple;
		Buffer		delbuffer;

		if (resultRelInfo->ri_FdwRoutine)
		{
			/* FDW must have provided a slot containing the deleted row */
			Assert(!TupIsNull(slot));
			delbuffer = InvalidBuffer;
		}
		else
		{
			slot = estate->es_trig_tuple_slot;
			if (oldtuple != NULL)
			{
				deltuple = *oldtuple;
				delbuffer = InvalidBuffer;
			}
			else
			{
				deltuple.t_self = *tupleid;
				if (!heap_fetch(resultRelationDesc, SnapshotAny,
								&deltuple, &delbuffer, false, NULL))
					elog(ERROR, "failed to fetch deleted tuple for DELETE RETURNING");
			}

			if (slot->tts_tupleDescriptor != RelationGetDescr(resultRelationDesc))
				ExecSetSlotDescriptor(slot, RelationGetDescr(resultRelationDesc));
			ExecStoreTuple(&deltuple, slot, InvalidBuffer, false);
		}

		rslot = ExecProcessReturning(resultRelInfo, slot, planSlot);

		/*
		 * Before releasing the target tuple again, make sure rslot has a
		 * local copy of any pass-by-reference values.
		 */
		ExecMaterializeSlot(rslot);

		ExecClearTuple(slot);
		if (BufferIsValid(delbuffer))
			ReleaseBuffer(delbuffer);

		return rslot;
	}

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecUpdate
 *
 *		note: we can't run UPDATE queries with transactions
 *		off because UPDATEs are actually INSERTs and our
 *		scan will mistakenly loop forever, updating the tuple
 *		it just inserted..  This should be fixed but until it
 *		is, we don't want to get stuck in an infinite loop
 *		which corrupts your database..
 *
 *		When updating a table, tupleid identifies the tuple to
 *		update and oldtuple is NULL.  When updating a view, oldtuple
 *		is passed to the INSTEAD OF triggers and identifies what to
 *		update, and tupleid is invalid.  When updating a foreign table,
 *		tupleid is invalid; the FDW has to figure out which row to
 *		update using data from the planSlot.  oldtuple is passed to
 *		foreign table triggers; it is NULL when the foreign table has
 *		no relevant triggers.
 *
 *		MERGE passes actionState of the action it's currently executing;
 *		regular UPDATE passes NULL. This is used by ExecUpdate to know if it's
 *		being called from MERGE or regular UPDATE operation. ExecUpdate may
 *		pass this information to ExecInsert if it ends up running DELETE+INSERT
 *		for partition key updates.
 *
 *		If the UPDATE fails because the tuple is concurrently updated/deleted
 *		by this or some other transaction, hufdp is filled with the reason as
 *		well as other important information. Currently only MERGE needs this
 *		information.
 *
 *		Returns RETURNING result if any, otherwise NULL.
 * ----------------------------------------------------------------
 */
extern TupleTableSlot *
ExecUpdate(ModifyTableState *mtstate,
		   ItemPointer tupleid,
		   HeapTuple oldtuple,
		   TupleTableSlot *slot,
		   TupleTableSlot *planSlot,
		   EPQState *epqstate,
		   EState *estate,
		   bool *tuple_updated,
		   HeapUpdateFailureData *hufdp,
		   MergeActionState *actionState,
		   bool canSetTag)
{
	HeapTuple	tuple;
	ResultRelInfo *resultRelInfo;
	Relation	resultRelationDesc;
	HTSU_Result result;
	HeapUpdateFailureData hufd;
	List	   *recheckIndexes = NIL;
	TupleConversionMap *saved_tcs_map = NULL;

	/*
	 * abort the operation if not running transactions
	 */
	if (IsBootstrapProcessingMode())
		elog(ERROR, "cannot UPDATE during bootstrap");

	if (tuple_updated)
		*tuple_updated = false;

	/*
	 * Initialize hufdp. Since the caller is only interested in the failure
	 * status, initialize with the state that is used to indicate successful
	 * operation.
	 */
	if (hufdp)
		hufdp->result = HeapTupleMayBeUpdated;

	/*
	 * get the heap tuple out of the tuple table slot, making sure we have a
	 * writable copy
	 */
	tuple = ExecMaterializeSlot(slot);

	/*
	 * get information on the (current) result relation
	 */
	resultRelInfo = estate->es_result_relation_info;
	resultRelationDesc = resultRelInfo->ri_RelationDesc;

	/* BEFORE ROW UPDATE Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_update_before_row)
	{
		slot = ExecBRUpdateTriggers(estate, epqstate, resultRelInfo,
									tupleid, oldtuple, slot, hufdp);

		if (slot == NULL)		/* "do nothing" */
			return NULL;

		/* trigger might have changed tuple */
		tuple = ExecMaterializeSlot(slot);
	}

	/* INSTEAD OF ROW UPDATE Triggers */
	if (resultRelInfo->ri_TrigDesc &&
		resultRelInfo->ri_TrigDesc->trig_update_instead_row)
	{
		slot = ExecIRUpdateTriggers(estate, resultRelInfo,
									oldtuple, slot);

		if (slot == NULL)		/* "do nothing" */
			return NULL;

		/* trigger might have changed tuple */
		tuple = ExecMaterializeSlot(slot);
	}
	else if (resultRelInfo->ri_FdwRoutine)
	{
		/*
		 * update in foreign table: let the FDW do it
		 */
		slot = resultRelInfo->ri_FdwRoutine->ExecForeignUpdate(estate,
															   resultRelInfo,
															   slot,
															   planSlot);

		if (slot == NULL)		/* "do nothing" */
			return NULL;

		/* FDW might have changed tuple */
		tuple = ExecMaterializeSlot(slot);

		/*
		 * AFTER ROW Triggers or RETURNING expressions might reference the
		 * tableoid column, so initialize t_tableOid before evaluating them.
		 */
		tuple->t_tableOid = RelationGetRelid(resultRelationDesc);
	}
	else
	{
		bool		partition_constraint_failed;

		/*
		 * Constraints might reference the tableoid column, so initialize
		 * t_tableOid before evaluating them.
		 */
		tuple->t_tableOid = RelationGetRelid(resultRelationDesc);

		/*
		 * Check any RLS UPDATE WITH CHECK policies
		 *
		 * If we generate a new candidate tuple after EvalPlanQual testing, we
		 * must loop back here and recheck any RLS policies and constraints.
		 * (We don't need to redo triggers, however.  If there are any BEFORE
		 * triggers then trigger.c will have done heap_lock_tuple to lock the
		 * correct tuple, so there's no need to do them again.)
		 */
lreplace:;

		/*
		 * If partition constraint fails, this row might get moved to another
		 * partition, in which case we should check the RLS CHECK policy just
		 * before inserting into the new partition, rather than doing it here.
		 * This is because a trigger on that partition might again change the
		 * row.  So skip the WCO checks if the partition constraint fails.
		 */
		partition_constraint_failed =
			resultRelInfo->ri_PartitionCheck &&
			!ExecPartitionCheck(resultRelInfo, slot, estate);

		if (!partition_constraint_failed &&
			resultRelInfo->ri_WithCheckOptions != NIL)
		{
			/*
			 * ExecWithCheckOptions() will skip any WCOs which are not of the
			 * kind we are looking for at this point.
			 */
			ExecWithCheckOptions(WCO_RLS_UPDATE_CHECK,
								 resultRelInfo, slot, estate);
		}

		/*
		 * If a partition check failed, try to move the row into the right
		 * partition.
		 */
		if (partition_constraint_failed)
		{
			bool		tuple_deleted;
			TupleTableSlot *ret_slot;
			PartitionTupleRouting *proute = mtstate->mt_partition_tuple_routing;
			int			map_index;
			TupleConversionMap *tupconv_map;

			/*
			 * Disallow an INSERT ON CONFLICT DO UPDATE that causes the
			 * original row to migrate to a different partition.  Maybe this
			 * can be implemented some day, but it seems a fringe feature with
			 * little redeeming value.
			 */
			if (((ModifyTable *) mtstate->ps.plan)->onConflictAction == ONCONFLICT_UPDATE)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("invalid ON UPDATE specification"),
						 errdetail("The result tuple would appear in a different partition than the original tuple.")));

			/*
			 * When an UPDATE is run on a leaf partition, we will not have
			 * partition tuple routing set up. In that case, fail with
			 * partition constraint violation error.
			 */
			if (proute == NULL)
				ExecPartitionCheckEmitError(resultRelInfo, slot, estate);

			/*
			 * Row movement, part 1.  Delete the tuple, but skip RETURNING
			 * processing. We want to return rows from INSERT.
			 */
			ExecDelete(mtstate, tupleid, oldtuple, planSlot, epqstate,
					   estate, &tuple_deleted, false, hufdp, NULL,
					   false);

			/*
			 * For some reason if DELETE didn't happen (e.g. trigger prevented
			 * it, or it was already deleted by self, or it was concurrently
			 * deleted by another transaction), then we should skip the insert
			 * as well; otherwise, an UPDATE could cause an increase in the
			 * total number of rows across all partitions, which is clearly
			 * wrong.
			 *
			 * For a normal UPDATE, the case where the tuple has been the
			 * subject of a concurrent UPDATE or DELETE would be handled by
			 * the EvalPlanQual machinery, but for an UPDATE that we've
			 * translated into a DELETE from this partition and an INSERT into
			 * some other partition, that's not available, because CTID chains
			 * can't span relation boundaries.  We mimic the semantics to a
			 * limited extent by skipping the INSERT if the DELETE fails to
			 * find a tuple. This ensures that two concurrent attempts to
			 * UPDATE the same tuple at the same time can't turn one tuple
			 * into two, and that an UPDATE of a just-deleted tuple can't
			 * resurrect it.
			 */
			if (!tuple_deleted)
				return NULL;

			/*
			 * Updates set the transition capture map only when a new subplan
			 * is chosen.  But for inserts, it is set for each row. So after
			 * INSERT, we need to revert back to the map created for UPDATE;
			 * otherwise the next UPDATE will incorrectly use the one created
			 * for INSERT.  So first save the one created for UPDATE.
			 */
			if (mtstate->mt_transition_capture)
				saved_tcs_map = mtstate->mt_transition_capture->tcs_map;

			/*
			 * We should convert the tuple into root's tuple descriptor, since
			 * ExecInsert() starts the search from root. To do that, we need to
			 * retrieve the tuple conversion map for this resultRelInfo.
			 *
			 * If we're running MERGE then resultRelInfo is per-partition
			 * resultRelInfo as initialized in ExecInitPartitionInfo(). Note
			 * that we don't expand inheritance for the resultRelation in case
			 * of MERGE and hence there is just one subplan. Whereas for
			 * regular UPDATE, resultRelInfo is one of the per-subplan
			 * resultRelInfos. In either case the position of this partition in
			 * tracked in ri_PartitionLeafIndex;
			 *
			 * Retrieve the map either by looking at the resultRelInfo's
			 * position in mtstate->resultRelInfo[] (for UPDATE) or by simply
			 * using the ri_PartitionLeafIndex value (for MERGE).
			 */
			if (mtstate->operation == CMD_MERGE)
			{
				map_index = resultRelInfo->ri_PartitionLeafIndex;
				Assert(mtstate->rootResultRelInfo == NULL);
				tupconv_map = TupConvMapForLeaf(proute,
								mtstate->resultRelInfo,
								map_index);
			}
			else
			{
				map_index = resultRelInfo - mtstate->resultRelInfo;
				Assert(map_index >= 0 && map_index < mtstate->mt_nplans);
				tupconv_map = tupconv_map_for_subplan(mtstate, map_index);
			}
			tuple = ConvertPartitionTupleSlot(tupconv_map,
											  tuple,
											  proute->root_tuple_slot,
											  &slot);

			/*
			 * Prepare for tuple routing, making it look like we're inserting
			 * into the root.
			 */
			slot = ExecPrepareTupleRouting(mtstate, estate, proute,
										   getTargetResultRelInfo(mtstate),
										   slot);

			ret_slot = ExecInsert(mtstate, slot, planSlot,
								  estate, actionState, canSetTag);

			/* Update is successful. */
			if (tuple_updated)
				*tuple_updated = true;

			/* Revert ExecPrepareTupleRouting's node change. */
			estate->es_result_relation_info = resultRelInfo;
			if (mtstate->mt_transition_capture)
			{
				mtstate->mt_transition_capture->tcs_original_insert_tuple = NULL;
				mtstate->mt_transition_capture->tcs_map = saved_tcs_map;
			}

			return ret_slot;
		}

		/*
		 * Check the constraints of the tuple.  Note that we pass the same
		 * slot for the orig_slot argument, because unlike ExecInsert(), no
		 * tuple-routing is performed here, hence the slot remains unchanged.
		 * We've already checked the partition constraint above; however, we
		 * must still ensure the tuple passes all other constraints, so we
		 * will call ExecConstraints() and have it validate all remaining
		 * checks.
		 */
		if (resultRelationDesc->rd_att->constr)
			ExecConstraints(resultRelInfo, slot, estate, false);

		/*
		 * replace the heap tuple
		 *
		 * Note: if es_crosscheck_snapshot isn't InvalidSnapshot, we check
		 * that the row to be updated is visible to that snapshot, and throw a
		 * can't-serialize error if not. This is a special-case behavior
		 * needed for referential integrity updates in transaction-snapshot
		 * mode transactions.
		 */
		result = heap_update(resultRelationDesc, tupleid, tuple,
							 estate->es_output_cid,
							 estate->es_crosscheck_snapshot,
							 true /* wait for commit */ ,
							 &hufd);

		/*
		 * Copy the necessary information, if the caller has asked for it. We
		 * must do this irrespective of whether the tuple was updated or
		 * deleted.
		 */
		if (hufdp)
			*hufdp = hufd;

		switch (result)
		{
			case HeapTupleSelfUpdated:

				/*
				 * The target tuple was already updated or deleted by the
				 * current command, or by a later command in the current
				 * transaction.  The former case is possible in a join UPDATE
				 * where multiple tuples join to the same target tuple. This
				 * is pretty questionable, but Postgres has always allowed it:
				 * we just execute the first update action and ignore
				 * additional update attempts.
				 *
				 * The latter case arises if the tuple is modified by a
				 * command in a BEFORE trigger, or perhaps by a command in a
				 * volatile function used in the query.  In such situations we
				 * should not ignore the update, but it is equally unsafe to
				 * proceed.  We don't want to discard the original UPDATE
				 * while keeping the triggered actions based on it; and we
				 * have no principled way to merge this update with the
				 * previous ones.  So throwing an error is the only safe
				 * course.
				 *
				 * If a trigger actually intends this type of interaction, it
				 * can re-execute the UPDATE (assuming it can figure out how)
				 * and then return NULL to cancel the outer update.
				 */
				if (hufd.cmax != estate->es_output_cid)
					ereport(ERROR,
							(errcode(ERRCODE_TRIGGERED_DATA_CHANGE_VIOLATION),
							 errmsg("tuple to be updated was already modified by an operation triggered by the current command"),
							 errhint("Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.")));

				/* Else, already updated by self; nothing to do */
				return NULL;

			case HeapTupleMayBeUpdated:
				break;

			case HeapTupleUpdated:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent update")));

				if (!ItemPointerEquals(tupleid, &hufd.ctid))
				{
					TupleTableSlot *epqslot;

					/*
					 * If we're executing MERGE, then the onus of running
					 * EvalPlanQual() and handling its outcome lies with the
					 * caller.
					 */
					if (actionState != NULL)
						return NULL;

					/* Regular UPDATE path. */
					epqslot = EvalPlanQual(estate,
										   epqstate,
										   resultRelationDesc,
										   GetEPQRangeTableIndex(resultRelInfo),
										   hufd.lockmode,
										   &hufd.ctid,
										   hufd.xmax);
					if (!TupIsNull(epqslot))
					{
						*tupleid = hufd.ctid;
						/* Normal UPDATE path */
						slot = ExecFilterJunk(resultRelInfo->ri_junkFilter, epqslot);
						tuple = ExecMaterializeSlot(slot);
						goto lreplace;
					}
				}

				/*
				 * tuple already deleted; nothing to do. But MERGE might want
				 * to handle it differently. We've already filled-in hufdp
				 * with sufficient information for MERGE to look at.
				 */
				return NULL;

			default:
				elog(ERROR, "unrecognized heap_update status: %u", result);
				return NULL;
		}

		/*
		 * Note: instead of having to update the old index tuples associated
		 * with the heap tuple, all we do is form and insert new index tuples.
		 * This is because UPDATEs are actually DELETEs and INSERTs, and index
		 * tuple deletion is done later by VACUUM (see notes in ExecDelete).
		 * All we do here is insert new index tuples.  -cim 9/27/89
		 */

		/*
		 * insert index entries for tuple
		 *
		 * Note: heap_update returns the tid (location) of the new tuple in
		 * the t_self field.
		 *
		 * If it's a HOT update, we mustn't insert new index entries.
		 */
		if (resultRelInfo->ri_NumIndices > 0 && !HeapTupleIsHeapOnly(tuple))
			recheckIndexes = ExecInsertIndexTuples(slot, &(tuple->t_self),
												   estate, false, NULL, NIL);
	}

	if (tuple_updated)
		*tuple_updated = true;

	if (canSetTag)
		(estate->es_processed)++;

	/* AFTER ROW UPDATE Triggers */
	ExecARUpdateTriggers(estate, resultRelInfo, tupleid, oldtuple, tuple,
						 recheckIndexes,
						 mtstate->operation == CMD_INSERT ?
						 mtstate->mt_oc_transition_capture :
						 mtstate->mt_transition_capture);

	list_free(recheckIndexes);

	/*
	 * Check any WITH CHECK OPTION constraints from parent views.  We are
	 * required to do this after testing all constraints and uniqueness
	 * violations per the SQL spec, so we do it after actually updating the
	 * record in the heap and all indexes.
	 *
	 * ExecWithCheckOptions() will skip any WCOs which are not of the kind we
	 * are looking for at this point.
	 */
	if (resultRelInfo->ri_WithCheckOptions != NIL)
		ExecWithCheckOptions(WCO_VIEW_CHECK, resultRelInfo, slot, estate);

	/* Process RETURNING if present */
	if (resultRelInfo->ri_projectReturning)
		return ExecProcessReturning(resultRelInfo, slot, planSlot);

	return NULL;
}

/*
 * ExecOnConflictUpdate --- execute UPDATE of INSERT ON CONFLICT DO UPDATE
 *
 * Try to lock tuple for update as part of speculative insertion.  If
 * a qual originating from ON CONFLICT DO UPDATE is satisfied, update
 * (but still lock row, even though it may not satisfy estate's
 * snapshot).
 *
 * Returns true if if we're done (with or without an update), or false if
 * the caller must retry the INSERT from scratch.
 */
static bool
ExecOnConflictUpdate(ModifyTableState *mtstate,
					 ResultRelInfo *resultRelInfo,
					 ItemPointer conflictTid,
					 TupleTableSlot *planSlot,
					 TupleTableSlot *excludedSlot,
					 EState *estate,
					 bool canSetTag,
					 TupleTableSlot **returning)
{
	ExprContext *econtext = mtstate->ps.ps_ExprContext;
	Relation	relation = resultRelInfo->ri_RelationDesc;
	ExprState  *onConflictSetWhere = resultRelInfo->ri_onConflict->oc_WhereClause;
	HeapTupleData tuple;
	HeapUpdateFailureData hufd;
	LockTupleMode lockmode;
	HTSU_Result test;
	Buffer		buffer;

	/* Determine lock mode to use */
	lockmode = ExecUpdateLockMode(estate, resultRelInfo);

	/*
	 * Lock tuple for update.  Don't follow updates when tuple cannot be
	 * locked without doing so.  A row locking conflict here means our
	 * previous conclusion that the tuple is conclusively committed is not
	 * true anymore.
	 */
	tuple.t_self = *conflictTid;
	test = heap_lock_tuple(relation, &tuple, estate->es_output_cid,
						   lockmode, LockWaitBlock, false, &buffer,
						   &hufd);
	switch (test)
	{
		case HeapTupleMayBeUpdated:
			/* success! */
			break;

		case HeapTupleInvisible:

			/*
			 * This can occur when a just inserted tuple is updated again in
			 * the same command. E.g. because multiple rows with the same
			 * conflicting key values are inserted.
			 *
			 * This is somewhat similar to the ExecUpdate()
			 * HeapTupleSelfUpdated case.  We do not want to proceed because
			 * it would lead to the same row being updated a second time in
			 * some unspecified order, and in contrast to plain UPDATEs
			 * there's no historical behavior to break.
			 *
			 * It is the user's responsibility to prevent this situation from
			 * occurring.  These problems are why SQL Standard similarly
			 * specifies that for SQL MERGE, an exception must be raised in
			 * the event of an attempt to update the same row twice.
			 */
			if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tuple.t_data)))
				ereport(ERROR,
						(errcode(ERRCODE_CARDINALITY_VIOLATION),
						 errmsg("ON CONFLICT DO UPDATE command cannot affect row a second time"),
						 errhint("Ensure that no rows proposed for insertion within the same command have duplicate constrained values.")));

			/* This shouldn't happen */
			elog(ERROR, "attempted to lock invisible tuple");

		case HeapTupleSelfUpdated:

			/*
			 * This state should never be reached. As a dirty snapshot is used
			 * to find conflicting tuples, speculative insertion wouldn't have
			 * seen this row to conflict with.
			 */
			elog(ERROR, "unexpected self-updated tuple");

		case HeapTupleUpdated:
			if (IsolationUsesXactSnapshot())
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("could not serialize access due to concurrent update")));

			/*
			 * Tell caller to try again from the very start.
			 *
			 * It does not make sense to use the usual EvalPlanQual() style
			 * loop here, as the new version of the row might not conflict
			 * anymore, or the conflicting tuple has actually been deleted.
			 */
			ReleaseBuffer(buffer);
			return false;

		default:
			elog(ERROR, "unrecognized heap_lock_tuple status: %u", test);
	}

	/*
	 * Success, the tuple is locked.
	 *
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous cycle.
	 */
	ResetExprContext(econtext);

	/*
	 * Verify that the tuple is visible to our MVCC snapshot if the current
	 * isolation level mandates that.
	 *
	 * It's not sufficient to rely on the check within ExecUpdate() as e.g.
	 * CONFLICT ... WHERE clause may prevent us from reaching that.
	 *
	 * This means we only ever continue when a new command in the current
	 * transaction could see the row, even though in READ COMMITTED mode the
	 * tuple will not be visible according to the current statement's
	 * snapshot.  This is in line with the way UPDATE deals with newer tuple
	 * versions.
	 */
	ExecCheckHeapTupleVisible(estate, &tuple, buffer);

	/* Store target's existing tuple in the state's dedicated slot */
	ExecStoreTuple(&tuple, mtstate->mt_existing, buffer, false);

	/*
	 * Make tuple and any needed join variables available to ExecQual and
	 * ExecProject.  The EXCLUDED tuple is installed in ecxt_innertuple, while
	 * the target's existing tuple is installed in the scantuple.  EXCLUDED
	 * has been made to reference INNER_VAR in setrefs.c, but there is no
	 * other redirection.
	 */
	econtext->ecxt_scantuple = mtstate->mt_existing;
	econtext->ecxt_innertuple = excludedSlot;
	econtext->ecxt_outertuple = NULL;

	if (!ExecQual(onConflictSetWhere, econtext))
	{
		ReleaseBuffer(buffer);
		InstrCountFiltered1(&mtstate->ps, 1);
		return true;			/* done with the tuple */
	}

	if (resultRelInfo->ri_WithCheckOptions != NIL)
	{
		/*
		 * Check target's existing tuple against UPDATE-applicable USING
		 * security barrier quals (if any), enforced here as RLS checks/WCOs.
		 *
		 * The rewriter creates UPDATE RLS checks/WCOs for UPDATE security
		 * quals, and stores them as WCOs of "kind" WCO_RLS_CONFLICT_CHECK,
		 * but that's almost the extent of its special handling for ON
		 * CONFLICT DO UPDATE.
		 *
		 * The rewriter will also have associated UPDATE applicable straight
		 * RLS checks/WCOs for the benefit of the ExecUpdate() call that
		 * follows.  INSERTs and UPDATEs naturally have mutually exclusive WCO
		 * kinds, so there is no danger of spurious over-enforcement in the
		 * INSERT or UPDATE path.
		 */
		ExecWithCheckOptions(WCO_RLS_CONFLICT_CHECK, resultRelInfo,
							 mtstate->mt_existing,
							 mtstate->ps.state);
	}

	/* Project the new tuple version */
	ExecProject(resultRelInfo->ri_onConflict->oc_ProjInfo);

	/*
	 * Note that it is possible that the target tuple has been modified in
	 * this session, after the above heap_lock_tuple. We choose to not error
	 * out in that case, in line with ExecUpdate's treatment of similar cases.
	 * This can happen if an UPDATE is triggered from within ExecQual(),
	 * ExecWithCheckOptions() or ExecProject() above, e.g. by selecting from a
	 * wCTE in the ON CONFLICT's SET.
	 */

	/* Execute UPDATE with projection */
	*returning = ExecUpdate(mtstate, &tuple.t_self, NULL,
							mtstate->mt_conflproj, planSlot,
							&mtstate->mt_epqstate, mtstate->ps.state,
							NULL, NULL, NULL, canSetTag);

	ReleaseBuffer(buffer);
	return true;
}


/*
 * Process BEFORE EACH STATEMENT triggers
 */
static void
fireBSTriggers(ModifyTableState *node)
{
	ModifyTable *plan = (ModifyTable *) node->ps.plan;
	ResultRelInfo *resultRelInfo = node->resultRelInfo;

	/*
	 * If the node modifies a partitioned table, we must fire its triggers.
	 * Note that in that case, node->resultRelInfo points to the first leaf
	 * partition, not the root table.
	 */
	if (node->rootResultRelInfo != NULL)
		resultRelInfo = node->rootResultRelInfo;

	switch (node->operation)
	{
		case CMD_INSERT:
			ExecBSInsertTriggers(node->ps.state, resultRelInfo);
			if (plan->onConflictAction == ONCONFLICT_UPDATE)
				ExecBSUpdateTriggers(node->ps.state,
									 resultRelInfo);
			break;
		case CMD_UPDATE:
			ExecBSUpdateTriggers(node->ps.state, resultRelInfo);
			break;
		case CMD_DELETE:
			ExecBSDeleteTriggers(node->ps.state, resultRelInfo);
			break;
		case CMD_MERGE:
			if (node->mt_merge_subcommands & MERGE_INSERT)
				ExecBSInsertTriggers(node->ps.state, resultRelInfo);
			if (node->mt_merge_subcommands & MERGE_UPDATE)
				ExecBSUpdateTriggers(node->ps.state, resultRelInfo);
			if (node->mt_merge_subcommands & MERGE_DELETE)
				ExecBSDeleteTriggers(node->ps.state, resultRelInfo);
			break;
		default:
			elog(ERROR, "unknown operation");
			break;
	}
}

/*
 * Return the target rel ResultRelInfo.
 *
 * This relation is the same as :
 * - the relation for which we will fire AFTER STATEMENT triggers.
 * - the relation into whose tuple format all captured transition tuples must
 *   be converted.
 * - the root partitioned table.
 */
static ResultRelInfo *
getTargetResultRelInfo(ModifyTableState *node)
{
	/*
	 * Note that if the node modifies a partitioned table, node->resultRelInfo
	 * points to the first leaf partition, not the root table.
	 */
	if (node->rootResultRelInfo != NULL)
		return node->rootResultRelInfo;
	else
		return node->resultRelInfo;
}

/*
 * Process AFTER EACH STATEMENT triggers
 */
static void
fireASTriggers(ModifyTableState *node)
{
	ModifyTable *plan = (ModifyTable *) node->ps.plan;
	ResultRelInfo *resultRelInfo = getTargetResultRelInfo(node);

	switch (node->operation)
	{
		case CMD_INSERT:
			if (plan->onConflictAction == ONCONFLICT_UPDATE)
				ExecASUpdateTriggers(node->ps.state,
									 resultRelInfo,
									 node->mt_oc_transition_capture);
			ExecASInsertTriggers(node->ps.state, resultRelInfo,
								 node->mt_transition_capture);
			break;
		case CMD_UPDATE:
			ExecASUpdateTriggers(node->ps.state, resultRelInfo,
								 node->mt_transition_capture);
			break;
		case CMD_DELETE:
			ExecASDeleteTriggers(node->ps.state, resultRelInfo,
								 node->mt_transition_capture);
			break;
		case CMD_MERGE:
			if (node->mt_merge_subcommands & MERGE_DELETE)
				ExecASDeleteTriggers(node->ps.state, resultRelInfo,
									 node->mt_transition_capture);
			if (node->mt_merge_subcommands & MERGE_UPDATE)
				ExecASUpdateTriggers(node->ps.state, resultRelInfo,
									 node->mt_transition_capture);
			if (node->mt_merge_subcommands & MERGE_INSERT)
				ExecASInsertTriggers(node->ps.state, resultRelInfo,
									 node->mt_transition_capture);
			break;
		default:
			elog(ERROR, "unknown operation");
			break;
	}
}

/*
 * Set up the state needed for collecting transition tuples for AFTER
 * triggers.
 */
static void
ExecSetupTransitionCaptureState(ModifyTableState *mtstate, EState *estate)
{
	ModifyTable *plan = (ModifyTable *) mtstate->ps.plan;
	ResultRelInfo *targetRelInfo = getTargetResultRelInfo(mtstate);

	/* Check for transition tables on the directly targeted relation. */
	mtstate->mt_transition_capture =
		MakeTransitionCaptureState(targetRelInfo->ri_TrigDesc,
								   RelationGetRelid(targetRelInfo->ri_RelationDesc),
								   mtstate->operation);
	if (plan->operation == CMD_INSERT &&
		plan->onConflictAction == ONCONFLICT_UPDATE)
		mtstate->mt_oc_transition_capture =
			MakeTransitionCaptureState(targetRelInfo->ri_TrigDesc,
									   RelationGetRelid(targetRelInfo->ri_RelationDesc),
									   CMD_UPDATE);

	/*
	 * If we found that we need to collect transition tuples then we may also
	 * need tuple conversion maps for any children that have TupleDescs that
	 * aren't compatible with the tuplestores.  (We can share these maps
	 * between the regular and ON CONFLICT cases.)
	 */
	if (mtstate->mt_transition_capture != NULL ||
		mtstate->mt_oc_transition_capture != NULL)
	{
		ExecSetupChildParentMapForTcs(mtstate);

		/*
		 * Install the conversion map for the first plan for UPDATE and DELETE
		 * operations.  It will be advanced each time we switch to the next
		 * plan.  (INSERT operations set it every time, so we need not update
		 * mtstate->mt_oc_transition_capture here.)
		 */
		if (mtstate->mt_transition_capture && mtstate->operation != CMD_INSERT)
			mtstate->mt_transition_capture->tcs_map =
				tupconv_map_for_subplan(mtstate, 0);
	}
}

/*
 * ExecPrepareTupleRouting --- prepare for routing one tuple
 *
 * Determine the partition in which the tuple in slot is to be inserted,
 * and modify mtstate and estate to prepare for it.
 *
 * Caller must revert the estate changes after executing the insertion!
 * In mtstate, transition capture changes may also need to be reverted.
 *
 * Returns a slot holding the tuple of the partition rowtype.
 */
TupleTableSlot *
ExecPrepareTupleRouting(ModifyTableState *mtstate,
						EState *estate,
						PartitionTupleRouting *proute,
						ResultRelInfo *targetRelInfo,
						TupleTableSlot *slot)
{
	ModifyTable *node;
	int			partidx;
	ResultRelInfo *partrel;
	HeapTuple	tuple;

	/*
	 * Determine the target partition.  If ExecFindPartition does not find
	 * a partition after all, it doesn't return here; otherwise, the returned
	 * value is to be used as an index into the arrays for the ResultRelInfo
	 * and TupleConversionMap for the partition.
	 */
	partidx = ExecFindPartition(targetRelInfo,
								proute->partition_dispatch_info,
								slot,
								estate);
	Assert(partidx >= 0 && partidx < proute->num_partitions);

	/*
	 * Get the ResultRelInfo corresponding to the selected partition; if not
	 * yet there, initialize it.
	 */
	partrel = proute->partitions[partidx];
	if (partrel == NULL)
		partrel = ExecInitPartitionInfo(mtstate, targetRelInfo,
										proute, estate,
										partidx);

	/* We do not yet have a way to insert into a foreign partition */
	if (partrel->ri_FdwRoutine)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot route inserted tuples to a foreign table")));

	/*
	 * Make it look like we are inserting into the partition.
	 */
	estate->es_result_relation_info = partrel;

	/* Get the heap tuple out of the given slot. */
	tuple = ExecMaterializeSlot(slot);

	/*
	 * If we're capturing transition tuples, we might need to convert from the
	 * partition rowtype to parent rowtype.
	 */
	if (mtstate->mt_transition_capture != NULL)
	{
		if (partrel->ri_TrigDesc &&
			partrel->ri_TrigDesc->trig_insert_before_row)
		{
			/*
			 * If there are any BEFORE triggers on the partition, we'll have
			 * to be ready to convert their result back to tuplestore format.
			 */
			mtstate->mt_transition_capture->tcs_original_insert_tuple = NULL;
			mtstate->mt_transition_capture->tcs_map =
				TupConvMapForLeaf(proute, targetRelInfo, partidx);
		}
		else
		{
			/*
			 * Otherwise, just remember the original unconverted tuple, to
			 * avoid a needless round trip conversion.
			 */
			mtstate->mt_transition_capture->tcs_original_insert_tuple = tuple;
			mtstate->mt_transition_capture->tcs_map = NULL;
		}
	}
	if (mtstate->mt_oc_transition_capture != NULL)
	{
		mtstate->mt_oc_transition_capture->tcs_map =
			TupConvMapForLeaf(proute, targetRelInfo, partidx);
	}

	/*
	 * Convert the tuple, if necessary.
	 */
	ConvertPartitionTupleSlot(proute->parent_child_tupconv_maps[partidx],
							  tuple,
							  proute->partition_tuple_slot,
							  &slot);

	/* Initialize information needed to handle ON CONFLICT DO UPDATE. */
	Assert(mtstate != NULL);
	node = (ModifyTable *) mtstate->ps.plan;
	if (node->onConflictAction == ONCONFLICT_UPDATE)
	{
		Assert(mtstate->mt_existing != NULL);
		ExecSetSlotDescriptor(mtstate->mt_existing,
							  RelationGetDescr(partrel->ri_RelationDesc));
		Assert(mtstate->mt_conflproj != NULL);
		ExecSetSlotDescriptor(mtstate->mt_conflproj,
							  partrel->ri_onConflict->oc_ProjTupdesc);
	}

	return slot;
}

/*
 * Initialize the child-to-root tuple conversion map array for UPDATE subplans.
 *
 * This map array is required to convert the tuple from the subplan result rel
 * to the target table descriptor. This requirement arises for two independent
 * scenarios:
 * 1. For update-tuple-routing.
 * 2. For capturing tuples in transition tables.
 */
static void
ExecSetupChildParentMapForSubplan(ModifyTableState *mtstate)
{
	ResultRelInfo *targetRelInfo = getTargetResultRelInfo(mtstate);
	ResultRelInfo *resultRelInfos = mtstate->resultRelInfo;
	TupleDesc	outdesc;
	int			numResultRelInfos = mtstate->mt_nplans;
	int			i;

	/*
	 * First check if there is already a per-subplan array allocated. Even if
	 * there is already a per-leaf map array, we won't require a per-subplan
	 * one, since we will use the subplan offset array to convert the subplan
	 * index to per-leaf index.
	 */
	if (mtstate->mt_per_subplan_tupconv_maps ||
		(mtstate->mt_partition_tuple_routing &&
		 mtstate->mt_partition_tuple_routing->child_parent_tupconv_maps))
		return;

	/*
	 * Build array of conversion maps from each child's TupleDesc to the one
	 * used in the target relation.  The map pointers may be NULL when no
	 * conversion is necessary, which is hopefully a common case.
	 */

	/* Get tuple descriptor of the target rel. */
	outdesc = RelationGetDescr(targetRelInfo->ri_RelationDesc);

	mtstate->mt_per_subplan_tupconv_maps = (TupleConversionMap **)
		palloc(sizeof(TupleConversionMap *) * numResultRelInfos);

	for (i = 0; i < numResultRelInfos; ++i)
	{
		mtstate->mt_per_subplan_tupconv_maps[i] =
			convert_tuples_by_name(RelationGetDescr(resultRelInfos[i].ri_RelationDesc),
								   outdesc,
								   gettext_noop("could not convert row type"));
	}
}

/*
 * Initialize the child-to-root tuple conversion map array required for
 * capturing transition tuples.
 *
 * The map array can be indexed either by subplan index or by leaf-partition
 * index.  For transition tables, we need a subplan-indexed access to the map,
 * and where tuple-routing is present, we also require a leaf-indexed access.
 */
static void
ExecSetupChildParentMapForTcs(ModifyTableState *mtstate)
{
	PartitionTupleRouting *proute = mtstate->mt_partition_tuple_routing;

	/*
	 * If partition tuple routing is set up, we will require partition-indexed
	 * access. In that case, create the map array indexed by partition; we
	 * will still be able to access the maps using a subplan index by
	 * converting the subplan index to a partition index using
	 * subplan_partition_offsets. If tuple routing is not set up, it means we
	 * don't require partition-indexed access. In that case, create just a
	 * subplan-indexed map.
	 */
	if (proute)
	{
		/*
		 * If a partition-indexed map array is to be created, the subplan map
		 * array has to be NULL.  If the subplan map array is already created,
		 * we won't be able to access the map using a partition index.
		 */
		Assert(mtstate->mt_per_subplan_tupconv_maps == NULL);

		ExecSetupChildParentMapForLeaf(proute);
	}
	else
		ExecSetupChildParentMapForSubplan(mtstate);
}

/*
 * For a given subplan index, get the tuple conversion map.
 */
static TupleConversionMap *
tupconv_map_for_subplan(ModifyTableState *mtstate, int whichplan)
{
	/*
	 * If a partition-index tuple conversion map array is allocated, we need
	 * to first get the index into the partition array. Exactly *one* of the
	 * two arrays is allocated. This is because if there is a partition array
	 * required, we don't require subplan-indexed array since we can translate
	 * subplan index into partition index. And, we create a subplan-indexed
	 * array *only* if partition-indexed array is not required.
	 */
	if (mtstate->mt_per_subplan_tupconv_maps == NULL)
	{
		int			leaf_index;
		PartitionTupleRouting *proute = mtstate->mt_partition_tuple_routing;

		/*
		 * If subplan-indexed array is NULL, things should have been arranged
		 * to convert the subplan index to partition index.
		 */
		Assert(proute && proute->subplan_partition_offsets != NULL &&
			   whichplan < proute->num_subplan_partition_offsets);

		leaf_index = proute->subplan_partition_offsets[whichplan];

		return TupConvMapForLeaf(proute, getTargetResultRelInfo(mtstate),
								 leaf_index);
	}
	else
	{
		Assert(whichplan >= 0 && whichplan < mtstate->mt_nplans);
		return mtstate->mt_per_subplan_tupconv_maps[whichplan];
	}
}

/* ----------------------------------------------------------------
 *	   ExecModifyTable
 *
 *		Perform table modifications as required, and return RETURNING results
 *		if needed.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecModifyTable(PlanState *pstate)
{
	ModifyTableState *node = castNode(ModifyTableState, pstate);
	PartitionTupleRouting *proute = node->mt_partition_tuple_routing;
	EState	   *estate = node->ps.state;
	CmdType		operation = node->operation;
	ResultRelInfo *saved_resultRelInfo;
	ResultRelInfo *resultRelInfo;
	PlanState  *subplanstate;
	JunkFilter *junkfilter;
	TupleTableSlot *slot;
	TupleTableSlot *planSlot;
	ItemPointer tupleid;
	ItemPointerData tuple_ctid;
	HeapTupleData oldtupdata;
	HeapTuple	oldtuple;

	CHECK_FOR_INTERRUPTS();

	/*
	 * This should NOT get called during EvalPlanQual; we should have passed a
	 * subplan tree to EvalPlanQual, instead.  Use a runtime test not just
	 * Assert because this condition is easy to miss in testing.  (Note:
	 * although ModifyTable should not get executed within an EvalPlanQual
	 * operation, we do have to allow it to be initialized and shut down in
	 * case it is within a CTE subplan.  Hence this test must be here, not in
	 * ExecInitModifyTable.)
	 */
	if (estate->es_epqTuple != NULL)
		elog(ERROR, "ModifyTable should not be called during EvalPlanQual");

	/*
	 * If we've already completed processing, don't try to do more.  We need
	 * this test because ExecPostprocessPlan might call us an extra time, and
	 * our subplan's nodes aren't necessarily robust against being called
	 * extra times.
	 */
	if (node->mt_done)
		return NULL;

	/*
	 * On first call, fire BEFORE STATEMENT triggers before proceeding.
	 */
	if (node->fireBSTriggers)
	{
		fireBSTriggers(node);
		node->fireBSTriggers = false;
	}

	/* Preload local variables */
	resultRelInfo = node->resultRelInfo + node->mt_whichplan;
	subplanstate = node->mt_plans[node->mt_whichplan];
	junkfilter = resultRelInfo->ri_junkFilter;

	/*
	 * es_result_relation_info must point to the currently active result
	 * relation while we are within this ModifyTable node.  Even though
	 * ModifyTable nodes can't be nested statically, they can be nested
	 * dynamically (since our subplan could include a reference to a modifying
	 * CTE).  So we have to save and restore the caller's value.
	 */
	saved_resultRelInfo = estate->es_result_relation_info;

	estate->es_result_relation_info = resultRelInfo;

	/*
	 * Fetch rows from subplan(s), and execute the required table modification
	 * for each row.
	 */
	for (;;)
	{
		/*
		 * Reset the per-output-tuple exprcontext.  This is needed because
		 * triggers expect to use that context as workspace.  It's a bit ugly
		 * to do this below the top level of the plan, however.  We might need
		 * to rethink this later.
		 */
		ResetPerTupleExprContext(estate);

		planSlot = ExecProcNode(subplanstate);

		if (TupIsNull(planSlot))
		{
			/* advance to next subplan if any */
			node->mt_whichplan++;

			if (node->mt_whichplan < node->mt_nplans)
			{
				resultRelInfo++;
				subplanstate = node->mt_plans[node->mt_whichplan];
				junkfilter = resultRelInfo->ri_junkFilter;
				estate->es_result_relation_info = resultRelInfo;
				EvalPlanQualSetPlan(&node->mt_epqstate, subplanstate->plan,
									node->mt_arowmarks[node->mt_whichplan]);
				/* Prepare to convert transition tuples from this child. */
				if (node->mt_transition_capture != NULL)
				{
					node->mt_transition_capture->tcs_map =
						tupconv_map_for_subplan(node, node->mt_whichplan);
				}
				if (node->mt_oc_transition_capture != NULL)
				{
					node->mt_oc_transition_capture->tcs_map =
						tupconv_map_for_subplan(node, node->mt_whichplan);
				}
				continue;
			}
			else
				break;
		}

		/*
		 * If resultRelInfo->ri_usesFdwDirectModify is true, all we need to do
		 * here is compute the RETURNING expressions.
		 */
		if (resultRelInfo->ri_usesFdwDirectModify)
		{
			Assert(resultRelInfo->ri_projectReturning);

			/*
			 * A scan slot containing the data that was actually inserted,
			 * updated or deleted has already been made available to
			 * ExecProcessReturning by IterateDirectModify, so no need to
			 * provide it here.
			 */
			slot = ExecProcessReturning(resultRelInfo, NULL, planSlot);

			estate->es_result_relation_info = saved_resultRelInfo;
			return slot;
		}

		EvalPlanQualSetSlot(&node->mt_epqstate, planSlot);
		slot = planSlot;

		if (operation == CMD_MERGE)
		{
			ExecMerge(node, estate, slot, junkfilter, resultRelInfo);
			continue;
		}

		tupleid = NULL;
		oldtuple = NULL;
		if (junkfilter != NULL)
		{
			/*
			 * extract the 'ctid' or 'wholerow' junk attribute.
			 */
			if (operation == CMD_UPDATE || operation == CMD_DELETE)
			{
				char		relkind;
				Datum		datum;
				bool		isNull;

				relkind = resultRelInfo->ri_RelationDesc->rd_rel->relkind;
				if (relkind == RELKIND_RELATION || relkind == RELKIND_MATVIEW)
				{
					datum = ExecGetJunkAttribute(slot,
												 junkfilter->jf_junkAttNo,
												 &isNull);
					/* shouldn't ever get a null result... */
					if (isNull)
						elog(ERROR, "ctid is NULL");

					tupleid = (ItemPointer) DatumGetPointer(datum);
					tuple_ctid = *tupleid;	/* be sure we don't free ctid!! */
					tupleid = &tuple_ctid;
				}

				/*
				 * Use the wholerow attribute, when available, to reconstruct
				 * the old relation tuple.
				 *
				 * Foreign table updates have a wholerow attribute when the
				 * relation has a row-level trigger.  Note that the wholerow
				 * attribute does not carry system columns.  Foreign table
				 * triggers miss seeing those, except that we know enough here
				 * to set t_tableOid.  Quite separately from this, the FDW may
				 * fetch its own junk attrs to identify the row.
				 *
				 * Other relevant relkinds, currently limited to views, always
				 * have a wholerow attribute.
				 */
				else if (AttributeNumberIsValid(junkfilter->jf_junkAttNo))
				{
					datum = ExecGetJunkAttribute(slot,
												 junkfilter->jf_junkAttNo,
												 &isNull);
					/* shouldn't ever get a null result... */
					if (isNull)
						elog(ERROR, "wholerow is NULL");

					oldtupdata.t_data = DatumGetHeapTupleHeader(datum);
					oldtupdata.t_len =
						HeapTupleHeaderGetDatumLength(oldtupdata.t_data);
					ItemPointerSetInvalid(&(oldtupdata.t_self));
					/* Historically, view triggers see invalid t_tableOid. */
					oldtupdata.t_tableOid =
						(relkind == RELKIND_VIEW) ? InvalidOid :
						RelationGetRelid(resultRelInfo->ri_RelationDesc);

					oldtuple = &oldtupdata;
				}
				else
					Assert(relkind == RELKIND_FOREIGN_TABLE);
			}

			/*
			 * apply the junkfilter if needed.
			 */
			if (operation != CMD_DELETE)
				slot = ExecFilterJunk(junkfilter, slot);
		}

		switch (operation)
		{
			case CMD_INSERT:
				/* Prepare for tuple routing if needed. */
				if (proute)
					slot = ExecPrepareTupleRouting(node, estate, proute,
												   resultRelInfo, slot);
				slot = ExecInsert(node, slot, planSlot,
								  estate, NULL, node->canSetTag);
				/* Revert ExecPrepareTupleRouting's state change. */
				if (proute)
					estate->es_result_relation_info = resultRelInfo;
				break;
			case CMD_UPDATE:
				slot = ExecUpdate(node, tupleid, oldtuple, slot, planSlot,
								  &node->mt_epqstate, estate,
								  NULL, NULL, NULL, node->canSetTag);
				break;
			case CMD_DELETE:
				slot = ExecDelete(node, tupleid, oldtuple, planSlot,
								  &node->mt_epqstate, estate,
								  NULL, true, NULL, NULL, node->canSetTag);
				break;
			default:
				elog(ERROR, "unknown operation");
				break;
		}

		/*
		 * If we got a RETURNING result, return it to caller.  We'll continue
		 * the work on next call.
		 */
		if (slot)
		{
			estate->es_result_relation_info = saved_resultRelInfo;
			return slot;
		}
	}

	/* Restore es_result_relation_info before exiting */
	estate->es_result_relation_info = saved_resultRelInfo;

	/*
	 * We're done, but fire AFTER STATEMENT triggers before exiting.
	 */
	fireASTriggers(node);

	node->mt_done = true;

	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecInitModifyTable
 * ----------------------------------------------------------------
 */
ModifyTableState *
ExecInitModifyTable(ModifyTable *node, EState *estate, int eflags)
{
	ModifyTableState *mtstate;
	CmdType		operation = node->operation;
	int			nplans = list_length(node->plans);
	ResultRelInfo *saved_resultRelInfo;
	ResultRelInfo *resultRelInfo;
	Plan	   *subplan;
	ListCell   *l;
	int			i;
	Relation	rel;
	bool		update_tuple_routing_needed = node->partColsUpdated;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	mtstate = makeNode(ModifyTableState);
	mtstate->ps.plan = (Plan *) node;
	mtstate->ps.state = estate;
	mtstate->ps.ExecProcNode = ExecModifyTable;

	mtstate->operation = operation;
	mtstate->canSetTag = node->canSetTag;
	mtstate->mt_done = false;

	mtstate->mt_plans = (PlanState **) palloc0(sizeof(PlanState *) * nplans);
	mtstate->resultRelInfo = estate->es_result_relations + node->resultRelIndex;

	/* If modifying a partitioned table, initialize the root table info */
	if (node->rootResultRelIndex >= 0)
		mtstate->rootResultRelInfo = estate->es_root_result_relations +
			node->rootResultRelIndex;

	mtstate->mt_arowmarks = (List **) palloc0(sizeof(List *) * nplans);
	mtstate->mt_nplans = nplans;

	/* set up epqstate with dummy subplan data for the moment */
	EvalPlanQualInit(&mtstate->mt_epqstate, estate, NULL, NIL, node->epqParam);
	mtstate->fireBSTriggers = true;

	/*
	 * call ExecInitNode on each of the plans to be executed and save the
	 * results into the array "mt_plans".  This is also a convenient place to
	 * verify that the proposed target relations are valid and open their
	 * indexes for insertion of new index entries.  Note we *must* set
	 * estate->es_result_relation_info correctly while we initialize each
	 * sub-plan; ExecContextForcesOids depends on that!
	 */
	saved_resultRelInfo = estate->es_result_relation_info;

	resultRelInfo = mtstate->resultRelInfo;

	/*
	 * mergeTargetRelation must be set if we're running MERGE and mustn't be
	 * set if we're not.
	 */
	Assert(operation != CMD_MERGE || node->mergeTargetRelation > 0);
	Assert(operation == CMD_MERGE || node->mergeTargetRelation == 0);

	resultRelInfo->ri_mergeTargetRTI = node->mergeTargetRelation;

	i = 0;
	foreach(l, node->plans)
	{
		subplan = (Plan *) lfirst(l);

		/* Initialize the usesFdwDirectModify flag */
		resultRelInfo->ri_usesFdwDirectModify = bms_is_member(i,
															  node->fdwDirectModifyPlans);

		/*
		 * Verify result relation is a valid target for the current operation
		 */
		CheckValidResultRel(resultRelInfo, operation);

		/*
		 * If there are indices on the result relation, open them and save
		 * descriptors in the result relation info, so that we can add new
		 * index entries for the tuples we add/update.  We need not do this
		 * for a DELETE, however, since deletion doesn't affect indexes. Also,
		 * inside an EvalPlanQual operation, the indexes might be open
		 * already, since we share the resultrel state with the original
		 * query.
		 */
		if (resultRelInfo->ri_RelationDesc->rd_rel->relhasindex &&
			operation != CMD_DELETE &&
			resultRelInfo->ri_IndexRelationDescs == NULL)
			ExecOpenIndices(resultRelInfo,
							node->onConflictAction != ONCONFLICT_NONE);

		/*
		 * If this is an UPDATE and a BEFORE UPDATE trigger is present, the
		 * trigger itself might modify the partition-key values. So arrange
		 * for tuple routing.
		 */
		if (resultRelInfo->ri_TrigDesc &&
			resultRelInfo->ri_TrigDesc->trig_update_before_row &&
			operation == CMD_UPDATE)
			update_tuple_routing_needed = true;

		/* Now init the plan for this result rel */
		estate->es_result_relation_info = resultRelInfo;
		mtstate->mt_plans[i] = ExecInitNode(subplan, estate, eflags);

		/* Also let FDWs init themselves for foreign-table result rels */
		if (!resultRelInfo->ri_usesFdwDirectModify &&
			resultRelInfo->ri_FdwRoutine != NULL &&
			resultRelInfo->ri_FdwRoutine->BeginForeignModify != NULL)
		{
			List	   *fdw_private = (List *) list_nth(node->fdwPrivLists, i);

			resultRelInfo->ri_FdwRoutine->BeginForeignModify(mtstate,
															 resultRelInfo,
															 fdw_private,
															 i,
															 eflags);
		}

		resultRelInfo++;
		i++;
	}

	estate->es_result_relation_info = saved_resultRelInfo;

	/* Get the target relation */
	rel = (getTargetResultRelInfo(mtstate))->ri_RelationDesc;

	/*
	 * If it's not a partitioned table after all, UPDATE tuple routing should
	 * not be attempted.
	 */
	if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		update_tuple_routing_needed = false;

	/*
	 * Build state for tuple routing if it's an INSERT or if it's an UPDATE of
	 * partition key.
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE &&
		(operation == CMD_INSERT || operation == CMD_MERGE ||
		 update_tuple_routing_needed))
		mtstate->mt_partition_tuple_routing =
						ExecSetupPartitionTupleRouting(mtstate, rel);

	/*
	 * Build state for collecting transition tuples.  This requires having a
	 * valid trigger query context, so skip it in explain-only mode.
	 */
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		ExecSetupTransitionCaptureState(mtstate, estate);

	/*
	 * If we are doing MERGE then setup child-parent mapping. This will be
	 * required in case we end up doing a partition-key update, triggering a
	 * tuple routing.
	 */
	if (mtstate->operation == CMD_MERGE &&
		mtstate->mt_partition_tuple_routing != NULL)
		ExecSetupChildParentMapForLeaf(mtstate->mt_partition_tuple_routing);

	/*
	 * Construct mapping from each of the per-subplan partition attnos to the
	 * root attno.  This is required when during update row movement the tuple
	 * descriptor of a source partition does not match the root partitioned
	 * table descriptor.  In such a case we need to convert tuples to the root
	 * tuple descriptor, because the search for destination partition starts
	 * from the root.  Skip this setup if it's not a partition key update.
	 */
	if (update_tuple_routing_needed)
		ExecSetupChildParentMapForSubplan(mtstate);

	/*
	 * Initialize any WITH CHECK OPTION constraints if needed.
	 */
	resultRelInfo = mtstate->resultRelInfo;
	i = 0;
	foreach(l, node->withCheckOptionLists)
	{
		List	   *wcoList = (List *) lfirst(l);
		List	   *wcoExprs = NIL;
		ListCell   *ll;

		foreach(ll, wcoList)
		{
			WithCheckOption *wco = (WithCheckOption *) lfirst(ll);
			ExprState  *wcoExpr = ExecInitQual((List *) wco->qual,
											   mtstate->mt_plans[i]);

			wcoExprs = lappend(wcoExprs, wcoExpr);
		}

		resultRelInfo->ri_WithCheckOptions = wcoList;
		resultRelInfo->ri_WithCheckOptionExprs = wcoExprs;
		resultRelInfo++;
		i++;
	}

	/*
	 * Initialize RETURNING projections if needed.
	 */
	if (node->returningLists)
	{
		TupleTableSlot *slot;
		ExprContext *econtext;

		/*
		 * Initialize result tuple slot and assign its rowtype using the first
		 * RETURNING list.  We assume the rest will look the same.
		 */
		mtstate->ps.plan->targetlist = (List *) linitial(node->returningLists);

		/* Set up a slot for the output of the RETURNING projection(s) */
		ExecInitResultTupleSlotTL(estate, &mtstate->ps);
		slot = mtstate->ps.ps_ResultTupleSlot;

		/* Need an econtext too */
		if (mtstate->ps.ps_ExprContext == NULL)
			ExecAssignExprContext(estate, &mtstate->ps);
		econtext = mtstate->ps.ps_ExprContext;

		/*
		 * Build a projection for each result rel.
		 */
		resultRelInfo = mtstate->resultRelInfo;
		foreach(l, node->returningLists)
		{
			List	   *rlist = (List *) lfirst(l);

			resultRelInfo->ri_projectReturning =
				ExecBuildProjectionInfo(rlist, econtext, slot, &mtstate->ps,
										resultRelInfo->ri_RelationDesc->rd_att);
			resultRelInfo++;
		}
	}
	else
	{
		/*
		 * We still must construct a dummy result tuple type, because InitPlan
		 * expects one (maybe should change that?).
		 */
		mtstate->ps.plan->targetlist = NIL;
		ExecInitResultTupleSlotTL(estate, &mtstate->ps);

		mtstate->ps.ps_ExprContext = NULL;
	}

	/* Set the list of arbiter indexes if needed for ON CONFLICT */
	resultRelInfo = mtstate->resultRelInfo;
	if (node->onConflictAction != ONCONFLICT_NONE)
		resultRelInfo->ri_onConflictArbiterIndexes = node->arbiterIndexes;

	/*
	 * If needed, Initialize target list, projection and qual for ON CONFLICT
	 * DO UPDATE.
	 */
	if (node->onConflictAction == ONCONFLICT_UPDATE)
	{
		ExprContext *econtext;
		TupleDesc	relationDesc;
		TupleDesc	tupDesc;

		/* insert may only have one plan, inheritance is not expanded */
		Assert(nplans == 1);

		/* already exists if created by RETURNING processing above */
		if (mtstate->ps.ps_ExprContext == NULL)
			ExecAssignExprContext(estate, &mtstate->ps);

		econtext = mtstate->ps.ps_ExprContext;
		relationDesc = resultRelInfo->ri_RelationDesc->rd_att;

		/*
		 * Initialize slot for the existing tuple.  If we'll be performing
		 * tuple routing, the tuple descriptor to use for this will be
		 * determined based on which relation the update is actually applied
		 * to, so we don't set its tuple descriptor here.
		 */
		mtstate->mt_existing =
			ExecInitExtraTupleSlot(mtstate->ps.state,
								   mtstate->mt_partition_tuple_routing ?
								   NULL : relationDesc);

		/* carried forward solely for the benefit of explain */
		mtstate->mt_excludedtlist = node->exclRelTlist;

		/* create state for DO UPDATE SET operation */
		resultRelInfo->ri_onConflict = makeNode(OnConflictSetState);

		/*
		 * Create the tuple slot for the UPDATE SET projection.
		 *
		 * Just like mt_existing above, we leave it without a tuple descriptor
		 * in the case of partitioning tuple routing, so that it can be
		 * changed by ExecPrepareTupleRouting.  In that case, we still save
		 * the tupdesc in the parent's state: it can be reused by partitions
		 * with an identical descriptor to the parent.
		 */
		tupDesc = ExecTypeFromTL((List *) node->onConflictSet,
								 relationDesc->tdhasoid);
		mtstate->mt_conflproj =
			ExecInitExtraTupleSlot(mtstate->ps.state,
								   mtstate->mt_partition_tuple_routing ?
								   NULL : tupDesc);
		resultRelInfo->ri_onConflict->oc_ProjTupdesc = tupDesc;

		/* build UPDATE SET projection state */
		resultRelInfo->ri_onConflict->oc_ProjInfo =
			ExecBuildProjectionInfo(node->onConflictSet, econtext,
									mtstate->mt_conflproj, &mtstate->ps,
									relationDesc);

		/* initialize state to evaluate the WHERE clause, if any */
		if (node->onConflictWhere)
		{
			ExprState  *qualexpr;

			qualexpr = ExecInitQual((List *) node->onConflictWhere,
									&mtstate->ps);
			resultRelInfo->ri_onConflict->oc_WhereClause = qualexpr;
		}
	}

	/*
	 * If we have any secondary relations in an UPDATE or DELETE, they need to
	 * be treated like non-locked relations in SELECT FOR UPDATE, ie, the
	 * EvalPlanQual mechanism needs to be told about them.  Locate the
	 * relevant ExecRowMarks.
	 */
	foreach(l, node->rowMarks)
	{
		PlanRowMark *rc = lfirst_node(PlanRowMark, l);
		ExecRowMark *erm;

		/* ignore "parent" rowmarks; they are irrelevant at runtime */
		if (rc->isParent)
			continue;

		/* find ExecRowMark (same for all subplans) */
		erm = ExecFindRowMark(estate, rc->rti, false);

		/* build ExecAuxRowMark for each subplan */
		for (i = 0; i < nplans; i++)
		{
			ExecAuxRowMark *aerm;

			subplan = mtstate->mt_plans[i]->plan;
			aerm = ExecBuildAuxRowMark(erm, subplan->targetlist);
			mtstate->mt_arowmarks[i] = lappend(mtstate->mt_arowmarks[i], aerm);
		}
	}

	resultRelInfo = mtstate->resultRelInfo;

	if (node->mergeActionList)
	{
		ListCell   *l;
		ExprContext *econtext;
		List	   *mergeMatchedActionStates = NIL;
		List	   *mergeNotMatchedActionStates = NIL;
		TupleDesc	relationDesc = resultRelInfo->ri_RelationDesc->rd_att;

		mtstate->mt_merge_subcommands = 0;

		if (mtstate->ps.ps_ExprContext == NULL)
			ExecAssignExprContext(estate, &mtstate->ps);

		econtext = mtstate->ps.ps_ExprContext;

		/* initialize slot for the existing tuple */
		Assert(mtstate->mt_existing == NULL);
		mtstate->mt_existing =
			ExecInitExtraTupleSlot(mtstate->ps.state,
								   mtstate->mt_partition_tuple_routing ?
								   NULL : relationDesc);

		/* initialize slot for merge actions */
		Assert(mtstate->mt_mergeproj == NULL);
		mtstate->mt_mergeproj =
			ExecInitExtraTupleSlot(mtstate->ps.state,
								   mtstate->mt_partition_tuple_routing ?
								   NULL : relationDesc);

		/*
		 * Create a MergeActionState for each action on the mergeActionList
		 * and add it to either a list of matched actions or not-matched
		 * actions.
		 */
		foreach(l, node->mergeActionList)
		{
			MergeAction *action = (MergeAction *) lfirst(l);
			MergeActionState *action_state = makeNode(MergeActionState);
			TupleDesc	tupDesc;

			action_state->matched = action->matched;
			action_state->commandType = action->commandType;
			action_state->whenqual = ExecInitQual((List *) action->qual,
					&mtstate->ps);

			/* create target slot for this action's projection */
			tupDesc = ExecTypeFromTL((List *) action->targetList,
					resultRelInfo->ri_RelationDesc->rd_rel->relhasoids);
			action_state->tupDesc = tupDesc;

			/* build action projection state */
			action_state->proj =
				ExecBuildProjectionInfo(action->targetList, econtext,
						mtstate->mt_mergeproj, &mtstate->ps,
						resultRelInfo->ri_RelationDesc->rd_att);

			/*
			 * We create two lists - one for WHEN MATCHED actions and one
			 * for WHEN NOT MATCHED actions - and stick the
			 * MergeActionState into the appropriate list.
			 */
			if (action_state->matched)
				mergeMatchedActionStates =
					lappend(mergeMatchedActionStates, action_state);
			else
				mergeNotMatchedActionStates =
					lappend(mergeNotMatchedActionStates, action_state);

			switch (action->commandType)
			{
				case CMD_INSERT:
					ExecCheckPlanOutput(resultRelInfo->ri_RelationDesc,
										action->targetList);
					mtstate->mt_merge_subcommands |= MERGE_INSERT;
					break;
				case CMD_UPDATE:
					ExecCheckPlanOutput(resultRelInfo->ri_RelationDesc,
										action->targetList);
					mtstate->mt_merge_subcommands |= MERGE_UPDATE;
					break;
				case CMD_DELETE:
					mtstate->mt_merge_subcommands |= MERGE_DELETE;
					break;
				case CMD_NOTHING:
					break;
				default:
					elog(ERROR, "unknown operation");
					break;
			}

			resultRelInfo->ri_mergeState->matchedActionStates =
						mergeMatchedActionStates;
			resultRelInfo->ri_mergeState->notMatchedActionStates =
						mergeNotMatchedActionStates;

		}
	}

	/* select first subplan */
	mtstate->mt_whichplan = 0;
	subplan = (Plan *) linitial(node->plans);
	EvalPlanQualSetPlan(&mtstate->mt_epqstate, subplan,
						mtstate->mt_arowmarks[0]);

	/*
	 * Initialize the junk filter(s) if needed.  INSERT queries need a filter
	 * if there are any junk attrs in the tlist.  UPDATE and DELETE always
	 * need a filter, since there's always at least one junk attribute present
	 * --- no need to look first.  Typically, this will be a 'ctid' or
	 * 'wholerow' attribute, but in the case of a foreign data wrapper it
	 * might be a set of junk attributes sufficient to identify the remote
	 * row. We follow this logic for MERGE, so it always has a junk attributes.
	 *
	 * If there are multiple result relations, each one needs its own junk
	 * filter.  Note multiple rels are only possible for UPDATE/DELETE, so we
	 * can't be fooled by some needing a filter and some not.
	 *
	 * This section of code is also a convenient place to verify that the
	 * output of an INSERT or UPDATE matches the target table(s).
	 */
	{
		bool		junk_filter_needed = false;

		switch (operation)
		{
			case CMD_INSERT:
				foreach(l, subplan->targetlist)
				{
					TargetEntry *tle = (TargetEntry *) lfirst(l);

					if (tle->resjunk)
					{
						junk_filter_needed = true;
						break;
					}
				}
				break;
			case CMD_UPDATE:
			case CMD_DELETE:
			case CMD_MERGE:
				junk_filter_needed = true;
				break;
			default:
				elog(ERROR, "unknown operation");
				break;
		}

		if (junk_filter_needed)
		{
			resultRelInfo = mtstate->resultRelInfo;
			for (i = 0; i < nplans; i++)
			{
				JunkFilter *j;

				subplan = mtstate->mt_plans[i]->plan;

				if (operation == CMD_INSERT || operation == CMD_UPDATE)
					ExecCheckPlanOutput(resultRelInfo->ri_RelationDesc,
										subplan->targetlist);

				j = ExecInitJunkFilter(subplan->targetlist,
									   resultRelInfo->ri_RelationDesc->rd_att->tdhasoid,
									   ExecInitExtraTupleSlot(estate, NULL));

				if (operation == CMD_UPDATE ||
					operation == CMD_DELETE ||
					operation == CMD_MERGE)
				{
					/* For UPDATE/DELETE, find the appropriate junk attr now */
					char		relkind;

					relkind = resultRelInfo->ri_RelationDesc->rd_rel->relkind;
					if (relkind == RELKIND_RELATION ||
						relkind == RELKIND_MATVIEW ||
						relkind == RELKIND_PARTITIONED_TABLE)
					{
						j->jf_junkAttNo = ExecFindJunkAttribute(j, "ctid");
						if (!AttributeNumberIsValid(j->jf_junkAttNo))
							elog(ERROR, "could not find junk ctid column");

						if (operation == CMD_MERGE &&
							relkind == RELKIND_PARTITIONED_TABLE)
						{
							j->jf_otherJunkAttNo = ExecFindJunkAttribute(j, "tableoid");
							if (!AttributeNumberIsValid(j->jf_otherJunkAttNo))
								elog(ERROR, "could not find junk tableoid column");

						}
					}
					else if (relkind == RELKIND_FOREIGN_TABLE)
					{
						/*
						 * When there is a row-level trigger, there should be
						 * a wholerow attribute.
						 */
						j->jf_junkAttNo = ExecFindJunkAttribute(j, "wholerow");
					}
					else
					{
						j->jf_junkAttNo = ExecFindJunkAttribute(j, "wholerow");
						if (!AttributeNumberIsValid(j->jf_junkAttNo))
							elog(ERROR, "could not find junk wholerow column");
					}
				}

				resultRelInfo->ri_junkFilter = j;
				resultRelInfo++;
			}
		}
		else
		{
			if (operation == CMD_INSERT)
				ExecCheckPlanOutput(mtstate->resultRelInfo->ri_RelationDesc,
									subplan->targetlist);
		}
	}

	/*
	 * Set up a tuple table slot for use for trigger output tuples. In a plan
	 * containing multiple ModifyTable nodes, all can share one such slot, so
	 * we keep it in the estate.
	 */
	if (estate->es_trig_tuple_slot == NULL)
		estate->es_trig_tuple_slot = ExecInitExtraTupleSlot(estate, NULL);

	/*
	 * Lastly, if this is not the primary (canSetTag) ModifyTable node, add it
	 * to estate->es_auxmodifytables so that it will be run to completion by
	 * ExecPostprocessPlan.  (It'd actually work fine to add the primary
	 * ModifyTable node too, but there's no need.)  Note the use of lcons not
	 * lappend: we need later-initialized ModifyTable nodes to be shut down
	 * before earlier ones.  This ensures that we don't throw away RETURNING
	 * rows that need to be seen by a later CTE subplan.
	 */
	if (!mtstate->canSetTag)
		estate->es_auxmodifytables = lcons(mtstate,
										   estate->es_auxmodifytables);

	return mtstate;
}

/* ----------------------------------------------------------------
 *		ExecEndModifyTable
 *
 *		Shuts down the plan.
 *
 *		Returns nothing of interest.
 * ----------------------------------------------------------------
 */
void
ExecEndModifyTable(ModifyTableState *node)
{
	int			i;

	/*
	 * Allow any FDWs to shut down
	 */
	for (i = 0; i < node->mt_nplans; i++)
	{
		ResultRelInfo *resultRelInfo = node->resultRelInfo + i;

		if (!resultRelInfo->ri_usesFdwDirectModify &&
			resultRelInfo->ri_FdwRoutine != NULL &&
			resultRelInfo->ri_FdwRoutine->EndForeignModify != NULL)
			resultRelInfo->ri_FdwRoutine->EndForeignModify(node->ps.state,
														   resultRelInfo);
	}

	/* Close all the partitioned tables, leaf partitions, and their indices */
	if (node->mt_partition_tuple_routing)
		ExecCleanupTupleRouting(node->mt_partition_tuple_routing);

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/*
	 * Terminate EPQ execution if active
	 */
	EvalPlanQualEnd(&node->mt_epqstate);

	/*
	 * shut down subplans
	 */
	for (i = 0; i < node->mt_nplans; i++)
		ExecEndNode(node->mt_plans[i]);
}

void
ExecReScanModifyTable(ModifyTableState *node)
{
	/*
	 * Currently, we don't need to support rescan on ModifyTable nodes. The
	 * semantics of that would be a bit debatable anyway.
	 */
	elog(ERROR, "ExecReScanModifyTable is not implemented");
}
