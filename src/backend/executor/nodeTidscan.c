/*-------------------------------------------------------------------------
 *
 * nodeTidscan.c
 *	  Routines to support direct tid scans of relations
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeTidscan.c,v 1.58 2008/01/01 19:45:49 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *
 *		ExecTidScan			scans a relation using tids
 *		ExecInitTidScan		creates and initializes state info.
 *		ExecTidReScan		rescans the tid relation.
 *		ExecEndTidScan		releases all storage.
 *		ExecTidMarkPos		marks scan position.
 *		ExecTidRestrPos		restores scan position.
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_type.h"
#include "executor/execdebug.h"
#include "executor/nodeTidscan.h"
#include "optimizer/clauses.h"
#include "utils/array.h"


#define IsCTIDVar(node)  \
	((node) != NULL && \
	 IsA((node), Var) && \
	 ((Var *) (node))->varattno == SelfItemPointerAttributeNumber && \
	 ((Var *) (node))->varlevelsup == 0)

static void TidListCreate(TidScanState *tidstate);
static int	itemptr_comparator(const void *a, const void *b);
static TupleTableSlot *TidNext(TidScanState *node);


/*
 * Compute the list of TIDs to be visited, by evaluating the expressions
 * for them.
 *
 * (The result is actually an array, not a list.)
 */
static void
TidListCreate(TidScanState *tidstate)
{
	List	   *evalList = tidstate->tss_tidquals;
	ExprContext *econtext = tidstate->ss.ps.ps_ExprContext;
	ItemPointerData *tidList;
	int			numAllocTids;
	int			numTids;
	ListCell   *l;

	/*
	 * We initialize the array with enough slots for the case that all quals
	 * are simple OpExprs or CurrentOfExprs.  If there are any
	 * ScalarArrayOpExprs, we may have to enlarge the array.
	 */
	numAllocTids = list_length(evalList);
	tidList = (ItemPointerData *)
		palloc(numAllocTids * sizeof(ItemPointerData));
	numTids = 0;
	tidstate->tss_isCurrentOf = false;

	foreach(l, evalList)
	{
		ExprState  *exstate = (ExprState *) lfirst(l);
		Expr	   *expr = exstate->expr;
		ItemPointer itemptr;
		bool		isNull;

		if (is_opclause(expr))
		{
			FuncExprState *fexstate = (FuncExprState *) exstate;
			Node	   *arg1;
			Node	   *arg2;

			arg1 = get_leftop(expr);
			arg2 = get_rightop(expr);
			if (IsCTIDVar(arg1))
				exstate = (ExprState *) lsecond(fexstate->args);
			else if (IsCTIDVar(arg2))
				exstate = (ExprState *) linitial(fexstate->args);
			else
				elog(ERROR, "could not identify CTID variable");

			itemptr = (ItemPointer)
				DatumGetPointer(ExecEvalExprSwitchContext(exstate,
														  econtext,
														  &isNull,
														  NULL));
			if (!isNull && ItemPointerIsValid(itemptr))
			{
				if (numTids >= numAllocTids)
				{
					numAllocTids *= 2;
					tidList = (ItemPointerData *)
						repalloc(tidList,
								 numAllocTids * sizeof(ItemPointerData));
				}
				tidList[numTids++] = *itemptr;
			}
		}
		else if (expr && IsA(expr, ScalarArrayOpExpr))
		{
			ScalarArrayOpExprState *saexstate = (ScalarArrayOpExprState *) exstate;
			Datum		arraydatum;
			ArrayType  *itemarray;
			Datum	   *ipdatums;
			bool	   *ipnulls;
			int			ndatums;
			int			i;

			exstate = (ExprState *) lsecond(saexstate->fxprstate.args);
			arraydatum = ExecEvalExprSwitchContext(exstate,
												   econtext,
												   &isNull,
												   NULL);
			if (isNull)
				continue;
			itemarray = DatumGetArrayTypeP(arraydatum);
			deconstruct_array(itemarray,
							  TIDOID, SizeOfIptrData, false, 's',
							  &ipdatums, &ipnulls, &ndatums);
			if (numTids + ndatums > numAllocTids)
			{
				numAllocTids = numTids + ndatums;
				tidList = (ItemPointerData *)
					repalloc(tidList,
							 numAllocTids * sizeof(ItemPointerData));
			}
			for (i = 0; i < ndatums; i++)
			{
				if (!ipnulls[i])
				{
					itemptr = (ItemPointer) DatumGetPointer(ipdatums[i]);
					if (ItemPointerIsValid(itemptr))
						tidList[numTids++] = *itemptr;
				}
			}
			pfree(ipdatums);
			pfree(ipnulls);
		}
		else if (expr && IsA(expr, CurrentOfExpr))
		{
			CurrentOfExpr *cexpr = (CurrentOfExpr *) expr;
			ItemPointerData cursor_tid;

			if (execCurrentOf(cexpr, econtext,
						   RelationGetRelid(tidstate->ss.ss_currentRelation),
							  &cursor_tid))
			{
				if (numTids >= numAllocTids)
				{
					numAllocTids *= 2;
					tidList = (ItemPointerData *)
						repalloc(tidList,
								 numAllocTids * sizeof(ItemPointerData));
				}
				tidList[numTids++] = cursor_tid;
				tidstate->tss_isCurrentOf = true;
			}
		}
		else
			elog(ERROR, "could not identify CTID expression");
	}

	/*
	 * Sort the array of TIDs into order, and eliminate duplicates.
	 * Eliminating duplicates is necessary since we want OR semantics across
	 * the list.  Sorting makes it easier to detect duplicates, and as a bonus
	 * ensures that we will visit the heap in the most efficient way.
	 */
	if (numTids > 1)
	{
		int			lastTid;
		int			i;

		/* CurrentOfExpr could never appear OR'd with something else */
		Assert(!tidstate->tss_isCurrentOf);

		qsort((void *) tidList, numTids, sizeof(ItemPointerData),
			  itemptr_comparator);
		lastTid = 0;
		for (i = 1; i < numTids; i++)
		{
			if (!ItemPointerEquals(&tidList[lastTid], &tidList[i]))
				tidList[++lastTid] = tidList[i];
		}
		numTids = lastTid + 1;
	}

	tidstate->tss_TidList = tidList;
	tidstate->tss_NumTids = numTids;
	tidstate->tss_TidPtr = -1;
}

/*
 * qsort comparator for ItemPointerData items
 */
static int
itemptr_comparator(const void *a, const void *b)
{
	const ItemPointerData *ipa = (const ItemPointerData *) a;
	const ItemPointerData *ipb = (const ItemPointerData *) b;
	BlockNumber ba = ItemPointerGetBlockNumber(ipa);
	BlockNumber bb = ItemPointerGetBlockNumber(ipb);
	OffsetNumber oa = ItemPointerGetOffsetNumber(ipa);
	OffsetNumber ob = ItemPointerGetOffsetNumber(ipb);

	if (ba < bb)
		return -1;
	if (ba > bb)
		return 1;
	if (oa < ob)
		return -1;
	if (oa > ob)
		return 1;
	return 0;
}

/* ----------------------------------------------------------------
 *		TidNext
 *
 *		Retrieve a tuple from the TidScan node's currentRelation
 *		using the tids in the TidScanState information.
 *
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
TidNext(TidScanState *node)
{
	EState	   *estate;
	ScanDirection direction;
	Snapshot	snapshot;
	Relation	heapRelation;
	HeapTuple	tuple;
	TupleTableSlot *slot;
	Index		scanrelid;
	Buffer		buffer = InvalidBuffer;
	ItemPointerData *tidList;
	int			numTids;
	bool		bBackward;

	/*
	 * extract necessary information from tid scan node
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	snapshot = estate->es_snapshot;
	heapRelation = node->ss.ss_currentRelation;
	slot = node->ss.ss_ScanTupleSlot;
	scanrelid = ((TidScan *) node->ss.ps.plan)->scan.scanrelid;

	/*
	 * Check if we are evaluating PlanQual for tuple of this relation.
	 * Additional checking is not good, but no other way for now. We could
	 * introduce new nodes for this case and handle TidScan --> NewNode
	 * switching in Init/ReScan plan...
	 */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		if (estate->es_evTupleNull[scanrelid - 1])
			return ExecClearTuple(slot);

		/*
		 * XXX shouldn't we check here to make sure tuple matches TID list? In
		 * runtime-key case this is not certain, is it?  However, in the WHERE
		 * CURRENT OF case it might not match anyway ...
		 */

		ExecStoreTuple(estate->es_evTuple[scanrelid - 1],
					   slot, InvalidBuffer, false);

		/* Flag for the next call that no more tuples */
		estate->es_evTupleNull[scanrelid - 1] = true;
		return slot;
	}

	/*
	 * First time through, compute the list of TIDs to be visited
	 */
	if (node->tss_TidList == NULL)
		TidListCreate(node);

	tidList = node->tss_TidList;
	numTids = node->tss_NumTids;

	tuple = &(node->tss_htup);

	/*
	 * Initialize or advance scan position, depending on direction.
	 */
	bBackward = ScanDirectionIsBackward(direction);
	if (bBackward)
	{
		if (node->tss_TidPtr < 0)
		{
			/* initialize for backward scan */
			node->tss_TidPtr = numTids - 1;
		}
		else
			node->tss_TidPtr--;
	}
	else
	{
		if (node->tss_TidPtr < 0)
		{
			/* initialize for forward scan */
			node->tss_TidPtr = 0;
		}
		else
			node->tss_TidPtr++;
	}

	while (node->tss_TidPtr >= 0 && node->tss_TidPtr < numTids)
	{
		tuple->t_self = tidList[node->tss_TidPtr];

		/*
		 * For WHERE CURRENT OF, the tuple retrieved from the cursor might
		 * since have been updated; if so, we should fetch the version that is
		 * current according to our snapshot.
		 */
		if (node->tss_isCurrentOf)
			heap_get_latest_tid(heapRelation, snapshot, &tuple->t_self);

		if (heap_fetch(heapRelation, snapshot, tuple, &buffer, false, NULL))
		{
			/*
			 * store the scanned tuple in the scan tuple slot of the scan
			 * state.  Eventually we will only do this and not return a tuple.
			 * Note: we pass 'false' because tuples returned by amgetnext are
			 * pointers onto disk pages and were not created with palloc() and
			 * so should not be pfree()'d.
			 */
			ExecStoreTuple(tuple,		/* tuple to store */
						   slot,	/* slot to store in */
						   buffer,		/* buffer associated with tuple  */
						   false);		/* don't pfree */

			/*
			 * At this point we have an extra pin on the buffer, because
			 * ExecStoreTuple incremented the pin count. Drop our local pin.
			 */
			ReleaseBuffer(buffer);

			return slot;
		}
		/* Bad TID or failed snapshot qual; try next */
		if (bBackward)
			node->tss_TidPtr--;
		else
			node->tss_TidPtr++;
	}

	/*
	 * if we get here it means the tid scan failed so we are at the end of the
	 * scan..
	 */
	return ExecClearTuple(slot);
}

/* ----------------------------------------------------------------
 *		ExecTidScan(node)
 *
 *		Scans the relation using tids and returns
 *		   the next qualifying tuple in the direction specified.
 *		It calls ExecScan() and passes it the access methods which returns
 *		the next tuple using the tids.
 *
 *		Conditions:
 *		  -- the "cursor" maintained by the AMI is positioned at the tuple
 *			 returned previously.
 *
 *		Initial States:
 *		  -- the relation indicated is opened for scanning so that the
 *			 "cursor" is positioned before the first qualifying tuple.
 *		  -- tidPtr is -1.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecTidScan(TidScanState *node)
{
	/*
	 * use TidNext as access method
	 */
	return ExecScan(&node->ss, (ExecScanAccessMtd) TidNext);
}

/* ----------------------------------------------------------------
 *		ExecTidReScan(node)
 * ----------------------------------------------------------------
 */
void
ExecTidReScan(TidScanState *node, ExprContext *exprCtxt)
{
	EState	   *estate;
	Index		scanrelid;

	estate = node->ss.ps.state;
	scanrelid = ((TidScan *) node->ss.ps.plan)->scan.scanrelid;

	node->ss.ps.ps_TupFromTlist = false;

	/* If we are being passed an outer tuple, save it for runtime key calc */
	if (exprCtxt != NULL)
		node->ss.ps.ps_ExprContext->ecxt_outertuple =
			exprCtxt->ecxt_outertuple;

	/* If this is re-scanning of PlanQual ... */
	if (estate->es_evTuple != NULL &&
		estate->es_evTuple[scanrelid - 1] != NULL)
	{
		estate->es_evTupleNull[scanrelid - 1] = false;
		return;
	}

	if (node->tss_TidList)
		pfree(node->tss_TidList);
	node->tss_TidList = NULL;
	node->tss_NumTids = 0;
	node->tss_TidPtr = -1;
}

/* ----------------------------------------------------------------
 *		ExecEndTidScan
 *
 *		Releases any storage allocated through C routines.
 *		Returns nothing.
 * ----------------------------------------------------------------
 */
void
ExecEndTidScan(TidScanState *node)
{
	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ss.ps);

	/*
	 * clear out tuple table slots
	 */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * close the heap relation.
	 */
	ExecCloseScanRelation(node->ss.ss_currentRelation);
}

/* ----------------------------------------------------------------
 *		ExecTidMarkPos
 *
 *		Marks scan position by marking the current tid.
 *		Returns nothing.
 * ----------------------------------------------------------------
 */
void
ExecTidMarkPos(TidScanState *node)
{
	node->tss_MarkTidPtr = node->tss_TidPtr;
}

/* ----------------------------------------------------------------
 *		ExecTidRestrPos
 *
 *		Restores scan position by restoring the current tid.
 *		Returns nothing.
 *
 *		XXX Assumes previously marked scan position belongs to current tid
 * ----------------------------------------------------------------
 */
void
ExecTidRestrPos(TidScanState *node)
{
	node->tss_TidPtr = node->tss_MarkTidPtr;
}

/* ----------------------------------------------------------------
 *		ExecInitTidScan
 *
 *		Initializes the tid scan's state information, creates
 *		scan keys, and opens the base and tid relations.
 *
 *		Parameters:
 *		  node: TidNode node produced by the planner.
 *		  estate: the execution state initialized in InitPlan.
 * ----------------------------------------------------------------
 */
TidScanState *
ExecInitTidScan(TidScan *node, EState *estate, int eflags)
{
	TidScanState *tidstate;
	Relation	currentRelation;

	/*
	 * create state structure
	 */
	tidstate = makeNode(TidScanState);
	tidstate->ss.ps.plan = (Plan *) node;
	tidstate->ss.ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &tidstate->ss.ps);

	tidstate->ss.ps.ps_TupFromTlist = false;

	/*
	 * initialize child expressions
	 */
	tidstate->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->scan.plan.targetlist,
					 (PlanState *) tidstate);
	tidstate->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->scan.plan.qual,
					 (PlanState *) tidstate);

	tidstate->tss_tidquals = (List *)
		ExecInitExpr((Expr *) node->tidquals,
					 (PlanState *) tidstate);

#define TIDSCAN_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &tidstate->ss.ps);
	ExecInitScanTupleSlot(estate, &tidstate->ss);

	/*
	 * mark tid list as not computed yet
	 */
	tidstate->tss_TidList = NULL;
	tidstate->tss_NumTids = 0;
	tidstate->tss_TidPtr = -1;

	/*
	 * open the base relation and acquire appropriate lock on it.
	 */
	currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid);

	tidstate->ss.ss_currentRelation = currentRelation;
	tidstate->ss.ss_currentScanDesc = NULL;		/* no heap scan here */

	/*
	 * get the scan type from the relation descriptor.
	 */
	ExecAssignScanType(&tidstate->ss, RelationGetDescr(currentRelation));

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&tidstate->ss.ps);
	ExecAssignScanProjectionInfo(&tidstate->ss);

	/*
	 * all done.
	 */
	return tidstate;
}

int
ExecCountSlotsTidScan(TidScan *node)
{
	return ExecCountSlotsNode(outerPlan((Plan *) node)) +
		ExecCountSlotsNode(innerPlan((Plan *) node)) + TIDSCAN_NSLOTS;
}
