/*-------------------------------------------------------------------------
 *
 * prepjointree.c
 *	  Planner preprocessing for subqueries and join tree manipulation.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/prep/prepjointree.c,v 1.4 2003/02/09 00:35:55 tgl Exp $
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


/* These parameters are set by GUC */
int		from_collapse_limit;
int		join_collapse_limit;


static bool is_simple_subquery(Query *subquery);
static bool has_nullable_targetlist(Query *subquery);
static void resolvenew_in_jointree(Node *jtnode, int varno, List *subtlist);
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
pull_up_IN_clauses(Query *parse, Node *node)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, SubLink))
	{
		SubLink	   *sublink = (SubLink *) node;
		Node	   *subst;

		/* Is it a convertible IN clause?  If not, return it as-is */
		subst = convert_IN_to_join(parse, sublink);
		if (subst == NULL)
			return node;
		return subst;
	}
	if (and_clause(node))
	{
		List   *newclauses = NIL;
		List   *oldclauses;

		foreach(oldclauses, ((BoolExpr *) node)->args)
		{
			Node   *oldclause = lfirst(oldclauses);

			newclauses = lappend(newclauses,
								 pull_up_IN_clauses(parse,
													oldclause));
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
pull_up_subqueries(Query *parse, Node *jtnode, bool below_outer_join)
{
	if (jtnode == NULL)
		return NULL;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(varno, parse->rtable);
		Query	   *subquery = rte->subquery;

		/*
		 * Is this a subquery RTE, and if so, is the subquery simple
		 * enough to pull up?  (If not, do nothing at this node.)
		 *
		 * If we are inside an outer join, only pull up subqueries whose
		 * targetlists are nullable --- otherwise substituting their tlist
		 * entries for upper Var references would do the wrong thing (the
		 * results wouldn't become NULL when they're supposed to). XXX
		 * This could be improved by generating pseudo-variables for such
		 * expressions; we'd have to figure out how to get the pseudo-
		 * variables evaluated at the right place in the modified plan
		 * tree. Fix it someday.
		 *
		 * Note: even if the subquery itself is simple enough, we can't pull
		 * it up if there is a reference to its whole tuple result.
		 * Perhaps a pseudo-variable is the answer here too.
		 */
		if (rte->rtekind == RTE_SUBQUERY && is_simple_subquery(subquery) &&
			(!below_outer_join || has_nullable_targetlist(subquery)) &&
			!contain_whole_tuple_var((Node *) parse, varno, 0))
		{
			int			rtoffset;
			List	   *subtlist;
			List	   *rt;

			/*
			 * First, pull up any IN clauses within the subquery's WHERE,
			 * so that we don't leave unoptimized INs behind.
			 */
			if (subquery->hasSubLinks)
				subquery->jointree->quals = pull_up_IN_clauses(subquery,
															   subquery->jointree->quals);

			/*
			 * Now, recursively pull up the subquery's subqueries, so
			 * that this routine's processing is complete for its jointree
			 * and rangetable.	NB: if the same subquery is referenced
			 * from multiple jointree items (which can't happen normally,
			 * but might after rule rewriting), then we will invoke this
			 * processing multiple times on that subquery.	OK because
			 * nothing will happen after the first time.  We do have to be
			 * careful to copy everything we pull up, however, or risk
			 * having chunks of structure multiply linked.
			 *
			 * Note: 'false' is correct here even if we are within an outer
			 * join in the upper query; the lower query starts with a clean
			 * slate for outer-join semantics.
			 */
			subquery->jointree = (FromExpr *)
				pull_up_subqueries(subquery, (Node *) subquery->jointree,
								   false);

			/*
			 * Now make a modifiable copy of the subquery that we can run
			 * OffsetVarNodes and IncrementVarSublevelsUp on.
			 */
			subquery = copyObject(subquery);

			/*
			 * Adjust level-0 varnos in subquery so that we can append its
			 * rangetable to upper query's.
			 */
			rtoffset = length(parse->rtable);
			OffsetVarNodes((Node *) subquery, rtoffset, 0);

			/*
			 * Upper-level vars in subquery are now one level closer to their
			 * parent than before.
			 */
			IncrementVarSublevelsUp((Node *) subquery, -1, 1);

			/*
			 * Replace all of the top query's references to the subquery's
			 * outputs with copies of the adjusted subtlist items, being
			 * careful not to replace any of the jointree structure.
			 * (This'd be a lot cleaner if we could use
			 * query_tree_mutator.)
			 */
			subtlist = subquery->targetList;
			parse->targetList = (List *)
				ResolveNew((Node *) parse->targetList,
						   varno, 0, subtlist, CMD_SELECT, 0);
			resolvenew_in_jointree((Node *) parse->jointree, varno, subtlist);
			Assert(parse->setOperations == NULL);
			parse->havingQual =
				ResolveNew(parse->havingQual,
						   varno, 0, subtlist, CMD_SELECT, 0);
			parse->in_info_list = (List *)
				ResolveNew((Node *) parse->in_info_list,
						   varno, 0, subtlist, CMD_SELECT, 0);

			foreach(rt, parse->rtable)
			{
				RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

				if (rte->rtekind == RTE_JOIN)
					rte->joinaliasvars = (List *)
						ResolveNew((Node *) rte->joinaliasvars,
								   varno, 0, subtlist, CMD_SELECT, 0);
			}

			/*
			 * Now append the adjusted rtable entries to upper query. (We
			 * hold off until after fixing the upper rtable entries; no
			 * point in running that code on the subquery ones too.)
			 */
			parse->rtable = nconc(parse->rtable, subquery->rtable);

			/*
			 * Pull up any FOR UPDATE markers, too.  (OffsetVarNodes
			 * already adjusted the marker values, so just nconc the
			 * list.)
			 */
			parse->rowMarks = nconc(parse->rowMarks, subquery->rowMarks);

			/*
			 * We also have to fix the relid sets of any parent InClauseInfo
			 * nodes.  (This could perhaps be done by ResolveNew, but it
			 * would clutter that routine's API unreasonably.)
			 */
			if (parse->in_info_list)
			{
				Relids	subrelids;

				subrelids = get_relids_in_jointree((Node *) subquery->jointree);
				fix_in_clause_relids(parse->in_info_list, varno, subrelids);
			}

			/*
			 * And now append any subquery InClauseInfos to our list.
			 */
			parse->in_info_list = nconc(parse->in_info_list,
										subquery->in_info_list);

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
		List	   *l;

		foreach(l, f->fromlist)
			lfirst(l) = pull_up_subqueries(parse, lfirst(l),
										   below_outer_join);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		/* Recurse, being careful to tell myself when inside outer join */
		switch (j->jointype)
		{
			case JOIN_INNER:
				j->larg = pull_up_subqueries(parse, j->larg,
											 below_outer_join);
				j->rarg = pull_up_subqueries(parse, j->rarg,
											 below_outer_join);
				break;
			case JOIN_LEFT:
				j->larg = pull_up_subqueries(parse, j->larg,
											 below_outer_join);
				j->rarg = pull_up_subqueries(parse, j->rarg,
											 true);
				break;
			case JOIN_FULL:
				j->larg = pull_up_subqueries(parse, j->larg,
											 true);
				j->rarg = pull_up_subqueries(parse, j->rarg,
											 true);
				break;
			case JOIN_RIGHT:
				j->larg = pull_up_subqueries(parse, j->larg,
											 true);
				j->rarg = pull_up_subqueries(parse, j->rarg,
											 below_outer_join);
				break;
			case JOIN_UNION:

				/*
				 * This is where we fail if upper levels of planner
				 * haven't rewritten UNION JOIN as an Append ...
				 */
				elog(ERROR, "UNION JOIN is not implemented yet");
				break;
			default:
				elog(ERROR, "pull_up_subqueries: unexpected join type %d",
					 j->jointype);
				break;
		}
	}
	else
		elog(ERROR, "pull_up_subqueries: unexpected node type %d",
			 nodeTag(jtnode));
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
		subquery->into != NULL ||
		subquery->isPortal)
		elog(ERROR, "is_simple_subquery: subquery is bogus");

	/*
	 * Can't currently pull up a query with setops. Maybe after querytree
	 * redesign...
	 */
	if (subquery->setOperations)
		return false;

	/*
	 * Can't pull up a subquery involving grouping, aggregation, sorting,
	 * or limiting.
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
	 * Don't pull up a subquery that has any set-returning functions in
	 * its targetlist.	Otherwise we might well wind up inserting
	 * set-returning functions into places where they mustn't go, such as
	 * quals of higher queries.
	 */
	if (expression_returns_set((Node *) subquery->targetList))
		return false;

	/*
	 * Hack: don't try to pull up a subquery with an empty jointree.
	 * query_planner() will correctly generate a Result plan for a
	 * jointree that's totally empty, but I don't think the right things
	 * happen if an empty FromExpr appears lower down in a jointree. Not
	 * worth working hard on this, just to collapse SubqueryScan/Result
	 * into Result...
	 */
	if (subquery->jointree->fromlist == NIL)
		return false;

	return true;
}

/*
 * has_nullable_targetlist
 *	  Check a subquery in the range table to see if all the non-junk
 *	  targetlist items are simple variables (and, hence, will correctly
 *	  go to NULL when examined above the point of an outer join).
 *
 * A possible future extension is to accept strict functions of simple
 * variables, eg, "x + 1".
 */
static bool
has_nullable_targetlist(Query *subquery)
{
	List	   *l;

	foreach(l, subquery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		/* ignore resjunk columns */
		if (tle->resdom->resjunk)
			continue;

		/* Okay if tlist item is a simple Var */
		if (tle->expr && IsA(tle->expr, Var))
			continue;

		return false;
	}
	return true;
}

/*
 * Helper routine for pull_up_subqueries: do ResolveNew on every expression
 * in the jointree, without changing the jointree structure itself.  Ugly,
 * but there's no other way...
 */
static void
resolvenew_in_jointree(Node *jtnode, int varno, List *subtlist)
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
		List	   *l;

		foreach(l, f->fromlist)
			resolvenew_in_jointree(lfirst(l), varno, subtlist);
		f->quals = ResolveNew(f->quals,
							  varno, 0, subtlist, CMD_SELECT, 0);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		resolvenew_in_jointree(j->larg, varno, subtlist);
		resolvenew_in_jointree(j->rarg, varno, subtlist);
		j->quals = ResolveNew(j->quals,
							  varno, 0, subtlist, CMD_SELECT, 0);

		/*
		 * We don't bother to update the colvars list, since it won't be
		 * used again ...
		 */
	}
	else
		elog(ERROR, "resolvenew_in_jointree: unexpected node type %d",
			 nodeTag(jtnode));
}

/*
 * preprocess_jointree
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
preprocess_jointree(Query *parse, Node *jtnode)
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
		List	   *l;

		foreach(l, f->fromlist)
		{
			Node	   *child = (Node *) lfirst(l);

			/* Recursively simplify this child... */
			child = preprocess_jointree(parse, child);
			/* Now, is it a FromExpr? */
			if (child && IsA(child, FromExpr))
			{
				/*
				 * Yes, so do we want to merge it into parent?	Always do
				 * so if child has just one element (since that doesn't
				 * make the parent's list any longer).  Otherwise merge if
				 * the resulting join list would be no longer than
				 * from_collapse_limit.
				 */
				FromExpr   *subf = (FromExpr *) child;
				int			childlen = length(subf->fromlist);
				int			myothers = length(newlist) + length(lnext(l));

				if (childlen <= 1 ||
					(childlen + myothers) <= from_collapse_limit)
				{
					newlist = nconc(newlist, subf->fromlist);
					/*
					 * By now, the quals have been converted to implicit-AND
					 * lists, so we just need to join the lists.  NOTE: we
					 * put the pulled-up quals first.
					 */
					f->quals = (Node *) nconc((List *) subf->quals,
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
		j->larg = preprocess_jointree(parse, j->larg);
		j->rarg = preprocess_jointree(parse, j->rarg);
		/*
		 * If it is an outer join, we must not flatten it.  An inner join
		 * is semantically equivalent to a FromExpr; we convert it to one,
		 * allowing it to be flattened into its parent, if the resulting
		 * FromExpr would have no more than join_collapse_limit members.
		 */
		if (j->jointype == JOIN_INNER && join_collapse_limit > 1)
		{
			int		leftlen,
					rightlen;

			if (j->larg && IsA(j->larg, FromExpr))
				leftlen = length(((FromExpr *) j->larg)->fromlist);
			else
				leftlen = 1;
			if (j->rarg && IsA(j->rarg, FromExpr))
				rightlen = length(((FromExpr *) j->rarg)->fromlist);
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
					f->fromlist = makeList1(j->larg);

				if (j->rarg && IsA(j->rarg, FromExpr))
				{
					FromExpr   *subf = (FromExpr *) j->rarg;

					f->fromlist = nconc(f->fromlist,
										subf->fromlist);
					f->quals = (Node *) nconc((List *) f->quals,
											  (List *) subf->quals);
				}
				else
					f->fromlist = lappend(f->fromlist, j->rarg);

				/* pulled-up quals first */
				f->quals = (Node *) nconc((List *) f->quals,
										  (List *) j->quals);

				return (Node *) f;
			}
		}
	}
	else
		elog(ERROR, "preprocess_jointree: unexpected node type %d",
			 nodeTag(jtnode));
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
	List	   *l;

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
		List	   *l;

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
		elog(ERROR, "get_relids_in_jointree: unexpected node type %d",
			 nodeTag(jtnode));
	return result;
}

/*
 * get_relids_for_join: get set of base RT indexes making up a join
 *
 * NB: this will not work reliably after preprocess_jointree() is run,
 * since that may eliminate join nodes from the jointree.
 */
Relids
get_relids_for_join(Query *parse, int joinrelid)
{
	Node	   *jtnode;

	jtnode = find_jointree_node_for_rel((Node *) parse->jointree, joinrelid);
	if (!jtnode)
		elog(ERROR, "get_relids_for_join: join node %d not found", joinrelid);
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
		List	   *l;

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
		elog(ERROR, "find_jointree_node_for_rel: unexpected node type %d",
			 nodeTag(jtnode));
	return NULL;
}
