/*-------------------------------------------------------------------------
 *
 * subselect.c
 *	  Planning routines for subselects and parameters.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/subselect.c,v 1.55.2.1 2003/11/30 22:24:16 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/subselect.h"
#include "parser/parsetree.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "utils/syscache.h"


Index		PlannerQueryLevel;	/* level of current query */
List	   *PlannerInitPlan;	/* init subplans for current query */
List	   *PlannerParamVar;	/* to get Var from Param->paramid */

int			PlannerPlanId = 0;	/* to assign unique ID to subquery plans */

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
new_param(Var *var, Index varlevel)
{
	Var		   *paramVar = (Var *) copyObject(var);

	paramVar->varlevelsup = varlevel;

	PlannerParamVar = lappend(PlannerParamVar, paramVar);

	return length(PlannerParamVar) - 1;
}

/*
 * Generate a Param node to replace the given Var,
 * which is expected to have varlevelsup > 0 (ie, it is not local).
 */
static Param *
replace_var(Var *var)
{
	List	   *ppv;
	Param	   *retval;
	Index		varlevel;
	int			i;

	Assert(var->varlevelsup > 0 && var->varlevelsup < PlannerQueryLevel);
	varlevel = PlannerQueryLevel - var->varlevelsup;

	/*
	 * If there's already a PlannerParamVar entry for this same Var, just
	 * use it.	NOTE: in sufficiently complex querytrees, it is possible
	 * for the same varno/varlevel to refer to different RTEs in different
	 * parts of the parsetree, so that different fields might end up
	 * sharing the same Param number.  As long as we check the vartype as
	 * well, I believe that this sort of aliasing will cause no trouble.
	 * The correct field should get stored into the Param slot at
	 * execution in each part of the tree.
	 *
	 * We also need to demand a match on vartypmod.  This does not matter
	 * for the Param itself, since those are not typmod-dependent, but it
	 * does matter when make_subplan() instantiates a modified copy of the
	 * Var for a subplan's args list.
	 */
	i = 0;
	foreach(ppv, PlannerParamVar)
	{
		Var		   *pvar = lfirst(ppv);

		if (pvar->varno == var->varno &&
			pvar->varattno == var->varattno &&
			pvar->varlevelsup == varlevel &&
			pvar->vartype == var->vartype &&
			pvar->vartypmod == var->vartypmod)
			break;
		i++;
	}

	if (!ppv)
	{
		/* Nope, so make a new one */
		i = new_param(var, varlevel);
	}

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = (AttrNumber) i;
	retval->paramtype = var->vartype;

	return retval;
}

/*
 * Convert a bare SubLink (as created by the parser) into a SubPlan.
 */
static Node *
make_subplan(SubLink *slink)
{
	SubPlan    *node = makeNode(SubPlan);
	Query	   *subquery = (Query *) (slink->subselect);
	Oid			result_type = exprType((Node *) slink);
	double		tuple_fraction;
	Plan	   *plan;
	List	   *lst;
	Node	   *result;

	/*
	 * Check to see if this node was already processed; if so we have
	 * trouble.  We check to see if the linked-to Query appears to have
	 * been planned already, too.
	 */
	if (subquery == NULL)
		elog(ERROR, "make_subplan: invalid expression structure (SubLink already processed?)");
	if (subquery->base_rel_list != NIL)
		elog(ERROR, "make_subplan: invalid expression structure (subquery already processed?)");

	/*
	 * Copy the source Query node.	This is a quick and dirty kluge to
	 * resolve the fact that the parser can generate trees with multiple
	 * links to the same sub-Query node, but the planner wants to scribble
	 * on the Query. Try to clean this up when we do querytree redesign...
	 */
	subquery = (Query *) copyObject(subquery);

	/*
	 * For an EXISTS subplan, tell lower-level planner to expect that only
	 * the first tuple will be retrieved.  For ALL and ANY subplans, we
	 * will be able to stop evaluating if the test condition fails, so
	 * very often not all the tuples will be retrieved; for lack of a
	 * better idea, specify 50% retrieval.	For EXPR and MULTIEXPR
	 * subplans, use default behavior (we're only expecting one row out,
	 * anyway).
	 *
	 * NOTE: if you change these numbers, also change cost_qual_eval_walker()
	 * in path/costsize.c.
	 *
	 * XXX If an ALL/ANY subplan is uncorrelated, we may decide to
	 * materialize its result below.  In that case it would've been better
	 * to specify full retrieval.  At present, however, we can only detect
	 * correlation or lack of it after we've made the subplan :-(. Perhaps
	 * detection of correlation should be done as a separate step.
	 * Meanwhile, we don't want to be too optimistic about the percentage
	 * of tuples retrieved, for fear of selecting a plan that's bad for
	 * the materialization case.
	 */
	if (slink->subLinkType == EXISTS_SUBLINK)
		tuple_fraction = 1.0;	/* just like a LIMIT 1 */
	else if (slink->subLinkType == ALL_SUBLINK ||
			 slink->subLinkType == ANY_SUBLINK)
		tuple_fraction = 0.5;	/* 50% */
	else
		tuple_fraction = -1.0;	/* default behavior */

	/*
	 * Generate the plan for the subquery.
	 */
	node->plan = plan = subquery_planner(subquery, tuple_fraction);

	node->plan_id = PlannerPlanId++;	/* Assign unique ID to this
										 * SubPlan */

	node->rtable = subquery->rtable;
	node->sublink = slink;

	slink->subselect = NULL;	/* cool ?! see error check above! */

	/*
	 * Make parParam list of params that current query level will pass to
	 * this child plan.
	 */
	foreach(lst, plan->extParam)
	{
		int			paramid = lfirsti(lst);
		Var		   *var = nth(paramid, PlannerParamVar);

		/* note varlevelsup is absolute level number */
		if (var->varlevelsup == PlannerQueryLevel)
			node->parParam = lappendi(node->parParam, paramid);
	}

	/*
	 * Un-correlated or undirect correlated plans of EXISTS, EXPR, or
	 * MULTIEXPR types can be used as initPlans.  For EXISTS or EXPR, we
	 * just produce a Param referring to the result of evaluating the
	 * initPlan.  For MULTIEXPR, we must build an AND or OR-clause of the
	 * individual comparison operators, using the appropriate lefthand
	 * side expressions and Params for the initPlan's target items.
	 */
	if (node->parParam == NIL && slink->subLinkType == EXISTS_SUBLINK)
	{
		Var		   *var = makeVar(0, 0, BOOLOID, -1, 0);
		Param	   *prm = makeNode(Param);

		prm->paramkind = PARAM_EXEC;
		prm->paramid = (AttrNumber) new_param(var, PlannerQueryLevel);
		prm->paramtype = var->vartype;
		pfree(var);				/* var is only needed for new_param */
		node->setParam = lappendi(node->setParam, prm->paramid);
		PlannerInitPlan = lappend(PlannerInitPlan, node);
		result = (Node *) prm;
	}
	else if (node->parParam == NIL && slink->subLinkType == EXPR_SUBLINK)
	{
		TargetEntry *te = lfirst(plan->targetlist);

		/* need a var node just to pass to new_param()... */
		Var		   *var = makeVar(0, 0, te->resdom->restype,
								  te->resdom->restypmod, 0);
		Param	   *prm = makeNode(Param);

		prm->paramkind = PARAM_EXEC;
		prm->paramid = (AttrNumber) new_param(var, PlannerQueryLevel);
		prm->paramtype = var->vartype;
		pfree(var);				/* var is only needed for new_param */
		node->setParam = lappendi(node->setParam, prm->paramid);
		PlannerInitPlan = lappend(PlannerInitPlan, node);
		result = (Node *) prm;
	}
	else if (node->parParam == NIL && slink->subLinkType == MULTIEXPR_SUBLINK)
	{
		List	   *newoper = NIL;
		int			i = 0;

		/*
		 * Convert oper list of Opers into a list of Exprs, using lefthand
		 * arguments and Params representing inside results.
		 */
		foreach(lst, slink->oper)
		{
			Oper	   *oper = (Oper *) lfirst(lst);
			Node	   *lefthand = nth(i, slink->lefthand);
			TargetEntry *te = nth(i, plan->targetlist);

			/* need a var node just to pass to new_param()... */
			Var		   *var = makeVar(0, 0, te->resdom->restype,
									  te->resdom->restypmod, 0);
			Param	   *prm = makeNode(Param);
			Operator	tup;
			Form_pg_operator opform;
			Node	   *left,
					   *right;

			prm->paramkind = PARAM_EXEC;
			prm->paramid = (AttrNumber) new_param(var, PlannerQueryLevel);
			prm->paramtype = var->vartype;
			pfree(var);			/* var is only needed for new_param */

			Assert(IsA(oper, Oper));
			tup = SearchSysCache(OPEROID,
								 ObjectIdGetDatum(oper->opno),
								 0, 0, 0);
			if (!HeapTupleIsValid(tup))
				elog(ERROR, "cache lookup failed for operator %u", oper->opno);
			opform = (Form_pg_operator) GETSTRUCT(tup);

			/*
			 * Note: we use make_operand in case runtime type conversion
			 * function calls must be inserted for this operator!
			 */
			left = make_operand(lefthand,
								exprType(lefthand), opform->oprleft);
			right = make_operand((Node *) prm,
								 prm->paramtype, opform->oprright);
			ReleaseSysCache(tup);

			newoper = lappend(newoper,
							  make_opclause(oper,
											(Var *) left,
											(Var *) right));
			node->setParam = lappendi(node->setParam, prm->paramid);
			i++;
		}
		slink->oper = newoper;
		slink->lefthand = NIL;
		PlannerInitPlan = lappend(PlannerInitPlan, node);
		if (i > 1)
			result = (Node *) ((slink->useor) ? make_orclause(newoper) :
							   make_andclause(newoper));
		else
			result = (Node *) lfirst(newoper);
	}
	else
	{
		Expr	   *expr = makeNode(Expr);
		List	   *args = NIL;
		List	   *newoper = NIL;
		int			i = 0;

		/*
		 * We can't convert subplans of ALL_SUBLINK or ANY_SUBLINK types
		 * to initPlans, even when they are uncorrelated or undirect
		 * correlated, because we need to scan the output of the subplan
		 * for each outer tuple.  However, we have the option to tack a
		 * MATERIAL node onto the top of an uncorrelated/undirect
		 * correlated subplan, which lets us do the work of evaluating the
		 * subplan only once.  We do this if the subplan's top plan node
		 * is anything more complicated than a plain sequential scan, and
		 * we do it even for seqscan if the qual appears selective enough
		 * to eliminate many tuples.
		 *
		 * XXX It's pretty ugly to be inserting a MATERIAL node at this
		 * point.  Since subquery_planner has already run SS_finalize_plan
		 * on the subplan tree, we have to kluge up parameter lists for
		 * the MATERIAL node.  Possibly this could be fixed by postponing
		 * SS_finalize_plan processing until setrefs.c is run.
		 */
		if (node->parParam == NIL)
		{
			bool		use_material;

			switch (nodeTag(plan))
			{
				case T_SeqScan:
					if (plan->initPlan || plan->subPlan)
						use_material = true;
					else
					{
						Selectivity qualsel;

						qualsel = clauselist_selectivity(subquery,
														 plan->qual,
														 0);
						/* Is 10% selectivity a good threshold?? */
						use_material = qualsel < 0.10;
					}
					break;
				case T_Material:
				case T_FunctionScan:
				case T_Sort:

					/*
					 * Don't add another Material node if there's one
					 * already, nor if the top node is any other type that
					 * materializes its output anyway.
					 */
					use_material = false;
					break;
				default:
					use_material = true;
					break;
			}
			if (use_material)
			{
				Plan	   *matplan;

				matplan = (Plan *) make_material(plan->targetlist, plan);
				/* kluge --- see comments above */
				matplan->extParam = listCopy(plan->extParam);
				matplan->locParam = listCopy(plan->locParam);
				node->plan = plan = matplan;
			}
		}

		/*
		 * Make expression of SUBPLAN type
		 */
		expr->typeOid = result_type;
		expr->opType = SUBPLAN_EXPR;
		expr->oper = (Node *) node;

		/*
		 * Make expr->args from parParam.
		 */
		foreach(lst, node->parParam)
		{
			Var		   *var = nth(lfirsti(lst), PlannerParamVar);

			var = (Var *) copyObject(var);

			/*
			 * Must fix absolute-level varlevelsup from the
			 * PlannerParamVar entry.  But since var is at current subplan
			 * level, this is easy:
			 */
			var->varlevelsup = 0;
			args = lappend(args, var);
		}
		expr->args = args;

		/*
		 * Convert oper list of Opers into a list of Exprs, using lefthand
		 * arguments and Consts representing inside results.
		 */
		foreach(lst, slink->oper)
		{
			Oper	   *oper = (Oper *) lfirst(lst);
			Node	   *lefthand = nth(i, slink->lefthand);
			TargetEntry *te = nth(i, plan->targetlist);
			Const	   *con;
			Operator	tup;
			Form_pg_operator opform;
			Node	   *left,
					   *right;

			con = makeNullConst(te->resdom->restype);

			Assert(IsA(oper, Oper));
			tup = SearchSysCache(OPEROID,
								 ObjectIdGetDatum(oper->opno),
								 0, 0, 0);
			if (!HeapTupleIsValid(tup))
				elog(ERROR, "cache lookup failed for operator %u", oper->opno);
			opform = (Form_pg_operator) GETSTRUCT(tup);

			/*
			 * Note: we use make_operand in case runtime type conversion
			 * function calls must be inserted for this operator!
			 */
			left = make_operand(lefthand,
								exprType(lefthand), opform->oprleft);
			right = make_operand((Node *) con,
								 con->consttype, opform->oprright);
			ReleaseSysCache(tup);

			newoper = lappend(newoper,
							  make_opclause(oper,
											(Var *) left,
											(Var *) right));
			i++;
		}
		slink->oper = newoper;
		slink->lefthand = NIL;
		result = (Node *) expr;
	}

	return result;
}

/*
 * finalize_primnode: build lists of subplans and params appearing
 * in the given expression tree.  NOTE: items are added to lists passed in,
 * so caller must initialize lists to NIL before first call!
 *
 * Note: the subplan list that is constructed here and assigned to the
 * plan's subPlan field will be replaced with an up-to-date list in
 * set_plan_references().  We could almost dispense with building this
 * subplan list at all; I believe the only place that uses it is the
 * check in make_subplan to see whether a subselect has any subselects.
 */

typedef struct finalize_primnode_results
{
	List	   *subplans;		/* List of subplans found in expr */
	List	   *paramids;		/* List of PARAM_EXEC paramids found */
} finalize_primnode_results;

static bool
finalize_primnode(Node *node, finalize_primnode_results *results)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		if (((Param *) node)->paramkind == PARAM_EXEC)
		{
			int			paramid = (int) ((Param *) node)->paramid;

			if (!intMember(paramid, results->paramids))
				results->paramids = lconsi(paramid, results->paramids);
		}
		return false;			/* no more to do here */
	}
	if (is_subplan(node))
	{
		SubPlan    *subplan = (SubPlan *) ((Expr *) node)->oper;
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
				!intMember(paramid, results->paramids))
				results->paramids = lconsi(paramid, results->paramids);
		}
		/* fall through to recurse into subplan args */
	}
	return expression_tree_walker(node, finalize_primnode,
								  (void *) results);
}

/*
 * Replace correlation vars (uplevel vars) with Params.
 */

static Node *replace_correlation_vars_mutator(Node *node, void *context);

Node *
SS_replace_correlation_vars(Node *expr)
{
	/* No setup needed for tree walk, so away we go */
	return replace_correlation_vars_mutator(expr, NULL);
}

static Node *
replace_correlation_vars_mutator(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup > 0)
			return (Node *) replace_var((Var *) node);
	}
	return expression_tree_mutator(node,
								   replace_correlation_vars_mutator,
								   context);
}

/*
 * Expand SubLinks to SubPlans in the given expression.
 */

static Node *process_sublinks_mutator(Node *node, void *context);

Node *
SS_process_sublinks(Node *expr)
{
	/* No setup needed for tree walk, so away we go */
	return process_sublinks_mutator(expr, NULL);
}

static Node *
process_sublinks_mutator(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;

		/*
		 * First, scan the lefthand-side expressions, if any. This is a
		 * tad klugy since we modify the input SubLink node, but that
		 * should be OK (make_subplan does it too!)
		 */
		sublink->lefthand = (List *)
			process_sublinks_mutator((Node *) sublink->lefthand, context);
		/* Now build the SubPlan node and make the expr to return */
		return make_subplan(sublink);
	}

	/*
	 * Note that we will never see a SubPlan expression in the input
	 * (since this is the very routine that creates 'em to begin with). So
	 * the code in expression_tree_mutator() that might do inappropriate
	 * things with SubPlans or SubLinks will not be exercised.
	 */
	Assert(!is_subplan(node));

	return expression_tree_mutator(node,
								   process_sublinks_mutator,
								   context);
}

List *
SS_finalize_plan(Plan *plan, List *rtable)
{
	List	   *extParam = NIL;
	List	   *locParam = NIL;
	finalize_primnode_results results;
	List	   *lst;

	if (plan == NULL)
		return NIL;

	results.subplans = NIL;		/* initialize lists to NIL */
	results.paramids = NIL;

	/*
	 * When we call finalize_primnode, results.paramids lists are
	 * automatically merged together.  But when recursing to self, we have
	 * to do it the hard way.  We want the paramids list to include params
	 * in subplans as well as at this level. (We don't care about finding
	 * subplans of subplans, though.)
	 */

	/* Find params and subplans in targetlist and qual */
	finalize_primnode((Node *) plan->targetlist, &results);
	finalize_primnode((Node *) plan->qual, &results);

	/* Check additional node-type-specific fields */
	switch (nodeTag(plan))
	{
		case T_Result:
			finalize_primnode(((Result *) plan)->resconstantqual,
							  &results);
			break;

		case T_IndexScan:
			finalize_primnode((Node *) ((IndexScan *) plan)->indxqual,
							  &results);

			/*
			 * we need not look at indxqualorig, since it will have the
			 * same param references as indxqual, and we aren't really
			 * concerned yet about having a complete subplan list.
			 */
			break;

		case T_TidScan:
			finalize_primnode((Node *) ((TidScan *) plan)->tideval,
							  &results);
			break;

		case T_SubqueryScan:

			/*
			 * In a SubqueryScan, SS_finalize_plan has already been run on
			 * the subplan by the inner invocation of subquery_planner, so
			 * there's no need to do it again.  Instead, just pull out the
			 * subplan's extParams list, which represents the params it
			 * needs from my level and higher levels.
			 */
			results.paramids = set_unioni(results.paramids,
							 ((SubqueryScan *) plan)->subplan->extParam);
			break;

		case T_FunctionScan:
			{
				RangeTblEntry *rte;

				rte = rt_fetch(((FunctionScan *) plan)->scan.scanrelid,
							   rtable);
				Assert(rte->rtekind == RTE_FUNCTION);
				finalize_primnode(rte->funcexpr, &results);
			}
			break;

		case T_Append:
			foreach(lst, ((Append *) plan)->appendplans)
				results.paramids = set_unioni(results.paramids,
								   SS_finalize_plan((Plan *) lfirst(lst),
													rtable));
			break;

		case T_NestLoop:
			finalize_primnode((Node *) ((Join *) plan)->joinqual,
							  &results);
			break;

		case T_MergeJoin:
			finalize_primnode((Node *) ((Join *) plan)->joinqual,
							  &results);
			finalize_primnode((Node *) ((MergeJoin *) plan)->mergeclauses,
							  &results);
			break;

		case T_HashJoin:
			finalize_primnode((Node *) ((Join *) plan)->joinqual,
							  &results);
			finalize_primnode((Node *) ((HashJoin *) plan)->hashclauses,
							  &results);
			break;

		case T_Hash:
			finalize_primnode(((Hash *) plan)->hashkey,
							  &results);
			break;

		case T_Agg:
		case T_SeqScan:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_SetOp:
		case T_Limit:
		case T_Group:
			break;

		default:
			elog(ERROR, "SS_finalize_plan: node %d unsupported",
				 nodeTag(plan));
	}

	/* Process left and right subplans, if any */
	results.paramids = set_unioni(results.paramids,
								  SS_finalize_plan(plan->lefttree,
												   rtable));
	results.paramids = set_unioni(results.paramids,
								  SS_finalize_plan(plan->righttree,
												   rtable));

	/* Now we have all the paramids and subplans */

	foreach(lst, results.paramids)
	{
		int			paramid = lfirsti(lst);
		Var		   *var = nth(paramid, PlannerParamVar);

		/* note varlevelsup is absolute level number */
		if (var->varlevelsup < PlannerQueryLevel)
			extParam = lappendi(extParam, paramid);
		else if (var->varlevelsup > PlannerQueryLevel)
			elog(ERROR, "SS_finalize_plan: plan shouldn't reference subplan's variable");
		else
		{
			Assert(var->varno == 0 && var->varattno == 0);
			locParam = lappendi(locParam, paramid);
		}
	}

	plan->extParam = extParam;
	plan->locParam = locParam;
	plan->subPlan = results.subplans;

	return results.paramids;
}
