/*-------------------------------------------------------------------------
 *
 * nodeAgg.c--
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
#include "fmgr.h"

#include "access/heapam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/catalog.h"
#include "parser/parse_type.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "storage/bufmgr.h"
#include "utils/palloc.h"
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
	func_ptr	xfn1;
	func_ptr	xfn2;
	func_ptr	finalfn;
	int			xfn1_nargs;
	int			xfn2_nargs;
	int			finalfn_nargs;
} AggFuncInfo;

static Datum aggGetAttr(TupleTableSlot *tuple, Aggreg *agg, bool *isNull);


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
 *			value1[i] = sfunc1(aggregate_attribute, value1[i])
 *			value2[i] = sfunc2(value2[i])
 *		 value1[i] = finalfunc(value1[i], value2[i])
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
	Aggreg	  **aggregates;
	Plan	   *outerPlan;
	int			i,
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
	char	   *nulls;
	bool		isDone;
	bool		isNull = FALSE,
				isNull1 = FALSE,
				isNull2 = FALSE;

	/* ---------------------
	 *	get state info from node
	 * ---------------------
	 */
	aggstate = node->aggstate;
	if (aggstate->agg_done)
		return NULL;

	estate = node->plan.state;
	econtext = aggstate->csstate.cstate.cs_ExprContext;
	aggregates = node->aggs;
	nagg = node->numAgg;

	value1 = node->aggstate->csstate.cstate.cs_ExprContext->ecxt_values;
	nulls = node->aggstate->csstate.cstate.cs_ExprContext->ecxt_nulls;

	value2 = (Datum *) palloc(sizeof(Datum) * nagg);
	MemSet(value2, 0, sizeof(Datum) * nagg);

	aggFuncInfo = (AggFuncInfo *) palloc(sizeof(AggFuncInfo) * nagg);
	MemSet(aggFuncInfo, 0, sizeof(AggFuncInfo) * nagg);

	noInitValue = (int *) palloc(sizeof(int) * nagg);
	MemSet(noInitValue, 0, sizeof(noInitValue) * nagg);

	outerPlan = outerPlan(node);
	oneTuple = NULL;

	projInfo = aggstate->csstate.cstate.cs_ProjInfo;

	for (i = 0; i < nagg; i++)
	{
		Aggreg	   *agg;
		char	   *aggname;
		HeapTuple	aggTuple;
		Form_pg_aggregate aggp;
		Oid			xfn1_oid,
					xfn2_oid,
					finalfn_oid;
		func_ptr	xfn1_ptr,
					xfn2_ptr,
					finalfn_ptr;
		int			xfn1_nargs,
					xfn2_nargs,
					finalfn_nargs;

		agg = aggregates[i];

		/* ---------------------
		 *	find transfer functions of all the aggregates and initialize
		 *	their initial values
		 * ---------------------
		 */
		aggname = agg->aggname;
		aggTuple = SearchSysCacheTuple(AGGNAME,
									   PointerGetDatum(aggname),
									   ObjectIdGetDatum(agg->basetype),
									   0, 0);
		if (!HeapTupleIsValid(aggTuple))
			elog(WARN, "ExecAgg: cache lookup failed for aggregate \"%s\"(%s)",
				 aggname,
				 typeidTypeName(agg->basetype));
		aggp = (Form_pg_aggregate) GETSTRUCT(aggTuple);

		xfn1_oid = aggp->aggtransfn1;
		xfn2_oid = aggp->aggtransfn2;
		finalfn_oid = aggp->aggfinalfn;

		if (OidIsValid(finalfn_oid))
		{
			fmgr_info(finalfn_oid, &finalfn_ptr, &finalfn_nargs);
			aggFuncInfo[i].finalfn_oid = finalfn_oid;
			aggFuncInfo[i].finalfn = finalfn_ptr;
			aggFuncInfo[i].finalfn_nargs = finalfn_nargs;
		}

		if (OidIsValid(xfn2_oid))
		{
			fmgr_info(xfn2_oid, &xfn2_ptr, &xfn2_nargs);
			aggFuncInfo[i].xfn2_oid = xfn2_oid;
			aggFuncInfo[i].xfn2 = xfn2_ptr;
			aggFuncInfo[i].xfn2_nargs = xfn2_nargs;
			value2[i] = (Datum) AggNameGetInitVal((char *) aggname,
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
				elog(WARN, "ExecAgg: agginitval2 is null");
		}

		if (OidIsValid(xfn1_oid))
		{
			fmgr_info(xfn1_oid, &xfn1_ptr, &xfn1_nargs);
			aggFuncInfo[i].xfn1_oid = xfn1_oid;
			aggFuncInfo[i].xfn1 = xfn1_ptr;
			aggFuncInfo[i].xfn1_nargs = xfn1_nargs;
			value1[i] = (Datum) AggNameGetInitVal((char *) aggname,
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
				noInitValue[i] = 1;
				nulls[i] = 1;
			}
		}
	}

	/* ----------------
	 *	 for each tuple from the the outer plan, apply all the aggregates
	 * ----------------
	 */
	for (;;)
	{
		HeapTuple	outerTuple = NULL;
		TupleTableSlot *outerslot;

		isNull = isNull1 = isNull2 = 0;
		outerslot = ExecProcNode(outerPlan, (Plan *) node);
		if (outerslot)
			outerTuple = outerslot->val;
		if (!HeapTupleIsValid(outerTuple))
		{

			/*
			 * when the outerplan doesn't return a single tuple, create a
			 * dummy heaptuple anyway because we still need to return a
			 * valid aggregate value. The value returned will be the
			 * initial values of the transition functions
			 */
			if (nTuplesAgged == 0)
			{
				TupleDesc	tupType;
				Datum	   *tupValue;
				char	   *null_array;

				tupType = aggstate->csstate.css_ScanTupleSlot->ttc_tupleDescriptor;
				tupValue = projInfo->pi_tupValue;

				/* initially, set all the values to NULL */
				null_array = malloc(tupType->natts);
				for (i = 0; i < tupType->natts; i++)
					null_array[i] = 'n';
				oneTuple = heap_formtuple(tupType, tupValue, null_array);
				free(null_array);
			}
			break;
		}

		for (i = 0; i < nagg; i++)
		{
			AttrNumber	attnum;
			int2		attlen;
			Datum		newVal = (Datum) NULL;
			AggFuncInfo *aggfns = &aggFuncInfo[i];
			Datum		args[2];
			Node	   *tagnode = NULL;

			switch (nodeTag(aggregates[i]->target))
			{
				case T_Var:
					tagnode = NULL;
					newVal = aggGetAttr(outerslot,
										aggregates[i],
										&isNull);
					break;
				case T_Expr:
					tagnode = ((Expr *) aggregates[i]->target)->oper;
					econtext->ecxt_scantuple = outerslot;
					newVal = ExecEvalExpr(aggregates[i]->target, econtext,
										  &isNull, &isDone);
					break;
				default:
					elog(WARN, "ExecAgg: Bad Agg->Target for Agg %d", i);
			}

			if (isNull)
				continue;		/* ignore this tuple for this agg */

			if (aggfns->xfn1)
			{
				if (noInitValue[i])
				{
					int			byVal;

					/*
					 * value1 and value2 has not been initialized. This is
					 * the first non-NULL value. We use it as the initial
					 * value.
					 */

					/*
					 * but we can't just use it straight, we have to make
					 * a copy of it since the tuple from which it came
					 * will be freed on the next iteration of the scan
					 */
					if (tagnode != NULL)
					{
						FunctionCachePtr fcache_ptr;

						if (nodeTag(tagnode) == T_Func)
							fcache_ptr = ((Func *) tagnode)->func_fcache;
						else
							fcache_ptr = ((Oper *) tagnode)->op_fcache;
						attlen = fcache_ptr->typlen;
						byVal = fcache_ptr->typbyval;
					}
					else
					{
						attnum = ((Var *) aggregates[i]->target)->varattno;
						attlen = outerslot->ttc_tupleDescriptor->attrs[attnum - 1]->attlen;
						byVal = outerslot->ttc_tupleDescriptor->attrs[attnum - 1]->attbyval;
					}
					if (attlen == -1)
					{
						/* variable length */
						attlen = VARSIZE((struct varlena *) newVal);
					}
					value1[i] = (Datum) palloc(attlen);
					if (byVal)
						value1[i] = newVal;
					else
						memmove((char *) (value1[i]), (char *) newVal, attlen);
					/* value1[i] = newVal; */
					noInitValue[i] = 0;
					nulls[i] = 0;
				}
				else
				{

					/*
					 * apply the transition functions.
					 */
					args[0] = value1[i];
					args[1] = newVal;
					value1[i] =
						(Datum) fmgr_c(aggfns->xfn1, aggfns->xfn1_oid,
								 aggfns->xfn1_nargs, (FmgrValues *) args,
									   &isNull1);
					Assert(!isNull1);
				}
			}

			if (aggfns->xfn2)
			{
				Datum		xfn2_val = value2[i];

				value2[i] =
					(Datum) fmgr_c(aggfns->xfn2, aggfns->xfn2_oid,
								   aggfns->xfn2_nargs,
								   (FmgrValues *) &xfn2_val, &isNull2);
				Assert(!isNull2);
			}
		}

		/*
		 * keep this for the projection (we only need one of these - all
		 * the tuples we aggregate over share the same group column)
		 */
		if (!oneTuple)
		{
			oneTuple = heap_copytuple(outerslot->val);
		}

		nTuplesAgged++;
	}

	/* --------------
	 * finalize the aggregate (if necessary), and get the resultant value
	 * --------------
	 */
	for (i = 0; i < nagg; i++)
	{
		char	   *args[2];
		AggFuncInfo *aggfns = &aggFuncInfo[i];

		if (noInitValue[i])
		{

			/*
			 * No values found for this agg; return current state. This
			 * seems to fix behavior for avg() aggregate. -tgl 12/96
			 */
		}
		else if (aggfns->finalfn && nTuplesAgged > 0)
		{
			if (aggfns->finalfn_nargs > 1)
			{
				args[0] = (char *) value1[i];
				args[1] = (char *) value2[i];
			}
			else if (aggfns->xfn1)
			{
				args[0] = (char *) value1[i];
			}
			else if (aggfns->xfn2)
			{
				args[0] = (char *) value2[i];
			}
			else
				elog(WARN, "ExecAgg: no valid transition functions??");
			value1[i] =
				(Datum) fmgr_c(aggfns->finalfn, aggfns->finalfn_oid,
							   aggfns->finalfn_nargs, (FmgrValues *) args,
							   &(nulls[i]));
		}
		else if (aggfns->xfn1)
		{

			/*
			 * value in the right place, ignore. (If you remove this case,
			 * fix the else part. -ay 2/95)
			 */
		}
		else if (aggfns->xfn2)
		{
			value1[i] = value2[i];
		}
		else
			elog(WARN, "ExecAgg: no valid transition functions??");
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
	{
		aggstate->agg_done = TRUE;
	}

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

	if (oneTuple)
		pfree(oneTuple);

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
	ExecAssignNodeBaseInfo(estate, &aggstate->csstate.cstate,
						   (Plan *) parent);
	ExecAssignExprContext(estate, &aggstate->csstate.cstate);

#define AGG_NSLOTS 2

	/*
	 * tuple table initialization
	 */
	ExecInitScanTupleSlot(estate, &aggstate->csstate);
	ExecInitResultTupleSlot(estate, &aggstate->csstate.cstate);

	econtext = aggstate->csstate.cstate.cs_ExprContext;
	econtext->ecxt_values =
		(Datum *) palloc(sizeof(Datum) * node->numAgg);
	MemSet(econtext->ecxt_values, 0, sizeof(Datum) * node->numAgg);
	econtext->ecxt_nulls = (char *) palloc(node->numAgg);
	MemSet(econtext->ecxt_nulls, 0, node->numAgg);

	/*
	 * initializes child nodes
	 */
	outerPlan = outerPlan(node);
	ExecInitNode(outerPlan, estate, (Plan *) node);

	/*
	 *	Result runs in its own context, but make it use our aggregates
	 *	fix for 'select sum(2+2)'
	 */
	if (nodeTag(outerPlan) == T_Result)
	{
		((Result *)outerPlan)->resstate->cstate.cs_ProjInfo->pi_exprContext->ecxt_values =
												econtext->ecxt_values;
		((Result *)outerPlan)->resstate->cstate.cs_ProjInfo->pi_exprContext->ecxt_nulls =
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
		   Aggreg *agg,
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

	attnum = ((Var *) agg->target)->varattno;

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
		tempSlot->ttc_tupleDescriptor = (TupleDesc) NULL,
			tempSlot->ttc_buffer = InvalidBuffer;
		tempSlot->ttc_whichplan = -1;

		tup = heap_copytuple(slot->val);
		td = CreateTupleDescCopy(slot->ttc_tupleDescriptor);

		ExecSetSlotDescriptor(tempSlot, td);

		ExecStoreTuple(tup, tempSlot, InvalidBuffer, true);
		return (Datum) tempSlot;
	}

	result = 
		heap_getattr(heapTuple, /* tuple containing attribute */
					 buffer,	/* buffer associated with tuple */
					 attnum,	/* attribute number of desired attribute */
					 tuple_type,/* tuple descriptor of tuple */
					 isNull);	/* return: is attribute null? */

	/* ----------------
	 *	return null if att is null
	 * ----------------
	 */
	if (*isNull)
		return (Datum) NULL;

	return result;
}
