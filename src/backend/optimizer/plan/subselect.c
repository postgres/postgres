/*-------------------------------------------------------------------------
 *
 * subselect.c
 *	  Planning routines for subselects and parameters.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/plan/subselect.c,v 1.100.2.3 2006/05/03 00:25:07 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/params.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/subselect.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
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


typedef struct finalize_primnode_context
{
	Bitmapset  *paramids;		/* Set of PARAM_EXEC paramids found */
	Bitmapset  *outer_params;	/* Set of accessible outer paramids */
} finalize_primnode_context;


static List *convert_sublink_opers(List *lefthand, List *operOids,
					  List *targetlist, int rtindex,
					  List **righthandIds);
static bool subplan_is_hashable(SubLink *slink, SubPlan *node);
static Node *replace_correlation_vars_mutator(Node *node, void *context);
static Node *process_sublinks_mutator(Node *node, bool *isTopQual);
static Bitmapset *finalize_plan(Plan *plan, List *rtable,
			  Bitmapset *outer_params,
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
	retval->paramid = (AttrNumber) i;
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
	retval->paramid = (AttrNumber) i;
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
	retval->paramid = (AttrNumber) list_length(PlannerParamList);
	retval->paramtype = paramtype;

	pitem = (PlannerParamItem *) palloc(sizeof(PlannerParamItem));
	pitem->item = (Node *) retval;
	pitem->abslevel = PlannerQueryLevel;

	PlannerParamList = lappend(PlannerParamList, pitem);

	return retval;
}

/*
 * Convert a bare SubLink (as created by the parser) into a SubPlan.
 *
 * We are given the raw SubLink and the already-processed lefthand argument
 * list (use this instead of the SubLink's own field).  We are also told if
 * this expression appears at top level of a WHERE/HAVING qual.
 *
 * The result is whatever we need to substitute in place of the SubLink
 * node in the executable expression.  This will be either the SubPlan
 * node (if we have to do the subplan as a subplan), or a Param node
 * representing the result of an InitPlan, or possibly an AND or OR tree
 * containing InitPlan Param nodes.
 */
static Node *
make_subplan(SubLink *slink, List *lefthand, bool isTopQual)
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
	 * 50% retrieval.  For EXPR and MULTIEXPR subplans, use default behavior
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
	node->useOr = slink->useOr;
	node->exprs = NIL;
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
	 * MULTIEXPR types can be used as initPlans.  For EXISTS, EXPR, or ARRAY,
	 * we just produce a Param referring to the result of evaluating the
	 * initPlan.  For MULTIEXPR, we must build an AND or OR-clause of the
	 * individual comparison operators, using the appropriate lefthand side
	 * expressions and Params for the initPlan's target items.
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
	else if (node->parParam == NIL && slink->subLinkType == MULTIEXPR_SUBLINK)
	{
		List	   *exprs;

		/* Convert the lefthand exprs and oper OIDs into executable exprs */
		exprs = convert_sublink_opers(lefthand,
									  slink->operOids,
									  plan->targetlist,
									  0,
									  &node->paramIds);
		node->setParam = list_copy(node->paramIds);
		PlannerInitPlan = lappend(PlannerInitPlan, node);

		/*
		 * The executable expressions are returned to become part of the outer
		 * plan's expression tree; they are not kept in the initplan node.
		 */
		if (list_length(exprs) > 1)
			result = (Node *) (node->useOr ? make_orclause(exprs) :
							   make_andclause(exprs));
		else
			result = (Node *) linitial(exprs);
	}
	else
	{
		List	   *args;
		ListCell   *l;

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

		/* Convert the lefthand exprs and oper OIDs into executable exprs */
		node->exprs = convert_sublink_opers(lefthand,
											slink->operOids,
											plan->targetlist,
											0,
											&node->paramIds);

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
 * convert_sublink_opers: given a lefthand-expressions list and a list of
 * operator OIDs, build a list of actually executable expressions.	The
 * righthand sides of the expressions are Params or Vars representing the
 * results of the sub-select.
 *
 * If rtindex is 0, we build Params to represent the sub-select outputs.
 * The paramids of the Params created are returned in the *righthandIds list.
 *
 * If rtindex is not 0, we build Vars using that rtindex as varno.	Copies
 * of the Var nodes are returned in *righthandIds (this is a bit of a type
 * cheat, but we can get away with it).
 */
static List *
convert_sublink_opers(List *lefthand, List *operOids,
					  List *targetlist, int rtindex,
					  List **righthandIds)
{
	List	   *result = NIL;
	ListCell   *l,
			   *lefthand_item,
			   *tlist_item;

	*righthandIds = NIL;
	lefthand_item = list_head(lefthand);
	tlist_item = list_head(targetlist);

	foreach(l, operOids)
	{
		Oid			opid = lfirst_oid(l);
		Node	   *leftop = (Node *) lfirst(lefthand_item);
		TargetEntry *te = (TargetEntry *) lfirst(tlist_item);
		Node	   *rightop;
		Operator	tup;

		Assert(!te->resjunk);

		if (rtindex)
		{
			/* Make the Var node representing the subplan's result */
			rightop = (Node *) makeVar(rtindex,
									   te->resno,
									   exprType((Node *) te->expr),
									   exprTypmod((Node *) te->expr),
									   0);

			/*
			 * Copy it for caller.	NB: we need a copy to avoid having
			 * doubly-linked substructure in the modified parse tree.
			 */
			*righthandIds = lappend(*righthandIds, copyObject(rightop));
		}
		else
		{
			/* Make the Param node representing the subplan's result */
			Param	   *prm;

			prm = generate_new_param(exprType((Node *) te->expr),
									 exprTypmod((Node *) te->expr));
			/* Record its ID */
			*righthandIds = lappend_int(*righthandIds, prm->paramid);
			rightop = (Node *) prm;
		}

		/* Look up the operator to pass to make_op_expr */
		tup = SearchSysCache(OPEROID,
							 ObjectIdGetDatum(opid),
							 0, 0, 0);
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for operator %u", opid);

		/*
		 * Make the expression node.
		 *
		 * Note: we use make_op_expr in case runtime type conversion function
		 * calls must be inserted for this operator!  (But we are not
		 * expecting to have to resolve unknown Params, so it's okay to pass a
		 * null pstate.)
		 */
		result = lappend(result,
						 make_op_expr(NULL,
									  tup,
									  leftop,
									  rightop,
									  exprType(leftop),
									  exprType((Node *) te->expr)));

		ReleaseSysCache(tup);

		lefthand_item = lnext(lefthand_item);
		tlist_item = lnext(tlist_item);
	}

	return result;
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
	 * The sublink type must be "= ANY" --- that is, an IN operator. (We
	 * require the operator name to be unqualified, which may be overly
	 * paranoid, or may not be.)  XXX since we also check that the operators
	 * are hashable, the test on operator name may be redundant?
	 */
	if (slink->subLinkType != ANY_SUBLINK)
		return false;
	if (list_length(slink->operName) != 1 ||
		strcmp(strVal(linitial(slink->operName)), "=") != 0)
		return false;

	/*
	 * The subplan must not have any direct correlation vars --- else we'd
	 * have to recompute its output each time, so that the hashtable wouldn't
	 * gain anything.
	 */
	if (node->parParam != NIL)
		return false;

	/*
	 * The estimated size of the subquery result must fit in work_mem. (XXX
	 * what about hashtable overhead?)
	 */
	subquery_size = node->plan->plan_rows *
		(MAXALIGN(node->plan->plan_width) + MAXALIGN(sizeof(HeapTupleData)));
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
	foreach(l, slink->operOids)
	{
		Oid			opid = lfirst_oid(l);
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
	}
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
	List	   *exprs;

	/*
	 * The sublink type must be "= ANY" --- that is, an IN operator. (We
	 * require the operator name to be unqualified, which may be overly
	 * paranoid, or may not be.)
	 */
	if (sublink->subLinkType != ANY_SUBLINK)
		return NULL;
	if (list_length(sublink->operName) != 1 ||
		strcmp(strVal(linitial(sublink->operName)), "=") != 0)
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
	left_varnos = pull_varnos((Node *) sublink->lefthand);
	if (bms_is_empty(left_varnos))
		return NULL;

	/*
	 * The left-hand expressions mustn't be volatile.  (Perhaps we should test
	 * the combining operators, too?  We'd only need to point the function
	 * directly at the sublink ...)
	 */
	if (contain_volatile_functions((Node *) sublink->lefthand))
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
	root->in_info_list = lappend(root->in_info_list, ininfo);

	/*
	 * Build the result qual expressions.  As a side effect,
	 * ininfo->sub_targetlist is filled with a list of Vars representing the
	 * subselect outputs.
	 */
	exprs = convert_sublink_opers(sublink->lefthand,
								  sublink->operOids,
								  subselect->targetList,
								  rtindex,
								  &ininfo->sub_targetlist);
	return (Node *) make_ands_explicit(exprs);
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
		List	   *lefthand;

		/*
		 * First, recursively process the lefthand-side expressions, if any.
		 */
		locTopQual = false;
		lefthand = (List *)
			process_sublinks_mutator((Node *) sublink->lefthand, &locTopQual);

		/*
		 * Now build the SubPlan node and make the expr to return.
		 */
		return make_subplan(sublink, lefthand, *isTopQual);
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
	Bitmapset  *outer_params,
			   *valid_params,
			   *initExtParam,
			   *initSetParam;
	Cost		initplan_cost;
	int			paramid;
	ListCell   *l;

	/*
	 * First, scan the param list to discover the sets of params that are
	 * available from outer query levels and my own query level. We do this
	 * once to save time in the per-plan recursion steps.
	 */
	outer_params = valid_params = NULL;
	paramid = 0;
	foreach(l, PlannerParamList)
	{
		PlannerParamItem *pitem = (PlannerParamItem *) lfirst(l);

		if (pitem->abslevel < PlannerQueryLevel)
		{
			/* valid outer-level parameter */
			outer_params = bms_add_member(outer_params, paramid);
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
	(void) finalize_plan(plan, rtable, outer_params, valid_params);

	bms_free(outer_params);
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
	/* but extParam shouldn't include any setParams */
	initExtParam = bms_del_members(initExtParam, initSetParam);
	/* empty test ensures extParam is exactly NULL if it's empty */
	if (!bms_is_empty(initExtParam))
		plan->extParam = bms_join(plan->extParam, initExtParam);

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
finalize_plan(Plan *plan, List *rtable,
			  Bitmapset *outer_params, Bitmapset *valid_params)
{
	finalize_primnode_context context;

	if (plan == NULL)
		return NULL;

	context.paramids = NULL;	/* initialize set to empty */
	context.outer_params = outer_params;

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
			finalize_primnode((Node *) ((TidScan *) plan)->tideval,
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

		case T_Append:
			{
				ListCell   *l;

				foreach(l, ((Append *) plan)->appendplans)
				{
					context.paramids =
						bms_add_members(context.paramids,
										finalize_plan((Plan *) lfirst(l),
													  rtable,
													  outer_params,
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
													  outer_params,
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
													  outer_params,
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
													 outer_params,
													 valid_params));

	context.paramids = bms_add_members(context.paramids,
									   finalize_plan(plan->righttree,
													 rtable,
													 outer_params,
													 valid_params));

	/* Now we have all the paramids */

	if (!bms_is_subset(context.paramids, valid_params))
		elog(ERROR, "plan should not reference subplan's variable");

	plan->extParam = bms_intersect(context.paramids, outer_params);
	plan->allParam = context.paramids;

	/*
	 * For speed at execution time, make sure extParam/allParam are actually
	 * NULL if they are empty sets.
	 */
	if (bms_is_empty(plan->extParam))
	{
		bms_free(plan->extParam);
		plan->extParam = NULL;
	}
	if (bms_is_empty(plan->allParam))
	{
		bms_free(plan->allParam);
		plan->allParam = NULL;
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
			int			paramid = (int) ((Param *) node)->paramid;

			context->paramids = bms_add_member(context->paramids, paramid);
		}
		return false;			/* no more to do here */
	}
	if (is_subplan(node))
	{
		SubPlan    *subplan = (SubPlan *) node;

		/* Add outer-level params needed by the subplan to paramids */
		context->paramids = bms_join(context->paramids,
									 bms_intersect(subplan->plan->extParam,
												   context->outer_params));
		/* fall through to recurse into subplan args */
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
	 * Set up for a new level of subquery.	This is just to keep
	 * SS_finalize_plan from becoming confused.
	 */
	PlannerQueryLevel++;
	PlannerInitPlan = NIL;

	/*
	 * Build extParam/allParam sets for plan nodes.
	 */
	SS_finalize_plan(plan, root->parse->rtable);

	/* Return to outer subquery context */
	PlannerQueryLevel--;
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
