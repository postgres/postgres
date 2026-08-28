/*-------------------------------------------------------------------------
 *
 * analyzejoins.c
 *	  Routines for simplifying joins after initial query analysis
 *
 * While we do a great deal of join simplification in prep/prepjointree.c,
 * certain optimizations cannot be performed at that stage for lack of
 * detailed information about the query.  The routines here are invoked
 * after initsplan.c has done its work, and can do additional join removal
 * and simplification steps based on the information extracted.
 *
 * Although the decisions about what can be removed are made using the
 * planner's derived data structures, the removals themselves are implemented
 * by editing the query's jointree, which is a far simpler and more stable
 * representation.  We make no attempt to update the derived data structures
 * to match; instead, query_planner() throws them all away and recomputes them
 * whenever we report having removed something.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/analyzejoins.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_class.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "parser/parse_agg.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"

/*
 * Utility structure.  A sorting procedure is needed to simplify the search
 * of SJE-candidate baserels referencing the same database relation.  Having
 * collected all baserels from the query jointree, the planner sorts them
 * according to the reloid value, groups them with the next pass and attempts
 * to remove self-joins.
 *
 * Preliminary sorting prevents quadratic behavior that can be harmful in the
 * case of numerous joins.
 */
typedef struct
{
	int			relid;
	Oid			reloid;
} SelfJoinCandidate;

bool		enable_self_join_elimination;

/* local functions */
static bool join_is_removable(PlannerInfo *root, SpecialJoinInfo *sjinfo);
static Node *remove_join_from_jointree(Node *jtnode, int ojrelid,
									   int *nremoved);
static void remove_rels_from_query_tree(PlannerInfo *root,
										Relids removed_relids);
static bool reduce_semijoin_in_jointree(Node *jtnode, Relids syn_righthand);
static bool rel_supports_distinctness(PlannerInfo *root, RelOptInfo *rel);
static bool rel_is_distinct_for(PlannerInfo *root, RelOptInfo *rel,
								List *clause_list, List **extra_clauses);
static DistinctColInfo *distinct_col_search(int colno, List *distinct_cols);
static bool innerrel_is_unique_ext(PlannerInfo *root,
								   Relids joinrelids,
								   Relids outerrelids,
								   RelOptInfo *innerrel,
								   JoinType jointype,
								   List *restrictlist,
								   bool force_cache,
								   List **extra_clauses);
static bool is_innerrel_unique_for(PlannerInfo *root,
								   Relids joinrelids,
								   Relids outerrelids,
								   RelOptInfo *innerrel,
								   JoinType jointype,
								   List *restrictlist,
								   List **extra_clauses);
static Node *remove_rel_from_jointree(Node *jtnode, int relid,
									  Node **orphan_quals, int *nremoved);
static Node *merge_quals(Node *quals1, Node *quals2);
static void fixup_selfjoin_jointree(PlannerInfo *root, Node *jtnode, int relid,
									Node **hoist_quals, bool *found_relid);
static List *fixup_selfjoin_quals(PlannerInfo *root, List *quals, int relid);
static Node *replace_selfjoin_qual(Node *qual);
static int	self_join_candidates_cmp(const void *a, const void *b);


/*
 * remove_useless_outer_joins
 *		Check for relations that don't actually need to be joined at all,
 *		and remove them from the query's jointree.
 *
 * Returns true if we removed anything.  In that case the caller must discard
 * everything it has derived from the jointree and compute it over again,
 * since we don't try to update any of that here.
 */
bool
remove_useless_outer_joins(PlannerInfo *root)
{
	Relids		removed_relids = NULL;
	ListCell   *lc;

	/*
	 * We are only interested in relations that are left-joined to, so we can
	 * scan the join_info_list to find them easily.
	 */
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);
		int			innerrelid;
		int			nremoved;
		RangeTblEntry *rte;

		/* Skip if not removable */
		if (!join_is_removable(root, sjinfo))
			continue;

		/*
		 * join_is_removable insists that the join's syntactic righthand side
		 * be a single baserel, so we can implement the removal by dropping
		 * the JoinExpr and everything below its righthand side.
		 */
		innerrelid = bms_singleton_member(sjinfo->syn_righthand);

		/* We verify that exactly one JoinExpr gets removed */
		nremoved = 0;
		root->parse->jointree = (FromExpr *)
			remove_join_from_jointree((Node *) root->parse->jointree,
									  sjinfo->ojrelid, &nremoved);
		if (nremoved != 1)
			elog(ERROR, "failed to find join %d in jointree", sjinfo->ojrelid);

		/* Track all the relids we've removed, for use below */
		removed_relids = bms_add_member(removed_relids, innerrelid);
		removed_relids = bms_add_member(removed_relids, sjinfo->ojrelid);

		/*
		 * As in pull_up_simple_subquery, discard no-longer-needed subqueries.
		 * This is not just an optimization, but is necessary to prevent
		 * subsequent processing from descending into stale subtrees and
		 * seeing inconsistent data.  Likewise discard any securityQuals of
		 * the removed rel.  (Although simple_rte_array[] will be rebuilt
		 * shortly, we can still use it to find the RTE in the parse tree.)
		 */
		rte = root->simple_rte_array[innerrelid];
		if (rte->rtekind == RTE_SUBQUERY)
			rte->subquery = NULL;
		rte->securityQuals = NIL;

		/*
		 * It's okay to keep scanning join_info_list for more removable joins,
		 * even though the data that join_is_removable consults is now
		 * slightly out of date.  Removing a join can only delete attr_needed
		 * bits and join clauses, and any attr_needed bit or join clause that
		 * mentions the removed rel above its own join level would have
		 * prevented that rel from being removable.  So what remains to be
		 * examined is unchanged by what we just did.
		 *
		 * The converse doesn't hold: dropping a join can make some other join
		 * removable that didn't look so before.  That's why our caller loops
		 * until we report finding nothing more to remove.
		 */
	}

	if (bms_is_empty(removed_relids))
		return false;

	/* Clean up the traces that the removed rels have left elsewhere */
	remove_rels_from_query_tree(root, removed_relids);

	return true;
}

/*
 * join_is_removable
 *	  Check whether we need not perform this special join at all, because
 *	  it will just duplicate its left input.
 *
 * This is true for a left join for which the join condition cannot match
 * more than one inner-side row.  (There are other possibly interesting
 * cases, but we don't have the infrastructure to prove them.)  We also
 * have to check that the inner side doesn't generate any variables needed
 * above the join.
 */
static bool
join_is_removable(PlannerInfo *root, SpecialJoinInfo *sjinfo)
{
	int			innerrelid;
	RelOptInfo *innerrel;
	Relids		inputrelids;
	Relids		joinrelids;
	List	   *clause_list = NIL;
	ListCell   *l;
	int			attroff;

	/*
	 * Must be a left join to a single baserel, else we aren't going to be
	 * able to do anything with it.
	 */
	if (sjinfo->jointype != JOIN_LEFT)
		return false;

	/*
	 * We test the syntactic righthand side, not min_righthand, because the
	 * removal is done by deleting the whole righthand subtree of the join.
	 * (min_righthand can be a singleton when syn_righthand is not, but in
	 * such a case the attr_needed tests below would reject the join anyway.)
	 */
	if (!bms_get_singleton_member(sjinfo->syn_righthand, &innerrelid))
		return false;
	Assert(bms_equal(sjinfo->min_righthand, sjinfo->syn_righthand));

	/*
	 * Never try to eliminate a left join to the query result rel.  Although
	 * the case is syntactically impossible in standard SQL, MERGE will build
	 * a join tree that looks exactly like that.
	 */
	if (innerrelid == root->parse->resultRelation)
		return false;

	innerrel = find_base_rel(root, innerrelid);

	/*
	 * Before we go to the effort of checking whether any innerrel variables
	 * are needed above the join, make a quick check to eliminate cases in
	 * which we will surely be unable to prove uniqueness of the innerrel.
	 */
	if (!rel_supports_distinctness(root, innerrel))
		return false;

	/* Compute the relid set for the join we are considering */
	inputrelids = bms_union(sjinfo->min_lefthand, sjinfo->min_righthand);
	Assert(sjinfo->ojrelid != 0);
	joinrelids = bms_copy(inputrelids);
	joinrelids = bms_add_member(joinrelids, sjinfo->ojrelid);

	/*
	 * We can't remove the join if any inner-rel attributes are used above the
	 * join.  Here, "above" the join includes pushed-down conditions, so we
	 * should reject if attr_needed includes the OJ's own relid; therefore,
	 * compare to inputrelids not joinrelids.
	 *
	 * As a micro-optimization, it seems better to start with max_attr and
	 * count down rather than starting with min_attr and counting up, on the
	 * theory that the system attributes are somewhat less likely to be wanted
	 * and should be tested last.
	 */
	for (attroff = innerrel->max_attr - innerrel->min_attr;
		 attroff >= 0;
		 attroff--)
	{
		if (!bms_is_subset(innerrel->attr_needed[attroff], inputrelids))
			return false;
	}

	/*
	 * Similarly check that the inner rel isn't needed by any PlaceHolderVars
	 * that will be used above the join.  The PHV case is a little bit more
	 * complicated, because PHVs may have been assigned a ph_eval_at location
	 * that includes the innerrel, yet their contained expression might not
	 * actually reference the innerrel (it could be just a constant, for
	 * instance).  If such a PHV is due to be evaluated above the join then it
	 * needn't prevent join removal.
	 */
	foreach(l, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = (PlaceHolderInfo *) lfirst(l);

		if (bms_overlap(phinfo->ph_lateral, innerrel->relids))
			return false;		/* it references innerrel laterally */
		if (!bms_overlap(phinfo->ph_eval_at, innerrel->relids))
			continue;			/* it definitely doesn't reference innerrel */
		if (bms_is_subset(phinfo->ph_needed, inputrelids))
			continue;			/* PHV is not used above the join */
		if (!bms_is_member(sjinfo->ojrelid, phinfo->ph_eval_at))
			return false;		/* it has to be evaluated below the join */

		/*
		 * We need to be sure there will still be a place to evaluate the PHV
		 * if we remove the join, ie that ph_eval_at wouldn't become empty.
		 */
		if (!bms_overlap(sjinfo->min_lefthand, phinfo->ph_eval_at))
			return false;		/* there isn't any other place to eval PHV */
		/* Check contained expression last, since this is a bit expensive */
		if (bms_overlap(pull_varnos(root, (Node *) phinfo->ph_var->phexpr),
						innerrel->relids))
			return false;		/* contained expression references innerrel */
	}

	/*
	 * Search for mergejoinable clauses that constrain the inner rel against
	 * either the outer rel or a pseudoconstant.  If an operator is
	 * mergejoinable then it behaves like equality for some btree opclass, so
	 * it's what we want.  The mergejoinability test also eliminates clauses
	 * containing volatile functions, which we couldn't depend on.
	 */
	foreach(l, innerrel->joininfo)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(l);

		/*
		 * If the current join commutes with some other outer join(s) via
		 * outer join identity 3, there will be multiple clones of its join
		 * clauses in the joininfo list.  We want to consider only the
		 * has_clone form of such clauses.  Processing more than one form
		 * would be wasteful, and also some of the others would confuse the
		 * RINFO_IS_PUSHED_DOWN test below.
		 */
		if (restrictinfo->is_clone)
			continue;			/* ignore it */

		/*
		 * If it's not a join clause for this outer join, we can't use it.
		 * Note that if the clause is pushed-down, then it is logically from
		 * above the outer join, even if it references no other rels (it might
		 * be from WHERE, for example).
		 */
		if (RINFO_IS_PUSHED_DOWN(restrictinfo, joinrelids))
			continue;			/* ignore; not useful here */

		/* Ignore if it's not a mergejoinable clause */
		if (!restrictinfo->can_join ||
			restrictinfo->mergeopfamilies == NIL)
			continue;			/* not mergejoinable */

		/*
		 * Check if the clause has the form "outer op inner" or "inner op
		 * outer", and if so mark which side is inner.
		 */
		if (!clause_sides_match_join(restrictinfo, sjinfo->min_lefthand,
									 innerrel->relids))
			continue;			/* no good for these input relations */

		/* OK, add to list */
		clause_list = lappend(clause_list, restrictinfo);
	}

	/*
	 * Now that we have the relevant equality join clauses, try to prove the
	 * innerrel distinct.
	 */
	if (rel_is_distinct_for(root, innerrel, clause_list, NULL))
		return true;

	/*
	 * Some day it would be nice to check for other methods of establishing
	 * distinctness.
	 */
	return false;
}

/*
 * remove_join_from_jointree
 *		Delete the JoinExpr with the given RT index, along with everything
 *		below its righthand side, from the query's jointree.
 *
 * The JoinExpr is replaced by its lefthand input.  Its ON conditions can just
 * be dropped: since this is a left join, they could only have determined
 * which righthand rows join to a given lefthand row, and there are no
 * righthand rows anymore.
 *
 * *nremoved is incremented by the number of JoinExprs removed (there should
 * be exactly one, but the caller checks that).
 */
static Node *
remove_join_from_jointree(Node *jtnode, int ojrelid, int *nremoved)
{
	if (jtnode == NULL)
		return NULL;
	if (IsA(jtnode, RangeTblRef))
	{
		/* nothing to do here */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			lfirst(l) = remove_join_from_jointree((Node *) lfirst(l),
												  ojrelid, nremoved);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		if (j->rtindex == ojrelid)
		{
			(*nremoved)++;
			return j->larg;
		}
		j->larg = remove_join_from_jointree(j->larg, ojrelid, nremoved);
		j->rarg = remove_join_from_jointree(j->rarg, ojrelid, nremoved);
	}
	else
		elog(ERROR, "unrecognized jointree node type: %d",
			 (int) nodeTag(jtnode));

	return jtnode;
}

/*
 * remove_rels_from_query_tree
 *		Delete all remaining references to the given relids from the query.
 *
 * Having removed some relations and outer joins from the jointree, we must
 * get rid of any references to them that are left behind elsewhere.  There
 * should be no ordinary Vars of a removed relation left, but OJ relids can
 * still appear in the nullingrels sets of surviving Vars and PlaceHolderVars,
 * and both regular and OJ relids can appear in the phrels sets of
 * PlaceHolderVars.  ChangeVarNodes knows how to strip a relid out of all of
 * those.
 */
static void
remove_rels_from_query_tree(PlannerInfo *root, Relids removed_relids)
{
	int			relid = -1;

	while ((relid = bms_next_member(removed_relids, relid)) >= 0)
	{
		ChangeVarNodes((Node *) root->parse, relid, INVALID_VAR, 0);

		/*
		 * processed_tlist shares some but not all of its nodes with
		 * parse->targetList, so it has to be processed separately.  (That's
		 * harmless: ChangeVarNodes works in-place, and removing a relid that
		 * isn't there is idempotent.)
		 */
		ChangeVarNodes((Node *) root->processed_tlist, relid, INVALID_VAR, 0);

		/* There could be references in the append_rel_list, too */
		if (root->append_rel_list != NIL)
			ChangeVarNodes((Node *) root->append_rel_list, relid, INVALID_VAR, 0);
	}
}

/*
 * reduce_unique_semijoins
 *		Check for semijoins that can be simplified to plain inner joins
 *		because the inner relation is provably unique for the join clauses.
 *
 * Ideally this would happen during reduce_outer_joins, but we don't have
 * enough information at that point.
 *
 * Like the join removal cases, we do this on the query's jointree, so
 * returning true means the caller must recompute the derived data.
 */
bool
reduce_unique_semijoins(PlannerInfo *root)
{
	bool		changed = false;
	ListCell   *lc;

	/*
	 * Scan the join_info_list to find semijoins.
	 */
	foreach(lc, root->join_info_list)
	{
		SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) lfirst(lc);
		int			innerrelid;
		RelOptInfo *innerrel;
		Relids		joinrelids;
		List	   *restrictlist;

		/*
		 * Must be a semijoin to a single baserel, else we aren't going to be
		 * able to do anything with it.
		 */
		if (sjinfo->jointype != JOIN_SEMI)
			continue;

		/*
		 * We test the syntactic righthand side, since that's what identifies
		 * the JoinExpr we'll modify.
		 */
		if (!bms_get_singleton_member(sjinfo->syn_righthand, &innerrelid))
			continue;
		Assert(bms_equal(sjinfo->min_righthand, sjinfo->syn_righthand));

		innerrel = find_base_rel(root, innerrelid);

		/*
		 * Before we trouble to run generate_join_implied_equalities, make a
		 * quick check to eliminate cases in which we will surely be unable to
		 * prove uniqueness of the innerrel.
		 */
		if (!rel_supports_distinctness(root, innerrel))
			continue;

		/* Compute the relid set for the join we are considering */
		joinrelids = bms_union(sjinfo->min_lefthand, sjinfo->min_righthand);
		Assert(sjinfo->ojrelid == 0);	/* SEMI joins don't have RT indexes */

		/*
		 * Since we're only considering a single-rel RHS, any join clauses it
		 * has must be clauses linking it to the semijoin's min_lefthand.  We
		 * can also consider EC-derived join clauses.
		 */
		restrictlist =
			list_concat(generate_join_implied_equalities(root,
														 joinrelids,
														 sjinfo->min_lefthand,
														 innerrel,
														 NULL),
						innerrel->joininfo);

		/* Test whether the innerrel is unique for those clauses. */
		if (!innerrel_is_unique(root,
								joinrelids, sjinfo->min_lefthand, innerrel,
								JOIN_SEMI, restrictlist, true))
			continue;

		/* OK, reduce the join to a plain inner join in the jointree. */
		if (!reduce_semijoin_in_jointree((Node *) root->parse->jointree,
										 sjinfo->syn_righthand))
			elog(ERROR, "failed to find semijoin in jointree");
		changed = true;
	}

	return changed;
}

/*
 * reduce_semijoin_in_jointree
 *		Find the JoinExpr for the semijoin with the given syntactic righthand
 *		side, and turn it into an inner join.
 *
 * Semijoins have no RT index of their own, so we have to identify the one
 * we want by the set of relids on its righthand side.
 */
static bool
reduce_semijoin_in_jointree(Node *jtnode, Relids syn_righthand)
{
	if (jtnode == NULL)
		return false;
	if (IsA(jtnode, RangeTblRef))
	{
		/* nothing to do here */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
		{
			if (reduce_semijoin_in_jointree((Node *) lfirst(l), syn_righthand))
				return true;
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		if (j->jointype == JOIN_SEMI &&
			bms_equal(get_relids_in_jointree(j->rarg, true, false),
					  syn_righthand))
		{
			j->jointype = JOIN_INNER;
			return true;
		}
		if (reduce_semijoin_in_jointree(j->larg, syn_righthand))
			return true;
		if (reduce_semijoin_in_jointree(j->rarg, syn_righthand))
			return true;
	}
	else
		elog(ERROR, "unrecognized jointree node type: %d",
			 (int) nodeTag(jtnode));

	return false;
}


/*
 * rel_supports_distinctness
 *		Could the relation possibly be proven distinct on some set of columns?
 *
 * This is effectively a pre-checking function for rel_is_distinct_for().
 * It must return true if rel_is_distinct_for() could possibly return true
 * with this rel, but it should not expend a lot of cycles.  The idea is
 * that callers can avoid doing possibly-expensive processing to compute
 * rel_is_distinct_for()'s argument lists if the call could not possibly
 * succeed.
 */
static bool
rel_supports_distinctness(PlannerInfo *root, RelOptInfo *rel)
{
	/* We only know about baserels ... */
	if (rel->reloptkind != RELOPT_BASEREL)
		return false;
	if (rel->rtekind == RTE_RELATION)
	{
		/*
		 * For a plain relation, we only know how to prove uniqueness by
		 * reference to unique indexes.  Make sure there's at least one
		 * suitable unique index.  It must be immediately enforced, and not a
		 * partial index. (Keep these conditions in sync with
		 * relation_has_unique_index_for!)
		 */
		ListCell   *lc;

		foreach(lc, rel->indexlist)
		{
			IndexOptInfo *ind = (IndexOptInfo *) lfirst(lc);

			if (ind->unique && ind->immediate && ind->indpred == NIL)
				return true;
		}
	}
	else if (rel->rtekind == RTE_SUBQUERY)
	{
		Query	   *subquery = root->simple_rte_array[rel->relid]->subquery;

		/* Check if the subquery has any qualities that support distinctness */
		if (query_supports_distinctness(subquery))
			return true;
	}
	/* We have no proof rules for any other rtekinds. */
	return false;
}

/*
 * rel_is_distinct_for
 *		Does the relation return only distinct rows according to clause_list?
 *
 * clause_list is a list of join restriction clauses involving this rel and
 * some other one.  Return true if no two rows emitted by this rel could
 * possibly join to the same row of the other rel.
 *
 * The caller must have already determined that each condition is a
 * mergejoinable equality with an expression in this relation on one side, and
 * an expression not involving this relation on the other.  The transient
 * outer_is_left flag is used to identify which side references this relation:
 * left side if outer_is_left is false, right side if it is true.
 *
 * Note that the passed-in clause_list may be destructively modified!  This
 * is OK for current uses, because the clause_list is built by the caller for
 * the sole purpose of passing to this function.
 *
 * (*extra_clauses) to be set to the right sides of baserestrictinfo clauses,
 * looking like "x = const" if distinctness is derived from such clauses, not
 * joininfo clauses.  Pass NULL to the extra_clauses if this value is not
 * needed.
 */
static bool
rel_is_distinct_for(PlannerInfo *root, RelOptInfo *rel, List *clause_list,
					List **extra_clauses)
{
	/*
	 * We could skip a couple of tests here if we assume all callers checked
	 * rel_supports_distinctness first, but it doesn't seem worth taking any
	 * risk for.
	 */
	if (rel->reloptkind != RELOPT_BASEREL)
		return false;
	if (rel->rtekind == RTE_RELATION)
	{
		/*
		 * Examine the indexes to see if we have a matching unique index.
		 * relation_has_unique_index_for automatically adds any usable
		 * restriction clauses for the rel, so we needn't do that here.
		 */
		if (relation_has_unique_index_for(root, rel, clause_list, extra_clauses))
			return true;
	}
	else if (rel->rtekind == RTE_SUBQUERY)
	{
		Index		relid = rel->relid;
		Query	   *subquery = root->simple_rte_array[relid]->subquery;
		List	   *distinct_cols = NIL;
		ListCell   *l;

		/*
		 * Build the argument list for query_is_distinct_for: a list of
		 * DistinctColInfo entries, each holding an output column number that
		 * the query needs to be distinct over, the equality operator that the
		 * column needs to be distinct according to, and that operator's input
		 * collation.  The collation matters because the subquery's own
		 * DISTINCT / GROUP BY / set-op proves uniqueness under its own
		 * collation, which need not agree with the operator's.
		 *
		 * (XXX we are not considering restriction clauses attached to the
		 * subquery; is that worth doing?)
		 */
		foreach(l, clause_list)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, l);
			OpExpr	   *opexpr;
			Var		   *var;
			DistinctColInfo *dcinfo;

			/*
			 * The caller's mergejoinability test should have selected only
			 * OpExprs.  The operator might be a cross-type operator and thus
			 * not exactly the same operator the subquery would consider;
			 * that's all right since query_is_distinct_for can resolve such
			 * cases.
			 */
			opexpr = castNode(OpExpr, rinfo->clause);

			/* caller identified the inner side for us */
			if (rinfo->outer_is_left)
				var = (Var *) get_rightop(rinfo->clause);
			else
				var = (Var *) get_leftop(rinfo->clause);

			/*
			 * We may ignore any RelabelType node above the operand.  (There
			 * won't be more than one, since eval_const_expressions() has been
			 * applied already.)
			 */
			if (var && IsA(var, RelabelType))
				var = (Var *) ((RelabelType *) var)->arg;

			/*
			 * If inner side isn't a Var referencing a subquery output column,
			 * this clause doesn't help us.
			 */
			if (!var || !IsA(var, Var) ||
				var->varno != relid || var->varlevelsup != 0)
				continue;

			dcinfo = palloc_object(DistinctColInfo);
			dcinfo->colno = var->varattno;
			dcinfo->opid = opexpr->opno;
			dcinfo->collid = opexpr->inputcollid;
			distinct_cols = lappend(distinct_cols, dcinfo);
		}

		if (query_is_distinct_for(subquery, distinct_cols))
			return true;
	}
	return false;
}


/*
 * query_supports_distinctness - could the query possibly be proven distinct
 *		on some set of output columns?
 *
 * This is effectively a pre-checking function for query_is_distinct_for().
 * It must return true if query_is_distinct_for() could possibly return true
 * with this query, but it should not expend a lot of cycles.  The idea is
 * that callers can avoid doing possibly-expensive processing to compute
 * query_is_distinct_for()'s argument lists if the call could not possibly
 * succeed.
 */
bool
query_supports_distinctness(Query *query)
{
	/* SRFs break distinctness except with plain DISTINCT, see below */
	if (query->hasTargetSRFs &&
		(query->distinctClause == NIL || query->hasDistinctOn))
		return false;

	/* check for features we can prove distinctness with */
	if (query->distinctClause != NIL ||
		query->groupClause != NIL ||
		query->groupingSets != NIL ||
		query->hasAggs ||
		query->havingQual ||
		query->setOperations)
		return true;

	return false;
}

/*
 * query_is_distinct_for - does query never return duplicates of the
 *		specified columns?
 *
 * query is a not-yet-planned subquery (in current usage, it's always from
 * a subquery RTE, which the planner avoids scribbling on).
 *
 * distinct_cols is a list of DistinctColInfo, one per requested output column.
 * Each entry names the subquery output column number we want distinct, the
 * upper-level equality operator we'll compare values with, and that operator's
 * input collation.  We are interested in whether rows consisting of just these
 * columns are certain to be distinct.
 *
 * "Distinctness" is defined according to whether the corresponding upper-level
 * equality operators would think the values are distinct.  (Note: each opid
 * could be a cross-type operator, and thus not exactly the equality operator
 * that the subquery would use itself.  We use equality_ops_are_compatible() to
 * check compatibility.  That looks at opfamily membership for index AMs that
 * have declared that they support consistent equality semantics within an
 * opfamily, and so should give trustworthy answers for all operators that we
 * might need to deal with here.)
 *
 * The collid must also agree on equality with the collation the subquery's own
 * DISTINCT/GROUP BY/set-op uses to deduplicate the column, else the subquery's
 * distinctness does not carry over to the caller's equality semantics.  Two
 * collations agree on equality if they match or if both are deterministic (in
 * which case both reduce equality to byte-equality; see CREATE COLLATION).
 */
bool
query_is_distinct_for(Query *query, List *distinct_cols)
{
	ListCell   *l;
	DistinctColInfo *dcinfo;

	/*
	 * DISTINCT (including DISTINCT ON) guarantees uniqueness if all the
	 * columns in the DISTINCT clause appear in colnos and operator semantics
	 * match.  With plain DISTINCT this is true even if there are SRFs in the
	 * tlist, since they are all DISTINCT columns and hence get expanded
	 * before the Unique step.  But with DISTINCT ON, the planner may postpone
	 * SRFs that are not DISTINCT ON or ORDER BY columns until after the
	 * Unique step, which can produce duplicates of the DISTINCT ON columns;
	 * so we can't rely on DISTINCT ON if there are any tlist SRFs.
	 */
	if (query->distinctClause &&
		!(query->hasTargetSRFs && query->hasDistinctOn))
	{
		foreach(l, query->distinctClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(l);
			TargetEntry *tle = get_sortgroupclause_tle(sgc,
													   query->targetList);

			dcinfo = distinct_col_search(tle->resno, distinct_cols);
			if (dcinfo == NULL ||
				!equality_ops_are_compatible(dcinfo->opid, sgc->eqop) ||
				!collations_agree_on_equality(dcinfo->collid,
											  exprCollation((Node *) tle->expr)))
				break;			/* exit early if no match */
		}
		if (l == NULL)			/* had matches for all? */
			return true;
	}

	/*
	 * Otherwise, a set-returning function in the query's targetlist can
	 * result in returning duplicate rows, despite any grouping that might
	 * occur before tlist evaluation.  (If all tlist SRFs are within GROUP BY
	 * columns, it would be safe because they'd be expanded before grouping.
	 * But it doesn't currently seem worth the effort to check for that.)
	 */
	if (query->hasTargetSRFs)
		return false;

	/*
	 * Similarly, GROUP BY without GROUPING SETS guarantees uniqueness if all
	 * the grouped columns appear in colnos and operator semantics match.
	 */
	if (query->groupClause && !query->groupingSets)
	{
		foreach(l, query->groupClause)
		{
			SortGroupClause *sgc = (SortGroupClause *) lfirst(l);
			TargetEntry *tle = get_sortgroupclause_tle(sgc,
													   query->targetList);

			dcinfo = distinct_col_search(tle->resno, distinct_cols);
			if (dcinfo == NULL ||
				!equality_ops_are_compatible(dcinfo->opid, sgc->eqop) ||
				!collations_agree_on_equality(dcinfo->collid,
											  exprCollation((Node *) tle->expr)))
				break;			/* exit early if no match */
		}
		if (l == NULL)			/* had matches for all? */
			return true;
	}
	else if (query->groupingSets)
	{
		List	   *gsets;

		/*
		 * If we have grouping sets with expressions, we probably don't have
		 * uniqueness and analysis would be hard. Punt.
		 */
		if (query->groupClause)
			return false;

		/*
		 * If we have no groupClause (therefore no grouping expressions), we
		 * might have one or many empty grouping sets.  If there's just one,
		 * or if the DISTINCT clause is used on the GROUP BY, then we're
		 * returning only one row and are certainly unique.  But otherwise, we
		 * know we're certainly not unique.
		 */
		if (query->groupDistinct)
			return true;

		gsets = expand_grouping_sets(query->groupingSets, false, -1);

		return (list_length(gsets) == 1);
	}
	else
	{
		/*
		 * If we have no GROUP BY, but do have aggregates or HAVING, then the
		 * result is at most one row so it's surely unique, for any operators.
		 */
		if (query->hasAggs || query->havingQual)
			return true;
	}

	/*
	 * UNION, INTERSECT, EXCEPT guarantee uniqueness of the whole output row,
	 * except with ALL.
	 */
	if (query->setOperations)
	{
		SetOperationStmt *topop = castNode(SetOperationStmt, query->setOperations);

		Assert(topop->op != SETOP_NONE);

		if (!topop->all)
		{
			ListCell   *lg;

			/* We're good if all the nonjunk output columns are in colnos */
			lg = list_head(topop->groupClauses);
			foreach(l, query->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(l);
				SortGroupClause *sgc;

				if (tle->resjunk)
					continue;	/* ignore resjunk columns */

				/* non-resjunk columns should have grouping clauses */
				Assert(lg != NULL);
				sgc = (SortGroupClause *) lfirst(lg);
				lg = lnext(topop->groupClauses, lg);

				dcinfo = distinct_col_search(tle->resno, distinct_cols);
				if (dcinfo == NULL ||
					!equality_ops_are_compatible(dcinfo->opid, sgc->eqop) ||
					!collations_agree_on_equality(dcinfo->collid,
												  exprCollation((Node *) tle->expr)))
					break;		/* exit early if no match */
			}
			if (l == NULL)		/* had matches for all? */
				return true;
		}
	}

	/*
	 * XXX Are there any other cases in which we can easily see the result
	 * must be distinct?
	 *
	 * If you do add more smarts to this function, be sure to update
	 * query_supports_distinctness() to match.
	 */

	return false;
}

/*
 * distinct_col_search - subroutine for query_is_distinct_for
 *
 * If colno matches the colno field of an entry in distinct_cols, return a
 * pointer to that entry; else return NULL.  (Ordinarily distinct_cols would
 * not contain duplicate colnos, but if it does, we arbitrarily select the
 * first match.)
 */
static DistinctColInfo *
distinct_col_search(int colno, List *distinct_cols)
{
	foreach_ptr(DistinctColInfo, dcinfo, distinct_cols)
	{
		if (dcinfo->colno == colno)
			return dcinfo;
	}

	return NULL;
}


/*
 * innerrel_is_unique
 *	  Check if the innerrel provably contains at most one tuple matching any
 *	  tuple from the outerrel, based on join clauses in the 'restrictlist'.
 *
 * We need an actual RelOptInfo for the innerrel, but it's sufficient to
 * identify the outerrel by its Relids.  This asymmetry supports use of this
 * function before joinrels have been built.  (The caller is expected to
 * also supply the joinrelids, just to save recalculating that.)
 *
 * The proof must be made based only on clauses that will be "joinquals"
 * rather than "otherquals" at execution.  For an inner join there's no
 * difference; but if the join is outer, we must ignore pushed-down quals,
 * as those will become "otherquals".  Note that this means the answer might
 * vary depending on whether IS_OUTER_JOIN(jointype); since we cache the
 * answer without regard to that, callers must take care not to call this
 * with jointypes that would be classified differently by IS_OUTER_JOIN().
 *
 * The actual proof is undertaken by is_innerrel_unique_for(); this function
 * is a frontend that is mainly concerned with caching the answers.
 * In particular, the force_cache argument allows overriding the internal
 * heuristic about whether to cache negative answers; it should be "true"
 * if making an inquiry that is not part of the normal bottom-up join search
 * sequence.
 */
bool
innerrel_is_unique(PlannerInfo *root,
				   Relids joinrelids,
				   Relids outerrelids,
				   RelOptInfo *innerrel,
				   JoinType jointype,
				   List *restrictlist,
				   bool force_cache)
{
	return innerrel_is_unique_ext(root, joinrelids, outerrelids, innerrel,
								  jointype, restrictlist, force_cache, NULL);
}

/*
 * innerrel_is_unique_ext
 *	  Do the same as innerrel_is_unique(), but also set to (*extra_clauses)
 *	  additional clauses from a baserestrictinfo list used to prove the
 *	  uniqueness.
 *
 * A non-NULL extra_clauses indicates that we're checking for self-join and
 * correspondingly dealing with filtered clauses.
 */
static bool
innerrel_is_unique_ext(PlannerInfo *root,
					   Relids joinrelids,
					   Relids outerrelids,
					   RelOptInfo *innerrel,
					   JoinType jointype,
					   List *restrictlist,
					   bool force_cache,
					   List **extra_clauses)
{
	MemoryContext old_context;
	ListCell   *lc;
	UniqueRelInfo *uniqueRelInfo;
	List	   *outer_exprs = NIL;
	bool		self_join = (extra_clauses != NULL);

	/* Certainly can't prove uniqueness when there are no joinclauses */
	if (restrictlist == NIL)
		return false;

	/*
	 * Make a quick check to eliminate cases in which we will surely be unable
	 * to prove uniqueness of the innerrel.
	 */
	if (!rel_supports_distinctness(root, innerrel))
		return false;

	/*
	 * Query the cache to see if we've managed to prove that innerrel is
	 * unique for any subset of this outerrel.  For non-self-join search, we
	 * don't need an exact match, as extra outerrels can't make the innerrel
	 * any less unique (or more formally, the restrictlist for a join to a
	 * superset outerrel must be a superset of the conditions we successfully
	 * used before). For self-join search, we require an exact match of
	 * outerrels because we need extra clauses to be valid for our case. Also,
	 * for self-join checking we've filtered the clauses list.  Thus, we can
	 * match only the result cached for a self-join search for another
	 * self-join check.
	 */
	foreach(lc, innerrel->unique_for_rels)
	{
		uniqueRelInfo = (UniqueRelInfo *) lfirst(lc);

		if ((!self_join && bms_is_subset(uniqueRelInfo->outerrelids, outerrelids)) ||
			(self_join && bms_equal(uniqueRelInfo->outerrelids, outerrelids) &&
			 uniqueRelInfo->self_join))
		{
			if (extra_clauses)
				*extra_clauses = uniqueRelInfo->extra_clauses;
			return true;		/* Success! */
		}
	}

	/*
	 * Conversely, we may have already determined that this outerrel, or some
	 * superset thereof, cannot prove this innerrel to be unique.
	 */
	foreach(lc, innerrel->non_unique_for_rels)
	{
		Relids		unique_for_rels = (Relids) lfirst(lc);

		if (bms_is_subset(outerrelids, unique_for_rels))
			return false;
	}

	/* No cached information, so try to make the proof. */
	if (is_innerrel_unique_for(root, joinrelids, outerrelids, innerrel,
							   jointype, restrictlist,
							   self_join ? &outer_exprs : NULL))
	{
		/*
		 * Cache the positive result for future probes, being sure to keep it
		 * in the planner_cxt even if we are working in GEQO.
		 *
		 * Note: one might consider trying to isolate the minimal subset of
		 * the outerrels that proved the innerrel unique.  But it's not worth
		 * the trouble, because the planner builds up joinrels incrementally
		 * and so we'll see the minimally sufficient outerrels before any
		 * supersets of them anyway.
		 */
		old_context = MemoryContextSwitchTo(root->planner_cxt);
		uniqueRelInfo = makeNode(UniqueRelInfo);
		uniqueRelInfo->outerrelids = bms_copy(outerrelids);
		uniqueRelInfo->self_join = self_join;
		uniqueRelInfo->extra_clauses = outer_exprs;
		innerrel->unique_for_rels = lappend(innerrel->unique_for_rels,
											uniqueRelInfo);
		MemoryContextSwitchTo(old_context);

		if (extra_clauses)
			*extra_clauses = outer_exprs;
		return true;			/* Success! */
	}
	else
	{
		/*
		 * None of the join conditions for outerrel proved innerrel unique, so
		 * we can safely reject this outerrel or any subset of it in future
		 * checks.
		 *
		 * However, in normal planning mode, caching this knowledge is totally
		 * pointless; it won't be queried again, because we build up joinrels
		 * from smaller to larger.  It's only useful when using GEQO or
		 * another planner extension that attempts planning multiple times.
		 *
		 * Also, allow callers to override that heuristic and force caching;
		 * that's useful for reduce_unique_semijoins, which calls here before
		 * the normal join search starts.
		 */
		if (force_cache || root->assumeReplanning)
		{
			old_context = MemoryContextSwitchTo(root->planner_cxt);
			innerrel->non_unique_for_rels =
				lappend(innerrel->non_unique_for_rels,
						bms_copy(outerrelids));
			MemoryContextSwitchTo(old_context);
		}

		return false;
	}
}

/*
 * is_innerrel_unique_for
 *	  Check if the innerrel provably contains at most one tuple matching any
 *	  tuple from the outerrel, based on join clauses in the 'restrictlist'.
 */
static bool
is_innerrel_unique_for(PlannerInfo *root,
					   Relids joinrelids,
					   Relids outerrelids,
					   RelOptInfo *innerrel,
					   JoinType jointype,
					   List *restrictlist,
					   List **extra_clauses)
{
	List	   *clause_list = NIL;
	ListCell   *lc;

	/*
	 * Search for mergejoinable clauses that constrain the inner rel against
	 * the outer rel.  If an operator is mergejoinable then it behaves like
	 * equality for some btree opclass, so it's what we want.  The
	 * mergejoinability test also eliminates clauses containing volatile
	 * functions, which we couldn't depend on.
	 */
	foreach(lc, restrictlist)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);

		/*
		 * As noted above, if it's a pushed-down clause and we're at an outer
		 * join, we can't use it.
		 */
		if (IS_OUTER_JOIN(jointype) &&
			RINFO_IS_PUSHED_DOWN(restrictinfo, joinrelids))
			continue;

		/* Ignore if it's not a mergejoinable clause */
		if (!restrictinfo->can_join ||
			restrictinfo->mergeopfamilies == NIL)
			continue;			/* not mergejoinable */

		/*
		 * Check if the clause has the form "outer op inner" or "inner op
		 * outer", and if so mark which side is inner.
		 */
		if (!clause_sides_match_join(restrictinfo, outerrelids,
									 innerrel->relids))
			continue;			/* no good for these input relations */

		/* OK, add to the list */
		clause_list = lappend(clause_list, restrictinfo);
	}

	/* Let rel_is_distinct_for() do the hard work */
	return rel_is_distinct_for(root, innerrel, clause_list, extra_clauses);
}

/*
 * Remove the toRemove relation after we have proven that it participates only
 * in an unneeded unique self-join with toKeep.
 *
 * The removal is done by deleting the relation's RangeTblRef from the
 * jointree and then pointing everything that referenced it at the relation we
 * are keeping.  All the conditions that were attached to the removed relation
 * thereby become conditions on the remaining one, which is what we want:
 * we've proven that the two relations select the same rows.  Note that
 * this change requires us to hoist those conditions up to someplace
 * syntactically enclosing toKeep.
 *
 * kmark and rmark are the PlanRowMarks (if any) for the kept and removed
 * relations.  We could re-locate those, but the caller already found them.
 */
static void
remove_self_join_rel(PlannerInfo *root,
					 RelOptInfo *toKeep, RelOptInfo *toRemove,
					 PlanRowMark *kmark, PlanRowMark *rmark)
{
	Node	   *orphan_quals = NULL;
	int			nremoved = 0;
	Node	   *hoist_quals = NULL;
	bool		found_relid = false;

	Assert(toKeep->relid > 0);
	Assert(toRemove->relid > 0);

	/* We verify that exactly one reference gets removed from the jointree */
	root->parse->jointree = (FromExpr *)
		remove_rel_from_jointree((Node *) root->parse->jointree,
								 toRemove->relid,
								 &orphan_quals, &nremoved);
	if (nremoved != 1)
		elog(ERROR, "failed to find relation %d in jointree", toRemove->relid);
	/* The topmost FromExpr can't have gone away, so nothing can be orphaned */
	Assert(root->parse->jointree != NULL);
	Assert(orphan_quals == NULL);

	/*
	 * Replace all references to the removed relation.  Note that this must
	 * happen after the jointree surgery, else we'd not be able to tell the
	 * two relations' RangeTblRefs apart.
	 */
	ChangeVarNodes((Node *) root->parse, toRemove->relid, toKeep->relid, 0);

	/*
	 * processed_tlist shares some but not all of its nodes with
	 * parse->targetList, so it has to be processed separately.  (That's
	 * harmless: ChangeVarNodes works in-place, and the second visit to a
	 * shared node finds nothing to change.)
	 */
	ChangeVarNodes((Node *) root->processed_tlist, toRemove->relid,
				   toKeep->relid, 0);

	/* There could be references in the append_rel_list, too */
	if (root->append_rel_list != NIL)
		ChangeVarNodes((Node *) root->append_rel_list, toRemove->relid,
					   toKeep->relid, 0);

	/* Clean up the quals that the substitution has messed with */
	fixup_selfjoin_jointree(root, (Node *) root->parse->jointree,
							toKeep->relid,
							&hoist_quals, &found_relid);
	/* We shouldn't have any leftover quals, and we must have found toKeep */
	Assert(hoist_quals == NULL);
	Assert(found_relid);

	/*
	 * If the removed relation has a row mark, transfer it to the remaining
	 * one.
	 *
	 * If both rels have row marks, just keep the one corresponding to the
	 * remaining relation because we verified earlier that they have the same
	 * strength.
	 */
	if (rmark)
	{
		if (kmark)
		{
			Assert(kmark->markType == rmark->markType);

			root->rowMarks = list_delete_ptr(root->rowMarks, rmark);
		}
		else
		{
			/* Shouldn't have inheritance children yet. */
			Assert(rmark->rti == rmark->prti);

			rmark->rti = rmark->prti = toKeep->relid;
		}
	}
}

/*
 * remove_rel_from_jointree
 *		Delete the RangeTblRef for the given relation from the query's
 *		jointree.
 *
 * This is used for self-join elimination, where the removed relation's
 * qual conditions must all be preserved (they will be transposed onto the
 * remaining relation afterwards).  Hence, if dropping the RangeTblRef leaves
 * a JoinExpr or FromExpr with nothing under it, we can't simply drop that
 * node; we hand its quals back to the caller in *orphan_quals, to be merged
 * into the nearest enclosing node that still has some content.  That's a
 * valid transformation only for inner joins, but a jointree node can't become
 * empty at an outer join here: remove_self_joins_one_group() insists that the
 * two relations be on the same side of every outer join, so the relation we
 * are keeping would have to be in the emptied subtree too.
 *
 * *nremoved is incremented by the number of RangeTblRefs removed (there
 * should be exactly one, but the caller checks that).
 */
static Node *
remove_rel_from_jointree(Node *jtnode, int relid,
						 Node **orphan_quals, int *nremoved)
{
	if (jtnode == NULL)
		return NULL;
	if (IsA(jtnode, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) jtnode;

		if (rtr->rtindex == relid)
		{
			(*nremoved)++;
			return NULL;
		}
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		List	   *newfromlist = NIL;
		Node	   *sub_orphans = NULL;
		ListCell   *l;

		foreach(l, f->fromlist)
		{
			Node	   *newchild;

			newchild = remove_rel_from_jointree((Node *) lfirst(l), relid,
												&sub_orphans, nremoved);
			if (newchild != NULL)
				newfromlist = lappend(newfromlist, newchild);
		}
		f->fromlist = newfromlist;
		f->quals = merge_quals(sub_orphans, f->quals);
		if (newfromlist == NIL)
		{
			/* Nothing left here, so pass our quals up to the parent */
			*orphan_quals = merge_quals(f->quals, *orphan_quals);
			return NULL;
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		Node	   *sub_orphans = NULL;

		j->larg = remove_rel_from_jointree(j->larg, relid,
										   &sub_orphans, nremoved);
		j->rarg = remove_rel_from_jointree(j->rarg, relid,
										   &sub_orphans, nremoved);
		if (j->larg == NULL || j->rarg == NULL)
		{
			Node	   *surviving = (j->larg != NULL) ? j->larg : j->rarg;
			Node	   *quals = merge_quals(sub_orphans, j->quals);

			/* As explained above, this can only happen for an inner join */
			Assert(j->jointype == JOIN_INNER);
			/* We can't have removed both children */
			Assert(surviving != NULL);

			/*
			 * Replace the join by a FromExpr, so that the surviving side's
			 * rows are still filtered by the join's conditions.
			 */
			return (Node *) makeFromExpr(list_make1(surviving), quals);
		}
		/* A subtree that survives never hands any quals back to us */
		Assert(sub_orphans == NULL);
	}
	else
		elog(ERROR, "unrecognized jointree node type: %d",
			 (int) nodeTag(jtnode));

	return jtnode;
}

/*
 * merge_quals
 *		Combine two jointree qual conditions.
 *
 * quals1 should be the quals from the lower of the two jointree levels,
 * so that those quals get applied first.
 *
 * Jointree quals have been through preprocess_expression() by now, so each
 * one is either NULL or an implicitly-ANDed List.
 */
static Node *
merge_quals(Node *quals1, Node *quals2)
{
	if (quals1 == NULL)
		return quals2;
	if (quals2 == NULL)
		return quals1;
	return (Node *) list_concat(castNode(List, quals1),
								castNode(List, quals2));
}

/*
 * fixup_selfjoin_jointree
 *		Clean up the query's jointree quals after self-join elimination has
 *		merged one relation into another.  (relid is the kept relation.)
 *
 * See fixup_selfjoin_quals() for what needs fixing locally to each qual list.
 * In addition, we need to check quals to see if they refer to relid, and if
 * so make sure they get hoisted to someplace syntactically above relid.
 * Do that using a "hoist_quals" in/out parameter similar to "orphan_quals"
 * in remove_rel_from_jointree.  (We can't readily merge these concerns into
 * a single pass, since remove_rel_from_jointree must run before we relabel
 * the removed rel's Vars.)  In addition, *found_relid is set true if
 * the subtree rooted at jtnode is found to contain relid's RangeTblRef,
 * so that we can tell when to stop hoisting quals.
 * If a qual gets hoisted up, we apply fixup_selfjoin_quals() to it only
 * after it reaches its final level.  This rule improves the odds of
 * detecting duplicate quals.
 */
static void
fixup_selfjoin_jointree(PlannerInfo *root, Node *jtnode, int relid,
						Node **hoist_quals, bool *found_relid)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) jtnode;

		if (rtr->rtindex == relid)
		{
			Assert(!*found_relid);
			*found_relid = true;
		}
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		Node	   *sub_hoist_quals = NULL;
		bool		sub_found_relid = false;
		ListCell   *l;

		foreach(l, f->fromlist)
			fixup_selfjoin_jointree(root, (Node *) lfirst(l), relid,
									&sub_hoist_quals, &sub_found_relid);
		if (sub_found_relid)
		{
			/* This FromExpr covers relid, so OK to stop hoisting quals here */
			f->quals = merge_quals(sub_hoist_quals, f->quals);
			Assert(!*found_relid);
			*found_relid = true;
		}
		else
		{
			/* We might need to hoist some of our own quals too */
			List	   *hoistable = NIL;
			List	   *keepable = NIL;

			foreach_ptr(Node, qual, castNode(List, f->quals))
			{
				if (bms_is_member(relid, pull_varnos(root, qual)))
					hoistable = lappend(hoistable, qual);
				else
					keepable = lappend(keepable, qual);
			}
			f->quals = (Node *) keepable;
			sub_hoist_quals = merge_quals(sub_hoist_quals, (Node *) hoistable);
			*hoist_quals = merge_quals(sub_hoist_quals, *hoist_quals);
		}
		f->quals = (Node *) fixup_selfjoin_quals(root,
												 castNode(List, f->quals),
												 relid);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		Node	   *sub_hoist_quals = NULL;
		bool		sub_found_relid = false;

		fixup_selfjoin_jointree(root, j->larg, relid,
								&sub_hoist_quals, &sub_found_relid);
		fixup_selfjoin_jointree(root, j->rarg, relid,
								&sub_hoist_quals, &sub_found_relid);
		if (sub_found_relid)
		{
			/* This JoinExpr covers relid, so OK to stop hoisting quals here */
			j->quals = merge_quals(sub_hoist_quals, j->quals);
			Assert(!*found_relid);
			*found_relid = true;
		}
		else
		{
			/* We might need to hoist some of our own quals too */
			List	   *hoistable = NIL;
			List	   *keepable = NIL;

			foreach_ptr(Node, qual, castNode(List, j->quals))
			{
				if (bms_is_member(relid, pull_varnos(root, qual)))
					hoistable = lappend(hoistable, qual);
				else
					keepable = lappend(keepable, qual);
			}
			j->quals = (Node *) keepable;
			sub_hoist_quals = merge_quals(sub_hoist_quals, (Node *) hoistable);
			/* We should never need to hoist quals above an outer join */
			Assert(sub_hoist_quals == NULL || j->jointype == JOIN_INNER);
			*hoist_quals = merge_quals(sub_hoist_quals, *hoist_quals);
		}
		j->quals = (Node *) fixup_selfjoin_quals(root,
												 castNode(List, j->quals),
												 relid);
	}
	else
		elog(ERROR, "unrecognized jointree node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * fixup_selfjoin_quals
 *		Clean up one qual list after self-join elimination.
 *
 * Two things need fixing here.  First, a join clause such as "t1.a = t2.a"
 * has turned into "t1.a = t1.a".  For a strict mergejoinable operator that
 * means "t1.a IS NOT NULL", and we should make the substitution, for two
 * reasons:
 * 1. It will typically result in better selectivity estimates.
 * 2. EquivalenceClass processing is likely to make the substitution
 *    if we don't.  While not directly harmful, we'd then fail to
 *    recognize it as a duplicate of a user-written "t1.a IS NOT NULL"
 *    clause, again leading to bad selectivity estimates.
 * Second, conditions that were written against the two relations separately
 * may now be identical, and we don't want to apply the same condition twice
 * (much less double-count its selectivity).
 *
 * We only touch the top-level conjuncts of the list.  There, turning a NULL
 * result into FALSE makes no difference, whereas below a NOT it would,
 * invalidating the IS NOT NULL substitution.  EquivalenceClass processing
 * will not be applied to sub-clauses, and cleaning up duplicates in them
 * seems like more trouble than it's worth.  Also, we only consider clauses
 * that mention the relation we merged into, so that we don't change the
 * treatment of anything we didn't touch.
 *
 * Since this is not a correctness issue but just an optimization opportunity,
 * we likewise don't worry about recognizing duplicates that appear in
 * different qual lists.
 */
static List *
fixup_selfjoin_quals(PlannerInfo *root, List *quals, int relid)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, quals)
	{
		Node	   *qual = (Node *) lfirst(l);

		if (bms_is_member(relid, pull_varnos(root, qual)))
		{
			qual = replace_selfjoin_qual(qual);
			/* Drop it if the substitution has made it a duplicate */
			if (list_member(result, qual))
				continue;
		}
		result = lappend(result, qual);
	}

	return result;
}

/*
 * replace_selfjoin_qual
 *		Replace one "X = X" qual by "X IS NOT NULL", if it is one.
 */
static Node *
replace_selfjoin_qual(Node *qual)
{
	OpExpr	   *opexpr;
	Node	   *leftop;
	Node	   *rightop;
	NullTest   *ntest;

	/* See if it looks like "X op X" */
	if (!is_opclause(qual))
		return qual;
	opexpr = (OpExpr *) qual;
	if (list_length(opexpr->args) != 2)
		return qual;
	leftop = get_leftop((Expr *) opexpr);
	rightop = get_rightop((Expr *) opexpr);
	if (!equal(leftop, rightop))
		return qual;

	/*
	 * The operator must be strict and behave like btree equality, else we
	 * can't conclude that it yields true for any non-null input.  And the
	 * input had better not be volatile, else the two evaluations might not
	 * agree.  If either condition doesn't hold, the clause is not a candidate
	 * to be an equivalence, so we needn't worry about it getting replaced by
	 * equivclass.c.
	 */
	set_opfuncid(opexpr);
	if (!func_strict(opexpr->opfuncid))
		return qual;
	if (!op_mergejoinable(opexpr->opno, exprType(leftop)))
		return qual;
	if (contain_volatile_functions(leftop))
		return qual;

	/* OK, replace it */
	ntest = makeNode(NullTest);
	ntest->arg = (Expr *) leftop;
	ntest->nulltesttype = IS_NOT_NULL;
	ntest->argisrow = false;	/* correct even if composite arg */
	ntest->location = -1;
	return (Node *) ntest;
}

/*
 * split_selfjoin_quals
 *		Processes 'joinquals' by building two lists: one containing the quals
 *		where the columns/exprs are on either side of the join match and
 *		another one containing the remaining quals.
 *
 * 'joinquals' must only contain quals for a RTE_RELATION being joined to
 * itself.
 */
static void
split_selfjoin_quals(PlannerInfo *root, List *joinquals, List **selfjoinquals,
					 List **otherjoinquals, int from, int to)
{
	List	   *sjoinquals = NIL;
	List	   *ojoinquals = NIL;

	foreach_node(RestrictInfo, rinfo, joinquals)
	{
		OpExpr	   *expr;
		Node	   *leftexpr;
		Node	   *rightexpr;

		/*
		 * Since the given joinquals all came from
		 * generate_join_implied_equalities, they ought to look like equality
		 * operators on single-relation expressions.  But let's check that.
		 * Anything that doesn't look like that can be dumped into ojoinquals.
		 */
		if (!rinfo->mergeopfamilies ||
			bms_num_members(rinfo->clause_relids) != 2 ||
			bms_membership(rinfo->left_relids) != BMS_SINGLETON ||
			bms_membership(rinfo->right_relids) != BMS_SINGLETON)
		{
			ojoinquals = lappend(ojoinquals, rinfo);
			continue;
		}

		expr = (OpExpr *) rinfo->clause;

		if (!IsA(expr, OpExpr) || list_length(expr->args) != 2)
		{
			ojoinquals = lappend(ojoinquals, rinfo);
			continue;
		}

		leftexpr = get_leftop(rinfo->clause);
		rightexpr = copyObject(get_rightop(rinfo->clause));

		if (leftexpr && IsA(leftexpr, RelabelType))
			leftexpr = (Node *) ((RelabelType *) leftexpr)->arg;
		if (rightexpr && IsA(rightexpr, RelabelType))
			rightexpr = (Node *) ((RelabelType *) rightexpr)->arg;

		/*
		 * Quite an expensive operation, narrowing the use case. For example,
		 * when we have cast of the same var to different (but compatible)
		 * types.
		 */
		ChangeVarNodes(rightexpr,
					   bms_singleton_member(rinfo->right_relids),
					   bms_singleton_member(rinfo->left_relids), 0);

		if (equal(leftexpr, rightexpr))
			sjoinquals = lappend(sjoinquals, rinfo);
		else
			ojoinquals = lappend(ojoinquals, rinfo);
	}

	*selfjoinquals = sjoinquals;
	*otherjoinquals = ojoinquals;
}

/*
 * Check for a case when uniqueness is at least partly derived from a
 * baserestrictinfo clause. In this case, we have a chance to return only
 * one row (if such clauses on both sides of SJ are equal) or nothing (if they
 * are different).
 */
static bool
match_unique_clauses(PlannerInfo *root, RelOptInfo *outer, List *uclauses,
					 Index relid)
{
	foreach_node(RestrictInfo, rinfo, uclauses)
	{
		Expr	   *clause;
		Node	   *iclause;
		Node	   *c1;
		bool		matched = false;

		Assert(outer->relid > 0 && relid > 0);

		/* Only filters like f(R.x1,...,R.xN) == expr we should consider. */
		Assert(bms_is_empty(rinfo->left_relids) ^
			   bms_is_empty(rinfo->right_relids));

		clause = (Expr *) copyObject(rinfo->clause);
		ChangeVarNodes((Node *) clause, relid, outer->relid, 0);

		iclause = bms_is_empty(rinfo->left_relids) ? get_rightop(clause) :
			get_leftop(clause);
		c1 = bms_is_empty(rinfo->left_relids) ? get_leftop(clause) :
			get_rightop(clause);

		/*
		 * Compare these left and right sides with the corresponding sides of
		 * the outer's filters. If no one is detected - return immediately.
		 */
		foreach_node(RestrictInfo, orinfo, outer->baserestrictinfo)
		{
			Node	   *oclause;
			Node	   *c2;

			if (orinfo->mergeopfamilies == NIL)
				/* Don't consider clauses that aren't similar to 'F(X)=G(Y)' */
				continue;

			Assert(is_opclause(orinfo->clause));

			oclause = bms_is_empty(orinfo->left_relids) ?
				get_rightop(orinfo->clause) : get_leftop(orinfo->clause);
			c2 = (bms_is_empty(orinfo->left_relids) ?
				  get_leftop(orinfo->clause) : get_rightop(orinfo->clause));

			if (equal(iclause, oclause) && equal(c1, c2))
			{
				matched = true;
				break;
			}
		}

		if (!matched)
			return false;
	}

	return true;
}

/*
 * Find and remove unique self-joins in a group of base relations that have
 * the same Oid.
 *
 * Return true if we removed any joins.
 *
 * After a removal, we continue searching for more removals, even though the
 * tests will be using derived data that is now partially stale.  That is safe
 * because we are trying to prove that a candidate pair of relations must
 * match the same row, and the stale data can only omit quals, never invent
 * them.  The removed relation's quals are moved onto the kept relation in
 * the jointree but not into its baserestrictinfo, and no other derived data
 * changes.  A proof made from a subset of the applicable quals remains valid
 * when the rest are added, since extra quals can only remove rows from the
 * join.  So a pass may miss a removal that a later pass will find, but it
 * cannot make one that isn't justified.
 */
static bool
remove_self_joins_one_group(PlannerInfo *root, Relids relids)
{
	bool		removed = false;
	int			k;				/* Index of kept relation */
	int			r = -1;			/* Index of removed relation */

	while ((r = bms_next_member(relids, r)) > 0)
	{
		RelOptInfo *rrel = root->simple_rel_array[r];

		/* k iterates over the relids after r */
		k = r;
		while ((k = bms_next_member(relids, k)) > 0)
		{
			Relids		joinrelids = NULL;
			RelOptInfo *krel = root->simple_rel_array[k];
			List	   *restrictlist;
			List	   *selfjoinquals;
			List	   *otherjoinquals;
			ListCell   *lc;
			bool		jinfo_check = true;
			PlanRowMark *kmark = NULL;
			PlanRowMark *rmark = NULL;
			List	   *uclauses = NIL;

			/* A sanity check: the relations have the same Oid. */
			Assert(root->simple_rte_array[k]->relid ==
				   root->simple_rte_array[r]->relid);

			/*
			 * It is impossible to eliminate the join of two relations if they
			 * are not on the same side of every outer join.  Otherwise, the
			 * planner can't find any variants of the correct query plan.
			 */
			foreach(lc, root->join_info_list)
			{
				SpecialJoinInfo *info = (SpecialJoinInfo *) lfirst(lc);

				if ((bms_is_member(k, info->syn_lefthand) ^
					 bms_is_member(r, info->syn_lefthand)) ||
					(bms_is_member(k, info->syn_righthand) ^
					 bms_is_member(r, info->syn_righthand)))
				{
					jinfo_check = false;
					break;
				}
			}
			if (!jinfo_check)
				continue;

			/*
			 * Check Row Marks equivalence. We can't remove the join if the
			 * relations have row marks of different strength (e.g., one is
			 * locked FOR UPDATE, and another just has ROW_MARK_REFERENCE for
			 * EvalPlanQual rechecking).
			 */
			foreach(lc, root->rowMarks)
			{
				PlanRowMark *rowMark = (PlanRowMark *) lfirst(lc);

				if (rowMark->rti == r)
				{
					Assert(rmark == NULL);
					rmark = rowMark;
				}
				else if (rowMark->rti == k)
				{
					Assert(kmark == NULL);
					kmark = rowMark;
				}

				if (kmark && rmark)
					break;
			}
			if (kmark && rmark && kmark->markType != rmark->markType)
				continue;

			/*
			 * We only deal with base rels here, so their relids bitset
			 * contains only one member -- their relid.
			 */
			joinrelids = bms_add_member(joinrelids, r);
			joinrelids = bms_add_member(joinrelids, k);

			/*
			 * PHVs should not impose any constraints on removing self-joins.
			 */

			/*
			 * At this stage, joininfo lists of inner and outer can contain
			 * only clauses required for a superior outer join that can't
			 * influence this optimization. So, we can avoid to call the
			 * build_joinrel_restrictlist() routine.
			 */
			restrictlist = generate_join_implied_equalities(root, joinrelids,
															rrel->relids,
															krel, NULL);
			if (restrictlist == NIL)
				continue;

			/*
			 * Process restrictlist to separate the self-join quals from the
			 * other quals. e.g., "x = x" goes to selfjoinquals and "a = b" to
			 * otherjoinquals.
			 */
			split_selfjoin_quals(root, restrictlist, &selfjoinquals,
								 &otherjoinquals, rrel->relid, krel->relid);

			Assert(list_length(restrictlist) ==
				   (list_length(selfjoinquals) + list_length(otherjoinquals)));

			/*
			 * To enable SJE for the only degenerate case without any self
			 * join clauses at all, add baserestrictinfo to this list. The
			 * degenerate case works only if both sides have the same clause.
			 * So doesn't matter which side to add.
			 */
			selfjoinquals = list_concat(selfjoinquals, krel->baserestrictinfo);

			/*
			 * Determine if the rrel can duplicate outer rows. We must bypass
			 * the unique rel cache here since we're possibly using a subset
			 * of join quals. We can use 'force_cache' == true when all join
			 * quals are self-join quals.  Otherwise, we could end up putting
			 * false negatives in the cache.
			 */
			if (!innerrel_is_unique_ext(root, joinrelids, rrel->relids,
										krel, JOIN_INNER, selfjoinquals,
										list_length(otherjoinquals) == 0,
										&uclauses))
				continue;

			/*
			 * 'uclauses' is the copy of outer->baserestrictinfo that are
			 * associated with an index.  We proved by matching selfjoinquals
			 * to a unique index that the outer relation has at most one
			 * matching row for each inner row.  Sometimes that is not enough.
			 * e.g. "WHERE s1.b = s2.b AND s1.a = 1 AND s2.a = 2" when the
			 * unique index is (a,b).  Having non-empty uclauses, we must
			 * validate that the inner baserestrictinfo contains the same
			 * expressions, or we won't match the same row on each side of the
			 * join.
			 */
			if (!match_unique_clauses(root, rrel, uclauses, krel->relid))
				continue;

			/* OK, remove rrel from the query */
			remove_self_join_rel(root, krel, rrel, kmark, rmark);
			removed = true;

			/*
			 * Since relation r is now gone, we mustn't keep looking for
			 * matches to it.  But we can keep scanning later relids members
			 * for additional join pairs.
			 */
			break;
		}
	}

	return removed;
}

/*
 * Gather indexes of base relations from the joinlist and try to eliminate
 * self-joins.
 *
 * Return true if we removed any joins.
 */
static bool
remove_self_joins_recurse(PlannerInfo *root, List *joinlist)
{
	bool		removed = false;
	ListCell   *jl;
	Relids		relids = NULL;
	SelfJoinCandidate *candidates;
	int			i;
	int			j;
	int			numRels;

	/* Collect indexes of base relations of the join tree */
	foreach(jl, joinlist)
	{
		Node	   *jlnode = (Node *) lfirst(jl);

		if (IsA(jlnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jlnode)->rtindex;
			RangeTblEntry *rte = root->simple_rte_array[varno];

			/*
			 * We only consider ordinary relations as candidates to be
			 * removed, and these relations should not have TABLESAMPLE
			 * clauses specified.  Removing a relation with TABLESAMPLE clause
			 * could potentially change the semantics of the query. Because of
			 * UPDATE/DELETE EPQ mechanism, currently Query->resultRelation or
			 * Query->mergeTargetRelation associated rel cannot be eliminated.
			 */
			if (rte->rtekind == RTE_RELATION &&
				rte->relkind == RELKIND_RELATION &&
				rte->tablesample == NULL &&
				varno != root->parse->resultRelation &&
				varno != root->parse->mergeTargetRelation)
			{
				Assert(!bms_is_member(varno, relids));
				relids = bms_add_member(relids, varno);
			}
		}
		else if (IsA(jlnode, List))
		{
			/* Recursively perform SJE within the sub-joinlist */
			removed |= remove_self_joins_recurse(root, (List *) jlnode);
		}
		else
			elog(ERROR, "unrecognized joinlist node type: %d",
				 (int) nodeTag(jlnode));
	}

	numRels = bms_num_members(relids);

	/* No work if not at least two relations at this level */
	if (numRels < 2)
		return removed;			/* ... but don't fail to report sub-removals */

	/*
	 * In order to find relations with the same oid we first build an array of
	 * candidates and then sort it by oid.
	 */
	candidates = palloc_array(SelfJoinCandidate, numRels);
	i = -1;
	j = 0;
	while ((i = bms_next_member(relids, i)) >= 0)
	{
		candidates[j].relid = i;
		candidates[j].reloid = root->simple_rte_array[i]->relid;
		j++;
	}

	qsort(candidates, numRels, sizeof(SelfJoinCandidate),
		  self_join_candidates_cmp);

	/*
	 * Iteratively form a group of relation indexes with the same oid and
	 * launch the routine that detects self-joins in this group.
	 *
	 * We remove considered relations from relids as we scan, so that that set
	 * should be empty at the end.
	 */
	i = 0;
	for (j = 1; j <= numRels; j++)
	{
		if (j == numRels || candidates[j].reloid != candidates[i].reloid)
		{
			if (j - i >= 2)
			{
				/* Create a group of relation indexes with the same oid */
				Relids		group = NULL;

				while (i < j)
				{
					group = bms_add_member(group, candidates[i].relid);
					i++;
				}
				relids = bms_del_members(relids, group);

				/* Try to remove self-joins from the group */
				removed |= remove_self_joins_one_group(root, group);
				bms_free(group);
			}
			else
			{
				/* Nothing to do with this group, just drop it from the set */
				while (i < j)
				{
					relids = bms_del_member(relids, candidates[i].relid);
					i++;
				}
			}
		}
	}

	Assert(bms_is_empty(relids));

	return removed;
}

/*
 * Compare self-join candidates by their oids.
 */
static int
self_join_candidates_cmp(const void *a, const void *b)
{
	const SelfJoinCandidate *ca = (const SelfJoinCandidate *) a;
	const SelfJoinCandidate *cb = (const SelfJoinCandidate *) b;

	if (ca->reloid != cb->reloid)
		return (ca->reloid < cb->reloid ? -1 : 1);
	else
		return 0;
}

/*
 * Find and remove useless self joins.
 *
 * Search for joins where a relation is joined to itself. If the join clause
 * for each tuple from one side of the join is proven to match the same
 * physical row (or nothing) on the other side, that self-join can be
 * eliminated from the query.  Suitable join clauses are assumed to be in the
 * form of X = X, and can be replaced with NOT NULL clauses.
 *
 * For the sake of simplicity, we don't apply this optimization to special
 * joins. Here is a list of what we could do in some particular cases:
 * 'a a1 semi join a a2': is reduced to inner by reduce_unique_semijoins,
 * and then removed normally.
 * 'a a1 anti join a a2': could simplify to a scan with 'outer quals AND
 * (IS NULL on join columns OR NOT inner quals)'.
 * 'a a1 left join a a2': could simplify to a scan like inner but without
 * NOT NULL conditions on join columns.
 * 'a a1 left join (a a2 join b)': can't simplify this, because join to b
 * can both remove rows and introduce duplicates.
 *
 * To search for removable joins, we order all the relations on their Oid,
 * go over each set with the same Oid, and consider each pair of relations
 * in this set.
 *
 * To remove the join, we delete one of the participating relations from the
 * query's jointree and rewrite all references to it to point to the remaining
 * relation.  We also have to modify their row marks.
 *
 * 'joinlist' is the top-level joinlist of the query; we use it to identify
 * groups of relations that could be joined to each other.
 *
 * We return true if we removed any self-joins.  If so, the caller must
 * recompute everything that was derived from the jointree, and should then
 * try join simplifications again since we might have exposed opportunities
 * for additional simplifications.
 */
bool
remove_useless_self_joins(PlannerInfo *root, List *joinlist)
{
	/* Skip if SJE is disabled, or if the joinlist has less than 2 members. */
	if (!enable_self_join_elimination || joinlist == NIL ||
		(list_length(joinlist) == 1 && !IsA(linitial(joinlist), List)))
		return false;

	/* Try to merge pairs of self-joined relations. */
	return remove_self_joins_recurse(root, joinlist);
}
