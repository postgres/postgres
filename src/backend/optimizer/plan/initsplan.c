/*-------------------------------------------------------------------------
 *
 * initsplan.c
 *	  Target list, qualification, joininfo initialization routines
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/initsplan.c,v 1.55 2000/12/14 22:30:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

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
#include "parser/parse_type.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static void mark_baserels_for_outer_join(Query *root, Relids rels,
										 Relids outerrels);
static void distribute_qual_to_rels(Query *root, Node *clause,
									bool ispusheddown,
									bool isouterjoin,
									Relids qualscope);
static void add_join_info_to_rels(Query *root, RestrictInfo *restrictinfo,
					  Relids join_relids);
static void add_vars_to_targetlist(Query *root, List *vars);
static void check_mergejoinable(RestrictInfo *restrictinfo);
static void check_hashjoinable(RestrictInfo *restrictinfo);


/*****************************************************************************
 *
 *	 TARGET LISTS
 *
 *****************************************************************************/

/*
 * build_base_rel_tlists
 *	  Creates rel nodes for every relation mentioned in the target list
 *	  'tlist' (if a node hasn't already been created) and adds them to
 *	  root->base_rel_list.  Creates targetlist entries for each var seen
 *	  in 'tlist' and adds them to the tlist of the appropriate rel node.
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
 *	  For each variable appearing in the list, add it to the relation's
 *	  targetlist if not already present.  Rel nodes will also be created
 *	  if not already present.
 */
static void
add_vars_to_targetlist(Query *root, List *vars)
{
	List	   *temp;

	foreach(temp, vars)
	{
		Var		   *var = (Var *) lfirst(temp);
		RelOptInfo *rel = get_base_rel(root, var->varno);

		add_var_to_tlist(rel, var);
	}
}

/*----------
 * add_missing_rels_to_query
 *
 *	  If we have a relation listed in the join tree that does not appear
 *	  in the target list nor qualifications, we must add it to the base
 *	  relation list so that it can be processed.  For instance,
 *			select count(*) from foo;
 *	  would fail to scan foo if this routine were not called.  More subtly,
 *			select f.x from foo f, foo f2
 *	  is a join of f and f2.  Note that if we have
 *			select foo.x from foo f
 *	  this also gets turned into a join (between foo as foo and foo as f).
 *
 *	  Returns a list of all the base relations (RelOptInfo nodes) that appear
 *	  in the join tree.  This list can be used for cross-checking in the
 *	  reverse direction, ie, that we have a join tree entry for every
 *	  relation used in the query.
 *----------
 */
List *
add_missing_rels_to_query(Query *root, Node *jtnode)
{
	List	   *result = NIL;

	if (jtnode == NULL)
		return NIL;
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;
		/* This call to get_base_rel does the primary work... */
		RelOptInfo *rel = get_base_rel(root, varno);

		result = makeList1(rel);
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		List	   *l;

		foreach(l, f->fromlist)
		{
			result = nconc(result,
						   add_missing_rels_to_query(root, lfirst(l)));
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		result = add_missing_rels_to_query(root, j->larg);
		result = nconc(result,
					   add_missing_rels_to_query(root, j->rarg));
	}
	else
		elog(ERROR, "add_missing_rels_to_query: unexpected node type %d",
			 nodeTag(jtnode));
	return result;
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
 *	  lists belonging to base RelOptInfos.  New base rel entries are created
 *	  as needed.  Also, base RelOptInfos are marked with outerjoinset
 *	  information, to aid in proper positioning of qual clauses that appear
 *	  above outer joins.
 *
 * NOTE: when dealing with inner joins, it is appropriate to let a qual clause
 * be evaluated at the lowest level where all the variables it mentions are
 * available.  However, we cannot push a qual down into the nullable side(s)
 * of an outer join since the qual might eliminate matching rows and cause a
 * NULL row to be incorrectly emitted by the join.  Therefore, rels appearing
 * within the nullable side(s) of an outer join are marked with
 * outerjoinset = list of Relids used at the outer join node.
 * This list will be added to the list of rels referenced by quals using such
 * a rel, thereby forcing them up the join tree to the right level.
 *
 * To ease the calculation of these values, distribute_quals_to_rels() returns
 * the list of Relids involved in its own level of join.  This is just an
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
									true, false, result);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		Relids		leftids,
					rightids;
		bool		isouterjoin;
		List	   *qual;

		/*
		 * Order of operations here is subtle and critical.  First we recurse
		 * to handle sub-JOINs.  Their join quals will be placed without
		 * regard for whether this level is an outer join, which is correct.
		 * Then, if we are an outer join, we mark baserels contained within
		 * the nullable side(s) with our own rel list; this will restrict
		 * placement of subsequent quals using those rels, including our own
		 * quals and quals above us in the join tree.
		 * Finally we place our own join quals.
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
				 * This is where we fail if upper levels of planner haven't
				 * rewritten UNION JOIN as an Append ...
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
									false, isouterjoin, result);
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
		RelOptInfo *rel = get_base_rel(root, lfirsti(relid));

		/*
		 * Since we do this bottom-up, any outer-rels previously marked
		 * should be within the new outer join set.
		 */
		Assert(is_subseti(rel->outerjoinset, outerrels));

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
						Relids qualscope)
{
	RestrictInfo *restrictinfo = makeNode(RestrictInfo);
	Relids		relids;
	List	   *vars;
	bool		can_be_equijoin;

	restrictinfo->clause = (Expr *) clause;
	restrictinfo->eval_cost = -1; /* not computed until needed */
	restrictinfo->subclauseindices = NIL;
	restrictinfo->mergejoinoperator = InvalidOid;
	restrictinfo->left_sortop = InvalidOid;
	restrictinfo->right_sortop = InvalidOid;
	restrictinfo->left_pathkey = NIL; /* not computable yet */
	restrictinfo->right_pathkey = NIL;
	restrictinfo->hashjoinoperator = InvalidOid;
	restrictinfo->left_dispersion = -1; /* not computed until needed */
	restrictinfo->right_dispersion = -1;

	/*
	 * Retrieve all relids and vars contained within the clause.
	 */
	clause_get_relids_vars(clause, &relids, &vars);

	/*
	 * Cross-check: clause should contain no relids not within its scope.
	 * Otherwise the parser messed up.
	 */
	if (! is_subseti(relids, qualscope))
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
	 * For a non-outer-join qual, we can evaluate the qual as soon as
	 * (1) we have all the rels it mentions, and (2) we are at or above any
	 * outer joins that can null any of these rels and are below the syntactic
	 * location of the given qual.  To enforce the latter, scan the base rels
	 * listed in relids, and merge their outer-join lists into the clause's
	 * own reference list.  At the time we are called, the outerjoinset list
	 * of each baserel will show exactly those outer joins that are below the
	 * qual in the join tree.
	 */
	if (isouterjoin)
	{
		relids = qualscope;
		can_be_equijoin = false;
	}
	else
	{
		Relids		newrelids = relids;
		List	   *relid;

		/* We rely on set_unioni to be nondestructive of its input lists... */
		can_be_equijoin = true;
		foreach(relid, relids)
		{
			RelOptInfo *rel = get_base_rel(root, lfirsti(relid));

			if (rel->outerjoinset &&
				! is_subseti(rel->outerjoinset, relids))
			{
				newrelids = set_unioni(newrelids, rel->outerjoinset);
				/*
				 * Because application of the qual will be delayed by outer
				 * join, we mustn't assume its vars are equal everywhere.
				 */
				can_be_equijoin = false;
			}
		}
		relids = newrelids;
		/* Should still be a subset of current scope ... */
		Assert(is_subseti(relids, qualscope));
	}

	/*
	 * Mark the qual as "pushed down" if it can be applied at a level below
	 * its original syntactic level.  This allows us to distinguish original
	 * JOIN/ON quals from higher-level quals pushed down to the same joinrel.
	 * A qual originating from WHERE is always considered "pushed down".
	 */
	restrictinfo->ispusheddown = ispusheddown || !sameseti(relids,
														   qualscope);

	if (length(relids) == 1)
	{

		/*
		 * There is only one relation participating in 'clause', so
		 * 'clause' is a restriction clause for that relation.
		 */
		RelOptInfo *rel = get_base_rel(root, lfirsti(relids));

		rel->baserestrictinfo = lcons(restrictinfo,
									  rel->baserestrictinfo);

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
	}
	else if (relids != NIL)
	{

		/*
		 * 'clause' is a join clause, since there is more than one rel in
		 * the relid list.	Set additional RestrictInfo fields for
		 * joining.
		 *
		 * We don't bother setting the merge/hashjoin info if we're not
		 * going to need it.  We do want to know about mergejoinable ops
		 * in any potential equijoin clause (see later in this routine),
		 * and we ignore enable_mergejoin if isouterjoin is true, because
		 * mergejoin is the only implementation we have for full and right
		 * outer joins.
		 */
		if (enable_mergejoin || isouterjoin || can_be_equijoin)
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
	 * If the clause has a mergejoinable operator, and is not an outer-join
	 * qualification nor bubbled up due to an outer join, then the two sides
	 * represent equivalent PathKeyItems for path keys: any path that is
	 * sorted by one side will also be sorted by the other (as soon as the
	 * two rels are joined, that is).  Record the key equivalence for future
	 * use.
	 */
	if (can_be_equijoin && restrictinfo->mergejoinoperator != InvalidOid)
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
		joininfo = find_joininfo_node(get_base_rel(root, cur_relid),
									  unjoined_relids);
		joininfo->jinfo_restrictinfo = lcons(restrictinfo,
										   joininfo->jinfo_restrictinfo);
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
	Index		irel1;
	Index		irel2;
	RelOptInfo *rel1;
	List	   *restrictlist;
	List	   *itm;
	Oid			ltype,
				rtype;
	Operator	eq_operator;
	Form_pg_operator pgopform;
	Expr	   *clause;

	/*
	 * Currently, since check_mergejoinable only accepts Var = Var clauses,
	 * we should only see Var nodes here.  Would have to work a little
	 * harder to locate the right rel(s) if more-general mergejoin clauses
	 * were accepted.
	 */
	Assert(IsA(item1, Var));
	irel1 = ((Var *) item1)->varno;
	Assert(IsA(item2, Var));
	irel2 = ((Var *) item2)->varno;
	/*
	 * If both vars belong to same rel, we need to look at that rel's
	 * baserestrictinfo list.  If different rels, each will have a
	 * joininfo node for the other, and we can scan either list.
	 */
	rel1 = get_base_rel(root, irel1);
	if (irel1 == irel2)
		restrictlist = rel1->baserestrictinfo;
	else
	{
		JoinInfo   *joininfo = find_joininfo_node(rel1,
												  makeListi1(irel2));

		restrictlist = joininfo->jinfo_restrictinfo;
	}
	/*
	 * Scan to see if equality is already known.
	 */
	foreach(itm, restrictlist)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(itm);
		Node	   *left,
				   *right;

		if (restrictinfo->mergejoinoperator == InvalidOid)
			continue;			/* ignore non-mergejoinable clauses */
		/* We now know the restrictinfo clause is a binary opclause */
		left = (Node *) get_leftop(restrictinfo->clause);
		right = (Node *) get_rightop(restrictinfo->clause);
		if ((equal(item1, left) && equal(item2, right)) ||
			(equal(item2, left) && equal(item1, right)))
			return;				/* found a matching clause */
	}
	/*
	 * This equality is new information, so construct a clause
	 * representing it to add to the query data structures.
	 */
	ltype = exprType(item1);
	rtype = exprType(item2);
	eq_operator = oper("=", ltype, rtype, true);
	if (!HeapTupleIsValid(eq_operator))
	{
		/*
		 * Would it be safe to just not add the equality to the query if
		 * we have no suitable equality operator for the combination of
		 * datatypes?  NO, because sortkey selection may screw up anyway.
		 */
		elog(ERROR, "Unable to identify an equality operator for types '%s' and '%s'",
			 typeidTypeName(ltype), typeidTypeName(rtype));
	}
	pgopform = (Form_pg_operator) GETSTRUCT(eq_operator);
	/*
	 * Let's just make sure this appears to be a compatible operator.
	 */
	if (pgopform->oprlsortop != sortop1 ||
		pgopform->oprrsortop != sortop2 ||
		pgopform->oprresult != BOOLOID)
		elog(ERROR, "Equality operator for types '%s' and '%s' should be mergejoinable, but isn't",
			 typeidTypeName(ltype), typeidTypeName(rtype));

	clause = makeNode(Expr);
	clause->typeOid = BOOLOID;
	clause->opType = OP_EXPR;
	clause->oper = (Node *) makeOper(oprid(eq_operator), /* opno */
									 InvalidOid, /* opid */
									 BOOLOID); /* operator result type */
	clause->args = makeList2(item1, item2);

	ReleaseSysCache(eq_operator);

	/*
	 * Note: we mark the qual "pushed down" to ensure that it can never be
	 * taken for an original JOIN/ON clause.  We also claim it is an outer-
	 * join clause, which it isn't, but that keeps distribute_qual_to_rels
	 * from examining the outerjoinsets of the relevant rels (which are no
	 * longer of interest, but could keep the qual from being pushed down
	 * to where it should be).  It'll also save a useless call to
	 * add_equijoined keys...
	 */
	distribute_qual_to_rels(root, (Node *) clause,
							true, true,
							pull_varnos((Node *) clause));
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
 *	  both operands are simple Vars and the operator is a mergejoinable
 *	  operator.
 */
static void
check_mergejoinable(RestrictInfo *restrictinfo)
{
	Expr	   *clause = restrictinfo->clause;
	Var		   *left,
			   *right;
	Oid			opno,
				leftOp,
				rightOp;

	if (!is_opclause((Node *) clause))
		return;

	left = get_leftop(clause);
	right = get_rightop(clause);

	/* caution: is_opclause accepts more than I do, so check it */
	if (!right)
		return;					/* unary opclauses need not apply */
	if (!IsA(left, Var) ||!IsA(right, Var))
		return;

	opno = ((Oper *) clause->oper)->opno;

	if (op_mergejoinable(opno,
						 left->vartype,
						 right->vartype,
						 &leftOp,
						 &rightOp))
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
 *	  both operands are simple Vars and the operator is a hashjoinable
 *	  operator.
 */
static void
check_hashjoinable(RestrictInfo *restrictinfo)
{
	Expr	   *clause = restrictinfo->clause;
	Var		   *left,
			   *right;
	Oid			opno;

	if (!is_opclause((Node *) clause))
		return;

	left = get_leftop(clause);
	right = get_rightop(clause);

	/* caution: is_opclause accepts more than I do, so check it */
	if (!right)
		return;					/* unary opclauses need not apply */
	if (!IsA(left, Var) ||!IsA(right, Var))
		return;

	opno = ((Oper *) clause->oper)->opno;

	if (op_hashjoinable(opno,
						left->vartype,
						right->vartype))
		restrictinfo->hashjoinoperator = opno;
}
