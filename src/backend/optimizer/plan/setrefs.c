/*-------------------------------------------------------------------------
 *
 * setrefs.c
 *	  Post-processing of a completed plan tree: fix references to subplan
 *	  vars, and compute regproc values for operators
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/setrefs.c,v 1.58 1999/10/30 23:07:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"

typedef struct {
	List	   *outer_tlist;
	List	   *inner_tlist;
	Index		acceptable_rel;
} join_references_context;

typedef struct {
	Index		subvarno;
	List	   *subplanTargetList;
} replace_vars_with_subplan_refs_context;

static void set_join_references(Join *join);
static void set_uppernode_references(Plan *plan, Index subvarno);
static Node *join_references_mutator(Node *node,
									 join_references_context *context);
static Node *replace_vars_with_subplan_refs(Node *node,
											Index subvarno,
											List *subplanTargetList);
static Node *replace_vars_with_subplan_refs_mutator(Node *node,
					replace_vars_with_subplan_refs_context *context);
static bool fix_opids_walker(Node *node, void *context);

/*****************************************************************************
 *
 *		SUBPLAN REFERENCES
 *
 *****************************************************************************/

/*
 * set_plan_references
 *	  This is the final processing pass of the planner/optimizer.  The plan
 *	  tree is complete; we just have to adjust some representational details
 *	  for the convenience of the executor.  We update Vars in upper plan nodes
 *	  to refer to the outputs of their subplans, and we compute regproc OIDs
 *	  for operators (ie, we look up the function that implements each op).
 *
 *	  set_plan_references recursively traverses the whole plan tree.
 *
 * Returns nothing of interest, but modifies internal fields of nodes.
 */
void
set_plan_references(Plan *plan)
{
	List	   *pl;

	if (plan == NULL)
		return;

	/*
	 * Plan-type-specific fixes
	 */
	switch (nodeTag(plan))
	{
		case T_SeqScan:
			/* nothing special */
			break;
		case T_IndexScan:
			fix_opids((Node *) ((IndexScan *) plan)->indxqual);
			fix_opids((Node *) ((IndexScan *) plan)->indxqualorig);
			break;
		case T_NestLoop:
			set_join_references((Join *) plan);
			break;
		case T_MergeJoin:
			set_join_references((Join *) plan);
			fix_opids((Node *) ((MergeJoin *) plan)->mergeclauses);
			break;
		case T_HashJoin:
			set_join_references((Join *) plan);
			fix_opids((Node *) ((HashJoin *) plan)->hashclauses);
			break;
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_Hash:
			/* These plan types don't actually bother to evaluate their
			 * targetlists or quals (because they just return their
			 * unmodified input tuples).  The optimizer is lazy about
			 * creating really valid targetlists for them.  Best to
			 * just leave the targetlist alone.
			 */
			break;
		case T_Agg:
		case T_Group:
			set_uppernode_references(plan, (Index) 0);
			break;
		case T_Result:
			/* Result may or may not have a subplan; no need to fix up
			 * subplan references if it hasn't got one...
			 *
			 * XXX why does Result use a different subvarno from Agg/Group?
			 */
			if (plan->lefttree != NULL)
				set_uppernode_references(plan, (Index) OUTER);
			fix_opids(((Result *) plan)->resconstantqual);
			break;
		case T_Append:
			foreach(pl, ((Append *) plan)->appendplans)
			{
				set_plan_references((Plan *) lfirst(pl));
			}
			break;
		default:
			elog(ERROR, "set_plan_references: unknown plan type %d",
				 nodeTag(plan));
			break;
	}

	/*
	 * For all plan types, fix operators in targetlist and qual expressions
	 */
	fix_opids((Node *) plan->targetlist);
	fix_opids((Node *) plan->qual);

	/*
	 * Now recurse into subplans, if any
	 *
	 * NOTE: it is essential that we recurse into subplans AFTER we set
	 * subplan references in this plan's tlist and quals.  If we did the
	 * reference-adjustments bottom-up, then we would fail to match this
	 * plan's var nodes against the already-modified nodes of the subplans.
	 */
	set_plan_references(plan->lefttree);
	set_plan_references(plan->righttree);
	foreach(pl, plan->initPlan)
	{
		SubPlan	   *sp = (SubPlan *) lfirst(pl);

		Assert(IsA(sp, SubPlan));
		set_plan_references(sp->plan);
	}
	foreach(pl, plan->subPlan)
	{
		SubPlan	   *sp = (SubPlan *) lfirst(pl);

		Assert(IsA(sp, SubPlan));
		set_plan_references(sp->plan);
	}
}

/*
 * set_join_references
 *	  Modifies the target list of a join node to reference its subplans,
 *	  by setting the varnos to OUTER or INNER and setting attno values to the
 *	  result domain number of either the corresponding outer or inner join
 *	  tuple item.
 *
 * Note: this same transformation has already been applied to the quals
 * of the join by createplan.c.  It's a little odd to do it here for the
 * targetlist and there for the quals, but it's easier that way.  (Look
 * at switch_outer() and the handling of nestloop inner indexscans to
 * see why.)
 *
 * Because the quals are reference-adjusted sooner, we cannot do equal()
 * comparisons between qual and tlist var nodes during the time between
 * creation of a plan node by createplan.c and its fixing by this module.
 * Fortunately, there doesn't seem to be any need to do that.
 *
 * 'join' is a join plan node
 */
static void
set_join_references(Join *join)
{
	Plan	   *outer = join->lefttree;
	Plan	   *inner = join->righttree;
	List	   *outer_tlist = ((outer == NULL) ? NIL : outer->targetlist);
	List	   *inner_tlist = ((inner == NULL) ? NIL : inner->targetlist);

	join->targetlist = join_references(join->targetlist,
									   outer_tlist,
									   inner_tlist,
									   (Index) 0);
}

/*
 * set_uppernode_references
 *	  Update the targetlist and quals of an upper-level plan node
 *	  to refer to the tuples returned by its lefttree subplan.
 *
 * This is used for single-input plan types like Agg, Group, Result.
 */
static void
set_uppernode_references(Plan *plan, Index subvarno)
{
	Plan	   *subplan = plan->lefttree;
	List	   *subplanTargetList;

	if (subplan != NULL)
		subplanTargetList = subplan->targetlist;
	else
		subplanTargetList = NIL;

	plan->targetlist = (List *)
		replace_vars_with_subplan_refs((Node *) plan->targetlist,
									   subvarno,
									   subplanTargetList);

	plan->qual = (List *)
		replace_vars_with_subplan_refs((Node *) plan->qual,
									   subvarno,
									   subplanTargetList);
}

/*
 * join_references
 *	   Creates a new set of targetlist entries or join qual clauses by
 *	   changing the varno/varattno values of variables in the clauses
 *	   to reference target list values from the outer and inner join
 *	   relation target lists.
 *
 * This is used in two different scenarios: a normal join clause, where
 * all the Vars in the clause *must* be replaced by OUTER or INNER references;
 * and an indexscan being used on the inner side of a nestloop join.
 * In the latter case we want to replace the outer-relation Vars by OUTER
 * references, but not touch the Vars of the inner relation.
 *
 * For a normal join, acceptable_rel should be zero so that any failure to
 * match a Var will be reported as an error.  For the indexscan case,
 * pass inner_tlist = NIL and acceptable_rel = the ID of the inner relation.
 *
 * 'clauses' is the targetlist or list of join clauses
 * 'outer_tlist' is the target list of the outer join relation
 * 'inner_tlist' is the target list of the inner join relation, or NIL
 * 'acceptable_rel' is either zero or the rangetable index of a relation
 *		whose Vars may appear in the clause without provoking an error.
 *
 * Returns the new expression tree.  The original clause structure is
 * not modified.
 */
List *
join_references(List *clauses,
				List *outer_tlist,
				List *inner_tlist,
				Index acceptable_rel)
{
	join_references_context context;

	context.outer_tlist = outer_tlist;
	context.inner_tlist = inner_tlist;
	context.acceptable_rel = acceptable_rel;
	return (List *) join_references_mutator((Node *) clauses, &context);
}

static Node *
join_references_mutator(Node *node,
						join_references_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		Var		   *newvar = (Var *) copyObject(var);
		Resdom	   *resdom;

		resdom = tlist_member((Node *) var, context->outer_tlist);
		if (resdom)
		{
			newvar->varno = OUTER;
			newvar->varattno = resdom->resno;
			return (Node *) newvar;
		}
		resdom = tlist_member((Node *) var, context->inner_tlist);
		if (resdom)
		{
			newvar->varno = INNER;
			newvar->varattno = resdom->resno;
			return (Node *) newvar;
		}
		/*
		 * Var not in either tlist --- either raise an error,
		 * or return the Var unmodified.
		 */
		if (var->varno != context->acceptable_rel)
			elog(ERROR, "join_references: variable not in subplan target lists");
		return (Node *) newvar;	/* copy is probably not necessary here... */
	}
	/*
	 * expression_tree_mutator will copy SubPlan nodes if given a chance.
	 * We do not want to do that here, because subselect.c has already
	 * constructed the initPlan and subPlan lists of the current plan node
	 * and we mustn't leave those dangling (ie, pointing to different
	 * copies of the nodes than what's in the targetlist & quals...)
	 * Instead, alter the SubPlan in-place.  Grotty --- is there a better way?
	 */
	if (is_subplan(node))
	{
		Expr	   *expr = (Expr *) node;
		SubLink	   *sublink = ((SubPlan *) expr->oper)->sublink;

		/* transform args list (params to be passed to subplan) */
		expr->args = (List *)
			join_references_mutator((Node *) expr->args,
									context);
		/* transform sublink's oper list as well */
		sublink->oper = (List *)
			join_references_mutator((Node *) sublink->oper,
									context);

		return (Node *) expr;
	}
	return expression_tree_mutator(node,
								   join_references_mutator,
								   (void *) context);
}

/*
 * replace_vars_with_subplan_refs
 *		This routine modifies an expression tree so that all Var nodes
 *		reference target nodes of a subplan.  It is used to fix up
 *		target and qual expressions of non-join upper-level plan nodes.
 *
 * An error is raised if no matching var can be found in the subplan tlist
 * --- so this routine should only be applied to nodes whose subplans'
 * targetlists were generated via flatten_tlist() or some such method.
 *
 * 'node': the tree to be fixed (a targetlist or qual list)
 * 'subvarno': varno to be assigned to all Vars
 * 'subplanTargetList': target list for subplan
 *
 * The resulting tree is a copy of the original in which all Var nodes have
 * varno = subvarno, varattno = resno of corresponding subplan target.
 * The original tree is not modified.
 */
static Node *
replace_vars_with_subplan_refs(Node *node,
							   Index subvarno,
							   List *subplanTargetList)
{
	replace_vars_with_subplan_refs_context context;

	context.subvarno = subvarno;
	context.subplanTargetList = subplanTargetList;
	return replace_vars_with_subplan_refs_mutator(node, &context);
}

static Node *
replace_vars_with_subplan_refs_mutator(Node *node,
							 replace_vars_with_subplan_refs_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		Var		   *newvar = (Var *) copyObject(var);
		Resdom	   *resdom;

		resdom = tlist_member((Node *) var, context->subplanTargetList);
		if (!resdom)
			elog(ERROR, "replace_vars_with_subplan_refs: variable not in subplan target list");

		newvar->varno = context->subvarno;
		newvar->varattno = resdom->resno;
		return (Node *) newvar;
	}
	/*
	 * expression_tree_mutator will copy SubPlan nodes if given a chance.
	 * We do not want to do that here, because subselect.c has already
	 * constructed the initPlan and subPlan lists of the current plan node
	 * and we mustn't leave those dangling (ie, pointing to different
	 * copies of the nodes than what's in the targetlist & quals...)
	 * Instead, alter the SubPlan in-place.  Grotty --- is there a better way?
	 */
	if (is_subplan(node))
	{
		Expr	   *expr = (Expr *) node;
		SubLink	   *sublink = ((SubPlan *) expr->oper)->sublink;

		/* transform args list (params to be passed to subplan) */
		expr->args = (List *)
			replace_vars_with_subplan_refs_mutator((Node *) expr->args,
												   context);
		/* transform sublink's oper list as well */
		sublink->oper = (List *)
			replace_vars_with_subplan_refs_mutator((Node *) sublink->oper,
												   context);

		return (Node *) expr;
	}
	return expression_tree_mutator(node,
								   replace_vars_with_subplan_refs_mutator,
								   (void *) context);
}

/*****************************************************************************
 *					OPERATOR REGPROC LOOKUP
 *****************************************************************************/

/*
 * fix_opids
 *	  Calculate opid field from opno for each Oper node in given tree.
 *	  The given tree can be anything expression_tree_walker handles.
 *
 * The argument is modified in-place.  (This is OK since we'd want the
 * same change for any node, even if it gets visited more than once due to
 * shared structure.)
 */
void
fix_opids(Node *node)
{
	/* This tree walk requires no special setup, so away we go... */
	fix_opids_walker(node, NULL);
}

static bool
fix_opids_walker (Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (is_opclause(node))
		replace_opid((Oper *) ((Expr *) node)->oper);
	return expression_tree_walker(node, fix_opids_walker, context);
}
