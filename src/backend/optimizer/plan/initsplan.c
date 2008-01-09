/*-------------------------------------------------------------------------
 *
 * initsplan.c
 *	  Target list, qualification, joininfo initialization routines
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/plan/initsplan.c,v 1.138 2008/01/09 20:42:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* These parameters are set by GUC */
int			from_collapse_limit;
int			join_collapse_limit;


static List *deconstruct_recurse(PlannerInfo *root, Node *jtnode,
					bool below_outer_join,
					Relids *qualscope, Relids *inner_join_rels);
static OuterJoinInfo *make_outerjoininfo(PlannerInfo *root,
				   Relids left_rels, Relids right_rels,
				   Relids inner_join_rels,
				   bool is_full_join, Node *clause);
static void distribute_qual_to_rels(PlannerInfo *root, Node *clause,
						bool is_deduced,
						bool below_outer_join,
						Relids qualscope,
						Relids ojscope,
						Relids outerjoin_nonnullable);
static bool check_outerjoin_delay(PlannerInfo *root, Relids *relids_p,
					  bool is_pushed_down);
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
 * jtnode.	Internally, the function recurses through the jointree.
 *
 * At the end of this process, there should be one baserel RelOptInfo for
 * every non-join RTE that is used in the query.  Therefore, this routine
 * is the only place that should call build_simple_rel with reloptkind
 * RELOPT_BASEREL.	(Note: build_simple_rel recurses internally to build
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
 *	  to the appropriate base relations.
 *
 * We mark such vars as needed by "relation 0" to ensure that they will
 * propagate up through all join plan steps.
 */
void
build_base_rel_tlists(PlannerInfo *root, List *final_tlist)
{
	List	   *tlist_vars = pull_var_clause((Node *) final_tlist, false);

	if (tlist_vars != NIL)
	{
		add_vars_to_targetlist(root, tlist_vars, bms_make_singleton(0));
		list_free(tlist_vars);
	}
}

/*
 * add_IN_vars_to_tlists
 *	  Add targetlist entries for each var needed in InClauseInfo entries
 *	  to the appropriate base relations.
 *
 * Normally this is a waste of time because scanning of the WHERE clause
 * will have added them.  But it is possible that eval_const_expressions()
 * simplified away all references to the vars after the InClauseInfos were
 * made.  We need the IN's righthand-side vars to be available at the join
 * anyway, in case we try to unique-ify the subselect's outputs.  (The only
 * known case that provokes this is "WHERE false AND foo IN (SELECT ...)".
 * We don't try to be very smart about such cases, just correct.)
 */
void
add_IN_vars_to_tlists(PlannerInfo *root)
{
	ListCell   *l;

	foreach(l, root->in_info_list)
	{
		InClauseInfo *ininfo = (InClauseInfo *) lfirst(l);
		List	   *in_vars;

		in_vars = pull_var_clause((Node *) ininfo->sub_targetlist, false);
		if (in_vars != NIL)
		{
			add_vars_to_targetlist(root, in_vars,
								   bms_union(ininfo->lefthand,
											 ininfo->righthand));
			list_free(in_vars);
		}
	}
}

/*
 * add_vars_to_targetlist
 *	  For each variable appearing in the list, add it to the owning
 *	  relation's targetlist if not already present, and mark the variable
 *	  as being needed for the indicated join (or for final output if
 *	  where_needed includes "relation 0").
 */
void
add_vars_to_targetlist(PlannerInfo *root, List *vars, Relids where_needed)
{
	ListCell   *temp;

	Assert(!bms_is_empty(where_needed));

	foreach(temp, vars)
	{
		Var		   *var = (Var *) lfirst(temp);
		RelOptInfo *rel = find_base_rel(root, var->varno);
		int			attrno = var->varattno;

		Assert(attrno >= rel->min_attr && attrno <= rel->max_attr);
		attrno -= rel->min_attr;
		if (bms_is_empty(rel->attr_needed[attrno]))
		{
			/* Variable not yet requested, so add to reltargetlist */
			/* XXX is copyObject necessary here? */
			rel->reltargetlist = lappend(rel->reltargetlist, copyObject(var));
		}
		rel->attr_needed[attrno] = bms_add_members(rel->attr_needed[attrno],
												   where_needed);
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
 *	  lists belonging to base RelOptInfos.	Also, add OuterJoinInfo nodes
 *	  to root->oj_info_list for any outer joins appearing in the query tree.
 *	  Return a "joinlist" data structure showing the join order decisions
 *	  that need to be made by make_one_rel().
 *
 * The "joinlist" result is a list of items that are either RangeTblRef
 * jointree nodes or sub-joinlists.  All the items at the same level of
 * joinlist must be joined in an order to be determined by make_one_rel()
 * (note that legal orders may be constrained by OuterJoinInfo nodes).
 * A sub-joinlist represents a subproblem to be planned separately. Currently
 * sub-joinlists arise only from FULL OUTER JOIN or when collapsing of
 * subproblems is stopped by join_collapse_limit or from_collapse_limit.
 *
 * NOTE: when dealing with inner joins, it is appropriate to let a qual clause
 * be evaluated at the lowest level where all the variables it mentions are
 * available.  However, we cannot push a qual down into the nullable side(s)
 * of an outer join since the qual might eliminate matching rows and cause a
 * NULL row to be incorrectly emitted by the join.	Therefore, we artificially
 * OR the minimum-relids of such an outer join into the required_relids of
 * clauses appearing above it.	This forces those clauses to be delayed until
 * application of the outer join (or maybe even higher in the join tree).
 */
List *
deconstruct_jointree(PlannerInfo *root)
{
	Relids		qualscope;
	Relids		inner_join_rels;

	/* Start recursion at top of jointree */
	Assert(root->parse->jointree != NULL &&
		   IsA(root->parse->jointree, FromExpr));

	return deconstruct_recurse(root, (Node *) root->parse->jointree, false,
							   &qualscope, &inner_join_rels);
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
 *		to by RestrictInfo and OuterJoinInfo nodes)
 *	*inner_join_rels gets the set of base Relids syntactically included in
 *		inner joins appearing at or below this jointree node (do not modify
 *		or free this, either)
 *	Return value is the appropriate joinlist for this jointree node
 *
 * In addition, entries will be added to root->oj_info_list for outer joins.
 */
static List *
deconstruct_recurse(PlannerInfo *root, Node *jtnode, bool below_outer_join,
					Relids *qualscope, Relids *inner_join_rels)
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
											   inner_join_rels);
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
		 * Now process the top-level quals.
		 */
		foreach(l, (List *) f->quals)
			distribute_qual_to_rels(root, (Node *) lfirst(l),
									false, below_outer_join,
									*qualscope, NULL, NULL);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		Relids		leftids,
					rightids,
					left_inners,
					right_inners,
					nonnullable_rels,
					ojscope;
		List	   *leftjoinlist,
				   *rightjoinlist;
		OuterJoinInfo *ojinfo;
		ListCell   *qual;

		/*
		 * Order of operations here is subtle and critical.  First we recurse
		 * to handle sub-JOINs.  Their join quals will be placed without
		 * regard for whether this level is an outer join, which is correct.
		 * Then we place our own join quals, which are restricted by lower
		 * outer joins in any case, and are forced to this level if this is an
		 * outer join and they mention the outer side.	Finally, if this is an
		 * outer join, we create an oj_info_list entry for the join.  This
		 * will prevent quals above us in the join tree that use those rels
		 * from being pushed down below this level.  (It's okay for upper
		 * quals to be pushed down to the outer side, however.)
		 */
		switch (j->jointype)
		{
			case JOIN_INNER:
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   below_outer_join,
												   &leftids, &left_inners);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													below_outer_join,
													&rightids, &right_inners);
				*qualscope = bms_union(leftids, rightids);
				*inner_join_rels = *qualscope;
				/* Inner join adds no restrictions for quals */
				nonnullable_rels = NULL;
				break;
			case JOIN_LEFT:
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   below_outer_join,
												   &leftids, &left_inners);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													true,
													&rightids, &right_inners);
				*qualscope = bms_union(leftids, rightids);
				*inner_join_rels = bms_union(left_inners, right_inners);
				nonnullable_rels = leftids;
				break;
			case JOIN_FULL:
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   true,
												   &leftids, &left_inners);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													true,
													&rightids, &right_inners);
				*qualscope = bms_union(leftids, rightids);
				*inner_join_rels = bms_union(left_inners, right_inners);
				/* each side is both outer and inner */
				nonnullable_rels = *qualscope;
				break;
			case JOIN_RIGHT:
				/* notice we switch leftids and rightids */
				leftjoinlist = deconstruct_recurse(root, j->larg,
												   true,
												   &rightids, &right_inners);
				rightjoinlist = deconstruct_recurse(root, j->rarg,
													below_outer_join,
													&leftids, &left_inners);
				*qualscope = bms_union(leftids, rightids);
				*inner_join_rels = bms_union(left_inners, right_inners);
				nonnullable_rels = leftids;
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				nonnullable_rels = NULL;		/* keep compiler quiet */
				leftjoinlist = rightjoinlist = NIL;
				break;
		}

		/*
		 * For an OJ, form the OuterJoinInfo now, because we need the OJ's
		 * semantic scope (ojscope) to pass to distribute_qual_to_rels.  But
		 * we mustn't add it to oj_info_list just yet, because we don't want
		 * distribute_qual_to_rels to think it is an outer join below us.
		 */
		if (j->jointype != JOIN_INNER)
		{
			ojinfo = make_outerjoininfo(root,
										leftids, rightids,
										*inner_join_rels,
										(j->jointype == JOIN_FULL),
										j->quals);
			ojscope = bms_union(ojinfo->min_lefthand, ojinfo->min_righthand);
		}
		else
		{
			ojinfo = NULL;
			ojscope = NULL;
		}

		/* Process the qual clauses */
		foreach(qual, (List *) j->quals)
			distribute_qual_to_rels(root, (Node *) lfirst(qual),
									false, below_outer_join,
									*qualscope, ojscope, nonnullable_rels);

		/* Now we can add the OuterJoinInfo to oj_info_list */
		if (ojinfo)
			root->oj_info_list = lappend(root->oj_info_list, ojinfo);

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
 *	  Build an OuterJoinInfo for the current outer join
 *
 * Inputs:
 *	left_rels: the base Relids syntactically on outer side of join
 *	right_rels: the base Relids syntactically on inner side of join
 *	inner_join_rels: base Relids participating in inner joins below this one
 *	is_full_join: what it says
 *	clause: the outer join's join condition
 *
 * If the join is a RIGHT JOIN, left_rels and right_rels are switched by
 * the caller, so that left_rels is always the nonnullable side.  Hence
 * we need only distinguish the LEFT and FULL cases.
 *
 * The node should eventually be appended to root->oj_info_list, but we
 * do not do that here.
 *
 * Note: we assume that this function is invoked bottom-up, so that
 * root->oj_info_list already contains entries for all outer joins that are
 * syntactically below this one.
 */
static OuterJoinInfo *
make_outerjoininfo(PlannerInfo *root,
				   Relids left_rels, Relids right_rels,
				   Relids inner_join_rels,
				   bool is_full_join, Node *clause)
{
	OuterJoinInfo *ojinfo = makeNode(OuterJoinInfo);
	Relids		clause_relids;
	Relids		strict_relids;
	Relids		min_lefthand;
	Relids		min_righthand;
	ListCell   *l;

	/*
	 * Presently the executor cannot support FOR UPDATE/SHARE marking of rels
	 * appearing on the nullable side of an outer join. (It's somewhat unclear
	 * what that would mean, anyway: what should we mark when a result row is
	 * generated from no element of the nullable relation?)  So, complain if
	 * any nullable rel is FOR UPDATE/SHARE.
	 *
	 * You might be wondering why this test isn't made far upstream in the
	 * parser.	It's because the parser hasn't got enough info --- consider
	 * FOR UPDATE applied to a view.  Only after rewriting and flattening do
	 * we know whether the view contains an outer join.
	 */
	foreach(l, root->parse->rowMarks)
	{
		RowMarkClause *rc = (RowMarkClause *) lfirst(l);

		if (bms_is_member(rc->rti, right_rels) ||
			(is_full_join && bms_is_member(rc->rti, left_rels)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("SELECT FOR UPDATE/SHARE cannot be applied to the nullable side of an outer join")));
	}

	/* this always starts out false */
	ojinfo->delay_upper_joins = false;

	/* If it's a full join, no need to be very smart */
	ojinfo->syn_lefthand = left_rels;
	ojinfo->syn_righthand = right_rels;
	ojinfo->is_full_join = is_full_join;
	if (is_full_join)
	{
		ojinfo->min_lefthand = left_rels;
		ojinfo->min_righthand = right_rels;
		ojinfo->lhs_strict = false;		/* don't care about this */
		return ojinfo;
	}

	/*
	 * Retrieve all relids mentioned within the join clause.
	 */
	clause_relids = pull_varnos(clause);

	/*
	 * For which relids is the clause strict, ie, it cannot succeed if the
	 * rel's columns are all NULL?
	 */
	strict_relids = find_nonnullable_rels(clause);

	/* Remember whether the clause is strict for any LHS relations */
	ojinfo->lhs_strict = bms_overlap(strict_relids, left_rels);

	/*
	 * Required LHS always includes the LHS rels mentioned in the clause. We
	 * may have to add more rels based on lower outer joins; see below.
	 */
	min_lefthand = bms_intersect(clause_relids, left_rels);

	/*
	 * Similarly for required RHS.	But here, we must also include any lower
	 * inner joins, to ensure we don't try to commute with any of them.
	 */
	min_righthand = bms_int_members(bms_union(clause_relids, inner_join_rels),
									right_rels);

	foreach(l, root->oj_info_list)
	{
		OuterJoinInfo *otherinfo = (OuterJoinInfo *) lfirst(l);

		/* ignore full joins --- other mechanisms preserve their ordering */
		if (otherinfo->is_full_join)
			continue;

		/*
		 * For a lower OJ in our LHS, if our join condition uses the lower
		 * join's RHS and is not strict for that rel, we must preserve the
		 * ordering of the two OJs, so add lower OJ's full syntactic relset to
		 * min_lefthand.  (We must use its full syntactic relset, not just its
		 * min_lefthand + min_righthand.  This is because there might be other
		 * OJs below this one that this one can commute with, but we cannot
		 * commute with them if we don't with this one.)
		 *
		 * Note: I believe we have to insist on being strict for at least one
		 * rel in the lower OJ's min_righthand, not its whole syn_righthand.
		 */
		if (bms_overlap(left_rels, otherinfo->syn_righthand) &&
			bms_overlap(clause_relids, otherinfo->syn_righthand) &&
			!bms_overlap(strict_relids, otherinfo->min_righthand))
		{
			min_lefthand = bms_add_members(min_lefthand,
										   otherinfo->syn_lefthand);
			min_lefthand = bms_add_members(min_lefthand,
										   otherinfo->syn_righthand);
		}

		/*
		 * For a lower OJ in our RHS, if our join condition does not use the
		 * lower join's RHS and the lower OJ's join condition is strict, we
		 * can interchange the ordering of the two OJs; otherwise we must add
		 * lower OJ's full syntactic relset to min_righthand.
		 *
		 * Here, we have to consider that "our join condition" includes any
		 * clauses that syntactically appeared above the lower OJ and below
		 * ours; those are equivalent to degenerate clauses in our OJ and must
		 * be treated as such.	Such clauses obviously can't reference our
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

	ojinfo->min_lefthand = min_lefthand;
	ojinfo->min_righthand = min_righthand;

	return ojinfo;
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
 *	  mentioned in the clause.	A RestrictInfo node is created and added to
 *	  the appropriate list for each rel.  Alternatively, if the clause uses a
 *	  mergejoinable operator and is not delayed by outer-join rules, enter
 *	  the left- and right-side expressions into the query's list of
 *	  EquivalenceClasses.
 *
 * 'clause': the qual clause to be distributed
 * 'is_deduced': TRUE if the qual came from implied-equality deduction
 * 'below_outer_join': TRUE if the qual is from a JOIN/ON that is below the
 *		nullable side of a higher-level outer join
 * 'qualscope': set of baserels the qual's syntactic scope covers
 * 'ojscope': NULL if not an outer-join qual, else the minimum set of baserels
 *		needed to form this join
 * 'outerjoin_nonnullable': NULL if not an outer-join qual, else the set of
 *		baserels appearing on the outer (nonnullable) side of the join
 *		(for FULL JOIN this includes both sides of the join, and must in fact
 *		equal qualscope)
 *
 * 'qualscope' identifies what level of JOIN the qual came from syntactically.
 * 'ojscope' is needed if we decide to force the qual up to the outer-join
 * level, which will be ojscope not necessarily qualscope.
 */
static void
distribute_qual_to_rels(PlannerInfo *root, Node *clause,
						bool is_deduced,
						bool below_outer_join,
						Relids qualscope,
						Relids ojscope,
						Relids outerjoin_nonnullable)
{
	Relids		relids;
	bool		is_pushed_down;
	bool		outerjoin_delayed;
	bool		pseudoconstant = false;
	bool		maybe_equivalence;
	bool		maybe_outer_join;
	RestrictInfo *restrictinfo;

	/*
	 * Retrieve all relids mentioned within the clause.
	 */
	relids = pull_varnos(clause);

	/*
	 * Cross-check: clause should contain no relids not within its scope.
	 * Otherwise the parser messed up.
	 */
	if (!bms_is_subset(relids, qualscope))
		elog(ERROR, "JOIN qualification cannot refer to other relations");
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
	 * plan node the pseudoconstant clause is assigned to.	It's usually best
	 * to put a gating node as high in the plan tree as possible. If we are
	 * not below an outer join, we can actually push the pseudoconstant qual
	 * all the way to the top of the tree.	If we are below an outer join, we
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
					relids = get_relids_in_jointree((Node *) root->parse->jointree);
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
	 * qual there.	However, this cannot happen, because all the rels the
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
		 * because the ojinfo list may now contain OJs above where the qual
		 * belongs.
		 */
		Assert(!ojscope);
		is_pushed_down = true;
		outerjoin_delayed = false;
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
		outerjoin_delayed = check_outerjoin_delay(root, &relids, false);

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
		 * Normal qual clause or degenerate outer-join clause.	Either way, we
		 * can mark it as pushed-down.
		 */
		is_pushed_down = true;

		/* Check to see if must be delayed by lower outer join */
		outerjoin_delayed = check_outerjoin_delay(root, &relids, true);

		if (outerjoin_delayed)
		{
			/* Should still be a subset of current scope ... */
			Assert(bms_is_subset(relids, qualscope));

			/*
			 * Because application of the qual will be delayed by outer join,
			 * we mustn't assume its vars are equal everywhere.
			 */
			maybe_equivalence = false;
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
									 relids);

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
		List	   *vars = pull_var_clause(clause, false);

		add_vars_to_targetlist(root, vars, relids);
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
	 */
	if (restrictinfo->mergeopfamilies)
	{
		if (maybe_equivalence)
		{
			if (process_equivalence(root, restrictinfo, below_outer_join))
				return;
			/* EC rejected it, so pass to distribute_restrictinfo_to_rels */
		}
		else if (maybe_outer_join && restrictinfo->can_join)
		{
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
			if (bms_equal(outerjoin_nonnullable, qualscope))
			{
				/* FULL JOIN (above tests cannot match in this case) */
				root->full_join_clauses = lappend(root->full_join_clauses,
												  restrictinfo);
				return;
			}
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
 * OuterJoinInfo structs.
 *
 * For an is_pushed_down qual, we can evaluate the qual as soon as (1) we have
 * all the rels it mentions, and (2) we are at or above any outer joins that
 * can null any of these rels and are below the syntactic location of the
 * given qual.	We must enforce (2) because pushing down such a clause below
 * the OJ might cause the OJ to emit null-extended rows that should not have
 * been formed, or that should have been rejected by the clause.  (This is
 * only an issue for non-strict quals, since if we can prove a qual mentioning
 * only nullable rels is strict, we'd have reduced the outer join to an inner
 * join in reduce_outer_joins().)
 *
 * To enforce (2), scan the oj_info_list and merge the required-relid sets of
 * any such OJs into the clause's own reference list.  At the time we are
 * called, the oj_info_list contains only outer joins below this qual.	We
 * have to repeat the scan until no new relids get added; this ensures that
 * the qual is suitably delayed regardless of the order in which OJs get
 * executed.  As an example, if we have one OJ with LHS=A, RHS=B, and one with
 * LHS=B, RHS=C, it is implied that these can be done in either order; if the
 * B/C join is done first then the join to A can null C, so a qual actually
 * mentioning only C cannot be applied below the join to A.
 *
 * For a non-pushed-down qual, this isn't going to determine where we place the
 * qual, but we need to determine outerjoin_delayed anyway for possible use
 * in reconsider_outer_join_clauses().
 *
 * Lastly, a pushed-down qual that references the nullable side of any current
 * oj_info_list member and has to be evaluated above that OJ (because its
 * required relids overlap the LHS too) causes that OJ's delay_upper_joins
 * flag to be set TRUE.  This will prevent any higher-level OJs from
 * being interchanged with that OJ, which would result in not having any
 * correct place to evaluate the qual.	(The case we care about here is a
 * sub-select WHERE clause within the RHS of some outer join.  The WHERE
 * clause must effectively be treated as a degenerate clause of that outer
 * join's condition.  Rather than trying to match such clauses with joins
 * directly, we set delay_upper_joins here, and when the upper outer join
 * is processed by make_outerjoininfo, it will refrain from allowing the
 * two OJs to commute.)
 */
static bool
check_outerjoin_delay(PlannerInfo *root, Relids *relids_p,
					  bool is_pushed_down)
{
	Relids		relids = *relids_p;
	bool		outerjoin_delayed;
	bool		found_some;

	outerjoin_delayed = false;
	do
	{
		ListCell   *l;

		found_some = false;
		foreach(l, root->oj_info_list)
		{
			OuterJoinInfo *ojinfo = (OuterJoinInfo *) lfirst(l);

			/* do we reference any nullable rels of this OJ? */
			if (bms_overlap(relids, ojinfo->min_righthand) ||
				(ojinfo->is_full_join &&
				 bms_overlap(relids, ojinfo->min_lefthand)))
			{
				/* yes; have we included all its rels in relids? */
				if (!bms_is_subset(ojinfo->min_lefthand, relids) ||
					!bms_is_subset(ojinfo->min_righthand, relids))
				{
					/* no, so add them in */
					relids = bms_add_members(relids, ojinfo->min_lefthand);
					relids = bms_add_members(relids, ojinfo->min_righthand);
					outerjoin_delayed = true;
					/* we'll need another iteration */
					found_some = true;
				}
				/* set delay_upper_joins if needed */
				if (is_pushed_down && !ojinfo->is_full_join &&
					bms_overlap(relids, ojinfo->min_lefthand))
					ojinfo->delay_upper_joins = true;
			}
		}
	} while (found_some);

	*relids_p = relids;
	return outerjoin_delayed;
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
			 * hashjoin info if we're not going to need it.)
			 */
			if (enable_hashjoin)
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
 * "both_const" indicates whether both items are known pseudo-constant;
 * in this case it is worth applying eval_const_expressions() in case we
 * can produce constant TRUE or constant FALSE.  (Otherwise it's not,
 * because the expressions went through eval_const_expressions already.)
 *
 * This is currently used only when an EquivalenceClass is found to
 * contain pseudoconstants.  See path/pathkeys.c for more details.
 */
void
process_implied_equality(PlannerInfo *root,
						 Oid opno,
						 Expr *item1,
						 Expr *item2,
						 Relids qualscope,
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
						   (Expr *) copyObject(item2));

	/* If both constant, try to reduce to a boolean constant. */
	if (both_const)
	{
		clause = (Expr *) eval_const_expressions((Node *) clause);

		/* If we produced const TRUE, just drop the clause */
		if (clause && IsA(clause, Const))
		{
			Const	   *cclause = (Const *) clause;

			Assert(cclause->consttype == BOOLOID);
			if (!cclause->constisnull && DatumGetBool(cclause->constvalue))
				return;
		}
	}

	/* Make a copy of qualscope to avoid problems if source EC changes */
	qualscope = bms_copy(qualscope);

	/*
	 * Push the new clause into all the appropriate restrictinfo lists.
	 */
	distribute_qual_to_rels(root, (Node *) clause,
							true, below_outer_join,
							qualscope, NULL, NULL);
}

/*
 * build_implied_join_equality --- build a RestrictInfo for a derived equality
 *
 * This overlaps the functionality of process_implied_equality(), but we
 * must return the RestrictInfo, not push it into the joininfo tree.
 */
RestrictInfo *
build_implied_join_equality(Oid opno,
							Expr *item1,
							Expr *item2,
							Relids qualscope)
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
						   (Expr *) copyObject(item2));

	/* Make a copy of qualscope to avoid problems if source EC changes */
	qualscope = bms_copy(qualscope);

	/*
	 * Build the RestrictInfo node itself.
	 */
	restrictinfo = make_restrictinfo(clause,
									 true,		/* is_pushed_down */
									 false,		/* outerjoin_delayed */
									 false,		/* pseudoconstant */
									 qualscope);

	/* Set mergejoinability info always, and hashjoinability if enabled */
	check_mergejoinable(restrictinfo);
	if (enable_hashjoin)
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

	if (restrictinfo->pseudoconstant)
		return;
	if (!is_opclause(clause))
		return;
	if (list_length(((OpExpr *) clause)->args) != 2)
		return;

	opno = ((OpExpr *) clause)->opno;

	if (op_mergejoinable(opno) &&
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
 *	  the operator is a hashjoinable operator.	The arguments can be
 *	  anything --- as long as there are no volatile functions in them.
 */
static void
check_hashjoinable(RestrictInfo *restrictinfo)
{
	Expr	   *clause = restrictinfo->clause;
	Oid			opno;

	if (restrictinfo->pseudoconstant)
		return;
	if (!is_opclause(clause))
		return;
	if (list_length(((OpExpr *) clause)->args) != 2)
		return;

	opno = ((OpExpr *) clause)->opno;

	if (op_hashjoinable(opno) &&
		!contain_volatile_functions((Node *) clause))
		restrictinfo->hashjoinoperator = opno;
}
