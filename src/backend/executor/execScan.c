/*-------------------------------------------------------------------------
 *
 * execScan.c--
 *    This code provides support for generalized relation scans. ExecScan
 *    is passed a node and a pointer to a function to "do the right thing"
 *    and return a tuple from the relation. ExecScan then does the tedious
 *    stuff - checking the qualification and projecting the tuple
 *    appropriately.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/executor/execScan.c,v 1.2 1996/10/31 10:11:34 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <sys/file.h>
#include "postgres.h"

#include "executor/executor.h"

/* ----------------------------------------------------------------
 *   	ExecScan
 *   
 *   	Scans the relation using the 'access method' indicated and 
 *   	returns the next qualifying tuple in the direction specified
 *   	in the global variable ExecDirection.
 *   	The access method returns the next tuple and execScan() is 
 *   	responisble for checking the tuple returned against the qual-clause.
 *   	
 *   	Conditions:
 *   	  -- the "cursor" maintained by the AMI is positioned at the tuple
 *   	     returned previously.
 *   
 *   	Initial States:
 *   	  -- the relation indicated is opened for scanning so that the 
 *   	     "cursor" is positioned before the first qualifying tuple.
 *
 *	May need to put startmmgr  and endmmgr in here.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecScan(Scan *node, 
	 TupleTableSlot* (*accessMtd)())	/* function returning a tuple */
{
    CommonScanState	*scanstate;
    EState		*estate;
    List		*qual;
    bool		isDone;

    TupleTableSlot	*slot;
    TupleTableSlot	*resultSlot;
    HeapTuple 		newTuple;

    ExprContext		*econtext;
    ProjectionInfo	*projInfo;


    /* ----------------
     *	initialize misc variables
     * ----------------
     */
    newTuple = 	NULL;
    slot =	NULL;

    estate = 	node->plan.state;
    scanstate =     	node->scanstate;

    /* ----------------
     *	get the expression context
     * ----------------
     */
    econtext = 	scanstate->cstate.cs_ExprContext;

    /* ----------------
     *	initialize fields in ExprContext which don't change
     *	in the course of the scan..
     * ----------------
     */
    qual = node->plan.qual;
    econtext->ecxt_relation = scanstate->css_currentRelation;
    econtext->ecxt_relid = node->scanrelid;

    if (scanstate->cstate.cs_TupFromTlist) {
	projInfo = scanstate->cstate.cs_ProjInfo;
	resultSlot = ExecProject(projInfo, &isDone);
	if (!isDone)
	  return resultSlot;
    }
    /* 
     * get a tuple from the access method 
     * loop until we obtain a tuple which passes the qualification.
     */
    for(;;) {
	slot = (TupleTableSlot *) (*accessMtd)(node);

	/* ----------------
	 *  if the slot returned by the accessMtd contains
	 *  NULL, then it means there is nothing more to scan
	 *  so we just return the empty slot.
	 * ----------------
	 */
	if (TupIsNull(slot)) return slot;
	
	/* ----------------
	 *   place the current tuple into the expr context
	 * ----------------
	 */
	econtext->ecxt_scantuple = slot;
	
	/* ----------------
	 *  check that the current tuple satisfies the qual-clause
	 *  if our qualification succeeds then we
	 *  leave the loop.
	 * ----------------
	 */

	/* add a check for non-nil qual here to avoid a
	   function call to ExecQual() when the qual is nil */
	if (!qual || ExecQual(qual, econtext) == true)
	    break;
    }

    /* ----------------
     *	form a projection tuple, store it in the result tuple
     *  slot and return it.
     * ----------------
     */
    projInfo = scanstate->cstate.cs_ProjInfo;

    resultSlot = ExecProject(projInfo, &isDone);
    scanstate->cstate.cs_TupFromTlist = !isDone;

    return resultSlot;
}

