/*-------------------------------------------------------------------------
 *
 * subselect.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"

#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/relation.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"

#include "optimizer/subselect.h"
#include "optimizer/planner.h"
#include "optimizer/planmain.h"
#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/clauses.h"
#include "optimizer/keys.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "optimizer/cost.h"

int			PlannerQueryLevel;	/* level of current query */
List	   *PlannerVarParam;	/* correlation Vars to Param mapper */
List	   *PlannerParamVar;	/* to get Var from Param->paramid */
List	   *PlannerInitPlan;	/* init subplans for current query */
int			PlannerPlanId;


static int
_new_param(Var *var, int varlevel)
{
	List	   *last;
	int			i = 0;

	if (PlannerParamVar == NULL)
		last = PlannerParamVar = makeNode(List);
	else
	{
		for (last = PlannerParamVar;;)
		{
			i++;
			if (lnext(last) == NULL)
				break;
			last = lnext(last);
		}
		lnext(last) = makeNode(List);
		last = lnext(last);
	}

	lnext(last) = NULL;
	lfirst(last) = makeVar(var->varno, var->varattno, var->vartype,
				var->vartypmod, varlevel, var->varnoold, var->varoattno);

	return i;
}

static Param *
_replace_var(Var *var)
{
	List	  **rt = (List **) nth(var->varlevelsup, PlannerVarParam);
	List	   *vpe = rt[var->varno - 1];
	Param	   *retval;
	int			i;

	if (vpe == NULL)
	{
		vpe = rt[var->varno - 1] = makeNode(List);
		lfirsti(vpe) = -1;
		lnext(vpe) = NULL;
	}

	for (i = ObjectIdAttributeNumber;; i++)
	{
		if (i == var->varattno)
			break;
		if (lnext(vpe) == NULL)
		{
			lnext(vpe) = makeNode(List);
			vpe = lnext(vpe);
			lfirsti(vpe) = -1;
			lnext(vpe) = NULL;
		}
		else
			vpe = lnext(vpe);
	}

	if ((i = lfirsti(vpe)) < 0) /* parameter is not assigned */
		i = _new_param(var, PlannerQueryLevel - var->varlevelsup);

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = (AttrNumber) i;
	retval->paramtype = var->vartype;

	return retval;
}

static Node *
_make_subplan(SubLink *slink)
{
	SubPlan    *node = makeNode(SubPlan);
	Plan	   *plan;
	List	   *lst;
	Node	   *result;
	List	   *saved_ip = PlannerInitPlan;

	PlannerInitPlan = NULL;

	PlannerQueryLevel++;		/* we becomes child */

	node->plan = plan = union_planner((Query *) slink->subselect);

	/*
	 * Assign subPlan, extParam and locParam to plan nodes. At the moment,
	 * SS_finalize_plan doesn't handle initPlan-s and so we assigne them
	 * to the topmost plan node and take care about its extParam too.
	 */
	(void) SS_finalize_plan(plan);
	plan->initPlan = PlannerInitPlan;

	/* Get extParam from InitPlan-s */
	foreach(lst, PlannerInitPlan)
	{
		List	   *lp;

		foreach(lp, ((SubPlan *) lfirst(lst))->plan->extParam)
		{
			if (!intMember(lfirsti(lp), plan->extParam))
				plan->extParam = lappendi(plan->extParam, lfirsti(lp));
		}
	}

	/* and now we are parent again */
	PlannerInitPlan = saved_ip;
	PlannerQueryLevel--;

	node->plan_id = PlannerPlanId++;
	node->rtable = ((Query *) slink->subselect)->rtable;
	node->sublink = slink;
	slink->subselect = NULL;	/* cool ?! */

	/* make parParam list */
	foreach(lst, plan->extParam)
	{
		Var		   *var = nth(lfirsti(lst), PlannerParamVar);

		if (var->varlevelsup == PlannerQueryLevel)
			node->parParam = lappendi(node->parParam, lfirsti(lst));
	}

	/*
	 * Un-correlated or undirect correlated plans of EXISTS or EXPR types
	 * can be used as initPlans...
	 */
	if (node->parParam == NULL && slink->subLinkType == EXPR_SUBLINK)
	{
		int			i = 0;

		/* transform right side of all sublink Oper-s into Param */
		foreach(lst, slink->oper)
		{
			List	   *rside = lnext(((Expr *) lfirst(lst))->args);
			TargetEntry *te = nth(i, plan->targetlist);
			Var		   *var = makeVar(0, 0, te->resdom->restype,
									  te->resdom->restypmod,
									  PlannerQueryLevel, 0, 0);
			Param	   *prm = makeNode(Param);

			prm->paramkind = PARAM_EXEC;
			prm->paramid = (AttrNumber) _new_param(var, PlannerQueryLevel);
			prm->paramtype = var->vartype;
			lfirst(rside) = prm;
			node->setParam = lappendi(node->setParam, prm->paramid);
			pfree(var);
			i++;
		}
		PlannerInitPlan = lappend(PlannerInitPlan, node);
		if (i > 1)
			result = (Node *) ((slink->useor) ? make_orclause(slink->oper) :
							   make_andclause(slink->oper));
		else
			result = (Node *) lfirst(slink->oper);
	}
	else if (node->parParam == NULL && slink->subLinkType == EXISTS_SUBLINK)
	{
		Var		   *var = makeVar(0, 0, BOOLOID, -1, PlannerQueryLevel, 0, 0);
		Param	   *prm = makeNode(Param);

		prm->paramkind = PARAM_EXEC;
		prm->paramid = (AttrNumber) _new_param(var, PlannerQueryLevel);
		prm->paramtype = var->vartype;
		node->setParam = lappendi(node->setParam, prm->paramid);
		pfree(var);
		PlannerInitPlan = lappend(PlannerInitPlan, node);
		result = (Node *) prm;
	}
	else
/* make expression of SUBPLAN type */
	{
		Expr	   *expr = makeNode(Expr);
		List	   *args = NULL;
		int			i = 0;

		expr->typeOid = BOOLOID;
		expr->opType = SUBPLAN_EXPR;
		expr->oper = (Node *) node;

		/*
		 * Make expr->args from parParam. Left sides of sublink Oper-s are
		 * handled by optimizer directly... Also, transform right side of
		 * sublink Oper-s into Const.
		 */
		foreach(lst, node->parParam)
		{
			Var		   *var = nth(lfirsti(lst), PlannerParamVar);

			var = (Var *) copyObject(var);
			var->varlevelsup = 0;
			args = lappend(args, var);
		}
		foreach(lst, slink->oper)
		{
			List	   *rside = lnext(((Expr *) lfirst(lst))->args);
			TargetEntry *te = nth(i, plan->targetlist);
			Const	   *con = makeConst(te->resdom->restype,
										0, 0, true, 0, 0, 0);

			lfirst(rside) = con;
			i++;
		}
		expr->args = args;
		result = (Node *) expr;
	}

	return result;

}

static List *
set_unioni(List *l1, List *l2)
{
	if (l1 == NULL)
		return l2;
	if (l2 == NULL)
		return l1;

	return nconc(l1, set_differencei(l2, l1));
}

static List *
_finalize_primnode(void *expr, List **subplan)
{
	List	   *result = NULL;

	if (expr == NULL)
		return NULL;

	if (IsA(expr, Param))
	{
		if (((Param *) expr)->paramkind == PARAM_EXEC)
			return lconsi(((Param *) expr)->paramid, (List *) NULL);
	}
	else if (single_node(expr))
		return NULL;
	else if (IsA(expr, List))
	{
		List	   *le;

		foreach(le, (List *) expr)
			result = set_unioni(result,
								_finalize_primnode(lfirst(le), subplan));
	}
	else if (IsA(expr, Iter))
		return _finalize_primnode(((Iter *) expr)->iterexpr, subplan);
	else if (or_clause(expr) || and_clause(expr) || is_opclause(expr) ||
			 not_clause(expr) || is_funcclause(expr))
		return _finalize_primnode(((Expr *) expr)->args, subplan);
	else if (IsA(expr, Aggref))
		return _finalize_primnode(((Aggref *) expr)->target, subplan);
	else if (IsA(expr, ArrayRef))
	{
		result = _finalize_primnode(((ArrayRef *) expr)->refupperindexpr, subplan);
		result = set_unioni(result,
							_finalize_primnode(((ArrayRef *) expr)->reflowerindexpr, subplan));
		result = set_unioni(result,
			  _finalize_primnode(((ArrayRef *) expr)->refexpr, subplan));
		result = set_unioni(result,
		 _finalize_primnode(((ArrayRef *) expr)->refassgnexpr, subplan));
	}
	else if (IsA(expr, TargetEntry))
		return _finalize_primnode(((TargetEntry *) expr)->expr, subplan);
	else if (is_subplan(expr))
	{
		List	   *lst;

		*subplan = lappend(*subplan, ((Expr *) expr)->oper);
		foreach(lst, ((SubPlan *) ((Expr *) expr)->oper)->plan->extParam)
		{
			Var		   *var = nth(lfirsti(lst), PlannerParamVar);

			if (var->varlevelsup < PlannerQueryLevel &&
				!intMember(lfirsti(lst), result))
				result = lappendi(result, lfirsti(lst));
		}
	}
	else
		elog(ERROR, "_finalize_primnode: can't handle node %d", nodeTag(expr));

	return result;
}

Node *
SS_replace_correlation_vars(Node *expr)
{

	if (expr == NULL)
		return NULL;
	if (IsA(expr, List))
	{
		List	   *le;

		foreach(le, (List *) expr)
			lfirst(le) = SS_replace_correlation_vars((Node *) lfirst(le));
	}
	else if (IsA(expr, Var))
	{
		if (((Var *) expr)->varlevelsup > 0)
		{
			Assert(((Var *) expr)->varlevelsup < PlannerQueryLevel);
			expr = (Node *) _replace_var((Var *) expr);
		}
	}
	else if (IsA(expr, Iter))
		((Iter *) expr)->iterexpr = SS_replace_correlation_vars(((Iter *) expr)->iterexpr);
	else if (single_node(expr))
		return expr;
	else if (or_clause(expr) || and_clause(expr) || is_opclause(expr) ||
			 not_clause(expr) || is_funcclause(expr))
		((Expr *) expr)->args = (List *)
			SS_replace_correlation_vars((Node *) ((Expr *) expr)->args);
	else if (IsA(expr, Aggref))
		((Aggref *) expr)->target = SS_replace_correlation_vars((Node *) ((Aggref *) expr)->target);
	else if (IsA(expr, ArrayRef))
	{
		((ArrayRef *) expr)->refupperindexpr = (List *)
			SS_replace_correlation_vars((Node *) ((ArrayRef *) expr)->refupperindexpr);
		((ArrayRef *) expr)->reflowerindexpr = (List *)
			SS_replace_correlation_vars((Node *) ((ArrayRef *) expr)->reflowerindexpr);
		((ArrayRef *) expr)->refexpr = SS_replace_correlation_vars((Node *) ((ArrayRef *) expr)->refexpr);
		((ArrayRef *) expr)->refassgnexpr = SS_replace_correlation_vars(((ArrayRef *) expr)->refassgnexpr);
	}
	else if (IsA(expr, TargetEntry))
		((TargetEntry *) expr)->expr = SS_replace_correlation_vars((Node *) ((TargetEntry *) expr)->expr);
	else if (IsA(expr, SubLink))
	{
		List	   *le;

		foreach(le, ((SubLink *) expr)->oper)	/* left sides only */
		{
			List	   *oparg = ((Expr *) lfirst(le))->args;

			lfirst(oparg) = (List *)
				SS_replace_correlation_vars((Node *) lfirst(oparg));
		}
		((SubLink *) expr)->lefthand = (List *)
			SS_replace_correlation_vars((Node *) ((SubLink *) expr)->lefthand);
	}
	else
		elog(NOTICE, "SS_replace_correlation_vars: can't handle node %d",
			 nodeTag(expr));

	return expr;
}

Node *
SS_process_sublinks(Node *expr)
{
	if (expr == NULL)
		return NULL;
	if (IsA(expr, List))
	{
		List	   *le;

		foreach(le, (List *) expr)
			lfirst(le) = SS_process_sublinks((Node *) lfirst(le));
	}
	else if (or_clause(expr) || and_clause(expr) || is_opclause(expr) ||
			 not_clause(expr) || is_funcclause(expr))
		((Expr *) expr)->args = (List *)
			SS_process_sublinks((Node *) ((Expr *) expr)->args);
	else if (IsA(expr, SubLink))/* got it! */
		expr = _make_subplan((SubLink *) expr);

	return expr;
}

List *
SS_finalize_plan(Plan *plan)
{
	List	   *extParam = NULL;
	List	   *locParam = NULL;
	List	   *subPlan = NULL;
	List	   *param_list;
	List	   *lst;

	if (plan == NULL)
		return NULL;

	param_list = _finalize_primnode(plan->targetlist, &subPlan);
	Assert(subPlan == NULL);

	switch (nodeTag(plan))
	{
		case T_Result:
			param_list = set_unioni(param_list,
									_finalize_primnode(((Result *) plan)->resconstantqual, &subPlan));
			/* subPlan is NOT necessarily NULL here ... */
			break;

		case T_Append:
			foreach(lst, ((Append *) plan)->appendplans)
				param_list = set_unioni(param_list,
								 SS_finalize_plan((Plan *) lfirst(lst)));
			break;

		case T_IndexScan:
			param_list = set_unioni(param_list,
			_finalize_primnode(((IndexScan *) plan)->indxqual, &subPlan));
			Assert(subPlan == NULL);
			break;

		case T_MergeJoin:
			param_list = set_unioni(param_list,
									_finalize_primnode(((MergeJoin *) plan)->mergeclauses, &subPlan));
			Assert(subPlan == NULL);
			break;

		case T_HashJoin:
			param_list = set_unioni(param_list,
									_finalize_primnode(((HashJoin *) plan)->hashclauses, &subPlan));
			Assert(subPlan == NULL);
			break;

		case T_Hash:
			param_list = set_unioni(param_list,
				 _finalize_primnode(((Hash *) plan)->hashkey, &subPlan));
			Assert(subPlan == NULL);
			break;

		case T_Agg:
			param_list = set_unioni(param_list,
					 _finalize_primnode(((Agg *) plan)->aggs, &subPlan));
			Assert(subPlan == NULL);
			break;

		case T_SeqScan:
		case T_NestLoop:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_Group:
			break;
		default:
			elog(ERROR, "SS_finalize_plan: node %d unsupported", nodeTag(plan));
			return NULL;
	}

	param_list = set_unioni(param_list, _finalize_primnode(plan->qual, &subPlan));
	param_list = set_unioni(param_list, SS_finalize_plan(plan->lefttree));
	param_list = set_unioni(param_list, SS_finalize_plan(plan->righttree));

	foreach(lst, param_list)
	{
		Var		   *var = nth(lfirsti(lst), PlannerParamVar);

		if (var->varlevelsup < PlannerQueryLevel)
			extParam = lappendi(extParam, lfirsti(lst));
		else if (var->varlevelsup > PlannerQueryLevel)
			elog(ERROR, "SS_finalize_plan: plan shouldn't reference subplan' variable");
		else
		{
			Assert(var->varno == 0 && var->varattno == 0);
			locParam = lappendi(locParam, lfirsti(lst));
		}
	}

	plan->extParam = extParam;
	plan->locParam = locParam;
	plan->subPlan = subPlan;

	return param_list;

}

/* Construct a list of all subplans found within the given node tree */

List *
SS_pull_subplan(Node *expr)
{
	List	   *result = NULL;

	if (expr == NULL || single_node(expr))
		return NULL;

	if (IsA(expr, List))
	{
		List	   *le;

		foreach(le, (List *) expr)
			result = nconc(result, SS_pull_subplan(lfirst(le)));
	}
	else if (IsA(expr, Iter))
		return SS_pull_subplan(((Iter *) expr)->iterexpr);
	else if (or_clause(expr) || and_clause(expr) || is_opclause(expr) ||
			 not_clause(expr) || is_funcclause(expr))
		return SS_pull_subplan((Node *) ((Expr *) expr)->args);
	else if (IsA(expr, Aggref))
		return SS_pull_subplan(((Aggref *) expr)->target);
	else if (IsA(expr, ArrayRef))
	{
		result = SS_pull_subplan((Node *) ((ArrayRef *) expr)->refupperindexpr);
		result = nconc(result,
		 SS_pull_subplan((Node *) ((ArrayRef *) expr)->reflowerindexpr));
		result = nconc(result,
					   SS_pull_subplan(((ArrayRef *) expr)->refexpr));
		result = nconc(result,
					 SS_pull_subplan(((ArrayRef *) expr)->refassgnexpr));
	}
	else if (IsA(expr, TargetEntry))
		return SS_pull_subplan(((TargetEntry *) expr)->expr);
	else if (is_subplan(expr))
		return lcons(((Expr *) expr)->oper, NULL);
	else
		elog(ERROR, "SS_pull_subplan: can't handle node %d", nodeTag(expr));

	return result;
}
