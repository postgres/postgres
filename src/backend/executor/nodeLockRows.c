/*-------------------------------------------------------------------------
 *
 * nodeLockRows.c
 *	  Routines to handle FOR UPDATE/FOR SHARE row locking
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeLockRows.c,v 1.1 2009/10/12 18:10:43 tgl Exp $
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

#include "access/xact.h"
#include "executor/executor.h"
#include "executor/nodeLockRows.h"
#include "storage/bufmgr.h"


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
	bool		epq_pushed;
	ListCell   *lc;

	/*
	 * get information from the node
	 */
	estate = node->ps.state;
	outerPlan = outerPlanState(node);

	/*
	 * Get next tuple from subplan, if any; but if we are evaluating
	 * an EvalPlanQual substitution, first finish that.
	 */
lnext:
	if (node->lr_useEvalPlan)
	{
		slot = EvalPlanQualNext(estate);
		if (TupIsNull(slot))
		{
			EvalPlanQualPop(estate, outerPlan);
			node->lr_useEvalPlan = false;
			slot = ExecProcNode(outerPlan);
		}
	}
	else
		slot = ExecProcNode(outerPlan);

	if (TupIsNull(slot))
		return NULL;

	/*
	 * Attempt to lock the source tuple(s).
	 */
	epq_pushed = false;
	foreach(lc, node->lr_rowMarks)
	{
		ExecRowMark *erm = (ExecRowMark *) lfirst(lc);
		Datum		datum;
		bool		isNull;
		HeapTupleData tuple;
		Buffer		buffer;
		ItemPointerData update_ctid;
		TransactionId update_xmax;
		LockTupleMode lockmode;
		HTSU_Result test;
		HeapTuple	copyTuple;

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

		/* fetch the tuple's ctid */
		datum = ExecGetJunkAttribute(slot,
									 erm->ctidAttNo,
									 &isNull);
		/* shouldn't ever get a null result... */
		if (isNull)
			elog(ERROR, "ctid is NULL");
		tuple.t_self = *((ItemPointer) DatumGetPointer(datum));

		/* okay, try to lock the tuple */
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
				if (epq_pushed)
					EvalPlanQualPop(estate, outerPlan);
				goto lnext;

			case HeapTupleMayBeUpdated:
				/* got the lock successfully */
				break;

			case HeapTupleUpdated:
				if (IsXactIsoLevelSerializable)
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent update")));
				if (ItemPointerEquals(&update_ctid,
									  &tuple.t_self))
				{
					/* Tuple was deleted, so don't return it */
					if (epq_pushed)
						EvalPlanQualPop(estate, outerPlan);
					goto lnext;
				}

				/* updated, so look at updated version */
				copyTuple = EvalPlanQualFetch(estate, erm->rti,
											  &update_ctid, update_xmax);

				if (copyTuple == NULL)
				{
					/* Tuple was deleted, so don't return it */
					if (epq_pushed)
						EvalPlanQualPop(estate, outerPlan);
					goto lnext;
				}

				/*
				 * Need to run a recheck subquery.
				 * Find or create a PQ stack entry.
				 */
				if (!epq_pushed)
				{
					EvalPlanQualPush(estate, erm->rti, outerPlan);
					epq_pushed = true;
				}

				/* Store target tuple for relation's scan node */
				EvalPlanQualSetTuple(estate, erm->rti, copyTuple);

				/* Continue loop until we have all target tuples */
				break;

			default:
				elog(ERROR, "unrecognized heap_lock_tuple status: %u",
					 test);
		}

		/* Remember locked tuple's TID for WHERE CURRENT OF */
		erm->curCtid = tuple.t_self;
	}

	/* If we need to do EvalPlanQual testing, loop back to do that */
	if (epq_pushed)
	{
		node->lr_useEvalPlan = true;
		goto lnext;
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
	Plan	   *outerPlan;
	JunkFilter *j;
	ListCell   *lc;

	/* check for unsupported flags */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * create state structure
	 */
	lrstate = makeNode(LockRowsState);
	lrstate->ps.plan = (Plan *) node;
	lrstate->ps.state = estate;
	lrstate->lr_useEvalPlan = false;

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
	outerPlan = outerPlan(node);
	outerPlanState(lrstate) = ExecInitNode(outerPlan, estate, eflags);

	/*
	 * LockRows nodes do no projections, so initialize projection info for this
	 * node appropriately
	 */
	ExecAssignResultTypeFromTL(&lrstate->ps);
	lrstate->ps.ps_ProjInfo = NULL;

	/*
	 * Initialize a junkfilter that we'll use to extract the ctid junk
	 * attributes.  (We won't actually apply the filter to remove the
	 * junk, we just pass the rows on as-is.  This is because the
	 * junkfilter isn't smart enough to not remove junk attrs that
	 * might be needed further up.)
	 */
	j = ExecInitJunkFilter(outerPlan->targetlist, false,
						   ExecInitExtraTupleSlot(estate));
	lrstate->lr_junkFilter = j;

	/*
	 * Locate the ExecRowMark(s) that this node is responsible for.
	 * (InitPlan should already have built the global list of ExecRowMarks.)
	 */
	lrstate->lr_rowMarks = NIL;
	foreach(lc, node->rowMarks)
	{
		RowMarkClause *rc = (RowMarkClause *) lfirst(lc);
		ExecRowMark *erm = NULL;
		char		resname[32];
		ListCell   *lce;

		/* ignore "parent" rowmarks; they are irrelevant at runtime */
		if (rc->isParent)
			continue;

		foreach(lce, estate->es_rowMarks)
		{
			erm = (ExecRowMark *) lfirst(lce);
			if (erm->rti == rc->rti &&
				erm->prti == rc->prti &&
				erm->rowmarkId == rc->rowmarkId)
				break;
			erm = NULL;
		}
		if (erm == NULL)
			elog(ERROR, "failed to find ExecRowMark for RowMarkClause");
		if (AttributeNumberIsValid(erm->ctidAttNo))
			elog(ERROR, "ExecRowMark is already claimed");

		/* Locate the junk attribute columns in the subplan output */

		/* always need the ctid */
		snprintf(resname, sizeof(resname), "ctid%u", erm->rowmarkId);
		erm->ctidAttNo = ExecFindJunkAttribute(j, resname);
		if (!AttributeNumberIsValid(erm->ctidAttNo))
			elog(ERROR, "could not find junk \"%s\" column",
				 resname);
		/* if child relation, need tableoid too */
		if (erm->rti != erm->prti)
		{
			snprintf(resname, sizeof(resname), "tableoid%u", erm->rowmarkId);
			erm->toidAttNo = ExecFindJunkAttribute(j, resname);
			if (!AttributeNumberIsValid(erm->toidAttNo))
				elog(ERROR, "could not find junk \"%s\" column",
					 resname);
		}

		lrstate->lr_rowMarks = lappend(lrstate->lr_rowMarks, erm);
	}

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
	ExecEndNode(outerPlanState(node));
}


void
ExecReScanLockRows(LockRowsState *node, ExprContext *exprCtxt)
{
	node->lr_useEvalPlan = false;

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
}
