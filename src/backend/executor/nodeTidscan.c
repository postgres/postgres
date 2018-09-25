/*-------------------------------------------------------------------------
 *
 * nodeTidscan.c
 *	  Routines to support direct tid scans of relations
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeTidscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *
 *		ExecTidScan			scans a relation using tids
 *		ExecInitTidScan		creates and initializes state info.
 *		ExecReScanTidScan	rescans the tid relation.
 *		ExecEndTidScan		releases all storage.
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "executor/execdebug.h"
#include "executor/nodeTidscan.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/rel.h"


#define IsCTIDVar(node)  \
	((node) != NULL && \
	 IsA((node), Var) && \
	 ((Var *) (node))->varattno == SelfItemPointerAttributeNumber && \
	 ((Var *) (node))->varlevelsup == 0)

/* one element in tss_tidexprs */
typedef struct TidExpr
{
	ExprState  *exprstate;		/* ExprState for a TID-yielding subexpr */
	bool		isarray;		/* if true, it yields tid[] not just tid */
	CurrentOfExpr *cexpr;		/* alternatively, we can have CURRENT OF */
} TidExpr;

static void TidExprListCreate(TidScanState *tidstate);
static void TidListEval(TidScanState *tidstate);
static int	itemptr_comparator(const void *a, const void *b);
static TupleTableSlot *TidNext(TidScanState *node);


/*
 * Extract the qual subexpressions that yield TIDs to search for,
 * and compile them into ExprStates if they're ordinary expressions.
 *
 * CURRENT OF is a special case that we can't compile usefully;
 * just drop it into the TidExpr list as-is.
 */
static void
TidExprListCreate(TidScanState *tidstate)
{
	TidScan    *node = (TidScan *) tidstate->ss.ps.plan;
	ListCell   *l;

	tidstate->tss_tidexprs = NIL;
	tidstate->tss_isCurrentOf = false;

	foreach(l, node->tidquals)
	{
		Expr	   *expr = (Expr *) lfirst(l);
		TidExpr    *tidexpr = (TidExpr *) palloc0(sizeof(TidExpr));

		if (is_opclause(expr))
		{
			Node	   *arg1;
			Node	   *arg2;

			arg1 = get_leftop(expr);
			arg2 = get_rightop(expr);
			if (IsCTIDVar(arg1))
				tidexpr->exprstate = ExecInitExpr((Expr *) arg2,
												  &tidstate->ss.ps);
			else if (IsCTIDVar(arg2))
				tidexpr->exprstate = ExecInitExpr((Expr *) arg1,
												  &tidstate->ss.ps);
			else
				elog(ERROR, "could not identify CTID variable");
			tidexpr->isarray = false;
		}
		else if (expr && IsA(expr, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saex = (ScalarArrayOpExpr *) expr;

			Assert(IsCTIDVar(linitial(saex->args)));
			tidexpr->exprstate = ExecInitExpr(lsecond(saex->args),
											  &tidstate->ss.ps);
			tidexpr->isarray = true;
		}
		else if (expr && IsA(expr, CurrentOfExpr))
		{
			CurrentOfExpr *cexpr = (CurrentOfExpr *) expr;

			tidexpr->cexpr = cexpr;
			tidstate->tss_isCurrentOf = true;
		}
		else
			elog(ERROR, "could not identify CTID expression");

		tidstate->tss_tidexprs = lappend(tidstate->tss_tidexprs, tidexpr);
	}

	/* CurrentOfExpr could never appear OR'd with something else */
	Assert(list_length(tidstate->tss_tidexprs) == 1 ||
		   !tidstate->tss_isCurrentOf);
}

/*
 * Compute the list of TIDs to be visited, by evaluating the expressions
 * for them.
 *
 * (The result is actually an array, not a list.)
 */
static void
TidListEval(TidScanState *tidstate)
{
	ExprContext *econtext = tidstate->ss.ps.ps_ExprContext;
	BlockNumber nblocks;
	ItemPointerData *tidList;
	int			numAllocTids;
	int			numTids;
	ListCell   *l;

	/*
	 * We silently discard any TIDs that are out of range at the time of scan
	 * start.  (Since we hold at least AccessShareLock on the table, it won't
	 * be possible for someone to truncate away the blocks we intend to
	 * visit.)
	 */
	nblocks = RelationGetNumberOfBlocks(tidstate->ss.ss_currentRelation);

	/*
	 * We initialize the array with enough slots for the case that all quals
	 * are simple OpExprs or CurrentOfExprs.  If there are any
	 * ScalarArrayOpExprs, we may have to enlarge the array.
	 */
	numAllocTids = list_length(tidstate->tss_tidexprs);
	tidList = (ItemPointerData *)
		palloc(numAllocTids * sizeof(ItemPointerData));
	numTids = 0;

	foreach(l, tidstate->tss_tidexprs)
	{
		TidExpr    *tidexpr = (TidExpr *) lfirst(l);
		ItemPointer itemptr;
		bool		isNull;

		if (tidexpr->exprstate && !tidexpr->isarray)
		{
			itemptr = (ItemPointer)
				DatumGetPointer(ExecEvalExprSwitchContext(tidexpr->exprstate,
														  econtext,
														  &isNull));
			if (!isNull &&
				ItemPointerIsValid(itemptr) &&
				ItemPointerGetBlockNumber(itemptr) < nblocks)
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
		else if (tidexpr->exprstate && tidexpr->isarray)
		{
			Datum		arraydatum;
			ArrayType  *itemarray;
			Datum	   *ipdatums;
			bool	   *ipnulls;
			int			ndatums;
			int			i;

			arraydatum = ExecEvalExprSwitchContext(tidexpr->exprstate,
												   econtext,
												   &isNull);
			if (isNull)
				continue;
			itemarray = DatumGetArrayTypeP(arraydatum);
			deconstruct_array(itemarray,
							  TIDOID, sizeof(ItemPointerData), false, 's',
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
					if (ItemPointerIsValid(itemptr) &&
						ItemPointerGetBlockNumber(itemptr) < nblocks)
						tidList[numTids++] = *itemptr;
				}
			}
			pfree(ipdatums);
			pfree(ipnulls);
		}
		else
		{
			ItemPointerData cursor_tid;

			Assert(tidexpr->cexpr);
			if (execCurrentOf(tidexpr->cexpr, econtext,
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
			}
		}
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

	/*
	 * First time through, compute the list of TIDs to be visited
	 */
	if (node->tss_TidList == NULL)
		TidListEval(node);

	tidList = node->tss_TidList;
	numTids = node->tss_NumTids;

	/*
	 * We use node->tss_htup as the tuple pointer; note this can't just be a
	 * local variable here, as the scan tuple slot will keep a pointer to it.
	 */
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
			 * Store the scanned tuple in the scan tuple slot of the scan
			 * state.  Eventually we will only do this and not return a tuple.
			 */
			ExecStoreBufferHeapTuple(tuple,	/* tuple to store */
									 slot,	/* slot to store in */
									 buffer);	/* buffer associated with
												 * tuple */

			/*
			 * At this point we have an extra pin on the buffer, because
			 * ExecStoreHeapTuple incremented the pin count. Drop our local
			 * pin.
			 */
			ReleaseBuffer(buffer);

			return slot;
		}
		/* Bad TID or failed snapshot qual; try next */
		if (bBackward)
			node->tss_TidPtr--;
		else
			node->tss_TidPtr++;

		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * if we get here it means the tid scan failed so we are at the end of the
	 * scan..
	 */
	return ExecClearTuple(slot);
}

/*
 * TidRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
TidRecheck(TidScanState *node, TupleTableSlot *slot)
{
	/*
	 * XXX shouldn't we check here to make sure tuple matches TID list? In
	 * runtime-key case this is not certain, is it?  However, in the WHERE
	 * CURRENT OF case it might not match anyway ...
	 */
	return true;
}


/* ----------------------------------------------------------------
 *		ExecTidScan(node)
 *
 *		Scans the relation using tids and returns
 *		   the next qualifying tuple in the direction specified.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
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
static TupleTableSlot *
ExecTidScan(PlanState *pstate)
{
	TidScanState *node = castNode(TidScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) TidNext,
					(ExecScanRecheckMtd) TidRecheck);
}

/* ----------------------------------------------------------------
 *		ExecReScanTidScan(node)
 * ----------------------------------------------------------------
 */
void
ExecReScanTidScan(TidScanState *node)
{
	if (node->tss_TidList)
		pfree(node->tss_TidList);
	node->tss_TidList = NULL;
	node->tss_NumTids = 0;
	node->tss_TidPtr = -1;

	ExecScanReScan(&node->ss);
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
	tidstate->ss.ps.ExecProcNode = ExecTidScan;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &tidstate->ss.ps);

	/*
	 * mark tid list as not computed yet
	 */
	tidstate->tss_TidList = NULL;
	tidstate->tss_NumTids = 0;
	tidstate->tss_TidPtr = -1;

	/*
	 * open the base relation and acquire appropriate lock on it.
	 */
	currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid, eflags);

	tidstate->ss.ss_currentRelation = currentRelation;
	tidstate->ss.ss_currentScanDesc = NULL; /* no heap scan here */

	/*
	 * get the scan type from the relation descriptor.
	 */
	ExecInitScanTupleSlot(estate, &tidstate->ss,
						  RelationGetDescr(currentRelation));

	/*
	 * Initialize result slot, type and projection.
	 */
	ExecInitResultTupleSlotTL(estate, &tidstate->ss.ps);
	ExecAssignScanProjectionInfo(&tidstate->ss);

	/*
	 * initialize child expressions
	 */
	tidstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) tidstate);

	TidExprListCreate(tidstate);

	/*
	 * all done.
	 */
	return tidstate;
}
