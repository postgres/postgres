/*-------------------------------------------------------------------------
 *
 * setrefs.c
 *	  Post-processing of a completed plan tree: fix references to subplan
 *	  vars, and compute regproc values for operators
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/setrefs.c,v 1.97.4.1 2004/05/11 13:15:23 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"


typedef struct
{
	List	   *rtable;
	List	   *outer_tlist;
	List	   *inner_tlist;
	Index		acceptable_rel;
	bool		tlists_have_non_vars;
} join_references_context;

typedef struct
{
	Index		subvarno;
	List	   *subplan_targetlist;
	bool		tlist_has_non_vars;
} replace_vars_with_subplan_refs_context;

static void fix_expr_references(Plan *plan, Node *node);
static bool fix_expr_references_walker(Node *node, void *context);
static void set_join_references(Join *join, List *rtable);
static void set_uppernode_references(Plan *plan, Index subvarno);
static bool targetlist_has_non_vars(List *tlist);
static List *join_references(List *clauses,
				List *rtable,
				List *outer_tlist,
				List *inner_tlist,
				Index acceptable_rel,
				bool tlists_have_non_vars);
static Node *join_references_mutator(Node *node,
						join_references_context *context);
static Node *replace_vars_with_subplan_refs(Node *node,
							   Index subvarno,
							   List *subplan_targetlist,
							   bool tlist_has_non_vars);
static Node *replace_vars_with_subplan_refs_mutator(Node *node,
						replace_vars_with_subplan_refs_context *context);
static bool fix_opfuncids_walker(Node *node, void *context);
static void set_sa_opfuncid(ScalarArrayOpExpr *opexpr);


/*****************************************************************************
 *
 *		SUBPLAN REFERENCES
 *
 *****************************************************************************/

/*
 * set_plan_references
 *	  This is the final processing pass of the planner/optimizer.  The plan
 *	  tree is complete; we just have to adjust some representational details
 *	  for the convenience of the executor.	We update Vars in upper plan nodes
 *	  to refer to the outputs of their subplans, and we compute regproc OIDs
 *	  for operators (ie, we look up the function that implements each op).
 *
 *	  set_plan_references recursively traverses the whole plan tree.
 *
 * Returns nothing of interest, but modifies internal fields of nodes.
 */
void
set_plan_references(Plan *plan, List *rtable)
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
			fix_expr_references(plan, (Node *) plan->targetlist);
			fix_expr_references(plan, (Node *) plan->qual);
			break;
		case T_IndexScan:
			fix_expr_references(plan, (Node *) plan->targetlist);
			fix_expr_references(plan, (Node *) plan->qual);
			fix_expr_references(plan,
								(Node *) ((IndexScan *) plan)->indxqual);
			fix_expr_references(plan,
							(Node *) ((IndexScan *) plan)->indxqualorig);
			break;
		case T_TidScan:
			fix_expr_references(plan, (Node *) plan->targetlist);
			fix_expr_references(plan, (Node *) plan->qual);
			fix_expr_references(plan,
								(Node *) ((TidScan *) plan)->tideval);
			break;
		case T_SubqueryScan:
			{
				RangeTblEntry *rte;

				/*
				 * We do not do set_uppernode_references() here, because a
				 * SubqueryScan will always have been created with correct
				 * references to its subplan's outputs to begin with.
				 */
				fix_expr_references(plan, (Node *) plan->targetlist);
				fix_expr_references(plan, (Node *) plan->qual);

				/* Recurse into subplan too */
				rte = rt_fetch(((SubqueryScan *) plan)->scan.scanrelid,
							   rtable);
				Assert(rte->rtekind == RTE_SUBQUERY);
				set_plan_references(((SubqueryScan *) plan)->subplan,
									rte->subquery->rtable);
			}
			break;
		case T_FunctionScan:
			{
				RangeTblEntry *rte;

				fix_expr_references(plan, (Node *) plan->targetlist);
				fix_expr_references(plan, (Node *) plan->qual);
				rte = rt_fetch(((FunctionScan *) plan)->scan.scanrelid,
							   rtable);
				Assert(rte->rtekind == RTE_FUNCTION);
				fix_expr_references(plan, rte->funcexpr);
			}
			break;
		case T_NestLoop:
			set_join_references((Join *) plan, rtable);
			fix_expr_references(plan, (Node *) plan->targetlist);
			fix_expr_references(plan, (Node *) plan->qual);
			fix_expr_references(plan, (Node *) ((Join *) plan)->joinqual);
			break;
		case T_MergeJoin:
			set_join_references((Join *) plan, rtable);
			fix_expr_references(plan, (Node *) plan->targetlist);
			fix_expr_references(plan, (Node *) plan->qual);
			fix_expr_references(plan, (Node *) ((Join *) plan)->joinqual);
			fix_expr_references(plan,
							(Node *) ((MergeJoin *) plan)->mergeclauses);
			break;
		case T_HashJoin:
			set_join_references((Join *) plan, rtable);
			fix_expr_references(plan, (Node *) plan->targetlist);
			fix_expr_references(plan, (Node *) plan->qual);
			fix_expr_references(plan, (Node *) ((Join *) plan)->joinqual);
			fix_expr_references(plan,
							  (Node *) ((HashJoin *) plan)->hashclauses);
			break;
		case T_Hash:

			/*
			 * Hash does not evaluate its targetlist or quals, so don't
			 * touch those (see comments below).  But we do need to fix
			 * its hashkeys.  The hashkeys are a little bizarre because
			 * they need to match the hashclauses of the parent HashJoin
			 * node, so we use join_references to fix them.
			 */
			((Hash *) plan)->hashkeys =
				join_references(((Hash *) plan)->hashkeys,
								rtable,
								NIL,
								plan->lefttree->targetlist,
								(Index) 0,
					targetlist_has_non_vars(plan->lefttree->targetlist));
			fix_expr_references(plan,
								(Node *) ((Hash *) plan)->hashkeys);
			break;
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_SetOp:

			/*
			 * These plan types don't actually bother to evaluate their
			 * targetlists or quals (because they just return their
			 * unmodified input tuples).  The optimizer is lazy about
			 * creating really valid targetlists for them.	Best to just
			 * leave the targetlist alone.	In particular, we do not want
			 * to process subplans for them, since we will likely end up
			 * reprocessing subplans that also appear in lower levels of
			 * the plan tree!
			 */
			break;
		case T_Limit:
			/*
			 * Like the plan types above, Limit doesn't evaluate its
			 * tlist or quals.  It does have live expressions for
			 * limit/offset, however.
			 */
			fix_expr_references(plan, ((Limit *) plan)->limitOffset);
			fix_expr_references(plan, ((Limit *) plan)->limitCount);
			break;
		case T_Agg:
		case T_Group:
			set_uppernode_references(plan, (Index) 0);
			fix_expr_references(plan, (Node *) plan->targetlist);
			fix_expr_references(plan, (Node *) plan->qual);
			break;
		case T_Result:

			/*
			 * Result may or may not have a subplan; no need to fix up
			 * subplan references if it hasn't got one...
			 *
			 * XXX why does Result use a different subvarno from Agg/Group?
			 */
			if (plan->lefttree != NULL)
				set_uppernode_references(plan, (Index) OUTER);
			fix_expr_references(plan, (Node *) plan->targetlist);
			fix_expr_references(plan, (Node *) plan->qual);
			fix_expr_references(plan, ((Result *) plan)->resconstantqual);
			break;
		case T_Append:

			/*
			 * Append, like Sort et al, doesn't actually evaluate its
			 * targetlist or quals, and we haven't bothered to give it its
			 * own tlist copy.	So, don't fix targetlist/qual. But do
			 * recurse into child plans.
			 */
			foreach(pl, ((Append *) plan)->appendplans)
				set_plan_references((Plan *) lfirst(pl), rtable);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(plan));
			break;
	}

	/*
	 * Now recurse into child plans and initplans, if any
	 *
	 * NOTE: it is essential that we recurse into child plans AFTER we set
	 * subplan references in this plan's tlist and quals.  If we did the
	 * reference-adjustments bottom-up, then we would fail to match this
	 * plan's var nodes against the already-modified nodes of the
	 * children.  Fortunately, that consideration doesn't apply to SubPlan
	 * nodes; else we'd need two passes over the expression trees.
	 */
	set_plan_references(plan->lefttree, rtable);
	set_plan_references(plan->righttree, rtable);

	foreach(pl, plan->initPlan)
	{
		SubPlan    *sp = (SubPlan *) lfirst(pl);

		Assert(IsA(sp, SubPlan));
		set_plan_references(sp->plan, sp->rtable);
	}
}

/*
 * fix_expr_references
 *	  Do final cleanup on expressions (targetlists or quals).
 *
 * This consists of looking up operator opcode info for OpExpr nodes
 * and recursively performing set_plan_references on subplans.
 *
 * The Plan argument is currently unused, but might be needed again someday.
 */
static void
fix_expr_references(Plan *plan, Node *node)
{
	/* This tree walk requires no special setup, so away we go... */
	fix_expr_references_walker(node, NULL);
}

static bool
fix_expr_references_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, OpExpr))
		set_opfuncid((OpExpr *) node);
	else if (IsA(node, DistinctExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, ScalarArrayOpExpr))
		set_sa_opfuncid((ScalarArrayOpExpr *) node);
	else if (IsA(node, NullIfExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, SubPlan))
	{
		SubPlan    *sp = (SubPlan *) node;

		set_plan_references(sp->plan, sp->rtable);
	}
	return expression_tree_walker(node, fix_expr_references_walker, context);
}

/*
 * set_join_references
 *	  Modifies the target list and quals of a join node to reference its
 *	  subplans, by setting the varnos to OUTER or INNER and setting attno
 *	  values to the result domain number of either the corresponding outer
 *	  or inner join tuple item.
 *
 * In the case of a nestloop with inner indexscan, we will also need to
 * apply the same transformation to any outer vars appearing in the
 * quals of the child indexscan.
 *
 *	'join' is a join plan node
 *	'rtable' is the associated range table
 */
static void
set_join_references(Join *join, List *rtable)
{
	Plan	   *outer_plan = join->plan.lefttree;
	Plan	   *inner_plan = join->plan.righttree;
	List	   *outer_tlist = outer_plan->targetlist;
	List	   *inner_tlist = inner_plan->targetlist;
	bool		tlists_have_non_vars;

	tlists_have_non_vars = targetlist_has_non_vars(outer_tlist) ||
		targetlist_has_non_vars(inner_tlist);

	/* All join plans have tlist, qual, and joinqual */
	join->plan.targetlist = join_references(join->plan.targetlist,
											rtable,
											outer_tlist,
											inner_tlist,
											(Index) 0,
											tlists_have_non_vars);
	join->plan.qual = join_references(join->plan.qual,
									  rtable,
									  outer_tlist,
									  inner_tlist,
									  (Index) 0,
									  tlists_have_non_vars);
	join->joinqual = join_references(join->joinqual,
									 rtable,
									 outer_tlist,
									 inner_tlist,
									 (Index) 0,
									 tlists_have_non_vars);

	/* Now do join-type-specific stuff */
	if (IsA(join, NestLoop))
	{
		if (IsA(inner_plan, IndexScan))
		{
			/*
			 * An index is being used to reduce the number of tuples
			 * scanned in the inner relation.  If there are join clauses
			 * being used with the index, we must update their outer-rel
			 * var nodes to refer to the outer side of the join.
			 */
			IndexScan  *innerscan = (IndexScan *) inner_plan;
			List	   *indxqualorig = innerscan->indxqualorig;

			/* No work needed if indxqual refers only to its own rel... */
			if (NumRelids((Node *) indxqualorig) > 1)
			{
				Index		innerrel = innerscan->scan.scanrelid;

				/* only refs to outer vars get changed in the inner qual */
				innerscan->indxqualorig = join_references(indxqualorig,
														  rtable,
														  outer_tlist,
														  NIL,
														  innerrel,
												   tlists_have_non_vars);
				innerscan->indxqual = join_references(innerscan->indxqual,
													  rtable,
													  outer_tlist,
													  NIL,
													  innerrel,
												   tlists_have_non_vars);

				/*
				 * We must fix the inner qpqual too, if it has join
				 * clauses (this could happen if the index is lossy: some
				 * indxquals may get rechecked as qpquals).
				 */
				if (NumRelids((Node *) inner_plan->qual) > 1)
					inner_plan->qual = join_references(inner_plan->qual,
													   rtable,
													   outer_tlist,
													   NIL,
													   innerrel,
												   tlists_have_non_vars);
			}
		}
		else if (IsA(inner_plan, TidScan))
		{
			TidScan    *innerscan = (TidScan *) inner_plan;
			Index		innerrel = innerscan->scan.scanrelid;

			innerscan->tideval = join_references(innerscan->tideval,
												 rtable,
												 outer_tlist,
												 NIL,
												 innerrel,
												 tlists_have_non_vars);
		}
	}
	else if (IsA(join, MergeJoin))
	{
		MergeJoin  *mj = (MergeJoin *) join;

		mj->mergeclauses = join_references(mj->mergeclauses,
										   rtable,
										   outer_tlist,
										   inner_tlist,
										   (Index) 0,
										   tlists_have_non_vars);
	}
	else if (IsA(join, HashJoin))
	{
		HashJoin   *hj = (HashJoin *) join;

		hj->hashclauses = join_references(hj->hashclauses,
										  rtable,
										  outer_tlist,
										  inner_tlist,
										  (Index) 0,
										  tlists_have_non_vars);
	}
}

/*
 * set_uppernode_references
 *	  Update the targetlist and quals of an upper-level plan node
 *	  to refer to the tuples returned by its lefttree subplan.
 *
 * This is used for single-input plan types like Agg, Group, Result.
 *
 * In most cases, we have to match up individual Vars in the tlist and
 * qual expressions with elements of the subplan's tlist (which was
 * generated by flatten_tlist() from these selfsame expressions, so it
 * should have all the required variables).  There is an important exception,
 * however: GROUP BY and ORDER BY expressions will have been pushed into the
 * subplan tlist unflattened.  If these values are also needed in the output
 * then we want to reference the subplan tlist element rather than recomputing
 * the expression.
 */
static void
set_uppernode_references(Plan *plan, Index subvarno)
{
	Plan	   *subplan = plan->lefttree;
	List	   *subplan_targetlist,
			   *output_targetlist,
			   *l;
	bool		tlist_has_non_vars;

	if (subplan != NULL)
		subplan_targetlist = subplan->targetlist;
	else
		subplan_targetlist = NIL;

	tlist_has_non_vars = targetlist_has_non_vars(subplan_targetlist);

	output_targetlist = NIL;
	foreach(l, plan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		Node	   *newexpr;

		newexpr = replace_vars_with_subplan_refs((Node *) tle->expr,
												 subvarno,
												 subplan_targetlist,
												 tlist_has_non_vars);
		output_targetlist = lappend(output_targetlist,
									makeTargetEntry(tle->resdom,
													(Expr *) newexpr));
	}
	plan->targetlist = output_targetlist;

	plan->qual = (List *)
		replace_vars_with_subplan_refs((Node *) plan->qual,
									   subvarno,
									   subplan_targetlist,
									   tlist_has_non_vars);
}

/*
 * targetlist_has_non_vars --- are there any non-Var entries in tlist?
 *
 * In most cases, subplan tlists will be "flat" tlists with only Vars.
 * Checking for this allows us to save comparisons in common cases.
 */
static bool
targetlist_has_non_vars(List *tlist)
{
	List	   *l;

	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->expr && !IsA(tle->expr, Var))
			return true;
	}
	return false;
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
 * 'rtable' is the current range table
 * 'outer_tlist' is the target list of the outer join relation
 * 'inner_tlist' is the target list of the inner join relation, or NIL
 * 'acceptable_rel' is either zero or the rangetable index of a relation
 *		whose Vars may appear in the clause without provoking an error.
 *
 * Returns the new expression tree.  The original clause structure is
 * not modified.
 */
static List *
join_references(List *clauses,
				List *rtable,
				List *outer_tlist,
				List *inner_tlist,
				Index acceptable_rel,
				bool tlists_have_non_vars)
{
	join_references_context context;

	context.rtable = rtable;
	context.outer_tlist = outer_tlist;
	context.inner_tlist = inner_tlist;
	context.acceptable_rel = acceptable_rel;
	context.tlists_have_non_vars = tlists_have_non_vars;
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
		Resdom	   *resdom;

		/* First look for the var in the input tlists */
		resdom = tlist_member((Node *) var, context->outer_tlist);
		if (resdom)
		{
			Var		   *newvar = (Var *) copyObject(var);

			newvar->varno = OUTER;
			newvar->varattno = resdom->resno;
			return (Node *) newvar;
		}
		resdom = tlist_member((Node *) var, context->inner_tlist);
		if (resdom)
		{
			Var		   *newvar = (Var *) copyObject(var);

			newvar->varno = INNER;
			newvar->varattno = resdom->resno;
			return (Node *) newvar;
		}

		/* Return the Var unmodified, if it's for acceptable_rel */
		if (var->varno == context->acceptable_rel)
			return (Node *) copyObject(var);

		/* No referent found for Var */
		elog(ERROR, "variable not found in subplan target lists");
	}
	/* Try matching more complex expressions too, if tlists have any */
	if (context->tlists_have_non_vars)
	{
		Resdom	   *resdom;

		resdom = tlist_member(node, context->outer_tlist);
		if (resdom)
		{
			/* Found a matching subplan output expression */
			Var		   *newvar;

			newvar = makeVar(OUTER,
							 resdom->resno,
							 resdom->restype,
							 resdom->restypmod,
							 0);
			newvar->varnoold = 0;		/* wasn't ever a plain Var */
			newvar->varoattno = 0;
			return (Node *) newvar;
		}
		resdom = tlist_member(node, context->inner_tlist);
		if (resdom)
		{
			/* Found a matching subplan output expression */
			Var		   *newvar;

			newvar = makeVar(INNER,
							 resdom->resno,
							 resdom->restype,
							 resdom->restypmod,
							 0);
			newvar->varnoold = 0;		/* wasn't ever a plain Var */
			newvar->varoattno = 0;
			return (Node *) newvar;
		}
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
 * If tlist_has_non_vars is true, then we try to match whole subexpressions
 * against elements of the subplan tlist, so that we can avoid recomputing
 * expressions that were already computed by the subplan.  (This is relatively
 * expensive, so we don't want to try it in the common case where the
 * subplan tlist is just a flattened list of Vars.)
 *
 * 'node': the tree to be fixed (a target item or qual)
 * 'subvarno': varno to be assigned to all Vars
 * 'subplan_targetlist': target list for subplan
 * 'tlist_has_non_vars': true if subplan_targetlist contains non-Var exprs
 *
 * The resulting tree is a copy of the original in which all Var nodes have
 * varno = subvarno, varattno = resno of corresponding subplan target.
 * The original tree is not modified.
 */
static Node *
replace_vars_with_subplan_refs(Node *node,
							   Index subvarno,
							   List *subplan_targetlist,
							   bool tlist_has_non_vars)
{
	replace_vars_with_subplan_refs_context context;

	context.subvarno = subvarno;
	context.subplan_targetlist = subplan_targetlist;
	context.tlist_has_non_vars = tlist_has_non_vars;
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
		Resdom	   *resdom;
		Var		   *newvar;

		resdom = tlist_member((Node *) var, context->subplan_targetlist);
		if (!resdom)
			elog(ERROR, "variable not found in subplan target list");
		newvar = (Var *) copyObject(var);
		newvar->varno = context->subvarno;
		newvar->varattno = resdom->resno;
		return (Node *) newvar;
	}
	/* Try matching more complex expressions too, if tlist has any */
	if (context->tlist_has_non_vars)
	{
		Resdom	   *resdom;

		resdom = tlist_member(node, context->subplan_targetlist);
		if (resdom)
		{
			/* Found a matching subplan output expression */
			Var		   *newvar;

			newvar = makeVar(context->subvarno,
							 resdom->resno,
							 resdom->restype,
							 resdom->restypmod,
							 0);
			newvar->varnoold = 0;		/* wasn't ever a plain Var */
			newvar->varoattno = 0;
			return (Node *) newvar;
		}
	}
	return expression_tree_mutator(node,
								   replace_vars_with_subplan_refs_mutator,
								   (void *) context);
}

/*****************************************************************************
 *					OPERATOR REGPROC LOOKUP
 *****************************************************************************/

/*
 * fix_opfuncids
 *	  Calculate opfuncid field from opno for each OpExpr node in given tree.
 *	  The given tree can be anything expression_tree_walker handles.
 *
 * The argument is modified in-place.  (This is OK since we'd want the
 * same change for any node, even if it gets visited more than once due to
 * shared structure.)
 */
void
fix_opfuncids(Node *node)
{
	/* This tree walk requires no special setup, so away we go... */
	fix_opfuncids_walker(node, NULL);
}

static bool
fix_opfuncids_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, OpExpr))
		set_opfuncid((OpExpr *) node);
	else if (IsA(node, DistinctExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, ScalarArrayOpExpr))
		set_sa_opfuncid((ScalarArrayOpExpr *) node);
	else if (IsA(node, NullIfExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	return expression_tree_walker(node, fix_opfuncids_walker, context);
}

/*
 * set_opfuncid
 *		Set the opfuncid (procedure OID) in an OpExpr node,
 *		if it hasn't been set already.
 *
 * Because of struct equivalence, this can also be used for
 * DistinctExpr and NullIfExpr nodes.
 */
void
set_opfuncid(OpExpr *opexpr)
{
	if (opexpr->opfuncid == InvalidOid)
		opexpr->opfuncid = get_opcode(opexpr->opno);
}

/*
 * set_sa_opfuncid
 *		As above, for ScalarArrayOpExpr nodes.
 */
static void
set_sa_opfuncid(ScalarArrayOpExpr *opexpr)
{
	if (opexpr->opfuncid == InvalidOid)
		opexpr->opfuncid = get_opcode(opexpr->opno);
}
