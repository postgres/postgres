/*-------------------------------------------------------------------------
 *
 * execScan.c
 *	  This code provides support for generalized relation scans. ExecScan
 *	  is passed a node and a pointer to a function to "do the right thing"
 *	  and return a tuple from the relation. ExecScan then does the tedious
 *	  stuff - checking the qualification and projecting the tuple
 *	  appropriately.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/execScan.c,v 1.16 2001/03/22 03:59:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <sys/types.h>
#include <sys/file.h>

#include "postgres.h"

#include "executor/executor.h"
#include "utils/memutils.h"


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
		 ExecScanAccessMtd accessMtd)	/* function returning a tuple */
{
	CommonScanState *scanstate;
	EState	   *estate;
	ExprContext *econtext;
	List	   *qual;
	ExprDoneCond isDone;
	TupleTableSlot *resultSlot;

	/* ----------------
	 *	Fetch data from node
	 * ----------------
	 */
	estate = node->plan.state;
	scanstate = node->scanstate;
	econtext = scanstate->cstate.cs_ExprContext;
	qual = node->plan.qual;

	/* ----------------
	 *	Check to see if we're still projecting out tuples from a previous
	 *	scan tuple (because there is a function-returning-set in the
	 *	projection expressions).  If so, try to project another one.
	 * ----------------
	 */
	if (scanstate->cstate.cs_TupFromTlist)
	{
		resultSlot = ExecProject(scanstate->cstate.cs_ProjInfo, &isDone);
		if (isDone == ExprMultipleResult)
			return resultSlot;
		/* Done with that source tuple... */
		scanstate->cstate.cs_TupFromTlist = false;
	}

	/* ----------------
	 *	Reset per-tuple memory context to free any expression evaluation
	 *	storage allocated in the previous tuple cycle.	Note this can't
	 *	happen until we're done projecting out tuples from a scan tuple.
	 * ----------------
	 */
	ResetExprContext(econtext);

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
		 *
		 * check for non-nil qual here to avoid a function call to
		 * ExecQual() when the qual is nil ... saves only a few cycles,
		 * but they add up ...
		 * ----------------
		 */
		if (!qual || ExecQual(qual, econtext, false))
		{
			/* ----------------
			 *	Found a satisfactory scan tuple.
			 *
			 *	Form a projection tuple, store it in the result tuple
			 *	slot and return it --- unless we find we can project no
			 *	tuples from this scan tuple, in which case continue scan.
			 * ----------------
			 */
			resultSlot = ExecProject(scanstate->cstate.cs_ProjInfo, &isDone);
			if (isDone != ExprEndResult)
			{
				scanstate->cstate.cs_TupFromTlist = (isDone == ExprMultipleResult);
				return resultSlot;
			}
		}

		/* ----------------
		 *	Tuple fails qual, so free per-tuple memory and try again.
		 * ----------------
		 */
		ResetExprContext(econtext);
	}
}
