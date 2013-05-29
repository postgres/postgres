/*-------------------------------------------------------------------------
 *
 * nodeLockRows.c
 *	  Routines to handle FOR UPDATE/FOR SHARE row locking
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
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
	bool		epq_started;
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

	/*
	 * Attempt to lock the source tuple(s).  (Note we only have locking
	 * rowmarks in lr_arowMarks.)
	 */
	epq_started = false;
	foreach(lc, node->lr_arowMarks)
	{
		ExecAuxRowMark *aerm = (ExecAuxRowMark *) lfirst(lc);
		ExecRowMark *erm = aerm->rowmark;
		Datum		datum;
		bool		isNull;
		HeapTupleData tuple;
		Buffer		buffer;
		HeapUpdateFailureData hufd;
		LockTupleMode lockmode;
		HTSU_Result test;
		HeapTuple	copyTuple;

		/* clear any leftover test tuple for this rel */
		if (node->lr_epqstate.estate != NULL)
			EvalPlanQualSetTuple(&node->lr_epqstate, erm->rti, NULL);

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

			if (tableoid != RelationGetRelid(erm->relation))
			{
				/* this child is inactive right now */
				ItemPointerSetInvalid(&(erm->curCtid));
				continue;
			}
		}

		/* fetch the tuple's ctid */
		datum = ExecGetJunkAttribute(slot,
									 aerm->ctidAttNo,
									 &isNull);
		/* shouldn't ever get a null result... */
		if (isNull)
			elog(ERROR, "ctid is NULL");
		tuple.t_self = *((ItemPointer) DatumGetPointer(datum));

		/* okay, try to lock the tuple */
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
							   lockmode, erm->noWait, true,
							   &buffer, &hufd);
		ReleaseBuffer(buffer);
		switch (test)
		{
			case HeapTupleSelfUpdated:

				/*
				 * The target tuple was already updated or deleted by the
				 * current command, or by a later command in the current
				 * transaction.  We *must* ignore the tuple in the former
				 * case, so as to avoid the "Halloween problem" of repeated
				 * update attempts.  In the latter case it might be sensible
				 * to fetch the updated tuple instead, but doing so would
				 * require changing heap_lock_tuple as well as heap_update and
				 * heap_delete to not complain about updating "invisible"
				 * tuples, which seems pretty scary.  So for now, treat the
				 * tuple as deleted and do not process.
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
				copyTuple = EvalPlanQualFetch(estate, erm->relation, lockmode,
											  &hufd.ctid, hufd.xmax);

				if (copyTuple == NULL)
				{
					/* Tuple was deleted, so don't return it */
					goto lnext;
				}
				/* remember the actually locked tuple's TID */
				tuple.t_self = copyTuple->t_self;

				/*
				 * Need to run a recheck subquery.	Initialize EPQ state if we
				 * didn't do so already.
				 */
				if (!epq_started)
				{
					EvalPlanQualBegin(&node->lr_epqstate, estate);
					epq_started = true;
				}

				/* Store target tuple for relation's scan node */
				EvalPlanQualSetTuple(&node->lr_epqstate, erm->rti, copyTuple);

				/* Continue loop until we have all target tuples */
				break;

			default:
				elog(ERROR, "unrecognized heap_lock_tuple status: %u",
					 test);
		}

		/* Remember locked tuple's TID for WHERE CURRENT OF */
		erm->curCtid = tuple.t_self;
	}

	/*
	 * If we need to do EvalPlanQual testing, do so.
	 */
	if (epq_started)
	{
		/*
		 * First, fetch a copy of any rows that were successfully locked
		 * without any update having occurred.	(We do this in a separate pass
		 * so as to avoid overhead in the common case where there are no
		 * concurrent updates.)
		 */
		foreach(lc, node->lr_arowMarks)
		{
			ExecAuxRowMark *aerm = (ExecAuxRowMark *) lfirst(lc);
			ExecRowMark *erm = aerm->rowmark;
			HeapTupleData tuple;
			Buffer		buffer;

			/* ignore non-active child tables */
			if (!ItemPointerIsValid(&(erm->curCtid)))
			{
				Assert(erm->rti != erm->prti);	/* check it's child table */
				continue;
			}

			if (EvalPlanQualGetTuple(&node->lr_epqstate, erm->rti) != NULL)
				continue;		/* it was updated and fetched above */

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
	 * Locate the ExecRowMark(s) that this node is responsible for, and
	 * construct ExecAuxRowMarks for them.	(InitPlan should already have
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

		/* find ExecRowMark and build ExecAuxRowMark */
		erm = ExecFindRowMark(estate, rc->rti);
		aerm = ExecBuildAuxRowMark(erm, outerPlan->targetlist);

		/*
		 * Only locking rowmarks go into our own list.	Non-locking marks are
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
