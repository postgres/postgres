/*-------------------------------------------------------------------------
 *
 * execScan.c
 *	  This code provides support for generalized relation scans. ExecScan
 *	  is passed a node and a pointer to a function to "do the right thing"
 *	  and return a tuple from the relation. ExecScan then does the tedious
 *	  stuff - checking the qualification and projecting the tuple
 *	  appropriately.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execScan.c,v 1.12 2000/07/12 02:37:01 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <sys/types.h>
#include <sys/file.h>
#include "postgres.h"

#include "executor/executor.h"

/* ----------------------------------------------------------------
 *		ExecScan
 *
 *		Scans the relation using the 'access method' indicated and
 *		returns the next qualifying tuple in the direction specified
 *		in the global variable ExecDirection.
 *		The access method returns the next tuple and execScan() is
 *		responsible for checking the tuple returned against the qual-clause.
 *
 *		Conditions:
 *		  -- the "cursor" maintained by the AMI is positioned at the tuple
 *			 returned previously.
 *
 *		Initial States:
 *		  -- the relation indicated is opened for scanning so that the
 *			 "cursor" is positioned before the first qualifying tuple.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecScan(Scan *node,
		 ExecScanAccessMtd accessMtd) /* function returning a tuple */
{
	CommonScanState *scanstate;
	EState	   *estate;
	List	   *qual;
	bool		isDone;
	TupleTableSlot *resultSlot;
	ExprContext *econtext;
	ProjectionInfo *projInfo;

	/* ----------------
	 *	Fetch data from node
	 * ----------------
	 */
	estate = node->plan.state;
	scanstate = node->scanstate;
	econtext = scanstate->cstate.cs_ExprContext;
	qual = node->plan.qual;

	/* ----------------
	 *	Reset per-tuple memory context to free any expression evaluation
	 *	storage allocated in the previous tuple cycle.
	 * ----------------
	 */
	ResetExprContext(econtext);

	/* ----------------
	 *	Check to see if we're still projecting out tuples from a previous
	 *	scan tuple (because there is a function-returning-set in the
	 *	projection expressions).  If so, try to project another one.
	 * ----------------
	 */
	if (scanstate->cstate.cs_TupFromTlist)
	{
		projInfo = scanstate->cstate.cs_ProjInfo;
		resultSlot = ExecProject(projInfo, &isDone);
		if (!isDone)
			return resultSlot;
		/* Done with that source tuple... */
		scanstate->cstate.cs_TupFromTlist = false;
	}

	/*
	 * get a tuple from the access method loop until we obtain a tuple
	 * which passes the qualification.
	 */
	for (;;)
	{
		TupleTableSlot *slot;

		slot = (*accessMtd) (node);

		/* ----------------
		 *	if the slot returned by the accessMtd contains
		 *	NULL, then it means there is nothing more to scan
		 *	so we just return an empty slot, being careful to use
		 *	the projection result slot so it has correct tupleDesc.
		 * ----------------
		 */
		if (TupIsNull(slot))
		{
			return ExecStoreTuple(NULL,
								  scanstate->cstate.cs_ProjInfo->pi_slot,
								  InvalidBuffer,
								  true);
		}

		/* ----------------
		 *	 place the current tuple into the expr context
		 * ----------------
		 */
		econtext->ecxt_scantuple = slot;

		/* ----------------
		 *	check that the current tuple satisfies the qual-clause
		 *	if our qualification succeeds then we may
		 *	leave the loop.
		 *
		 * check for non-nil qual here to avoid a function call to
		 * ExecQual() when the qual is nil ... saves only a few cycles,
		 * but they add up ...
		 * ----------------
		 */
		if (!qual || ExecQual(qual, econtext, false))
			break;

		/* ----------------
		 *	Tuple fails qual, so free per-tuple memory and try again.
		 * ----------------
		 */
		ResetExprContext(econtext);
	}

	/* ----------------
	 *	Found a satisfactory scan tuple.
	 *
	 *	Form a projection tuple, store it in the result tuple
	 *	slot and return it.
	 * ----------------
	 */
	projInfo = scanstate->cstate.cs_ProjInfo;

	resultSlot = ExecProject(projInfo, &isDone);
	scanstate->cstate.cs_TupFromTlist = !isDone;

	return resultSlot;
}
