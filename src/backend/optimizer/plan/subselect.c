/*-------------------------------------------------------------------------
 *
 * subselect.c
 *	  Planning routines for subselects and parameters.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/subselect.c,v 1.64 2003/01/12 04:03:34 tgl Exp $
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
#include "parser/parsetree.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "utils/lsyscache.h"
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
 *
 * We also need to create Param slots that don't correspond to any outer Var.
 * For these, we set varno = 0 and varlevelsup = 0, so that they can't
 * accidentally match an outer Var.
 *--------------------
 */


typedef struct finalize_primnode_results
{
	List	   *paramids;		/* List of PARAM_EXEC paramids found */
} finalize_primnode_results;


static List *convert_sublink_opers(List *lefthand, List *operOids,
								   List *targetlist, List **paramIds);
static bool subplan_is_hashable(SubLink *slink, SubPlan *node);
static Node *replace_correlation_vars_mutator(Node *node, void *context);
static Node *process_sublinks_mutator(Node *node, void *context);
static bool finalize_primnode(Node *node, finalize_primnode_results *results);


/*
 * Create a new entry in the PlannerParamVar list, and return its index.
 *
 * var contains the data to use, except for varlevelsup which
 * is set from the absolute level value given by varlevel.  NOTE that
 * the passed var is scribbled on and placed directly into the list!
 * Generally, caller should have just created or copied it.
 */
static int
new_param(Var *var, Index varlevel)
{
	var->varlevelsup = varlevel;

	PlannerParamVar = lappend(PlannerParamVar, var);

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
	 */
	i = 0;
	foreach(ppv, PlannerParamVar)
	{
		Var		   *pvar = lfirst(ppv);

		if (pvar->varno == var->varno &&
			pvar->varattno == var->varattno &&
			pvar->varlevelsup == varlevel &&
			pvar->vartype == var->vartype)
			break;
		i++;
	}

	if (!ppv)
	{
		/* Nope, so make a new one */
		i = new_param((Var *) copyObject(var), varlevel);
	}

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = (AttrNumber) i;
	retval->paramtype = var->vartype;

	return retval;
}

/*
 * Generate a new Param node that will not conflict with any other.
 */
static Param *
generate_new_param(Oid paramtype, int32 paramtypmod)
{
	Var		   *var = makeVar(0, 0, paramtype, paramtypmod, 0);
	Param	   *retval = makeNode(Param);

	retval->paramkind = PARAM_EXEC;
	retval->paramid = (AttrNumber) new_param(var, 0);
	retval->paramtype = paramtype;

	return retval;
}

/*
 * Convert a bare SubLink (as created by the parser) into a SubPlan.
 *
 * We are given the raw SubLink and the already-processed lefthand argument
 * list (use this instead of the SubLink's own field).
 *
 * The result is whatever we need to substitute in place of the SubLink
 * node in the executable expression.  This will be either the SubPlan
 * node (if we have to do the subplan as a subplan), or a Param node
 * representing the result of an InitPlan, or possibly an AND or OR tree
 * containing InitPlan Param nodes.
 */
static Node *
make_subplan(SubLink *slink, List *lefthand)
{
	SubPlan	   *node = makeNode(SubPlan);
	Query	   *subquery = (Query *) (slink->subselect);
	double		tuple_fraction;
	Plan	   *plan;
	List	   *lst;
	Node	   *result;

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
	 * XXX If an ALL/ANY subplan is uncorrelated, we may decide to hash or
	 * materialize its result below.  In that case it would've been better to
	 * specify full retrieval.  At present, however, we can only detect
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

	/*
	 * Initialize other fields of the SubPlan node.
	 */
	node->subLinkType = slink->subLinkType;
	node->useOr = slink->useOr;
	node->exprs = NIL;
	node->paramIds = NIL;
	node->useHashTable = false;
	node->unknownEqFalse = false;
	node->setParam = NIL;
	node->parParam = NIL;
	node->args = NIL;

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
		Param	   *prm;

		prm = generate_new_param(BOOLOID, -1);
		node->setParam = lappendi(node->setParam, prm->paramid);
		PlannerInitPlan = lappend(PlannerInitPlan, node);
		result = (Node *) prm;
	}
	else if (node->parParam == NIL && slink->subLinkType == EXPR_SUBLINK)
	{
		TargetEntry *te = lfirst(plan->targetlist);
		Param	   *prm;

		Assert(!te->resdom->resjunk);
		prm = generate_new_param(te->resdom->restype, te->resdom->restypmod);
		node->setParam = lappendi(node->setParam, prm->paramid);
		PlannerInitPlan = lappend(PlannerInitPlan, node);
		result = (Node *) prm;
	}
	else if (node->parParam == NIL && slink->subLinkType == MULTIEXPR_SUBLINK)
	{
		List   *exprs;

		/* Convert the lefthand exprs and oper OIDs into executable exprs */
		exprs = convert_sublink_opers(lefthand,
									  slink->operOids,
									  plan->targetlist,
									  &node->paramIds);
		node->setParam = nconc(node->setParam, listCopy(node->paramIds));
		PlannerInitPlan = lappend(PlannerInitPlan, node);
		/*
		 * The executable expressions are returned to become part of the
		 * outer plan's expression tree; they are not kept in the initplan
		 * node.
		 */
		if (length(exprs) > 1)
			result = (Node *) (node->useOr ? make_orclause(exprs) :
							   make_andclause(exprs));
		else
			result = (Node *) lfirst(exprs);
	}
	else
	{
		List	   *args;

		/*
		 * We can't convert subplans of ALL_SUBLINK or ANY_SUBLINK types
		 * to initPlans, even when they are uncorrelated or undirect
		 * correlated, because we need to scan the output of the subplan
		 * for each outer tuple.  But if it's an IN (= ANY) test, we might
		 * be able to use a hashtable to avoid comparing all the tuples.
		 */
		if (subplan_is_hashable(slink, node))
			node->useHashTable = true;
		/*
		 * Otherwise, we have the option to tack a MATERIAL node onto the top
		 * of the subplan, to reduce the cost of reading it repeatedly.  This
		 * is pointless for a direct-correlated subplan, since we'd have to
		 * recompute its results each time anyway.  For uncorrelated/undirect
		 * correlated subplans, we add MATERIAL if the subplan's top plan node
		 * is anything more complicated than a plain sequential scan, and we
		 * do it even for seqscan if the qual appears selective enough to
		 * eliminate many tuples.
		 *
		 * XXX It's pretty ugly to be inserting a MATERIAL node at this
		 * point.  Since subquery_planner has already run SS_finalize_plan
		 * on the subplan tree, we have to kluge up parameter lists for
		 * the MATERIAL node.  Possibly this could be fixed by postponing
		 * SS_finalize_plan processing until setrefs.c is run.
		 */
		else if (node->parParam == NIL)
		{
			bool		use_material;

			switch (nodeTag(plan))
			{
				case T_SeqScan:
					if (plan->initPlan)
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
				Path		matpath; /* dummy for result of cost_material */

				matplan = (Plan *) make_material(plan->targetlist, plan);
				/* need to calculate costs */
				cost_material(&matpath,
							  plan->total_cost,
							  plan->plan_rows,
							  plan->plan_width);
				matplan->startup_cost = matpath.startup_cost;
				matplan->total_cost = matpath.total_cost;
				/* parameter kluge --- see comments above */
				matplan->extParam = listCopy(plan->extParam);
				matplan->locParam = listCopy(plan->locParam);
				node->plan = plan = matplan;
			}
		}

		/* Convert the lefthand exprs and oper OIDs into executable exprs */
		node->exprs = convert_sublink_opers(lefthand,
											slink->operOids,
											plan->targetlist,
											&node->paramIds);

		/*
		 * Make node->args from parParam.
		 */
		args = NIL;
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
		node->args = args;

		result = (Node *) node;
	}

	return result;
}

/*
 * convert_sublink_opers: given a lefthand-expressions list and a list of
 * operator OIDs, build a list of actually executable expressions.  The
 * righthand sides of the expressions are Params representing the results
 * of the sub-select.
 *
 * The paramids of the Params created are returned in the *paramIds list.
 */
static List *
convert_sublink_opers(List *lefthand, List *operOids,
					  List *targetlist, List **paramIds)
{
	List	   *result = NIL;
	List	   *lst;

	*paramIds = NIL;

	foreach(lst, operOids)
	{
		Oid			opid = (Oid) lfirsti(lst);
		Node	   *leftop = lfirst(lefthand);
		TargetEntry *te = lfirst(targetlist);
		Param	   *prm;
		Operator	tup;
		Form_pg_operator opform;
		Node	   *left,
				   *right;

		Assert(!te->resdom->resjunk);

		/* Make the Param node representing the subplan's result */
		prm = generate_new_param(te->resdom->restype,
								 te->resdom->restypmod);

		/* Record its ID */
		*paramIds = lappendi(*paramIds, prm->paramid);

		/* Look up the operator to get its declared input types */
		tup = SearchSysCache(OPEROID,
							 ObjectIdGetDatum(opid),
							 0, 0, 0);
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for operator %u", opid);
		opform = (Form_pg_operator) GETSTRUCT(tup);

		/*
		 * Make the expression node.
		 *
		 * Note: we use make_operand in case runtime type conversion
		 * function calls must be inserted for this operator!
		 */
		left = make_operand(leftop, exprType(leftop), opform->oprleft);
		right = make_operand((Node *) prm, prm->paramtype, opform->oprright);
		result = lappend(result,
						 make_opclause(opid,
									   opform->oprresult,
									   false, /* set-result not allowed */
									   (Expr *) left,
									   (Expr *) right));

		ReleaseSysCache(tup);

		lefthand = lnext(lefthand);
		targetlist = lnext(targetlist);
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
	List	   *opids;

	/*
	 * The sublink type must be "= ANY" --- that is, an IN operator.
	 * (We require the operator name to be unqualified, which may be
	 * overly paranoid, or may not be.)  XXX since we also check that the
	 * operators are hashable, the test on operator name may be redundant?
	 */
	if (slink->subLinkType != ANY_SUBLINK)
		return false;
	if (length(slink->operName) != 1 ||
		strcmp(strVal(lfirst(slink->operName)), "=") != 0)
		return false;
	/*
	 * The subplan must not have any direct correlation vars --- else we'd
	 * have to recompute its output each time, so that the hashtable wouldn't
	 * gain anything.
	 */
	if (node->parParam != NIL)
		return false;
	/*
	 * The estimated size of the subquery result must fit in SortMem.
	 * (XXX what about hashtable overhead?)
	 */
	subquery_size = node->plan->plan_rows *
		(MAXALIGN(node->plan->plan_width) + MAXALIGN(sizeof(HeapTupleData)));
	if (subquery_size > SortMem * 1024L)
		return false;
	/*
	 * The combining operators must be hashable, strict, and self-commutative.
	 * The need for hashability is obvious, since we want to use hashing.
	 * Without strictness, behavior in the presence of nulls is too
	 * unpredictable.  (We actually must assume even more than plain
	 * strictness, see nodeSubplan.c for details.)  And commutativity ensures
	 * that the left and right datatypes are the same; this allows us to
	 * assume that the combining operators are equality for the righthand
	 * datatype, so that they can be used to compare righthand tuples as
	 * well as comparing lefthand to righthand tuples.  (This last restriction
	 * could be relaxed by using two different sets of operators with the
	 * hash table, but there is no obvious usefulness to that at present.)
	 */
	foreach(opids, slink->operOids)
	{
		Oid			opid = (Oid) lfirsti(opids);
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
 * Replace correlation vars (uplevel vars) with Params.
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
			return (Node *) replace_var((Var *) node);
	}
	return expression_tree_mutator(node,
								   replace_correlation_vars_mutator,
								   context);
}

/*
 * Expand SubLinks to SubPlans in the given expression.
 */
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
		List	   *lefthand;

		/*
		 * First, recursively process the lefthand-side expressions, if any.
		 */
		lefthand = (List *)
			process_sublinks_mutator((Node *) sublink->lefthand, context);
		/*
		 * Now build the SubPlan node and make the expr to return.
		 */
		return make_subplan(sublink, lefthand);
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

/*
 * SS_finalize_plan - do final sublink processing for a completed Plan.
 *
 * This recursively computes and sets the extParam and locParam lists
 * for every Plan node in the given tree.
 */
List *
SS_finalize_plan(Plan *plan, List *rtable)
{
	List	   *extParam = NIL;
	List	   *locParam = NIL;
	finalize_primnode_results results;
	List	   *lst;

	if (plan == NULL)
		return NIL;

	results.paramids = NIL;		/* initialize list to NIL */

	/*
	 * When we call finalize_primnode, results.paramids lists are
	 * automatically merged together.  But when recursing to self, we have
	 * to do it the hard way.  We want the paramids list to include params
	 * in subplans as well as at this level.
	 */

	/* Find params in targetlist and qual */
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
			 * same param references as indxqual.
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
			finalize_primnode((Node *) ((Hash *) plan)->hashkeys,
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

	/* Process left and right child plans, if any */
	results.paramids = set_unioni(results.paramids,
								  SS_finalize_plan(plan->lefttree,
												   rtable));
	results.paramids = set_unioni(results.paramids,
								  SS_finalize_plan(plan->righttree,
												   rtable));

	/* Now we have all the paramids */

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

	return results.paramids;
}

/*
 * finalize_primnode: build lists of params appearing
 * in the given expression tree.  NOTE: items are added to list passed in,
 * so caller must initialize list to NIL before first call!
 */
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
		SubPlan	   *subplan = (SubPlan *) node;
		List	   *lst;

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
