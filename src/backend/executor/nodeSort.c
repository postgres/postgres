/*-------------------------------------------------------------------------
 *
 * nodeSort.c--
 *	  Routines to handle sorting of relations.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeSort.c,v 1.12 1998/01/07 21:02:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/execdebug.h"
#include "executor/nodeSort.h"
#include "access/heapam.h"
#include "utils/palloc.h"
#include "utils/psort.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "storage/bufmgr.h"
#include "optimizer/internal.h" /* for _TEMP_RELATION_ID_ */

/* ----------------------------------------------------------------
 *		FormSortKeys(node)
 *
 *		Forms the structure containing information used to sort the relation.
 *
 *		Returns an array of ScanKeyData.
 * ----------------------------------------------------------------
 */
static ScanKey
FormSortKeys(Sort *sortnode)
{
	ScanKey		sortkeys;
	List	   *targetList;
	List	   *tl;
	int			keycount;
	Resdom	   *resdom;
	AttrNumber	resno;
	Index		reskey;
	Oid			reskeyop;

	/* ----------------
	 *	get information from the node
	 * ----------------
	 */
	targetList = sortnode->plan.targetlist;
	keycount = sortnode->keycount;

	/* ----------------
	 *	first allocate space for scan keys
	 * ----------------
	 */
	if (keycount <= 0)
		elog(ERROR, "FormSortKeys: keycount <= 0");
	sortkeys = (ScanKey) palloc(keycount * sizeof(ScanKeyData));

	/* ----------------
	 *	form each scan key from the resdom info in the target list
	 * ----------------
	 */
	foreach(tl, targetList)
	{
		TargetEntry *target = (TargetEntry *) lfirst(tl);

		resdom = target->resdom;
		resno = resdom->resno;
		reskey = resdom->reskey;
		reskeyop = resdom->reskeyop;

		if (reskey > 0)
		{
			ScanKeyEntryInitialize(&sortkeys[reskey - 1],
								   0,
								   resno,
								   (RegProcedure) DatumGetInt32(reskeyop),
								   (Datum) 0);
		}
	}

	return sortkeys;
}

/* ----------------------------------------------------------------
 *		ExecSort
 *
 * old comments
 *		Sorts tuples from the outer subtree of the node in psort,
 *		which saves the results in a temporary file or memory. After the
 *		initial call, returns a tuple from the file with each call.
 *		Assumes that heap access method is used.
 *
 *		Conditions:
 *		  -- none.
 *
 *		Initial States:
 *		  -- the outer child is prepared to return the first tuple.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecSort(Sort *node)
{
	EState	   *estate;
	SortState  *sortstate;
	Plan	   *outerNode;
	ScanDirection dir;
	int			keycount;
	ScanKey		sortkeys;
	HeapTuple	heapTuple;
	TupleTableSlot *slot;
	bool should_free;

	/* ----------------
	 *	get state info from node
	 * ----------------
	 */
	SO1_printf("ExecSort: %s\n",
			   "entering routine");

	sortstate = node->sortstate;
	estate = node->plan.state;
	dir = estate->es_direction;

	/* ----------------
	 *	the first time we call this, psort sorts this into a file.
	 *	Subsequent calls return tuples from psort.
	 * ----------------
	 */

	if (sortstate->sort_Flag == false)
	{
		SO1_printf("ExecSort: %s\n",
				   "sortstate == false -> sorting subplan");
		/* ----------------
		 *	set all relations to be scanned in the forward direction
		 *	while creating the temporary relation.
		 * ----------------
		 */
		estate->es_direction = ForwardScanDirection;

		/* ----------------
		 *	 prepare information for psort_begin()
		 * ----------------
		 */
		outerNode = outerPlan((Plan *) node);

		keycount = node->keycount;
		sortkeys = (ScanKey) sortstate->sort_Keys;
		SO1_printf("ExecSort: %s\n",
				   "calling psort_begin");

		if (!psort_begin(node,	/* this node */
						 keycount,		/* number keys */
						 sortkeys))		/* keys */
		{
			/* Psort says, there are no tuples to be sorted */
			return NULL;
		}

		/* ----------------
		 *	 restore to user specified direction
		 * ----------------
		 */
		estate->es_direction = dir;

		/* ----------------
		 *	make sure the tuple descriptor is up to date
		 * ----------------
		 */
		slot = (TupleTableSlot *) sortstate->csstate.cstate.cs_ResultTupleSlot;
		slot->ttc_tupleDescriptor = ExecGetTupType(outerNode);
		/* ----------------
		 *	finally set the sorted flag to true
		 * ----------------
		 */
		sortstate->sort_Flag = true;
		SO1_printf(stderr, "ExecSort: sorting done.\n");
	}
	else
	{
		slot = (TupleTableSlot *) sortstate->csstate.cstate.cs_ResultTupleSlot;
		/* *** get_cs_ResultTupleSlot((CommonState) sortstate); */
/*		slot =	sortstate->csstate.css_ScanTupleSlot; orig */
	}

	SO1_printf("ExecSort: %s\n",
			   "retrieving tuple from sorted relation");

	/* ----------------
	 *	at this point we grab a tuple from psort
	 * ----------------
	 */
	heapTuple = psort_grabtuple(node, &should_free);

	return (ExecStoreTuple(heapTuple, slot, InvalidBuffer, should_free));
}

/* ----------------------------------------------------------------
 *		ExecInitSort
 *
 * old comments
 *		Creates the run-time state information for the sort node
 *		produced by the planner and initailizes its outer subtree.
 * ----------------------------------------------------------------
 */
bool
ExecInitSort(Sort *node, EState *estate, Plan *parent)
{
	SortState  *sortstate;
	Plan	   *outerPlan;
	ScanKey		sortkeys;

	SO1_printf("ExecInitSort: %s\n",
			   "initializing sort node");

	/* ----------------
	 *	assign the node's execution state
	 * ----------------
	 */
	node->plan.state = estate;

	/* ----------------
	 * create state structure
	 * ----------------
	 */
	sortstate = makeNode(SortState);
	sortstate->sort_Flag = 0;
	sortstate->sort_Keys = NULL;
	node->cleaned = FALSE;

	node->sortstate = sortstate;

	/* ----------------
	 *	Miscellanious initialization
	 *
	 *		 +	assign node's base_id
	 *		 +	assign debugging hooks
	 *
	 *	Sort nodes don't initialize their ExprContexts because
	 *	they never call ExecQual or ExecTargetList.
	 * ----------------
	 */
	ExecAssignNodeBaseInfo(estate, &sortstate->csstate.cstate, parent);

#define SORT_NSLOTS 1
	/* ----------------
	 *	tuple table initialization
	 *
	 *	sort nodes only return scan tuples from their sorted
	 *	relation.
	 * ----------------
	 */
	ExecInitResultTupleSlot(estate, &sortstate->csstate.cstate);
	ExecInitScanTupleSlot(estate, &sortstate->csstate);

	/* ----------------
	 * initializes child nodes
	 * ----------------
	 */
	outerPlan = outerPlan((Plan *) node);
	ExecInitNode(outerPlan, estate, (Plan *) node);

	/* ----------------
	 *	initialize sortstate information
	 * ----------------
	 */
	sortkeys = FormSortKeys(node);
	sortstate->sort_Keys = sortkeys;
	sortstate->sort_Flag = false;

	/* ----------------
	 *	initialize tuple type.	no need to initialize projection
	 *	info because this node doesn't do projections.
	 * ----------------
	 */
	ExecAssignResultTypeFromOuterPlan((Plan *) node, &sortstate->csstate.cstate);
	ExecAssignScanTypeFromOuterPlan((Plan *) node, &sortstate->csstate);
	sortstate->csstate.cstate.cs_ProjInfo = NULL;

	SO1_printf("ExecInitSort: %s\n",
			   "sort node initialized");

	/* ----------------
	 *	return relation oid of temporary sort relation in a list
	 *	(someday -- for now we return LispTrue... cim 10/12/89)
	 * ----------------
	 */
	return TRUE;
}

int
ExecCountSlotsSort(Sort *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
	ExecCountSlotsNode(innerPlan((Plan *) node)) +
	SORT_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndSort(node)
 *
 * old comments
 * ----------------------------------------------------------------
 */
void
ExecEndSort(Sort *node)
{
	SortState  *sortstate;
	Plan	   *outerPlan;

	/* ----------------
	 *	get info from the sort state
	 * ----------------
	 */
	SO1_printf("ExecEndSort: %s\n",
			   "shutting down sort node");

	sortstate = node->sortstate;

	/* ----------------
	 *	shut down the subplan
	 * ----------------
	 */
	outerPlan = outerPlan((Plan *) node);
	ExecEndNode(outerPlan, (Plan *) node);

	/* ----------------
	 *	clean out the tuple table
	 * ----------------
	 */
	ExecClearTuple(sortstate->csstate.css_ScanTupleSlot);

	/* Clean up after psort */
	psort_end(node);

	SO1_printf("ExecEndSort: %s\n",
			   "sort node shutdown");
}

/* ----------------------------------------------------------------
 *		ExecSortMarkPos
 *
 *		Calls psort to save the current position in the sorted file.
 * ----------------------------------------------------------------
 */
void
ExecSortMarkPos(Sort *node)
{
	SortState  *sortstate;

	/* ----------------
	 *	if we haven't sorted yet, just return
	 * ----------------
	 */
	sortstate = node->sortstate;
	if (sortstate->sort_Flag == false)
		return;

	psort_markpos(node);

	return;
}

/* ----------------------------------------------------------------
 *		ExecSortRestrPos
 *
 *		Calls psort to restore the last saved sort file position.
 * ----------------------------------------------------------------
 */
void
ExecSortRestrPos(Sort *node)
{
	SortState  *sortstate;

	/* ----------------
	 *	if we haven't sorted yet, just return.
	 * ----------------
	 */
	sortstate = node->sortstate;
	if (sortstate->sort_Flag == false)
		return;

	/* ----------------
	 *	restore the scan to the previously marked position
	 * ----------------
	 */
	psort_restorepos(node);
}
