/*-------------------------------------------------------------------------
 *
 * nodeUnique.c
 *	  Routines to handle unique'ing of queries where appropriate
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeUnique.c,v 1.20 1999/02/13 23:15:29 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecUnique		- generate a unique'd temporary relation
 *		ExecInitUnique	- initialize node and subnodes..
 *		ExecEndUnique	- shutdown node and subnodes
 *
 * NOTES
 *		Assumes tuples returned from subplan arrive in
 *		sorted order.
 *
 */
#include <string.h>

#include "postgres.h"
#include "fmgr.h"

#include "executor/executor.h"
#include "executor/nodeUnique.h"
#include "optimizer/clauses.h"
#include "access/heapam.h"
#include "access/printtup.h"	/* for getTypeOutAndElem() */
#include "utils/builtins.h"		/* for namecpy() */

/* ----------------------------------------------------------------
 *		ExecIdenticalTuples
 *
 *		This is a hack function used by ExecUnique to see if
 *		two tuples are identical.  This should be provided
 *		by the heap tuple code but isn't.  The real problem
 *		is that we assume we can byte compare tuples to determine
 *		if they are "equal".  In fact, if we have user defined
 *		types there may be problems because it's possible that
 *		an ADT may have multiple representations with the
 *		same ADT value. -cim
 * ----------------------------------------------------------------
 */
static bool						/* true if tuples are identical, false
								 * otherwise */
ExecIdenticalTuples(TupleTableSlot *t1, TupleTableSlot *t2)
{
	HeapTuple	h1;
	HeapTuple	h2;
	char	   *d1;
	char	   *d2;
	int			len;

	h1 = t1->val;
	h2 = t2->val;

	/* ----------------
	 *	if tuples aren't the same length then they are
	 *	obviously different (one may have null attributes).
	 * ----------------
	 */
	if (h1->t_len != h2->t_len)
		return false;

	/* ----------------
	 *	if the tuples have different header offsets then
	 *	they are different.  This will prevent us from returning
	 *	true when comparing tuples of one attribute where one of
	 *	two we're looking at is null (t_len - t_hoff == 0).
	 *	THE t_len FIELDS CAN BE THE SAME IN THIS CASE!!
	 * ----------------
	 */
	if (h1->t_data->t_hoff != h2->t_data->t_hoff)
		return false;

	/* ----------------
	 *	ok, now get the pointers to the data and the
	 *	size of the attribute portion of the tuple.
	 * ----------------
	 */
	d1 = (char *) GETSTRUCT(h1);
	d2 = (char *) GETSTRUCT(h2);
	len = (int) h1->t_len - (int) h1->t_data->t_hoff;

	/* ----------------
	 *	byte compare the data areas and return the result.
	 * ----------------
	 */
	if (memcmp(d1, d2, len) != 0)
		return false;

	return true;
}

/* ----------------------------------------------------------------
 *		ExecUnique
 *
 *		This is a very simple node which filters out duplicate
 *		tuples from a stream of sorted tuples from a subplan.
 *
 *		XXX see comments below regarding freeing tuples.
 * ----------------------------------------------------------------
 */
TupleTableSlot *				/* return: a tuple or NULL */
ExecUnique(Unique *node)
{
	UniqueState *uniquestate;
	TupleTableSlot *resultTupleSlot;
	TupleTableSlot *slot;
	Plan	   *outerPlan;
	char	   *uniqueAttr;
	AttrNumber	uniqueAttrNum;
	TupleDesc	tupDesc;
	Oid			typoutput,
				typelem;

	/* ----------------
	 *	get information from the node
	 * ----------------
	 */
	uniquestate = node->uniquestate;
	outerPlan = outerPlan((Plan *) node);
	resultTupleSlot = uniquestate->cs_ResultTupleSlot;
	uniqueAttr = node->uniqueAttr;
	uniqueAttrNum = node->uniqueAttrNum;

	if (uniqueAttr)
	{
		tupDesc = ExecGetResultType(uniquestate);
		getTypeOutAndElem((Oid) tupDesc->attrs[uniqueAttrNum - 1]->atttypid,
						  &typoutput, &typelem);
	}
	else
	{							/* keep compiler quiet */
		tupDesc = NULL;
		typoutput = InvalidOid;
		typelem = InvalidOid;
	}

	/* ----------------
	 *	now loop, returning only non-duplicate tuples.
	 *	We assume that the tuples arrive in sorted order
	 *	so we can detect duplicates easily.
	 * ----------------
	 */
	for (;;)
	{
		/* ----------------
		 *	 fetch a tuple from the outer subplan
		 * ----------------
		 */
		slot = ExecProcNode(outerPlan, (Plan *) node);
		if (TupIsNull(slot))
			return NULL;

		/* ----------------
		 *	 we use the result tuple slot to hold our saved tuples.
		 *	 if we haven't a saved tuple to compare our new tuple with,
		 *	 then we exit the loop. This new tuple as the saved tuple
		 *	 the next time we get here.
		 * ----------------
		 */
		if (TupIsNull(resultTupleSlot))
			break;

		/* ----------------
		 *	 now test if the new tuple and the previous
		 *	 tuple match.  If so then we loop back and fetch
		 *	 another new tuple from the subplan.
		 * ----------------
		 */

		if (uniqueAttr)
		{

			/*
			 * to check equality, we check to see if the typoutput of the
			 * attributes are equal
			 */
			bool		isNull1,
						isNull2;
			Datum		attr1,
						attr2;
			char	   *val1,
					   *val2;

			attr1 = heap_getattr(slot->val,
								 uniqueAttrNum, tupDesc, &isNull1);
			attr2 = heap_getattr(resultTupleSlot->val,
								 uniqueAttrNum, tupDesc, &isNull2);

			if (isNull1 == isNull2)
			{
				if (isNull1)	/* both are null, they are equal */
					continue;
				val1 = fmgr(typoutput, attr1, typelem,
							tupDesc->attrs[uniqueAttrNum - 1]->atttypmod);
				val2 = fmgr(typoutput, attr2, typelem,
							tupDesc->attrs[uniqueAttrNum - 1]->atttypmod);

				/*
				 * now, val1 and val2 are ascii representations so we can
				 * use strcmp for comparison
				 */
				if (strcmp(val1, val2) == 0)	/* they are equal */
				{
					pfree(val1);
					pfree(val2);
					continue;
				}
				pfree(val1);
				pfree(val2);
				break;
			}
			else
/* one is null and the other isn't, they aren't equal */
				break;

		}
		else
		{
			if (!ExecIdenticalTuples(slot, resultTupleSlot))
				break;
		}

	}

	/* ----------------
	 *	we have a new tuple different from the previous saved tuple
	 *	so we save it in the saved tuple slot.	We copy the tuple
	 *	so we don't increment the buffer ref count.
	 * ----------------
	 */
	ExecStoreTuple(heap_copytuple(slot->val),
				   resultTupleSlot,
				   InvalidBuffer,
				   true);

	return resultTupleSlot;
}

/* ----------------------------------------------------------------
 *		ExecInitUnique
 *
 *		This initializes the unique node state structures and
 *		the node's subplan.
 * ----------------------------------------------------------------
 */
bool							/* return: initialization status */
ExecInitUnique(Unique *node, EState *estate, Plan *parent)
{
	UniqueState *uniquestate;
	Plan	   *outerPlan;
	char	   *uniqueAttr;

	/* ----------------
	 *	assign execution state to node
	 * ----------------
	 */
	node->plan.state = estate;

	/* ----------------
	 *	create new UniqueState for node
	 * ----------------
	 */
	uniquestate = makeNode(UniqueState);
	node->uniquestate = uniquestate;
	uniqueAttr = node->uniqueAttr;

	/* ----------------
	 *	Miscellanious initialization
	 *
	 *		 +	assign node's base_id
	 *		 +	assign debugging hooks and
	 *
	 *	Unique nodes have no ExprContext initialization because
	 *	they never call ExecQual or ExecTargetList.
	 * ----------------
	 */
	ExecAssignNodeBaseInfo(estate, uniquestate, parent);

#define UNIQUE_NSLOTS 1
	/* ------------
	 * Tuple table initialization
	 * ------------
	 */
	ExecInitResultTupleSlot(estate, uniquestate);

	/* ----------------
	 *	then initialize outer plan
	 * ----------------
	 */
	outerPlan = outerPlan((Plan *) node);
	ExecInitNode(outerPlan, estate, (Plan *) node);

	/* ----------------
	 *	unique nodes do no projections, so initialize
	 *	projection info for this node appropriately
	 * ----------------
	 */
	ExecAssignResultTypeFromOuterPlan((Plan *) node, uniquestate);
	uniquestate->cs_ProjInfo = NULL;

	if (uniqueAttr)
	{
		TupleDesc	tupDesc;
		int			i = 0;

		tupDesc = ExecGetResultType(uniquestate);

		/*
		 * the parser should have ensured that uniqueAttr is a legal
		 * attribute name
		 */
		while (strcmp((tupDesc->attrs[i]->attname).data, uniqueAttr) != 0)
			i++;
		node->uniqueAttrNum = i + 1;	/* attribute numbers start from 1 */
	}
	else
		node->uniqueAttrNum = InvalidAttrNumber;

	/* ----------------
	 *	all done.
	 * ----------------
	 */
	return TRUE;
}

int
ExecCountSlotsUnique(Unique *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
	ExecCountSlotsNode(innerPlan(node)) +
	UNIQUE_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndUnique
 *
 *		This shuts down the subplan and frees resources allocated
 *		to this node.
 * ----------------------------------------------------------------
 */
void
ExecEndUnique(Unique *node)
{
	UniqueState *uniquestate;

	uniquestate = node->uniquestate;
	ExecEndNode(outerPlan((Plan *) node), (Plan *) node);
	ExecClearTuple(uniquestate->cs_ResultTupleSlot);
}


void
ExecReScanUnique(Unique *node, ExprContext *exprCtxt, Plan *parent)
{
	UniqueState *uniquestate = node->uniquestate;

	ExecClearTuple(uniquestate->cs_ResultTupleSlot);

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);

}
