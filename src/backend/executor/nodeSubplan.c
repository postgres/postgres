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

/* should be exported by execMain.c */
extern void ExecCheckPerms(CmdType op, int resRel, List *rtable, Query *q);

/* ----------------------------------------------------------------
 *		ExecSubPlan(node)
 *
 * ----------------------------------------------------------------
 */
Datum
ExecSubPlan(SubPlan *node, List *pvar, ExprContext *econtext, bool *isNull)
{
	Plan	   *plan = node->plan;
	SubLink    *sublink = node->sublink;
	SubLinkType subLinkType = sublink->subLinkType;
	bool		useor = sublink->useor;
	TupleTableSlot *slot;
	Datum		result;
	bool		found = false;	/* TRUE if got at least one subplan tuple */
	List	   *lst;

	if (node->setParam != NIL)
		elog(ERROR, "ExecSubPlan: can't set parent params from subquery");

	/*
	 * Set Params of this plan from parent plan correlation Vars
	 */
	if (node->parParam != NIL)
	{
		foreach(lst, node->parParam)
		{
			ParamExecData *prm = &(econtext->ecxt_param_exec_vals[lfirsti(lst)]);

			Assert(pvar != NIL);
			prm->value = ExecEvalExpr((Node *) lfirst(pvar),
									  econtext,
									  &(prm->isnull), NULL);
			pvar = lnext(pvar);
		}
		plan->chgParam = nconc(plan->chgParam, listCopy(node->parParam));
	}
	Assert(pvar == NIL);

	ExecReScan(plan, (ExprContext *) NULL, plan);

	/*
	 * For all sublink types except EXPR_SUBLINK, the result is boolean
	 * as are the results of the combining operators.  We combine results
	 * within a tuple (if there are multiple columns) using OR semantics
	 * if "useor" is true, AND semantics if not.  We then combine results
	 * across tuples (if the subplan produces more than one) using OR
	 * semantics for ANY_SUBLINK or AND semantics for ALL_SUBLINK.  NULL
	 * results from the combining operators are handled according to the
	 * usual SQL semantics for OR and AND.  The result for no input
	 * tuples is FALSE for ANY_SUBLINK, TRUE for ALL_SUBLINK.
	 *
	 * For EXPR_SUBLINK we require the subplan to produce no more than one
	 * tuple, else an error is raised.  If zero tuples are produced, we
	 * return NULL.  (XXX it would probably be more correct to evaluate
	 * the combining operator with a NULL input?)  Assuming we get a tuple:
	 * if there is only one column then we just return its result as-is, NULL
	 * or otherwise.  If there is more than one column we combine the results
	 * per "useor" --- this only makes sense if the combining operators yield
	 * boolean, and we assume the parser has checked that.
	 */
	result = (Datum) (subLinkType == ALL_SUBLINK ? true : false);
	*isNull = false;

	for (slot = ExecProcNode(plan, plan);
		 !TupIsNull(slot);
		 slot = ExecProcNode(plan, plan))
	{
		HeapTuple	tup = slot->val;
		TupleDesc	tdesc = slot->ttc_tupleDescriptor;
		Datum		rowresult = (Datum) (useor ? false : true);
		bool		rownull = false;
		int			col = 1;

		if (subLinkType == EXISTS_SUBLINK)
			return (Datum) true;

		/* cannot allow multiple input tuples for EXPR sublink */
		if (subLinkType == EXPR_SUBLINK && found)
			elog(ERROR, "ExecSubPlan: more than one tuple returned by expression subselect");

		found = true;

		/* iterate over combining operators for columns of tuple */
		foreach(lst, sublink->oper)
		{
			Expr	   *expr = (Expr *) lfirst(lst);
			Const	   *con = lsecond(expr->args);
			Datum		expresult;
			bool		expnull;

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
			con->constvalue = heap_getattr(tup, col, tdesc,
										   &(con->constisnull));
			/*
			 * Now we can eval the combining operator for this column.
			 */
			expresult = ExecEvalExpr((Node *) expr, econtext, &expnull,
									 (bool *) NULL);
			/*
			 * Combine the result into the row result as appropriate.
			 */
			if (col == 1)
			{
				rowresult = expresult;
				rownull = expnull;
			}
			else if (useor)
			{
				/* combine within row per OR semantics */
				if (expnull)
					rownull = true;
				else if (DatumGetInt32(expresult) != 0)
				{
					rowresult = (Datum) true;
					rownull = false;
					break;		/* needn't look at any more columns */
				}
			}
			else
			{
				/* combine within row per AND semantics */
				if (expnull)
					rownull = true;
				else if (DatumGetInt32(expresult) == 0)
				{
					rowresult = (Datum) false;
					rownull = false;
					break;		/* needn't look at any more columns */
				}
			}
			col++;
		}

		if (subLinkType == ANY_SUBLINK)
		{
			/* combine across rows per OR semantics */
			if (rownull)
				*isNull = true;
			else if (DatumGetInt32(rowresult) != 0)
			{
				result = (Datum) true;
				*isNull = false;
				break;			/* needn't look at any more rows */
			}
		}
		else if (subLinkType == ALL_SUBLINK)
		{
			/* combine across rows per AND semantics */
			if (rownull)
				*isNull = true;
			else if (DatumGetInt32(rowresult) == 0)
			{
				result = (Datum) false;
				*isNull = false;
				break;			/* needn't look at any more rows */
			}
		}
		else
		{
			/* must be EXPR_SUBLINK */
			result = rowresult;
			*isNull = rownull;
		}
	}

	if (!found)
	{
		/* deal with empty subplan result.  result/isNull were previously
		 * initialized correctly for all sublink types except EXPR.
		 */
		if (subLinkType == EXPR_SUBLINK)
		{
				result = (Datum) false;
				*isNull = true;
		}
	}

	return result;
}

/* ----------------------------------------------------------------
 *		ExecInitSubPlan
 *
 * ----------------------------------------------------------------
 */
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
