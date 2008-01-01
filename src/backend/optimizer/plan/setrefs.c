/*-------------------------------------------------------------------------
 *
 * setrefs.c
 *	  Post-processing of a completed plan tree: fix references to subplan
 *	  vars, compute regproc values for operators, etc
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/plan/setrefs.c,v 1.141 2008/01/01 19:45:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
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
	PlannerGlobal *glob;
	int			rtoffset;
} fix_scan_expr_context;

typedef struct
{
	PlannerGlobal *glob;
	indexed_tlist *outer_itlist;
	indexed_tlist *inner_itlist;
	Index		acceptable_rel;
	int			rtoffset;
} fix_join_expr_context;

typedef struct
{
	PlannerGlobal *glob;
	indexed_tlist *subplan_itlist;
	int			rtoffset;
} fix_upper_expr_context;

#define fix_scan_list(glob, lst, rtoffset) \
	((List *) fix_scan_expr(glob, (Node *) (lst), rtoffset))

static Plan *set_plan_refs(PlannerGlobal *glob, Plan *plan, int rtoffset);
static Plan *set_subqueryscan_references(PlannerGlobal *glob,
							SubqueryScan *plan,
							int rtoffset);
static bool trivial_subqueryscan(SubqueryScan *plan);
static Node *fix_scan_expr(PlannerGlobal *glob, Node *node, int rtoffset);
static Node *fix_scan_expr_mutator(Node *node, fix_scan_expr_context *context);
static bool fix_scan_expr_walker(Node *node, fix_scan_expr_context *context);
static void set_join_references(PlannerGlobal *glob, Join *join, int rtoffset);
static void set_inner_join_references(PlannerGlobal *glob, Plan *inner_plan,
						  indexed_tlist *outer_itlist);
static void set_upper_references(PlannerGlobal *glob, Plan *plan, int rtoffset);
static void set_dummy_tlist_references(Plan *plan, int rtoffset);
static indexed_tlist *build_tlist_index(List *tlist);
static Var *search_indexed_tlist_for_var(Var *var,
							 indexed_tlist *itlist,
							 Index newvarno,
							 int rtoffset);
static Var *search_indexed_tlist_for_non_var(Node *node,
								 indexed_tlist *itlist,
								 Index newvarno);
static List *fix_join_expr(PlannerGlobal *glob,
			  List *clauses,
			  indexed_tlist *outer_itlist,
			  indexed_tlist *inner_itlist,
			  Index acceptable_rel, int rtoffset);
static Node *fix_join_expr_mutator(Node *node,
					  fix_join_expr_context *context);
static Node *fix_upper_expr(PlannerGlobal *glob,
			   Node *node,
			   indexed_tlist *subplan_itlist,
			   int rtoffset);
static Node *fix_upper_expr_mutator(Node *node,
					   fix_upper_expr_context *context);
static bool fix_opfuncids_walker(Node *node, void *context);


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
 * for the convenience of the executor:
 *
 * 1. We flatten the various subquery rangetables into a single list, and
 * zero out RangeTblEntry fields that are not useful to the executor.
 *
 * 2. We adjust Vars in scan nodes to be consistent with the flat rangetable.
 *
 * 3. We adjust Vars in upper plan nodes to refer to the outputs of their
 * subplans.
 *
 * 4. We compute regproc OIDs for operators (ie, we look up the function
 * that implements each op).
 *
 * 5. We create a list of OIDs of relations that the plan depends on.
 * This will be used by plancache.c to drive invalidation of cached plans.
 * (Someday we might want to generalize this to include other types of
 * objects, but for now tracking relations seems to solve most problems.)
 *
 * We also perform one final optimization step, which is to delete
 * SubqueryScan plan nodes that aren't doing anything useful (ie, have
 * no qual and a no-op targetlist).  The reason for doing this last is that
 * it can't readily be done before set_plan_references, because it would
 * break set_upper_references: the Vars in the subquery's top tlist
 * wouldn't match up with the Vars in the outer plan tree.  The SubqueryScan
 * serves a necessary function as a buffer between outer query and subquery
 * variable numbering ... but after we've flattened the rangetable this is
 * no longer a problem, since then there's only one rtindex namespace.
 *
 * set_plan_references recursively traverses the whole plan tree.
 *
 * Inputs:
 *	glob: global data for planner run
 *	plan: the topmost node of the plan
 *	rtable: the rangetable for the current subquery
 *
 * The return value is normally the same Plan node passed in, but can be
 * different when the passed-in Plan is a SubqueryScan we decide isn't needed.
 *
 * The flattened rangetable entries are appended to glob->finalrtable, and
 * the list of relation OIDs is appended to glob->relationOids.
 *
 * Notice that we modify Plan nodes in-place, but use expression_tree_mutator
 * to process targetlist and qual expressions.	We can assume that the Plan
 * nodes were just built by the planner and are not multiply referenced, but
 * it's not so safe to assume that for expression tree nodes.
 */
Plan *
set_plan_references(PlannerGlobal *glob, Plan *plan, List *rtable)
{
	int			rtoffset = list_length(glob->finalrtable);
	ListCell   *lc;

	/*
	 * In the flat rangetable, we zero out substructure pointers that are not
	 * needed by the executor; this reduces the storage space and copying cost
	 * for cached plans.  We keep only the alias and eref Alias fields, which
	 * are needed by EXPLAIN.
	 */
	foreach(lc, rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
		RangeTblEntry *newrte;

		/* flat copy to duplicate all the scalar fields */
		newrte = (RangeTblEntry *) palloc(sizeof(RangeTblEntry));
		memcpy(newrte, rte, sizeof(RangeTblEntry));

		/* zap unneeded sub-structure */
		newrte->subquery = NULL;
		newrte->funcexpr = NULL;
		newrte->funccoltypes = NIL;
		newrte->funccoltypmods = NIL;
		newrte->values_lists = NIL;
		newrte->joinaliasvars = NIL;

		glob->finalrtable = lappend(glob->finalrtable, newrte);

		/*
		 * If it's a plain relation RTE, add the table to relationOids.
		 *
		 * We do this even though the RTE might be unreferenced in the plan
		 * tree; this would correspond to cases such as views that were
		 * expanded, child tables that were eliminated by constraint
		 * exclusion, etc.	Schema invalidation on such a rel must still force
		 * rebuilding of the plan.
		 *
		 * Note we don't bother to avoid duplicate list entries.  We could,
		 * but it would probably cost more cycles than it would save.
		 */
		if (newrte->rtekind == RTE_RELATION)
			glob->relationOids = lappend_oid(glob->relationOids,
											 newrte->relid);
	}

	/* Now fix the Plan tree */
	return set_plan_refs(glob, plan, rtoffset);
}

/*
 * set_plan_refs: recurse through the Plan nodes of a single subquery level
 */
static Plan *
set_plan_refs(PlannerGlobal *glob, Plan *plan, int rtoffset)
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
			{
				SeqScan    *splan = (SeqScan *) plan;

				splan->scanrelid += rtoffset;
				splan->plan.targetlist =
					fix_scan_list(glob, splan->plan.targetlist, rtoffset);
				splan->plan.qual =
					fix_scan_list(glob, splan->plan.qual, rtoffset);
			}
			break;
		case T_IndexScan:
			{
				IndexScan  *splan = (IndexScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(glob, splan->scan.plan.targetlist, rtoffset);
				splan->scan.plan.qual =
					fix_scan_list(glob, splan->scan.plan.qual, rtoffset);
				splan->indexqual =
					fix_scan_list(glob, splan->indexqual, rtoffset);
				splan->indexqualorig =
					fix_scan_list(glob, splan->indexqualorig, rtoffset);
			}
			break;
		case T_BitmapIndexScan:
			{
				BitmapIndexScan *splan = (BitmapIndexScan *) plan;

				splan->scan.scanrelid += rtoffset;
				/* no need to fix targetlist and qual */
				Assert(splan->scan.plan.targetlist == NIL);
				Assert(splan->scan.plan.qual == NIL);
				splan->indexqual =
					fix_scan_list(glob, splan->indexqual, rtoffset);
				splan->indexqualorig =
					fix_scan_list(glob, splan->indexqualorig, rtoffset);
			}
			break;
		case T_BitmapHeapScan:
			{
				BitmapHeapScan *splan = (BitmapHeapScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(glob, splan->scan.plan.targetlist, rtoffset);
				splan->scan.plan.qual =
					fix_scan_list(glob, splan->scan.plan.qual, rtoffset);
				splan->bitmapqualorig =
					fix_scan_list(glob, splan->bitmapqualorig, rtoffset);
			}
			break;
		case T_TidScan:
			{
				TidScan    *splan = (TidScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(glob, splan->scan.plan.targetlist, rtoffset);
				splan->scan.plan.qual =
					fix_scan_list(glob, splan->scan.plan.qual, rtoffset);
				splan->tidquals =
					fix_scan_list(glob, splan->tidquals, rtoffset);
			}
			break;
		case T_SubqueryScan:
			/* Needs special treatment, see comments below */
			return set_subqueryscan_references(glob,
											   (SubqueryScan *) plan,
											   rtoffset);
		case T_FunctionScan:
			{
				FunctionScan *splan = (FunctionScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(glob, splan->scan.plan.targetlist, rtoffset);
				splan->scan.plan.qual =
					fix_scan_list(glob, splan->scan.plan.qual, rtoffset);
				splan->funcexpr =
					fix_scan_expr(glob, splan->funcexpr, rtoffset);
			}
			break;
		case T_ValuesScan:
			{
				ValuesScan *splan = (ValuesScan *) plan;

				splan->scan.scanrelid += rtoffset;
				splan->scan.plan.targetlist =
					fix_scan_list(glob, splan->scan.plan.targetlist, rtoffset);
				splan->scan.plan.qual =
					fix_scan_list(glob, splan->scan.plan.qual, rtoffset);
				splan->values_lists =
					fix_scan_list(glob, splan->values_lists, rtoffset);
			}
			break;

		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin:
			set_join_references(glob, (Join *) plan, rtoffset);
			break;

		case T_Hash:
		case T_Material:
		case T_Sort:
		case T_Unique:
		case T_SetOp:

			/*
			 * These plan types don't actually bother to evaluate their
			 * targetlists, because they just return their unmodified input
			 * tuples.	Even though the targetlist won't be used by the
			 * executor, we fix it up for possible use by EXPLAIN (not to
			 * mention ease of debugging --- wrong varnos are very confusing).
			 */
			set_dummy_tlist_references(plan, rtoffset);

			/*
			 * Since these plan types don't check quals either, we should not
			 * find any qual expression attached to them.
			 */
			Assert(plan->qual == NIL);
			break;
		case T_Limit:
			{
				Limit	   *splan = (Limit *) plan;

				/*
				 * Like the plan types above, Limit doesn't evaluate its tlist
				 * or quals.  It does have live expressions for limit/offset,
				 * however; and those cannot contain subplan variable refs, so
				 * fix_scan_expr works for them.
				 */
				set_dummy_tlist_references(plan, rtoffset);
				Assert(splan->plan.qual == NIL);

				splan->limitOffset =
					fix_scan_expr(glob, splan->limitOffset, rtoffset);
				splan->limitCount =
					fix_scan_expr(glob, splan->limitCount, rtoffset);
			}
			break;
		case T_Agg:
		case T_Group:
			set_upper_references(glob, plan, rtoffset);
			break;
		case T_Result:
			{
				Result	   *splan = (Result *) plan;

				/*
				 * Result may or may not have a subplan; if not, it's more
				 * like a scan node than an upper node.
				 */
				if (splan->plan.lefttree != NULL)
					set_upper_references(glob, plan, rtoffset);
				else
				{
					splan->plan.targetlist =
						fix_scan_list(glob, splan->plan.targetlist, rtoffset);
					splan->plan.qual =
						fix_scan_list(glob, splan->plan.qual, rtoffset);
				}
				/* resconstantqual can't contain any subplan variable refs */
				splan->resconstantqual =
					fix_scan_expr(glob, splan->resconstantqual, rtoffset);
			}
			break;
		case T_Append:
			{
				Append	   *splan = (Append *) plan;

				/*
				 * Append, like Sort et al, doesn't actually evaluate its
				 * targetlist or check quals.
				 */
				set_dummy_tlist_references(plan, rtoffset);
				Assert(splan->plan.qual == NIL);
				foreach(l, splan->appendplans)
				{
					lfirst(l) = set_plan_refs(glob,
											  (Plan *) lfirst(l),
											  rtoffset);
				}
			}
			break;
		case T_BitmapAnd:
			{
				BitmapAnd  *splan = (BitmapAnd *) plan;

				/* BitmapAnd works like Append, but has no tlist */
				Assert(splan->plan.targetlist == NIL);
				Assert(splan->plan.qual == NIL);
				foreach(l, splan->bitmapplans)
				{
					lfirst(l) = set_plan_refs(glob,
											  (Plan *) lfirst(l),
											  rtoffset);
				}
			}
			break;
		case T_BitmapOr:
			{
				BitmapOr   *splan = (BitmapOr *) plan;

				/* BitmapOr works like Append, but has no tlist */
				Assert(splan->plan.targetlist == NIL);
				Assert(splan->plan.qual == NIL);
				foreach(l, splan->bitmapplans)
				{
					lfirst(l) = set_plan_refs(glob,
											  (Plan *) lfirst(l),
											  rtoffset);
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(plan));
			break;
	}

	/*
	 * Now recurse into child plans, if any
	 *
	 * NOTE: it is essential that we recurse into child plans AFTER we set
	 * subplan references in this plan's tlist and quals.  If we did the
	 * reference-adjustments bottom-up, then we would fail to match this
	 * plan's var nodes against the already-modified nodes of the children.
	 */
	plan->lefttree = set_plan_refs(glob, plan->lefttree, rtoffset);
	plan->righttree = set_plan_refs(glob, plan->righttree, rtoffset);

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
set_subqueryscan_references(PlannerGlobal *glob,
							SubqueryScan *plan,
							int rtoffset)
{
	Plan	   *result;

	/* First, recursively process the subplan */
	plan->subplan = set_plan_references(glob, plan->subplan, plan->subrtable);

	/* subrtable is no longer needed in the plan tree */
	plan->subrtable = NIL;

	if (trivial_subqueryscan(plan))
	{
		/*
		 * We can omit the SubqueryScan node and just pull up the subplan.
		 */
		ListCell   *lp,
				   *lc;

		result = plan->subplan;

		/* We have to be sure we don't lose any initplans */
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
		 * not do set_upper_references() here, because a SubqueryScan will
		 * always have been created with correct references to its subplan's
		 * outputs to begin with.
		 */
		plan->scan.scanrelid += rtoffset;
		plan->scan.plan.targetlist =
			fix_scan_list(glob, plan->scan.plan.targetlist, rtoffset);
		plan->scan.plan.qual =
			fix_scan_list(glob, plan->scan.plan.qual, rtoffset);

		result = (Plan *) plan;
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
 * copyVar
 *		Copy a Var node.
 *
 * fix_scan_expr and friends do this enough times that it's worth having
 * a bespoke routine instead of using the generic copyObject() function.
 */
static inline Var *
copyVar(Var *var)
{
	Var		   *newvar = (Var *) palloc(sizeof(Var));

	*newvar = *var;
	return newvar;
}

/*
 * fix_scan_expr
 *		Do set_plan_references processing on a scan-level expression
 *
 * This consists of incrementing all Vars' varnos by rtoffset,
 * looking up operator opcode info for OpExpr and related nodes,
 * and adding OIDs from regclass Const nodes into glob->relationOids.
 */
static Node *
fix_scan_expr(PlannerGlobal *glob, Node *node, int rtoffset)
{
	fix_scan_expr_context context;

	context.glob = glob;
	context.rtoffset = rtoffset;

	if (rtoffset != 0)
	{
		return fix_scan_expr_mutator(node, &context);
	}
	else
	{
		/*
		 * If rtoffset == 0, we don't need to change any Vars, which makes
		 * it OK to just scribble on the input node tree instead of copying
		 * (since the only change, filling in any unset opfuncid fields,
		 * is harmless).  This saves just enough cycles to be noticeable on
		 * trivial queries.
		 */
		(void) fix_scan_expr_walker(node, &context);
		return node;
	}
}

static Node *
fix_scan_expr_mutator(Node *node, fix_scan_expr_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = copyVar((Var *) node);

		Assert(var->varlevelsup == 0);

		/*
		 * We should not see any Vars marked INNER, but in a nestloop inner
		 * scan there could be OUTER Vars.	Leave them alone.
		 */
		Assert(var->varno != INNER);
		if (var->varno > 0 && var->varno != OUTER)
			var->varno += context->rtoffset;
		if (var->varnoold > 0)
			var->varnoold += context->rtoffset;
		return (Node *) var;
	}
	if (IsA(node, CurrentOfExpr))
	{
		CurrentOfExpr *cexpr = (CurrentOfExpr *) copyObject(node);

		Assert(cexpr->cvarno != INNER);
		Assert(cexpr->cvarno != OUTER);
		cexpr->cvarno += context->rtoffset;
		return (Node *) cexpr;
	}

	/*
	 * Since we update opcode info in-place, this part could possibly scribble
	 * on the planner's input data structures, but it's OK.
	 */
	if (IsA(node, OpExpr))
		set_opfuncid((OpExpr *) node);
	else if (IsA(node, DistinctExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, NullIfExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, ScalarArrayOpExpr))
		set_sa_opfuncid((ScalarArrayOpExpr *) node);
	else if (IsA(node, Const))
	{
		Const	   *con = (Const *) node;

		/* Check for regclass reference */
		if (con->consttype == REGCLASSOID && !con->constisnull)
			context->glob->relationOids =
				lappend_oid(context->glob->relationOids,
							DatumGetObjectId(con->constvalue));
		/* Fall through to let expression_tree_mutator copy it */
	}
	return expression_tree_mutator(node, fix_scan_expr_mutator,
								   (void *) context);
}

static bool
fix_scan_expr_walker(Node *node, fix_scan_expr_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, OpExpr))
		set_opfuncid((OpExpr *) node);
	else if (IsA(node, DistinctExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, NullIfExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, ScalarArrayOpExpr))
		set_sa_opfuncid((ScalarArrayOpExpr *) node);
	else if (IsA(node, Const))
	{
		Const	   *con = (Const *) node;

		/* Check for regclass reference */
		if (con->consttype == REGCLASSOID && !con->constisnull)
			context->glob->relationOids =
				lappend_oid(context->glob->relationOids,
							DatumGetObjectId(con->constvalue));
		return false;
	}
	return expression_tree_walker(node, fix_scan_expr_walker,
								  (void *) context);
}

/*
 * set_join_references
 *	  Modify the target list and quals of a join node to reference its
 *	  subplans, by setting the varnos to OUTER or INNER and setting attno
 *	  values to the result domain number of either the corresponding outer
 *	  or inner join tuple item.  Also perform opcode lookup for these
 *	  expressions. and add regclass OIDs to glob->relationOids.
 *
 * In the case of a nestloop with inner indexscan, we will also need to
 * apply the same transformation to any outer vars appearing in the
 * quals of the child indexscan.  set_inner_join_references does that.
 */
static void
set_join_references(PlannerGlobal *glob, Join *join, int rtoffset)
{
	Plan	   *outer_plan = join->plan.lefttree;
	Plan	   *inner_plan = join->plan.righttree;
	indexed_tlist *outer_itlist;
	indexed_tlist *inner_itlist;

	outer_itlist = build_tlist_index(outer_plan->targetlist);
	inner_itlist = build_tlist_index(inner_plan->targetlist);

	/* All join plans have tlist, qual, and joinqual */
	join->plan.targetlist = fix_join_expr(glob,
										  join->plan.targetlist,
										  outer_itlist,
										  inner_itlist,
										  (Index) 0,
										  rtoffset);
	join->plan.qual = fix_join_expr(glob,
									join->plan.qual,
									outer_itlist,
									inner_itlist,
									(Index) 0,
									rtoffset);
	join->joinqual = fix_join_expr(glob,
								   join->joinqual,
								   outer_itlist,
								   inner_itlist,
								   (Index) 0,
								   rtoffset);

	/* Now do join-type-specific stuff */
	if (IsA(join, NestLoop))
	{
		/* This processing is split out to handle possible recursion */
		set_inner_join_references(glob, inner_plan, outer_itlist);
	}
	else if (IsA(join, MergeJoin))
	{
		MergeJoin  *mj = (MergeJoin *) join;

		mj->mergeclauses = fix_join_expr(glob,
										 mj->mergeclauses,
										 outer_itlist,
										 inner_itlist,
										 (Index) 0,
										 rtoffset);
	}
	else if (IsA(join, HashJoin))
	{
		HashJoin   *hj = (HashJoin *) join;

		hj->hashclauses = fix_join_expr(glob,
										hj->hashclauses,
										outer_itlist,
										inner_itlist,
										(Index) 0,
										rtoffset);
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
 *
 * Note we do *not* apply any rtoffset for non-join Vars; this is because
 * the quals will be processed again by fix_scan_expr when the set_plan_refs
 * recursion reaches the inner indexscan, and so we'd have done it twice.
 */
static void
set_inner_join_references(PlannerGlobal *glob, Plan *inner_plan,
						  indexed_tlist *outer_itlist)
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
			innerscan->indexqualorig = fix_join_expr(glob,
													 indexqualorig,
													 outer_itlist,
													 NULL,
													 innerrel,
													 0);
			innerscan->indexqual = fix_join_expr(glob,
												 innerscan->indexqual,
												 outer_itlist,
												 NULL,
												 innerrel,
												 0);

			/*
			 * We must fix the inner qpqual too, if it has join clauses (this
			 * could happen if special operators are involved: some indexquals
			 * may get rechecked as qpquals).
			 */
			if (NumRelids((Node *) inner_plan->qual) > 1)
				inner_plan->qual = fix_join_expr(glob,
												 inner_plan->qual,
												 outer_itlist,
												 NULL,
												 innerrel,
												 0);
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
			innerscan->indexqualorig = fix_join_expr(glob,
													 indexqualorig,
													 outer_itlist,
													 NULL,
													 innerrel,
													 0);
			innerscan->indexqual = fix_join_expr(glob,
												 innerscan->indexqual,
												 outer_itlist,
												 NULL,
												 innerrel,
												 0);
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
			innerscan->bitmapqualorig = fix_join_expr(glob,
													  bitmapqualorig,
													  outer_itlist,
													  NULL,
													  innerrel,
													  0);

		/*
		 * We must fix the inner qpqual too, if it has join clauses (this
		 * could happen if special operators are involved: some indexquals may
		 * get rechecked as qpquals).
		 */
		if (NumRelids((Node *) inner_plan->qual) > 1)
			inner_plan->qual = fix_join_expr(glob,
											 inner_plan->qual,
											 outer_itlist,
											 NULL,
											 innerrel,
											 0);

		/* Now recurse */
		set_inner_join_references(glob, inner_plan->lefttree, outer_itlist);
	}
	else if (IsA(inner_plan, BitmapAnd))
	{
		/* All we need do here is recurse */
		BitmapAnd  *innerscan = (BitmapAnd *) inner_plan;
		ListCell   *l;

		foreach(l, innerscan->bitmapplans)
		{
			set_inner_join_references(glob, (Plan *) lfirst(l), outer_itlist);
		}
	}
	else if (IsA(inner_plan, BitmapOr))
	{
		/* All we need do here is recurse */
		BitmapOr   *innerscan = (BitmapOr *) inner_plan;
		ListCell   *l;

		foreach(l, innerscan->bitmapplans)
		{
			set_inner_join_references(glob, (Plan *) lfirst(l), outer_itlist);
		}
	}
	else if (IsA(inner_plan, TidScan))
	{
		TidScan    *innerscan = (TidScan *) inner_plan;
		Index		innerrel = innerscan->scan.scanrelid;

		innerscan->tidquals = fix_join_expr(glob,
											innerscan->tidquals,
											outer_itlist,
											NULL,
											innerrel,
											0);
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
			set_inner_join_references(glob, (Plan *) lfirst(l), outer_itlist);
		}
	}
	else if (IsA(inner_plan, Result))
	{
		/* Recurse through a gating Result node (similar to Append case) */
		Result	   *result = (Result *) inner_plan;

		if (result->plan.lefttree)
			set_inner_join_references(glob, result->plan.lefttree, outer_itlist);
	}
}

/*
 * set_upper_references
 *	  Update the targetlist and quals of an upper-level plan node
 *	  to refer to the tuples returned by its lefttree subplan.
 *	  Also perform opcode lookup for these expressions, and
 *	  add regclass OIDs to glob->relationOids.
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
set_upper_references(PlannerGlobal *glob, Plan *plan, int rtoffset)
{
	Plan	   *subplan = plan->lefttree;
	indexed_tlist *subplan_itlist;
	List	   *output_targetlist;
	ListCell   *l;

	subplan_itlist = build_tlist_index(subplan->targetlist);

	output_targetlist = NIL;
	foreach(l, plan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		Node	   *newexpr;

		newexpr = fix_upper_expr(glob,
								 (Node *) tle->expr,
								 subplan_itlist,
								 rtoffset);
		tle = flatCopyTargetEntry(tle);
		tle->expr = (Expr *) newexpr;
		output_targetlist = lappend(output_targetlist, tle);
	}
	plan->targetlist = output_targetlist;

	plan->qual = (List *)
		fix_upper_expr(glob,
					   (Node *) plan->qual,
					   subplan_itlist,
					   rtoffset);

	pfree(subplan_itlist);
}

/*
 * set_dummy_tlist_references
 *	  Replace the targetlist of an upper-level plan node with a simple
 *	  list of OUTER references to its child.
 *
 * This is used for plan types like Sort and Append that don't evaluate
 * their targetlists.  Although the executor doesn't care at all what's in
 * the tlist, EXPLAIN needs it to be realistic.
 *
 * Note: we could almost use set_upper_references() here, but it fails for
 * Append for lack of a lefttree subplan.  Single-purpose code is faster
 * anyway.
 */
static void
set_dummy_tlist_references(Plan *plan, int rtoffset)
{
	List	   *output_targetlist;
	ListCell   *l;

	output_targetlist = NIL;
	foreach(l, plan->targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		Var		   *oldvar = (Var *) tle->expr;
		Var		   *newvar;

		newvar = makeVar(OUTER,
						 tle->resno,
						 exprType((Node *) oldvar),
						 exprTypmod((Node *) oldvar),
						 0);
		if (IsA(oldvar, Var))
		{
			newvar->varnoold = oldvar->varno + rtoffset;
			newvar->varoattno = oldvar->varattno;
		}
		else
		{
			newvar->varnoold = 0;		/* wasn't ever a plain Var */
			newvar->varoattno = 0;
		}

		tle = flatCopyTargetEntry(tle);
		tle->expr = (Expr *) newvar;
		output_targetlist = lappend(output_targetlist, tle);
	}
	plan->targetlist = output_targetlist;

	/* We don't touch plan->qual here */
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
 * Also ensure that varnoold is incremented by rtoffset.
 * If no match, return NULL.
 */
static Var *
search_indexed_tlist_for_var(Var *var, indexed_tlist *itlist,
							 Index newvarno, int rtoffset)
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
			Var		   *newvar = copyVar(var);

			newvar->varno = newvarno;
			newvar->varattno = vinfo->resno;
			if (newvar->varnoold > 0)
				newvar->varnoold += rtoffset;
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
 * fix_join_expr
 *	   Create a new set of targetlist entries or join qual clauses by
 *	   changing the varno/varattno values of variables in the clauses
 *	   to reference target list values from the outer and inner join
 *	   relation target lists.  Also perform opcode lookup and add
 *	   regclass OIDs to glob->relationOids.
 *
 * This is used in two different scenarios: a normal join clause, where
 * all the Vars in the clause *must* be replaced by OUTER or INNER references;
 * and an indexscan being used on the inner side of a nestloop join.
 * In the latter case we want to replace the outer-relation Vars by OUTER
 * references, while Vars of the inner relation should be adjusted by rtoffset.
 * (We also implement RETURNING clause fixup using this second scenario.)
 *
 * For a normal join, acceptable_rel should be zero so that any failure to
 * match a Var will be reported as an error.  For the indexscan case,
 * pass inner_itlist = NULL and acceptable_rel = the (not-offseted-yet) ID
 * of the inner relation.
 *
 * 'clauses' is the targetlist or list of join clauses
 * 'outer_itlist' is the indexed target list of the outer join relation
 * 'inner_itlist' is the indexed target list of the inner join relation,
 *		or NULL
 * 'acceptable_rel' is either zero or the rangetable index of a relation
 *		whose Vars may appear in the clause without provoking an error.
 * 'rtoffset' is what to add to varno for Vars of acceptable_rel.
 *
 * Returns the new expression tree.  The original clause structure is
 * not modified.
 */
static List *
fix_join_expr(PlannerGlobal *glob,
			  List *clauses,
			  indexed_tlist *outer_itlist,
			  indexed_tlist *inner_itlist,
			  Index acceptable_rel,
			  int rtoffset)
{
	fix_join_expr_context context;

	context.glob = glob;
	context.outer_itlist = outer_itlist;
	context.inner_itlist = inner_itlist;
	context.acceptable_rel = acceptable_rel;
	context.rtoffset = rtoffset;
	return (List *) fix_join_expr_mutator((Node *) clauses, &context);
}

static Node *
fix_join_expr_mutator(Node *node, fix_join_expr_context *context)
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
											  OUTER,
											  context->rtoffset);
		if (newvar)
			return (Node *) newvar;
		if (context->inner_itlist)
		{
			newvar = search_indexed_tlist_for_var(var,
												  context->inner_itlist,
												  INNER,
												  context->rtoffset);
			if (newvar)
				return (Node *) newvar;
		}

		/* If it's for acceptable_rel, adjust and return it */
		if (var->varno == context->acceptable_rel)
		{
			var = copyVar(var);
			var->varno += context->rtoffset;
			var->varnoold += context->rtoffset;
			return (Node *) var;
		}

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

	/*
	 * Since we update opcode info in-place, this part could possibly scribble
	 * on the planner's input data structures, but it's OK.
	 */
	if (IsA(node, OpExpr))
		set_opfuncid((OpExpr *) node);
	else if (IsA(node, DistinctExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, NullIfExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, ScalarArrayOpExpr))
		set_sa_opfuncid((ScalarArrayOpExpr *) node);
	else if (IsA(node, Const))
	{
		Const	   *con = (Const *) node;

		/* Check for regclass reference */
		if (con->consttype == REGCLASSOID && !con->constisnull)
			context->glob->relationOids =
				lappend_oid(context->glob->relationOids,
							DatumGetObjectId(con->constvalue));
		/* Fall through to let expression_tree_mutator copy it */
	}
	return expression_tree_mutator(node,
								   fix_join_expr_mutator,
								   (void *) context);
}

/*
 * fix_upper_expr
 *		Modifies an expression tree so that all Var nodes reference outputs
 *		of a subplan.  Also performs opcode lookup, and adds regclass OIDs to
 *		glob->relationOids.
 *
 * This is used to fix up target and qual expressions of non-join upper-level
 * plan nodes.
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
 * 'rtoffset': how much to increment varnoold by
 *
 * The resulting tree is a copy of the original in which all Var nodes have
 * varno = OUTER, varattno = resno of corresponding subplan target.
 * The original tree is not modified.
 */
static Node *
fix_upper_expr(PlannerGlobal *glob,
			   Node *node,
			   indexed_tlist *subplan_itlist,
			   int rtoffset)
{
	fix_upper_expr_context context;

	context.glob = glob;
	context.subplan_itlist = subplan_itlist;
	context.rtoffset = rtoffset;
	return fix_upper_expr_mutator(node, &context);
}

static Node *
fix_upper_expr_mutator(Node *node, fix_upper_expr_context *context)
{
	Var		   *newvar;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		newvar = search_indexed_tlist_for_var(var,
											  context->subplan_itlist,
											  OUTER,
											  context->rtoffset);
		if (!newvar)
			elog(ERROR, "variable not found in subplan target list");
		return (Node *) newvar;
	}
	/* Try matching more complex expressions too, if tlist has any */
	if (context->subplan_itlist->has_non_vars)
	{
		newvar = search_indexed_tlist_for_non_var(node,
												  context->subplan_itlist,
												  OUTER);
		if (newvar)
			return (Node *) newvar;
	}

	/*
	 * Since we update opcode info in-place, this part could possibly scribble
	 * on the planner's input data structures, but it's OK.
	 */
	if (IsA(node, OpExpr))
		set_opfuncid((OpExpr *) node);
	else if (IsA(node, DistinctExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, NullIfExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, ScalarArrayOpExpr))
		set_sa_opfuncid((ScalarArrayOpExpr *) node);
	else if (IsA(node, Const))
	{
		Const	   *con = (Const *) node;

		/* Check for regclass reference */
		if (con->consttype == REGCLASSOID && !con->constisnull)
			context->glob->relationOids =
				lappend_oid(context->glob->relationOids,
							DatumGetObjectId(con->constvalue));
		/* Fall through to let expression_tree_mutator copy it */
	}
	return expression_tree_mutator(node,
								   fix_upper_expr_mutator,
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
 * We also must perform opcode lookup and add regclass OIDs to
 * glob->relationOids.
 *
 * 'rlist': the RETURNING targetlist to be fixed
 * 'topplan': the top Plan node for the query (not yet passed through
 *		set_plan_references)
 * 'resultRelation': RT index of the associated result relation
 *
 * Note: we assume that result relations will have rtoffset zero, that is,
 * they are not coming from a subplan.
 */
List *
set_returning_clause_references(PlannerGlobal *glob,
								List *rlist,
								Plan *topplan,
								Index resultRelation)
{
	indexed_tlist *itlist;

	/*
	 * We can perform the desired Var fixup by abusing the fix_join_expr
	 * machinery that normally handles inner indexscan fixup.  We search the
	 * top plan's targetlist for Vars of non-result relations, and use
	 * fix_join_expr to convert RETURNING Vars into references to those tlist
	 * entries, while leaving result-rel Vars as-is.
	 */
	itlist = build_tlist_index_other_vars(topplan->targetlist, resultRelation);

	rlist = fix_join_expr(glob,
						  rlist,
						  itlist,
						  NULL,
						  resultRelation,
						  0);

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
	else if (IsA(node, NullIfExpr))
		set_opfuncid((OpExpr *) node);	/* rely on struct equivalence */
	else if (IsA(node, ScalarArrayOpExpr))
		set_sa_opfuncid((ScalarArrayOpExpr *) node);
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
void
set_sa_opfuncid(ScalarArrayOpExpr *opexpr)
{
	if (opexpr->opfuncid == InvalidOid)
		opexpr->opfuncid = get_opcode(opexpr->opno);
}
