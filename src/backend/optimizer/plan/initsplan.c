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
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/initsplan.c,v 1.50 2000/09/12 21:06:54 tgl Exp $
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


static void mark_baserels_for_outer_join(Query *root, Relids rels,
										 Relids outerrels);
static void add_restrict_and_join_to_rel(Query *root, Node *clause,
										 bool isjoinqual,
										 Relids outerjoinrelids);
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
 *			select f.x from foo f, foo f2
 *	  is a join of f and f2.  Note that if we have
 *			select foo.x from foo f
 *	  this also gets turned into a join (between foo as foo and foo as f).
 *
 *	  To avoid putting useless entries into the per-relation targetlists,
 *	  this should only be called after all the variables in the targetlist
 *	  and quals have been processed by the routines above.
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
	if (IsA(jtnode, List))
	{
		List	   *l;

		foreach(l, (List *) jtnode)
		{
			result = nconc(result,
						   add_missing_rels_to_query(root, lfirst(l)));
		}
	}
	else if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;
		RelOptInfo *rel = get_base_rel(root, varno);

		/*
		 * If the rel isn't otherwise referenced, give it a dummy
		 * targetlist consisting of its own OID.
		 */
		if (rel->targetlist == NIL)
		{
			Var		   *var = makeVar(varno, ObjectIdAttributeNumber,
									  OIDOID, -1, 0);

			add_var_to_tlist(rel, var);
		}

		result = lcons(rel, NIL);
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
 * add_join_quals_to_rels
 *	  Recursively scan the join tree for JOIN/ON (and JOIN/USING) qual
 *	  clauses, and add these to the appropriate JoinInfo lists.  Also,
 *	  mark base RelOptInfos with outerjoinset information, which will
 *	  be needed for proper placement of WHERE clauses during
 *	  add_restrict_and_join_to_rels().
 *
 * NOTE: when dealing with inner joins, it is appropriate to let a qual clause
 * be evaluated at the lowest level where all the variables it mentions are
 * available.  However, we cannot do this within an outer join since the qual
 * might eliminate matching rows and cause a NULL row to be added improperly.
 * Therefore, rels appearing within (the nullable side of) an outer join
 * are marked with outerjoinset = list of Relids used at the outer join node.
 * This list will be added to the list of rels referenced by quals using
 * such a rel, thereby forcing them up the join tree to the right level.
 *
 * To ease the calculation of these values, add_join_quals_to_rels() returns
 * the list of Relids involved in its own level of join.  This is just an
 * internal convenience; no outside callers pay attention to the result.
 */
Relids
add_join_quals_to_rels(Query *root, Node *jtnode)
{
	Relids		result = NIL;

	if (jtnode == NULL)
		return result;
	if (IsA(jtnode, List))
	{
		List	   *l;

		/*
		 * Note: we assume it's impossible to see same RT index from more
		 * than one subtree, so nconc() is OK rather than LispUnioni().
		 */
		foreach(l, (List *) jtnode)
			result = nconc(result,
						   add_join_quals_to_rels(root, lfirst(l)));
	}
	else if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;

		/* No quals to deal with, just return correct result */
		result = lconsi(varno, NIL);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		Relids		leftids,
					rightids,
					outerjoinids;
		List	   *qual;

		/*
		 * Order of operations here is subtle and critical.  First we recurse
		 * to handle sub-JOINs.  Their join quals will be placed without
		 * regard for whether this level is an outer join, which is correct.
		 * Then, if we are an outer join, we mark baserels contained within
		 * the nullable side(s) with our own rel list; this will restrict
		 * placement of subsequent quals using those rels, including our own
		 * quals, quals above us in the join tree, and WHERE quals.
		 * Finally we place our own join quals.
		 */
		leftids = add_join_quals_to_rels(root, j->larg);
		rightids = add_join_quals_to_rels(root, j->rarg);

		result = nconc(listCopy(leftids), rightids);

		outerjoinids = NIL;
		switch (j->jointype)
		{
			case JOIN_INNER:
				/* Inner join adds no restrictions for quals */
				break;
			case JOIN_LEFT:
				mark_baserels_for_outer_join(root, rightids, result);
				outerjoinids = result;
				break;
			case JOIN_FULL:
				mark_baserels_for_outer_join(root, result, result);
				outerjoinids = result;
				break;
			case JOIN_RIGHT:
				mark_baserels_for_outer_join(root, leftids, result);
				outerjoinids = result;
				break;
			case JOIN_UNION:
				/*
				 * This is where we fail if upper levels of planner haven't
				 * rewritten UNION JOIN as an Append ...
				 */
				elog(ERROR, "UNION JOIN is not implemented yet");
				break;
			default:
				elog(ERROR, "add_join_quals_to_rels: unsupported join type %d",
					 (int) j->jointype);
				break;
		}

		foreach(qual, (List *) j->quals)
			add_restrict_and_join_to_rel(root, (Node *) lfirst(qual),
										 true, outerjoinids);
	}
	else
		elog(ERROR, "add_join_quals_to_rels: unexpected node type %d",
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
 * add_restrict_and_join_to_rels
 *	  Fill RestrictInfo and JoinInfo lists of relation entries for all
 *	  relations appearing within clauses.  Creates new relation entries if
 *	  necessary, adding them to root->base_rel_list.
 *
 * 'clauses': the list of clauses in the cnfify'd query qualification.
 */
void
add_restrict_and_join_to_rels(Query *root, List *clauses)
{
	List	   *clause;

	foreach(clause, clauses)
		add_restrict_and_join_to_rel(root, (Node *) lfirst(clause),
									 false, NIL);
}

/*
 * add_restrict_and_join_to_rel
 *	  Add clause information to either the 'RestrictInfo' or 'JoinInfo' field
 *	  (depending on whether the clause is a join) of each base relation
 *	  mentioned in the clause.	A RestrictInfo node is created and added to
 *	  the appropriate list for each rel.  Also, if the clause uses a
 *	  mergejoinable operator and is not an outer-join qual, enter the left-
 *	  and right-side expressions into the query's lists of equijoined vars.
 *
 * isjoinqual is true if the clause came from JOIN/ON or JOIN/USING;
 * we have to mark the created RestrictInfo accordingly.  If the JOIN
 * is an OUTER join, the caller must set outerjoinrelids = all relids of join,
 * which will override the joinrel identifiers extracted from the clause
 * itself.  For inner join quals and WHERE clauses, set outerjoinrelids = NIL.
 * (Passing the whole list, and not just an "isouterjoin" boolean, is simply
 * a speed optimization: we could extract the same list from the base rels'
 * outerjoinsets, but since add_join_quals_to_rels() already knows what we
 * should use, might as well pass it in instead of recalculating it.)
 */
static void
add_restrict_and_join_to_rel(Query *root, Node *clause,
							 bool isjoinqual,
							 Relids outerjoinrelids)
{
	RestrictInfo *restrictinfo = makeNode(RestrictInfo);
	Relids		relids;
	List	   *vars;
	bool		can_be_equijoin;

	restrictinfo->clause = (Expr *) clause;
	restrictinfo->isjoinqual = isjoinqual;
	restrictinfo->subclauseindices = NIL;
	restrictinfo->mergejoinoperator = InvalidOid;
	restrictinfo->left_sortop = InvalidOid;
	restrictinfo->right_sortop = InvalidOid;
	restrictinfo->hashjoinoperator = InvalidOid;

	/*
	 * Retrieve all relids and vars contained within the clause.
	 */
	clause_get_relids_vars(clause, &relids, &vars);

	/*
	 * If caller has given us a join relid list, use it; otherwise, we must
	 * scan the referenced base rels and add in any outer-join rel lists.
	 * This prevents the clause from being applied at a lower level of joining
	 * than any OUTER JOIN that should be evaluated before it.
	 */
	if (outerjoinrelids)
	{
		/* Safety check: parser should have enforced this to start with */
		if (! is_subseti(relids, outerjoinrelids))
			elog(ERROR, "JOIN qualification may not refer to other relations");
		relids = outerjoinrelids;
		can_be_equijoin = false;
	}
	else
	{
		Relids		newrelids = relids;
		List	   *relid;

		/* We rely on LispUnioni to be nondestructive of its input lists... */
		can_be_equijoin = true;
		foreach(relid, relids)
		{
			RelOptInfo *rel = get_base_rel(root, lfirsti(relid));

			if (rel->outerjoinset)
			{
				newrelids = LispUnioni(newrelids, rel->outerjoinset);
				/*
				 * Because application of the qual will be delayed by outer
				 * join, we mustn't assume its vars are equal everywhere.
				 */
				can_be_equijoin = false;
			}
		}
		relids = newrelids;
	}

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
		 * going to need it.
		 */
		if (enable_mergejoin || can_be_equijoin)
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
		 * attach it.  This means query_planner() screwed up --- it should
		 * treat variable-less clauses separately.
		 */
		elog(ERROR, "add_restrict_and_join_to_rel: can't cope with variable-free clause");
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
												  lconsi(irel2, NIL));

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
	clause->args = lcons(item1, lcons(item2, NIL));

	add_restrict_and_join_to_rel(root, (Node *) clause,
								 false, NIL);
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
