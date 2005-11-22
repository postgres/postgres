/*-------------------------------------------------------------------------
 *
 * prepjointree.c
 *	  Planner preprocessing for subqueries and join tree manipulation.
 *
 * NOTE: the intended sequence for invoking these operations is
 *		pull_up_IN_clauses
 *		pull_up_subqueries
 *		do expression preprocessing (including flattening JOIN alias vars)
 *		reduce_outer_joins
 *		simplify_jointree
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/prep/prepjointree.c,v 1.31.2.1 2005/11/22 18:23:11 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"


/* These parameters are set by GUC */
int			from_collapse_limit;
int			join_collapse_limit;


typedef struct reduce_outer_joins_state
{
	Relids		relids;			/* base relids within this subtree */
	bool		contains_outer; /* does subtree contain outer join(s)? */
	List	   *sub_states;		/* List of states for subtree components */
} reduce_outer_joins_state;

static bool is_simple_subquery(Query *subquery);
static bool has_nullable_targetlist(Query *subquery);
static void resolvenew_in_jointree(Node *jtnode, int varno,
					   RangeTblEntry *rte, List *subtlist);
static reduce_outer_joins_state *reduce_outer_joins_pass1(Node *jtnode);
static void reduce_outer_joins_pass2(Node *jtnode,
						 reduce_outer_joins_state *state,
						 PlannerInfo *root,
						 Relids nonnullable_rels);
static Relids find_nonnullable_rels(Node *node, bool top_level);
static void fix_in_clause_relids(List *in_info_list, int varno,
					 Relids subrelids);
static Node *find_jointree_node_for_rel(Node *jtnode, int relid);


/*
 * pull_up_IN_clauses
 *		Attempt to pull up top-level IN clauses to be treated like joins.
 *
 * A clause "foo IN (sub-SELECT)" appearing at the top level of WHERE can
 * be processed by pulling the sub-SELECT up to become a rangetable entry
 * and handling the implied equality comparisons as join operators (with
 * special join rules).
 * This optimization *only* works at the top level of WHERE, because
 * it cannot distinguish whether the IN ought to return FALSE or NULL in
 * cases involving NULL inputs.  This routine searches for such clauses
 * and does the necessary parsetree transformations if any are found.
 *
 * This routine has to run before preprocess_expression(), so the WHERE
 * clause is not yet reduced to implicit-AND format.  That means we need
 * to recursively search through explicit AND clauses, which are
 * probably only binary ANDs.  We stop as soon as we hit a non-AND item.
 *
 * Returns the possibly-modified version of the given qual-tree node.
 */
Node *
pull_up_IN_clauses(PlannerInfo *root, Node *node)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		Node	   *subst;

		/* Is it a convertible IN clause?  If not, return it as-is */
		subst = convert_IN_to_join(root, sublink);
		if (subst == NULL)
			return node;
		return subst;
	}
	if (and_clause(node))
	{
		List	   *newclauses = NIL;
		ListCell   *l;

		foreach(l, ((BoolExpr *) node)->args)
		{
			Node	   *oldclause = (Node *) lfirst(l);

			newclauses = lappend(newclauses,
								 pull_up_IN_clauses(root, oldclause));
		}
		return (Node *) make_andclause(newclauses);
	}
	/* Stop if not an AND */
	return node;
}

/*
 * pull_up_subqueries
 *		Look for subqueries in the rangetable that can be pulled up into
 *		the parent query.  If the subquery has no special features like
 *		grouping/aggregation then we can merge it into the parent's jointree.
 *
 * below_outer_join is true if this jointree node is within the nullable
 * side of an outer join.  This restricts what we can do.
 *
 * A tricky aspect of this code is that if we pull up a subquery we have
 * to replace Vars that reference the subquery's outputs throughout the
 * parent query, including quals attached to jointree nodes above the one
 * we are currently processing!  We handle this by being careful not to
 * change the jointree structure while recursing: no nodes other than
 * subquery RangeTblRef entries will be replaced.  Also, we can't turn
 * ResolveNew loose on the whole jointree, because it'll return a mutated
 * copy of the tree; we have to invoke it just on the quals, instead.
 */
Node *
pull_up_subqueries(PlannerInfo *root, Node *jtnode, bool below_outer_join)
{
	if (jtnode == NULL)
		return NULL;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;
		Query	   *parse = root->parse;
		RangeTblEntry *rte = rt_fetch(varno, parse->rtable);
		Query	   *subquery = rte->subquery;

		/*
		 * Is this a subquery RTE, and if so, is the subquery simple enough to
		 * pull up?  (If not, do nothing at this node.)
		 *
		 * If we are inside an outer join, only pull up subqueries whose
		 * targetlists are nullable --- otherwise substituting their tlist
		 * entries for upper Var references would do the wrong thing (the
		 * results wouldn't become NULL when they're supposed to).
		 *
		 * XXX This could be improved by generating pseudo-variables for such
		 * expressions; we'd have to figure out how to get the pseudo-
		 * variables evaluated at the right place in the modified plan tree.
		 * Fix it someday.
		 */
		if (rte->rtekind == RTE_SUBQUERY &&
			is_simple_subquery(subquery) &&
			(!below_outer_join || has_nullable_targetlist(subquery)))
		{
			PlannerInfo *subroot;
			int			rtoffset;
			List	   *subtlist;
			ListCell   *rt;

			/*
			 * Need a modifiable copy of the subquery to hack on.  Even if we
			 * didn't sometimes choose not to pull up below, we must do this
			 * to avoid problems if the same subquery is referenced from
			 * multiple jointree items (which can't happen normally, but might
			 * after rule rewriting).
			 */
			subquery = copyObject(subquery);

			/*
			 * Create a PlannerInfo data structure for this subquery.
			 *
			 * NOTE: the next few steps should match the first processing in
			 * subquery_planner().	Can we refactor to avoid code duplication,
			 * or would that just make things uglier?
			 */
			subroot = makeNode(PlannerInfo);
			subroot->parse = subquery;

			/*
			 * Pull up any IN clauses within the subquery's WHERE, so that we
			 * don't leave unoptimized INs behind.
			 */
			subroot->in_info_list = NIL;
			if (subquery->hasSubLinks)
				subquery->jointree->quals = pull_up_IN_clauses(subroot,
												  subquery->jointree->quals);

			/*
			 * Recursively pull up the subquery's subqueries, so that this
			 * routine's processing is complete for its jointree and
			 * rangetable.
			 *
			 * Note: 'false' is correct here even if we are within an outer
			 * join in the upper query; the lower query starts with a clean
			 * slate for outer-join semantics.
			 */
			subquery->jointree = (FromExpr *)
				pull_up_subqueries(subroot, (Node *) subquery->jointree,
								   false);

			/*
			 * Now we must recheck whether the subquery is still simple enough
			 * to pull up.	If not, abandon processing it.
			 *
			 * We don't really need to recheck all the conditions involved,
			 * but it's easier just to keep this "if" looking the same as the
			 * one above.
			 */
			if (is_simple_subquery(subquery) &&
				(!below_outer_join || has_nullable_targetlist(subquery)))
			{
				/* good to go */
			}
			else
			{
				/*
				 * Give up, return unmodified RangeTblRef.
				 *
				 * Note: The work we just did will be redone when the subquery
				 * gets planned on its own.  Perhaps we could avoid that by
				 * storing the modified subquery back into the rangetable, but
				 * I'm not gonna risk it now.
				 */
				return jtnode;
			}

			/*
			 * Adjust level-0 varnos in subquery so that we can append its
			 * rangetable to upper query's.  We have to fix the subquery's
			 * in_info_list, as well.
			 */
			rtoffset = list_length(parse->rtable);
			OffsetVarNodes((Node *) subquery, rtoffset, 0);
			OffsetVarNodes((Node *) subroot->in_info_list, rtoffset, 0);

			/*
			 * Upper-level vars in subquery are now one level closer to their
			 * parent than before.
			 */
			IncrementVarSublevelsUp((Node *) subquery, -1, 1);
			IncrementVarSublevelsUp((Node *) subroot->in_info_list, -1, 1);

			/*
			 * Replace all of the top query's references to the subquery's
			 * outputs with copies of the adjusted subtlist items, being
			 * careful not to replace any of the jointree structure. (This'd
			 * be a lot cleaner if we could use query_tree_mutator.)
			 */
			subtlist = subquery->targetList;
			parse->targetList = (List *)
				ResolveNew((Node *) parse->targetList,
						   varno, 0, rte,
						   subtlist, CMD_SELECT, 0);
			resolvenew_in_jointree((Node *) parse->jointree, varno,
								   rte, subtlist);
			Assert(parse->setOperations == NULL);
			parse->havingQual =
				ResolveNew(parse->havingQual,
						   varno, 0, rte,
						   subtlist, CMD_SELECT, 0);
			root->in_info_list = (List *)
				ResolveNew((Node *) root->in_info_list,
						   varno, 0, rte,
						   subtlist, CMD_SELECT, 0);

			foreach(rt, parse->rtable)
			{
				RangeTblEntry *otherrte = (RangeTblEntry *) lfirst(rt);

				if (otherrte->rtekind == RTE_JOIN)
					otherrte->joinaliasvars = (List *)
						ResolveNew((Node *) otherrte->joinaliasvars,
								   varno, 0, rte,
								   subtlist, CMD_SELECT, 0);
			}

			/*
			 * Now append the adjusted rtable entries to upper query. (We hold
			 * off until after fixing the upper rtable entries; no point in
			 * running that code on the subquery ones too.)
			 */
			parse->rtable = list_concat(parse->rtable, subquery->rtable);

			/*
			 * Pull up any FOR UPDATE/SHARE markers, too.  (OffsetVarNodes
			 * already adjusted the marker values, so just list_concat the
			 * list.)
			 *
			 * Executor can't handle multiple FOR UPDATE/SHARE/NOWAIT flags,
			 * so complain if they are valid but different
			 */
			if (parse->rowMarks && subquery->rowMarks)
			{
				if (parse->forUpdate != subquery->forUpdate)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot use both FOR UPDATE and FOR SHARE in one query")));
				if (parse->rowNoWait != subquery->rowNoWait)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot use both wait and NOWAIT in one query")));
			}
			parse->rowMarks = list_concat(parse->rowMarks, subquery->rowMarks);
			if (subquery->rowMarks)
			{
				parse->forUpdate = subquery->forUpdate;
				parse->rowNoWait = subquery->rowNoWait;
			}

			/*
			 * We also have to fix the relid sets of any parent InClauseInfo
			 * nodes.  (This could perhaps be done by ResolveNew, but it would
			 * clutter that routine's API unreasonably.)
			 */
			if (root->in_info_list)
			{
				Relids		subrelids;

				subrelids = get_relids_in_jointree((Node *) subquery->jointree);
				fix_in_clause_relids(root->in_info_list, varno, subrelids);
			}

			/*
			 * And now append any subquery InClauseInfos to our list.
			 */
			root->in_info_list = list_concat(root->in_info_list,
											 subroot->in_info_list);

			/*
			 * Miscellaneous housekeeping.
			 */
			parse->hasSubLinks |= subquery->hasSubLinks;
			/* subquery won't be pulled up if it hasAggs, so no work there */

			/*
			 * Return the adjusted subquery jointree to replace the
			 * RangeTblRef entry in my jointree.
			 */
			return (Node *) subquery->jointree;
		}
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			lfirst(l) = pull_up_subqueries(root, lfirst(l),
										   below_outer_join);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		/* Recurse, being careful to tell myself when inside outer join */
		switch (j->jointype)
		{
			case JOIN_INNER:
				j->larg = pull_up_subqueries(root, j->larg,
											 below_outer_join);
				j->rarg = pull_up_subqueries(root, j->rarg,
											 below_outer_join);
				break;
			case JOIN_LEFT:
				j->larg = pull_up_subqueries(root, j->larg,
											 below_outer_join);
				j->rarg = pull_up_subqueries(root, j->rarg,
											 true);
				break;
			case JOIN_FULL:
				j->larg = pull_up_subqueries(root, j->larg,
											 true);
				j->rarg = pull_up_subqueries(root, j->rarg,
											 true);
				break;
			case JOIN_RIGHT:
				j->larg = pull_up_subqueries(root, j->larg,
											 true);
				j->rarg = pull_up_subqueries(root, j->rarg,
											 below_outer_join);
				break;
			case JOIN_UNION:

				/*
				 * This is where we fail if upper levels of planner haven't
				 * rewritten UNION JOIN as an Append ...
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("UNION JOIN is not implemented")));
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				break;
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return jtnode;
}

/*
 * is_simple_subquery
 *	  Check a subquery in the range table to see if it's simple enough
 *	  to pull up into the parent query.
 */
static bool
is_simple_subquery(Query *subquery)
{
	/*
	 * Let's just make sure it's a valid subselect ...
	 */
	if (!IsA(subquery, Query) ||
		subquery->commandType != CMD_SELECT ||
		subquery->resultRelation != 0 ||
		subquery->into != NULL)
		elog(ERROR, "subquery is bogus");

	/*
	 * Can't currently pull up a query with setops. Maybe after querytree
	 * redesign...
	 */
	if (subquery->setOperations)
		return false;

	/*
	 * Can't pull up a subquery involving grouping, aggregation, sorting, or
	 * limiting.
	 */
	if (subquery->hasAggs ||
		subquery->groupClause ||
		subquery->havingQual ||
		subquery->sortClause ||
		subquery->distinctClause ||
		subquery->limitOffset ||
		subquery->limitCount)
		return false;

	/*
	 * Don't pull up a subquery that has any set-returning functions in its
	 * targetlist.	Otherwise we might well wind up inserting set-returning
	 * functions into places where they mustn't go, such as quals of higher
	 * queries.
	 */
	if (expression_returns_set((Node *) subquery->targetList))
		return false;

	/*
	 * Hack: don't try to pull up a subquery with an empty jointree.
	 * query_planner() will correctly generate a Result plan for a jointree
	 * that's totally empty, but I don't think the right things happen if an
	 * empty FromExpr appears lower down in a jointree. Not worth working hard
	 * on this, just to collapse SubqueryScan/Result into Result...
	 */
	if (subquery->jointree->fromlist == NIL)
		return false;

	return true;
}

/*
 * has_nullable_targetlist
 *	  Check a subquery in the range table to see if all the non-junk
 *	  targetlist items are simple variables or strict functions of simple
 *	  variables (and, hence, will correctly go to NULL when examined above
 *	  the point of an outer join).
 *
 * NOTE: it would be correct (and useful) to ignore output columns that aren't
 * actually referenced by the enclosing query ... but we do not have that
 * information available at this point.
 */
static bool
has_nullable_targetlist(Query *subquery)
{
	ListCell   *l;

	foreach(l, subquery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		/* ignore resjunk columns */
		if (tle->resjunk)
			continue;

		/* Must contain a Var of current level */
		if (!contain_vars_of_level((Node *) tle->expr, 0))
			return false;

		/* Must not contain any non-strict constructs */
		if (contain_nonstrict_functions((Node *) tle->expr))
			return false;

		/* This one's OK, keep scanning */
	}
	return true;
}

/*
 * Helper routine for pull_up_subqueries: do ResolveNew on every expression
 * in the jointree, without changing the jointree structure itself.  Ugly,
 * but there's no other way...
 */
static void
resolvenew_in_jointree(Node *jtnode, int varno,
					   RangeTblEntry *rte, List *subtlist)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		/* nothing to do here */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			resolvenew_in_jointree(lfirst(l), varno, rte, subtlist);
		f->quals = ResolveNew(f->quals,
							  varno, 0, rte,
							  subtlist, CMD_SELECT, 0);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		resolvenew_in_jointree(j->larg, varno, rte, subtlist);
		resolvenew_in_jointree(j->rarg, varno, rte, subtlist);
		j->quals = ResolveNew(j->quals,
							  varno, 0, rte,
							  subtlist, CMD_SELECT, 0);

		/*
		 * We don't bother to update the colvars list, since it won't be used
		 * again ...
		 */
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * reduce_outer_joins
 *		Attempt to reduce outer joins to plain inner joins.
 *
 * The idea here is that given a query like
 *		SELECT ... FROM a LEFT JOIN b ON (...) WHERE b.y = 42;
 * we can reduce the LEFT JOIN to a plain JOIN if the "=" operator in WHERE
 * is strict.  The strict operator will always return NULL, causing the outer
 * WHERE to fail, on any row where the LEFT JOIN filled in NULLs for b's
 * columns.  Therefore, there's no need for the join to produce null-extended
 * rows in the first place --- which makes it a plain join not an outer join.
 * (This scenario may not be very likely in a query written out by hand, but
 * it's reasonably likely when pushing quals down into complex views.)
 *
 * More generally, an outer join can be reduced in strength if there is a
 * strict qual above it in the qual tree that constrains a Var from the
 * nullable side of the join to be non-null.  (For FULL joins this applies
 * to each side separately.)
 *
 * To ease recognition of strict qual clauses, we require this routine to be
 * run after expression preprocessing (i.e., qual canonicalization and JOIN
 * alias-var expansion).
 */
void
reduce_outer_joins(PlannerInfo *root)
{
	reduce_outer_joins_state *state;

	/*
	 * To avoid doing strictness checks on more quals than necessary, we want
	 * to stop descending the jointree as soon as there are no outer joins
	 * below our current point.  This consideration forces a two-pass process.
	 * The first pass gathers information about which base rels appear below
	 * each side of each join clause, and about whether there are outer
	 * join(s) below each side of each join clause. The second pass examines
	 * qual clauses and changes join types as it descends the tree.
	 */
	state = reduce_outer_joins_pass1((Node *) root->parse->jointree);

	/* planner.c shouldn't have called me if no outer joins */
	if (state == NULL || !state->contains_outer)
		elog(ERROR, "so where are the outer joins?");

	reduce_outer_joins_pass2((Node *) root->parse->jointree,
							 state, root, NULL);
}

/*
 * reduce_outer_joins_pass1 - phase 1 data collection
 *
 * Returns a state node describing the given jointree node.
 */
static reduce_outer_joins_state *
reduce_outer_joins_pass1(Node *jtnode)
{
	reduce_outer_joins_state *result;

	result = (reduce_outer_joins_state *)
		palloc(sizeof(reduce_outer_joins_state));
	result->relids = NULL;
	result->contains_outer = false;
	result->sub_states = NIL;

	if (jtnode == NULL)
		return result;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		result->relids = bms_make_singleton(varno);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
		{
			reduce_outer_joins_state *sub_state;

			sub_state = reduce_outer_joins_pass1(lfirst(l));
			result->relids = bms_add_members(result->relids,
											 sub_state->relids);
			result->contains_outer |= sub_state->contains_outer;
			result->sub_states = lappend(result->sub_states, sub_state);
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		reduce_outer_joins_state *sub_state;

		/* join's own RT index is not wanted in result->relids */
		if (IS_OUTER_JOIN(j->jointype))
			result->contains_outer = true;

		sub_state = reduce_outer_joins_pass1(j->larg);
		result->relids = bms_add_members(result->relids,
										 sub_state->relids);
		result->contains_outer |= sub_state->contains_outer;
		result->sub_states = lappend(result->sub_states, sub_state);

		sub_state = reduce_outer_joins_pass1(j->rarg);
		result->relids = bms_add_members(result->relids,
										 sub_state->relids);
		result->contains_outer |= sub_state->contains_outer;
		result->sub_states = lappend(result->sub_states, sub_state);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return result;
}

/*
 * reduce_outer_joins_pass2 - phase 2 processing
 *
 *	jtnode: current jointree node
 *	state: state data collected by phase 1 for this node
 *	root: toplevel planner state
 *	nonnullable_rels: set of base relids forced non-null by upper quals
 */
static void
reduce_outer_joins_pass2(Node *jtnode,
						 reduce_outer_joins_state *state,
						 PlannerInfo *root,
						 Relids nonnullable_rels)
{
	/*
	 * pass 2 should never descend as far as an empty subnode or base rel,
	 * because it's only called on subtrees marked as contains_outer.
	 */
	if (jtnode == NULL)
		elog(ERROR, "reached empty jointree");
	if (IsA(jtnode, RangeTblRef))
		elog(ERROR, "reached base rel");
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;
		ListCell   *s;
		Relids		pass_nonnullable;

		/* Scan quals to see if we can add any nonnullability constraints */
		pass_nonnullable = find_nonnullable_rels(f->quals, true);
		pass_nonnullable = bms_add_members(pass_nonnullable,
										   nonnullable_rels);
		/* And recurse --- but only into interesting subtrees */
		Assert(list_length(f->fromlist) == list_length(state->sub_states));
		forboth(l, f->fromlist, s, state->sub_states)
		{
			reduce_outer_joins_state *sub_state = lfirst(s);

			if (sub_state->contains_outer)
				reduce_outer_joins_pass2(lfirst(l), sub_state, root,
										 pass_nonnullable);
		}
		bms_free(pass_nonnullable);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		int			rtindex = j->rtindex;
		JoinType	jointype = j->jointype;
		reduce_outer_joins_state *left_state = linitial(state->sub_states);
		reduce_outer_joins_state *right_state = lsecond(state->sub_states);

		/* Can we simplify this join? */
		switch (jointype)
		{
			case JOIN_LEFT:
				if (bms_overlap(nonnullable_rels, right_state->relids))
					jointype = JOIN_INNER;
				break;
			case JOIN_RIGHT:
				if (bms_overlap(nonnullable_rels, left_state->relids))
					jointype = JOIN_INNER;
				break;
			case JOIN_FULL:
				if (bms_overlap(nonnullable_rels, left_state->relids))
				{
					if (bms_overlap(nonnullable_rels, right_state->relids))
						jointype = JOIN_INNER;
					else
						jointype = JOIN_LEFT;
				}
				else
				{
					if (bms_overlap(nonnullable_rels, right_state->relids))
						jointype = JOIN_RIGHT;
				}
				break;
			default:
				break;
		}
		if (jointype != j->jointype)
		{
			/* apply the change to both jointree node and RTE */
			RangeTblEntry *rte = rt_fetch(rtindex, root->parse->rtable);

			Assert(rte->rtekind == RTE_JOIN);
			Assert(rte->jointype == j->jointype);
			rte->jointype = j->jointype = jointype;
		}

		/* Only recurse if there's more to do below here */
		if (left_state->contains_outer || right_state->contains_outer)
		{
			Relids		local_nonnullable;
			Relids		pass_nonnullable;

			/*
			 * If this join is (now) inner, we can add any nonnullability
			 * constraints its quals provide to those we got from above. But
			 * if it is outer, we can only pass down the local constraints
			 * into the nullable side, because an outer join never eliminates
			 * any rows from its non-nullable side.  If it's a FULL join then
			 * it doesn't eliminate anything from either side.
			 */
			if (jointype != JOIN_FULL)
			{
				local_nonnullable = find_nonnullable_rels(j->quals, true);
				local_nonnullable = bms_add_members(local_nonnullable,
													nonnullable_rels);
			}
			else
				local_nonnullable = NULL;		/* no use in calculating it */

			if (left_state->contains_outer)
			{
				if (jointype == JOIN_INNER || jointype == JOIN_RIGHT)
					pass_nonnullable = local_nonnullable;
				else
					pass_nonnullable = nonnullable_rels;
				reduce_outer_joins_pass2(j->larg, left_state, root,
										 pass_nonnullable);
			}
			if (right_state->contains_outer)
			{
				if (jointype == JOIN_INNER || jointype == JOIN_LEFT)
					pass_nonnullable = local_nonnullable;
				else
					pass_nonnullable = nonnullable_rels;
				reduce_outer_joins_pass2(j->rarg, right_state, root,
										 pass_nonnullable);
			}
			bms_free(local_nonnullable);
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * find_nonnullable_rels
 *		Determine which base rels are forced nonnullable by given quals
 *
 * We don't use expression_tree_walker here because we don't want to
 * descend through very many kinds of nodes; only the ones we can be sure
 * are strict.	We can descend through the top level of implicit AND'ing,
 * but not through any explicit ANDs (or ORs) below that, since those are not
 * strict constructs.  The List case handles the top-level implicit AND list
 * as well as lists of arguments to strict operators/functions.
 */
static Relids
find_nonnullable_rels(Node *node, bool top_level)
{
	Relids		result = NULL;

	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varlevelsup == 0)
			result = bms_make_singleton(var->varno);
	}
	else if (IsA(node, List))
	{
		ListCell   *l;

		foreach(l, (List *) node)
		{
			result = bms_join(result, find_nonnullable_rels(lfirst(l),
															top_level));
		}
	}
	else if (IsA(node, FuncExpr))
	{
		FuncExpr   *expr = (FuncExpr *) node;

		if (func_strict(expr->funcid))
			result = find_nonnullable_rels((Node *) expr->args, false);
	}
	else if (IsA(node, OpExpr))
	{
		OpExpr	   *expr = (OpExpr *) node;

		if (op_strict(expr->opno))
			result = find_nonnullable_rels((Node *) expr->args, false);
	}
	else if (IsA(node, BoolExpr))
	{
		BoolExpr   *expr = (BoolExpr *) node;

		/* NOT is strict, others are not */
		if (expr->boolop == NOT_EXPR)
			result = find_nonnullable_rels((Node *) expr->args, false);
	}
	else if (IsA(node, RelabelType))
	{
		RelabelType *expr = (RelabelType *) node;

		result = find_nonnullable_rels((Node *) expr->arg, top_level);
	}
	else if (IsA(node, ConvertRowtypeExpr))
	{
		/* not clear this is useful, but it can't hurt */
		ConvertRowtypeExpr *expr = (ConvertRowtypeExpr *) node;

		result = find_nonnullable_rels((Node *) expr->arg, top_level);
	}
	else if (IsA(node, NullTest))
	{
		NullTest   *expr = (NullTest *) node;

		/*
		 * IS NOT NULL can be considered strict, but only at top level; else
		 * we might have something like NOT (x IS NOT NULL).
		 */
		if (top_level && expr->nulltesttype == IS_NOT_NULL)
			result = find_nonnullable_rels((Node *) expr->arg, false);
	}
	else if (IsA(node, BooleanTest))
	{
		BooleanTest *expr = (BooleanTest *) node;

		/*
		 * Appropriate boolean tests are strict at top level.
		 */
		if (top_level &&
			(expr->booltesttype == IS_TRUE ||
			 expr->booltesttype == IS_FALSE ||
			 expr->booltesttype == IS_NOT_UNKNOWN))
			result = find_nonnullable_rels((Node *) expr->arg, false);
	}
	return result;
}

/*
 * simplify_jointree
 *		Attempt to simplify a query's jointree.
 *
 * If we succeed in pulling up a subquery then we might form a jointree
 * in which a FromExpr is a direct child of another FromExpr.  In that
 * case we can consider collapsing the two FromExprs into one.	This is
 * an optional conversion, since the planner will work correctly either
 * way.  But we may find a better plan (at the cost of more planning time)
 * if we merge the two nodes, creating a single join search space out of
 * two.  To allow the user to trade off planning time against plan quality,
 * we provide a control parameter from_collapse_limit that limits the size
 * of the join search space that can be created this way.
 *
 * We also consider flattening explicit inner JOINs into FromExprs (which
 * will in turn allow them to be merged into parent FromExprs).  The tradeoffs
 * here are the same as for flattening FromExprs, but we use a different
 * control parameter so that the user can use explicit JOINs to control the
 * join order even when they are inner JOINs.
 *
 * NOTE: don't try to do this in the same jointree scan that does subquery
 * pullup!	Since we're changing the jointree structure here, that wouldn't
 * work reliably --- see comments for pull_up_subqueries().
 */
Node *
simplify_jointree(PlannerInfo *root, Node *jtnode)
{
	if (jtnode == NULL)
		return NULL;
	if (IsA(jtnode, RangeTblRef))
	{
		/* nothing to do here... */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		List	   *newlist = NIL;
		int			children_remaining;
		ListCell   *l;

		children_remaining = list_length(f->fromlist);
		foreach(l, f->fromlist)
		{
			Node	   *child = (Node *) lfirst(l);

			children_remaining--;
			/* Recursively simplify this child... */
			child = simplify_jointree(root, child);
			/* Now, is it a FromExpr? */
			if (child && IsA(child, FromExpr))
			{
				/*
				 * Yes, so do we want to merge it into parent?	Always do so
				 * if child has just one element (since that doesn't make the
				 * parent's list any longer).  Otherwise merge if the
				 * resulting join list would be no longer than
				 * from_collapse_limit.
				 */
				FromExpr   *subf = (FromExpr *) child;
				int			childlen = list_length(subf->fromlist);
				int			myothers = list_length(newlist) + children_remaining;

				if (childlen <= 1 ||
					(childlen + myothers) <= from_collapse_limit)
				{
					newlist = list_concat(newlist, subf->fromlist);

					/*
					 * By now, the quals have been converted to implicit-AND
					 * lists, so we just need to join the lists.  NOTE: we put
					 * the pulled-up quals first.
					 */
					f->quals = (Node *) list_concat((List *) subf->quals,
													(List *) f->quals);
				}
				else
					newlist = lappend(newlist, child);
			}
			else
				newlist = lappend(newlist, child);
		}
		f->fromlist = newlist;
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		/* Recursively simplify the children... */
		j->larg = simplify_jointree(root, j->larg);
		j->rarg = simplify_jointree(root, j->rarg);

		/*
		 * If it is an outer join, we must not flatten it.	An inner join is
		 * semantically equivalent to a FromExpr; we convert it to one,
		 * allowing it to be flattened into its parent, if the resulting
		 * FromExpr would have no more than join_collapse_limit members.
		 */
		if (j->jointype == JOIN_INNER && join_collapse_limit > 1)
		{
			int			leftlen,
						rightlen;

			if (j->larg && IsA(j->larg, FromExpr))
				leftlen = list_length(((FromExpr *) j->larg)->fromlist);
			else
				leftlen = 1;
			if (j->rarg && IsA(j->rarg, FromExpr))
				rightlen = list_length(((FromExpr *) j->rarg)->fromlist);
			else
				rightlen = 1;
			if ((leftlen + rightlen) <= join_collapse_limit)
			{
				FromExpr   *f = makeNode(FromExpr);

				f->fromlist = NIL;
				f->quals = NULL;

				if (j->larg && IsA(j->larg, FromExpr))
				{
					FromExpr   *subf = (FromExpr *) j->larg;

					f->fromlist = subf->fromlist;
					f->quals = subf->quals;
				}
				else
					f->fromlist = list_make1(j->larg);

				if (j->rarg && IsA(j->rarg, FromExpr))
				{
					FromExpr   *subf = (FromExpr *) j->rarg;

					f->fromlist = list_concat(f->fromlist,
											  subf->fromlist);
					f->quals = (Node *) list_concat((List *) f->quals,
													(List *) subf->quals);
				}
				else
					f->fromlist = lappend(f->fromlist, j->rarg);

				/* pulled-up quals first */
				f->quals = (Node *) list_concat((List *) f->quals,
												(List *) j->quals);

				return (Node *) f;
			}
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return jtnode;
}

/*
 * fix_in_clause_relids: update RT-index sets of InClauseInfo nodes
 *
 * When we pull up a subquery, any InClauseInfo references to the subquery's
 * RT index have to be replaced by the set of substituted relids.
 *
 * We assume we may modify the InClauseInfo nodes in-place.
 */
static void
fix_in_clause_relids(List *in_info_list, int varno, Relids subrelids)
{
	ListCell   *l;

	foreach(l, in_info_list)
	{
		InClauseInfo *ininfo = (InClauseInfo *) lfirst(l);

		if (bms_is_member(varno, ininfo->lefthand))
		{
			ininfo->lefthand = bms_del_member(ininfo->lefthand, varno);
			ininfo->lefthand = bms_add_members(ininfo->lefthand, subrelids);
		}
		if (bms_is_member(varno, ininfo->righthand))
		{
			ininfo->righthand = bms_del_member(ininfo->righthand, varno);
			ininfo->righthand = bms_add_members(ininfo->righthand, subrelids);
		}
	}
}

/*
 * get_relids_in_jointree: get set of base RT indexes present in a jointree
 */
Relids
get_relids_in_jointree(Node *jtnode)
{
	Relids		result = NULL;

	if (jtnode == NULL)
		return result;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		result = bms_make_singleton(varno);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
		{
			result = bms_join(result,
							  get_relids_in_jointree(lfirst(l)));
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		/* join's own RT index is not wanted in result */
		result = get_relids_in_jointree(j->larg);
		result = bms_join(result, get_relids_in_jointree(j->rarg));
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return result;
}

/*
 * get_relids_for_join: get set of base RT indexes making up a join
 *
 * NB: this will not work reliably after simplify_jointree() is run,
 * since that may eliminate join nodes from the jointree.
 */
Relids
get_relids_for_join(PlannerInfo *root, int joinrelid)
{
	Node	   *jtnode;

	jtnode = find_jointree_node_for_rel((Node *) root->parse->jointree,
										joinrelid);
	if (!jtnode)
		elog(ERROR, "could not find join node %d", joinrelid);
	return get_relids_in_jointree(jtnode);
}

/*
 * find_jointree_node_for_rel: locate jointree node for a base or join RT index
 *
 * Returns NULL if not found
 */
static Node *
find_jointree_node_for_rel(Node *jtnode, int relid)
{
	if (jtnode == NULL)
		return NULL;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		if (relid == varno)
			return jtnode;
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
		{
			jtnode = find_jointree_node_for_rel(lfirst(l), relid);
			if (jtnode)
				return jtnode;
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		if (relid == j->rtindex)
			return jtnode;
		jtnode = find_jointree_node_for_rel(j->larg, relid);
		if (jtnode)
			return jtnode;
		jtnode = find_jointree_node_for_rel(j->rarg, relid);
		if (jtnode)
			return jtnode;
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return NULL;
}
