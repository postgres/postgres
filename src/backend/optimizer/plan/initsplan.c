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
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/initsplan.c,v 1.46 2000/04/12 17:15:21 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"
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
#include "utils/lsyscache.h"


static void add_restrict_and_join_to_rel(Query *root, Node *clause);
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
 * make_var_only_tlist
 *	  Creates rel nodes for every relation mentioned in the target list
 *	  'tlist' (if a node hasn't already been created) and adds them to
 *	  *query_relation_list*.  Creates targetlist entries for each member of
 *	  'tlist' and adds them to the tlist field of the appropriate rel node.
 */
void
make_var_only_tlist(Query *root, List *tlist)
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

/*
 * add_missing_rels_to_query
 *
 *	  If we have a range variable in the FROM clause that does not appear
 *	  in the target list nor qualifications, we must add it to the base
 *	  relation list so that it will be joined.	For instance, "select f.x
 *	  from foo f, foo f2" is a join of f and f2.  Note that if we have
 *	  "select foo.x from foo f", it also gets turned into a join (between
 *	  foo as foo and foo as f).
 *
 *	  To avoid putting useless entries into the per-relation targetlists,
 *	  this should only be called after all the variables in the targetlist
 *	  and quals have been processed by the routines above.
 */
void
add_missing_rels_to_query(Query *root)
{
	int			varno = 1;
	List	   *l;

	foreach(l, root->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);

		if (rte->inJoinSet)
		{
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
		}
		varno++;
	}
}

/*****************************************************************************
 *
 *	  QUALIFICATIONS
 *
 *****************************************************************************/



/*
 * add_restrict_and_join_to_rels
 *	  Fill RestrictInfo and JoinInfo lists of relation entries for all
 *	  relations appearing within clauses.  Creates new relation entries if
 *	  necessary, adding them to *query_relation_list*.
 *
 * 'clauses': the list of clauses in the cnfify'd query qualification.
 */
void
add_restrict_and_join_to_rels(Query *root, List *clauses)
{
	List	   *clause;

	foreach(clause, clauses)
		add_restrict_and_join_to_rel(root, (Node *) lfirst(clause));
}

/*
 * add_restrict_and_join_to_rel
 *	  Add clause information to either the 'RestrictInfo' or 'JoinInfo' field
 *	  (depending on whether the clause is a join) of each base relation
 *	  mentioned in the clause.	A RestrictInfo node is created and added to
 *	  the appropriate list for each rel.  Also, if the clause uses a
 *	  mergejoinable operator, enter the left- and right-side expressions
 *	  into the query's lists of equijoined vars.
 */
static void
add_restrict_and_join_to_rel(Query *root, Node *clause)
{
	RestrictInfo *restrictinfo = makeNode(RestrictInfo);
	Relids		relids;
	List	   *vars;

	restrictinfo->clause = (Expr *) clause;
	restrictinfo->subclauseindices = NIL;
	restrictinfo->mergejoinoperator = InvalidOid;
	restrictinfo->left_sortop = InvalidOid;
	restrictinfo->right_sortop = InvalidOid;
	restrictinfo->hashjoinoperator = InvalidOid;

	/*
	 * Retrieve all relids and vars contained within the clause.
	 */
	clause_get_relids_vars(clause, &relids, &vars);

	if (length(relids) == 1)
	{

		/*
		 * There is only one relation participating in 'clause', so
		 * 'clause' must be a restriction clause for that relation.
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
		check_mergejoinable(restrictinfo);
	}
	else
	{

		/*
		 * 'clause' is a join clause, since there is more than one atom in
		 * the relid list.	Set additional RestrictInfo fields for
		 * joining.
		 *
		 * We need the merge info whether or not mergejoin is enabled (for
		 * constructing equijoined-var lists), but we don't bother setting
		 * hash info if hashjoin is disabled.
		 */
		check_mergejoinable(restrictinfo);
		if (enable_hashjoin)
			check_hashjoinable(restrictinfo);

		/*
		 * Add clause to the join lists of all the relevant relations.
		 * (If, perchance, 'clause' contains NO vars, then nothing will
		 * happen...)
		 */
		add_join_info_to_rels(root, restrictinfo, relids);

		/*
		 * Add vars used in the join clause to targetlists of member
		 * relations, so that they will be emitted by the plan nodes that
		 * scan those relations (else they won't be available at the join
		 * node!).
		 */
		add_vars_to_targetlist(root, vars);
	}

	/*
	 * If the clause has a mergejoinable operator, then the two sides
	 * represent equivalent PathKeyItems for path keys: any path that is
	 * sorted by one side will also be sorted by the other (after joining,
	 * that is).  Record the key equivalence for future use.
	 */
	if (restrictinfo->mergejoinoperator != InvalidOid)
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
