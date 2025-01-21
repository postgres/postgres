/*-------------------------------------------------------------------------
 * execScan.h
 *		Inline-able support functions for Scan nodes
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/executor/execScan.h
 *-------------------------------------------------------------------------
 */

#ifndef EXECSCAN_H
#define EXECSCAN_H

#include "miscadmin.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"

/*
 * ExecScanFetch -- check interrupts & fetch next potential tuple
 *
 * This routine substitutes a test tuple if inside an EvalPlanQual recheck.
 * Otherwise, it simply executes the access method's next-tuple routine.
 *
 * The pg_attribute_always_inline attribute allows the compiler to inline
 * this function into its caller. When EPQState is NULL, the EvalPlanQual
 * logic is completely eliminated at compile time, avoiding unnecessary
 * run-time checks and code for cases where EPQ is not required.
 */
static pg_attribute_always_inline TupleTableSlot *
ExecScanFetch(ScanState *node,
			  EPQState *epqstate,
			  ExecScanAccessMtd accessMtd,
			  ExecScanRecheckMtd recheckMtd)
{
	CHECK_FOR_INTERRUPTS();

	if (epqstate != NULL)
	{
		/*
		 * We are inside an EvalPlanQual recheck.  Return the test tuple if
		 * one is available, after rechecking any access-method-specific
		 * conditions.
		 */
		Index		scanrelid = ((Scan *) node->ps.plan)->scanrelid;

		if (scanrelid == 0)
		{
			/*
			 * This is a ForeignScan or CustomScan which has pushed down a
			 * join to the remote side.  The recheck method is responsible not
			 * only for rechecking the scan/join quals but also for storing
			 * the correct tuple in the slot.
			 */

			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			if (!(*recheckMtd) (node, slot))
				ExecClearTuple(slot);	/* would not be returned by scan */
			return slot;
		}
		else if (epqstate->relsubs_done[scanrelid - 1])
		{
			/*
			 * Return empty slot, as either there is no EPQ tuple for this rel
			 * or we already returned it.
			 */

			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			return ExecClearTuple(slot);
		}
		else if (epqstate->relsubs_slot[scanrelid - 1] != NULL)
		{
			/*
			 * Return replacement tuple provided by the EPQ caller.
			 */

			TupleTableSlot *slot = epqstate->relsubs_slot[scanrelid - 1];

			Assert(epqstate->relsubs_rowmark[scanrelid - 1] == NULL);

			/* Mark to remember that we shouldn't return it again */
			epqstate->relsubs_done[scanrelid - 1] = true;

			/* Return empty slot if we haven't got a test tuple */
			if (TupIsNull(slot))
				return NULL;

			/* Check if it meets the access-method conditions */
			if (!(*recheckMtd) (node, slot))
				return ExecClearTuple(slot);	/* would not be returned by
												 * scan */
			return slot;
		}
		else if (epqstate->relsubs_rowmark[scanrelid - 1] != NULL)
		{
			/*
			 * Fetch and return replacement tuple using a non-locking rowmark.
			 */

			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			/* Mark to remember that we shouldn't return more */
			epqstate->relsubs_done[scanrelid - 1] = true;

			if (!EvalPlanQualFetchRowMark(epqstate, scanrelid, slot))
				return NULL;

			/* Return empty slot if we haven't got a test tuple */
			if (TupIsNull(slot))
				return NULL;

			/* Check if it meets the access-method conditions */
			if (!(*recheckMtd) (node, slot))
				return ExecClearTuple(slot);	/* would not be returned by
												 * scan */
			return slot;
		}
	}

	/*
	 * Run the node-type-specific access method function to get the next tuple
	 */
	return (*accessMtd) (node);
}

/* ----------------------------------------------------------------
 * ExecScanExtended
 *		Scans the relation using the specified 'access method' and returns the
 *		next tuple.  Optionally checks the tuple against 'qual' and applies
 *		'projInfo' if provided.
 *
 * The 'recheck method' validates an arbitrary tuple of the relation against
 * conditions enforced by the access method.
 *
 * This function is an alternative to ExecScan, used when callers may omit
 * 'qual' or 'projInfo'. The pg_attribute_always_inline attribute allows the
 * compiler to eliminate non-relevant branches at compile time, avoiding
 * run-time checks in those cases.
 *
 * Conditions:
 *	-- The AMI "cursor" is positioned at the previously returned tuple.
 *
 * Initial States:
 *	-- The relation is opened for scanning, with the "cursor"
 *	positioned before the first qualifying tuple.
 * ----------------------------------------------------------------
 */
static pg_attribute_always_inline TupleTableSlot *
ExecScanExtended(ScanState *node,
				 ExecScanAccessMtd accessMtd,	/* function returning a tuple */
				 ExecScanRecheckMtd recheckMtd,
				 EPQState *epqstate,
				 ExprState *qual,
				 ProjectionInfo *projInfo)
{
	ExprContext *econtext = node->ps.ps_ExprContext;

	/* interrupt checks are in ExecScanFetch */

	/*
	 * If we have neither a qual to check nor a projection to do, just skip
	 * all the overhead and return the raw scan tuple.
	 */
	if (!qual && !projInfo)
	{
		ResetExprContext(econtext);
		return ExecScanFetch(node, epqstate, accessMtd, recheckMtd);
	}

	/*
	 * Reset per-tuple memory context to free any expression evaluation
	 * storage allocated in the previous tuple cycle.
	 */
	ResetExprContext(econtext);

	/*
	 * get a tuple from the access method.  Loop until we obtain a tuple that
	 * passes the qualification.
	 */
	for (;;)
	{
		TupleTableSlot *slot;

		slot = ExecScanFetch(node, epqstate, accessMtd, recheckMtd);

		/*
		 * if the slot returned by the accessMtd contains NULL, then it means
		 * there is nothing more to scan so we just return an empty slot,
		 * being careful to use the projection result slot so it has correct
		 * tupleDesc.
		 */
		if (TupIsNull(slot))
		{
			if (projInfo)
				return ExecClearTuple(projInfo->pi_state.resultslot);
			else
				return slot;
		}

		/*
		 * place the current tuple into the expr context
		 */
		econtext->ecxt_scantuple = slot;

		/*
		 * check that the current tuple satisfies the qual-clause
		 *
		 * check for non-null qual here to avoid a function call to ExecQual()
		 * when the qual is null ... saves only a few cycles, but they add up
		 * ...
		 */
		if (qual == NULL || ExecQual(qual, econtext))
		{
			/*
			 * Found a satisfactory scan tuple.
			 */
			if (projInfo)
			{
				/*
				 * Form a projection tuple, store it in the result tuple slot
				 * and return it.
				 */
				return ExecProject(projInfo);
			}
			else
			{
				/*
				 * Here, we aren't projecting, so just return scan tuple.
				 */
				return slot;
			}
		}
		else
			InstrCountFiltered1(node, 1);

		/*
		 * Tuple fails qual, so free per-tuple memory and try again.
		 */
		ResetExprContext(econtext);
	}
}

#endif							/* EXECSCAN_H */
