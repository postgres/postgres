/*-------------------------------------------------------------------------
 *
 * subselect.c
 *	  Planning routines for subselects and parameters.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/plan/subselect.c,v 1.112.2.4 2008/07/10 01:17:44 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/subselect.h"
#include "optimizer/var.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


Index		PlannerQueryLevel;	/* level of current query */
List	   *PlannerInitPlan;	/* init subplans for current query */
List	   *PlannerParamList;	/* to keep track of cross-level Params */

int			PlannerPlanId = 0;	/* to assign unique ID to subquery plans */

/*
 * PlannerParamList keeps track of the PARAM_EXEC slots that we have decided
 * we need for the query.  At runtime these slots are used to pass values
 * either down into subqueries (for outer references in subqueries) or up out
 * of subqueries (for the results of a subplan).  The n'th entry in the list
 * (n counts from 0) corresponds to Param->paramid = n.
 *
 * Each ParamList item shows the absolute query level it is associated with,
 * where the outermost query is level 1 and nested subqueries have higher
 * numbers.  The item the parameter slot represents can be one of three kinds:
 *
 * A Var: the slot represents a variable of that level that must be passed
 * down because subqueries have outer references to it.  The varlevelsup
 * value in the Var will always be zero.
 *
 * An Aggref (with an expression tree representing its argument): the slot
 * represents an aggregate expression that is an outer reference for some
 * subquery.  The Aggref itself has agglevelsup = 0, and its argument tree
 * is adjusted to match in level.
 *
 * A Param: the slot holds the result of a subplan (it is a setParam item
 * for that subplan).  The absolute level shown for such items corresponds
 * to the parent query of the subplan.
 *
 * Note: we detect duplicate Var parameters and coalesce them into one slot,
 * but we do not do this for Aggref or Param slots.
 */
typedef struct PlannerParamItem
{
	Node	   *item;			/* the Var, Aggref, or Param */
	Index		abslevel;		/* its absolute query level */
} PlannerParamItem;


typedef struct convert_testexpr_context
{
	List	   *subst_nodes;	/* Nodes to substitute for Params */
} convert_testexpr_context;

typedef struct finalize_primnode_context
{
	Bitmapset  *paramids;		/* Non-local PARAM_EXEC paramids found */
} finalize_primnode_context;


static List *generate_subquery_params(List *tlist, List **paramIds);
static List *generate_subquery_vars(List *tlist, Index varno);
static Node *convert_testexpr(Node *testexpr,
							  List *subst_nodes);
static Node *convert_testexpr_mutator(Node *node,
						 convert_testexpr_context *context);
static bool subplan_is_hashable(SubLink *slink, SubPlan *node);
static bool hash_ok_operator(OpExpr *expr);
static Node *replace_correlation_vars_mutator(Node *node, void *context);
static Node *process_sublinks_mutator(Node *node, bool *isTopQual);
static Bitmapset *finalize_plan(Plan *plan, List *rtable,
			  Bitmapset *valid_params);
static bool finalize_primnode(Node *node, finalize_primnode_context *context);


/*
 * Generate a Param node to replace the given Var,
 * which is expected to have varlevelsup > 0 (ie, it is not local).
 */
static Param *
replace_outer_var(Var *var)
{
	Param	   *retval;
	ListCell   *ppl;
	PlannerParamItem *pitem;
	Index		abslevel;
	int			i;

	Assert(var->varlevelsup > 0 && var->varlevelsup < PlannerQueryLevel);
	abslevel = PlannerQueryLevel - var->varlevelsup;

	/*
	 * If there's already a PlannerParamList entry for this same Var, just use
	 * it.	NOTE: in sufficiently complex querytrees, it is possible for the
	 * same varno/abslevel to refer to different RTEs in different parts of
	 * the parsetree, so that different fields might end up sharing the same
	 * Param number.  As long as we check the vartype as well, I believe that
	 * this sort of aliasing will cause no trouble. The correct field should
	 * get stored into the Param slot at execution in each part of the tree.
	 *
	 * We also need to demand a match on vartypmod.  This does not matter for
	 * the Param itself, since those are not typmod-dependent, but it does
	 * matter when make_subplan() instantiates a modified copy of the Var for
	 * a subplan's args list.
	 */
	i = 0;
	foreach(ppl, PlannerParamList)
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

		pitem = (PlannerParamItem *) palloc(sizeof(PlannerParamItem));
		pitem->item = (Node *) var;
		pitem->abslevel = abslevel;

		PlannerParamList = lappend(PlannerParamList, pitem);
		/* i is already the correct index for the new item */
	}

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = i;
	retval->paramtype = var->vartype;

	return retval;
}

/*
 * Generate a Param node to replace the given Aggref
 * which is expected to have agglevelsup > 0 (ie, it is not local).
 */
static Param *
replace_outer_agg(Aggref *agg)
{
	Param	   *retval;
	PlannerParamItem *pitem;
	Index		abslevel;
	int			i;

	Assert(agg->agglevelsup > 0 && agg->agglevelsup < PlannerQueryLevel);
	abslevel = PlannerQueryLevel - agg->agglevelsup;

	/*
	 * It does not seem worthwhile to try to match duplicate outer aggs. Just
	 * make a new slot every time.
	 */
	agg = (Aggref *) copyObject(agg);
	IncrementVarSublevelsUp((Node *) agg, -((int) agg->agglevelsup), 0);
	Assert(agg->agglevelsup == 0);

	pitem = (PlannerParamItem *) palloc(sizeof(PlannerParamItem));
	pitem->item = (Node *) agg;
	pitem->abslevel = abslevel;

	PlannerParamList = lappend(PlannerParamList, pitem);
	i = list_length(PlannerParamList) - 1;

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = i;
	retval->paramtype = agg->aggtype;

	return retval;
}

/*
 * Generate a new Param node that will not conflict with any other.
 *
 * This is used to allocate PARAM_EXEC slots for subplan outputs.
 *
 * paramtypmod is currently unused but might be wanted someday.
 */
static Param *
generate_new_param(Oid paramtype, int32 paramtypmod)
{
	Param	   *retval;
	PlannerParamItem *pitem;

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = list_length(PlannerParamList);
	retval->paramtype = paramtype;

	pitem = (PlannerParamItem *) palloc(sizeof(PlannerParamItem));
	pitem->item = (Node *) retval;
	pitem->abslevel = PlannerQueryLevel;

	PlannerParamList = lappend(PlannerParamList, pitem);

	return retval;
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
make_subplan(SubLink *slink, Node *testexpr, bool isTopQual)
{
	SubPlan    *node = makeNode(SubPlan);
	Query	   *subquery = (Query *) (slink->subselect);
	double		tuple_fraction;
	Plan	   *plan;
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
	 * For an EXISTS subplan, tell lower-level planner to expect that only the
	 * first tuple will be retrieved.  For ALL and ANY subplans, we will be
	 * able to stop evaluating if the test condition fails, so very often not
	 * all the tuples will be retrieved; for lack of a better idea, specify
	 * 50% retrieval.  For EXPR and ROWCOMPARE subplans, use default behavior
	 * (we're only expecting one row out, anyway).
	 *
	 * NOTE: if you change these numbers, also change cost_qual_eval_walker()
	 * in path/costsize.c.
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
	node->plan = plan = subquery_planner(subquery, tuple_fraction, NULL);

	node->plan_id = PlannerPlanId++;	/* Assign unique ID to this SubPlan */

	node->rtable = subquery->rtable;

	/*
	 * Initialize other fields of the SubPlan node.
	 */
	node->subLinkType = slink->subLinkType;
	node->testexpr = NULL;
	node->paramIds = NIL;
	node->useHashTable = false;
	/* At top level of a qual, can treat UNKNOWN the same as FALSE */
	node->unknownEqFalse = isTopQual;
	node->setParam = NIL;
	node->parParam = NIL;
	node->args = NIL;

	/*
	 * Make parParam list of params that current query level will pass to this
	 * child plan.
	 */
	tmpset = bms_copy(plan->extParam);
	while ((paramid = bms_first_member(tmpset)) >= 0)
	{
		PlannerParamItem *pitem = list_nth(PlannerParamList, paramid);

		if (pitem->abslevel == PlannerQueryLevel)
			node->parParam = lappend_int(node->parParam, paramid);
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
	if (node->parParam == NIL && slink->subLinkType == EXISTS_SUBLINK)
	{
		Param	   *prm;

		prm = generate_new_param(BOOLOID, -1);
		node->setParam = list_make1_int(prm->paramid);
		PlannerInitPlan = lappend(PlannerInitPlan, node);
		result = (Node *) prm;
	}
	else if (node->parParam == NIL && slink->subLinkType == EXPR_SUBLINK)
	{
		TargetEntry *te = linitial(plan->targetlist);
		Param	   *prm;

		Assert(!te->resjunk);
		prm = generate_new_param(exprType((Node *) te->expr),
								 exprTypmod((Node *) te->expr));
		node->setParam = list_make1_int(prm->paramid);
		PlannerInitPlan = lappend(PlannerInitPlan, node);
		result = (Node *) prm;
	}
	else if (node->parParam == NIL && slink->subLinkType == ARRAY_SUBLINK)
	{
		TargetEntry *te = linitial(plan->targetlist);
		Oid			arraytype;
		Param	   *prm;

		Assert(!te->resjunk);
		arraytype = get_array_type(exprType((Node *) te->expr));
		if (!OidIsValid(arraytype))
			elog(ERROR, "could not find array type for datatype %s",
				 format_type_be(exprType((Node *) te->expr)));
		prm = generate_new_param(arraytype, -1);
		node->setParam = list_make1_int(prm->paramid);
		PlannerInitPlan = lappend(PlannerInitPlan, node);
		result = (Node *) prm;
	}
	else if (node->parParam == NIL && slink->subLinkType == ROWCOMPARE_SUBLINK)
	{
		/* Adjust the Params */
		List	   *params;

		params = generate_subquery_params(plan->targetlist,
										  &node->paramIds);
		result = convert_testexpr(testexpr,
								  params);
		node->setParam = list_copy(node->paramIds);
		PlannerInitPlan = lappend(PlannerInitPlan, node);

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
			params = generate_subquery_params(plan->targetlist,
											  &node->paramIds);
			node->testexpr = convert_testexpr(testexpr,
											  params);
		}

		/*
		 * We can't convert subplans of ALL_SUBLINK or ANY_SUBLINK types to
		 * initPlans, even when they are uncorrelated or undirect correlated,
		 * because we need to scan the output of the subplan for each outer
		 * tuple.  But if it's an IN (= ANY) test, we might be able to use a
		 * hashtable to avoid comparing all the tuples.
		 */
		if (subplan_is_hashable(slink, node))
			node->useHashTable = true;

		/*
		 * Otherwise, we have the option to tack a MATERIAL node onto the top
		 * of the subplan, to reduce the cost of reading it repeatedly.  This
		 * is pointless for a direct-correlated subplan, since we'd have to
		 * recompute its results each time anyway.	For uncorrelated/undirect
		 * correlated subplans, we add MATERIAL unless the subplan's top plan
		 * node would materialize its output anyway.
		 */
		else if (node->parParam == NIL)
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
				node->plan = plan = materialize_finished_plan(plan);
		}

		/*
		 * Make node->args from parParam.
		 */
		args = NIL;
		foreach(l, node->parParam)
		{
			PlannerParamItem *pitem = list_nth(PlannerParamList, lfirst_int(l));

			/*
			 * The Var or Aggref has already been adjusted to have the correct
			 * varlevelsup or agglevelsup.	We probably don't even need to
			 * copy it again, but be safe.
			 */
			args = lappend(args, copyObject(pitem->item));
		}
		node->args = args;

		result = (Node *) node;
	}

	return result;
}

/*
 * generate_subquery_params: build a list of Params representing the output
 * columns of a sublink's sub-select, given the sub-select's targetlist.
 *
 * We also return an integer list of the paramids of the Params.
 */
static List *
generate_subquery_params(List *tlist, List **paramIds)
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

		param = generate_new_param(exprType((Node *) tent->expr),
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
generate_subquery_vars(List *tlist, Index varno)
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
convert_testexpr(Node *testexpr,
				 List *subst_nodes)
{
	convert_testexpr_context context;

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
subplan_is_hashable(SubLink *slink, SubPlan *node)
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
	subquery_size = node->plan->plan_rows *
		(MAXALIGN(node->plan->plan_width) + MAXALIGN(sizeof(HeapTupleHeaderData)));
	if (subquery_size > work_mem * 1024L)
		return false;

	/*
	 * The combining operators must be hashable, strict, and self-commutative.
	 * The need for hashability is obvious, since we want to use hashing.
	 * Without strictness, behavior in the presence of nulls is too
	 * unpredictable.  (We actually must assume even more than plain
	 * strictness, see nodeSubplan.c for details.)	And commutativity ensures
	 * that the left and right datatypes are the same; this allows us to
	 * assume that the combining operators are equality for the righthand
	 * datatype, so that they can be used to compare righthand tuples as well
	 * as comparing lefthand to righthand tuples.  (This last restriction
	 * could be relaxed by using two different sets of operators with the hash
	 * table, but there is no obvious usefulness to that at present.)
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
	if (!optup->oprcanhash || optup->oprcom != opid ||
		!func_strict(optup->oprcode))
	{
		ReleaseSysCache(tup);
		return false;
	}
	ReleaseSysCache(tup);
	return true;
}

/*
 * convert_IN_to_join: can we convert an IN SubLink to join style?
 *
 * The caller has found a SubLink at the top level of WHERE, but has not
 * checked the properties of the SubLink at all.  Decide whether it is
 * appropriate to process this SubLink in join style.  If not, return NULL.
 * If so, build the qual clause(s) to replace the SubLink, and return them.
 *
 * Side effects of a successful conversion include adding the SubLink's
 * subselect to the query's rangetable and adding an InClauseInfo node to
 * its in_info_list.
 */
Node *
convert_IN_to_join(PlannerInfo *root, SubLink *sublink)
{
	Query	   *parse = root->parse;
	Query	   *subselect = (Query *) sublink->subselect;
	Relids		left_varnos;
	int			rtindex;
	RangeTblEntry *rte;
	RangeTblRef *rtr;
	InClauseInfo *ininfo;

	/*
	 * The sublink type must be "= ANY" --- that is, an IN operator.  We
	 * expect that the test expression will be either a single OpExpr, or an
	 * AND-clause containing OpExprs.  (If it's anything else then the parser
	 * must have determined that the operators have non-equality-like
	 * semantics.  In the OpExpr case we can't be sure what the operator's
	 * semantics are like, and must check for ourselves.)
	 */
	if (sublink->subLinkType != ANY_SUBLINK)
		return NULL;
	if (sublink->testexpr && IsA(sublink->testexpr, OpExpr))
	{
		List	   *opclasses;
		List	   *opstrats;

		get_op_btree_interpretation(((OpExpr *) sublink->testexpr)->opno,
									&opclasses, &opstrats);
		if (!list_member_int(opstrats, ROWCOMPARE_EQ))
			return NULL;
	}
	else if (!and_clause(sublink->testexpr))
		return NULL;

	/*
	 * The sub-select must not refer to any Vars of the parent query. (Vars of
	 * higher levels should be okay, though.)
	 */
	if (contain_vars_of_level((Node *) subselect, 1))
		return NULL;

	/*
	 * The left-hand expressions must contain some Vars of the current query,
	 * else it's not gonna be a join.
	 */
	left_varnos = pull_varnos(sublink->testexpr);
	if (bms_is_empty(left_varnos))
		return NULL;

	/*
	 * The combining operators and left-hand expressions mustn't be volatile.
	 */
	if (contain_volatile_functions(sublink->testexpr))
		return NULL;

	/*
	 * Okay, pull up the sub-select into top range table and jointree.
	 *
	 * We rely here on the assumption that the outer query has no references
	 * to the inner (necessarily true, other than the Vars that we build
	 * below). Therefore this is a lot easier than what pull_up_subqueries has
	 * to go through.
	 */
	rte = addRangeTableEntryForSubquery(NULL,
										subselect,
										makeAlias("IN_subquery", NIL),
										false);
	parse->rtable = lappend(parse->rtable, rte);
	rtindex = list_length(parse->rtable);
	rtr = makeNode(RangeTblRef);
	rtr->rtindex = rtindex;
	parse->jointree->fromlist = lappend(parse->jointree->fromlist, rtr);

	/*
	 * Now build the InClauseInfo node.
	 */
	ininfo = makeNode(InClauseInfo);
	ininfo->lefthand = left_varnos;
	ininfo->righthand = bms_make_singleton(rtindex);

	/*
	 * ininfo->sub_targetlist is filled with a list of Vars representing the
	 * subselect outputs.
	 */
	ininfo->sub_targetlist = generate_subquery_vars(subselect->targetList,
													rtindex);

	/* Add the completed node to the query's list */
	root->in_info_list = lappend(root->in_info_list, ininfo);

	return convert_testexpr(sublink->testexpr,
							ininfo->sub_targetlist);
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
			return (Node *) replace_outer_var((Var *) node);
	}
	if (IsA(node, Aggref))
	{
		if (((Aggref *) node)->agglevelsup > 0)
			return (Node *) replace_outer_agg((Aggref *) node);
	}
	return expression_tree_mutator(node,
								   replace_correlation_vars_mutator,
								   context);
}

/*
 * Expand SubLinks to SubPlans in the given expression.
 *
 * The isQual argument tells whether or not this expression is a WHERE/HAVING
 * qualifier expression.  If it is, any sublinks appearing at top level need
 * not distinguish FALSE from UNKNOWN return values.
 */
Node *
SS_process_sublinks(Node *expr, bool isQual)
{
	/* The only context needed is the initial are-we-in-a-qual flag */
	return process_sublinks_mutator(expr, &isQual);
}

static Node *
process_sublinks_mutator(Node *node, bool *isTopQual)
{
	bool		locTopQual;

	if (node == NULL)
		return NULL;
	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		Node	   *testexpr;

		/*
		 * First, recursively process the lefthand-side expressions, if any.
		 */
		locTopQual = false;
		testexpr = process_sublinks_mutator(sublink->testexpr, &locTopQual);

		/*
		 * Now build the SubPlan node and make the expr to return.
		 */
		return make_subplan(sublink, testexpr, *isTopQual);
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
	 * If we recurse down through anything other than an AND node, we are
	 * definitely not at top qual level anymore.  (Due to the coding here, we
	 * will not get called on the List subnodes of an AND, so no check is
	 * needed for List.)
	 */
	if (and_clause(node))
	{
		List	   *newargs = NIL;
		ListCell   *l;

		/* Still at qual top-level */
		locTopQual = *isTopQual;

		foreach(l, ((BoolExpr *) node)->args)
		{
			Node	   *newarg;

			newarg = process_sublinks_mutator(lfirst(l),
											  (void *) &locTopQual);
			if (and_clause(newarg))
				newargs = list_concat(newargs, ((BoolExpr *) newarg)->args);
			else
				newargs = lappend(newargs, newarg);
		}
		return (Node *) make_andclause(newargs);
	}

	/* otherwise not at qual top-level */
	locTopQual = false;

	if (or_clause(node))
	{
		List	   *newargs = NIL;
		ListCell   *l;

		foreach(l, ((BoolExpr *) node)->args)
		{
			Node	   *newarg;

			newarg = process_sublinks_mutator(lfirst(l),
											  (void *) &locTopQual);
			if (or_clause(newarg))
				newargs = list_concat(newargs, ((BoolExpr *) newarg)->args);
			else
				newargs = lappend(newargs, newarg);
		}
		return (Node *) make_orclause(newargs);
	}

	return expression_tree_mutator(node,
								   process_sublinks_mutator,
								   (void *) &locTopQual);
}

/*
 * SS_finalize_plan - do final sublink processing for a completed Plan.
 *
 * This recursively computes the extParam and allParam sets for every Plan
 * node in the given plan tree.  It also attaches any generated InitPlans
 * to the top plan node.
 */
void
SS_finalize_plan(Plan *plan, List *rtable)
{
	Bitmapset  *valid_params,
			   *initExtParam,
			   *initSetParam;
	Cost		initplan_cost;
	int			paramid;
	ListCell   *l;

	/*
	 * First, scan the param list to discover the sets of params that are
	 * available from outer query levels and my own query level. We do this
	 * once to save time in the per-plan recursion steps.  (This calculation
	 * is overly generous: it can include a lot of params that actually
	 * shouldn't be referenced here.  However, valid_params is just used as
	 * a debugging crosscheck, so it's not worth trying to be exact.)
	 */
	valid_params = NULL;
	paramid = 0;
	foreach(l, PlannerParamList)
	{
		PlannerParamItem *pitem = (PlannerParamItem *) lfirst(l);

		if (pitem->abslevel < PlannerQueryLevel)
		{
			/* valid outer-level parameter */
			valid_params = bms_add_member(valid_params, paramid);
		}
		else if (pitem->abslevel == PlannerQueryLevel &&
				 IsA(pitem->item, Param))
		{
			/* valid local parameter (i.e., a setParam of my child) */
			valid_params = bms_add_member(valid_params, paramid);
		}

		paramid++;
	}

	/*
	 * Now recurse through plan tree.
	 */
	(void) finalize_plan(plan, rtable, valid_params);

	bms_free(valid_params);

	/*
	 * Finally, attach any initPlans to the topmost plan node, and add their
	 * extParams to the topmost node's, too.  However, any setParams of the
	 * initPlans should not be present in the topmost node's extParams, only
	 * in its allParams.  (As of PG 8.1, it's possible that some initPlans
	 * have extParams that are setParams of other initPlans, so we have to
	 * take care of this situation explicitly.)
	 *
	 * We also add the total_cost of each initPlan to the startup cost of the
	 * top node.  This is a conservative overestimate, since in fact each
	 * initPlan might be executed later than plan startup, or even not at all.
	 */
	plan->initPlan = PlannerInitPlan;
	PlannerInitPlan = NIL;		/* make sure they're not attached twice */

	initExtParam = initSetParam = NULL;
	initplan_cost = 0;
	foreach(l, plan->initPlan)
	{
		SubPlan    *initplan = (SubPlan *) lfirst(l);
		ListCell   *l2;

		initExtParam = bms_add_members(initExtParam,
									   initplan->plan->extParam);
		foreach(l2, initplan->setParam)
		{
			initSetParam = bms_add_member(initSetParam, lfirst_int(l2));
		}
		initplan_cost += initplan->plan->total_cost;
	}
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

/*
 * Recursive processing of all nodes in the plan tree
 *
 * The return value is the computed allParam set for the given Plan node.
 * This is just an internal notational convenience.
 */
static Bitmapset *
finalize_plan(Plan *plan, List *rtable, Bitmapset *valid_params)
{
	finalize_primnode_context context;

	if (plan == NULL)
		return NULL;

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
			{
				RangeTblEntry *rte;

				rte = rt_fetch(((FunctionScan *) plan)->scan.scanrelid,
							   rtable);
				Assert(rte->rtekind == RTE_FUNCTION);
				finalize_primnode(rte->funcexpr, &context);
			}
			break;

		case T_ValuesScan:
			{
				RangeTblEntry *rte;

				rte = rt_fetch(((ValuesScan *) plan)->scan.scanrelid,
							   rtable);
				Assert(rte->rtekind == RTE_VALUES);
				finalize_primnode((Node *) rte->values_lists, &context);
			}
			break;

		case T_Append:
			{
				ListCell   *l;

				foreach(l, ((Append *) plan)->appendplans)
				{
					context.paramids =
						bms_add_members(context.paramids,
										finalize_plan((Plan *) lfirst(l),
													  rtable,
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
										finalize_plan((Plan *) lfirst(l),
													  rtable,
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
										finalize_plan((Plan *) lfirst(l),
													  rtable,
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
									   finalize_plan(plan->lefttree,
													 rtable,
													 valid_params));

	context.paramids = bms_add_members(context.paramids,
									   finalize_plan(plan->righttree,
													 rtable,
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
		Plan	   *plan = subplan->plan;
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
	List	   *saved_initplan = PlannerInitPlan;
	SubPlan    *node;
	Param	   *prm;

	/*
	 * We must run SS_finalize_plan(), since that's normally done before a
	 * subplan gets put into the initplan list.  However it will try to attach
	 * any pre-existing initplans to this one, which we don't want (they are
	 * siblings not children of this initplan).  So, a quick kluge to hide
	 * them.  (This is something else that could perhaps be cleaner if we did
	 * extParam/allParam processing in setrefs.c instead of here?  See notes
	 * for materialize_finished_plan.)
	 */
	PlannerInitPlan = NIL;

	/*
	 * Build extParam/allParam sets for plan nodes.
	 */
	SS_finalize_plan(plan, root->parse->rtable);

	/* Restore outer initplan list */
	PlannerInitPlan = saved_initplan;

	/*
	 * Create a SubPlan node and add it to the outer list of InitPlans.
	 */
	node = makeNode(SubPlan);
	node->subLinkType = EXPR_SUBLINK;
	node->plan = plan;
	node->plan_id = PlannerPlanId++;	/* Assign unique ID to this SubPlan */

	node->rtable = root->parse->rtable;

	PlannerInitPlan = lappend(PlannerInitPlan, node);

	/*
	 * The node can't have any inputs (since it's an initplan), so the
	 * parParam and args lists remain empty.
	 */

	/*
	 * Make a Param that will be the subplan's output.
	 */
	prm = generate_new_param(resulttype, resulttypmod);
	node->setParam = list_make1_int(prm->paramid);

	return prm;
}
