/*-------------------------------------------------------------------------
 *
 * nodeSubplan.c
 *	  routines to support subselects
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
 *
 * ----------------------------------------------------------------
 */
Datum
ExecSubPlan(SubPlan *node, List *pvar, ExprContext *econtext)
{
	Plan	   *plan = node->plan;
	SubLink    *sublink = node->sublink;
	SubLinkType subLinkType = sublink->subLinkType;
	TupleTableSlot *slot;
	List	   *lst;
	Datum		result = (Datum) false;
	bool		found = false;	/* TRUE if got at least one subplan tuple */

	if (node->setParam != NULL)
		elog(ERROR, "ExecSubPlan: can't set parent params from subquery");

	/*
	 * Set Params of this plan from parent plan correlation Vars
	 */
	if (node->parParam != NULL)
	{
		foreach(lst, node->parParam)
		{
			ParamExecData *prm = &(econtext->ecxt_param_exec_vals[lfirsti(lst)]);

			prm->value = ExecEvalExpr((Node *) lfirst(pvar),
									  econtext,
									  &(prm->isnull), NULL);
			pvar = lnext(pvar);
		}
		plan->chgParam = nconc(plan->chgParam, listCopy(node->parParam));
	}

	ExecReScan(plan, (ExprContext *) NULL, plan);

	/*
	 * For all sublink types except EXPR_SUBLINK, the result type is
	 * boolean, and we have a fairly clear idea of how to combine multiple
	 * subitems and deal with NULL values or an empty subplan result.
	 *
	 * For EXPR_SUBLINK, the result type is whatever the combining operator
	 * returns.  We have no way to deal with more than one column in the
	 * subplan result --- hopefully the parser forbids that.  More
	 * seriously, it's unclear what to do with NULL values or an empty
	 * subplan result. For now, we error out, but should something else
	 * happen?
	 */

	for (slot = ExecProcNode(plan, plan);
		 !TupIsNull(slot);
		 slot = ExecProcNode(plan, plan))
	{
		HeapTuple	tup = slot->val;
		TupleDesc	tdesc = slot->ttc_tupleDescriptor;
		int			i = 1;

		if (subLinkType == EXPR_SUBLINK && found)
		{
			elog(ERROR, "ExecSubPlan: more than one tuple returned by expression subselect");
			return (Datum) false;
		}

		if (subLinkType == EXISTS_SUBLINK)
			return (Datum) true;

		found = true;

		foreach(lst, sublink->oper)
		{
			Expr	   *expr = (Expr *) lfirst(lst);
			Const	   *con = lsecond(expr->args);
			bool		isnull;

			/*
			 * The righthand side of the expression should be either a Const
			 * or a function call taking a Const as arg (the function would
			 * be a run-time type coercion inserted by the parser to get to
			 * the input type needed by the operator).  Find the Const node
			 * and insert the actual righthand side value into it.
			 */
			if (! IsA(con, Const))
			{
				Assert(IsA(con, Expr));
				con = lfirst(((Expr *) con)->args);
				Assert(IsA(con, Const));
			}
			con->constvalue = heap_getattr(tup, i, tdesc, &(con->constisnull));
			/*
			 * Now we can eval the expression.
			 */
			result = ExecEvalExpr((Node *) expr, econtext, &isnull,
								  (bool *) NULL);
			if (isnull)
			{
				if (subLinkType == EXPR_SUBLINK)
					elog(ERROR, "ExecSubPlan: null value returned by expression subselect");
				else
					result = (Datum) false;
			}
			if (subLinkType != EXPR_SUBLINK)
			{
				if ((!(bool) result && !(sublink->useor)) ||
					((bool) result && sublink->useor))
					break;
			}
			i++;
		}

		if (subLinkType == ALL_SUBLINK && !(bool) result)
			break;
		if (subLinkType == ANY_SUBLINK && (bool) result)
			break;
	}

	if (!found)
	{
		/* deal with empty subplan result.	Note default result is 'false' */
		if (subLinkType == ALL_SUBLINK)
			result = (Datum) true;
		else if (subLinkType == EXPR_SUBLINK)
			elog(ERROR, "ExecSubPlan: no tuples returned by expression subselect");
	}

	return result;
}

/* ----------------------------------------------------------------
 *		ExecInitSubPlan
 *
 * ----------------------------------------------------------------
 */
extern void ExecCheckPerms(CmdType op, int resRel, List *rtable, Query *q);
bool
ExecInitSubPlan(SubPlan *node, EState *estate, Plan *parent)
{
	EState	   *sp_estate = CreateExecutorState();

	ExecCheckPerms(CMD_SELECT, 0, node->rtable, (Query *) NULL);

	sp_estate->es_range_table = node->rtable;
	sp_estate->es_param_list_info = estate->es_param_list_info;
	sp_estate->es_param_exec_vals = estate->es_param_exec_vals;
	sp_estate->es_tupleTable =
		ExecCreateTupleTable(ExecCountSlotsNode(node->plan) + 10);
	sp_estate->es_snapshot = estate->es_snapshot;

	if (!ExecInitNode(node->plan, sp_estate, NULL))
		return false;

	node->shutdown = true;

	/*
	 * If this plan is un-correlated or undirect correlated one and want
	 * to set params for parent plan then prepare parameters.
	 */
	if (node->setParam != NULL)
	{
		List	   *lst;

		foreach(lst, node->setParam)
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

	return true;
}

/* ----------------------------------------------------------------
 *		ExecSetParamPlan
 *
 *		Executes plan of node and sets parameters.
 * ----------------------------------------------------------------
 */
void
ExecSetParamPlan(SubPlan *node)
{
	Plan	   *plan = node->plan;
	SubLink    *sublink = node->sublink;
	TupleTableSlot *slot;
	List	   *lst;
	bool		found = false;

	if (sublink->subLinkType == ANY_SUBLINK ||
		sublink->subLinkType == ALL_SUBLINK)
		elog(ERROR, "ExecSetParamPlan: ANY/ALL subselect unsupported");

	if (plan->chgParam != NULL)
		ExecReScan(plan, (ExprContext *) NULL, plan);

	for (slot = ExecProcNode(plan, plan);
		 !TupIsNull(slot);
		 slot = ExecProcNode(plan, plan))
	{
		HeapTuple	tup = slot->val;
		TupleDesc	tdesc = slot->ttc_tupleDescriptor;
		int			i = 1;

		if (sublink->subLinkType == EXPR_SUBLINK && found)
		{
			elog(ERROR, "ExecSetParamPlan: more than one tuple returned by expression subselect");
			return;
		}

		found = true;

		if (sublink->subLinkType == EXISTS_SUBLINK)
		{
			ParamExecData *prm = &(plan->state->es_param_exec_vals[lfirsti(node->setParam)]);

			prm->execPlan = NULL;
			prm->value = (Datum) true;
			prm->isnull = false;
			break;
		}

		/*
		 * If this is uncorrelated subquery then its plan will be closed
		 * (see below) and this tuple will be free-ed - bad for not byval
		 * types... But is free-ing possible in the next ExecProcNode in
		 * this loop ? Who knows... Someday we'll keep track of saved
		 * tuples...
		 */
		tup = heap_copytuple(tup);

		foreach(lst, node->setParam)
		{
			ParamExecData *prm = &(plan->state->es_param_exec_vals[lfirsti(lst)]);

			prm->execPlan = NULL;
			prm->value = heap_getattr(tup, i, tdesc, &(prm->isnull));
			i++;
		}
	}

	if (!found)
	{
		if (sublink->subLinkType == EXISTS_SUBLINK)
		{
			ParamExecData *prm = &(plan->state->es_param_exec_vals[lfirsti(node->setParam)]);

			prm->execPlan = NULL;
			prm->value = (Datum) false;
			prm->isnull = false;
		}
		else
		{
			foreach(lst, node->setParam)
			{
				ParamExecData *prm = &(plan->state->es_param_exec_vals[lfirsti(lst)]);

				prm->execPlan = NULL;
				prm->value = (Datum) NULL;
				prm->isnull = true;
			}
		}
	}

	if (plan->extParam == NULL) /* un-correlated ... */
	{
		ExecEndNode(plan, plan);
		node->shutdown = false;
	}
}

/* ----------------------------------------------------------------
 *		ExecEndSubPlan
 * ----------------------------------------------------------------
 */
void
ExecEndSubPlan(SubPlan *node)
{

	if (node->shutdown)
	{
		ExecEndNode(node->plan, node->plan);
		node->shutdown = false;
	}

}

void
ExecReScanSetParamPlan(SubPlan *node, Plan *parent)
{
	Plan	   *plan = node->plan;
	List	   *lst;

	if (node->parParam != NULL)
		elog(ERROR, "ExecReScanSetParamPlan: direct correlated subquery unsupported, yet");
	if (node->setParam == NULL)
		elog(ERROR, "ExecReScanSetParamPlan: setParam list is NULL");
	if (plan->extParam == NULL)
		elog(ERROR, "ExecReScanSetParamPlan: extParam list of plan is NULL");

	/*
	 * Don't actual re-scan: ExecSetParamPlan does re-scan if
	 * node->plan->chgParam is not NULL... ExecReScan (plan, NULL, plan);
	 */

	foreach(lst, node->setParam)
	{
		ParamExecData *prm = &(plan->state->es_param_exec_vals[lfirsti(lst)]);

		prm->execPlan = node;
	}

	parent->chgParam = nconc(parent->chgParam, listCopy(node->setParam));

}
