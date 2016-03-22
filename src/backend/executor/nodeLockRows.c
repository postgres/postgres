/*-------------------------------------------------------------------------
 *
 * nodeLockRows.c
 *	  Routines to handle FOR UPDATE/FOR SHARE row locking
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeLockRows.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecLockRows		- fetch locked rows
 *		ExecInitLockRows	- initialize node and subnodes..
 *		ExecEndLockRows		- shutdown node and subnodes
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "executor/nodeLockRows.h"
#include "foreign/fdwapi.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"
#include "utils/tqual.h"


/* ----------------------------------------------------------------
 *		ExecLockRows
 * ----------------------------------------------------------------
 */
TupleTableSlot *				/* return: a tuple or NULL */
ExecLockRows(LockRowsState *node)
{
	TupleTableSlot *slot;
	EState	   *estate;
	PlanState  *outerPlan;
	bool		epq_needed;
	ListCell   *lc;

	/*
	 * get information from the node
	 */
	estate = node->ps.state;
	outerPlan = outerPlanState(node);

	/*
	 * Get next tuple from subplan, if any.
	 */
lnext:
	slot = ExecProcNode(outerPlan);

	if (TupIsNull(slot))
		return NULL;

	/* We don't need EvalPlanQual unless we get updated tuple version(s) */
	epq_needed = false;

	/*
	 * Attempt to lock the source tuple(s).  (Note we only have locking
	 * rowmarks in lr_arowMarks.)
	 */
	foreach(lc, node->lr_arowMarks)
	{
		ExecAuxRowMark *aerm = (ExecAuxRowMark *) lfirst(lc);
		ExecRowMark *erm = aerm->rowmark;
		HeapTuple  *testTuple;
		Datum		datum;
		bool		isNull;
		HeapTupleData tuple;
		Buffer		buffer;
		HeapUpdateFailureData hufd;
		LockTupleMode lockmode;
		HTSU_Result test;
		HeapTuple	copyTuple;

		/* clear any leftover test tuple for this rel */
		testTuple = &(node->lr_curtuples[erm->rti - 1]);
		if (*testTuple != NULL)
			heap_freetuple(*testTuple);
		*testTuple = NULL;

		/* if child rel, must check whether it produced this row */
		if (erm->rti != erm->prti)
		{
			Oid			tableoid;

			datum = ExecGetJunkAttribute(slot,
										 aerm->toidAttNo,
										 &isNull);
			/* shouldn't ever get a null result... */
			if (isNull)
				elog(ERROR, "tableoid is NULL");
			tableoid = DatumGetObjectId(datum);

			Assert(OidIsValid(erm->relid));
			if (tableoid != erm->relid)
			{
				/* this child is inactive right now */
				erm->ermActive = false;
				ItemPointerSetInvalid(&(erm->curCtid));
				continue;
			}
		}
		erm->ermActive = true;

		/* fetch the tuple's ctid */
		datum = ExecGetJunkAttribute(slot,
									 aerm->ctidAttNo,
									 &isNull);
		/* shouldn't ever get a null result... */
		if (isNull)
			elog(ERROR, "ctid is NULL");

		/* requests for foreign tables must be passed to their FDW */
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
			copyTuple = fdwroutine->RefetchForeignRow(estate,
													  erm,
													  datum,
													  &updated);
			if (copyTuple == NULL)
			{
				/* couldn't get the lock, so skip this row */
				goto lnext;
			}

			/* save locked tuple for possible EvalPlanQual testing below */
			*testTuple = copyTuple;

			/*
			 * if FDW says tuple was updated before getting locked, we need to
			 * perform EPQ testing to see if quals are still satisfied
			 */
			if (updated)
				epq_needed = true;

			continue;
		}

		/* okay, try to lock the tuple */
		tuple.t_self = *((ItemPointer) DatumGetPointer(datum));
		switch (erm->markType)
		{
			case ROW_MARK_EXCLUSIVE:
				lockmode = LockTupleExclusive;
				break;
			case ROW_MARK_NOKEYEXCLUSIVE:
				lockmode = LockTupleNoKeyExclusive;
				break;
			case ROW_MARK_SHARE:
				lockmode = LockTupleShare;
				break;
			case ROW_MARK_KEYSHARE:
				lockmode = LockTupleKeyShare;
				break;
			default:
				elog(ERROR, "unsupported rowmark type");
				lockmode = LockTupleNoKeyExclusive;		/* keep compiler quiet */
				break;
		}

		test = heap_lock_tuple(erm->relation, &tuple,
							   estate->es_output_cid,
							   lockmode, erm->waitPolicy, true,
							   &buffer, &hufd);
		ReleaseBuffer(buffer);
		switch (test)
		{
			case HeapTupleWouldBlock:
				/* couldn't lock tuple in SKIP LOCKED mode */
				goto lnext;

			case HeapTupleSelfUpdated:

				/*
				 * The target tuple was already updated or deleted by the
				 * current command, or by a later command in the current
				 * transaction.  We *must* ignore the tuple in the former
				 * case, so as to avoid the "Halloween problem" of repeated
				 * update attempts.  In the latter case it might be sensible
				 * to fetch the updated tuple instead, but doing so would
				 * require changing heap_update and heap_delete to not
				 * complain about updating "invisible" tuples, which seems
				 * pretty scary (heap_lock_tuple will not complain, but few
				 * callers expect HeapTupleInvisible, and we're not one of
				 * them).  So for now, treat the tuple as deleted and do not
				 * process.
				 */
				goto lnext;

			case HeapTupleMayBeUpdated:
				/* got the lock successfully */
				break;

			case HeapTupleUpdated:
				if (IsolationUsesXactSnapshot())
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent update")));
				if (ItemPointerEquals(&hufd.ctid, &tuple.t_self))
				{
					/* Tuple was deleted, so don't return it */
					goto lnext;
				}

				/* updated, so fetch and lock the updated version */
				copyTuple = EvalPlanQualFetch(estate, erm->relation,
											  lockmode, erm->waitPolicy,
											  &hufd.ctid, hufd.xmax);

				if (copyTuple == NULL)
				{
					/*
					 * Tuple was deleted; or it's locked and we're under SKIP
					 * LOCKED policy, so don't return it
					 */
					goto lnext;
				}
				/* remember the actually locked tuple's TID */
				tuple.t_self = copyTuple->t_self;

				/* Save locked tuple for EvalPlanQual testing below */
				*testTuple = copyTuple;

				/* Remember we need to do EPQ testing */
				epq_needed = true;

				/* Continue loop until we have all target tuples */
				break;

			case HeapTupleInvisible:
				elog(ERROR, "attempted to lock invisible tuple");

			default:
				elog(ERROR, "unrecognized heap_lock_tuple status: %u",
					 test);
		}

		/* Remember locked tuple's TID for EPQ testing and WHERE CURRENT OF */
		erm->curCtid = tuple.t_self;
	}

	/*
	 * If we need to do EvalPlanQual testing, do so.
	 */
	if (epq_needed)
	{
		/* Initialize EPQ machinery */
		EvalPlanQualBegin(&node->lr_epqstate, estate);

		/*
		 * Transfer any already-fetched tuples into the EPQ state, and fetch a
		 * copy of any rows that were successfully locked without any update
		 * having occurred.  (We do this in a separate pass so as to avoid
		 * overhead in the common case where there are no concurrent updates.)
		 * Make sure any inactive child rels have NULL test tuples in EPQ.
		 */
		foreach(lc, node->lr_arowMarks)
		{
			ExecAuxRowMark *aerm = (ExecAuxRowMark *) lfirst(lc);
			ExecRowMark *erm = aerm->rowmark;
			HeapTupleData tuple;
			Buffer		buffer;

			/* skip non-active child tables, but clear their test tuples */
			if (!erm->ermActive)
			{
				Assert(erm->rti != erm->prti);	/* check it's child table */
				EvalPlanQualSetTuple(&node->lr_epqstate, erm->rti, NULL);
				continue;
			}

			/* was tuple updated and fetched above? */
			if (node->lr_curtuples[erm->rti - 1] != NULL)
			{
				/* yes, so set it as the EPQ test tuple for this rel */
				EvalPlanQualSetTuple(&node->lr_epqstate,
									 erm->rti,
									 node->lr_curtuples[erm->rti - 1]);
				/* freeing this tuple is now the responsibility of EPQ */
				node->lr_curtuples[erm->rti - 1] = NULL;
				continue;
			}

			/* foreign tables should have been fetched above */
			Assert(erm->relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE);
			Assert(ItemPointerIsValid(&(erm->curCtid)));

			/* okay, fetch the tuple */
			tuple.t_self = erm->curCtid;
			if (!heap_fetch(erm->relation, SnapshotAny, &tuple, &buffer,
							false, NULL))
				elog(ERROR, "failed to fetch tuple for EvalPlanQual recheck");

			/* successful, copy and store tuple */
			EvalPlanQualSetTuple(&node->lr_epqstate, erm->rti,
								 heap_copytuple(&tuple));
			ReleaseBuffer(buffer);
		}

		/*
		 * Now fetch any non-locked source rows --- the EPQ logic knows how to
		 * do that.
		 */
		EvalPlanQualSetSlot(&node->lr_epqstate, slot);
		EvalPlanQualFetchRowMarks(&node->lr_epqstate);

		/*
		 * And finally we can re-evaluate the tuple.
		 */
		slot = EvalPlanQualNext(&node->lr_epqstate);
		if (TupIsNull(slot))
		{
			/* Updated tuple fails qual, so ignore it and go on */
			goto lnext;
		}
	}

	/* Got all locks, so return the current tuple */
	return slot;
}

/* ----------------------------------------------------------------
 *		ExecInitLockRows
 *
 *		This initializes the LockRows node state structures and
 *		the node's subplan.
 * ----------------------------------------------------------------
 */
LockRowsState *
ExecInitLockRows(LockRows *node, EState *estate, int eflags)
{
	LockRowsState *lrstate;
	Plan	   *outerPlan = outerPlan(node);
	List	   *epq_arowmarks;
	ListCell   *lc;

	/* check for unsupported flags */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * create state structure
	 */
	lrstate = makeNode(LockRowsState);
	lrstate->ps.plan = (Plan *) node;
	lrstate->ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * LockRows nodes never call ExecQual or ExecProject.
	 */

	/*
	 * Tuple table initialization (XXX not actually used...)
	 */
	ExecInitResultTupleSlot(estate, &lrstate->ps);

	/*
	 * then initialize outer plan
	 */
	outerPlanState(lrstate) = ExecInitNode(outerPlan, estate, eflags);

	/*
	 * LockRows nodes do no projections, so initialize projection info for
	 * this node appropriately
	 */
	ExecAssignResultTypeFromTL(&lrstate->ps);
	lrstate->ps.ps_ProjInfo = NULL;

	/*
	 * Create workspace in which we can remember per-RTE locked tuples
	 */
	lrstate->lr_ntables = list_length(estate->es_range_table);
	lrstate->lr_curtuples = (HeapTuple *)
		palloc0(lrstate->lr_ntables * sizeof(HeapTuple));

	/*
	 * Locate the ExecRowMark(s) that this node is responsible for, and
	 * construct ExecAuxRowMarks for them.  (InitPlan should already have
	 * built the global list of ExecRowMarks.)
	 */
	lrstate->lr_arowMarks = NIL;
	epq_arowmarks = NIL;
	foreach(lc, node->rowMarks)
	{
		PlanRowMark *rc = (PlanRowMark *) lfirst(lc);
		ExecRowMark *erm;
		ExecAuxRowMark *aerm;

		Assert(IsA(rc, PlanRowMark));

		/* ignore "parent" rowmarks; they are irrelevant at runtime */
		if (rc->isParent)
			continue;

		/* safety check on size of lr_curtuples array */
		Assert(rc->rti > 0 && rc->rti <= lrstate->lr_ntables);

		/* find ExecRowMark and build ExecAuxRowMark */
		erm = ExecFindRowMark(estate, rc->rti, false);
		aerm = ExecBuildAuxRowMark(erm, outerPlan->targetlist);

		/*
		 * Only locking rowmarks go into our own list.  Non-locking marks are
		 * passed off to the EvalPlanQual machinery.  This is because we don't
		 * want to bother fetching non-locked rows unless we actually have to
		 * do an EPQ recheck.
		 */
		if (RowMarkRequiresRowShareLock(erm->markType))
			lrstate->lr_arowMarks = lappend(lrstate->lr_arowMarks, aerm);
		else
			epq_arowmarks = lappend(epq_arowmarks, aerm);
	}

	/* Now we have the info needed to set up EPQ state */
	EvalPlanQualInit(&lrstate->lr_epqstate, estate,
					 outerPlan, epq_arowmarks, node->epqParam);

	return lrstate;
}

/* ----------------------------------------------------------------
 *		ExecEndLockRows
 *
 *		This shuts down the subplan and frees resources allocated
 *		to this node.
 * ----------------------------------------------------------------
 */
void
ExecEndLockRows(LockRowsState *node)
{
	EvalPlanQualEnd(&node->lr_epqstate);
	ExecEndNode(outerPlanState(node));
}


void
ExecReScanLockRows(LockRowsState *node)
{
	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (node->ps.lefttree->chgParam == NULL)
		ExecReScan(node->ps.lefttree);
}
