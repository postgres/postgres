/*-------------------------------------------------------------------------
 *
 * setrefs.c
 *	  Post-processing of a completed plan tree: fix references to subplan
 *	  vars, and compute regproc values for operators
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/plan/setrefs.c,v 1.126.2.1 2007/02/16 03:49:10 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "parser/parse_expr.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"


typedef struct
{
	Index		varno;			/* RT index of Var */
	AttrNumber	varattno;		/* attr number of Var */
	AttrNumber	resno;			/* TLE position of Var */
} tlist_vinfo;

typedef struct
{
	List	   *tlist;			/* underlying target list */
	int			num_vars;		/* number of plain Var tlist entries */
	bool		has_non_vars;	/* are there non-plain-Var entries? */
	/* array of num_vars entries: */
	tlist_vinfo vars[1];		/* VARIABLE LENGTH ARRAY */
} indexed_tlist;				/* VARIABLE LENGTH STRUCT */

typedef struct
{
	indexed_tlist *outer_itlist;
	indexed_tlist *inner_itlist;
	Index		acceptable_rel;
} join_references_context;

typedef struct
{
	indexed_tlist *subplan_itlist;
	Index		subvarno;
} replace_vars_with_subplan_refs_context;

static Plan *set_subqueryscan_references(SubqueryScan *plan, List *rtable);
static bool trivial_subqueryscan(SubqueryScan *plan);
static void adjust_plan_varnos(Plan *plan, int rtoffset);
static void adjust_expr_varnos(Node *node, int rtoffset);
static bool adjust_expr_varnos_walker(Node *node, int *context);
static void fix_expr_references(Plan *plan, Node *node);
static bool fix_expr_references_walker(Node *node, void *context);
static void set_join_references(Join *join);
static void set_inner_join_references(Plan *inner_plan,
						  indexed_tlist *outer_itlist);
static void set_uppernode_references(Plan *plan, Index subvarno);
static indexed_tlist *build_tlist_index(List *tlist);
static Var *search_indexed_tlist_for_var(Var *var,
							 indexed_tlist *itlist,
							 Index newvarno);
static Var *search_indexed_tlist_for_non_var(Node *node,
								 indexed_tlist *itlist,
								 Index newvarno);
static List *join_references(List *clauses,
				indexed_tlist *outer_itlist,
				indexed_tlist *inner_itlist,
				Index acceptable_rel);
static Node *join_references_mutator(Node *node,
						join_references_context *context);
static Node *replace_vars_with_subplan_refs(Node *node,
							   indexed_tlist *subplan_itlist,
							   Index subvarno);
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
 *
 * This is the final processing pass of the planner/optimizer.	The plan
 * tree is complete; we just have to adjust some representational details
 * for the convenience of the executor.  We update Vars in upper plan nodes
 * to refer to the outputs of their subplans, and we compute regproc OIDs
 * for operators (ie, we look up the function that implements each op).
 *
 * We also perform one final optimization step, which is to delete
 * SubqueryScan plan nodes that aren't doing anything useful (ie, have
 * no qual and a no-op targetlist).  The reason for doing this last is that
 * it can't readily be done before set_plan_references, because it would
 * break set_uppernode_references: the Vars in the subquery's top tlist
 * won't match up with the Vars in the outer plan tree.  The SubqueryScan
 * serves a necessary function as a buffer between outer query and subquery
 * variable numbering ... but the executor doesn't care about that, only the
 * planner.
 *
 * set_plan_references recursively traverses the whole plan tree.
 *
 * The return value is normally the same Plan node passed in, but can be
 * different when the passed-in Plan is a SubqueryScan we decide isn't needed.
 *
 * Note: to delete a SubqueryScan, we have to renumber Vars in its child nodes
 * and append the modified subquery rangetable to the outer rangetable.
 * Therefore "rtable" is an in/out argument and really should be declared
 * "List **".  But in the interest of notational simplicity we don't do that.
 * (Since rtable can't be NIL if there's a SubqueryScan, the list header
 * address won't change when we append a subquery rangetable.)
 */
Plan *
set_plan_references(Plan *plan, List *rtable)
{
	ListCell   *l;

	if (plan == NULL)
		return NULL;

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
								(Node *) ((IndexScan *) plan)->indexqual);
			fix_expr_references(plan,
								(Node *) ((IndexScan *) plan)->indexqualorig);
			break;
		case T_BitmapIndexScan:
			/* no need to fix targetlist and qual */
			Assert(plan->targetlist == NIL);
			Assert(plan->qual == NIL);
			fix_expr_references(plan,
							 (Node *) ((BitmapIndexScan *) plan)->indexqual);
			fix_expr_references(plan,
						 (Node *) ((BitmapIndexScan *) plan)->indexqualorig);
			break;
		case T_BitmapHeapScan:
			fix_expr_references(plan, (Node *) plan->targetlist);
			fix_expr_references(plan, (Node *) plan->qual);
			fix_expr_references(plan,
						 (Node *) ((BitmapHeapScan *) plan)->bitmapqualorig);
			break;
		case T_TidScan:
			fix_expr_references(plan, (Node *) plan->targetlist);
			fix_expr_references(plan, (Node *) plan->qual);
			fix_expr_references(plan, (Node *) ((TidScan *) plan)->tidquals);
			break;
		case T_SubqueryScan:
			/* Needs special treatment, see comments below */
			return set_subqueryscan_references((SubqueryScan *) plan, rtable);
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
		case T_ValuesScan:
			{
				RangeTblEntry *rte;

				fix_expr_references(plan, (Node *) plan->targetlist);
				fix_expr_references(plan, (Node *) plan->qual);
				rte = rt_fetch(((ValuesScan *) plan)->scan.scanrelid,
							   rtable);
				Assert(rte->rtekind == RTE_VALUES);
				fix_expr_references(plan, (Node *) rte->values_lists);
			}
			break;
		case T_NestLoop:
			set_join_references((Join *) plan);
			fix_expr_references(plan, (Node *) plan->targetlist);
			fix_expr_references(plan, (Node *) plan->qual);
			fix_expr_references(plan, (Node *) ((Join *) plan)->joinqual);
			break;
		case T_MergeJoin:
			set_join_references((Join *) plan);
			fix_expr_references(plan, (Node *) plan->targetlist);
			fix_expr_references(plan, (Node *) plan->qual);
			fix_expr_references(plan, (Node *) ((Join *) plan)->joinqual);
			fix_expr_references(plan,
								(Node *) ((MergeJoin *) plan)->mergeclauses);
			break;
		case T_HashJoin:
			set_join_references((Join *) plan);
			fix_expr_references(plan, (Node *) plan->targetlist);
			fix_expr_references(plan, (Node *) plan->qual);
			fix_expr_references(plan, (Node *) ((Join *) plan)->joinqual);
			fix_expr_references(plan,
								(Node *) ((HashJoin *) plan)->hashclauses);
			break;
		case T_Hash:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_SetOp:

			/*
			 * These plan types don't actually bother to evaluate their
			 * targetlists (because they just return their unmodified input
			 * tuples).  The optimizer is lazy about creating really valid
			 * targetlists for them --- it tends to just put in a pointer to
			 * the child plan node's tlist.  Hence, we leave the tlist alone.
			 * In particular, we do not want to process subplans in the tlist,
			 * since we will likely end up reprocessing subplans that also
			 * appear in lower levels of the plan tree!
			 *
			 * Since these plan types don't check quals either, we should not
			 * find any qual expression attached to them.
			 */
			Assert(plan->qual == NIL);
			break;
		case T_Limit:

			/*
			 * Like the plan types above, Limit doesn't evaluate its tlist or
			 * quals.  It does have live expressions for limit/offset,
			 * however.
			 */
			Assert(plan->qual == NIL);
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
			 * Result may or may not have a subplan; no need to fix up subplan
			 * references if it hasn't got one...
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
			 * targetlist or check quals, and we haven't bothered to give it
			 * its own tlist copy. So, don't fix targetlist/qual. But do
			 * recurse into child plans.
			 */
			Assert(plan->qual == NIL);
			foreach(l, ((Append *) plan)->appendplans)
				lfirst(l) = set_plan_references((Plan *) lfirst(l), rtable);
			break;
		case T_BitmapAnd:
			/* BitmapAnd works like Append, but has no tlist */
			Assert(plan->targetlist == NIL);
			Assert(plan->qual == NIL);
			foreach(l, ((BitmapAnd *) plan)->bitmapplans)
				lfirst(l) = set_plan_references((Plan *) lfirst(l), rtable);
			break;
		case T_BitmapOr:
			/* BitmapOr works like Append, but has no tlist */
			Assert(plan->targetlist == NIL);
			Assert(plan->qual == NIL);
			foreach(l, ((BitmapOr *) plan)->bitmapplans)
				lfirst(l) = set_plan_references((Plan *) lfirst(l), rtable);
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
	 * plan's var nodes against the already-modified nodes of the children.
	 * Fortunately, that consideration doesn't apply to SubPlan nodes; else
	 * we'd need two passes over the expression trees.
	 */
	plan->lefttree = set_plan_references(plan->lefttree, rtable);
	plan->righttree = set_plan_references(plan->righttree, rtable);

	foreach(l, plan->initPlan)
	{
		SubPlan    *sp = (SubPlan *) lfirst(l);

		Assert(IsA(sp, SubPlan));
		sp->plan = set_plan_references(sp->plan, sp->rtable);
	}

	return plan;
}

/*
 * set_subqueryscan_references
 *		Do set_plan_references processing on a SubqueryScan
 *
 * We try to strip out the SubqueryScan entirely; if we can't, we have
 * to do the normal processing on it.
 */
static Plan *
set_subqueryscan_references(SubqueryScan *plan, List *rtable)
{
	Plan	   *result;
	RangeTblEntry *rte;
	ListCell   *l;

	/* First, recursively process the subplan */
	rte = rt_fetch(plan->scan.scanrelid, rtable);
	Assert(rte->rtekind == RTE_SUBQUERY);
	plan->subplan = set_plan_references(plan->subplan,
										rte->subquery->rtable);

	/*
	 * We have to process any initplans too; set_plan_references can't do it
	 * for us because of the possibility of double-processing.
	 */
	foreach(l, plan->scan.plan.initPlan)
	{
		SubPlan    *sp = (SubPlan *) lfirst(l);

		Assert(IsA(sp, SubPlan));
		sp->plan = set_plan_references(sp->plan, sp->rtable);
	}

	if (trivial_subqueryscan(plan))
	{
		/*
		 * We can omit the SubqueryScan node and just pull up the subplan. We
		 * have to merge its rtable into the outer rtable, which means
		 * adjusting varnos throughout the subtree.
		 */
		int			rtoffset = list_length(rtable);
		List	   *sub_rtable;
		ListCell   *lp,
				   *lc;

		sub_rtable = copyObject(rte->subquery->rtable);
		range_table_walker(sub_rtable,
						   adjust_expr_varnos_walker,
						   (void *) &rtoffset,
						   QTW_IGNORE_RT_SUBQUERIES);
		rtable = list_concat(rtable, sub_rtable);

		/*
		 * we have to copy the subplan to make sure there are no duplicately
		 * linked nodes in it, else adjust_plan_varnos might increment some
		 * varnos twice
		 */
		result = copyObject(plan->subplan);

		adjust_plan_varnos(result, rtoffset);

		result->initPlan = list_concat(plan->scan.plan.initPlan,
									   result->initPlan);

		/*
		 * We also have to transfer the SubqueryScan's result-column names
		 * into the subplan, else columns sent to client will be improperly
		 * labeled if this is the topmost plan level.  Copy the "source
		 * column" information too.
		 */
		forboth(lp, plan->scan.plan.targetlist, lc, result->targetlist)
		{
			TargetEntry *ptle = (TargetEntry *) lfirst(lp);
			TargetEntry *ctle = (TargetEntry *) lfirst(lc);

			ctle->resname = ptle->resname;
			ctle->resorigtbl = ptle->resorigtbl;
			ctle->resorigcol = ptle->resorigcol;
		}
	}
	else
	{
		/*
		 * Keep the SubqueryScan node.	We have to do the processing that
		 * set_plan_references would otherwise have done on it.  Notice we do
		 * not do set_uppernode_references() here, because a SubqueryScan will
		 * always have been created with correct references to its subplan's
		 * outputs to begin with.
		 */
		result = (Plan *) plan;

		fix_expr_references(result, (Node *) result->targetlist);
		fix_expr_references(result, (Node *) result->qual);
	}

	return result;
}

/*
 * trivial_subqueryscan
 *		Detect whether a SubqueryScan can be deleted from the plan tree.
 *
 * We can delete it if it has no qual to check and the targetlist just
 * regurgitates the output of the child plan.
 */
static bool
trivial_subqueryscan(SubqueryScan *plan)
{
	int			attrno;
	ListCell   *lp,
			   *lc;

	if (plan->scan.plan.qual != NIL)
		return false;

	if (list_length(plan->scan.plan.targetlist) !=
		list_length(plan->subplan->targetlist))
		return false;			/* tlists not same length */

	attrno = 1;
	forboth(lp, plan->scan.plan.targetlist, lc, plan->subplan->targetlist)
	{
		TargetEntry *ptle = (TargetEntry *) lfirst(lp);
		TargetEntry *ctle = (TargetEntry *) lfirst(lc);

		if (ptle->resjunk != ctle->resjunk)
			return false;		/* tlist doesn't match junk status */

		/*
		 * We accept either a Var referencing the corresponding element of the
		 * subplan tlist, or a Const equaling the subplan element. See
		 * generate_setop_tlist() for motivation.
		 */
		if (ptle->expr && IsA(ptle->expr, Var))
		{
			Var		   *var = (Var *) ptle->expr;

			Assert(var->varno == plan->scan.scanrelid);
			Assert(var->varlevelsup == 0);
			if (var->varattno != attrno)
				return false;	/* out of order */
		}
		else if (ptle->expr && IsA(ptle->expr, Const))
		{
			if (!equal(ptle->expr, ctle->expr))
				return false;
		}
		else
			return false;

		attrno++;
	}

	return true;
}

/*
 * adjust_plan_varnos
 *		Offset varnos and other rangetable indexes in a plan tree by rtoffset.
 */
static void
adjust_plan_varnos(Plan *plan, int rtoffset)
{
	ListCell   *l;

	if (plan == NULL)
		return;

	/*
	 * Plan-type-specific fixes
	 */
	switch (nodeTag(plan))
	{
		case T_SeqScan:
			((SeqScan *) plan)->scanrelid += rtoffset;
			adjust_expr_varnos((Node *) plan->targetlist, rtoffset);
			adjust_expr_varnos((Node *) plan->qual, rtoffset);
			break;
		case T_IndexScan:
			((IndexScan *) plan)->scan.scanrelid += rtoffset;
			adjust_expr_varnos((Node *) plan->targetlist, rtoffset);
			adjust_expr_varnos((Node *) plan->qual, rtoffset);
			adjust_expr_varnos((Node *) ((IndexScan *) plan)->indexqual,
							   rtoffset);
			adjust_expr_varnos((Node *) ((IndexScan *) plan)->indexqualorig,
							   rtoffset);
			break;
		case T_BitmapIndexScan:
			((BitmapIndexScan *) plan)->scan.scanrelid += rtoffset;
			/* no need to fix targetlist and qual */
			Assert(plan->targetlist == NIL);
			Assert(plan->qual == NIL);
			adjust_expr_varnos((Node *) ((BitmapIndexScan *) plan)->indexqual,
							   rtoffset);
			adjust_expr_varnos((Node *) ((BitmapIndexScan *) plan)->indexqualorig,
							   rtoffset);
			break;
		case T_BitmapHeapScan:
			((BitmapHeapScan *) plan)->scan.scanrelid += rtoffset;
			adjust_expr_varnos((Node *) plan->targetlist, rtoffset);
			adjust_expr_varnos((Node *) plan->qual, rtoffset);
			adjust_expr_varnos((Node *) ((BitmapHeapScan *) plan)->bitmapqualorig,
							   rtoffset);
			break;
		case T_TidScan:
			((TidScan *) plan)->scan.scanrelid += rtoffset;
			adjust_expr_varnos((Node *) plan->targetlist, rtoffset);
			adjust_expr_varnos((Node *) plan->qual, rtoffset);
			adjust_expr_varnos((Node *) ((TidScan *) plan)->tidquals,
							   rtoffset);
			break;
		case T_SubqueryScan:
			((SubqueryScan *) plan)->scan.scanrelid += rtoffset;
			adjust_expr_varnos((Node *) plan->targetlist, rtoffset);
			adjust_expr_varnos((Node *) plan->qual, rtoffset);
			/* we should not recurse into the subquery! */
			break;
		case T_FunctionScan:
			((FunctionScan *) plan)->scan.scanrelid += rtoffset;
			adjust_expr_varnos((Node *) plan->targetlist, rtoffset);
			adjust_expr_varnos((Node *) plan->qual, rtoffset);
			/* rte was already fixed by set_subqueryscan_references */
			break;
		case T_ValuesScan:
			((ValuesScan *) plan)->scan.scanrelid += rtoffset;
			adjust_expr_varnos((Node *) plan->targetlist, rtoffset);
			adjust_expr_varnos((Node *) plan->qual, rtoffset);
			/* rte was already fixed by set_subqueryscan_references */
			break;
		case T_NestLoop:
			adjust_expr_varnos((Node *) plan->targetlist, rtoffset);
			adjust_expr_varnos((Node *) plan->qual, rtoffset);
			adjust_expr_varnos((Node *) ((Join *) plan)->joinqual, rtoffset);
			break;
		case T_MergeJoin:
			adjust_expr_varnos((Node *) plan->targetlist, rtoffset);
			adjust_expr_varnos((Node *) plan->qual, rtoffset);
			adjust_expr_varnos((Node *) ((Join *) plan)->joinqual, rtoffset);
			adjust_expr_varnos((Node *) ((MergeJoin *) plan)->mergeclauses,
							   rtoffset);
			break;
		case T_HashJoin:
			adjust_expr_varnos((Node *) plan->targetlist, rtoffset);
			adjust_expr_varnos((Node *) plan->qual, rtoffset);
			adjust_expr_varnos((Node *) ((Join *) plan)->joinqual, rtoffset);
			adjust_expr_varnos((Node *) ((HashJoin *) plan)->hashclauses,
							   rtoffset);
			break;
		case T_Hash:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_SetOp:

			/*
			 * Even though the targetlist won't be used by the executor, we
			 * fix it up for possible use by EXPLAIN (not to mention ease of
			 * debugging --- wrong varnos are very confusing).
			 */
			adjust_expr_varnos((Node *) plan->targetlist, rtoffset);
			Assert(plan->qual == NIL);
			break;
		case T_Limit:

			/*
			 * Like the plan types above, Limit doesn't evaluate its tlist or
			 * quals.  It does have live expressions for limit/offset,
			 * however.
			 */
			adjust_expr_varnos((Node *) plan->targetlist, rtoffset);
			Assert(plan->qual == NIL);
			adjust_expr_varnos(((Limit *) plan)->limitOffset, rtoffset);
			adjust_expr_varnos(((Limit *) plan)->limitCount, rtoffset);
			break;
		case T_Agg:
		case T_Group:
			adjust_expr_varnos((Node *) plan->targetlist, rtoffset);
			adjust_expr_varnos((Node *) plan->qual, rtoffset);
			break;
		case T_Result:
			adjust_expr_varnos((Node *) plan->targetlist, rtoffset);
			adjust_expr_varnos((Node *) plan->qual, rtoffset);
			adjust_expr_varnos(((Result *) plan)->resconstantqual, rtoffset);
			break;
		case T_Append:
			adjust_expr_varnos((Node *) plan->targetlist, rtoffset);
			Assert(plan->qual == NIL);
			foreach(l, ((Append *) plan)->appendplans)
				adjust_plan_varnos((Plan *) lfirst(l), rtoffset);
			break;
		case T_BitmapAnd:
			/* BitmapAnd works like Append, but has no tlist */
			Assert(plan->targetlist == NIL);
			Assert(plan->qual == NIL);
			foreach(l, ((BitmapAnd *) plan)->bitmapplans)
				adjust_plan_varnos((Plan *) lfirst(l), rtoffset);
			break;
		case T_BitmapOr:
			/* BitmapOr works like Append, but has no tlist */
			Assert(plan->targetlist == NIL);
			Assert(plan->qual == NIL);
			foreach(l, ((BitmapOr *) plan)->bitmapplans)
				adjust_plan_varnos((Plan *) lfirst(l), rtoffset);
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(plan));
			break;
	}

	/*
	 * Now recurse into child plans.
	 *
	 * We don't need to (and in fact mustn't) recurse into subqueries, so no
	 * need to examine initPlan list.
	 */
	adjust_plan_varnos(plan->lefttree, rtoffset);
	adjust_plan_varnos(plan->righttree, rtoffset);
}

/*
 * adjust_expr_varnos
 *		Offset varnos of Vars in an expression by rtoffset.
 *
 * This is different from the rewriter's OffsetVarNodes in that it has to
 * work on an already-planned expression tree; in particular, we should not
 * disturb INNER and OUTER references.	On the other hand, we don't have to
 * recurse into subqueries nor deal with outer-level Vars, so it's pretty
 * simple.
 */
static void
adjust_expr_varnos(Node *node, int rtoffset)
{
	/* This tree walk requires no special setup, so away we go... */
	adjust_expr_varnos_walker(node, &rtoffset);
}

static bool
adjust_expr_varnos_walker(Node *node, int *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		Assert(var->varlevelsup == 0);
		if (var->varno > 0 && var->varno != INNER && var->varno != OUTER)
			var->varno += *context;
		if (var->varnoold > 0)
			var->varnoold += *context;
		return false;
	}
	return expression_tree_walker(node, adjust_expr_varnos_walker,
								  (void *) context);
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

		sp->plan = set_plan_references(sp->plan, sp->rtable);
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
 * quals of the child indexscan.  set_inner_join_references does that.
 *
 *	'join' is a join plan node
 */
static void
set_join_references(Join *join)
{
	Plan	   *outer_plan = join->plan.lefttree;
	Plan	   *inner_plan = join->plan.righttree;
	indexed_tlist *outer_itlist;
	indexed_tlist *inner_itlist;

	outer_itlist = build_tlist_index(outer_plan->targetlist);
	inner_itlist = build_tlist_index(inner_plan->targetlist);

	/* All join plans have tlist, qual, and joinqual */
	join->plan.targetlist = join_references(join->plan.targetlist,
											outer_itlist,
											inner_itlist,
											(Index) 0);
	join->plan.qual = join_references(join->plan.qual,
									  outer_itlist,
									  inner_itlist,
									  (Index) 0);
	join->joinqual = join_references(join->joinqual,
									 outer_itlist,
									 inner_itlist,
									 (Index) 0);

	/* Now do join-type-specific stuff */
	if (IsA(join, NestLoop))
	{
		/* This processing is split out to handle possible recursion */
		set_inner_join_references(inner_plan,
								  outer_itlist);
	}
	else if (IsA(join, MergeJoin))
	{
		MergeJoin  *mj = (MergeJoin *) join;

		mj->mergeclauses = join_references(mj->mergeclauses,
										   outer_itlist,
										   inner_itlist,
										   (Index) 0);
	}
	else if (IsA(join, HashJoin))
	{
		HashJoin   *hj = (HashJoin *) join;

		hj->hashclauses = join_references(hj->hashclauses,
										  outer_itlist,
										  inner_itlist,
										  (Index) 0);
	}

	pfree(outer_itlist);
	pfree(inner_itlist);
}

/*
 * set_inner_join_references
 *		Handle join references appearing in an inner indexscan's quals
 *
 * To handle bitmap-scan plan trees, we have to be able to recurse down
 * to the bottom BitmapIndexScan nodes; likewise, appendrel indexscans
 * require recursing through Append nodes.	This is split out as a separate
 * function so that it can recurse.
 */
static void
set_inner_join_references(Plan *inner_plan, indexed_tlist *outer_itlist)
{
	if (IsA(inner_plan, IndexScan))
	{
		/*
		 * An index is being used to reduce the number of tuples scanned in
		 * the inner relation.	If there are join clauses being used with the
		 * index, we must update their outer-rel var nodes to refer to the
		 * outer side of the join.
		 */
		IndexScan  *innerscan = (IndexScan *) inner_plan;
		List	   *indexqualorig = innerscan->indexqualorig;

		/* No work needed if indexqual refers only to its own rel... */
		if (NumRelids((Node *) indexqualorig) > 1)
		{
			Index		innerrel = innerscan->scan.scanrelid;

			/* only refs to outer vars get changed in the inner qual */
			innerscan->indexqualorig = join_references(indexqualorig,
													   outer_itlist,
													   NULL,
													   innerrel);
			innerscan->indexqual = join_references(innerscan->indexqual,
												   outer_itlist,
												   NULL,
												   innerrel);

			/*
			 * We must fix the inner qpqual too, if it has join clauses (this
			 * could happen if special operators are involved: some indexquals
			 * may get rechecked as qpquals).
			 */
			if (NumRelids((Node *) inner_plan->qual) > 1)
				inner_plan->qual = join_references(inner_plan->qual,
												   outer_itlist,
												   NULL,
												   innerrel);
		}
	}
	else if (IsA(inner_plan, BitmapIndexScan))
	{
		/*
		 * Same, but index is being used within a bitmap plan.
		 */
		BitmapIndexScan *innerscan = (BitmapIndexScan *) inner_plan;
		List	   *indexqualorig = innerscan->indexqualorig;

		/* No work needed if indexqual refers only to its own rel... */
		if (NumRelids((Node *) indexqualorig) > 1)
		{
			Index		innerrel = innerscan->scan.scanrelid;

			/* only refs to outer vars get changed in the inner qual */
			innerscan->indexqualorig = join_references(indexqualorig,
													   outer_itlist,
													   NULL,
													   innerrel);
			innerscan->indexqual = join_references(innerscan->indexqual,
												   outer_itlist,
												   NULL,
												   innerrel);
			/* no need to fix inner qpqual */
			Assert(inner_plan->qual == NIL);
		}
	}
	else if (IsA(inner_plan, BitmapHeapScan))
	{
		/*
		 * The inner side is a bitmap scan plan.  Fix the top node, and
		 * recurse to get the lower nodes.
		 *
		 * Note: create_bitmap_scan_plan removes clauses from bitmapqualorig
		 * if they are duplicated in qpqual, so must test these independently.
		 */
		BitmapHeapScan *innerscan = (BitmapHeapScan *) inner_plan;
		Index		innerrel = innerscan->scan.scanrelid;
		List	   *bitmapqualorig = innerscan->bitmapqualorig;

		/* only refs to outer vars get changed in the inner qual */
		if (NumRelids((Node *) bitmapqualorig) > 1)
			innerscan->bitmapqualorig = join_references(bitmapqualorig,
														outer_itlist,
														NULL,
														innerrel);

		/*
		 * We must fix the inner qpqual too, if it has join clauses (this
		 * could happen if special operators are involved: some indexquals may
		 * get rechecked as qpquals).
		 */
		if (NumRelids((Node *) inner_plan->qual) > 1)
			inner_plan->qual = join_references(inner_plan->qual,
											   outer_itlist,
											   NULL,
											   innerrel);

		/* Now recurse */
		set_inner_join_references(inner_plan->lefttree,
								  outer_itlist);
	}
	else if (IsA(inner_plan, BitmapAnd))
	{
		/* All we need do here is recurse */
		BitmapAnd  *innerscan = (BitmapAnd *) inner_plan;
		ListCell   *l;

		foreach(l, innerscan->bitmapplans)
		{
			set_inner_join_references((Plan *) lfirst(l),
									  outer_itlist);
		}
	}
	else if (IsA(inner_plan, BitmapOr))
	{
		/* All we need do here is recurse */
		BitmapOr   *innerscan = (BitmapOr *) inner_plan;
		ListCell   *l;

		foreach(l, innerscan->bitmapplans)
		{
			set_inner_join_references((Plan *) lfirst(l),
									  outer_itlist);
		}
	}
	else if (IsA(inner_plan, Append))
	{
		/*
		 * The inner side is an append plan.  Recurse to see if it contains
		 * indexscans that need to be fixed.
		 */
		Append	   *appendplan = (Append *) inner_plan;
		ListCell   *l;

		foreach(l, appendplan->appendplans)
		{
			set_inner_join_references((Plan *) lfirst(l),
									  outer_itlist);
		}
	}
	else if (IsA(inner_plan, Result))
	{
		/* Recurse through a gating Result node (similar to Append case) */
		Result	   *result = (Result *) inner_plan;

		if (result->plan.lefttree)
			set_inner_join_references(result->plan.lefttree, outer_itlist);
	}
	else if (IsA(inner_plan, TidScan))
	{
		TidScan    *innerscan = (TidScan *) inner_plan;
		Index		innerrel = innerscan->scan.scanrelid;

		innerscan->tidquals = join_references(innerscan->tidquals,
											  outer_itlist,
											  NULL,
											  innerrel);
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
	indexed_tlist *subplan_itlist;
	List	   *output_targetlist;
	ListCell   *l;

	if (subplan != NULL)
		subplan_itlist = build_tlist_index(subplan->targetlist);
	else
		subplan_itlist = build_tlist_index(NIL);

	output_targetlist = NIL;
	foreach(l, plan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		Node	   *newexpr;

		newexpr = replace_vars_with_subplan_refs((Node *) tle->expr,
												 subplan_itlist,
												 subvarno);
		tle = flatCopyTargetEntry(tle);
		tle->expr = (Expr *) newexpr;
		output_targetlist = lappend(output_targetlist, tle);
	}
	plan->targetlist = output_targetlist;

	plan->qual = (List *)
		replace_vars_with_subplan_refs((Node *) plan->qual,
									   subplan_itlist,
									   subvarno);

	pfree(subplan_itlist);
}

/*
 * build_tlist_index --- build an index data structure for a child tlist
 *
 * In most cases, subplan tlists will be "flat" tlists with only Vars,
 * so we try to optimize that case by extracting information about Vars
 * in advance.	Matching a parent tlist to a child is still an O(N^2)
 * operation, but at least with a much smaller constant factor than plain
 * tlist_member() searches.
 *
 * The result of this function is an indexed_tlist struct to pass to
 * search_indexed_tlist_for_var() or search_indexed_tlist_for_non_var().
 * When done, the indexed_tlist may be freed with a single pfree().
 */
static indexed_tlist *
build_tlist_index(List *tlist)
{
	indexed_tlist *itlist;
	tlist_vinfo *vinfo;
	ListCell   *l;

	/* Create data structure with enough slots for all tlist entries */
	itlist = (indexed_tlist *)
		palloc(offsetof(indexed_tlist, vars) +
			   list_length(tlist) * sizeof(tlist_vinfo));

	itlist->tlist = tlist;
	itlist->has_non_vars = false;

	/* Find the Vars and fill in the index array */
	vinfo = itlist->vars;
	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->expr && IsA(tle->expr, Var))
		{
			Var		   *var = (Var *) tle->expr;

			vinfo->varno = var->varno;
			vinfo->varattno = var->varattno;
			vinfo->resno = tle->resno;
			vinfo++;
		}
		else
			itlist->has_non_vars = true;
	}

	itlist->num_vars = (vinfo - itlist->vars);

	return itlist;
}

/*
 * build_tlist_index_other_vars --- build a restricted tlist index
 *
 * This is like build_tlist_index, but we only index tlist entries that
 * are Vars and belong to some rel other than the one specified.
 */
static indexed_tlist *
build_tlist_index_other_vars(List *tlist, Index ignore_rel)
{
	indexed_tlist *itlist;
	tlist_vinfo *vinfo;
	ListCell   *l;

	/* Create data structure with enough slots for all tlist entries */
	itlist = (indexed_tlist *)
		palloc(offsetof(indexed_tlist, vars) +
			   list_length(tlist) * sizeof(tlist_vinfo));

	itlist->tlist = tlist;
	itlist->has_non_vars = false;

	/* Find the desired Vars and fill in the index array */
	vinfo = itlist->vars;
	foreach(l, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->expr && IsA(tle->expr, Var))
		{
			Var		   *var = (Var *) tle->expr;

			if (var->varno != ignore_rel)
			{
				vinfo->varno = var->varno;
				vinfo->varattno = var->varattno;
				vinfo->resno = tle->resno;
				vinfo++;
			}
		}
	}

	itlist->num_vars = (vinfo - itlist->vars);

	return itlist;
}

/*
 * search_indexed_tlist_for_var --- find a Var in an indexed tlist
 *
 * If a match is found, return a copy of the given Var with suitably
 * modified varno/varattno (to wit, newvarno and the resno of the TLE entry).
 * If no match, return NULL.
 */
static Var *
search_indexed_tlist_for_var(Var *var, indexed_tlist *itlist, Index newvarno)
{
	Index		varno = var->varno;
	AttrNumber	varattno = var->varattno;
	tlist_vinfo *vinfo;
	int			i;

	vinfo = itlist->vars;
	i = itlist->num_vars;
	while (i-- > 0)
	{
		if (vinfo->varno == varno && vinfo->varattno == varattno)
		{
			/* Found a match */
			Var		   *newvar = (Var *) copyObject(var);

			newvar->varno = newvarno;
			newvar->varattno = vinfo->resno;
			return newvar;
		}
		vinfo++;
	}
	return NULL;				/* no match */
}

/*
 * search_indexed_tlist_for_non_var --- find a non-Var in an indexed tlist
 *
 * If a match is found, return a Var constructed to reference the tlist item.
 * If no match, return NULL.
 *
 * NOTE: it is a waste of time to call this if !itlist->has_non_vars
 */
static Var *
search_indexed_tlist_for_non_var(Node *node,
								 indexed_tlist *itlist, Index newvarno)
{
	TargetEntry *tle;

	tle = tlist_member(node, itlist->tlist);
	if (tle)
	{
		/* Found a matching subplan output expression */
		Var		   *newvar;

		newvar = makeVar(newvarno,
						 tle->resno,
						 exprType((Node *) tle->expr),
						 exprTypmod((Node *) tle->expr),
						 0);
		newvar->varnoold = 0;	/* wasn't ever a plain Var */
		newvar->varoattno = 0;
		return newvar;
	}
	return NULL;				/* no match */
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
 * references, but not touch the Vars of the inner relation.  (We also
 * implement RETURNING clause fixup using this second scenario.)
 *
 * For a normal join, acceptable_rel should be zero so that any failure to
 * match a Var will be reported as an error.  For the indexscan case,
 * pass inner_itlist = NULL and acceptable_rel = the ID of the inner relation.
 *
 * 'clauses' is the targetlist or list of join clauses
 * 'outer_itlist' is the indexed target list of the outer join relation
 * 'inner_itlist' is the indexed target list of the inner join relation,
 *		or NULL
 * 'acceptable_rel' is either zero or the rangetable index of a relation
 *		whose Vars may appear in the clause without provoking an error.
 *
 * Returns the new expression tree.  The original clause structure is
 * not modified.
 */
static List *
join_references(List *clauses,
				indexed_tlist *outer_itlist,
				indexed_tlist *inner_itlist,
				Index acceptable_rel)
{
	join_references_context context;

	context.outer_itlist = outer_itlist;
	context.inner_itlist = inner_itlist;
	context.acceptable_rel = acceptable_rel;
	return (List *) join_references_mutator((Node *) clauses, &context);
}

static Node *
join_references_mutator(Node *node,
						join_references_context *context)
{
	Var		   *newvar;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/* First look for the var in the input tlists */
		newvar = search_indexed_tlist_for_var(var,
											  context->outer_itlist,
											  OUTER);
		if (newvar)
			return (Node *) newvar;
		if (context->inner_itlist)
		{
			newvar = search_indexed_tlist_for_var(var,
												  context->inner_itlist,
												  INNER);
			if (newvar)
				return (Node *) newvar;
		}

		/* Return the Var unmodified, if it's for acceptable_rel */
		if (var->varno == context->acceptable_rel)
			return (Node *) copyObject(var);

		/* No referent found for Var */
		elog(ERROR, "variable not found in subplan target lists");
	}
	/* Try matching more complex expressions too, if tlists have any */
	if (context->outer_itlist->has_non_vars)
	{
		newvar = search_indexed_tlist_for_non_var(node,
												  context->outer_itlist,
												  OUTER);
		if (newvar)
			return (Node *) newvar;
	}
	if (context->inner_itlist && context->inner_itlist->has_non_vars)
	{
		newvar = search_indexed_tlist_for_non_var(node,
												  context->inner_itlist,
												  INNER);
		if (newvar)
			return (Node *) newvar;
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
 * If itlist->has_non_vars is true, then we try to match whole subexpressions
 * against elements of the subplan tlist, so that we can avoid recomputing
 * expressions that were already computed by the subplan.  (This is relatively
 * expensive, so we don't want to try it in the common case where the
 * subplan tlist is just a flattened list of Vars.)
 *
 * 'node': the tree to be fixed (a target item or qual)
 * 'subplan_itlist': indexed target list for subplan
 * 'subvarno': varno to be assigned to all Vars
 *
 * The resulting tree is a copy of the original in which all Var nodes have
 * varno = subvarno, varattno = resno of corresponding subplan target.
 * The original tree is not modified.
 */
static Node *
replace_vars_with_subplan_refs(Node *node,
							   indexed_tlist *subplan_itlist,
							   Index subvarno)
{
	replace_vars_with_subplan_refs_context context;

	context.subplan_itlist = subplan_itlist;
	context.subvarno = subvarno;
	return replace_vars_with_subplan_refs_mutator(node, &context);
}

static Node *
replace_vars_with_subplan_refs_mutator(Node *node,
							 replace_vars_with_subplan_refs_context *context)
{
	Var		   *newvar;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		newvar = search_indexed_tlist_for_var(var,
											  context->subplan_itlist,
											  context->subvarno);
		if (!newvar)
			elog(ERROR, "variable not found in subplan target list");
		return (Node *) newvar;
	}
	/* Try matching more complex expressions too, if tlist has any */
	if (context->subplan_itlist->has_non_vars)
	{
		newvar = search_indexed_tlist_for_non_var(node,
												  context->subplan_itlist,
												  context->subvarno);
		if (newvar)
			return (Node *) newvar;
	}
	return expression_tree_mutator(node,
								   replace_vars_with_subplan_refs_mutator,
								   (void *) context);
}

/*
 * set_returning_clause_references
 *		Perform setrefs.c's work on a RETURNING targetlist
 *
 * If the query involves more than just the result table, we have to
 * adjust any Vars that refer to other tables to reference junk tlist
 * entries in the top plan's targetlist.  Vars referencing the result
 * table should be left alone, however (the executor will evaluate them
 * using the actual heap tuple, after firing triggers if any).	In the
 * adjusted RETURNING list, result-table Vars will still have their
 * original varno, but Vars for other rels will have varno OUTER.
 *
 * We also must apply fix_expr_references to the list.
 *
 * 'rlist': the RETURNING targetlist to be fixed
 * 'topplan': the top Plan node for the query (not yet passed through
 *		set_plan_references)
 * 'resultRelation': RT index of the query's result relation
 */
List *
set_returning_clause_references(List *rlist,
								Plan *topplan,
								Index resultRelation)
{
	indexed_tlist *itlist;

	/*
	 * We can perform the desired Var fixup by abusing the join_references
	 * machinery that normally handles inner indexscan fixup.  We search the
	 * top plan's targetlist for Vars of non-result relations, and use
	 * join_references to convert RETURNING Vars into references to those
	 * tlist entries, while leaving result-rel Vars as-is.
	 */
	itlist = build_tlist_index_other_vars(topplan->targetlist, resultRelation);

	rlist = join_references(rlist,
							itlist,
							NULL,
							resultRelation);

	fix_expr_references(topplan, (Node *) rlist);

	pfree(itlist);

	return rlist;
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
