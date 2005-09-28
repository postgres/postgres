/*-------------------------------------------------------------------------
 *
 * initsplan.c
 *	  Target list, qualification, joininfo initialization routines
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/initsplan.c,v 1.91.2.3 2005/09/28 21:17:48 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "parser/parse_expr.h"
#include "parser/parse_oper.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static void mark_baserels_for_outer_join(Query *root, Relids rels,
							 Relids outerrels);
static void distribute_qual_to_rels(Query *root, Node *clause,
						bool ispusheddown,
						bool isdeduced,
						bool below_outer_join,
						Relids outerjoin_nonnullable,
						Relids qualscope);
static void add_vars_to_targetlist(Query *root, List *vars,
					   Relids where_needed);
static bool qual_is_redundant(Query *root, RestrictInfo *restrictinfo,
				  List *restrictlist);
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
 * At the end of this process, there should be one baserel RelOptInfo for
 * every non-join RTE that is used in the query.  Therefore, this routine
 * is the only place that should call build_base_rel.  But build_other_rel
 * will be used later to build rels for inheritance children.
 */
void
add_base_rels_to_query(Query *root, Node *jtnode)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		build_base_rel(root, varno);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		List	   *l;

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
build_base_rel_tlists(Query *root, List *final_tlist)
{
	List	   *tlist_vars = pull_var_clause((Node *) final_tlist, false);

	if (tlist_vars != NIL)
	{
		add_vars_to_targetlist(root, tlist_vars, bms_make_singleton(0));
		freeList(tlist_vars);
	}
}

/*
 * add_vars_to_targetlist
 *	  For each variable appearing in the list, add it to the owning
 *	  relation's targetlist if not already present, and mark the variable
 *	  as being needed for the indicated join (or for final output if
 *	  where_needed includes "relation 0").
 */
static void
add_vars_to_targetlist(Query *root, List *vars, Relids where_needed)
{
	List	   *temp;

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
			FastAppend(&rel->reltargetlist, copyObject(var));
		}
		rel->attr_needed[attrno] = bms_add_members(rel->attr_needed[attrno],
												   where_needed);
	}
}


/*****************************************************************************
 *
 *	  QUALIFICATIONS
 *
 *****************************************************************************/


/*
 * distribute_quals_to_rels
 *	  Recursively scan the query's join tree for WHERE and JOIN/ON qual
 *	  clauses, and add these to the appropriate RestrictInfo and JoinInfo
 *	  lists belonging to base RelOptInfos.	Also, base RelOptInfos are marked
 *	  with outerjoinset information, to aid in proper positioning of qual
 *	  clauses that appear above outer joins.
 *
 * jtnode is the jointree node currently being examined.  below_outer_join
 * is TRUE if this node is within the nullable side of a higher-level outer
 * join.
 *
 * NOTE: when dealing with inner joins, it is appropriate to let a qual clause
 * be evaluated at the lowest level where all the variables it mentions are
 * available.  However, we cannot push a qual down into the nullable side(s)
 * of an outer join since the qual might eliminate matching rows and cause a
 * NULL row to be incorrectly emitted by the join.	Therefore, rels appearing
 * within the nullable side(s) of an outer join are marked with
 *		outerjoinset = set of Relids used at the outer join node.
 * This set will be added to the set of rels referenced by quals using such
 * a rel, thereby forcing them up the join tree to the right level.
 *
 * To ease the calculation of these values, distribute_quals_to_rels() returns
 * the set of base Relids involved in its own level of join.  This is just an
 * internal convenience; no outside callers pay attention to the result.
 */
Relids
distribute_quals_to_rels(Query *root, Node *jtnode,
						 bool below_outer_join)
{
	Relids		result = NULL;

	if (jtnode == NULL)
		return result;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		/* No quals to deal with, just return correct result */
		result = bms_make_singleton(varno);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		List	   *l;
		List	   *qual;

		/*
		 * First, recurse to handle child joins.
		 */
		foreach(l, f->fromlist)
		{
			result = bms_add_members(result,
									 distribute_quals_to_rels(root,
															  lfirst(l),
															  below_outer_join));
		}

		/*
		 * Now process the top-level quals.  These are always marked as
		 * "pushed down", since they clearly didn't come from a JOIN expr.
		 */
		foreach(qual, (List *) f->quals)
			distribute_qual_to_rels(root, (Node *) lfirst(qual),
									true, false, below_outer_join,
									NULL, result);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		Relids		leftids,
					rightids,
					nonnullable_rels,
					nullable_rels;
		List	   *qual;

		/*
		 * Order of operations here is subtle and critical.  First we
		 * recurse to handle sub-JOINs.  Their join quals will be placed
		 * without regard for whether this level is an outer join, which
		 * is correct.	Then we place our own join quals, which are
		 * restricted by lower outer joins in any case, and are forced to
		 * this level if this is an outer join and they mention the outer
		 * side.  Finally, if this is an outer join, we mark baserels
		 * contained within the inner side(s) with our own rel set; this
		 * will prevent quals above us in the join tree that use those
		 * rels from being pushed down below this level.  (It's okay for
		 * upper quals to be pushed down to the outer side, however.)
		 */
		switch (j->jointype)
		{
			case JOIN_INNER:
				leftids = distribute_quals_to_rels(root, j->larg,
												   below_outer_join);
				rightids = distribute_quals_to_rels(root, j->rarg,
													below_outer_join);

				result = bms_union(leftids, rightids);
				/* Inner join adds no restrictions for quals */
				nonnullable_rels = NULL;
				nullable_rels = NULL;
				break;
			case JOIN_LEFT:
				leftids = distribute_quals_to_rels(root, j->larg,
												   below_outer_join);
				rightids = distribute_quals_to_rels(root, j->rarg,
													true);

				result = bms_union(leftids, rightids);
				nonnullable_rels = leftids;
				nullable_rels = rightids;
				break;
			case JOIN_FULL:
				leftids = distribute_quals_to_rels(root, j->larg,
												   true);
				rightids = distribute_quals_to_rels(root, j->rarg,
													true);

				result = bms_union(leftids, rightids);
				/* each side is both outer and inner */
				nonnullable_rels = result;
				nullable_rels = result;
				break;
			case JOIN_RIGHT:
				leftids = distribute_quals_to_rels(root, j->larg,
												   true);
				rightids = distribute_quals_to_rels(root, j->rarg,
													below_outer_join);

				result = bms_union(leftids, rightids);
				nonnullable_rels = rightids;
				nullable_rels = leftids;
				break;
			case JOIN_UNION:

				/*
				 * This is where we fail if upper levels of planner
				 * haven't rewritten UNION JOIN as an Append ...
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("UNION JOIN is not implemented")));
				nonnullable_rels = NULL;	/* keep compiler quiet */
				nullable_rels = NULL;
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
				nonnullable_rels = NULL;	/* keep compiler quiet */
				nullable_rels = NULL;
				break;
		}

		foreach(qual, (List *) j->quals)
			distribute_qual_to_rels(root, (Node *) lfirst(qual),
									false, false, below_outer_join,
									nonnullable_rels, result);

		if (nullable_rels != NULL)
			mark_baserels_for_outer_join(root, nullable_rels, result);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return result;
}

/*
 * mark_baserels_for_outer_join
 *	  Mark all base rels listed in 'rels' as having the given outerjoinset.
 */
static void
mark_baserels_for_outer_join(Query *root, Relids rels, Relids outerrels)
{
	Relids		tmprelids;
	int			relno;

	tmprelids = bms_copy(rels);
	while ((relno = bms_first_member(tmprelids)) >= 0)
	{
		RelOptInfo *rel = find_base_rel(root, relno);

		/*
		 * Since we do this bottom-up, any outer-rels previously marked
		 * should be within the new outer join set.
		 */
		Assert(bms_is_subset(rel->outerjoinset, outerrels));

		/*
		 * Presently the executor cannot support FOR UPDATE marking of
		 * rels appearing on the nullable side of an outer join. (It's
		 * somewhat unclear what that would mean, anyway: what should we
		 * mark when a result row is generated from no element of the
		 * nullable relation?)	So, complain if target rel is FOR UPDATE.
		 * It's sufficient to make this check once per rel, so do it only
		 * if rel wasn't already known nullable.
		 */
		if (rel->outerjoinset == NULL)
		{
			if (intMember(relno, root->rowMarks))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("SELECT FOR UPDATE cannot be applied to the nullable side of an outer join")));
		}

		rel->outerjoinset = outerrels;
	}
	bms_free(tmprelids);
}

/*
 * distribute_qual_to_rels
 *	  Add clause information to either the 'RestrictInfo' or 'JoinInfo' field
 *	  (depending on whether the clause is a join) of each base relation
 *	  mentioned in the clause.	A RestrictInfo node is created and added to
 *	  the appropriate list for each rel.  Also, if the clause uses a
 *	  mergejoinable operator and is not delayed by outer-join rules, enter
 *	  the left- and right-side expressions into the query's lists of
 *	  equijoined vars.
 *
 * 'clause': the qual clause to be distributed
 * 'ispusheddown': if TRUE, force the clause to be marked 'ispusheddown'
 *		(this indicates the clause came from a FromExpr, not a JoinExpr)
 * 'isdeduced': TRUE if the qual came from implied-equality deduction
 * 'below_outer_join': TRUE if the qual is from a JOIN/ON that is below the
 *		nullable side of a higher-level outer join.
 * 'outerjoin_nonnullable': NULL if not an outer-join qual, else the set of
 *		baserels appearing on the outer (nonnullable) side of the join
 * 'qualscope': set of baserels the qual's syntactic scope covers
 *
 * 'qualscope' identifies what level of JOIN the qual came from.  For a top
 * level qual (WHERE qual), qualscope lists all baserel ids and in addition
 * 'ispusheddown' will be TRUE.
 */
static void
distribute_qual_to_rels(Query *root, Node *clause,
						bool ispusheddown,
						bool isdeduced,
						bool below_outer_join,
						Relids outerjoin_nonnullable,
						Relids qualscope)
{
	RestrictInfo *restrictinfo = makeNode(RestrictInfo);
	RelOptInfo *rel;
	Relids		relids;
	List	   *vars;
	bool		can_be_equijoin;

	restrictinfo->clause = (Expr *) clause;
	restrictinfo->subclauseindices = NIL;
	restrictinfo->eval_cost.startup = -1;		/* not computed until
												 * needed */
	restrictinfo->this_selec = -1;		/* not computed until needed */
	restrictinfo->left_relids = NULL;	/* set below, if join clause */
	restrictinfo->right_relids = NULL;
	restrictinfo->mergejoinoperator = InvalidOid;
	restrictinfo->left_sortop = InvalidOid;
	restrictinfo->right_sortop = InvalidOid;
	restrictinfo->left_pathkey = NIL;	/* not computable yet */
	restrictinfo->right_pathkey = NIL;
	restrictinfo->left_mergescansel = -1;		/* not computed until
												 * needed */
	restrictinfo->right_mergescansel = -1;
	restrictinfo->hashjoinoperator = InvalidOid;
	restrictinfo->left_bucketsize = -1; /* not computed until needed */
	restrictinfo->right_bucketsize = -1;

	/*
	 * Retrieve all relids and vars contained within the clause.
	 */
	clause_get_relids_vars(clause, &relids, &vars);

	/*
	 * Cross-check: clause should contain no relids not within its scope.
	 * Otherwise the parser messed up.
	 */
	if (!bms_is_subset(relids, qualscope))
		elog(ERROR, "JOIN qualification may not refer to other relations");

	/*
	 * If the clause is variable-free, we force it to be evaluated at its
	 * original syntactic level.  Note that this should not happen for
	 * top-level clauses, because query_planner() special-cases them.  But
	 * it will happen for variable-free JOIN/ON clauses.  We don't have to
	 * be real smart about such a case, we just have to be correct.
	 */
	if (bms_is_empty(relids))
		relids = qualscope;

	/*
	 * Check to see if clause application must be delayed by outer-join
	 * considerations.
	 */
	if (isdeduced)
	{
		/*
		 * If the qual came from implied-equality deduction, we can
		 * evaluate the qual at its natural semantic level.  It is not
		 * affected by any outer-join rules (else we'd not have decided
		 * the vars were equal).
		 */
		Assert(bms_equal(relids, qualscope));
		can_be_equijoin = true;
	}
	else if (bms_overlap(relids, outerjoin_nonnullable))
	{
		/*
		 * The qual is attached to an outer join and mentions (some of
		 * the) rels on the nonnullable side.  Force the qual to be
		 * evaluated exactly at the level of joining corresponding to the
		 * outer join. We cannot let it get pushed down into the
		 * nonnullable side, since then we'd produce no output rows,
		 * rather than the intended single null-extended row, for any
		 * nonnullable-side rows failing the qual.
		 *
		 * Note: an outer-join qual that mentions only nullable-side rels can
		 * be pushed down into the nullable side without changing the join
		 * result, so we treat it the same as an ordinary inner-join qual,
		 * except for not setting can_be_equijoin (see below).
		 */
		relids = qualscope;
		can_be_equijoin = false;
	}
	else
	{
		/*
		 * For a non-outer-join qual, we can evaluate the qual as soon as
		 * (1) we have all the rels it mentions, and (2) we are at or
		 * above any outer joins that can null any of these rels and are
		 * below the syntactic location of the given qual. To enforce the
		 * latter, scan the base rels listed in relids, and merge their
		 * outer-join sets into the clause's own reference list.  At the
		 * time we are called, the outerjoinset of each baserel will show
		 * exactly those outer joins that are below the qual in the join
		 * tree.
		 */
		Relids		addrelids = NULL;
		Relids		tmprelids;
		int			relno;

		tmprelids = bms_copy(relids);
		while ((relno = bms_first_member(tmprelids)) >= 0)
		{
			RelOptInfo *rel = find_base_rel(root, relno);

			if (rel->outerjoinset != NULL)
				addrelids = bms_add_members(addrelids, rel->outerjoinset);
		}
		bms_free(tmprelids);

		if (bms_is_subset(addrelids, relids))
		{
			/*
			 * Qual is not delayed by any lower outer-join restriction.
			 * If it is not itself below or within an outer join, we
			 * can consider it "valid everywhere", so consider feeding
			 * it to the equijoin machinery.  (If it is within an outer
			 * join, we can't consider it "valid everywhere": once the
			 * contained variables have gone to NULL, we'd be asserting
			 * things like NULL = NULL, which is not true.)
			 */
			if (!below_outer_join && outerjoin_nonnullable == NULL)
				can_be_equijoin = true;
			else
				can_be_equijoin = false;
		}
		else
		{
			relids = bms_union(relids, addrelids);
			/* Should still be a subset of current scope ... */
			Assert(bms_is_subset(relids, qualscope));

			/*
			 * Because application of the qual will be delayed by outer
			 * join, we mustn't assume its vars are equal everywhere.
			 */
			can_be_equijoin = false;
		}
		bms_free(addrelids);
	}

	/*
	 * Mark the qual as "pushed down" if it can be applied at a level
	 * below its original syntactic level.	This allows us to distinguish
	 * original JOIN/ON quals from higher-level quals pushed down to the
	 * same joinrel. A qual originating from WHERE is always considered
	 * "pushed down".
	 */
	restrictinfo->ispusheddown = ispusheddown || !bms_equal(relids,
															qualscope);

	switch (bms_membership(relids))
	{
		case BMS_SINGLETON:

			/*
			 * There is only one relation participating in 'clause', so
			 * 'clause' is a restriction clause for that relation.
			 */
			rel = find_base_rel(root, bms_singleton_member(relids));

			/*
			 * Check for a "mergejoinable" clause even though it's not a
			 * join clause.  This is so that we can recognize that "a.x =
			 * a.y" makes x and y eligible to be considered equal, even
			 * when they belong to the same rel.  Without this, we would
			 * not recognize that "a.x = a.y AND a.x = b.z AND a.y = c.q"
			 * allows us to consider z and q equal after their rels are
			 * joined.
			 */
			if (can_be_equijoin)
				check_mergejoinable(restrictinfo);

			/*
			 * If the clause was deduced from implied equality, check to
			 * see whether it is redundant with restriction clauses we
			 * already have for this rel.  Note we cannot apply this check
			 * to user-written clauses, since we haven't found the
			 * canonical pathkey sets yet while processing user clauses.
			 * (NB: no comparable check is done in the join-clause case;
			 * redundancy will be detected when the join clause is moved
			 * into a join rel's restriction list.)
			 */
			if (!isdeduced ||
			!qual_is_redundant(root, restrictinfo, rel->baserestrictinfo))
			{
				/* Add clause to rel's restriction list */
				rel->baserestrictinfo = lappend(rel->baserestrictinfo,
												restrictinfo);
			}
			break;
		case BMS_MULTIPLE:

			/*
			 * 'clause' is a join clause, since there is more than one rel
			 * in the relid set.   Set additional RestrictInfo fields for
			 * joining.  First, does it look like a normal join clause,
			 * i.e., a binary operator relating expressions that come from
			 * distinct relations?	If so we might be able to use it in a
			 * join algorithm.
			 */
			if (is_opclause(clause) && length(((OpExpr *) clause)->args) == 2)
			{
				Relids		left_relids;
				Relids		right_relids;

				left_relids = pull_varnos(get_leftop((Expr *) clause));
				right_relids = pull_varnos(get_rightop((Expr *) clause));
				if (!bms_is_empty(left_relids) &&
					!bms_is_empty(right_relids) &&
					!bms_overlap(left_relids, right_relids))
				{
					restrictinfo->left_relids = left_relids;
					restrictinfo->right_relids = right_relids;
				}
			}

			/*
			 * Now check for hash or mergejoinable operators.
			 *
			 * We don't bother setting the hashjoin info if we're not going
			 * to need it.	We do want to know about mergejoinable ops in
			 * all cases, however, because we use mergejoinable ops for
			 * other purposes such as detecting redundant clauses.
			 */
			check_mergejoinable(restrictinfo);
			if (enable_hashjoin)
				check_hashjoinable(restrictinfo);

			/*
			 * Add clause to the join lists of all the relevant relations.
			 */
			add_join_clause_to_rels(root, restrictinfo, relids);

			/*
			 * Add vars used in the join clause to targetlists of their
			 * relations, so that they will be emitted by the plan nodes
			 * that scan those relations (else they won't be available at
			 * the join node!).
			 */
			add_vars_to_targetlist(root, vars, relids);
			break;
		default:

			/*
			 * 'clause' references no rels, and therefore we have no place
			 * to attach it.  Shouldn't get here if callers are working
			 * properly.
			 */
			elog(ERROR, "cannot cope with variable-free clause");
			break;
	}

	/*
	 * If the clause has a mergejoinable operator, and is not an
	 * outer-join qualification nor bubbled up due to an outer join, then
	 * the two sides represent equivalent PathKeyItems for path keys: any
	 * path that is sorted by one side will also be sorted by the other
	 * (as soon as the two rels are joined, that is).  Record the key
	 * equivalence for future use.	(We can skip this for a deduced
	 * clause, since the keys are already known equivalent in that case.)
	 */
	if (can_be_equijoin && restrictinfo->mergejoinoperator != InvalidOid &&
		!isdeduced)
		add_equijoined_keys(root, restrictinfo);
}

/*
 * process_implied_equality
 *	  Check to see whether we already have a restrictinfo item that says
 *	  item1 = item2, and create one if not; or if delete_it is true,
 *	  remove any such restrictinfo item.
 *
 * This processing is a consequence of transitivity of mergejoin equality:
 * if we have mergejoinable clauses A = B and B = C, we can deduce A = C
 * (where = is an appropriate mergejoinable operator).	See path/pathkeys.c
 * for more details.
 */
void
process_implied_equality(Query *root,
						 Node *item1, Node *item2,
						 Oid sortop1, Oid sortop2,
						 Relids item1_relids, Relids item2_relids,
						 bool delete_it)
{
	Relids		relids;
	BMS_Membership membership;
	RelOptInfo *rel1;
	List	   *restrictlist;
	List	   *itm;
	Oid			ltype,
				rtype;
	Operator	eq_operator;
	Form_pg_operator pgopform;
	Expr	   *clause;

	/* Get set of relids referenced in the two expressions */
	relids = bms_union(item1_relids, item2_relids);
	membership = bms_membership(relids);

	/*
	 * generate_implied_equalities() shouldn't call me on two constants.
	 */
	Assert(membership != BMS_EMPTY_SET);

	/*
	 * If the exprs involve a single rel, we need to look at that rel's
	 * baserestrictinfo list.  If multiple rels, any one will have a
	 * joininfo node for the rest, and we can scan any of 'em.
	 */
	if (membership == BMS_SINGLETON)
	{
		rel1 = find_base_rel(root, bms_singleton_member(relids));
		restrictlist = rel1->baserestrictinfo;
	}
	else
	{
		Relids		other_rels;
		int			first_rel;
		JoinInfo   *joininfo;

		/* Copy relids, find and remove one member */
		other_rels = bms_copy(relids);
		first_rel = bms_first_member(other_rels);

		rel1 = find_base_rel(root, first_rel);

		/* use remaining members to find join node */
		joininfo = find_joininfo_node(rel1, other_rels);

		restrictlist = joininfo ? joininfo->jinfo_restrictinfo : NIL;

		bms_free(other_rels);
	}

	/*
	 * Scan to see if equality is already known.  If so, we're done in the
	 * add case, and done after removing it in the delete case.
	 */
	foreach(itm, restrictlist)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(itm);
		Node	   *left,
				   *right;

		if (restrictinfo->mergejoinoperator == InvalidOid)
			continue;			/* ignore non-mergejoinable clauses */
		/* We now know the restrictinfo clause is a binary opclause */
		left = get_leftop(restrictinfo->clause);
		right = get_rightop(restrictinfo->clause);
		if ((equal(item1, left) && equal(item2, right)) ||
			(equal(item2, left) && equal(item1, right)))
		{
			/* found a matching clause */
			if (delete_it)
			{
				if (membership == BMS_SINGLETON)
				{
					/* delete it from local restrictinfo list */
					rel1->baserestrictinfo = lremove(restrictinfo,
												 rel1->baserestrictinfo);
				}
				else
				{
					/* let joininfo.c do it */
					remove_join_clause_from_rels(root, restrictinfo, relids);
				}
			}
			return;				/* done */
		}
	}

	/* Didn't find it.  Done if deletion requested */
	if (delete_it)
		return;

	/*
	 * This equality is new information, so construct a clause
	 * representing it to add to the query data structures.
	 */
	ltype = exprType(item1);
	rtype = exprType(item2);
	eq_operator = compatible_oper(makeList1(makeString("=")),
								  ltype, rtype, true);
	if (!HeapTupleIsValid(eq_operator))
	{
		/*
		 * Would it be safe to just not add the equality to the query if
		 * we have no suitable equality operator for the combination of
		 * datatypes?  NO, because sortkey selection may screw up anyway.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not identify an equality operator for types %s and %s",
						format_type_be(ltype), format_type_be(rtype))));
	}
	pgopform = (Form_pg_operator) GETSTRUCT(eq_operator);

	/*
	 * Let's just make sure this appears to be a compatible operator.
	 */
	if (pgopform->oprlsortop != sortop1 ||
		pgopform->oprrsortop != sortop2 ||
		pgopform->oprresult != BOOLOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("equality operator for types %s and %s should be merge-joinable, but isn't",
						format_type_be(ltype), format_type_be(rtype))));

	/*
	 * Now we can build the new clause.  Copy to ensure it shares no
	 * substructure with original (this is necessary in case there are
	 * subselects in there...)
	 */
	clause = make_opclause(oprid(eq_operator),	/* opno */
						   BOOLOID,		/* opresulttype */
						   false,		/* opretset */
						   (Expr *) copyObject(item1),
						   (Expr *) copyObject(item2));

	ReleaseSysCache(eq_operator);

	/*
	 * Push the new clause into all the appropriate restrictinfo lists.
	 *
	 * Note: we mark the qual "pushed down" to ensure that it can never be
	 * taken for an original JOIN/ON clause.
	 */
	distribute_qual_to_rels(root, (Node *) clause,
							true, true, false, NULL, relids);
}

/*
 * qual_is_redundant
 *	  Detect whether an implied-equality qual that turns out to be a
 *	  restriction clause for a single base relation is redundant with
 *	  already-known restriction clauses for that rel.  This occurs with,
 *	  for example,
 *				SELECT * FROM tab WHERE f1 = f2 AND f2 = f3;
 *	  We need to suppress the redundant condition to avoid computing
 *	  too-small selectivity, not to mention wasting time at execution.
 *
 * Note: quals of the form "var = const" are never considered redundant,
 * only those of the form "var = var".	This is needed because when we
 * have constants in an implied-equality set, we use a different strategy
 * that suppresses all "var = var" deductions.	We must therefore keep
 * all the "var = const" quals.
 */
static bool
qual_is_redundant(Query *root,
				  RestrictInfo *restrictinfo,
				  List *restrictlist)
{
	Node	   *newleft;
	Node	   *newright;
	List	   *oldquals;
	List	   *olditem;
	List	   *equalexprs;
	bool		someadded;

	newleft = get_leftop(restrictinfo->clause);
	newright = get_rightop(restrictinfo->clause);

	/* Never redundant unless vars appear on both sides */
	if (!contain_var_clause(newleft) || !contain_var_clause(newright))
		return false;

	/*
	 * Set cached pathkeys.  NB: it is okay to do this now because this
	 * routine is only invoked while we are generating implied equalities.
	 * Therefore, the equi_key_list is already complete and so we can
	 * correctly determine canonical pathkeys.
	 */
	cache_mergeclause_pathkeys(root, restrictinfo);
	/* If different, say "not redundant" (should never happen) */
	if (restrictinfo->left_pathkey != restrictinfo->right_pathkey)
		return false;

	/*
	 * Scan existing quals to find those referencing same pathkeys.
	 * Usually there will be few, if any, so build a list of just the
	 * interesting ones.
	 */
	oldquals = NIL;
	foreach(olditem, restrictlist)
	{
		RestrictInfo *oldrinfo = (RestrictInfo *) lfirst(olditem);

		if (oldrinfo->mergejoinoperator != InvalidOid)
		{
			cache_mergeclause_pathkeys(root, oldrinfo);
			if (restrictinfo->left_pathkey == oldrinfo->left_pathkey &&
				restrictinfo->right_pathkey == oldrinfo->right_pathkey)
				oldquals = lcons(oldrinfo, oldquals);
		}
	}
	if (oldquals == NIL)
		return false;

	/*
	 * Now, we want to develop a list of exprs that are known equal to the
	 * left side of the new qual.  We traverse the old-quals list
	 * repeatedly to transitively expand the exprs list.  If at any point
	 * we find we can reach the right-side expr of the new qual, we are
	 * done.  We give up when we can't expand the equalexprs list any
	 * more.
	 */
	equalexprs = makeList1(newleft);
	do
	{
		someadded = false;
		/* cannot use foreach here because of possible lremove */
		olditem = oldquals;
		while (olditem)
		{
			RestrictInfo *oldrinfo = (RestrictInfo *) lfirst(olditem);
			Node	   *oldleft = get_leftop(oldrinfo->clause);
			Node	   *oldright = get_rightop(oldrinfo->clause);
			Node	   *newguy = NULL;

			/* must advance olditem before lremove possibly pfree's it */
			olditem = lnext(olditem);

			if (member(oldleft, equalexprs))
				newguy = oldright;
			else if (member(oldright, equalexprs))
				newguy = oldleft;
			else
				continue;
			if (equal(newguy, newright))
				return true;	/* we proved new clause is redundant */
			equalexprs = lcons(newguy, equalexprs);
			someadded = true;

			/*
			 * Remove this qual from list, since we don't need it anymore.
			 */
			oldquals = lremove(oldrinfo, oldquals);
		}
	} while (someadded);

	return false;				/* it's not redundant */
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
	Oid			opno,
				leftOp,
				rightOp;

	if (!is_opclause(clause))
		return;
	if (length(((OpExpr *) clause)->args) != 2)
		return;

	opno = ((OpExpr *) clause)->opno;

	if (op_mergejoinable(opno,
						 &leftOp,
						 &rightOp) &&
		!contain_volatile_functions((Node *) clause))
	{
		restrictinfo->mergejoinoperator = opno;
		restrictinfo->left_sortop = leftOp;
		restrictinfo->right_sortop = rightOp;
	}
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

	if (!is_opclause(clause))
		return;
	if (length(((OpExpr *) clause)->args) != 2)
		return;

	opno = ((OpExpr *) clause)->opno;

	if (op_hashjoinable(opno) &&
		!contain_volatile_functions((Node *) clause))
		restrictinfo->hashjoinoperator = opno;
}
