/*-------------------------------------------------------------------------
 *
 * placeholder.c
 *	  PlaceHolderVar and PlaceHolderInfo manipulation routines
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/placeholder.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/placeholder.h"
#include "optimizer/planmain.h"
#include "utils/lsyscache.h"


typedef struct contain_placeholder_references_context
{
	int			relid;
	int			sublevels_up;
} contain_placeholder_references_context;

/* Local functions */
static void find_placeholders_recurse(PlannerInfo *root, Node *jtnode);
static void find_placeholders_in_expr(PlannerInfo *root, Node *expr);
static bool contain_placeholder_references_walker(Node *node,
												  contain_placeholder_references_context *context);


/*
 * make_placeholder_expr
 *		Make a PlaceHolderVar for the given expression.
 *
 * phrels is the syntactic location (as a set of relids) to attribute
 * to the expression.
 *
 * The caller is responsible for adjusting phlevelsup and phnullingrels
 * as needed.  Because we do not know here which query level the PHV
 * will be associated with, it's important that this function touches
 * only root->glob; messing with other parts of PlannerInfo would be
 * likely to do the wrong thing.
 */
PlaceHolderVar *
make_placeholder_expr(PlannerInfo *root, Expr *expr, Relids phrels)
{
	PlaceHolderVar *phv = makeNode(PlaceHolderVar);

	phv->phexpr = expr;
	phv->phrels = phrels;
	phv->phnullingrels = NULL;	/* caller may change this later */
	phv->phid = ++(root->glob->lastPHId);
	phv->phlevelsup = 0;		/* caller may change this later */

	return phv;
}

/*
 * find_placeholder_info
 *		Fetch the PlaceHolderInfo for the given PHV
 *
 * If the PlaceHolderInfo doesn't exist yet, create it if we haven't yet
 * frozen the set of PlaceHolderInfos for the query; else throw an error.
 *
 * This is separate from make_placeholder_expr because subquery pullup has
 * to make PlaceHolderVars for expressions that might not be used at all in
 * the upper query, or might not remain after const-expression simplification.
 * We build PlaceHolderInfos only for PHVs that are still present in the
 * simplified query passed to query_planner().
 *
 * Note: this should only be called after query_planner() has started.
 */
PlaceHolderInfo *
find_placeholder_info(PlannerInfo *root, PlaceHolderVar *phv)
{
	PlaceHolderInfo *phinfo;
	Relids		rels_used;

	/* if this ever isn't true, we'd need to be able to look in parent lists */
	Assert(phv->phlevelsup == 0);

	/* Use placeholder_array to look up existing PlaceHolderInfo quickly */
	if (phv->phid < root->placeholder_array_size)
		phinfo = root->placeholder_array[phv->phid];
	else
		phinfo = NULL;
	if (phinfo != NULL)
	{
		Assert(phinfo->phid == phv->phid);
		return phinfo;
	}

	/* Not found, so create it */
	if (root->placeholdersFrozen)
		elog(ERROR, "too late to create a new PlaceHolderInfo");

	phinfo = makeNode(PlaceHolderInfo);

	phinfo->phid = phv->phid;
	phinfo->ph_var = copyObject(phv);

	/*
	 * By convention, phinfo->ph_var->phnullingrels is always empty, since the
	 * PlaceHolderInfo represents the initially-calculated state of the
	 * PlaceHolderVar.  PlaceHolderVars appearing in the query tree might have
	 * varying values of phnullingrels, reflecting outer joins applied above
	 * the calculation level.
	 */
	phinfo->ph_var->phnullingrels = NULL;

	/*
	 * Any referenced rels that are outside the PHV's syntactic scope are
	 * LATERAL references, which should be included in ph_lateral but not in
	 * ph_eval_at.  If no referenced rels are within the syntactic scope,
	 * force evaluation at the syntactic location.
	 */
	rels_used = pull_varnos(root, (Node *) phv->phexpr);
	phinfo->ph_lateral = bms_difference(rels_used, phv->phrels);
	phinfo->ph_eval_at = bms_int_members(rels_used, phv->phrels);
	/* If no contained vars, force evaluation at syntactic location */
	if (bms_is_empty(phinfo->ph_eval_at))
	{
		phinfo->ph_eval_at = bms_copy(phv->phrels);
		Assert(!bms_is_empty(phinfo->ph_eval_at));
	}
	phinfo->ph_needed = NULL;	/* initially it's unused */
	/* for the moment, estimate width using just the datatype info */
	phinfo->ph_width = get_typavgwidth(exprType((Node *) phv->phexpr),
									   exprTypmod((Node *) phv->phexpr));

	/*
	 * Add to both placeholder_list and placeholder_array.  Note: because we
	 * store pointers to the PlaceHolderInfos in two data structures, it'd be
	 * unsafe to pass the whole placeholder_list structure through
	 * expression_tree_mutator or the like --- or at least, you'd have to
	 * rebuild the placeholder_array afterwards.
	 */
	root->placeholder_list = lappend(root->placeholder_list, phinfo);

	if (phinfo->phid >= root->placeholder_array_size)
	{
		/* Must allocate or enlarge placeholder_array */
		int			new_size;

		new_size = root->placeholder_array_size ? root->placeholder_array_size * 2 : 8;
		while (phinfo->phid >= new_size)
			new_size *= 2;
		if (root->placeholder_array)
			root->placeholder_array =
				repalloc0_array(root->placeholder_array, PlaceHolderInfo *, root->placeholder_array_size, new_size);
		else
			root->placeholder_array =
				palloc0_array(PlaceHolderInfo *, new_size);
		root->placeholder_array_size = new_size;
	}
	root->placeholder_array[phinfo->phid] = phinfo;

	/*
	 * The PHV's contained expression may contain other, lower-level PHVs.  We
	 * now know we need to get those into the PlaceHolderInfo list, too, so we
	 * may as well do that immediately.
	 */
	find_placeholders_in_expr(root, (Node *) phinfo->ph_var->phexpr);

	return phinfo;
}

/*
 * find_placeholders_in_jointree
 *		Search the jointree for PlaceHolderVars, and build PlaceHolderInfos
 *
 * We don't need to look at the targetlist because build_base_rel_tlists()
 * will already have made entries for any PHVs in the tlist.
 */
void
find_placeholders_in_jointree(PlannerInfo *root)
{
	/* This must be done before freezing the set of PHIs */
	Assert(!root->placeholdersFrozen);

	/* We need do nothing if the query contains no PlaceHolderVars */
	if (root->glob->lastPHId != 0)
	{
		/* Start recursion at top of jointree */
		Assert(root->parse->jointree != NULL &&
			   IsA(root->parse->jointree, FromExpr));
		find_placeholders_recurse(root, (Node *) root->parse->jointree);
	}
}

/*
 * find_placeholders_recurse
 *	  One recursion level of find_placeholders_in_jointree.
 *
 * jtnode is the current jointree node to examine.
 */
static void
find_placeholders_recurse(PlannerInfo *root, Node *jtnode)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		/* No quals to deal with here */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		/*
		 * First, recurse to handle child joins.
		 */
		foreach(l, f->fromlist)
		{
			find_placeholders_recurse(root, lfirst(l));
		}

		/*
		 * Now process the top-level quals.
		 */
		find_placeholders_in_expr(root, f->quals);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		/*
		 * First, recurse to handle child joins.
		 */
		find_placeholders_recurse(root, j->larg);
		find_placeholders_recurse(root, j->rarg);

		/* Process the qual clauses */
		find_placeholders_in_expr(root, j->quals);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * find_placeholders_in_expr
 *		Find all PlaceHolderVars in the given expression, and create
 *		PlaceHolderInfo entries for them.
 */
static void
find_placeholders_in_expr(PlannerInfo *root, Node *expr)
{
	List	   *vars;
	ListCell   *vl;

	/*
	 * pull_var_clause does more than we need here, but it'll do and it's
	 * convenient to use.
	 */
	vars = pull_var_clause(expr,
						   PVC_RECURSE_AGGREGATES |
						   PVC_RECURSE_WINDOWFUNCS |
						   PVC_INCLUDE_PLACEHOLDERS);
	foreach(vl, vars)
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) lfirst(vl);

		/* Ignore any plain Vars */
		if (!IsA(phv, PlaceHolderVar))
			continue;

		/* Create a PlaceHolderInfo entry if there's not one already */
		(void) find_placeholder_info(root, phv);
	}
	list_free(vars);
}

/*
 * fix_placeholder_input_needed_levels
 *		Adjust the "needed at" levels for placeholder inputs
 *
 * This is called after we've finished determining the eval_at levels for
 * all placeholders.  We need to make sure that all vars and placeholders
 * needed to evaluate each placeholder will be available at the scan or join
 * level where the evaluation will be done.  (It might seem that scan-level
 * evaluations aren't interesting, but that's not so: a LATERAL reference
 * within a placeholder's expression needs to cause the referenced var or
 * placeholder to be marked as needed in the scan where it's evaluated.)
 * Note that this loop can have side-effects on the ph_needed sets of other
 * PlaceHolderInfos; that's okay because we don't examine ph_needed here, so
 * there are no ordering issues to worry about.
 */
void
fix_placeholder_input_needed_levels(PlannerInfo *root)
{
	ListCell   *lc;

	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);
		List	   *vars = pull_var_clause((Node *) phinfo->ph_var->phexpr,
										   PVC_RECURSE_AGGREGATES |
										   PVC_RECURSE_WINDOWFUNCS |
										   PVC_INCLUDE_PLACEHOLDERS);

		add_vars_to_targetlist(root, vars, phinfo->ph_eval_at);
		list_free(vars);
	}
}

/*
 * rebuild_placeholder_attr_needed
 *	  Put back attr_needed bits for Vars/PHVs needed in PlaceHolderVars.
 *
 * This is used to rebuild attr_needed/ph_needed sets after removal of a
 * useless outer join.  It should match what
 * fix_placeholder_input_needed_levels did, except that we call
 * add_vars_to_attr_needed not add_vars_to_targetlist.
 */
void
rebuild_placeholder_attr_needed(PlannerInfo *root)
{
	ListCell   *lc;

	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);
		List	   *vars = pull_var_clause((Node *) phinfo->ph_var->phexpr,
										   PVC_RECURSE_AGGREGATES |
										   PVC_RECURSE_WINDOWFUNCS |
										   PVC_INCLUDE_PLACEHOLDERS);

		add_vars_to_attr_needed(root, vars, phinfo->ph_eval_at);
		list_free(vars);
	}
}

/*
 * add_placeholders_to_base_rels
 *		Add any required PlaceHolderVars to base rels' targetlists.
 *
 * If any placeholder can be computed at a base rel and is needed above it,
 * add it to that rel's targetlist.  This might look like it could be merged
 * with fix_placeholder_input_needed_levels, but it must be separate because
 * join removal happens in between, and can change the ph_eval_at sets.  There
 * is essentially the same logic in add_placeholders_to_joinrel, but we can't
 * do that part until joinrels are formed.
 */
void
add_placeholders_to_base_rels(PlannerInfo *root)
{
	ListCell   *lc;

	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);
		Relids		eval_at = phinfo->ph_eval_at;
		int			varno;

		if (bms_get_singleton_member(eval_at, &varno) &&
			bms_nonempty_difference(phinfo->ph_needed, eval_at))
		{
			RelOptInfo *rel = find_base_rel(root, varno);

			/*
			 * As in add_vars_to_targetlist(), a value computed at scan level
			 * has not yet been nulled by any outer join, so its phnullingrels
			 * should be empty.
			 */
			Assert(phinfo->ph_var->phnullingrels == NULL);

			/* Copying the PHV might be unnecessary here, but be safe */
			rel->reltarget->exprs = lappend(rel->reltarget->exprs,
											copyObject(phinfo->ph_var));
			/* reltarget's cost and width fields will be updated later */
		}
	}
}

/*
 * add_placeholders_to_joinrel
 *		Add any newly-computable PlaceHolderVars to a join rel's targetlist;
 *		and if computable PHVs contain lateral references, add those
 *		references to the joinrel's direct_lateral_relids.
 *
 * A join rel should emit a PlaceHolderVar if (a) the PHV can be computed
 * at or below this join level and (b) the PHV is needed above this level.
 * Our caller build_join_rel() has already added any PHVs that were computed
 * in either join input rel, so we need add only newly-computable ones to
 * the targetlist.  However, direct_lateral_relids must be updated for every
 * PHV computable at or below this join, as explained below.
 */
void
add_placeholders_to_joinrel(PlannerInfo *root, RelOptInfo *joinrel,
							RelOptInfo *outer_rel, RelOptInfo *inner_rel,
							SpecialJoinInfo *sjinfo)
{
	Relids		relids = joinrel->relids;
	int64		tuple_width = joinrel->reltarget->width;
	ListCell   *lc;

	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);

		/* Is it computable here? */
		if (bms_is_subset(phinfo->ph_eval_at, relids))
		{
			/* Is it still needed above this joinrel? */
			if (bms_nonempty_difference(phinfo->ph_needed, relids))
			{
				/*
				 * Yes, but only add to tlist if it wasn't computed in either
				 * input; otherwise it should be there already.  Also, we
				 * charge the cost of evaluating the contained expression if
				 * the PHV can be computed here but not in either input.  This
				 * is a bit bogus because we make the decision based on the
				 * first pair of possible input relations considered for the
				 * joinrel.  With other pairs, it might be possible to compute
				 * the PHV in one input or the other, and then we'd be double
				 * charging the PHV's cost for some join paths.  For now, live
				 * with that; but we might want to improve it later by
				 * refiguring the reltarget costs for each pair of inputs.
				 */
				if (!bms_is_subset(phinfo->ph_eval_at, outer_rel->relids) &&
					!bms_is_subset(phinfo->ph_eval_at, inner_rel->relids))
				{
					/* Copying might be unnecessary here, but be safe */
					PlaceHolderVar *phv = copyObject(phinfo->ph_var);
					QualCost	cost;

					/*
					 * It'll start out not nulled by anything.  Joins above
					 * this one might add to its phnullingrels later, in much
					 * the same way as for Vars.
					 */
					Assert(phv->phnullingrels == NULL);

					joinrel->reltarget->exprs = lappend(joinrel->reltarget->exprs,
														phv);
					cost_qual_eval_node(&cost, (Node *) phv->phexpr, root);
					joinrel->reltarget->cost.startup += cost.startup;
					joinrel->reltarget->cost.per_tuple += cost.per_tuple;
					tuple_width += phinfo->ph_width;
				}
			}

			/*
			 * Also adjust joinrel's direct_lateral_relids to include the
			 * PHV's source rel(s).  We must do this even if we're not
			 * actually going to emit the PHV, otherwise join_is_legal() will
			 * reject valid join orderings.  (In principle maybe we could
			 * instead remove the joinrel's lateral_relids dependency; but
			 * that's complicated to get right, and cases where we're not
			 * going to emit the PHV are too rare to justify the work.)
			 *
			 * In principle we should only do this if the join doesn't yet
			 * include the PHV's source rel(s).  But our caller
			 * build_join_rel() will clean things up by removing the join's
			 * own relids from its direct_lateral_relids, so we needn't
			 * account for that here.
			 */
			joinrel->direct_lateral_relids =
				bms_add_members(joinrel->direct_lateral_relids,
								phinfo->ph_lateral);
		}
	}

	joinrel->reltarget->width = clamp_width_est(tuple_width);
}

/*
 * contain_placeholder_references_to
 *		Detect whether any PlaceHolderVars in the given clause contain
 *		references to the given relid (typically an OJ relid).
 *
 * "Contain" means that there's a use of the relid inside the PHV's
 * contained expression, so that changing the nullability status of
 * the rel might change what the PHV computes.
 *
 * The code here to cope with upper-level PHVs is likely dead, but keep it
 * anyway just in case.
 */
bool
contain_placeholder_references_to(PlannerInfo *root, Node *clause,
								  int relid)
{
	contain_placeholder_references_context context;

	/* We can answer quickly in the common case that there's no PHVs at all */
	if (root->glob->lastPHId == 0)
		return false;
	/* Else run the recursive search */
	context.relid = relid;
	context.sublevels_up = 0;
	return contain_placeholder_references_walker(clause, &context);
}

static bool
contain_placeholder_references_walker(Node *node,
									  contain_placeholder_references_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		/* We should just look through PHVs of other query levels */
		if (phv->phlevelsup == context->sublevels_up)
		{
			/* If phrels matches, we found what we came for */
			if (bms_is_member(context->relid, phv->phrels))
				return true;

			/*
			 * We should not examine phnullingrels: what we are looking for is
			 * references in the contained expression, not OJs that might null
			 * the result afterwards.  Also, we don't need to recurse into the
			 * contained expression, because phrels should adequately
			 * summarize what's in there.  So we're done here.
			 */
			return false;
		}
	}
	else if (IsA(node, Query))
	{
		/* Recurse into RTE subquery or not-yet-planned sublink subquery */
		bool		result;

		context->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   contain_placeholder_references_walker,
								   context,
								   0);
		context->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, contain_placeholder_references_walker,
								  context);
}
