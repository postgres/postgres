/*-------------------------------------------------------------------------
 *
 * initsplan.c
 *	  Target list, qualification, joininfo initialization routines
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/initsplan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/analyze.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"


/* These parameters are set by GUC */
int			from_collapse_limit;
int			join_collapse_limit;


/* Elements of the postponed_qual_list used during deconstruct_recurse */
typedef struct PostponedQual
{
	Node	   *qual;			/* a qual clause waiting to be processed */
	Relids		relids;			/* the set of baserels it references */
} PostponedQual;


static void extract_lateral_references(PlannerInfo *root, RelOptInfo *brel,
						   Index rtindex);
static List *deconstruct_recurse(PlannerInfo *root, Node *jtnode,
					bool below_outer_join,
					Relids *qualscope, Relids *inner_join_rels,
					List **postponed_qual_list);
static SpecialJoinInfo *make_outerjoininfo(PlannerInfo *root,
				   Relids left_rels, Relids right_rels,
				   Relids inner_join_rels,
				   JoinType jointype, List *clause);
static void compute_semijoin_info(SpecialJoinInfo *sjinfo, List *clause);
static void distribute_qual_to_rels(PlannerInfo *root, Node *clause,
						bool is_deduced,
						bool below_outer_join,
						JoinType jointype,
						Relids qualscope,
						Relids ojscope,
						Relids outerjoin_nonnullable,
						Relids deduced_nullable_relids,
						List **postponed_qual_list);
static bool check_outerjoin_delay(PlannerInfo *root, Relids *relids_p,
					  Relids *nullable_relids_p, bool is_pushed_down);
static bool check_equivalence_delay(PlannerInfo *root,
						RestrictInfo *restrictinfo);
static bool check_redundant_nullability_qual(PlannerInfo *root, Node *clause);
static void check_mergejoinable(RestrictInfo *restrictinfo);
static void check_hashjoinable(RestrictInfo *restrictinfo);


/*****************************************************************************
 *
 *	 JOIN TREES
 *
 *****************************************************************************/

/*
 * add_base_rels_to_query
 *
 *	  Scan the query's jointree and create baserel RelOptInfos for all
 *	  the base relations (ie, table, subquery, and function RTEs)
 *	  appearing in the jointree.
 *
 * The initial invocation must pass root->parse->jointree as the value of
 * jtnode.  Internally, the function recurses through the jointree.
 *
 * At the end of this process, there should be one baserel RelOptInfo for
 * every non-join RTE that is used in the query.  Therefore, this routine
 * is the only place that should call build_simple_rel with reloptkind
 * RELOPT_BASEREL.  (Note: build_simple_rel recurses internally to build
 * "other rel" RelOptInfos for the members of any appendrels we find here.)
 */
void
add_base_rels_to_query(PlannerInfo *root, Node *jtnode)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		(void) build_simple_rel(root, varno, RELOPT_BASEREL);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			add_base_rels_to_query(root, lfirst(l));
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		add_base_rels_to_query(root, j->larg);
		add_base_rels_to_query(root, j->rarg);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}


/*****************************************************************************
 *
 *	 TARGET LISTS
 *
 *****************************************************************************/

/*
 * build_base_rel_tlists
 *	  Add targetlist entries for each var needed in the query's final tlist
 *	  (and HAVING clause, if any) to the appropriate base relations.
 *
 * We mark such vars as needed by "relation 0" to ensure that they will
 * propagate up through all join plan steps.
 */
void
build_base_rel_tlists(PlannerInfo *root, List *final_tlist)
{
	List	   *tlist_vars = pull_var_clause((Node *) final_tlist,
											 PVC_RECURSE_AGGREGATES,
											 PVC_INCLUDE_PLACEHOLDERS);

	if (tlist_vars != NIL)
	{
		add_vars_to_targetlist(root, tlist_vars, bms_make_singleton(0), true);
		list_free(tlist_vars);
	}

	/*
	 * If there's a HAVING clause, we'll need the Vars it uses, too.
	 */
	if (root->parse->havingQual)
	{
		List	   *having_vars = pull_var_clause(root->parse->havingQual,
												  PVC_RECURSE_AGGREGATES,
												  PVC_INCLUDE_PLACEHOLDERS);

		if (having_vars != NIL)
		{
			add_vars_to_targetlist(root, having_vars,
								   bms_make_singleton(0), true);
			list_free(having_vars);
		}
	}
}

/*
 * add_vars_to_targetlist
 *	  For each variable appearing in the list, add it to the owning
 *	  relation's targetlist if not already present, and mark the variable
 *	  as being needed for the indicated join (or for final output if
 *	  where_needed includes "relation 0").
 *
 *	  The list may also contain PlaceHolderVars.  These don't necessarily
 *	  have a single owning relation; we keep their attr_needed info in
 *	  root->placeholder_list instead.  If create_new_ph is true, it's OK
 *	  to create new PlaceHolderInfos; otherwise, the PlaceHolderInfos must
 *	  already exist, and we should only update their ph_needed.  (This should
 *	  be true before deconstruct_jointree begins, and false after that.)
 */
void
add_vars_to_targetlist(PlannerInfo *root, List *vars,
					   Relids where_needed, bool create_new_ph)
{
	ListCell   *temp;

	Assert(!bms_is_empty(where_needed));

	foreach(temp, vars)
	{
		Node	   *node = (Node *) lfirst(temp);

		if (IsA(node, Var))
		{
			Var		   *var = (Var *) node;
			RelOptInfo *rel = find_base_rel(root, var->varno);
			int			attno = var->varattno;

			if (bms_is_subset(where_needed, rel->relids))
				continue;
			Assert(attno >= rel->min_attr && attno <= rel->max_attr);
			attno -= rel->min_attr;
			if (rel->attr_needed[attno] == NULL)
			{
				/* Variable not yet requested, so add to rel's targetlist */
				/* XXX is copyObject necessary here? */
				rel->reltarget.exprs = lappend(rel->reltarget.exprs,
											   copyObject(var));
				/* reltarget cost and width will be computed later */
			}
			rel->attr_needed[attno] = bms_add_members(rel->attr_needed[attno],
													  where_needed);
		}
		else if (IsA(node, PlaceHolderVar))
		{
			PlaceHolderVar *phv = (PlaceHolderVar *) node;
			PlaceHolderInfo *phinfo = find_placeholder_info(root, phv,
															create_new_ph);

			phinfo->ph_needed = bms_add_members(phinfo->ph_needed,
												where_needed);
		}
		else
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
	}
}


/*****************************************************************************
 *
 *	  LATERAL REFERENCES
 *
 *****************************************************************************/

/*
 * find_lateral_references
 *	  For each LATERAL subquery, extract all its references to Vars and
 *	  PlaceHolderVars of the current query level, and make sure those values
 *	  will be available for evaluation of the subquery.
 *
 * While later planning steps ensure that the Var/PHV source rels are on the
 * outside of nestloops relative to the LATERAL subquery, we also need to
 * ensure that the Vars/PHVs propagate up to the nestloop join level; this
 * means setting suitable where_needed values for them.
 *
 * Note that this only deals with lateral references in unflattened LATERAL
 * subqueries.  When we flatten a LATERAL subquery, its lateral references
 * become plain Vars in the parent query, but they may have to be wrapped in
 * PlaceHolderVars if they need to be forced NULL by outer joins that don't
 * also null the LATERAL subquery.  That's all handled elsewhere.
 *
 * This has to run before deconstruct_jointree, since it might result in
 * creation of PlaceHolderInfos.
 */
void
find_lateral_references(PlannerInfo *root)
{
	Index		rti;

	/* We need do nothing if the query contains no LATERAL RTEs */
	if (!root->hasLateralRTEs)
		return;

	/*
	 * Examine all baserels (the rel array has been set up by now).
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];

		/* there may be empty slots corresponding to non-baserel RTEs */
		if (brel == NULL)
			continue;

		Assert(brel->relid == rti);		/* sanity check on array */

		/*
		 * This bit is less obvious than it might look.  We ignore appendrel
		 * otherrels and consider only their parent baserels.  In a case where
		 * a LATERAL-containing UNION ALL subquery was pulled up, it is the
		 * otherrel that is actually going to be in the plan.  However, we
		 * want to mark all its lateral references as needed by the parent,
		 * because it is the parent's relid that will be used for join
		 * planning purposes.  And the parent's RTE will contain all the
		 * lateral references we need to know, since the pulled-up member is
		 * nothing but a copy of parts of the original RTE's subquery.  We
		 * could visit the parent's children instead and transform their
		 * references back to the parent's relid, but it would be much more
		 * complicated for no real gain.  (Important here is that the child
		 * members have not yet received any processing beyond being pulled
		 * up.)  Similarly, in appendrels created by inheritance expansion,
		 * it's sufficient to look at the parent relation.
		 */

		/* ignore RTEs that are "other rels" */
		if (brel->reloptkind != RELOPT_BASEREL)
			continue;

		extract_lateral_references(root, brel, rti);
	}
}

static void
extract_lateral_references(PlannerInfo *root, RelOptInfo *brel, Index rtindex)
{
	RangeTblEntry *rte = root->simple_rte_array[rtindex];
	List	   *vars;
	List	   *newvars;
	Relids		where_needed;
	ListCell   *lc;

	/* No cross-references are possible if it's not LATERAL */
	if (!rte->lateral)
		return;

	/* Fetch the appropriate variables */
	if (rte->rtekind == RTE_RELATION)
		vars = pull_vars_of_level((Node *) rte->tablesample, 0);
	else if (rte->rtekind == RTE_SUBQUERY)
		vars = pull_vars_of_level((Node *) rte->subquery, 1);
	else if (rte->rtekind == RTE_FUNCTION)
		vars = pull_vars_of_level((Node *) rte->functions, 0);
	else if (rte->rtekind == RTE_VALUES)
		vars = pull_vars_of_level((Node *) rte->values_lists, 0);
	else
	{
		Assert(false);
		return;					/* keep compiler quiet */
	}

	if (vars == NIL)
		return;					/* nothing to do */

	/* Copy each Var (or PlaceHolderVar) and adjust it to match our level */
	newvars = NIL;
	foreach(lc, vars)
	{
		Node	   *node = (Node *) lfirst(lc);

		node = copyObject(node);
		if (IsA(node, Var))
		{
			Var		   *var = (Var *) node;

			/* Adjustment is easy since it's just one node */
			var->varlevelsup = 0;
		}
		else if (IsA(node, PlaceHolderVar))
		{
			PlaceHolderVar *phv = (PlaceHolderVar *) node;
			int			levelsup = phv->phlevelsup;

			/* Have to work harder to adjust the contained expression too */
			if (levelsup != 0)
				IncrementVarSublevelsUp(node, -levelsup, 0);

			/*
			 * If we pulled the PHV out of a subquery RTE, its expression
			 * needs to be preprocessed.  subquery_planner() already did this
			 * for level-zero PHVs in function and values RTEs, though.
			 */
			if (levelsup > 0)
				phv->phexpr = preprocess_phv_expression(root, phv->phexpr);
		}
		else
			Assert(false);
		newvars = lappend(newvars, node);
	}

	list_free(vars);

	/*
	 * We mark the Vars as being "needed" at the LATERAL RTE.  This is a bit
	 * of a cheat: a more formal approach would be to mark each one as needed
	 * at the join of the LATERAL RTE with its source RTE.  But it will work,
	 * and it's much less tedious than computing a separate where_needed for
	 * each Var.
	 */
	where_needed = bms_make_singleton(rtindex);

	/*
	 * Push Vars into their source relations' targetlists, and PHVs into
	 * root->placeholder_list.
	 */
	add_vars_to_targetlist(root, newvars, where_needed, true);

	/* Remember the lateral references for create_lateral_join_info */
	brel->lateral_vars = newvars;
}

/*
 * create_lateral_join_info
 *	  Fill in the per-base-relation direct_lateral_relids, lateral_relids
 *	  and lateral_referencers sets.
 *
 * This has to run after deconstruct_jointree, because we need to know the
 * final ph_eval_at values for PlaceHolderVars.
 */
void
create_lateral_join_info(PlannerInfo *root)
{
	bool		found_laterals = false;
	Index		rti;
	ListCell   *lc;

	/* We need do nothing if the query contains no LATERAL RTEs */
	if (!root->hasLateralRTEs)
		return;

	/*
	 * Examine all baserels (the rel array has been set up by now).
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];
		Relids		lateral_relids;

		/* there may be empty slots corresponding to non-baserel RTEs */
		if (brel == NULL)
			continue;

		Assert(brel->relid == rti);		/* sanity check on array */

		/* ignore RTEs that are "other rels" */
		if (brel->reloptkind != RELOPT_BASEREL)
			continue;

		lateral_relids = NULL;

		/* consider each laterally-referenced Var or PHV */
		foreach(lc, brel->lateral_vars)
		{
			Node	   *node = (Node *) lfirst(lc);

			if (IsA(node, Var))
			{
				Var		   *var = (Var *) node;

				found_laterals = true;
				lateral_relids = bms_add_member(lateral_relids,
												var->varno);
			}
			else if (IsA(node, PlaceHolderVar))
			{
				PlaceHolderVar *phv = (PlaceHolderVar *) node;
				PlaceHolderInfo *phinfo = find_placeholder_info(root, phv,
																false);

				found_laterals = true;
				lateral_relids = bms_add_members(lateral_relids,
												 phinfo->ph_eval_at);
			}
			else
				Assert(false);
		}

		/* We now have all the simple lateral refs from this rel */
		brel->direct_lateral_relids = lateral_relids;
		brel->lateral_relids = bms_copy(lateral_relids);
	}

	/*
	 * Now check for lateral references within PlaceHolderVars, and mark their
	 * eval_at rels as having lateral references to the source rels.
	 *
	 * For a PHV that is due to be evaluated at a baserel, mark its source(s)
	 * as direct lateral dependencies of the baserel (adding onto the ones
	 * recorded above).  If it's due to be evaluated at a join, mark its
	 * source(s) as indirect lateral dependencies of each baserel in the join,
	 * ie put them into lateral_relids but not direct_lateral_relids.  This is
	 * appropriate because we can't put any such baserel on the outside of a
	 * join to one of the PHV's lateral dependencies, but on the other hand we
	 * also can't yet join it directly to the dependency.
	 */
	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(lc);
		Relids		eval_at = phinfo->ph_eval_at;
		int			varno;

		if (phinfo->ph_lateral == NULL)
			continue;			/* PHV is uninteresting if no lateral refs */

		found_laterals = true;

		if (bms_get_singleton_member(eval_at, &varno))
		{
			/* Evaluation site is a baserel */
			RelOptInfo *brel = find_base_rel(root, varno);

			brel->direct_lateral_relids =
				bms_add_members(brel->direct_lateral_relids,
								phinfo->ph_lateral);
			brel->lateral_relids =
				bms_add_members(brel->lateral_relids,
								phinfo->ph_lateral);
		}
		else
		{
			/* Evaluation site is a join */
			varno = -1;
			while ((varno = bms_next_member(eval_at, varno)) >= 0)
			{
				RelOptInfo *brel = find_base_rel(root, varno);

				brel->lateral_relids = bms_add_members(brel->lateral_relids,
													   phinfo->ph_lateral);
			}
		}
	}

	/*
	 * If we found no actual lateral references, we're done; but reset the
	 * hasLateralRTEs flag to avoid useless work later.
	 */
	if (!found_laterals)
	{
		root->hasLateralRTEs = false;
		return;
	}

	/*
	 * Calculate the transitive closure of the lateral_relids sets, so that
	 * they describe both direct and indirect lateral references.  If relation
	 * X references Y laterally, and Y references Z laterally, then we will
	 * have to scan X on the inside of a nestloop with Z, so for all intents
	 * and purposes X is laterally dependent on Z too.
	 *
	 * This code is essentially Warshall's algorithm for transitive closure.
	 * The outer loop considers each baserel, and propagates its lateral
	 * dependencies to those baserels that have a lateral dependency on it.
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];
		Relids		outer_lateral_relids;
		Index		rti2;

		if (brel == NULL || brel->reloptkind != RELOPT_BASEREL)
			continue;

		/* need not consider baserel further if it has no lateral refs */
		outer_lateral_relids = brel->lateral_relids;
		if (outer_lateral_relids == NULL)
			continue;

		/* else scan all baserels */
		for (rti2 = 1; rti2 < root->simple_rel_array_size; rti2++)
		{
			RelOptInfo *brel2 = root->simple_rel_array[rti2];

			if (brel2 == NULL || brel2->reloptkind != RELOPT_BASEREL)
				continue;

			/* if brel2 has lateral ref to brel, propagate brel's refs */
			if (bms_is_member(rti, brel2->lateral_relids))
				brel2->lateral_relids = bms_add_members(brel2->lateral_relids,
														outer_lateral_relids);
		}
	}

	/*
	 * Now that we've identified all lateral references, mark each baserel
	 * with the set of relids of rels that reference it laterally (possibly
	 * indirectly) --- that is, the inverse mapping of lateral_relids.
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];
		Relids		lateral_relids;
		int			rti2;

		if (brel == NULL || brel->reloptkind != RELOPT_BASEREL)
			continue;

		/* Nothing to do at rels with no lateral refs */
		lateral_relids = brel->lateral_relids;
		if (lateral_relids == NULL)
			continue;

		/*
		 * We should not have broken the invariant that lateral_relids is
		 * exactly NULL if empty.
		 */
		Assert(!bms_is_empty(lateral_relids));

		/* Also, no rel should have a lateral dependency on itself */
		Assert(!bms_is_member(rti, lateral_relids));

		/* Mark this rel's referencees */
		rti2 = -1;
		while ((rti2 = bms_next_member(lateral_relids, rti2)) >= 0)
		{
			RelOptInfo *brel2 = root->simple_rel_array[rti2];

			Assert(brel2 != NULL && brel2->reloptkind == RELOPT_BASEREL);
			brel2->lateral_referencers =
				bms_add_member(brel2->lateral_referencers, rti);
		}
	}

	/*
	 * Lastly, propagate lateral_relids and lateral_referencers from appendrel
	 * parent rels to their child rels.  We intentionally give each child rel
	 * the same minimum parameterization, even though it's quite possible that
	 * some don't reference all the lateral rels.  This is because any append
	 * path for the parent will have to have the same parameterization for
	 * every child anyway, and there's no value in forcing extra
	 * reparameterize_path() calls.  Similarly, a lateral reference to the
	 * parent prevents use of otherwise-movable join rels for each child.
	 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *brel = root->simple_rel_array[rti];

		if (brel == NULL || brel->reloptkind != RELOPT_BASEREL)
			continue;

		if (root->simple_rte_array[rti]->inh)
		{
			foreach(lc, root->append_rel_list)
			{
				AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(lc);
				RelOptInfo *childrel;

				if (appinfo->parent_relid != rti)
					continue;
				childrel = root->simple_rel_array[appinfo->child_relid];
				Assert(childrel->reloptkind == RELOPT_OTHER_MEMBER_REL);
				Assert(childrel->direct_lateral_relids == NULL);
				childrel->direct_lateral_relids = brel->direct_lateral_relids;
				Assert(childrel->lateral_relids == NULL);
				childrel->lateral_relids = brel->lateral_relids;
				Assert(childrel->lateral_referencers == NULL);
				childrel->lateral_referencers = brel->lateral_referencers;
			}
		}
	}
}


/*****************************************************************************
 *
 *	  JOIN TREE PROCESSING
 *
 *****************************************************************************/

/*
 * deconstruct_jointree
 *	  Recursively scan the query's join tree for WHERE and JOIN/ON qual
 *	  clauses, and add these to the appropriate restrictinfo and joininfo
 *	  lists belonging to base RelOptInfos.  Also, add SpecialJoinInfo nodes
 *	  to root->join_info_list for any outer joins appearing in the query tree.
 *	  Return a "joinlist" data structure showing the join order decisions
 *	  that need to be made by make_one_rel().
 *
 * The "joinlist" result is a list of items that are either RangeTblRef
 * jointree nodes or sub-joinlists.  All the items at the same level of
 * joinlist must be joined in an order to be determined by make_one_rel()
 * (note that legal orders may be constrained by SpecialJoinInfo nodes).
 * A sub-joinlist represents a subproblem to be planned separately. Currently
 * sub-joinlists arise only from FULL OUTER JOIN or when collapsing of
 * subproblems is stopped by join_collapse_limit or from_collapse_limit.
 *
 * NOTE: when dealing with inner joins, it is appropriate to let a qual clause
 * be evaluated at the lowest level where all the variables it mentions are
 * available.  However, we cannot push a qual down into the nullable side(s)
 * of an outer join since the qual might eliminate matching rows and cause a
 * NULL row to be incorrectly emitted by the join.  Therefore, we artificially
 * OR the minimum-relids of such an outer join into the required_relids of
 * clauses appearing above it.  This forces those clauses to be delayed until
 * application of the outer join (or maybe even higher in the join tree).
 */
List *
deconstruct_jointree(PlannerInfo *root)
{
	List	   *result;
	Relids		qualscope;
	Relids		inner_join_rels;
	List	   *postponed_qual_list = NIL;

	/* Start recursion at top of jointree */
	Assert(root->parse->jointree != NULL &&
		   IsA(root->parse->jointree, FromExpr));

	/* this is filled as we scan the jointree */
	root->nullable_baserels = NULL;

	result = deconstruct_recurse(root, (Node *) root->parse->jointree, false,
								 &qualscope, &inner_join_rels,
								 &postponed_qual_list);

	/* Shouldn't be any leftover quals */
	Assert(postponed_qual_list == NIL);

	return result;
}

/*
 * deconstruct_recurse
 *	  One recursion level of deconstruct_jointree processing.
 *
 * Inputs:
 *	jtnode is the jointree node to examine
 *	below_outer_join is TRUE if this node is within the nullable side of a
 *		higher-level outer join
 * Outputs:
 *	*qualscope gets the set of base Relids syntactically included in this
 *		jointree node (do not modify or free this, as it may also be pointed
 *		to by RestrictInfo and SpecialJoinInfo nodes)
 *	*inner_join_rels gets the set of base Relids syntactically included in
 *		inner joins appearing at or below this jointree node (do not modify
 *		or free this, either)
 *	*postponed_qual_list is a list of PostponedQual structs, which we can
 *		add quals to if they turn out to belong to a higher join level
 *	Return value is the appropriate joinlist for this jointree node
 *
 * In addition, entries will be added to root->join_info_list for outer joins.
 */
static List *
deconstruct_recurse(PlannerInfo *root, Node *jtnode, bool below_outer_join,
					Relids *qualscope, Relids *inner_join_rels,
					List **postponed_qual_list)
{
	List	   *joinlist;

	if (jtnode == NULL)
	{
		*qualscope = NULL;
		*inner_join_rels = NULL;
		return NIL;
	}
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		/* No quals to deal with, just return correct result */
		*qualscope = bms_make_singleton(varno);
		/* A single baserel does not create an inner join */
		*inner_join_rels = NULL;
		joinlist = list_make1(jtnode);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		List	   *child_postponed_quals = NIL;
		int			remaining;
		ListCell   *l;

		/*
		 * First, recurse to handle child joins.  We collapse subproblems into
		 * a single joinlist whenever the resulting joinlist wouldn't exceed
		 * from_collapse_limit members.  Also, always collapse one-element
		 * subproblems, since that won't lengthen the joinlist anyway.
		 */
		*qualscope = NULL;
		*inner_join_rels = NULL;
		joinlist = NIL;
		remaining = list_length(f->fromlist);
		foreach(l, f->fromlist)
		{
			Relids		sub_qualscope;
			List	   *sub_joinlist;
			int			sub_members;

			sub_joinlist = deconstruct_recurse(root, lfirst(l),
											   below_outer_join,
											   &sub_qualscope,
											   inner_join_rels,
											   &child_postponed_quals);
			*qualscope = bms_add_members(*qualscope, sub_qualscope);
			sub_members = list_length(sub_joinlist);
			remaining--;
			if (sub_members <= 1 ||
				list_length(joinlist) + sub_members + remaining <= from_collapse_limit)
				joinlist = list_concat(joinlist, sub_joinlist);
			else
				joinlist = lappend(joinlist, sub_joinlist);
		}

		/*
		 * A FROM with more than one list element is an inner join subsuming
		 * all below it, so we should report inner_join_rels = qualscope. If
		 * there was exactly one element, we should (and already did) report
		 * whatever its inner_join_rels were.  If there were no elements (is
		 * that possible?) the initialization before the loop fixed it.
		 */
		if (list_length(f->fromlist) > 1)
			*inner_join_rels = *qualscope;

		/*
		 * Try to process any quals postponed by children.  If they need
		 * further postponement, add them to my output postponed_qual_list.
		 */
		foreach(l, child_postponed_quals)
		{
			PostponedQual *pq = (PostponedQual *) lfirst(l);

			if (bms_is_subset(pq->relids, *qualscope))
				distribute_qual_to_rels(root, pq->qual,
										false, below_outer_join, JOIN_INNER,
										*qualscope, NULL, NULL, NULL,
										NULL);
			else
				*postponed_qual_list = lappend(*postponed_qual_list, pq);
		}

		/*
		 * Now process the top-level quals.
		 */
		foreach(l, (List *) f->quals)
		{
			Node	   *qual = (Node *) lfirst(l);

			distribute_qual_to_rels(root, qual,
									false, below_outer_join, JOIN_INNER,
									*qualscope, NULL, NULL, NULL,
									postponed_qual_list);
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		List	   *child_postponed_quals = NIL;
		Relids		leftids,
					rightids,
					left_inners,
					right_inners,
					nonnullable_rels,
					nullable_rels,
					ojscope;
		List	   *leftjoinlist,
				   *rightjoinlist;
		List	   *my_quals;
		SpecialJoinInfo *sjinfo;
		ListCell   *l;

		/*
		 * Order of operations here is subtle and critical.  First we recurse
		 * to handle sub-JOINs.  Their join quals will be placed without
		 * regard for whether this level is an outer join, which is correct.
		 * Then we place our own join quals, which are restricted by lower
		 * outer joins in any case, and are forced to this level if this is an
		 * outer join and they mention the outer side.  Finally, if this is an
		 * outer join, we create a join_info_list entry for the join.  This
		 * will prevent quals above us in the join tree that use those rels
		 * from being pushed down below this level.  (It's okay for upper
		 * quals to be pushed down to the outer side, however.)
		 */
		switch (j->jointype)
		{
			case JOIN_INNER:
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   below_outer_join,
												   &leftids, &left_inners,
												   &child_postponed_quals);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													below_outer_join,
													&rightids, &right_inners,
													&child_postponed_quals);
				*qualscope = bms_union(leftids, rightids);
				*inner_join_rels = *qualscope;
				/* Inner join adds no restrictions for quals */
				nonnullable_rels = NULL;
				/* and it doesn't force anything to null, either */
				nullable_rels = NULL;
				break;
			case JOIN_LEFT:
			case JOIN_ANTI:
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   below_outer_join,
												   &leftids, &left_inners,
												   &child_postponed_quals);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													true,
													&rightids, &right_inners,
													&child_postponed_quals);
				*qualscope = bms_union(leftids, rightids);
				*inner_join_rels = bms_union(left_inners, right_inners);
				nonnullable_rels = leftids;
				nullable_rels = rightids;
				break;
			case JOIN_SEMI:
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   below_outer_join,
												   &leftids, &left_inners,
												   &child_postponed_quals);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													below_outer_join,
													&rightids, &right_inners,
													&child_postponed_quals);
				*qualscope = bms_union(leftids, rightids);
				*inner_join_rels = bms_union(left_inners, right_inners);
				/* Semi join adds no restrictions for quals */
				nonnullable_rels = NULL;

				/*
				 * Theoretically, a semijoin would null the RHS; but since the
				 * RHS can't be accessed above the join, this is immaterial
				 * and we needn't account for it.
				 */
				nullable_rels = NULL;
				break;
			case JOIN_FULL:
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   true,
												   &leftids, &left_inners,
												   &child_postponed_quals);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													true,
													&rightids, &right_inners,
													&child_postponed_quals);
				*qualscope = bms_union(leftids, rightids);
				*inner_join_rels = bms_union(left_inners, right_inners);
				/* each side is both outer and inner */
				nonnullable_rels = *qualscope;
				nullable_rels = *qualscope;
				break;
			default:
				/* JOIN_RIGHT was eliminated during reduce_outer_joins() */
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				nonnullable_rels = NULL;		/* keep compiler quiet */
				nullable_rels = NULL;
				leftjoinlist = rightjoinlist = NIL;
				break;
		}

		/* Report all rels that will be nulled anywhere in the jointree */
		root->nullable_baserels = bms_add_members(root->nullable_baserels,
												  nullable_rels);

		/*
		 * Try to process any quals postponed by children.  If they need
		 * further postponement, add them to my output postponed_qual_list.
		 * Quals that can be processed now must be included in my_quals, so
		 * that they'll be handled properly in make_outerjoininfo.
		 */
		my_quals = NIL;
		foreach(l, child_postponed_quals)
		{
			PostponedQual *pq = (PostponedQual *) lfirst(l);

			if (bms_is_subset(pq->relids, *qualscope))
				my_quals = lappend(my_quals, pq->qual);
			else
			{
				/*
				 * We should not be postponing any quals past an outer join.
				 * If this Assert fires, pull_up_subqueries() messed up.
				 */
				Assert(j->jointype == JOIN_INNER);
				*postponed_qual_list = lappend(*postponed_qual_list, pq);
			}
		}
		/* list_concat is nondestructive of its second argument */
		my_quals = list_concat(my_quals, (List *) j->quals);

		/*
		 * For an OJ, form the SpecialJoinInfo now, because we need the OJ's
		 * semantic scope (ojscope) to pass to distribute_qual_to_rels.  But
		 * we mustn't add it to join_info_list just yet, because we don't want
		 * distribute_qual_to_rels to think it is an outer join below us.
		 *
		 * Semijoins are a bit of a hybrid: we build a SpecialJoinInfo, but we
		 * want ojscope = NULL for distribute_qual_to_rels.
		 */
		if (j->jointype != JOIN_INNER)
		{
			sjinfo = make_outerjoininfo(root,
										leftids, rightids,
										*inner_join_rels,
										j->jointype,
										my_quals);
			if (j->jointype == JOIN_SEMI)
				ojscope = NULL;
			else
				ojscope = bms_union(sjinfo->min_lefthand,
									sjinfo->min_righthand);
		}
		else
		{
			sjinfo = NULL;
			ojscope = NULL;
		}

		/* Process the JOIN's qual clauses */
		foreach(l, my_quals)
		{
			Node	   *qual = (Node *) lfirst(l);

			distribute_qual_to_rels(root, qual,
									false, below_outer_join, j->jointype,
									*qualscope,
									ojscope, nonnullable_rels, NULL,
									postponed_qual_list);
		}

		/* Now we can add the SpecialJoinInfo to join_info_list */
		if (sjinfo)
		{
			root->join_info_list = lappend(root->join_info_list, sjinfo);
			/* Each time we do that, recheck placeholder eval levels */
			update_placeholder_eval_levels(root, sjinfo);
		}

		/*
		 * Finally, compute the output joinlist.  We fold subproblems together
		 * except at a FULL JOIN or where join_collapse_limit would be
		 * exceeded.
		 */
		if (j->jointype == JOIN_FULL)
		{
			/* force the join order exactly at this node */
			joinlist = list_make1(list_make2(leftjoinlist, rightjoinlist));
		}
		else if (list_length(leftjoinlist) + list_length(rightjoinlist) <=
				 join_collapse_limit)
		{
			/* OK to combine subproblems */
			joinlist = list_concat(leftjoinlist, rightjoinlist);
		}
		else
		{
			/* can't combine, but needn't force join order above here */
			Node	   *leftpart,
					   *rightpart;

			/* avoid creating useless 1-element sublists */
			if (list_length(leftjoinlist) == 1)
				leftpart = (Node *) linitial(leftjoinlist);
			else
				leftpart = (Node *) leftjoinlist;
			if (list_length(rightjoinlist) == 1)
				rightpart = (Node *) linitial(rightjoinlist);
			else
				rightpart = (Node *) rightjoinlist;
			joinlist = list_make2(leftpart, rightpart);
		}
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
		joinlist = NIL;			/* keep compiler quiet */
	}
	return joinlist;
}

/*
 * make_outerjoininfo
 *	  Build a SpecialJoinInfo for the current outer join
 *
 * Inputs:
 *	left_rels: the base Relids syntactically on outer side of join
 *	right_rels: the base Relids syntactically on inner side of join
 *	inner_join_rels: base Relids participating in inner joins below this one
 *	jointype: what it says (must always be LEFT, FULL, SEMI, or ANTI)
 *	clause: the outer join's join condition (in implicit-AND format)
 *
 * The node should eventually be appended to root->join_info_list, but we
 * do not do that here.
 *
 * Note: we assume that this function is invoked bottom-up, so that
 * root->join_info_list already contains entries for all outer joins that are
 * syntactically below this one.
 */
static SpecialJoinInfo *
make_outerjoininfo(PlannerInfo *root,
				   Relids left_rels, Relids right_rels,
				   Relids inner_join_rels,
				   JoinType jointype, List *clause)
{
	SpecialJoinInfo *sjinfo = makeNode(SpecialJoinInfo);
	Relids		clause_relids;
	Relids		strict_relids;
	Relids		min_lefthand;
	Relids		min_righthand;
	ListCell   *l;

	/*
	 * We should not see RIGHT JOIN here because left/right were switched
	 * earlier
	 */
	Assert(jointype != JOIN_INNER);
	Assert(jointype != JOIN_RIGHT);

	/*
	 * Presently the executor cannot support FOR [KEY] UPDATE/SHARE marking of
	 * rels appearing on the nullable side of an outer join. (It's somewhat
	 * unclear what that would mean, anyway: what should we mark when a result
	 * row is generated from no element of the nullable relation?)	So,
	 * complain if any nullable rel is FOR [KEY] UPDATE/SHARE.
	 *
	 * You might be wondering why this test isn't made far upstream in the
	 * parser.  It's because the parser hasn't got enough info --- consider
	 * FOR UPDATE applied to a view.  Only after rewriting and flattening do
	 * we know whether the view contains an outer join.
	 *
	 * We use the original RowMarkClause list here; the PlanRowMark list would
	 * list everything.
	 */
	foreach(l, root->parse->rowMarks)
	{
		RowMarkClause *rc = (RowMarkClause *) lfirst(l);

		if (bms_is_member(rc->rti, right_rels) ||
			(jointype == JOIN_FULL && bms_is_member(rc->rti, left_rels)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/*------
			 translator: %s is a SQL row locking clause such as FOR UPDATE */
					 errmsg("%s cannot be applied to the nullable side of an outer join",
							LCS_asString(rc->strength))));
	}

	sjinfo->syn_lefthand = left_rels;
	sjinfo->syn_righthand = right_rels;
	sjinfo->jointype = jointype;
	/* this always starts out false */
	sjinfo->delay_upper_joins = false;

	compute_semijoin_info(sjinfo, clause);

	/* If it's a full join, no need to be very smart */
	if (jointype == JOIN_FULL)
	{
		sjinfo->min_lefthand = bms_copy(left_rels);
		sjinfo->min_righthand = bms_copy(right_rels);
		sjinfo->lhs_strict = false;		/* don't care about this */
		return sjinfo;
	}

	/*
	 * Retrieve all relids mentioned within the join clause.
	 */
	clause_relids = pull_varnos((Node *) clause);

	/*
	 * For which relids is the clause strict, ie, it cannot succeed if the
	 * rel's columns are all NULL?
	 */
	strict_relids = find_nonnullable_rels((Node *) clause);

	/* Remember whether the clause is strict for any LHS relations */
	sjinfo->lhs_strict = bms_overlap(strict_relids, left_rels);

	/*
	 * Required LHS always includes the LHS rels mentioned in the clause. We
	 * may have to add more rels based on lower outer joins; see below.
	 */
	min_lefthand = bms_intersect(clause_relids, left_rels);

	/*
	 * Similarly for required RHS.  But here, we must also include any lower
	 * inner joins, to ensure we don't try to commute with any of them.
	 */
	min_righthand = bms_int_members(bms_union(clause_relids, inner_join_rels),
									right_rels);

	/*
	 * Now check previous outer joins for ordering restrictions.
	 */
	foreach(l, root->join_info_list)
	{
		SpecialJoinInfo *otherinfo = (SpecialJoinInfo *) lfirst(l);

		/* ignore full joins --- other mechanisms preserve their ordering */
		if (otherinfo->jointype == JOIN_FULL)
			continue;

		/*
		 * For a lower OJ in our LHS, if our join condition uses the lower
		 * join's RHS and is not strict for that rel, we must preserve the
		 * ordering of the two OJs, so add lower OJ's full syntactic relset to
		 * min_lefthand.  (We must use its full syntactic relset, not just its
		 * min_lefthand + min_righthand.  This is because there might be other
		 * OJs below this one that this one can commute with, but we cannot
		 * commute with them if we don't with this one.)  Also, if the current
		 * join is a semijoin or antijoin, we must preserve ordering
		 * regardless of strictness.
		 *
		 * Note: I believe we have to insist on being strict for at least one
		 * rel in the lower OJ's min_righthand, not its whole syn_righthand.
		 */
		if (bms_overlap(left_rels, otherinfo->syn_righthand))
		{
			if (bms_overlap(clause_relids, otherinfo->syn_righthand) &&
				(jointype == JOIN_SEMI || jointype == JOIN_ANTI ||
				 !bms_overlap(strict_relids, otherinfo->min_righthand)))
			{
				min_lefthand = bms_add_members(min_lefthand,
											   otherinfo->syn_lefthand);
				min_lefthand = bms_add_members(min_lefthand,
											   otherinfo->syn_righthand);
			}
		}

		/*
		 * For a lower OJ in our RHS, if our join condition does not use the
		 * lower join's RHS and the lower OJ's join condition is strict, we
		 * can interchange the ordering of the two OJs; otherwise we must add
		 * the lower OJ's full syntactic relset to min_righthand.
		 *
		 * Also, if our join condition does not use the lower join's LHS
		 * either, force the ordering to be preserved.  Otherwise we can end
		 * up with SpecialJoinInfos with identical min_righthands, which can
		 * confuse join_is_legal (see discussion in backend/optimizer/README).
		 *
		 * Also, we must preserve ordering anyway if either the current join
		 * or the lower OJ is either a semijoin or an antijoin.
		 *
		 * Here, we have to consider that "our join condition" includes any
		 * clauses that syntactically appeared above the lower OJ and below
		 * ours; those are equivalent to degenerate clauses in our OJ and must
		 * be treated as such.  Such clauses obviously can't reference our
		 * LHS, and they must be non-strict for the lower OJ's RHS (else
		 * reduce_outer_joins would have reduced the lower OJ to a plain
		 * join).  Hence the other ways in which we handle clauses within our
		 * join condition are not affected by them.  The net effect is
		 * therefore sufficiently represented by the delay_upper_joins flag
		 * saved for us by check_outerjoin_delay.
		 */
		if (bms_overlap(right_rels, otherinfo->syn_righthand))
		{
			if (bms_overlap(clause_relids, otherinfo->syn_righthand) ||
				!bms_overlap(clause_relids, otherinfo->min_lefthand) ||
				jointype == JOIN_SEMI ||
				jointype == JOIN_ANTI ||
				otherinfo->jointype == JOIN_SEMI ||
				otherinfo->jointype == JOIN_ANTI ||
				!otherinfo->lhs_strict || otherinfo->delay_upper_joins)
			{
				min_righthand = bms_add_members(min_righthand,
												otherinfo->syn_lefthand);
				min_righthand = bms_add_members(min_righthand,
												otherinfo->syn_righthand);
			}
		}
	}

	/*
	 * Examine PlaceHolderVars.  If a PHV is supposed to be evaluated within
	 * this join's nullable side, then ensure that min_righthand contains the
	 * full eval_at set of the PHV.  This ensures that the PHV actually can be
	 * evaluated within the RHS.  Note that this works only because we should
	 * already have determined the final eval_at level for any PHV
	 * syntactically within this join.
	 */
	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);
		Relids		ph_syn_level = phinfo->ph_var->phrels;

		/* Ignore placeholder if it didn't syntactically come from RHS */
		if (!bms_is_subset(ph_syn_level, right_rels))
			continue;

		/* Else, prevent join from being formed before we eval the PHV */
		min_righthand = bms_add_members(min_righthand, phinfo->ph_eval_at);
	}

	/*
	 * If we found nothing to put in min_lefthand, punt and make it the full
	 * LHS, to avoid having an empty min_lefthand which will confuse later
	 * processing. (We don't try to be smart about such cases, just correct.)
	 * Likewise for min_righthand.
	 */
	if (bms_is_empty(min_lefthand))
		min_lefthand = bms_copy(left_rels);
	if (bms_is_empty(min_righthand))
		min_righthand = bms_copy(right_rels);

	/* Now they'd better be nonempty */
	Assert(!bms_is_empty(min_lefthand));
	Assert(!bms_is_empty(min_righthand));
	/* Shouldn't overlap either */
	Assert(!bms_overlap(min_lefthand, min_righthand));

	sjinfo->min_lefthand = min_lefthand;
	sjinfo->min_righthand = min_righthand;

	return sjinfo;
}

/*
 * compute_semijoin_info
 *	  Fill semijoin-related fields of a new SpecialJoinInfo
 *
 * Note: this relies on only the jointype and syn_righthand fields of the
 * SpecialJoinInfo; the rest may not be set yet.
 */
static void
compute_semijoin_info(SpecialJoinInfo *sjinfo, List *clause)
{
	List	   *semi_operators;
	List	   *semi_rhs_exprs;
	bool		all_btree;
	bool		all_hash;
	ListCell   *lc;

	/* Initialize semijoin-related fields in case we can't unique-ify */
	sjinfo->semi_can_btree = false;
	sjinfo->semi_can_hash = false;
	sjinfo->semi_operators = NIL;
	sjinfo->semi_rhs_exprs = NIL;

	/* Nothing more to do if it's not a semijoin */
	if (sjinfo->jointype != JOIN_SEMI)
		return;

	/*
	 * Look to see whether the semijoin's join quals consist of AND'ed
	 * equality operators, with (only) RHS variables on only one side of each
	 * one.  If so, we can figure out how to enforce uniqueness for the RHS.
	 *
	 * Note that the input clause list is the list of quals that are
	 * *syntactically* associated with the semijoin, which in practice means
	 * the synthesized comparison list for an IN or the WHERE of an EXISTS.
	 * Particularly in the latter case, it might contain clauses that aren't
	 * *semantically* associated with the join, but refer to just one side or
	 * the other.  We can ignore such clauses here, as they will just drop
	 * down to be processed within one side or the other.  (It is okay to
	 * consider only the syntactically-associated clauses here because for a
	 * semijoin, no higher-level quals could refer to the RHS, and so there
	 * can be no other quals that are semantically associated with this join.
	 * We do things this way because it is useful to have the set of potential
	 * unique-ification expressions before we can extract the list of quals
	 * that are actually semantically associated with the particular join.)
	 *
	 * Note that the semi_operators list consists of the joinqual operators
	 * themselves (but commuted if needed to put the RHS value on the right).
	 * These could be cross-type operators, in which case the operator
	 * actually needed for uniqueness is a related single-type operator. We
	 * assume here that that operator will be available from the btree or hash
	 * opclass when the time comes ... if not, create_unique_plan() will fail.
	 */
	semi_operators = NIL;
	semi_rhs_exprs = NIL;
	all_btree = true;
	all_hash = enable_hashagg;	/* don't consider hash if not enabled */
	foreach(lc, clause)
	{
		OpExpr	   *op = (OpExpr *) lfirst(lc);
		Oid			opno;
		Node	   *left_expr;
		Node	   *right_expr;
		Relids		left_varnos;
		Relids		right_varnos;
		Relids		all_varnos;
		Oid			opinputtype;

		/* Is it a binary opclause? */
		if (!IsA(op, OpExpr) ||
			list_length(op->args) != 2)
		{
			/* No, but does it reference both sides? */
			all_varnos = pull_varnos((Node *) op);
			if (!bms_overlap(all_varnos, sjinfo->syn_righthand) ||
				bms_is_subset(all_varnos, sjinfo->syn_righthand))
			{
				/*
				 * Clause refers to only one rel, so ignore it --- unless it
				 * contains volatile functions, in which case we'd better
				 * punt.
				 */
				if (contain_volatile_functions((Node *) op))
					return;
				continue;
			}
			/* Non-operator clause referencing both sides, must punt */
			return;
		}

		/* Extract data from binary opclause */
		opno = op->opno;
		left_expr = linitial(op->args);
		right_expr = lsecond(op->args);
		left_varnos = pull_varnos(left_expr);
		right_varnos = pull_varnos(right_expr);
		all_varnos = bms_union(left_varnos, right_varnos);
		opinputtype = exprType(left_expr);

		/* Does it reference both sides? */
		if (!bms_overlap(all_varnos, sjinfo->syn_righthand) ||
			bms_is_subset(all_varnos, sjinfo->syn_righthand))
		{
			/*
			 * Clause refers to only one rel, so ignore it --- unless it
			 * contains volatile functions, in which case we'd better punt.
			 */
			if (contain_volatile_functions((Node *) op))
				return;
			continue;
		}

		/* check rel membership of arguments */
		if (!bms_is_empty(right_varnos) &&
			bms_is_subset(right_varnos, sjinfo->syn_righthand) &&
			!bms_overlap(left_varnos, sjinfo->syn_righthand))
		{
			/* typical case, right_expr is RHS variable */
		}
		else if (!bms_is_empty(left_varnos) &&
				 bms_is_subset(left_varnos, sjinfo->syn_righthand) &&
				 !bms_overlap(right_varnos, sjinfo->syn_righthand))
		{
			/* flipped case, left_expr is RHS variable */
			opno = get_commutator(opno);
			if (!OidIsValid(opno))
				return;
			right_expr = left_expr;
		}
		else
		{
			/* mixed membership of args, punt */
			return;
		}

		/* all operators must be btree equality or hash equality */
		if (all_btree)
		{
			/* oprcanmerge is considered a hint... */
			if (!op_mergejoinable(opno, opinputtype) ||
				get_mergejoin_opfamilies(opno) == NIL)
				all_btree = false;
		}
		if (all_hash)
		{
			/* ... but oprcanhash had better be correct */
			if (!op_hashjoinable(opno, opinputtype))
				all_hash = false;
		}
		if (!(all_btree || all_hash))
			return;

		/* so far so good, keep building lists */
		semi_operators = lappend_oid(semi_operators, opno);
		semi_rhs_exprs = lappend(semi_rhs_exprs, copyObject(right_expr));
	}

	/* Punt if we didn't find at least one column to unique-ify */
	if (semi_rhs_exprs == NIL)
		return;

	/*
	 * The expressions we'd need to unique-ify mustn't be volatile.
	 */
	if (contain_volatile_functions((Node *) semi_rhs_exprs))
		return;

	/*
	 * If we get here, we can unique-ify the semijoin's RHS using at least one
	 * of sorting and hashing.  Save the information about how to do that.
	 */
	sjinfo->semi_can_btree = all_btree;
	sjinfo->semi_can_hash = all_hash;
	sjinfo->semi_operators = semi_operators;
	sjinfo->semi_rhs_exprs = semi_rhs_exprs;
}


/*****************************************************************************
 *
 *	  QUALIFICATIONS
 *
 *****************************************************************************/

/*
 * distribute_qual_to_rels
 *	  Add clause information to either the baserestrictinfo or joininfo list
 *	  (depending on whether the clause is a join) of each base relation
 *	  mentioned in the clause.  A RestrictInfo node is created and added to
 *	  the appropriate list for each rel.  Alternatively, if the clause uses a
 *	  mergejoinable operator and is not delayed by outer-join rules, enter
 *	  the left- and right-side expressions into the query's list of
 *	  EquivalenceClasses.  Alternatively, if the clause needs to be treated
 *	  as belonging to a higher join level, just add it to postponed_qual_list.
 *
 * 'clause': the qual clause to be distributed
 * 'is_deduced': TRUE if the qual came from implied-equality deduction
 * 'below_outer_join': TRUE if the qual is from a JOIN/ON that is below the
 *		nullable side of a higher-level outer join
 * 'jointype': type of join the qual is from (JOIN_INNER for a WHERE clause)
 * 'qualscope': set of baserels the qual's syntactic scope covers
 * 'ojscope': NULL if not an outer-join qual, else the minimum set of baserels
 *		needed to form this join
 * 'outerjoin_nonnullable': NULL if not an outer-join qual, else the set of
 *		baserels appearing on the outer (nonnullable) side of the join
 *		(for FULL JOIN this includes both sides of the join, and must in fact
 *		equal qualscope)
 * 'deduced_nullable_relids': if is_deduced is TRUE, the nullable relids to
 *		impute to the clause; otherwise NULL
 * 'postponed_qual_list': list of PostponedQual structs, which we can add
 *		this qual to if it turns out to belong to a higher join level.
 *		Can be NULL if caller knows postponement is impossible.
 *
 * 'qualscope' identifies what level of JOIN the qual came from syntactically.
 * 'ojscope' is needed if we decide to force the qual up to the outer-join
 * level, which will be ojscope not necessarily qualscope.
 *
 * In normal use (when is_deduced is FALSE), at the time this is called,
 * root->join_info_list must contain entries for all and only those special
 * joins that are syntactically below this qual.  But when is_deduced is TRUE,
 * we are adding new deduced clauses after completion of deconstruct_jointree,
 * so it cannot be assumed that root->join_info_list has anything to do with
 * qual placement.
 */
static void
distribute_qual_to_rels(PlannerInfo *root, Node *clause,
						bool is_deduced,
						bool below_outer_join,
						JoinType jointype,
						Relids qualscope,
						Relids ojscope,
						Relids outerjoin_nonnullable,
						Relids deduced_nullable_relids,
						List **postponed_qual_list)
{
	Relids		relids;
	bool		is_pushed_down;
	bool		outerjoin_delayed;
	bool		pseudoconstant = false;
	bool		maybe_equivalence;
	bool		maybe_outer_join;
	Relids		nullable_relids;
	RestrictInfo *restrictinfo;

	/*
	 * Retrieve all relids mentioned within the clause.
	 */
	relids = pull_varnos(clause);

	/*
	 * In ordinary SQL, a WHERE or JOIN/ON clause can't reference any rels
	 * that aren't within its syntactic scope; however, if we pulled up a
	 * LATERAL subquery then we might find such references in quals that have
	 * been pulled up.  We need to treat such quals as belonging to the join
	 * level that includes every rel they reference.  Although we could make
	 * pull_up_subqueries() place such quals correctly to begin with, it's
	 * easier to handle it here.  When we find a clause that contains Vars
	 * outside its syntactic scope, we add it to the postponed-quals list, and
	 * process it once we've recursed back up to the appropriate join level.
	 */
	if (!bms_is_subset(relids, qualscope))
	{
		PostponedQual *pq = (PostponedQual *) palloc(sizeof(PostponedQual));

		Assert(root->hasLateralRTEs);	/* shouldn't happen otherwise */
		Assert(jointype == JOIN_INNER); /* mustn't postpone past outer join */
		Assert(!is_deduced);	/* shouldn't be deduced, either */
		pq->qual = clause;
		pq->relids = relids;
		*postponed_qual_list = lappend(*postponed_qual_list, pq);
		return;
	}

	/*
	 * If it's an outer-join clause, also check that relids is a subset of
	 * ojscope.  (This should not fail if the syntactic scope check passed.)
	 */
	if (ojscope && !bms_is_subset(relids, ojscope))
		elog(ERROR, "JOIN qualification cannot refer to other relations");

	/*
	 * If the clause is variable-free, our normal heuristic for pushing it
	 * down to just the mentioned rels doesn't work, because there are none.
	 *
	 * If the clause is an outer-join clause, we must force it to the OJ's
	 * semantic level to preserve semantics.
	 *
	 * Otherwise, when the clause contains volatile functions, we force it to
	 * be evaluated at its original syntactic level.  This preserves the
	 * expected semantics.
	 *
	 * When the clause contains no volatile functions either, it is actually a
	 * pseudoconstant clause that will not change value during any one
	 * execution of the plan, and hence can be used as a one-time qual in a
	 * gating Result plan node.  We put such a clause into the regular
	 * RestrictInfo lists for the moment, but eventually createplan.c will
	 * pull it out and make a gating Result node immediately above whatever
	 * plan node the pseudoconstant clause is assigned to.  It's usually best
	 * to put a gating node as high in the plan tree as possible. If we are
	 * not below an outer join, we can actually push the pseudoconstant qual
	 * all the way to the top of the tree.  If we are below an outer join, we
	 * leave the qual at its original syntactic level (we could push it up to
	 * just below the outer join, but that seems more complex than it's
	 * worth).
	 */
	if (bms_is_empty(relids))
	{
		if (ojscope)
		{
			/* clause is attached to outer join, eval it there */
			relids = bms_copy(ojscope);
			/* mustn't use as gating qual, so don't mark pseudoconstant */
		}
		else
		{
			/* eval at original syntactic level */
			relids = bms_copy(qualscope);
			if (!contain_volatile_functions(clause))
			{
				/* mark as gating qual */
				pseudoconstant = true;
				/* tell createplan.c to check for gating quals */
				root->hasPseudoConstantQuals = true;
				/* if not below outer join, push it to top of tree */
				if (!below_outer_join)
				{
					relids =
						get_relids_in_jointree((Node *) root->parse->jointree,
											   false);
					qualscope = bms_copy(relids);
				}
			}
		}
	}

	/*----------
	 * Check to see if clause application must be delayed by outer-join
	 * considerations.
	 *
	 * A word about is_pushed_down: we mark the qual as "pushed down" if
	 * it is (potentially) applicable at a level different from its original
	 * syntactic level.  This flag is used to distinguish OUTER JOIN ON quals
	 * from other quals pushed down to the same joinrel.  The rules are:
	 *		WHERE quals and INNER JOIN quals: is_pushed_down = true.
	 *		Non-degenerate OUTER JOIN quals: is_pushed_down = false.
	 *		Degenerate OUTER JOIN quals: is_pushed_down = true.
	 * A "degenerate" OUTER JOIN qual is one that doesn't mention the
	 * non-nullable side, and hence can be pushed down into the nullable side
	 * without changing the join result.  It is correct to treat it as a
	 * regular filter condition at the level where it is evaluated.
	 *
	 * Note: it is not immediately obvious that a simple boolean is enough
	 * for this: if for some reason we were to attach a degenerate qual to
	 * its original join level, it would need to be treated as an outer join
	 * qual there.  However, this cannot happen, because all the rels the
	 * clause mentions must be in the outer join's min_righthand, therefore
	 * the join it needs must be formed before the outer join; and we always
	 * attach quals to the lowest level where they can be evaluated.  But
	 * if we were ever to re-introduce a mechanism for delaying evaluation
	 * of "expensive" quals, this area would need work.
	 *----------
	 */
	if (is_deduced)
	{
		/*
		 * If the qual came from implied-equality deduction, it should not be
		 * outerjoin-delayed, else deducer blew it.  But we can't check this
		 * because the join_info_list may now contain OJs above where the qual
		 * belongs.  For the same reason, we must rely on caller to supply the
		 * correct nullable_relids set.
		 */
		Assert(!ojscope);
		is_pushed_down = true;
		outerjoin_delayed = false;
		nullable_relids = deduced_nullable_relids;
		/* Don't feed it back for more deductions */
		maybe_equivalence = false;
		maybe_outer_join = false;
	}
	else if (bms_overlap(relids, outerjoin_nonnullable))
	{
		/*
		 * The qual is attached to an outer join and mentions (some of the)
		 * rels on the nonnullable side, so it's not degenerate.
		 *
		 * We can't use such a clause to deduce equivalence (the left and
		 * right sides might be unequal above the join because one of them has
		 * gone to NULL) ... but we might be able to use it for more limited
		 * deductions, if it is mergejoinable.  So consider adding it to the
		 * lists of set-aside outer-join clauses.
		 */
		is_pushed_down = false;
		maybe_equivalence = false;
		maybe_outer_join = true;

		/* Check to see if must be delayed by lower outer join */
		outerjoin_delayed = check_outerjoin_delay(root,
												  &relids,
												  &nullable_relids,
												  false);

		/*
		 * Now force the qual to be evaluated exactly at the level of joining
		 * corresponding to the outer join.  We cannot let it get pushed down
		 * into the nonnullable side, since then we'd produce no output rows,
		 * rather than the intended single null-extended row, for any
		 * nonnullable-side rows failing the qual.
		 *
		 * (Do this step after calling check_outerjoin_delay, because that
		 * trashes relids.)
		 */
		Assert(ojscope);
		relids = ojscope;
		Assert(!pseudoconstant);
	}
	else
	{
		/*
		 * Normal qual clause or degenerate outer-join clause.  Either way, we
		 * can mark it as pushed-down.
		 */
		is_pushed_down = true;

		/* Check to see if must be delayed by lower outer join */
		outerjoin_delayed = check_outerjoin_delay(root,
												  &relids,
												  &nullable_relids,
												  true);

		if (outerjoin_delayed)
		{
			/* Should still be a subset of current scope ... */
			Assert(root->hasLateralRTEs || bms_is_subset(relids, qualscope));
			Assert(ojscope == NULL || bms_is_subset(relids, ojscope));

			/*
			 * Because application of the qual will be delayed by outer join,
			 * we mustn't assume its vars are equal everywhere.
			 */
			maybe_equivalence = false;

			/*
			 * It's possible that this is an IS NULL clause that's redundant
			 * with a lower antijoin; if so we can just discard it.  We need
			 * not test in any of the other cases, because this will only be
			 * possible for pushed-down, delayed clauses.
			 */
			if (check_redundant_nullability_qual(root, clause))
				return;
		}
		else
		{
			/*
			 * Qual is not delayed by any lower outer-join restriction, so we
			 * can consider feeding it to the equivalence machinery. However,
			 * if it's itself within an outer-join clause, treat it as though
			 * it appeared below that outer join (note that we can only get
			 * here when the clause references only nullable-side rels).
			 */
			maybe_equivalence = true;
			if (outerjoin_nonnullable != NULL)
				below_outer_join = true;
		}

		/*
		 * Since it doesn't mention the LHS, it's certainly not useful as a
		 * set-aside OJ clause, even if it's in an OJ.
		 */
		maybe_outer_join = false;
	}

	/*
	 * Build the RestrictInfo node itself.
	 */
	restrictinfo = make_restrictinfo((Expr *) clause,
									 is_pushed_down,
									 outerjoin_delayed,
									 pseudoconstant,
									 relids,
									 outerjoin_nonnullable,
									 nullable_relids);

	/*
	 * If it's a join clause (either naturally, or because delayed by
	 * outer-join rules), add vars used in the clause to targetlists of their
	 * relations, so that they will be emitted by the plan nodes that scan
	 * those relations (else they won't be available at the join node!).
	 *
	 * Note: if the clause gets absorbed into an EquivalenceClass then this
	 * may be unnecessary, but for now we have to do it to cover the case
	 * where the EC becomes ec_broken and we end up reinserting the original
	 * clauses into the plan.
	 */
	if (bms_membership(relids) == BMS_MULTIPLE)
	{
		List	   *vars = pull_var_clause(clause,
										   PVC_RECURSE_AGGREGATES,
										   PVC_INCLUDE_PLACEHOLDERS);

		add_vars_to_targetlist(root, vars, relids, false);
		list_free(vars);
	}

	/*
	 * We check "mergejoinability" of every clause, not only join clauses,
	 * because we want to know about equivalences between vars of the same
	 * relation, or between vars and consts.
	 */
	check_mergejoinable(restrictinfo);

	/*
	 * If it is a true equivalence clause, send it to the EquivalenceClass
	 * machinery.  We do *not* attach it directly to any restriction or join
	 * lists.  The EC code will propagate it to the appropriate places later.
	 *
	 * If the clause has a mergejoinable operator and is not
	 * outerjoin-delayed, yet isn't an equivalence because it is an outer-join
	 * clause, the EC code may yet be able to do something with it.  We add it
	 * to appropriate lists for further consideration later.  Specifically:
	 *
	 * If it is a left or right outer-join qualification that relates the two
	 * sides of the outer join (no funny business like leftvar1 = leftvar2 +
	 * rightvar), we add it to root->left_join_clauses or
	 * root->right_join_clauses according to which side the nonnullable
	 * variable appears on.
	 *
	 * If it is a full outer-join qualification, we add it to
	 * root->full_join_clauses.  (Ideally we'd discard cases that aren't
	 * leftvar = rightvar, as we do for left/right joins, but this routine
	 * doesn't have the info needed to do that; and the current usage of the
	 * full_join_clauses list doesn't require that, so it's not currently
	 * worth complicating this routine's API to make it possible.)
	 *
	 * If none of the above hold, pass it off to
	 * distribute_restrictinfo_to_rels().
	 *
	 * In all cases, it's important to initialize the left_ec and right_ec
	 * fields of a mergejoinable clause, so that all possibly mergejoinable
	 * expressions have representations in EquivalenceClasses.  If
	 * process_equivalence is successful, it will take care of that;
	 * otherwise, we have to call initialize_mergeclause_eclasses to do it.
	 */
	if (restrictinfo->mergeopfamilies)
	{
		if (maybe_equivalence)
		{
			if (check_equivalence_delay(root, restrictinfo) &&
				process_equivalence(root, restrictinfo, below_outer_join))
				return;
			/* EC rejected it, so set left_ec/right_ec the hard way ... */
			initialize_mergeclause_eclasses(root, restrictinfo);
			/* ... and fall through to distribute_restrictinfo_to_rels */
		}
		else if (maybe_outer_join && restrictinfo->can_join)
		{
			/* we need to set up left_ec/right_ec the hard way */
			initialize_mergeclause_eclasses(root, restrictinfo);
			/* now see if it should go to any outer-join lists */
			if (bms_is_subset(restrictinfo->left_relids,
							  outerjoin_nonnullable) &&
				!bms_overlap(restrictinfo->right_relids,
							 outerjoin_nonnullable))
			{
				/* we have outervar = innervar */
				root->left_join_clauses = lappend(root->left_join_clauses,
												  restrictinfo);
				return;
			}
			if (bms_is_subset(restrictinfo->right_relids,
							  outerjoin_nonnullable) &&
				!bms_overlap(restrictinfo->left_relids,
							 outerjoin_nonnullable))
			{
				/* we have innervar = outervar */
				root->right_join_clauses = lappend(root->right_join_clauses,
												   restrictinfo);
				return;
			}
			if (jointype == JOIN_FULL)
			{
				/* FULL JOIN (above tests cannot match in this case) */
				root->full_join_clauses = lappend(root->full_join_clauses,
												  restrictinfo);
				return;
			}
			/* nope, so fall through to distribute_restrictinfo_to_rels */
		}
		else
		{
			/* we still need to set up left_ec/right_ec */
			initialize_mergeclause_eclasses(root, restrictinfo);
		}
	}

	/* No EC special case applies, so push it into the clause lists */
	distribute_restrictinfo_to_rels(root, restrictinfo);
}

/*
 * check_outerjoin_delay
 *		Detect whether a qual referencing the given relids must be delayed
 *		in application due to the presence of a lower outer join, and/or
 *		may force extra delay of higher-level outer joins.
 *
 * If the qual must be delayed, add relids to *relids_p to reflect the lowest
 * safe level for evaluating the qual, and return TRUE.  Any extra delay for
 * higher-level joins is reflected by setting delay_upper_joins to TRUE in
 * SpecialJoinInfo structs.  We also compute nullable_relids, the set of
 * referenced relids that are nullable by lower outer joins (note that this
 * can be nonempty even for a non-delayed qual).
 *
 * For an is_pushed_down qual, we can evaluate the qual as soon as (1) we have
 * all the rels it mentions, and (2) we are at or above any outer joins that
 * can null any of these rels and are below the syntactic location of the
 * given qual.  We must enforce (2) because pushing down such a clause below
 * the OJ might cause the OJ to emit null-extended rows that should not have
 * been formed, or that should have been rejected by the clause.  (This is
 * only an issue for non-strict quals, since if we can prove a qual mentioning
 * only nullable rels is strict, we'd have reduced the outer join to an inner
 * join in reduce_outer_joins().)
 *
 * To enforce (2), scan the join_info_list and merge the required-relid sets of
 * any such OJs into the clause's own reference list.  At the time we are
 * called, the join_info_list contains only outer joins below this qual.  We
 * have to repeat the scan until no new relids get added; this ensures that
 * the qual is suitably delayed regardless of the order in which OJs get
 * executed.  As an example, if we have one OJ with LHS=A, RHS=B, and one with
 * LHS=B, RHS=C, it is implied that these can be done in either order; if the
 * B/C join is done first then the join to A can null C, so a qual actually
 * mentioning only C cannot be applied below the join to A.
 *
 * For a non-pushed-down qual, this isn't going to determine where we place the
 * qual, but we need to determine outerjoin_delayed and nullable_relids anyway
 * for use later in the planning process.
 *
 * Lastly, a pushed-down qual that references the nullable side of any current
 * join_info_list member and has to be evaluated above that OJ (because its
 * required relids overlap the LHS too) causes that OJ's delay_upper_joins
 * flag to be set TRUE.  This will prevent any higher-level OJs from
 * being interchanged with that OJ, which would result in not having any
 * correct place to evaluate the qual.  (The case we care about here is a
 * sub-select WHERE clause within the RHS of some outer join.  The WHERE
 * clause must effectively be treated as a degenerate clause of that outer
 * join's condition.  Rather than trying to match such clauses with joins
 * directly, we set delay_upper_joins here, and when the upper outer join
 * is processed by make_outerjoininfo, it will refrain from allowing the
 * two OJs to commute.)
 */
static bool
check_outerjoin_delay(PlannerInfo *root,
					  Relids *relids_p, /* in/out parameter */
					  Relids *nullable_relids_p,		/* output parameter */
					  bool is_pushed_down)
{
	Relids		relids;
	Relids		nullable_relids;
	bool		outerjoin_delayed;
	bool		found_some;

	/* fast path if no special joins */
	if (root->join_info_list == NIL)
	{
		*nullable_relids_p = NULL;
		return false;
	}

	/* must copy relids because we need the original value at the end */
	relids = bms_copy(*relids_p);
	nullable_relids = NULL;
	outerjoin_delayed = false;
	do
	{
		ListCell   *l;

		found_some = false;
		foreach(l, root->join_info_list)
		{
			SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(l);

			/* do we reference any nullable rels of this OJ? */
			if (bms_overlap(relids, sjinfo->min_righthand) ||
				(sjinfo->jointype == JOIN_FULL &&
				 bms_overlap(relids, sjinfo->min_lefthand)))
			{
				/* yes; have we included all its rels in relids? */
				if (!bms_is_subset(sjinfo->min_lefthand, relids) ||
					!bms_is_subset(sjinfo->min_righthand, relids))
				{
					/* no, so add them in */
					relids = bms_add_members(relids, sjinfo->min_lefthand);
					relids = bms_add_members(relids, sjinfo->min_righthand);
					outerjoin_delayed = true;
					/* we'll need another iteration */
					found_some = true;
				}
				/* track all the nullable rels of relevant OJs */
				nullable_relids = bms_add_members(nullable_relids,
												  sjinfo->min_righthand);
				if (sjinfo->jointype == JOIN_FULL)
					nullable_relids = bms_add_members(nullable_relids,
													  sjinfo->min_lefthand);
				/* set delay_upper_joins if needed */
				if (is_pushed_down && sjinfo->jointype != JOIN_FULL &&
					bms_overlap(relids, sjinfo->min_lefthand))
					sjinfo->delay_upper_joins = true;
			}
		}
	} while (found_some);

	/* identify just the actually-referenced nullable rels */
	nullable_relids = bms_int_members(nullable_relids, *relids_p);

	/* replace *relids_p, and return nullable_relids */
	bms_free(*relids_p);
	*relids_p = relids;
	*nullable_relids_p = nullable_relids;
	return outerjoin_delayed;
}

/*
 * check_equivalence_delay
 *		Detect whether a potential equivalence clause is rendered unsafe
 *		by outer-join-delay considerations.  Return TRUE if it's safe.
 *
 * The initial tests in distribute_qual_to_rels will consider a mergejoinable
 * clause to be a potential equivalence clause if it is not outerjoin_delayed.
 * But since the point of equivalence processing is that we will recombine the
 * two sides of the clause with others, we have to check that each side
 * satisfies the not-outerjoin_delayed condition on its own; otherwise it might
 * not be safe to evaluate everywhere we could place a derived equivalence
 * condition.
 */
static bool
check_equivalence_delay(PlannerInfo *root,
						RestrictInfo *restrictinfo)
{
	Relids		relids;
	Relids		nullable_relids;

	/* fast path if no special joins */
	if (root->join_info_list == NIL)
		return true;

	/* must copy restrictinfo's relids to avoid changing it */
	relids = bms_copy(restrictinfo->left_relids);
	/* check left side does not need delay */
	if (check_outerjoin_delay(root, &relids, &nullable_relids, true))
		return false;

	/* and similarly for the right side */
	relids = bms_copy(restrictinfo->right_relids);
	if (check_outerjoin_delay(root, &relids, &nullable_relids, true))
		return false;

	return true;
}

/*
 * check_redundant_nullability_qual
 *	  Check to see if the qual is an IS NULL qual that is redundant with
 *	  a lower JOIN_ANTI join.
 *
 * We want to suppress redundant IS NULL quals, not so much to save cycles
 * as to avoid generating bogus selectivity estimates for them.  So if
 * redundancy is detected here, distribute_qual_to_rels() just throws away
 * the qual.
 */
static bool
check_redundant_nullability_qual(PlannerInfo *root, Node *clause)
{
	Var		   *forced_null_var;
	Index		forced_null_rel;
	ListCell   *lc;

	/* Check for IS NULL, and identify the Var forced to NULL */
	forced_null_var = find_forced_null_var(clause);
	if (forced_null_var == NULL)
		return false;
	forced_null_rel = forced_null_var->varno;

	/*
	 * If the Var comes from the nullable side of a lower antijoin, the IS
	 * NULL condition is necessarily true.
	 */
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);

		if (sjinfo->jointype == JOIN_ANTI &&
			bms_is_member(forced_null_rel, sjinfo->syn_righthand))
			return true;
	}

	return false;
}

/*
 * distribute_restrictinfo_to_rels
 *	  Push a completed RestrictInfo into the proper restriction or join
 *	  clause list(s).
 *
 * This is the last step of distribute_qual_to_rels() for ordinary qual
 * clauses.  Clauses that are interesting for equivalence-class processing
 * are diverted to the EC machinery, but may ultimately get fed back here.
 */
void
distribute_restrictinfo_to_rels(PlannerInfo *root,
								RestrictInfo *restrictinfo)
{
	Relids		relids = restrictinfo->required_relids;
	RelOptInfo *rel;

	switch (bms_membership(relids))
	{
		case BMS_SINGLETON:

			/*
			 * There is only one relation participating in the clause, so it
			 * is a restriction clause for that relation.
			 */
			rel = find_base_rel(root, bms_singleton_member(relids));

			/* Add clause to rel's restriction list */
			rel->baserestrictinfo = lappend(rel->baserestrictinfo,
											restrictinfo);
			break;
		case BMS_MULTIPLE:

			/*
			 * The clause is a join clause, since there is more than one rel
			 * in its relid set.
			 */

			/*
			 * Check for hashjoinable operators.  (We don't bother setting the
			 * hashjoin info except in true join clauses.)
			 */
			check_hashjoinable(restrictinfo);

			/*
			 * Add clause to the join lists of all the relevant relations.
			 */
			add_join_clause_to_rels(root, restrictinfo, relids);
			break;
		default:

			/*
			 * clause references no rels, and therefore we have no place to
			 * attach it.  Shouldn't get here if callers are working properly.
			 */
			elog(ERROR, "cannot cope with variable-free clause");
			break;
	}
}

/*
 * process_implied_equality
 *	  Create a restrictinfo item that says "item1 op item2", and push it
 *	  into the appropriate lists.  (In practice opno is always a btree
 *	  equality operator.)
 *
 * "qualscope" is the nominal syntactic level to impute to the restrictinfo.
 * This must contain at least all the rels used in the expressions, but it
 * is used only to set the qual application level when both exprs are
 * variable-free.  Otherwise the qual is applied at the lowest join level
 * that provides all its variables.
 *
 * "nullable_relids" is the set of relids used in the expressions that are
 * potentially nullable below the expressions.  (This has to be supplied by
 * caller because this function is used after deconstruct_jointree, so we
 * don't have knowledge of where the clause items came from.)
 *
 * "both_const" indicates whether both items are known pseudo-constant;
 * in this case it is worth applying eval_const_expressions() in case we
 * can produce constant TRUE or constant FALSE.  (Otherwise it's not,
 * because the expressions went through eval_const_expressions already.)
 *
 * Note: this function will copy item1 and item2, but it is caller's
 * responsibility to make sure that the Relids parameters are fresh copies
 * not shared with other uses.
 *
 * This is currently used only when an EquivalenceClass is found to
 * contain pseudoconstants.  See path/pathkeys.c for more details.
 */
void
process_implied_equality(PlannerInfo *root,
						 Oid opno,
						 Oid collation,
						 Expr *item1,
						 Expr *item2,
						 Relids qualscope,
						 Relids nullable_relids,
						 bool below_outer_join,
						 bool both_const)
{
	Expr	   *clause;

	/*
	 * Build the new clause.  Copy to ensure it shares no substructure with
	 * original (this is necessary in case there are subselects in there...)
	 */
	clause = make_opclause(opno,
						   BOOLOID,		/* opresulttype */
						   false,		/* opretset */
						   (Expr *) copyObject(item1),
						   (Expr *) copyObject(item2),
						   InvalidOid,
						   collation);

	/* If both constant, try to reduce to a boolean constant. */
	if (both_const)
	{
		clause = (Expr *) eval_const_expressions(root, (Node *) clause);

		/* If we produced const TRUE, just drop the clause */
		if (clause && IsA(clause, Const))
		{
			Const	   *cclause = (Const *) clause;

			Assert(cclause->consttype == BOOLOID);
			if (!cclause->constisnull && DatumGetBool(cclause->constvalue))
				return;
		}
	}

	/*
	 * Push the new clause into all the appropriate restrictinfo lists.
	 */
	distribute_qual_to_rels(root, (Node *) clause,
							true, below_outer_join, JOIN_INNER,
							qualscope, NULL, NULL, nullable_relids,
							NULL);
}

/*
 * build_implied_join_equality --- build a RestrictInfo for a derived equality
 *
 * This overlaps the functionality of process_implied_equality(), but we
 * must return the RestrictInfo, not push it into the joininfo tree.
 *
 * Note: this function will copy item1 and item2, but it is caller's
 * responsibility to make sure that the Relids parameters are fresh copies
 * not shared with other uses.
 *
 * Note: we do not do initialize_mergeclause_eclasses() here.  It is
 * caller's responsibility that left_ec/right_ec be set as necessary.
 */
RestrictInfo *
build_implied_join_equality(Oid opno,
							Oid collation,
							Expr *item1,
							Expr *item2,
							Relids qualscope,
							Relids nullable_relids)
{
	RestrictInfo *restrictinfo;
	Expr	   *clause;

	/*
	 * Build the new clause.  Copy to ensure it shares no substructure with
	 * original (this is necessary in case there are subselects in there...)
	 */
	clause = make_opclause(opno,
						   BOOLOID,		/* opresulttype */
						   false,		/* opretset */
						   (Expr *) copyObject(item1),
						   (Expr *) copyObject(item2),
						   InvalidOid,
						   collation);

	/*
	 * Build the RestrictInfo node itself.
	 */
	restrictinfo = make_restrictinfo(clause,
									 true,		/* is_pushed_down */
									 false,		/* outerjoin_delayed */
									 false,		/* pseudoconstant */
									 qualscope, /* required_relids */
									 NULL,		/* outer_relids */
									 nullable_relids);	/* nullable_relids */

	/* Set mergejoinability/hashjoinability flags */
	check_mergejoinable(restrictinfo);
	check_hashjoinable(restrictinfo);

	return restrictinfo;
}


/*****************************************************************************
 *
 *	 CHECKS FOR MERGEJOINABLE AND HASHJOINABLE CLAUSES
 *
 *****************************************************************************/

/*
 * check_mergejoinable
 *	  If the restrictinfo's clause is mergejoinable, set the mergejoin
 *	  info fields in the restrictinfo.
 *
 *	  Currently, we support mergejoin for binary opclauses where
 *	  the operator is a mergejoinable operator.  The arguments can be
 *	  anything --- as long as there are no volatile functions in them.
 */
static void
check_mergejoinable(RestrictInfo *restrictinfo)
{
	Expr	   *clause = restrictinfo->clause;
	Oid			opno;
	Node	   *leftarg;

	if (restrictinfo->pseudoconstant)
		return;
	if (!is_opclause(clause))
		return;
	if (list_length(((OpExpr *) clause)->args) != 2)
		return;

	opno = ((OpExpr *) clause)->opno;
	leftarg = linitial(((OpExpr *) clause)->args);

	if (op_mergejoinable(opno, exprType(leftarg)) &&
		!contain_volatile_functions((Node *) clause))
		restrictinfo->mergeopfamilies = get_mergejoin_opfamilies(opno);

	/*
	 * Note: op_mergejoinable is just a hint; if we fail to find the operator
	 * in any btree opfamilies, mergeopfamilies remains NIL and so the clause
	 * is not treated as mergejoinable.
	 */
}

/*
 * check_hashjoinable
 *	  If the restrictinfo's clause is hashjoinable, set the hashjoin
 *	  info fields in the restrictinfo.
 *
 *	  Currently, we support hashjoin for binary opclauses where
 *	  the operator is a hashjoinable operator.  The arguments can be
 *	  anything --- as long as there are no volatile functions in them.
 */
static void
check_hashjoinable(RestrictInfo *restrictinfo)
{
	Expr	   *clause = restrictinfo->clause;
	Oid			opno;
	Node	   *leftarg;

	if (restrictinfo->pseudoconstant)
		return;
	if (!is_opclause(clause))
		return;
	if (list_length(((OpExpr *) clause)->args) != 2)
		return;

	opno = ((OpExpr *) clause)->opno;
	leftarg = linitial(((OpExpr *) clause)->args);

	if (op_hashjoinable(opno, exprType(leftarg)) &&
		!contain_volatile_functions((Node *) clause))
		restrictinfo->hashjoinoperator = opno;
}
