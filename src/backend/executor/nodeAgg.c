/*-------------------------------------------------------------------------
 *
 * nodeAgg.c
 *	  Routines to handle aggregate nodes.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * NOTE
 *	  The implementation of Agg node has been reworked to handle legal
 *	  SQL aggregates. (Do not expect POSTQUEL semantics.)	 -- ay 2/95
 *
 * IDENTIFICATION
 *	  /usr/local/devel/pglite/cvs/src/backend/executor/nodeAgg.c,v 1.13 1995/08/01 20:19:07 jolly Exp
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_aggregate.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "optimizer/clauses.h"
#include "parser/parse_type.h"
#include "utils/syscache.h"

/*
 * AggFuncInfo -
 *	  keeps the transition functions information around
 */
typedef struct AggFuncInfo
{
	Oid			xfn1_oid;
	Oid			xfn2_oid;
	Oid			finalfn_oid;
	FmgrInfo	xfn1;
	FmgrInfo	xfn2;
	FmgrInfo	finalfn;
} AggFuncInfo;

static Datum aggGetAttr(TupleTableSlot *tuple, Aggref *aggref, bool *isNull);


/* ---------------------------------------
 *
 * ExecAgg -
 *
 *	  ExecAgg receives tuples from its outer subplan and aggregates over
 *	  the appropriate attribute for each (unique) aggregate in the target
 *	  list. (The number of tuples to aggregate over depends on whether a
 *	  GROUP BY clause is present. It might be the number of tuples in a
 *	  group or all the tuples that satisfy the qualifications.) The value of
 *	  each aggregate is stored in the expression context for ExecProject to
 *	  evaluate the result tuple.
 *
 *	  ExecAgg evaluates each aggregate in the following steps: (initcond1,
 *	  initcond2 are the initial values and sfunc1, sfunc2, and finalfunc are
 *	  the transition functions.)
 *
 *		 value1[i] = initcond1
 *		 value2[i] = initcond2
 *		 forall tuples do
 *			value1[i] = sfunc1(value1[i], aggregated_value)
 *			value2[i] = sfunc2(value2[i])
 *		 value1[i] = finalfunc(value1[i], value2[i])
 *
 *	  If initcond1 is NULL then the first non-NULL aggregated_value is
 *	  assigned directly to value1[i].  sfunc1 isn't applied until value1[i]
 *	  is non-NULL.
 *
 *	  If the outer subplan is a Group node, ExecAgg returns as many tuples
 *	  as there are groups.
 *
 *	  XXX handling of NULL doesn't work
 *
 *	  OLD COMMENTS
 *
 *		XXX Aggregates should probably have another option: what to do
 *		with transfn2 if we hit a null value.  "count" (transfn1 = null,
 *		transfn2 = increment) will want to have transfn2 called; "avg"
 *		(transfn1 = add, transfn2 = increment) will not. -pma 1/3/93
 *
 * ------------------------------------------
 */
TupleTableSlot *
ExecAgg(Agg *node)
{
	AggState   *aggstate;
	EState	   *estate;
	Plan	   *outerPlan;
	int			aggno,
				nagg;
	Datum	   *value1,
			   *value2;
	int		   *noInitValue;
	AggFuncInfo *aggFuncInfo;
	long		nTuplesAgged = 0;
	ExprContext *econtext;
	ProjectionInfo *projInfo;
	TupleTableSlot *resultSlot;
	HeapTuple	oneTuple;
	List	   *alist;
	char	   *nulls;
	bool		isDone;
	bool		isNull = FALSE,
				isNull1 = FALSE,
				isNull2 = FALSE;
	bool		qual_result;


	/* ---------------------
	 *	get state info from node
	 * ---------------------
	 */

	/*
	 * We loop retrieving groups until we find one matching
	 * node->plan.qual
	 */
	do
	{
		aggstate = node->aggstate;
		if (aggstate->agg_done)
			return NULL;

		estate = node->plan.state;
		econtext = aggstate->csstate.cstate.cs_ExprContext;

		nagg = length(node->aggs);

		value1 = node->aggstate->csstate.cstate.cs_ExprContext->ecxt_values;
		nulls = node->aggstate->csstate.cstate.cs_ExprContext->ecxt_nulls;

		value2 = (Datum *) palloc(sizeof(Datum) * nagg);
		MemSet(value2, 0, sizeof(Datum) * nagg);

		aggFuncInfo = (AggFuncInfo *) palloc(sizeof(AggFuncInfo) * nagg);
		MemSet(aggFuncInfo, 0, sizeof(AggFuncInfo) * nagg);

		noInitValue = (int *) palloc(sizeof(int) * nagg);
		MemSet(noInitValue, 0, sizeof(int) * nagg);

		outerPlan = outerPlan(node);
		oneTuple = NULL;

		projInfo = aggstate->csstate.cstate.cs_ProjInfo;

		aggno = -1;
		foreach(alist, node->aggs)
		{
			Aggref	   *aggref = lfirst(alist);
			char	   *aggname;
			HeapTuple	aggTuple;
			Form_pg_aggregate aggp;
			Oid			xfn1_oid,
						xfn2_oid,
						finalfn_oid;

			aggref->aggno = ++aggno;

			/* ---------------------
			 *	find transfer functions of all the aggregates and initialize
			 *	their initial values
			 * ---------------------
			 */
			aggname = aggref->aggname;
			aggTuple = SearchSysCacheTuple(AGGNAME,
										   PointerGetDatum(aggname),
									  ObjectIdGetDatum(aggref->basetype),
										   0, 0);
			if (!HeapTupleIsValid(aggTuple))
				elog(ERROR, "ExecAgg: cache lookup failed for aggregate \"%s\"(%s)",
					 aggname,
					 typeidTypeName(aggref->basetype));
			aggp = (Form_pg_aggregate) GETSTRUCT(aggTuple);

			xfn1_oid = aggp->aggtransfn1;
			xfn2_oid = aggp->aggtransfn2;
			finalfn_oid = aggp->aggfinalfn;

			if (OidIsValid(finalfn_oid))
			{
				fmgr_info(finalfn_oid, &aggFuncInfo[aggno].finalfn);
				aggFuncInfo[aggno].finalfn_oid = finalfn_oid;
			}

			if (OidIsValid(xfn2_oid))
			{
				fmgr_info(xfn2_oid, &aggFuncInfo[aggno].xfn2);
				aggFuncInfo[aggno].xfn2_oid = xfn2_oid;
				value2[aggno] = (Datum) AggNameGetInitVal((char *) aggname,
													   aggp->aggbasetype,
														  2,
														  &isNull2);
				/* ------------------------------------------
				 * If there is a second transition function, its initial
				 * value must exist -- as it does not depend on data values,
				 * we have no other way of determining an initial value.
				 * ------------------------------------------
				 */
				if (isNull2)
					elog(ERROR, "ExecAgg: agginitval2 is null");
			}

			if (OidIsValid(xfn1_oid))
			{
				fmgr_info(xfn1_oid, &aggFuncInfo[aggno].xfn1);
				aggFuncInfo[aggno].xfn1_oid = xfn1_oid;
				value1[aggno] = (Datum) AggNameGetInitVal((char *) aggname,
													   aggp->aggbasetype,
														  1,
														  &isNull1);

				/* ------------------------------------------
				 * If the initial value for the first transition function
				 * doesn't exist in the pg_aggregate table then we let
				 * the first value returned from the outer procNode become
				 * the initial value. (This is useful for aggregates like
				 * max{} and min{}.)
				 * ------------------------------------------
				 */
				if (isNull1)
				{
					noInitValue[aggno] = 1;
					nulls[aggno] = 1;
				}
			}
		}

		/* ----------------
		 *	 for each tuple from the the outer plan, apply all the aggregates
		 * ----------------
		 */
		for (;;)
		{
			TupleTableSlot *outerslot;

			isNull = isNull1 = isNull2 = 0;
			outerslot = ExecProcNode(outerPlan, (Plan *) node);
			if (TupIsNull(outerslot))
			{

				/*
				 * when the outerplan doesn't return a single tuple,
				 * create a dummy heaptuple anyway because we still need
				 * to return a valid aggregate value. The value returned
				 * will be the initial values of the transition functions
				 */
				if (nTuplesAgged == 0)
				{
					TupleDesc	tupType;
					Datum	   *tupValue;
					char	   *null_array;
					AttrNumber	attnum;

					tupType = aggstate->csstate.css_ScanTupleSlot->ttc_tupleDescriptor;
					tupValue = projInfo->pi_tupValue;

					/* initially, set all the values to NULL */
					null_array = palloc(sizeof(char) * tupType->natts);
					for (attnum = 0; attnum < tupType->natts; attnum++)
						null_array[attnum] = 'n';
					oneTuple = heap_formtuple(tupType, tupValue, null_array);
					pfree(null_array);
				}
				break;
			}

			aggno = -1;
			foreach(alist, node->aggs)
			{
				Aggref	   *aggref = lfirst(alist);
				AggFuncInfo *aggfns = &aggFuncInfo[++aggno];
				Datum		newVal;
				Datum		args[2];

				/* Do we really need the special case for Var here? */
				if (IsA(aggref->target, Var))
				{
					newVal = aggGetAttr(outerslot, aggref,
										&isNull);
				}
				else
				{
					econtext->ecxt_scantuple = outerslot;
					newVal = ExecEvalExpr(aggref->target, econtext,
										  &isNull, &isDone);
				}

				if (isNull && !aggref->usenulls)
					continue;	/* ignore this tuple for this agg */

				if (aggfns->xfn1.fn_addr != NULL)
				{
					if (noInitValue[aggno])
					{

						/*
						 * value1 has not been initialized. This is the
						 * first non-NULL input value. We use it as the
						 * initial value for value1.
						 *
						 * But we can't just use it straight, we have to make
						 * a copy of it since the tuple from which it came
						 * will be freed on the next iteration of the
						 * scan.  This requires finding out how to copy
						 * the Datum.  We assume the datum is of the agg's
						 * basetype, or at least binary compatible with
						 * it.
						 */
						Type		aggBaseType = typeidType(aggref->basetype);
						int			attlen = typeLen(aggBaseType);
						bool		byVal = typeByVal(aggBaseType);

						if (byVal)
							value1[aggno] = newVal;
						else
						{
							if (attlen == -1)	/* variable length */
								attlen = VARSIZE((struct varlena *) newVal);
							value1[aggno] = (Datum) palloc(attlen);
							memcpy((char *) (value1[aggno]), (char *) newVal,
								   attlen);
						}
						noInitValue[aggno] = 0;
						nulls[aggno] = 0;
					}
					else
					{

						/*
						 * apply the transition functions.
						 */
						args[0] = value1[aggno];
						args[1] = newVal;
						value1[aggno] = (Datum) fmgr_c(&aggfns->xfn1,
										  (FmgrValues *) args, &isNull1);
						Assert(!isNull1);
					}
				}

				if (aggfns->xfn2.fn_addr != NULL)
				{
					args[0] = value2[aggno];
					value2[aggno] = (Datum) fmgr_c(&aggfns->xfn2,
										  (FmgrValues *) args, &isNull2);
					Assert(!isNull2);
				}
			}

			/*
			 * keep this for the projection (we only need one of these -
			 * all the tuples we aggregate over share the same group
			 * column)
			 */
			if (!oneTuple)
				oneTuple = heap_copytuple(outerslot->val);

			nTuplesAgged++;
		}

		/* --------------
		 * finalize the aggregate (if necessary), and get the resultant value
		 * --------------
		 */

		aggno = -1;
		foreach(alist, node->aggs)
		{
			char	   *args[2];
			AggFuncInfo *aggfns = &aggFuncInfo[++aggno];

			if (noInitValue[aggno])
			{

				/*
				 * No values found for this agg; return current state.
				 * This seems to fix behavior for avg() aggregate. -tgl
				 * 12/96
				 */
			}
			else if (aggfns->finalfn.fn_addr != NULL && nTuplesAgged > 0)
			{
				if (aggfns->finalfn.fn_nargs > 1)
				{
					args[0] = (char *) value1[aggno];
					args[1] = (char *) value2[aggno];
				}
				else if (aggfns->xfn1.fn_addr != NULL)
					args[0] = (char *) value1[aggno];
				else if (aggfns->xfn2.fn_addr != NULL)
					args[0] = (char *) value2[aggno];
				else
					elog(NOTICE, "ExecAgg: no valid transition functions??");
				value1[aggno] = (Datum) fmgr_c(&aggfns->finalfn,
								   (FmgrValues *) args, &(nulls[aggno]));
			}
			else if (aggfns->xfn1.fn_addr != NULL)
			{

				/*
				 * value in the right place, ignore. (If you remove this
				 * case, fix the else part. -ay 2/95)
				 */
			}
			else if (aggfns->xfn2.fn_addr != NULL)
				value1[aggno] = value2[aggno];
			else
				elog(ERROR, "ExecAgg: no valid transition functions??");
		}

		/*
		 * whether the aggregation is done depends on whether we are doing
		 * aggregation over groups or the entire table
		 */
		if (nodeTag(outerPlan) == T_Group)
		{
			/* aggregation over groups */
			aggstate->agg_done = ((Group *) outerPlan)->grpstate->grp_done;
		}
		else
			aggstate->agg_done = TRUE;

		/* ----------------
		 *	form a projection tuple, store it in the result tuple
		 *	slot and return it.
		 * ----------------
		 */

		ExecStoreTuple(oneTuple,
					   aggstate->csstate.css_ScanTupleSlot,
					   InvalidBuffer,
					   false);
		econtext->ecxt_scantuple = aggstate->csstate.css_ScanTupleSlot;

		resultSlot = ExecProject(projInfo, &isDone);

		/*
		 * As long as the retrieved group does not match the
		 * qualifications it is ignored and the next group is fetched
		 */
		if (node->plan.qual != NULL)
			qual_result = ExecQual(fix_opids(node->plan.qual), econtext);
		else
			qual_result = false;

		if (oneTuple)
			pfree(oneTuple);
	}
	while (node->plan.qual != NULL && qual_result != true);

	return resultSlot;
}

/* -----------------
 * ExecInitAgg
 *
 *	Creates the run-time information for the agg node produced by the
 *	planner and initializes its outer subtree
 * -----------------
 */
bool
ExecInitAgg(Agg *node, EState *estate, Plan *parent)
{
	AggState   *aggstate;
	Plan	   *outerPlan;
	ExprContext *econtext;

	/*
	 * assign the node's execution state
	 */
	node->plan.state = estate;

	/*
	 * create state structure
	 */
	aggstate = makeNode(AggState);
	node->aggstate = aggstate;
	aggstate->agg_done = FALSE;

	/*
	 * assign node's base id and create expression context
	 */
	ExecAssignNodeBaseInfo(estate, &aggstate->csstate.cstate, (Plan *) parent);
	ExecAssignExprContext(estate, &aggstate->csstate.cstate);

#define AGG_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitScanTupleSlot(estate, &aggstate->csstate);
	ExecInitResultTupleSlot(estate, &aggstate->csstate.cstate);

	econtext = aggstate->csstate.cstate.cs_ExprContext;
	econtext->ecxt_values = (Datum *) palloc(sizeof(Datum) * length(node->aggs));
	MemSet(econtext->ecxt_values, 0, sizeof(Datum) * length(node->aggs));
	econtext->ecxt_nulls = (char *) palloc(sizeof(char) * length(node->aggs));
	MemSet(econtext->ecxt_nulls, 0, sizeof(char) * length(node->aggs));

	/*
	 * initializes child nodes
	 */
	outerPlan = outerPlan(node);
	ExecInitNode(outerPlan, estate, (Plan *) node);

	/*
	 * Result runs in its own context, but make it use our aggregates fix
	 * for 'select sum(2+2)'
	 */
	if (nodeTag(outerPlan) == T_Result)
	{
		((Result *) outerPlan)->resstate->cstate.cs_ProjInfo->pi_exprContext->ecxt_values =
			econtext->ecxt_values;
		((Result *) outerPlan)->resstate->cstate.cs_ProjInfo->pi_exprContext->ecxt_nulls =
			econtext->ecxt_nulls;
	}


	/* ----------------
	 *	initialize tuple type.
	 * ----------------
	 */
	ExecAssignScanTypeFromOuterPlan((Plan *) node, &aggstate->csstate);

	/*
	 * Initialize tuple type for both result and scan. This node does no
	 * projection
	 */
	ExecAssignResultTypeFromTL((Plan *) node, &aggstate->csstate.cstate);
	ExecAssignProjectionInfo((Plan *) node, &aggstate->csstate.cstate);

	return TRUE;
}

int
ExecCountSlotsAgg(Agg *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
	ExecCountSlotsNode(innerPlan(node)) +
	AGG_NSLOTS;
}

/* ------------------------
 *		ExecEndAgg(node)
 *
 * -----------------------
 */
void
ExecEndAgg(Agg *node)
{
	AggState   *aggstate;
	Plan	   *outerPlan;

	aggstate = node->aggstate;

	ExecFreeProjectionInfo(&aggstate->csstate.cstate);

	outerPlan = outerPlan(node);
	ExecEndNode(outerPlan, (Plan *) node);

	/* clean up tuple table */
	ExecClearTuple(aggstate->csstate.css_ScanTupleSlot);
}


/*****************************************************************************
 *	Support Routines
 *****************************************************************************/

/*
 * aggGetAttr -
 *	  get the attribute (specified in the Var node in agg) to aggregate
 *	  over from the tuple
 */
static Datum
aggGetAttr(TupleTableSlot *slot,
		   Aggref *aggref,
		   bool *isNull)
{
	Datum		result;
	AttrNumber	attnum;
	HeapTuple	heapTuple;
	TupleDesc	tuple_type;
	Buffer		buffer;

	/* ----------------
	 *	 extract tuple information from the slot
	 * ----------------
	 */
	heapTuple = slot->val;
	tuple_type = slot->ttc_tupleDescriptor;
	buffer = slot->ttc_buffer;

	attnum = ((Var *) aggref->target)->varattno;

	/*
	 * If the attribute number is invalid, then we are supposed to return
	 * the entire tuple, we give back a whole slot so that callers know
	 * what the tuple looks like.
	 */
	if (attnum == InvalidAttrNumber)
	{
		TupleTableSlot *tempSlot;
		TupleDesc	td;
		HeapTuple	tup;

		tempSlot = makeNode(TupleTableSlot);
		tempSlot->ttc_shouldFree = false;
		tempSlot->ttc_descIsNew = true;
		tempSlot->ttc_tupleDescriptor = (TupleDesc) NULL;
		tempSlot->ttc_buffer = InvalidBuffer;
		tempSlot->ttc_whichplan = -1;

		tup = heap_copytuple(heapTuple);
		td = CreateTupleDescCopy(slot->ttc_tupleDescriptor);

		ExecSetSlotDescriptor(tempSlot, td);

		ExecStoreTuple(tup, tempSlot, InvalidBuffer, true);
		return (Datum) tempSlot;
	}

	result = heap_getattr(heapTuple,	/* tuple containing attribute */
						  attnum,		/* attribute number of desired
										 * attribute */
						  tuple_type,	/* tuple descriptor of tuple */
						  isNull);		/* return: is attribute null? */

	/* ----------------
	 *	return null if att is null
	 * ----------------
	 */
	if (*isNull)
		return (Datum) NULL;

	return result;
}

void
ExecReScanAgg(Agg *node, ExprContext *exprCtxt, Plan *parent)
{
	AggState   *aggstate = node->aggstate;
	ExprContext *econtext = aggstate->csstate.cstate.cs_ExprContext;

	aggstate->agg_done = FALSE;
	MemSet(econtext->ecxt_values, 0, sizeof(Datum) * length(node->aggs));
	MemSet(econtext->ecxt_nulls, 0, sizeof(char) * length(node->aggs));

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((Plan *) node)->lefttree->chgParam == NULL)
		ExecReScan(((Plan *) node)->lefttree, exprCtxt, (Plan *) node);

}
