/*-------------------------------------------------------------------------
 *
 * subselect.c
 *	  Planning routines for subselects and parameters.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/subselect.c,v 1.18.2.1 1999/07/15 01:54:29 tgl Exp $
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
List	   *PlannerInitPlan;	/* init subplans for current query */
List	   *PlannerParamVar;	/* to get Var from Param->paramid */
int			PlannerPlanId;		/* to assign unique ID to subquery plans */

/*--------------------
 * PlannerParamVar is a list of Var nodes, wherein the n'th entry
 * (n counts from 0) corresponds to Param->paramid = n.  The Var nodes
 * are ordinary except for one thing: their varlevelsup field does NOT
 * have the usual interpretation of "subplan levels out from current".
 * Instead, it contains the absolute plan level, with the outermost
 * plan being level 1 and nested plans having higher level numbers.
 * This nonstandardness is useful because we don't have to run around
 * and update the list elements when we enter or exit a subplan
 * recursion level.  But we must pay attention not to confuse this
 * meaning with the normal meaning of varlevelsup.
 *--------------------
 */


/*
 * Create a new entry in the PlannerParamVar list, and return its index.
 *
 * var contains the data to be copied, except for varlevelsup which
 * is set from the absolute level value given by varlevel.
 */
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

/*
 * Generate a Param node to replace the given Var,
 * which is expected to have varlevelsup > 0 (ie, it is not local).
 */
static Param *
_replace_var(Var *var)
{
	List	   *ppv;
	Param	   *retval;
	int			varlevel;
	int			i;

	Assert(var->varlevelsup > 0 && var->varlevelsup < PlannerQueryLevel);
	varlevel = PlannerQueryLevel - var->varlevelsup;

	/*
	 * If there's already a PlannerParamVar entry for this same Var,
	 * just use it.  NOTE: in situations involving UNION or inheritance,
	 * it is possible for the same varno/varlevel to refer to different RTEs
	 * in different parts of the parsetree, so that different fields might
	 * end up sharing the same Param number.  As long as we check the vartype
	 * as well, I believe that this sort of aliasing will cause no trouble.
	 * The correct field should get stored into the Param slot at execution
	 * in each part of the tree.
	 */
	i = 0;
	foreach(ppv, PlannerParamVar)
	{
		Var	   *pvar = lfirst(ppv);

		if (pvar->varno == var->varno &&
			pvar->varattno == var->varattno &&
			pvar->varlevelsup == varlevel &&
			pvar->vartype == var->vartype)
			break;
		i++;
	}

	if (! ppv)
	{
		/* Nope, so make a new one */
		i = _new_param(var, varlevel);
	}

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

	/* Create extParam list as union of InitPlan-s' lists */
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

	/* make parParam list of params coming from current query level */
	foreach(lst, plan->extParam)
	{
		Var		   *var = nth(lfirsti(lst), PlannerParamVar);

		/* note varlevelsup is absolute level number */
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
									  0, 0, 0);
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
		Var		   *var = makeVar(0, 0, BOOLOID, -1, 0, 0, 0);
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
	{
		/* make expression of SUBPLAN type */
		Expr	   *expr = makeNode(Expr);
		List	   *args = NIL;
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
			/* Must fix absolute-level varlevelsup from the
			 * PlannerParamVar entry.  But since var is at current
			 * subplan level, this is easy:
			 */
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

typedef struct finalize_primnode_results {
	List	*subplans;			/* List of subplans found in expr */
	List	*paramids;			/* List of PARAM_EXEC paramids found */
} finalize_primnode_results;

static bool finalize_primnode_walker(Node *node,
									 finalize_primnode_results *results);

static void
finalize_primnode(Node *expr, finalize_primnode_results *results)
{
	results->subplans = NIL;	/* initialize */
	results->paramids = NIL;
	(void) finalize_primnode_walker(expr, results);
}

static bool
finalize_primnode_walker(Node *node,
						 finalize_primnode_results *results)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		if (((Param *) node)->paramkind == PARAM_EXEC)
		{
			int		paramid = (int) ((Param *) node)->paramid;

			if (! intMember(paramid, results->paramids))
				results->paramids = lconsi(paramid, results->paramids);
		}
		return false;			/* no more to do here */
	}
	if (is_subplan(node))
	{
		SubPlan	   *subplan = (SubPlan *) ((Expr *) node)->oper;
		List	   *lst;

		/* Add subplan to subplans list */
		results->subplans = lappend(results->subplans, subplan);
		/* Check extParam list for params to add to paramids */
		foreach(lst, subplan->plan->extParam)
		{
			int			paramid = lfirsti(lst);
			Var		   *var = nth(paramid, PlannerParamVar);

			/* note varlevelsup is absolute level number */
			if (var->varlevelsup < PlannerQueryLevel &&
				! intMember(paramid, results->paramids))
				results->paramids = lconsi(paramid, results->paramids);
		}
		/* XXX We do NOT allow expression_tree_walker to examine the args
		 * passed to the subplan.  Is that correct???  It's what the
		 * old code did, but it seems mighty bogus...  tgl 7/14/99
		 */
		return false;			/* don't recurse into subplan args */
	}
	return expression_tree_walker(node, finalize_primnode_walker,
								  (void *) results);
}

/* Replace correlation vars (uplevel vars) with Params. */

/* XXX should replace this with use of a generalized tree rebuilder,
 * designed along the same lines as expression_tree_walker.
 * Not done yet.
 */
Node *
SS_replace_correlation_vars(Node *expr)
{
	if (expr == NULL)
		return NULL;
	if (IsA(expr, Var))
	{
		if (((Var *) expr)->varlevelsup > 0)
			expr = (Node *) _replace_var((Var *) expr);
	}
	else if (single_node(expr))
		return expr;
	else if (IsA(expr, List))
	{
		List	   *le;

		foreach(le, (List *) expr)
			lfirst(le) = SS_replace_correlation_vars((Node *) lfirst(le));
	}
	else if (IsA(expr, Expr))
	{
		/* XXX do we need to do anything special with subplans? */
		((Expr *) expr)->args = (List *)
			SS_replace_correlation_vars((Node *) ((Expr *) expr)->args);
	}
	else if (IsA(expr, Aggref))
		((Aggref *) expr)->target = SS_replace_correlation_vars(((Aggref *) expr)->target);
	else if (IsA(expr, Iter))
		((Iter *) expr)->iterexpr = SS_replace_correlation_vars(((Iter *) expr)->iterexpr);
	else if (IsA(expr, ArrayRef))
	{
		((ArrayRef *) expr)->refupperindexpr = (List *)
			SS_replace_correlation_vars((Node *) ((ArrayRef *) expr)->refupperindexpr);
		((ArrayRef *) expr)->reflowerindexpr = (List *)
			SS_replace_correlation_vars((Node *) ((ArrayRef *) expr)->reflowerindexpr);
		((ArrayRef *) expr)->refexpr = SS_replace_correlation_vars(((ArrayRef *) expr)->refexpr);
		((ArrayRef *) expr)->refassgnexpr = SS_replace_correlation_vars(((ArrayRef *) expr)->refassgnexpr);
	}
	else if (IsA(expr, CaseExpr))
	{
		CaseExpr   *caseexpr = (CaseExpr *) expr;
		List	   *le;

		foreach(le, caseexpr->args)
		{
			CaseWhen   *when = (CaseWhen *) lfirst(le);
			Assert(IsA(when, CaseWhen));
			when->expr = SS_replace_correlation_vars(when->expr);
			when->result = SS_replace_correlation_vars(when->result);
		}
		/* caseexpr->arg should be null, but we'll check it anyway */
		caseexpr->arg = SS_replace_correlation_vars(caseexpr->arg);
		caseexpr->defresult = SS_replace_correlation_vars(caseexpr->defresult);
	}
	else if (IsA(expr, TargetEntry))
		((TargetEntry *) expr)->expr = SS_replace_correlation_vars(((TargetEntry *) expr)->expr);
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
		elog(ERROR, "SS_replace_correlation_vars: can't handle node %d",
			 nodeTag(expr));

	return expr;
}

/* Replace sublinks by subplans in the given expression */

/* XXX should replace this with use of a generalized tree rebuilder,
 * designed along the same lines as expression_tree_walker.
 * Not done yet.
 */
Node *
SS_process_sublinks(Node *expr)
{
	if (expr == NULL)
		return NULL;
	if (IsA(expr, SubLink))
	{
		expr = _make_subplan((SubLink *) expr);
	}
	else if (single_node(expr))
		return expr;
	else if (IsA(expr, List))
	{
		List	   *le;

		foreach(le, (List *) expr)
			lfirst(le) = SS_process_sublinks((Node *) lfirst(le));
	}
	else if (IsA(expr, Expr))
	{
		/* We should never see a subplan node here, since this is the
		 * routine that makes 'em in the first place.  No need to check.
		 */
		((Expr *) expr)->args = (List *)
			SS_process_sublinks((Node *) ((Expr *) expr)->args);
	}
	else if (IsA(expr, Aggref))
		((Aggref *) expr)->target = SS_process_sublinks(((Aggref *) expr)->target);
	else if (IsA(expr, Iter))
		((Iter *) expr)->iterexpr = SS_process_sublinks(((Iter *) expr)->iterexpr);
	else if (IsA(expr, ArrayRef))
	{
		((ArrayRef *) expr)->refupperindexpr = (List *)
			SS_process_sublinks((Node *) ((ArrayRef *) expr)->refupperindexpr);
		((ArrayRef *) expr)->reflowerindexpr = (List *)
			SS_process_sublinks((Node *) ((ArrayRef *) expr)->reflowerindexpr);
		((ArrayRef *) expr)->refexpr = SS_process_sublinks(((ArrayRef *) expr)->refexpr);
		((ArrayRef *) expr)->refassgnexpr = SS_process_sublinks(((ArrayRef *) expr)->refassgnexpr);
	}
	else if (IsA(expr, CaseExpr))
	{
		CaseExpr   *caseexpr = (CaseExpr *) expr;
		List	   *le;

		foreach(le, caseexpr->args)
		{
			CaseWhen   *when = (CaseWhen *) lfirst(le);
			Assert(IsA(when, CaseWhen));
			when->expr = SS_process_sublinks(when->expr);
			when->result = SS_process_sublinks(when->result);
		}
		/* caseexpr->arg should be null, but we'll check it anyway */
		caseexpr->arg = SS_process_sublinks(caseexpr->arg);
		caseexpr->defresult = SS_process_sublinks(caseexpr->defresult);
	}
	else
		elog(ERROR, "SS_process_sublinks: can't handle node %d",
			 nodeTag(expr));

	return expr;
}

List *
SS_finalize_plan(Plan *plan)
{
	List	   *extParam = NIL;
	List	   *locParam = NIL;
	finalize_primnode_results results;
	List	   *lst;

	if (plan == NULL)
		return NULL;

	/* Find params in targetlist, make sure there are no subplans there */
	finalize_primnode((Node *) plan->targetlist, &results);
	Assert(results.subplans == NIL);

	/* From here on, we invoke finalize_primnode_walker not finalize_primnode,
	 * so that results.paramids lists are automatically merged together and
	 * we don't have to do it the hard way.  But when recursing to self,
	 * we do have to merge the lists.  Oh well.
	 */
	switch (nodeTag(plan))
	{
		case T_Result:
			finalize_primnode_walker(((Result *) plan)->resconstantqual,
									 &results);
			/* results.subplans is NOT necessarily empty here ... */
			break;

		case T_Append:
			foreach(lst, ((Append *) plan)->appendplans)
				results.paramids = set_unioni(results.paramids,
								SS_finalize_plan((Plan *) lfirst(lst)));
			break;

		case T_IndexScan:
			finalize_primnode_walker((Node *) ((IndexScan *) plan)->indxqual,
									 &results);
			Assert(results.subplans == NIL);
			break;

		case T_MergeJoin:
			finalize_primnode_walker((Node *) ((MergeJoin *) plan)->mergeclauses,
									 &results);
			Assert(results.subplans == NIL);
			break;

		case T_HashJoin:
			finalize_primnode_walker((Node *) ((HashJoin *) plan)->hashclauses,
									 &results);
			Assert(results.subplans == NIL);
			break;

		case T_Hash:
			finalize_primnode_walker((Node *) ((Hash *) plan)->hashkey,
									 &results);
			Assert(results.subplans == NIL);
			break;

		case T_Agg:
			finalize_primnode_walker((Node *) ((Agg *) plan)->aggs,
									 &results);
			Assert(results.subplans == NIL);
			break;

		case T_SeqScan:
		case T_NestLoop:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_Group:
			break;

		default:
			elog(ERROR, "SS_finalize_plan: node %d unsupported",
				 nodeTag(plan));
			return NULL;
	}

	finalize_primnode_walker((Node *) plan->qual, &results);
	/* subplans are OK in the qual... */

	results.paramids = set_unioni(results.paramids,
								  SS_finalize_plan(plan->lefttree));
	results.paramids = set_unioni(results.paramids,
								  SS_finalize_plan(plan->righttree));

	/* Now we have all the paramids and subplans */

	foreach(lst, results.paramids)
	{
		Var		   *var = nth(lfirsti(lst), PlannerParamVar);

		/* note varlevelsup is absolute level number */
		if (var->varlevelsup < PlannerQueryLevel)
			extParam = lappendi(extParam, lfirsti(lst));
		else if (var->varlevelsup > PlannerQueryLevel)
			elog(ERROR, "SS_finalize_plan: plan shouldn't reference subplan's variable");
		else
		{
			Assert(var->varno == 0 && var->varattno == 0);
			locParam = lappendi(locParam, lfirsti(lst));
		}
	}

	plan->extParam = extParam;
	plan->locParam = locParam;
	plan->subPlan = results.subplans;

	return results.paramids;
}

/* Construct a list of all subplans found within the given node tree */

static bool SS_pull_subplan_walker(Node *node, List **listptr);

List *
SS_pull_subplan(Node *expr)
{
	List	   *result = NIL;

	SS_pull_subplan_walker(expr, &result);
	return result;
}

static bool
SS_pull_subplan_walker(Node *node, List **listptr)
{
	if (node == NULL)
		return false;
	if (is_subplan(node))
	{
		*listptr = lappend(*listptr, ((Expr *) node)->oper);
		/* XXX original code did not examine args to subplan, is this right? */
		return false;
	}
	return expression_tree_walker(node, SS_pull_subplan_walker,
								  (void *) listptr);
}
