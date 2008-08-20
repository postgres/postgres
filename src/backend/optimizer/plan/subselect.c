/*-------------------------------------------------------------------------
 *
 * subselect.c
 *	  Planning routines for subselects and parameters.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/plan/subselect.c,v 1.137 2008/08/20 19:58:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/var.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


typedef struct convert_testexpr_context
{
	PlannerInfo *root;
	List	   *subst_nodes;	/* Nodes to substitute for Params */
} convert_testexpr_context;

typedef struct process_sublinks_context
{
	PlannerInfo *root;
	bool		isTopQual;
} process_sublinks_context;

typedef struct finalize_primnode_context
{
	PlannerInfo *root;
	Bitmapset  *paramids;		/* Non-local PARAM_EXEC paramids found */
} finalize_primnode_context;


static List *generate_subquery_params(PlannerInfo *root, List *tlist,
									  List **paramIds);
static List *generate_subquery_vars(PlannerInfo *root, List *tlist,
									Index varno);
static Node *convert_testexpr(PlannerInfo *root,
				 Node *testexpr,
				 List *subst_nodes);
static Node *convert_testexpr_mutator(Node *node,
						 convert_testexpr_context *context);
static bool subplan_is_hashable(SubLink *slink, SubPlan *node, Plan *plan);
static bool hash_ok_operator(OpExpr *expr);
static bool simplify_EXISTS_query(Query *query);
static Node *replace_correlation_vars_mutator(Node *node, PlannerInfo *root);
static Node *process_sublinks_mutator(Node *node,
						 process_sublinks_context *context);
static Bitmapset *finalize_plan(PlannerInfo *root,
			  Plan *plan,
			  Bitmapset *valid_params);
static bool finalize_primnode(Node *node, finalize_primnode_context *context);


/*
 * Generate a Param node to replace the given Var,
 * which is expected to have varlevelsup > 0 (ie, it is not local).
 */
static Param *
replace_outer_var(PlannerInfo *root, Var *var)
{
	Param	   *retval;
	ListCell   *ppl;
	PlannerParamItem *pitem;
	Index		abslevel;
	int			i;

	Assert(var->varlevelsup > 0 && var->varlevelsup < root->query_level);
	abslevel = root->query_level - var->varlevelsup;

	/*
	 * If there's already a paramlist entry for this same Var, just use it.
	 * NOTE: in sufficiently complex querytrees, it is possible for the same
	 * varno/abslevel to refer to different RTEs in different parts of the
	 * parsetree, so that different fields might end up sharing the same Param
	 * number.	As long as we check the vartype/typmod as well, I believe that
	 * this sort of aliasing will cause no trouble.  The correct field should
	 * get stored into the Param slot at execution in each part of the tree.
	 */
	i = 0;
	foreach(ppl, root->glob->paramlist)
	{
		pitem = (PlannerParamItem *) lfirst(ppl);
		if (pitem->abslevel == abslevel && IsA(pitem->item, Var))
		{
			Var		   *pvar = (Var *) pitem->item;

			if (pvar->varno == var->varno &&
				pvar->varattno == var->varattno &&
				pvar->vartype == var->vartype &&
				pvar->vartypmod == var->vartypmod)
				break;
		}
		i++;
	}

	if (!ppl)
	{
		/* Nope, so make a new one */
		var = (Var *) copyObject(var);
		var->varlevelsup = 0;

		pitem = makeNode(PlannerParamItem);
		pitem->item = (Node *) var;
		pitem->abslevel = abslevel;

		root->glob->paramlist = lappend(root->glob->paramlist, pitem);
		/* i is already the correct index for the new item */
	}

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = i;
	retval->paramtype = var->vartype;
	retval->paramtypmod = var->vartypmod;

	return retval;
}

/*
 * Generate a Param node to replace the given Aggref
 * which is expected to have agglevelsup > 0 (ie, it is not local).
 */
static Param *
replace_outer_agg(PlannerInfo *root, Aggref *agg)
{
	Param	   *retval;
	PlannerParamItem *pitem;
	Index		abslevel;
	int			i;

	Assert(agg->agglevelsup > 0 && agg->agglevelsup < root->query_level);
	abslevel = root->query_level - agg->agglevelsup;

	/*
	 * It does not seem worthwhile to try to match duplicate outer aggs. Just
	 * make a new slot every time.
	 */
	agg = (Aggref *) copyObject(agg);
	IncrementVarSublevelsUp((Node *) agg, -((int) agg->agglevelsup), 0);
	Assert(agg->agglevelsup == 0);

	pitem = makeNode(PlannerParamItem);
	pitem->item = (Node *) agg;
	pitem->abslevel = abslevel;

	root->glob->paramlist = lappend(root->glob->paramlist, pitem);
	i = list_length(root->glob->paramlist) - 1;

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = i;
	retval->paramtype = agg->aggtype;
	retval->paramtypmod = -1;

	return retval;
}

/*
 * Generate a new Param node that will not conflict with any other.
 *
 * This is used to allocate PARAM_EXEC slots for subplan outputs.
 */
static Param *
generate_new_param(PlannerInfo *root, Oid paramtype, int32 paramtypmod)
{
	Param	   *retval;
	PlannerParamItem *pitem;

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = list_length(root->glob->paramlist);
	retval->paramtype = paramtype;
	retval->paramtypmod = paramtypmod;

	pitem = makeNode(PlannerParamItem);
	pitem->item = (Node *) retval;
	pitem->abslevel = root->query_level;

	root->glob->paramlist = lappend(root->glob->paramlist, pitem);

	return retval;
}

/*
 * Get the datatype of the first column of the plan's output.
 *
 * This is stored for ARRAY_SUBLINK and for exprType(), which doesn't have any
 * way to get at the plan associated with a SubPlan node.  We really only need
 * the value for EXPR_SUBLINK and ARRAY_SUBLINK subplans, but for consistency
 * we set it always.
 */
static Oid
get_first_col_type(Plan *plan)
{
	/* In cases such as EXISTS, tlist might be empty; arbitrarily use VOID */
	if (plan->targetlist)
	{
		TargetEntry *tent = (TargetEntry *) linitial(plan->targetlist);

		Assert(IsA(tent, TargetEntry));
		if (!tent->resjunk)
			return exprType((Node *) tent->expr);
	}
	return VOIDOID;
}

/*
 * Convert a SubLink (as created by the parser) into a SubPlan.
 *
 * We are given the original SubLink and the already-processed testexpr
 * (use this instead of the SubLink's own field).  We are also told if
 * this expression appears at top level of a WHERE/HAVING qual.
 *
 * The result is whatever we need to substitute in place of the SubLink
 * node in the executable expression.  This will be either the SubPlan
 * node (if we have to do the subplan as a subplan), or a Param node
 * representing the result of an InitPlan, or a row comparison expression
 * tree containing InitPlan Param nodes.
 */
static Node *
make_subplan(PlannerInfo *root, SubLink *slink, Node *testexpr, bool isTopQual)
{
	Query	   *subquery = (Query *) (slink->subselect);
	double		tuple_fraction;
	SubPlan    *splan;
	Plan	   *plan;
	PlannerInfo *subroot;
	bool		isInitPlan;
	Bitmapset  *tmpset;
	int			paramid;
	Node	   *result;

	/*
	 * Copy the source Query node.	This is a quick and dirty kluge to resolve
	 * the fact that the parser can generate trees with multiple links to the
	 * same sub-Query node, but the planner wants to scribble on the Query.
	 * Try to clean this up when we do querytree redesign...
	 */
	subquery = (Query *) copyObject(subquery);

	/*
	 * If it's an EXISTS subplan, we might be able to simplify it.
	 */
	if (slink->subLinkType == EXISTS_SUBLINK)
		(void) simplify_EXISTS_query(subquery);

	/*
	 * For an EXISTS subplan, tell lower-level planner to expect that only the
	 * first tuple will be retrieved.  For ALL and ANY subplans, we will be
	 * able to stop evaluating if the test condition fails, so very often not
	 * all the tuples will be retrieved; for lack of a better idea, specify
	 * 50% retrieval.  For EXPR and ROWCOMPARE subplans, use default behavior
	 * (we're only expecting one row out, anyway).
	 *
	 * NOTE: if you change these numbers, also change cost_qual_eval_walker()
	 * and get_initplan_cost() in path/costsize.c.
	 *
	 * XXX If an ALL/ANY subplan is uncorrelated, we may decide to hash or
	 * materialize its result below.  In that case it would've been better to
	 * specify full retrieval.	At present, however, we can only detect
	 * correlation or lack of it after we've made the subplan :-(. Perhaps
	 * detection of correlation should be done as a separate step. Meanwhile,
	 * we don't want to be too optimistic about the percentage of tuples
	 * retrieved, for fear of selecting a plan that's bad for the
	 * materialization case.
	 */
	if (slink->subLinkType == EXISTS_SUBLINK)
		tuple_fraction = 1.0;	/* just like a LIMIT 1 */
	else if (slink->subLinkType == ALL_SUBLINK ||
			 slink->subLinkType == ANY_SUBLINK)
		tuple_fraction = 0.5;	/* 50% */
	else
		tuple_fraction = 0.0;	/* default behavior */

	/*
	 * Generate the plan for the subquery.
	 */
	plan = subquery_planner(root->glob, subquery,
							root->query_level + 1,
							tuple_fraction,
							&subroot);

	/*
	 * Initialize the SubPlan node.  Note plan_id isn't set yet.
	 */
	splan = makeNode(SubPlan);
	splan->subLinkType = slink->subLinkType;
	splan->testexpr = NULL;
	splan->paramIds = NIL;
	splan->firstColType = get_first_col_type(plan);
	splan->useHashTable = false;
	/* At top level of a qual, can treat UNKNOWN the same as FALSE */
	splan->unknownEqFalse = isTopQual;
	splan->setParam = NIL;
	splan->parParam = NIL;
	splan->args = NIL;

	/*
	 * Make parParam list of params that current query level will pass to this
	 * child plan.
	 */
	tmpset = bms_copy(plan->extParam);
	while ((paramid = bms_first_member(tmpset)) >= 0)
	{
		PlannerParamItem *pitem = list_nth(root->glob->paramlist, paramid);

		if (pitem->abslevel == root->query_level)
			splan->parParam = lappend_int(splan->parParam, paramid);
	}
	bms_free(tmpset);

	/*
	 * Un-correlated or undirect correlated plans of EXISTS, EXPR, ARRAY, or
	 * ROWCOMPARE types can be used as initPlans.  For EXISTS, EXPR, or ARRAY,
	 * we just produce a Param referring to the result of evaluating the
	 * initPlan.  For ROWCOMPARE, we must modify the testexpr tree to contain
	 * PARAM_EXEC Params instead of the PARAM_SUBLINK Params emitted by the
	 * parser.
	 */
	if (splan->parParam == NIL && slink->subLinkType == EXISTS_SUBLINK)
	{
		Param	   *prm;

		prm = generate_new_param(root, BOOLOID, -1);
		splan->setParam = list_make1_int(prm->paramid);
		isInitPlan = true;
		result = (Node *) prm;
	}
	else if (splan->parParam == NIL && slink->subLinkType == EXPR_SUBLINK)
	{
		TargetEntry *te = linitial(plan->targetlist);
		Param	   *prm;

		Assert(!te->resjunk);
		prm = generate_new_param(root,
								 exprType((Node *) te->expr),
								 exprTypmod((Node *) te->expr));
		splan->setParam = list_make1_int(prm->paramid);
		isInitPlan = true;
		result = (Node *) prm;
	}
	else if (splan->parParam == NIL && slink->subLinkType == ARRAY_SUBLINK)
	{
		TargetEntry *te = linitial(plan->targetlist);
		Oid			arraytype;
		Param	   *prm;

		Assert(!te->resjunk);
		arraytype = get_array_type(exprType((Node *) te->expr));
		if (!OidIsValid(arraytype))
			elog(ERROR, "could not find array type for datatype %s",
				 format_type_be(exprType((Node *) te->expr)));
		prm = generate_new_param(root,
								 arraytype,
								 exprTypmod((Node *) te->expr));
		splan->setParam = list_make1_int(prm->paramid);
		isInitPlan = true;
		result = (Node *) prm;
	}
	else if (splan->parParam == NIL && slink->subLinkType == ROWCOMPARE_SUBLINK)
	{
		/* Adjust the Params */
		List	   *params;

		params = generate_subquery_params(root,
										  plan->targetlist,
										  &splan->paramIds);
		result = convert_testexpr(root,
								  testexpr,
								  params);
		splan->setParam = list_copy(splan->paramIds);
		isInitPlan = true;

		/*
		 * The executable expression is returned to become part of the outer
		 * plan's expression tree; it is not kept in the initplan node.
		 */
	}
	else
	{
		List	   *args;
		ListCell   *l;

		if (testexpr)
		{
			List	   *params;

			/* Adjust the Params in the testexpr */
			params = generate_subquery_params(root,
											  plan->targetlist,
											  &splan->paramIds);
			splan->testexpr = convert_testexpr(root,
											   testexpr,
											   params);
		}

		/*
		 * We can't convert subplans of ALL_SUBLINK or ANY_SUBLINK types to
		 * initPlans, even when they are uncorrelated or undirect correlated,
		 * because we need to scan the output of the subplan for each outer
		 * tuple.  But if it's an IN (= ANY) test, we might be able to use a
		 * hashtable to avoid comparing all the tuples.
		 */
		if (subplan_is_hashable(slink, splan, plan))
			splan->useHashTable = true;

		/*
		 * Otherwise, we have the option to tack a MATERIAL node onto the top
		 * of the subplan, to reduce the cost of reading it repeatedly.  This
		 * is pointless for a direct-correlated subplan, since we'd have to
		 * recompute its results each time anyway.	For uncorrelated/undirect
		 * correlated subplans, we add MATERIAL unless the subplan's top plan
		 * node would materialize its output anyway.
		 */
		else if (splan->parParam == NIL)
		{
			bool		use_material;

			switch (nodeTag(plan))
			{
				case T_Material:
				case T_FunctionScan:
				case T_Sort:
					use_material = false;
					break;
				default:
					use_material = true;
					break;
			}
			if (use_material)
				plan = materialize_finished_plan(plan);
		}

		/*
		 * Make splan->args from parParam.
		 */
		args = NIL;
		foreach(l, splan->parParam)
		{
			PlannerParamItem *pitem = list_nth(root->glob->paramlist,
											   lfirst_int(l));

			/*
			 * The Var or Aggref has already been adjusted to have the correct
			 * varlevelsup or agglevelsup.	We probably don't even need to
			 * copy it again, but be safe.
			 */
			args = lappend(args, copyObject(pitem->item));
		}
		splan->args = args;

		result = (Node *) splan;
		isInitPlan = false;
	}

	/*
	 * Add the subplan and its rtable to the global lists.
	 */
	root->glob->subplans = lappend(root->glob->subplans,
								   plan);
	root->glob->subrtables = lappend(root->glob->subrtables,
									 subroot->parse->rtable);
	splan->plan_id = list_length(root->glob->subplans);

	if (isInitPlan)
		root->init_plans = lappend(root->init_plans, splan);

	/*
	 * A parameterless subplan (not initplan) should be prepared to handle
	 * REWIND efficiently.	If it has direct parameters then there's no point
	 * since it'll be reset on each scan anyway; and if it's an initplan then
	 * there's no point since it won't get re-run without parameter changes
	 * anyway.	The input of a hashed subplan doesn't need REWIND either.
	 */
	if (splan->parParam == NIL && !isInitPlan && !splan->useHashTable)
		root->glob->rewindPlanIDs = bms_add_member(root->glob->rewindPlanIDs,
												   splan->plan_id);

	return result;
}

/*
 * generate_subquery_params: build a list of Params representing the output
 * columns of a sublink's sub-select, given the sub-select's targetlist.
 *
 * We also return an integer list of the paramids of the Params.
 */
static List *
generate_subquery_params(PlannerInfo *root, List *tlist, List **paramIds)
{
	List	   *result;
	List	   *ids;
	ListCell   *lc;

	result = ids = NIL;
	foreach(lc, tlist)
	{
		TargetEntry *tent = (TargetEntry *) lfirst(lc);
		Param	   *param;

		if (tent->resjunk)
			continue;

		param = generate_new_param(root,
								   exprType((Node *) tent->expr),
								   exprTypmod((Node *) tent->expr));
		result = lappend(result, param);
		ids = lappend_int(ids, param->paramid);
	}

	*paramIds = ids;
	return result;
}

/*
 * generate_subquery_vars: build a list of Vars representing the output
 * columns of a sublink's sub-select, given the sub-select's targetlist.
 * The Vars have the specified varno (RTE index).
 */
static List *
generate_subquery_vars(PlannerInfo *root, List *tlist, Index varno)
{
	List	   *result;
	ListCell   *lc;

	result = NIL;
	foreach(lc, tlist)
	{
		TargetEntry *tent = (TargetEntry *) lfirst(lc);
		Var		   *var;

		if (tent->resjunk)
			continue;

		var = makeVar(varno,
					  tent->resno,
					  exprType((Node *) tent->expr),
					  exprTypmod((Node *) tent->expr),
					  0);
		result = lappend(result, var);
	}

	return result;
}

/*
 * convert_testexpr: convert the testexpr given by the parser into
 * actually executable form.  This entails replacing PARAM_SUBLINK Params
 * with Params or Vars representing the results of the sub-select.  The
 * nodes to be substituted are passed in as the List result from
 * generate_subquery_params or generate_subquery_vars.
 *
 * The given testexpr has already been recursively processed by
 * process_sublinks_mutator.  Hence it can no longer contain any
 * PARAM_SUBLINK Params for lower SubLink nodes; we can safely assume that
 * any we find are for our own level of SubLink.
 */
static Node *
convert_testexpr(PlannerInfo *root,
				 Node *testexpr,
				 List *subst_nodes)
{
	convert_testexpr_context context;

	context.root = root;
	context.subst_nodes = subst_nodes;
	return convert_testexpr_mutator(testexpr, &context);
}

static Node *
convert_testexpr_mutator(Node *node,
						 convert_testexpr_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_SUBLINK)
		{
			if (param->paramid <= 0 ||
				param->paramid > list_length(context->subst_nodes))
				elog(ERROR, "unexpected PARAM_SUBLINK ID: %d", param->paramid);

			/*
			 * We copy the list item to avoid having doubly-linked
			 * substructure in the modified parse tree.  This is probably
			 * unnecessary when it's a Param, but be safe.
			 */
			return (Node *) copyObject(list_nth(context->subst_nodes,
												param->paramid - 1));
		}
	}
	return expression_tree_mutator(node,
								   convert_testexpr_mutator,
								   (void *) context);
}

/*
 * subplan_is_hashable: decide whether we can implement a subplan by hashing
 *
 * Caution: the SubPlan node is not completely filled in yet.  We can rely
 * on its plan and parParam fields, however.
 */
static bool
subplan_is_hashable(SubLink *slink, SubPlan *node, Plan *plan)
{
	double		subquery_size;
	ListCell   *l;

	/*
	 * The sublink type must be "= ANY" --- that is, an IN operator.  We
	 * expect that the test expression will be either a single OpExpr, or an
	 * AND-clause containing OpExprs.  (If it's anything else then the parser
	 * must have determined that the operators have non-equality-like
	 * semantics.  In the OpExpr case we can't be sure what the operator's
	 * semantics are like, but the test below for hashability will reject
	 * anything that's not equality.)
	 */
	if (slink->subLinkType != ANY_SUBLINK)
		return false;
	if (slink->testexpr == NULL ||
		(!IsA(slink->testexpr, OpExpr) &&
		 !and_clause(slink->testexpr)))
		return false;

	/*
	 * The subplan must not have any direct correlation vars --- else we'd
	 * have to recompute its output each time, so that the hashtable wouldn't
	 * gain anything.
	 */
	if (node->parParam != NIL)
		return false;

	/*
	 * The estimated size of the subquery result must fit in work_mem. (Note:
	 * we use sizeof(HeapTupleHeaderData) here even though the tuples will
	 * actually be stored as MinimalTuples; this provides some fudge factor
	 * for hashtable overhead.)
	 */
	subquery_size = plan->plan_rows *
		(MAXALIGN(plan->plan_width) + MAXALIGN(sizeof(HeapTupleHeaderData)));
	if (subquery_size > work_mem * 1024L)
		return false;

	/*
	 * The combining operators must be hashable and strict. The need for
	 * hashability is obvious, since we want to use hashing. Without
	 * strictness, behavior in the presence of nulls is too unpredictable.	We
	 * actually must assume even more than plain strictness: they can't yield
	 * NULL for non-null inputs, either (see nodeSubplan.c).  However, hash
	 * indexes and hash joins assume that too.
	 */
	if (IsA(slink->testexpr, OpExpr))
	{
		if (!hash_ok_operator((OpExpr *) slink->testexpr))
			return false;
	}
	else
	{
		foreach(l, ((BoolExpr *) slink->testexpr)->args)
		{
			Node	   *andarg = (Node *) lfirst(l);

			if (!IsA(andarg, OpExpr))
				return false;	/* probably can't happen */
			if (!hash_ok_operator((OpExpr *) andarg))
				return false;
		}
	}

	return true;
}

static bool
hash_ok_operator(OpExpr *expr)
{
	Oid			opid = expr->opno;
	HeapTuple	tup;
	Form_pg_operator optup;

	tup = SearchSysCache(OPEROID,
						 ObjectIdGetDatum(opid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for operator %u", opid);
	optup = (Form_pg_operator) GETSTRUCT(tup);
	if (!optup->oprcanhash || !func_strict(optup->oprcode))
	{
		ReleaseSysCache(tup);
		return false;
	}
	ReleaseSysCache(tup);
	return true;
}

/*
 * convert_ANY_sublink_to_join: can we convert an ANY SubLink to a join?
 *
 * The caller has found an ANY SubLink at the top level of one of the query's
 * qual clauses, but has not checked the properties of the SubLink further.
 * Decide whether it is appropriate to process this SubLink in join style.
 * Return TRUE if so, FALSE if the SubLink cannot be converted.
 *
 * The only non-obvious input parameter is available_rels: this is the set
 * of query rels that can safely be referenced in the sublink expression.
 * (We must restrict this to avoid changing the semantics when a sublink
 * is present in an outer join's ON qual.)  The conversion must fail if
 * the converted qual would reference any but these parent-query relids.
 *
 * On success, two output parameters are returned:
 *	*new_qual is set to the qual tree that should replace the SubLink in
 *		the parent query's qual tree.  The qual clauses are wrapped in a
 *		FlattenedSubLink node to help later processing place them properly.
 *	*fromlist is set to a list of pulled-up jointree item(s) that must be
 *		added at the proper spot in the parent query's jointree.
 *
 * Side effects of a successful conversion include adding the SubLink's
 * subselect to the query's rangetable.
 */
bool
convert_ANY_sublink_to_join(PlannerInfo *root, SubLink *sublink,
							Relids available_rels,
							Node **new_qual, List **fromlist)
{
	Query	   *parse = root->parse;
	Query	   *subselect = (Query *) sublink->subselect;
	Relids		left_varnos;
	int			rtindex;
	RangeTblEntry *rte;
	RangeTblRef *rtr;
	List	   *subquery_vars;
	Expr	   *quals;
	FlattenedSubLink *fslink;

	Assert(sublink->subLinkType == ANY_SUBLINK);

	/*
	 * The sub-select must not refer to any Vars of the parent query. (Vars of
	 * higher levels should be okay, though.)
	 */
	if (contain_vars_of_level((Node *) subselect, 1))
		return false;

	/*
	 * The test expression must contain some Vars of the current query,
	 * else it's not gonna be a join.  (Note that it won't have Vars
	 * referring to the subquery, rather Params.)
	 */
	left_varnos = pull_varnos(sublink->testexpr);
	if (bms_is_empty(left_varnos))
		return false;

	/*
	 * However, it can't refer to anything outside available_rels.
	 */
	if (!bms_is_subset(left_varnos, available_rels))
		return false;

	/*
	 * The combining operators and left-hand expressions mustn't be volatile.
	 */
	if (contain_volatile_functions(sublink->testexpr))
		return false;

	/*
	 * Okay, pull up the sub-select into upper range table.
	 *
	 * We rely here on the assumption that the outer query has no references
	 * to the inner (necessarily true, other than the Vars that we build
	 * below). Therefore this is a lot easier than what pull_up_subqueries has
	 * to go through.
	 */
	rte = addRangeTableEntryForSubquery(NULL,
										subselect,
										makeAlias("ANY_subquery", NIL),
										false);
	parse->rtable = lappend(parse->rtable, rte);
	rtindex = list_length(parse->rtable);

	/*
	 * Form a RangeTblRef for the pulled-up sub-select.  This must be added
	 * to the upper jointree, but it is caller's responsibility to figure
	 * out where.
	 */
	rtr = makeNode(RangeTblRef);
	rtr->rtindex = rtindex;
	*fromlist = list_make1(rtr);

	/*
	 * Build a list of Vars representing the subselect outputs.
	 */
	subquery_vars = generate_subquery_vars(root,
										   subselect->targetList,
										   rtindex);

	/*
	 * Build the replacement qual expression, replacing Params with these Vars.
	 */
	quals = (Expr *) convert_testexpr(root,
									  sublink->testexpr,
									  subquery_vars);

	/*
	 * And finally, build the FlattenedSubLink node.
	 */
	fslink = makeNode(FlattenedSubLink);
	fslink->jointype = JOIN_SEMI;
	fslink->lefthand = left_varnos;
	fslink->righthand = bms_make_singleton(rtindex);
	fslink->quals = quals;

	*new_qual = (Node *) fslink;

	return true;
}

/*
 * simplify_EXISTS_query: remove any useless stuff in an EXISTS's subquery
 *
 * The only thing that matters about an EXISTS query is whether it returns
 * zero or more than zero rows.  Therefore, we can remove certain SQL features
 * that won't affect that.  The only part that is really likely to matter in
 * typical usage is simplifying the targetlist: it's a common habit to write
 * "SELECT * FROM" even though there is no need to evaluate any columns.
 *
 * Note: by suppressing the targetlist we could cause an observable behavioral
 * change, namely that any errors that might occur in evaluating the tlist
 * won't occur, nor will other side-effects of volatile functions.  This seems
 * unlikely to bother anyone in practice.
 *
 * Returns TRUE if was able to discard the targetlist, else FALSE.
 */
static bool
simplify_EXISTS_query(Query *query)
{
	/*
	 * We don't try to simplify at all if the query uses set operations,
	 * aggregates, HAVING, LIMIT/OFFSET, or FOR UPDATE/SHARE; none of these
	 * seem likely in normal usage and their possible effects are complex.
	 */
	if (query->commandType != CMD_SELECT ||
		query->intoClause ||
		query->setOperations ||
		query->hasAggs ||
		query->havingQual ||
		query->limitOffset ||
		query->limitCount ||
		query->rowMarks)
		return false;

	/*
	 * Mustn't throw away the targetlist if it contains set-returning
	 * functions; those could affect whether zero rows are returned!
	 */
	if (expression_returns_set((Node *) query->targetList))
		return false;

	/*
	 * Otherwise, we can throw away the targetlist, as well as any GROUP,
	 * DISTINCT, and ORDER BY clauses; none of those clauses will change
	 * a nonzero-rows result to zero rows or vice versa.  (Furthermore,
	 * since our parsetree representation of these clauses depends on the
	 * targetlist, we'd better throw them away if we drop the targetlist.)
	 */
	query->targetList = NIL;
	query->groupClause = NIL;
	query->distinctClause = NIL;
	query->sortClause = NIL;
	query->hasDistinctOn = false;

	return true;
}

/*
 * convert_EXISTS_sublink_to_join: can we convert an EXISTS SubLink to a join?
 *
 * The API of this function is identical to convert_ANY_sublink_to_join's,
 * except that we also support the case where the caller has found NOT EXISTS,
 * so we need an additional input parameter "under_not".
 */
bool
convert_EXISTS_sublink_to_join(PlannerInfo *root, SubLink *sublink,
							   bool under_not,
							   Relids available_rels,
							   Node **new_qual, List **fromlist)
{
	Query	   *parse = root->parse;
	Query	   *subselect = (Query *) sublink->subselect;
	Node	   *whereClause;
	int			rtoffset;
	int			varno;
	Relids		clause_varnos;
	Relids		left_varnos;
	Relids		right_varnos;
	Relids		subselect_varnos;
	FlattenedSubLink *fslink;

	Assert(sublink->subLinkType == EXISTS_SUBLINK);

	/*
	 * Copy the subquery so we can modify it safely (see comments in
	 * make_subplan).
	 */
	subselect = (Query *) copyObject(subselect);

	/*
	 * See if the subquery can be simplified based on the knowledge that
	 * it's being used in EXISTS().  If we aren't able to get rid of its
	 * targetlist, we have to fail, because the pullup operation leaves
	 * us with noplace to evaluate the targetlist.
	 */
	if (!simplify_EXISTS_query(subselect))
		return false;

	/*
	 * Separate out the WHERE clause.  (We could theoretically also remove
	 * top-level plain JOIN/ON clauses, but it's probably not worth the
	 * trouble.)
	 */
	whereClause = subselect->jointree->quals;
	subselect->jointree->quals = NULL;

	/*
	 * The rest of the sub-select must not refer to any Vars of the parent
	 * query.  (Vars of higher levels should be okay, though.)
	 */
	if (contain_vars_of_level((Node *) subselect, 1))
		return false;

	/*
	 * On the other hand, the WHERE clause must contain some Vars of the
	 * parent query, else it's not gonna be a join.
	 */
	if (!contain_vars_of_level(whereClause, 1))
		return false;

	/*
	 * We don't risk optimizing if the WHERE clause is volatile, either.
	 */
	if (contain_volatile_functions(whereClause))
		return false;

	/*
	 * Prepare to pull up the sub-select into top range table.
	 *
	 * We rely here on the assumption that the outer query has no references
	 * to the inner (necessarily true). Therefore this is a lot easier than
	 * what pull_up_subqueries has to go through.
	 *
	 * In fact, it's even easier than what convert_ANY_sublink_to_join has
	 * to do.  The machinations of simplify_EXISTS_query ensured that there
	 * is nothing interesting in the subquery except an rtable and jointree,
	 * and even the jointree FromExpr no longer has quals.  So we can just
	 * append the rtable to our own and attach the fromlist to our own.
	 * But first, adjust all level-zero varnos in the subquery to account
	 * for the rtable merger.
	 */
	rtoffset = list_length(parse->rtable);
	OffsetVarNodes((Node *) subselect, rtoffset, 0);
	OffsetVarNodes(whereClause, rtoffset, 0);

	/*
	 * Upper-level vars in subquery will now be one level closer to their
	 * parent than before; in particular, anything that had been level 1
	 * becomes level zero.
	 */
	IncrementVarSublevelsUp((Node *) subselect, -1, 1);
	IncrementVarSublevelsUp(whereClause, -1, 1);

	/*
	 * Now that the WHERE clause is adjusted to match the parent query
	 * environment, we can easily identify all the level-zero rels it uses.
	 * The ones <= rtoffset are "left rels" of the join we're forming,
	 * and the ones > rtoffset are "right rels".
	 */
	clause_varnos = pull_varnos(whereClause);
	left_varnos = right_varnos = NULL;
	while ((varno = bms_first_member(clause_varnos)) >= 0)
	{
		if (varno <= rtoffset)
			left_varnos = bms_add_member(left_varnos, varno);
		else
			right_varnos = bms_add_member(right_varnos, varno);
	}
	bms_free(clause_varnos);
	Assert(!bms_is_empty(left_varnos));

	/*
	 * Now that we've got the set of upper-level varnos, we can make the
	 * last check: only available_rels can be referenced.
	 */
	if (!bms_is_subset(left_varnos, available_rels))
		return false;

	/* Identify all the rels syntactically within the subselect */
	subselect_varnos = get_relids_in_jointree((Node *) subselect->jointree,
											  true);
	Assert(bms_is_subset(right_varnos, subselect_varnos));

	/* Now we can attach the modified subquery rtable to the parent */
	parse->rtable = list_concat(parse->rtable, subselect->rtable);

	/*
	 * Pass back the subquery fromlist to be attached to upper jointree
	 * in a suitable place.
	 */
	*fromlist = subselect->jointree->fromlist;

	/*
	 * And finally, build the FlattenedSubLink node.
	 */
	fslink = makeNode(FlattenedSubLink);
	fslink->jointype = under_not ? JOIN_ANTI : JOIN_SEMI;
	fslink->lefthand = left_varnos;
	fslink->righthand = subselect_varnos;
	fslink->quals = (Expr *) whereClause;

	*new_qual = (Node *) fslink;

	return true;
}

/*
 * Replace correlation vars (uplevel vars) with Params.
 *
 * Uplevel aggregates are replaced, too.
 *
 * Note: it is critical that this runs immediately after SS_process_sublinks.
 * Since we do not recurse into the arguments of uplevel aggregates, they will
 * get copied to the appropriate subplan args list in the parent query with
 * uplevel vars not replaced by Params, but only adjusted in level (see
 * replace_outer_agg).	That's exactly what we want for the vars of the parent
 * level --- but if an aggregate's argument contains any further-up variables,
 * they have to be replaced with Params in their turn.	That will happen when
 * the parent level runs SS_replace_correlation_vars.  Therefore it must do
 * so after expanding its sublinks to subplans.  And we don't want any steps
 * in between, else those steps would never get applied to the aggregate
 * argument expressions, either in the parent or the child level.
 */
Node *
SS_replace_correlation_vars(PlannerInfo *root, Node *expr)
{
	/* No setup needed for tree walk, so away we go */
	return replace_correlation_vars_mutator(expr, root);
}

static Node *
replace_correlation_vars_mutator(Node *node, PlannerInfo *root)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup > 0)
			return (Node *) replace_outer_var(root, (Var *) node);
	}
	if (IsA(node, Aggref))
	{
		if (((Aggref *) node)->agglevelsup > 0)
			return (Node *) replace_outer_agg(root, (Aggref *) node);
	}
	return expression_tree_mutator(node,
								   replace_correlation_vars_mutator,
								   (void *) root);
}

/*
 * Expand SubLinks to SubPlans in the given expression.
 *
 * The isQual argument tells whether or not this expression is a WHERE/HAVING
 * qualifier expression.  If it is, any sublinks appearing at top level need
 * not distinguish FALSE from UNKNOWN return values.
 */
Node *
SS_process_sublinks(PlannerInfo *root, Node *expr, bool isQual)
{
	process_sublinks_context context;

	context.root = root;
	context.isTopQual = isQual;
	return process_sublinks_mutator(expr, &context);
}

static Node *
process_sublinks_mutator(Node *node, process_sublinks_context *context)
{
	process_sublinks_context locContext;

	locContext.root = context->root;

	if (node == NULL)
		return NULL;
	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		Node	   *testexpr;

		/*
		 * First, recursively process the lefthand-side expressions, if any.
		 * They're not top-level anymore.
		 */
		locContext.isTopQual = false;
		testexpr = process_sublinks_mutator(sublink->testexpr, &locContext);

		/*
		 * Now build the SubPlan node and make the expr to return.
		 */
		return make_subplan(context->root,
							sublink,
							testexpr,
							context->isTopQual);
	}

	/*
	 * We should never see a SubPlan expression in the input (since this is
	 * the very routine that creates 'em to begin with).  We shouldn't find
	 * ourselves invoked directly on a Query, either.
	 */
	Assert(!is_subplan(node));
	Assert(!IsA(node, Query));

	/*
	 * Because make_subplan() could return an AND or OR clause, we have to
	 * take steps to preserve AND/OR flatness of a qual.  We assume the input
	 * has been AND/OR flattened and so we need no recursion here.
	 *
	 * (Due to the coding here, we will not get called on the List subnodes of
	 * an AND; and the input is *not* yet in implicit-AND format.  So no check
	 * is needed for a bare List.)
	 *
	 * Anywhere within the top-level AND/OR clause structure, we can tell
	 * make_subplan() that NULL and FALSE are interchangeable.  So isTopQual
	 * propagates down in both cases.  (Note that this is unlike the meaning
	 * of "top level qual" used in most other places in Postgres.)
	 */
	if (and_clause(node))
	{
		List	   *newargs = NIL;
		ListCell   *l;

		/* Still at qual top-level */
		locContext.isTopQual = context->isTopQual;

		foreach(l, ((BoolExpr *) node)->args)
		{
			Node	   *newarg;

			newarg = process_sublinks_mutator(lfirst(l), &locContext);
			if (and_clause(newarg))
				newargs = list_concat(newargs, ((BoolExpr *) newarg)->args);
			else
				newargs = lappend(newargs, newarg);
		}
		return (Node *) make_andclause(newargs);
	}

	if (or_clause(node))
	{
		List	   *newargs = NIL;
		ListCell   *l;

		/* Still at qual top-level */
		locContext.isTopQual = context->isTopQual;

		foreach(l, ((BoolExpr *) node)->args)
		{
			Node	   *newarg;

			newarg = process_sublinks_mutator(lfirst(l), &locContext);
			if (or_clause(newarg))
				newargs = list_concat(newargs, ((BoolExpr *) newarg)->args);
			else
				newargs = lappend(newargs, newarg);
		}
		return (Node *) make_orclause(newargs);
	}

	/*
	 * If we recurse down through anything other than an AND or OR node,
	 * we are definitely not at top qual level anymore.
	 */
	locContext.isTopQual = false;

	return expression_tree_mutator(node,
								   process_sublinks_mutator,
								   (void *) &locContext);
}

/*
 * SS_finalize_plan - do final sublink processing for a completed Plan.
 *
 * This recursively computes the extParam and allParam sets for every Plan
 * node in the given plan tree.  It also optionally attaches any previously
 * generated InitPlans to the top plan node.  (Any InitPlans should already
 * have been put through SS_finalize_plan.)
 */
void
SS_finalize_plan(PlannerInfo *root, Plan *plan, bool attach_initplans)
{
	Bitmapset  *valid_params,
			   *initExtParam,
			   *initSetParam;
	Cost		initplan_cost;
	int			paramid;
	ListCell   *l;

	/*
	 * Examine any initPlans to determine the set of external params they
	 * reference, the set of output params they supply, and their total cost.
	 * We'll use at least some of this info below.  (Note we are assuming that
	 * finalize_plan doesn't touch the initPlans.)
	 *
	 * In the case where attach_initplans is false, we are assuming that the
	 * existing initPlans are siblings that might supply params needed by the
	 * current plan.
	 */
	initExtParam = initSetParam = NULL;
	initplan_cost = 0;
	foreach(l, root->init_plans)
	{
		SubPlan    *initsubplan = (SubPlan *) lfirst(l);
		Plan	   *initplan = planner_subplan_get_plan(root, initsubplan);
		ListCell   *l2;

		initExtParam = bms_add_members(initExtParam, initplan->extParam);
		foreach(l2, initsubplan->setParam)
		{
			initSetParam = bms_add_member(initSetParam, lfirst_int(l2));
		}
		initplan_cost += get_initplan_cost(root, initsubplan);
	}

	/*
	 * Now determine the set of params that are validly referenceable in this
	 * query level; to wit, those available from outer query levels plus the
	 * output parameters of any initPlans.  (We do not include output
	 * parameters of regular subplans.  Those should only appear within the
	 * testexpr of SubPlan nodes, and are taken care of locally within
	 * finalize_primnode.)
	 *
	 * Note: this is a bit overly generous since some parameters of upper
	 * query levels might belong to query subtrees that don't include this
	 * query.  However, valid_params is only a debugging crosscheck, so it
	 * doesn't seem worth expending lots of cycles to try to be exact.
	 */
	valid_params = bms_copy(initSetParam);
	paramid = 0;
	foreach(l, root->glob->paramlist)
	{
		PlannerParamItem *pitem = (PlannerParamItem *) lfirst(l);

		if (pitem->abslevel < root->query_level)
		{
			/* valid outer-level parameter */
			valid_params = bms_add_member(valid_params, paramid);
		}

		paramid++;
	}

	/*
	 * Now recurse through plan tree.
	 */
	(void) finalize_plan(root, plan, valid_params);

	bms_free(valid_params);

	/*
	 * Finally, attach any initPlans to the topmost plan node, and add their
	 * extParams to the topmost node's, too.  However, any setParams of the
	 * initPlans should not be present in the topmost node's extParams, only
	 * in its allParams.  (As of PG 8.1, it's possible that some initPlans
	 * have extParams that are setParams of other initPlans, so we have to
	 * take care of this situation explicitly.)
	 *
	 * We also add the eval cost of each initPlan to the startup cost of the
	 * top node.  This is a conservative overestimate, since in fact each
	 * initPlan might be executed later than plan startup, or even not at all.
	 */
	if (attach_initplans)
	{
		plan->initPlan = root->init_plans;
		root->init_plans = NIL;		/* make sure they're not attached twice */

		/* allParam must include all these params */
		plan->allParam = bms_add_members(plan->allParam, initExtParam);
		plan->allParam = bms_add_members(plan->allParam, initSetParam);
		/* extParam must include any child extParam */
		plan->extParam = bms_add_members(plan->extParam, initExtParam);
		/* but extParam shouldn't include any setParams */
		plan->extParam = bms_del_members(plan->extParam, initSetParam);
		/* ensure extParam is exactly NULL if it's empty */
		if (bms_is_empty(plan->extParam))
			plan->extParam = NULL;

		plan->startup_cost += initplan_cost;
		plan->total_cost += initplan_cost;
	}
}

/*
 * Recursive processing of all nodes in the plan tree
 *
 * The return value is the computed allParam set for the given Plan node.
 * This is just an internal notational convenience.
 */
static Bitmapset *
finalize_plan(PlannerInfo *root, Plan *plan, Bitmapset *valid_params)
{
	finalize_primnode_context context;

	if (plan == NULL)
		return NULL;

	context.root = root;
	context.paramids = NULL;	/* initialize set to empty */

	/*
	 * When we call finalize_primnode, context.paramids sets are automatically
	 * merged together.  But when recursing to self, we have to do it the hard
	 * way.  We want the paramids set to include params in subplans as well as
	 * at this level.
	 */

	/* Find params in targetlist and qual */
	finalize_primnode((Node *) plan->targetlist, &context);
	finalize_primnode((Node *) plan->qual, &context);

	/* Check additional node-type-specific fields */
	switch (nodeTag(plan))
	{
		case T_Result:
			finalize_primnode(((Result *) plan)->resconstantqual,
							  &context);
			break;

		case T_IndexScan:
			finalize_primnode((Node *) ((IndexScan *) plan)->indexqual,
							  &context);

			/*
			 * we need not look at indexqualorig, since it will have the same
			 * param references as indexqual.
			 */
			break;

		case T_BitmapIndexScan:
			finalize_primnode((Node *) ((BitmapIndexScan *) plan)->indexqual,
							  &context);

			/*
			 * we need not look at indexqualorig, since it will have the same
			 * param references as indexqual.
			 */
			break;

		case T_BitmapHeapScan:
			finalize_primnode((Node *) ((BitmapHeapScan *) plan)->bitmapqualorig,
							  &context);
			break;

		case T_TidScan:
			finalize_primnode((Node *) ((TidScan *) plan)->tidquals,
							  &context);
			break;

		case T_SubqueryScan:

			/*
			 * In a SubqueryScan, SS_finalize_plan has already been run on the
			 * subplan by the inner invocation of subquery_planner, so there's
			 * no need to do it again.	Instead, just pull out the subplan's
			 * extParams list, which represents the params it needs from my
			 * level and higher levels.
			 */
			context.paramids = bms_add_members(context.paramids,
								 ((SubqueryScan *) plan)->subplan->extParam);
			break;

		case T_FunctionScan:
			finalize_primnode(((FunctionScan *) plan)->funcexpr,
							  &context);
			break;

		case T_ValuesScan:
			finalize_primnode((Node *) ((ValuesScan *) plan)->values_lists,
							  &context);
			break;

		case T_Append:
			{
				ListCell   *l;

				foreach(l, ((Append *) plan)->appendplans)
				{
					context.paramids =
						bms_add_members(context.paramids,
										finalize_plan(root,
													  (Plan *) lfirst(l),
													  valid_params));
				}
			}
			break;

		case T_BitmapAnd:
			{
				ListCell   *l;

				foreach(l, ((BitmapAnd *) plan)->bitmapplans)
				{
					context.paramids =
						bms_add_members(context.paramids,
										finalize_plan(root,
													  (Plan *) lfirst(l),
													  valid_params));
				}
			}
			break;

		case T_BitmapOr:
			{
				ListCell   *l;

				foreach(l, ((BitmapOr *) plan)->bitmapplans)
				{
					context.paramids =
						bms_add_members(context.paramids,
										finalize_plan(root,
													  (Plan *) lfirst(l),
													  valid_params));
				}
			}
			break;

		case T_NestLoop:
			finalize_primnode((Node *) ((Join *) plan)->joinqual,
							  &context);
			break;

		case T_MergeJoin:
			finalize_primnode((Node *) ((Join *) plan)->joinqual,
							  &context);
			finalize_primnode((Node *) ((MergeJoin *) plan)->mergeclauses,
							  &context);
			break;

		case T_HashJoin:
			finalize_primnode((Node *) ((Join *) plan)->joinqual,
							  &context);
			finalize_primnode((Node *) ((HashJoin *) plan)->hashclauses,
							  &context);
			break;

		case T_Limit:
			finalize_primnode(((Limit *) plan)->limitOffset,
							  &context);
			finalize_primnode(((Limit *) plan)->limitCount,
							  &context);
			break;

		case T_Hash:
		case T_Agg:
		case T_SeqScan:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_SetOp:
		case T_Group:
			break;

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(plan));
	}

	/* Process left and right child plans, if any */
	context.paramids = bms_add_members(context.paramids,
									   finalize_plan(root,
													 plan->lefttree,
													 valid_params));

	context.paramids = bms_add_members(context.paramids,
									   finalize_plan(root,
													 plan->righttree,
													 valid_params));

	/* Now we have all the paramids */

	if (!bms_is_subset(context.paramids, valid_params))
		elog(ERROR, "plan should not reference subplan's variable");

	/*
	 * Note: by definition, extParam and allParam should have the same value
	 * in any plan node that doesn't have child initPlans.  We set them
	 * equal here, and later SS_finalize_plan will update them properly
	 * in node(s) that it attaches initPlans to.
	 *
	 * For speed at execution time, make sure extParam/allParam are actually
	 * NULL if they are empty sets.
	 */
	if (bms_is_empty(context.paramids))
	{
		plan->extParam = NULL;
		plan->allParam = NULL;
	}
	else
	{
		plan->extParam = context.paramids;
		plan->allParam = bms_copy(context.paramids);
	}

	return plan->allParam;
}

/*
 * finalize_primnode: add IDs of all PARAM_EXEC params appearing in the given
 * expression tree to the result set.
 */
static bool
finalize_primnode(Node *node, finalize_primnode_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		if (((Param *) node)->paramkind == PARAM_EXEC)
		{
			int			paramid = ((Param *) node)->paramid;

			context->paramids = bms_add_member(context->paramids, paramid);
		}
		return false;			/* no more to do here */
	}
	if (is_subplan(node))
	{
		SubPlan    *subplan = (SubPlan *) node;
		Plan	   *plan = planner_subplan_get_plan(context->root, subplan);
		ListCell   *lc;
		Bitmapset  *subparamids;

		/* Recurse into the testexpr, but not into the Plan */
		finalize_primnode(subplan->testexpr, context);

		/*
		 * Remove any param IDs of output parameters of the subplan that were
		 * referenced in the testexpr.  These are not interesting for
		 * parameter change signaling since we always re-evaluate the subplan.
		 * Note that this wouldn't work too well if there might be uses of the
		 * same param IDs elsewhere in the plan, but that can't happen because
		 * generate_new_param never tries to merge params.
		 */
		foreach(lc, subplan->paramIds)
		{
			context->paramids = bms_del_member(context->paramids,
											   lfirst_int(lc));
		}

		/* Also examine args list */
		finalize_primnode((Node *) subplan->args, context);

		/*
		 * Add params needed by the subplan to paramids, but excluding those
		 * we will pass down to it.
		 */
		subparamids = bms_copy(plan->extParam);
		foreach(lc, subplan->parParam)
		{
			subparamids = bms_del_member(subparamids, lfirst_int(lc));
		}
		context->paramids = bms_join(context->paramids, subparamids);

		return false;			/* no more to do here */
	}
	return expression_tree_walker(node, finalize_primnode,
								  (void *) context);
}

/*
 * SS_make_initplan_from_plan - given a plan tree, make it an InitPlan
 *
 * The plan is expected to return a scalar value of the indicated type.
 * We build an EXPR_SUBLINK SubPlan node and put it into the initplan
 * list for the current query level.  A Param that represents the initplan's
 * output is returned.
 *
 * We assume the plan hasn't been put through SS_finalize_plan.
 */
Param *
SS_make_initplan_from_plan(PlannerInfo *root, Plan *plan,
						   Oid resulttype, int32 resulttypmod)
{
	SubPlan    *node;
	Param	   *prm;

	/*
	 * We must run SS_finalize_plan(), since that's normally done before a
	 * subplan gets put into the initplan list.  Tell it not to attach any
	 * pre-existing initplans to this one, since they are siblings not
	 * children of this initplan.  (This is something else that could perhaps
	 * be cleaner if we did extParam/allParam processing in setrefs.c instead
	 * of here?  See notes for materialize_finished_plan.)
	 */

	/*
	 * Build extParam/allParam sets for plan nodes.
	 */
	SS_finalize_plan(root, plan, false);

	/*
	 * Add the subplan and its rtable to the global lists.
	 */
	root->glob->subplans = lappend(root->glob->subplans,
								   plan);
	root->glob->subrtables = lappend(root->glob->subrtables,
									 root->parse->rtable);

	/*
	 * Create a SubPlan node and add it to the outer list of InitPlans.
	 * Note it has to appear after any other InitPlans it might depend on
	 * (see comments in ExecReScan).
	 */
	node = makeNode(SubPlan);
	node->subLinkType = EXPR_SUBLINK;
	node->firstColType = get_first_col_type(plan);
	node->plan_id = list_length(root->glob->subplans);

	root->init_plans = lappend(root->init_plans, node);

	/*
	 * The node can't have any inputs (since it's an initplan), so the
	 * parParam and args lists remain empty.
	 */

	/*
	 * Make a Param that will be the subplan's output.
	 */
	prm = generate_new_param(root, resulttype, resulttypmod);
	node->setParam = list_make1_int(prm->paramid);

	return prm;
}
