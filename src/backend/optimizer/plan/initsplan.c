/*-------------------------------------------------------------------------
 *
 * initsplan.c
 *	  Target list, qualification, joininfo initialization routines
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/initsplan.c,v 1.81 2003/01/15 19:35:40 tgl Exp $
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
						bool isouterjoin,
						bool isdeduced,
						Relids qualscope);
static void add_join_info_to_rels(Query *root, RestrictInfo *restrictinfo,
					  Relids join_relids);
static void add_vars_to_targetlist(Query *root, List *vars);
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
		{
			add_base_rels_to_query(root, lfirst(l));
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		add_base_rels_to_query(root, j->larg);
		add_base_rels_to_query(root, j->rarg);
		/*
		 * Safety check: join RTEs should not be SELECT FOR UPDATE targets
		 */
		if (intMember(j->rtindex, root->rowMarks))
			elog(ERROR, "SELECT FOR UPDATE cannot be applied to a join");
	}
	else
		elog(ERROR, "add_base_rels_to_query: unexpected node type %d",
			 nodeTag(jtnode));
}


/*****************************************************************************
 *
 *	 TARGET LISTS
 *
 *****************************************************************************/

/*
 * build_base_rel_tlists
 *	  Creates targetlist entries for each var seen in 'tlist' and adds
 *	  them to the tlist of the appropriate rel node.
 */
void
build_base_rel_tlists(Query *root, List *tlist)
{
	List	   *tlist_vars = pull_var_clause((Node *) tlist, false);

	add_vars_to_targetlist(root, tlist_vars);
	freeList(tlist_vars);
}

/*
 * add_vars_to_targetlist
 *	  For each variable appearing in the list, add it to the owning
 *	  relation's targetlist if not already present.
 */
static void
add_vars_to_targetlist(Query *root, List *vars)
{
	List	   *temp;

	foreach(temp, vars)
	{
		Var		   *var = (Var *) lfirst(temp);
		RelOptInfo *rel = find_base_rel(root, var->varno);

		add_var_to_tlist(rel, var);
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
 * NOTE: when dealing with inner joins, it is appropriate to let a qual clause
 * be evaluated at the lowest level where all the variables it mentions are
 * available.  However, we cannot push a qual down into the nullable side(s)
 * of an outer join since the qual might eliminate matching rows and cause a
 * NULL row to be incorrectly emitted by the join.	Therefore, rels appearing
 * within the nullable side(s) of an outer join are marked with
 * outerjoinset = list of Relids used at the outer join node.
 * This list will be added to the list of rels referenced by quals using such
 * a rel, thereby forcing them up the join tree to the right level.
 *
 * To ease the calculation of these values, distribute_quals_to_rels() returns
 * the list of base Relids involved in its own level of join.  This is just an
 * internal convenience; no outside callers pay attention to the result.
 */
Relids
distribute_quals_to_rels(Query *root, Node *jtnode)
{
	Relids		result = NIL;

	if (jtnode == NULL)
		return result;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		/* No quals to deal with, just return correct result */
		result = makeListi1(varno);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		List	   *l;
		List	   *qual;

		/*
		 * First, recurse to handle child joins.
		 *
		 * Note: we assume it's impossible to see same RT index from more
		 * than one subtree, so nconc() is OK rather than set_unioni().
		 */
		foreach(l, f->fromlist)
		{
			result = nconc(result,
						   distribute_quals_to_rels(root, lfirst(l)));
		}

		/*
		 * Now process the top-level quals.  These are always marked as
		 * "pushed down", since they clearly didn't come from a JOIN expr.
		 */
		foreach(qual, (List *) f->quals)
			distribute_qual_to_rels(root, (Node *) lfirst(qual),
									true, false, false, result);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		Relids		leftids,
					rightids;
		bool		isouterjoin;
		List	   *qual;

		/*
		 * Order of operations here is subtle and critical.  First we
		 * recurse to handle sub-JOINs.  Their join quals will be placed
		 * without regard for whether this level is an outer join, which
		 * is correct. Then, if we are an outer join, we mark baserels
		 * contained within the nullable side(s) with our own rel list;
		 * this will restrict placement of subsequent quals using those
		 * rels, including our own quals and quals above us in the join
		 * tree. Finally we place our own join quals.
		 */
		leftids = distribute_quals_to_rels(root, j->larg);
		rightids = distribute_quals_to_rels(root, j->rarg);

		result = nconc(listCopy(leftids), rightids);

		isouterjoin = false;
		switch (j->jointype)
		{
			case JOIN_INNER:
				/* Inner join adds no restrictions for quals */
				break;
			case JOIN_LEFT:
				mark_baserels_for_outer_join(root, rightids, result);
				isouterjoin = true;
				break;
			case JOIN_FULL:
				mark_baserels_for_outer_join(root, result, result);
				isouterjoin = true;
				break;
			case JOIN_RIGHT:
				mark_baserels_for_outer_join(root, leftids, result);
				isouterjoin = true;
				break;
			case JOIN_UNION:

				/*
				 * This is where we fail if upper levels of planner
				 * haven't rewritten UNION JOIN as an Append ...
				 */
				elog(ERROR, "UNION JOIN is not implemented yet");
				break;
			default:
				elog(ERROR,
					 "distribute_quals_to_rels: unsupported join type %d",
					 (int) j->jointype);
				break;
		}

		foreach(qual, (List *) j->quals)
			distribute_qual_to_rels(root, (Node *) lfirst(qual),
									false, isouterjoin, false, result);
	}
	else
		elog(ERROR, "distribute_quals_to_rels: unexpected node type %d",
			 nodeTag(jtnode));
	return result;
}

/*
 * mark_baserels_for_outer_join
 *	  Mark all base rels listed in 'rels' as having the given outerjoinset.
 */
static void
mark_baserels_for_outer_join(Query *root, Relids rels, Relids outerrels)
{
	List	   *relid;

	foreach(relid, rels)
	{
		int			relno = lfirsti(relid);
		RelOptInfo *rel = find_base_rel(root, relno);

		/*
		 * Since we do this bottom-up, any outer-rels previously marked
		 * should be within the new outer join set.
		 */
		Assert(is_subseti(rel->outerjoinset, outerrels));

		/*
		 * Presently the executor cannot support FOR UPDATE marking of
		 * rels appearing on the nullable side of an outer join. (It's
		 * somewhat unclear what that would mean, anyway: what should we
		 * mark when a result row is generated from no element of the
		 * nullable relation?)	So, complain if target rel is FOR UPDATE.
		 * It's sufficient to make this check once per rel, so do it only
		 * if rel wasn't already known nullable.
		 */
		if (rel->outerjoinset == NIL)
		{
			if (intMember(relno, root->rowMarks))
				elog(ERROR, "SELECT FOR UPDATE cannot be applied to the nullable side of an OUTER JOIN");
		}

		rel->outerjoinset = outerrels;
	}
}

/*
 * distribute_qual_to_rels
 *	  Add clause information to either the 'RestrictInfo' or 'JoinInfo' field
 *	  (depending on whether the clause is a join) of each base relation
 *	  mentioned in the clause.	A RestrictInfo node is created and added to
 *	  the appropriate list for each rel.  Also, if the clause uses a
 *	  mergejoinable operator and is not an outer-join qual, enter the left-
 *	  and right-side expressions into the query's lists of equijoined vars.
 *
 * 'clause': the qual clause to be distributed
 * 'ispusheddown': if TRUE, force the clause to be marked 'ispusheddown'
 *		(this indicates the clause came from a FromExpr, not a JoinExpr)
 * 'isouterjoin': TRUE if the qual came from an OUTER JOIN's ON-clause
 * 'isdeduced': TRUE if the qual came from implied-equality deduction
 * 'qualscope': list of baserels the qual's syntactic scope covers
 *
 * 'qualscope' identifies what level of JOIN the qual came from.  For a top
 * level qual (WHERE qual), qualscope lists all baserel ids and in addition
 * 'ispusheddown' will be TRUE.
 */
static void
distribute_qual_to_rels(Query *root, Node *clause,
						bool ispusheddown,
						bool isouterjoin,
						bool isdeduced,
						Relids qualscope)
{
	RestrictInfo *restrictinfo = makeNode(RestrictInfo);
	Relids		relids;
	List	   *vars;
	bool		can_be_equijoin;

	restrictinfo->clause = (Expr *) clause;
	restrictinfo->subclauseindices = NIL;
	restrictinfo->eval_cost.startup = -1; /* not computed until needed */
	restrictinfo->this_selec = -1;		/* not computed until needed */
	restrictinfo->left_relids = NIL; /* set below, if join clause */
	restrictinfo->right_relids = NIL;
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
	if (!is_subseti(relids, qualscope))
		elog(ERROR, "JOIN qualification may not refer to other relations");

	/*
	 * If the clause is variable-free, we force it to be evaluated at its
	 * original syntactic level.  Note that this should not happen for
	 * top-level clauses, because query_planner() special-cases them.  But
	 * it will happen for variable-free JOIN/ON clauses.  We don't have to
	 * be real smart about such a case, we just have to be correct.
	 */
	if (relids == NIL)
		relids = qualscope;

	/*
	 * For an outer-join qual, pretend that the clause references all rels
	 * appearing within its syntactic scope, even if it really doesn't.
	 * This ensures that the clause will be evaluated exactly at the level
	 * of joining corresponding to the outer join.
	 *
	 * For a non-outer-join qual, we can evaluate the qual as soon as (1) we
	 * have all the rels it mentions, and (2) we are at or above any outer
	 * joins that can null any of these rels and are below the syntactic
	 * location of the given qual.	To enforce the latter, scan the base
	 * rels listed in relids, and merge their outer-join lists into the
	 * clause's own reference list.  At the time we are called, the
	 * outerjoinset list of each baserel will show exactly those outer
	 * joins that are below the qual in the join tree.
	 *
	 * If the qual came from implied-equality deduction, we can evaluate the
	 * qual at its natural semantic level.
	 *
	 */
	if (isdeduced)
	{
		Assert(sameseti(relids, qualscope));
		can_be_equijoin = true;
	}
	else if (isouterjoin)
	{
		relids = qualscope;
		can_be_equijoin = false;
	}
	else
	{
		Relids		newrelids = relids;
		List	   *relid;

		/*
		 * We rely on set_unioni to be nondestructive of its input
		 * lists...
		 */
		can_be_equijoin = true;
		foreach(relid, relids)
		{
			RelOptInfo *rel = find_base_rel(root, lfirsti(relid));

			if (rel->outerjoinset &&
				!is_subseti(rel->outerjoinset, relids))
			{
				newrelids = set_unioni(newrelids, rel->outerjoinset);

				/*
				 * Because application of the qual will be delayed by
				 * outer join, we mustn't assume its vars are equal
				 * everywhere.
				 */
				can_be_equijoin = false;
			}
		}
		relids = newrelids;
		/* Should still be a subset of current scope ... */
		Assert(is_subseti(relids, qualscope));
	}

	/*
	 * Mark the qual as "pushed down" if it can be applied at a level
	 * below its original syntactic level.	This allows us to distinguish
	 * original JOIN/ON quals from higher-level quals pushed down to the
	 * same joinrel. A qual originating from WHERE is always considered
	 * "pushed down".
	 */
	restrictinfo->ispusheddown = ispusheddown || !sameseti(relids,
														   qualscope);

	if (length(relids) == 1)
	{
		/*
		 * There is only one relation participating in 'clause', so
		 * 'clause' is a restriction clause for that relation.
		 */
		RelOptInfo *rel = find_base_rel(root, lfirsti(relids));

		/*
		 * Check for a "mergejoinable" clause even though it's not a join
		 * clause.	This is so that we can recognize that "a.x = a.y"
		 * makes x and y eligible to be considered equal, even when they
		 * belong to the same rel.	Without this, we would not recognize
		 * that "a.x = a.y AND a.x = b.z AND a.y = c.q" allows us to
		 * consider z and q equal after their rels are joined.
		 */
		if (can_be_equijoin)
			check_mergejoinable(restrictinfo);

		/*
		 * If the clause was deduced from implied equality, check to see
		 * whether it is redundant with restriction clauses we already
		 * have for this rel.  Note we cannot apply this check to
		 * user-written clauses, since we haven't found the canonical
		 * pathkey sets yet while processing user clauses.	(NB: no
		 * comparable check is done in the join-clause case; redundancy
		 * will be detected when the join clause is moved into a join
		 * rel's restriction list.)
		 */
		if (!isdeduced ||
			!qual_is_redundant(root, restrictinfo, rel->baserestrictinfo))
		{
			/* Add clause to rel's restriction list */
			rel->baserestrictinfo = lappend(rel->baserestrictinfo,
											restrictinfo);
		}
	}
	else if (relids != NIL)
	{
		/*
		 * 'clause' is a join clause, since there is more than one rel in
		 * the relid list.	Set additional RestrictInfo fields for
		 * joining.  First, does it look like a normal join clause, i.e.,
		 * a binary operator relating expressions that come from distinct
		 * relations?  If so we might be able to use it in a join algorithm.
		 */
		if (is_opclause(clause) && length(((OpExpr *) clause)->args) == 2)
		{
			List	   *left_relids;
			List	   *right_relids;

			left_relids = pull_varnos(get_leftop((Expr *) clause));
			right_relids = pull_varnos(get_rightop((Expr *) clause));
			if (left_relids && right_relids &&
				nonoverlap_setsi(left_relids, right_relids))
			{
				restrictinfo->left_relids = left_relids;
				restrictinfo->right_relids = right_relids;
			}
		}

		/*
		 * Now check for hash or mergejoinable operators.
		 *
		 * We don't bother setting the hashjoin info if we're not going
		 * to need it.	We do want to know about mergejoinable ops in all
		 * cases, however, because we use mergejoinable ops for other
		 * purposes such as detecting redundant clauses.
		 */
		check_mergejoinable(restrictinfo);
		if (enable_hashjoin)
			check_hashjoinable(restrictinfo);

		/*
		 * Add clause to the join lists of all the relevant relations.
		 */
		add_join_info_to_rels(root, restrictinfo, relids);

		/*
		 * Add vars used in the join clause to targetlists of their
		 * relations, so that they will be emitted by the plan nodes that
		 * scan those relations (else they won't be available at the join
		 * node!).
		 */
		add_vars_to_targetlist(root, vars);
	}
	else
	{
		/*
		 * 'clause' references no rels, and therefore we have no place to
		 * attach it.  Shouldn't get here if callers are working properly.
		 */
		elog(ERROR, "distribute_qual_to_rels: can't cope with variable-free clause");
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
 * add_join_info_to_rels
 *	  For every relation participating in a join clause, add 'restrictinfo' to
 *	  the appropriate joininfo list (creating a new list and adding it to the
 *	  appropriate rel node if necessary).
 *
 * 'restrictinfo' describes the join clause
 * 'join_relids' is the list of relations participating in the join clause
 */
static void
add_join_info_to_rels(Query *root, RestrictInfo *restrictinfo,
					  Relids join_relids)
{
	List	   *join_relid;

	/* For every relid, find the joininfo, and add the proper join entries */
	foreach(join_relid, join_relids)
	{
		int			cur_relid = lfirsti(join_relid);
		Relids		unjoined_relids = NIL;
		JoinInfo   *joininfo;
		List	   *otherrel;

		/* Get the relids not equal to the current relid */
		foreach(otherrel, join_relids)
		{
			if (lfirsti(otherrel) != cur_relid)
				unjoined_relids = lappendi(unjoined_relids, lfirsti(otherrel));
		}

		/*
		 * Find or make the joininfo node for this combination of rels,
		 * and add the restrictinfo node to it.
		 */
		joininfo = find_joininfo_node(find_base_rel(root, cur_relid),
									  unjoined_relids);
		joininfo->jinfo_restrictinfo = lappend(joininfo->jinfo_restrictinfo,
											   restrictinfo);
	}
}

/*
 * process_implied_equality
 *	  Check to see whether we already have a restrictinfo item that says
 *	  item1 = item2, and create one if not.  This is a consequence of
 *	  transitivity of mergejoin equality: if we have mergejoinable
 *	  clauses A = B and B = C, we can deduce A = C (where = is an
 *	  appropriate mergejoinable operator).
 */
void
process_implied_equality(Query *root, Node *item1, Node *item2,
						 Oid sortop1, Oid sortop2)
{
	Oid			ltype,
				rtype;
	Operator	eq_operator;
	Form_pg_operator pgopform;
	Expr	   *clause;

	/*
	 * Forget it if this equality is already recorded.
	 *
	 * Note: if only a single relation is involved, we may fall through
	 * here and end up rejecting the equality later on in qual_is_redundant.
	 * This is a tad slow but should be okay.
	 */
	if (exprs_known_equal(root, item1, item2))
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
		elog(ERROR, "Unable to identify an equality operator for types '%s' and '%s'",
			 format_type_be(ltype), format_type_be(rtype));
	}
	pgopform = (Form_pg_operator) GETSTRUCT(eq_operator);

	/*
	 * Let's just make sure this appears to be a compatible operator.
	 */
	if (pgopform->oprlsortop != sortop1 ||
		pgopform->oprrsortop != sortop2 ||
		pgopform->oprresult != BOOLOID)
		elog(ERROR, "Equality operator for types '%s' and '%s' should be mergejoinable, but isn't",
			 format_type_be(ltype), format_type_be(rtype));

	clause = make_opclause(oprid(eq_operator), /* opno */
						   BOOLOID,	/* opresulttype */
						   false, /* opretset */
						   (Expr *) item1,
						   (Expr *) item2);

	ReleaseSysCache(eq_operator);

	/*
	 * Push the new clause into all the appropriate restrictinfo lists.
	 *
	 * Note: we mark the qual "pushed down" to ensure that it can never be
	 * taken for an original JOIN/ON clause.
	 */
	distribute_qual_to_rels(root, (Node *) clause,
							true, false, true,
							pull_varnos((Node *) clause));
}

/*
 * exprs_known_equal
 *	  Detect whether two expressions are known equal due to equijoin clauses.
 *
 * This is not completely accurate since we avoid adding redundant restriction
 * clauses to individual base rels (see qual_is_redundant).  However, after
 * the implied-equality-deduction phase, it is complete for expressions
 * involving Vars of multiple rels; that's sufficient for planned uses.
 */
bool
exprs_known_equal(Query *root, Node *item1, Node *item2)
{
	List	   *relids;
	RelOptInfo *rel1;
	List	   *restrictlist;
	List	   *itm;

	/* Get list of relids referenced in the two expressions */
	relids = set_unioni(pull_varnos(item1), pull_varnos(item2));

	/*
	 * If there are no Vars at all, say "true".  This prevents
	 * process_implied_equality from trying to store "const = const"
	 * deductions.
	 */
	if (relids == NIL)
		return true;

	/*
	 * If the exprs involve a single rel, we need to look at that rel's
	 * baserestrictinfo list.  If multiple rels, any one will have a
	 * joininfo node for the rest, and we can scan any of 'em.
	 */
	rel1 = find_base_rel(root, lfirsti(relids));
	relids = lnext(relids);
	if (relids == NIL)
		restrictlist = rel1->baserestrictinfo;
	else
	{
		JoinInfo   *joininfo = find_joininfo_node(rel1, relids);

		restrictlist = joininfo->jinfo_restrictinfo;
	}

	/*
	 * Scan to see if equality is known.
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
			return true;		/* found a matching clause */
	}

	return false;
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
 */
static bool
qual_is_redundant(Query *root,
				  RestrictInfo *restrictinfo,
				  List *restrictlist)
{
	List	   *oldquals;
	List	   *olditem;
	Node	   *newleft;
	Node	   *newright;
	List	   *equalexprs;
	bool		someadded;

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
	 * done.  We give up when we can't expand the equalexprs list any more.
	 */
	newleft = get_leftop(restrictinfo->clause);
	newright = get_rightop(restrictinfo->clause);
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
 *	  the operator is a hashjoinable operator.  The arguments can be
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
