/*-------------------------------------------------------------------------
 *
 * nodeSubplan.c
 *	  routines to support subselects
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/executor/nodeSubplan.c,v 1.42 2003/01/10 21:08:08 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		ExecSubPlan  - process a subselect
 *		ExecInitSubPlan - initialize a subselect
 *		ExecEndSubPlan	- shut down a subselect
 */
#include "postgres.h"

#include "access/heapam.h"
#include "executor/executor.h"
#include "executor/nodeSubplan.h"
#include "tcop/pquery.h"


/* ----------------------------------------------------------------
 *		ExecSubPlan(node)
 * ----------------------------------------------------------------
 */
Datum
ExecSubPlan(SubPlanState *node,
			ExprContext *econtext,
			bool *isNull)
{
	SubPlan	   *subplan = (SubPlan *) node->xprstate.expr;
	PlanState  *planstate = node->planstate;
	SubLinkType subLinkType = subplan->subLinkType;
	bool		useOr = subplan->useOr;
	MemoryContext oldcontext;
	TupleTableSlot *slot;
	Datum		result;
	bool		found = false;	/* TRUE if got at least one subplan tuple */
	List	   *pvar;
	List	   *lst;

	/*
	 * We are probably in a short-lived expression-evaluation context.
	 * Switch to the child plan's per-query context for manipulating its
	 * chgParam, calling ExecProcNode on it, etc.
	 */
	oldcontext = MemoryContextSwitchTo(node->sub_estate->es_query_cxt);

	if (subplan->setParam != NIL)
		elog(ERROR, "ExecSubPlan: can't set parent params from subquery");

	/*
	 * Set Params of this plan from parent plan correlation Vars
	 */
	pvar = node->args;
	if (subplan->parParam != NIL)
	{
		foreach(lst, subplan->parParam)
		{
			ParamExecData *prm;

			prm = &(econtext->ecxt_param_exec_vals[lfirsti(lst)]);
			Assert(pvar != NIL);
			prm->value = ExecEvalExprSwitchContext((ExprState *) lfirst(pvar),
												   econtext,
												   &(prm->isnull),
												   NULL);
			pvar = lnext(pvar);
		}
		planstate->chgParam = nconc(planstate->chgParam,
									listCopy(subplan->parParam));
	}
	Assert(pvar == NIL);

	ExecReScan(planstate, NULL);

	/*
	 * For all sublink types except EXPR_SUBLINK, the result is boolean as
	 * are the results of the combining operators.	We combine results
	 * within a tuple (if there are multiple columns) using OR semantics
	 * if "useOr" is true, AND semantics if not.  We then combine results
	 * across tuples (if the subplan produces more than one) using OR
	 * semantics for ANY_SUBLINK or AND semantics for ALL_SUBLINK.
	 * (MULTIEXPR_SUBLINK doesn't allow multiple tuples from the subplan.)
	 * NULL results from the combining operators are handled according to
	 * the usual SQL semantics for OR and AND.	The result for no input
	 * tuples is FALSE for ANY_SUBLINK, TRUE for ALL_SUBLINK, NULL for
	 * MULTIEXPR_SUBLINK.
	 *
	 * For EXPR_SUBLINK we require the subplan to produce no more than one
	 * tuple, else an error is raised.	If zero tuples are produced, we
	 * return NULL.  Assuming we get a tuple, we just return its first
	 * column (there can be only one non-junk column in this case).
	 */
	result = BoolGetDatum(subLinkType == ALL_SUBLINK);
	*isNull = false;

	for (slot = ExecProcNode(planstate);
		 !TupIsNull(slot);
		 slot = ExecProcNode(planstate))
	{
		HeapTuple	tup = slot->val;
		TupleDesc	tdesc = slot->ttc_tupleDescriptor;
		Datum		rowresult = BoolGetDatum(!useOr);
		bool		rownull = false;
		int			col = 1;
		List	   *plst;

		if (subLinkType == EXISTS_SUBLINK)
		{
			found = true;
			result = BoolGetDatum(true);
			break;
		}

		if (subLinkType == EXPR_SUBLINK)
		{
			/* cannot allow multiple input tuples for EXPR sublink */
			if (found)
				elog(ERROR, "More than one tuple returned by a subselect used as an expression.");
			found = true;

			/*
			 * We need to copy the subplan's tuple in case the result is
			 * of pass-by-ref type --- our return value will point into
			 * this copied tuple!  Can't use the subplan's instance of the
			 * tuple since it won't still be valid after next
			 * ExecProcNode() call. node->curTuple keeps track of the
			 * copied tuple for eventual freeing.
			 */
			MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
			tup = heap_copytuple(tup);
			if (node->curTuple)
				heap_freetuple(node->curTuple);
			node->curTuple = tup;
			MemoryContextSwitchTo(node->sub_estate->es_query_cxt);

			result = heap_getattr(tup, col, tdesc, isNull);
			/* keep scanning subplan to make sure there's only one tuple */
			continue;
		}

		/* cannot allow multiple input tuples for MULTIEXPR sublink either */
		if (subLinkType == MULTIEXPR_SUBLINK && found)
			elog(ERROR, "More than one tuple returned by a subselect used as an expression.");

		found = true;

		/*
		 * For ALL, ANY, and MULTIEXPR sublinks, iterate over combining
		 * operators for columns of tuple.
		 */
		plst = subplan->paramIds;
		foreach(lst, node->exprs)
		{
			ExprState  *exprstate = (ExprState *) lfirst(lst);
			int			paramid = lfirsti(plst);
			ParamExecData *prmdata;
			Datum		expresult;
			bool		expnull;

			/*
			 * Load up the Param representing this column of the sub-select.
			 */
			prmdata = &(econtext->ecxt_param_exec_vals[paramid]);
			Assert(prmdata->execPlan == NULL);
			prmdata->value = heap_getattr(tup, col, tdesc,
										  &(prmdata->isnull));

			/*
			 * Now we can eval the combining operator for this column.
			 */
			expresult = ExecEvalExprSwitchContext(exprstate, econtext,
												  &expnull, NULL);

			/*
			 * Combine the result into the row result as appropriate.
			 */
			if (col == 1)
			{
				rowresult = expresult;
				rownull = expnull;
			}
			else if (useOr)
			{
				/* combine within row per OR semantics */
				if (expnull)
					rownull = true;
				else if (DatumGetBool(expresult))
				{
					rowresult = BoolGetDatum(true);
					rownull = false;
					break;		/* needn't look at any more columns */
				}
			}
			else
			{
				/* combine within row per AND semantics */
				if (expnull)
					rownull = true;
				else if (!DatumGetBool(expresult))
				{
					rowresult = BoolGetDatum(false);
					rownull = false;
					break;		/* needn't look at any more columns */
				}
			}

			plst = lnext(plst);
			col++;
		}

		if (subLinkType == ANY_SUBLINK)
		{
			/* combine across rows per OR semantics */
			if (rownull)
				*isNull = true;
			else if (DatumGetBool(rowresult))
			{
				result = BoolGetDatum(true);
				*isNull = false;
				break;			/* needn't look at any more rows */
			}
		}
		else if (subLinkType == ALL_SUBLINK)
		{
			/* combine across rows per AND semantics */
			if (rownull)
				*isNull = true;
			else if (!DatumGetBool(rowresult))
			{
				result = BoolGetDatum(false);
				*isNull = false;
				break;			/* needn't look at any more rows */
			}
		}
		else
		{
			/* must be MULTIEXPR_SUBLINK */
			result = rowresult;
			*isNull = rownull;
		}
	}

	if (!found)
	{
		/*
		 * deal with empty subplan result.	result/isNull were previously
		 * initialized correctly for all sublink types except EXPR and
		 * MULTIEXPR; for those, return NULL.
		 */
		if (subLinkType == EXPR_SUBLINK || subLinkType == MULTIEXPR_SUBLINK)
		{
			result = (Datum) 0;
			*isNull = true;
		}
	}

	MemoryContextSwitchTo(oldcontext);

	return result;
}

/* ----------------------------------------------------------------
 *		ExecInitSubPlan
 * ----------------------------------------------------------------
 */
void
ExecInitSubPlan(SubPlanState *node, EState *estate)
{
	SubPlan	   *subplan = (SubPlan *) node->xprstate.expr;
	EState	   *sp_estate;
	MemoryContext oldcontext;

	/*
	 * Do access checking on the rangetable entries in the subquery.
	 * Here, we assume the subquery is a SELECT.
	 */
	ExecCheckRTPerms(subplan->rtable, CMD_SELECT);

	/*
	 * initialize my state
	 */
	node->needShutdown = false;
	node->curTuple = NULL;
	node->hashtable = NULL;
	node->hashnulls = NULL;

	/*
	 * create an EState for the subplan
	 *
	 * The subquery needs its own EState because it has its own rangetable.
	 * It shares our Param ID space, however.  XXX if rangetable access were
	 * done differently, the subquery could share our EState, which would
	 * eliminate some thrashing about in this module...
	 */
	sp_estate = CreateExecutorState();
	node->sub_estate = sp_estate;

	oldcontext = MemoryContextSwitchTo(sp_estate->es_query_cxt);

	sp_estate->es_range_table = subplan->rtable;
	sp_estate->es_param_list_info = estate->es_param_list_info;
	sp_estate->es_param_exec_vals = estate->es_param_exec_vals;
	sp_estate->es_tupleTable =
		ExecCreateTupleTable(ExecCountSlotsNode(subplan->plan) + 10);
	sp_estate->es_snapshot = estate->es_snapshot;
	sp_estate->es_instrument = estate->es_instrument;

	/*
	 * Start up the subplan (this is a very cut-down form of InitPlan())
	 */
	node->planstate = ExecInitNode(subplan->plan, sp_estate);

	node->needShutdown = true;	/* now we need to shutdown the subplan */

	MemoryContextSwitchTo(oldcontext);

	/*
	 * If this plan is un-correlated or undirect correlated one and want
	 * to set params for parent plan then prepare parameters.
	 */
	if (subplan->setParam != NIL)
	{
		List	   *lst;

		foreach(lst, subplan->setParam)
		{
			ParamExecData *prm = &(estate->es_param_exec_vals[lfirsti(lst)]);

			prm->execPlan = node;
		}

		/*
		 * Note that in the case of un-correlated subqueries we don't care
		 * about setting parent->chgParam here: indices take care about
		 * it, for others - it doesn't matter...
		 */
	}
}

/* ----------------------------------------------------------------
 *		ExecSetParamPlan
 *
 *		Executes an InitPlan subplan and sets its output parameters.
 *
 * This is called from ExecEvalParam() when the value of a PARAM_EXEC
 * parameter is requested and the param's execPlan field is set (indicating
 * that the param has not yet been evaluated).	This allows lazy evaluation
 * of initplans: we don't run the subplan until/unless we need its output.
 * Note that this routine MUST clear the execPlan fields of the plan's
 * output parameters after evaluating them!
 * ----------------------------------------------------------------
 */
void
ExecSetParamPlan(SubPlanState *node, ExprContext *econtext)
{
	SubPlan	   *subplan = (SubPlan *) node->xprstate.expr;
	PlanState  *planstate = node->planstate;
	SubLinkType	subLinkType = subplan->subLinkType;
	MemoryContext oldcontext;
	TupleTableSlot *slot;
	List	   *lst;
	bool		found = false;

	/*
	 * Must switch to child query's per-query memory context.
	 */
	oldcontext = MemoryContextSwitchTo(node->sub_estate->es_query_cxt);

	if (subLinkType == ANY_SUBLINK ||
		subLinkType == ALL_SUBLINK)
		elog(ERROR, "ExecSetParamPlan: ANY/ALL subselect unsupported");

	if (planstate->chgParam != NULL)
		ExecReScan(planstate, NULL);

	for (slot = ExecProcNode(planstate);
		 !TupIsNull(slot);
		 slot = ExecProcNode(planstate))
	{
		HeapTuple	tup = slot->val;
		TupleDesc	tdesc = slot->ttc_tupleDescriptor;
		int			i = 1;

		if (subLinkType == EXISTS_SUBLINK)
		{
			ParamExecData *prm = &(econtext->ecxt_param_exec_vals[lfirsti(subplan->setParam)]);

			prm->execPlan = NULL;
			prm->value = BoolGetDatum(true);
			prm->isnull = false;
			found = true;
			break;
		}

		if (found &&
			(subLinkType == EXPR_SUBLINK ||
			 subLinkType == MULTIEXPR_SUBLINK))
			elog(ERROR, "More than one tuple returned by a subselect used as an expression.");

		found = true;

		/*
		 * We need to copy the subplan's tuple into our own context,
		 * in case any of the params are pass-by-ref type --- the pointers
		 * stored in the param structs will point at this copied tuple!
		 * node->curTuple keeps track of the copied tuple for eventual
		 * freeing.
		 */
		MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
		tup = heap_copytuple(tup);
		if (node->curTuple)
			heap_freetuple(node->curTuple);
		node->curTuple = tup;
		MemoryContextSwitchTo(node->sub_estate->es_query_cxt);

		foreach(lst, subplan->setParam)
		{
			ParamExecData *prm = &(econtext->ecxt_param_exec_vals[lfirsti(lst)]);

			prm->execPlan = NULL;
			prm->value = heap_getattr(tup, i, tdesc, &(prm->isnull));
			i++;
		}
	}

	if (!found)
	{
		if (subLinkType == EXISTS_SUBLINK)
		{
			ParamExecData *prm = &(econtext->ecxt_param_exec_vals[lfirsti(subplan->setParam)]);

			prm->execPlan = NULL;
			prm->value = BoolGetDatum(false);
			prm->isnull = false;
		}
		else
		{
			foreach(lst, subplan->setParam)
			{
				ParamExecData *prm = &(econtext->ecxt_param_exec_vals[lfirsti(lst)]);

				prm->execPlan = NULL;
				prm->value = (Datum) 0;
				prm->isnull = true;
			}
		}
	}

	MemoryContextSwitchTo(oldcontext);
}

/* ----------------------------------------------------------------
 *		ExecEndSubPlan
 * ----------------------------------------------------------------
 */
void
ExecEndSubPlan(SubPlanState *node)
{
	if (node->needShutdown)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(node->sub_estate->es_query_cxt);
		ExecEndPlan(node->planstate, node->sub_estate);
		MemoryContextSwitchTo(oldcontext);
		FreeExecutorState(node->sub_estate);
		node->sub_estate = NULL;
		node->planstate = NULL;
		node->needShutdown = false;
	}
	if (node->curTuple)
	{
		heap_freetuple(node->curTuple);
		node->curTuple = NULL;
	}
}

void
ExecReScanSetParamPlan(SubPlanState *node, PlanState *parent)
{
	PlanState  *planstate = node->planstate;
	SubPlan	   *subplan = (SubPlan *) node->xprstate.expr;
	EState	   *estate = parent->state;
	List	   *lst;

	if (subplan->parParam != NULL)
		elog(ERROR, "ExecReScanSetParamPlan: direct correlated subquery unsupported, yet");
	if (subplan->setParam == NULL)
		elog(ERROR, "ExecReScanSetParamPlan: setParam list is NULL");
	if (planstate->plan->extParam == NULL)
		elog(ERROR, "ExecReScanSetParamPlan: extParam list of plan is NULL");

	/*
	 * Don't actually re-scan: ExecSetParamPlan does it if needed.
	 */

	/*
	 * Mark this subplan's output parameters as needing recalculation
	 */
	foreach(lst, subplan->setParam)
	{
		ParamExecData *prm = &(estate->es_param_exec_vals[lfirsti(lst)]);

		prm->execPlan = node;
	}

	parent->chgParam = nconc(parent->chgParam, listCopy(subplan->setParam));
}
