/*-------------------------------------------------------------------------
 *
 * initsplan.c
 *	  Target list, qualification, joininfo initialization routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/initsplan.c,v 1.37 1999/08/16 02:17:54 tgl Exp $
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
	List	   *tlist_vars = pull_var_clause((Node *) tlist);

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
 * add_missing_vars_to_tlist
 *	  If we have range variable(s) in the FROM clause that does not appear
 *	  in the target list nor qualifications, we add it to the base relation
 *	  list. For instance, "select f.x from foo f, foo f2" is a join of f and
 *	  f2. Note that if we have "select foo.x from foo f", it also gets turned
 *	  into a join.
 */
void
add_missing_vars_to_tlist(Query *root, List *tlist)
{
	int			varno = 1;
	List	   *l;

	foreach(l, root->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);
		Relids		relids;

		relids = lconsi(varno, NIL);
		if (rte->inFromCl && !rel_member(relids, root->base_rel_list))
		{
			RelOptInfo *rel;
			Var		   *var;

			/* add it to base_rel_list */
			rel = get_base_rel(root, varno);
			/* give it a dummy tlist entry for its OID */
			var = makeVar(varno, ObjectIdAttributeNumber,
						  OIDOID, -1, 0, varno, ObjectIdAttributeNumber);
			add_var_to_tlist(rel, var);
		}
		pfree(relids);
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
		add_restrict_and_join_to_rel(root, (Node*) lfirst(clause));
}

/*
 * add_restrict_and_join_to_rel
 *	  Add clause information to either the 'RestrictInfo' or 'JoinInfo' field
 *	  (depending on whether the clause is a join) of each base relation
 *	  mentioned in the clause.  A RestrictInfo node is created and added to
 *	  the appropriate list for each rel.
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
	 * The selectivity of the clause must be computed regardless of
	 * whether it's a restriction or a join clause
	 */
	restrictinfo->selectivity = compute_clause_selec(root, clause);

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

		rel->restrictinfo = lcons(restrictinfo, rel->restrictinfo);
	}
	else
	{
		/*
		 * 'clause' is a join clause, since there is more than one atom in
		 * the relid list.  Add it to the join lists of all the relevant
		 * relations.  (If, perchance, 'clause' contains NO vars, then
		 * nothing will happen...)
		 */
		add_join_info_to_rels(root, restrictinfo, relids);
		/* we are going to be doing a join, so add vars to targetlists */
		add_vars_to_targetlist(root, vars);
	}
}

/*
 * add_join_info_to_rels
 *	  For every relation participating in a join clause, add 'restrictinfo' to
 *	  the appropriate joininfo list (creating a new one and adding it to the
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
		JoinInfo   *joininfo;
		Relids		unjoined_relids = NIL;
		List	   *otherrel;

		/* Get the relids not equal to the current relid */
		foreach(otherrel, join_relids)
		{
			if (lfirsti(otherrel) != cur_relid)
				unjoined_relids = lappendi(unjoined_relids, lfirsti(otherrel));
		}

		/*
		 * Find or make the joininfo node for this combination of rels
		 */
		joininfo = find_joininfo_node(get_base_rel(root, cur_relid),
									  unjoined_relids);

		/*
		 * And add the restrictinfo node to it.  NOTE that each joininfo
		 * gets its own copy of the restrictinfo node!  (Is this really
		 * necessary?  Possibly ... later parts of the optimizer destructively
		 * modify restrict/join clauses...)
		 */
		joininfo->jinfo_restrictinfo = lcons(copyObject((void *) restrictinfo),
											 joininfo->jinfo_restrictinfo);
	}
}

/*****************************************************************************
 *
 *	 JOININFO
 *
 *****************************************************************************/

/*
 * set_joininfo_mergeable_hashable
 *	  Examine each join clause used in a query and set the merge and hash
 *	  info fields in those that are mergejoinable or hashjoinable.
 */
void
set_joininfo_mergeable_hashable(List *rel_list)
{
	List	   *x;

	foreach(x, rel_list)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(x);
		List	   *y;

		foreach(y, rel->joininfo)
		{
			JoinInfo   *joininfo = (JoinInfo *) lfirst(y);
			List	   *z;

			foreach(z, joininfo->jinfo_restrictinfo)
			{
				RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(z);

				if (_enable_mergejoin_)
					check_mergejoinable(restrictinfo);
				if (_enable_hashjoin_)
					check_hashjoinable(restrictinfo);
			}
		}
	}
}

/*
 * check_mergejoinable
 *	  If the restrictinfo's clause is mergejoinable, set the mergejoin
 *	  info fields in the restrictinfo.
 *
 *	  Currently, we support mergejoin for binary opclauses where
 *	  both operands are simple Vars and the operator is a mergejoinable
 *	  operator.  (Note: since we are only examining clauses that were
 *	  classified as joins, it is certain that the two Vars belong to
 *	  different relations... if we accepted more general clause structures
 *	  we might need to check that the two sides refer to different rels...)
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

	if (! is_opclause((Node *) clause))
		return;

	left = get_leftop(clause);
	right = get_rightop(clause);

	/* caution: is_opclause accepts more than I do, so check it */
	if (! right)
		return;					/* unary opclauses need not apply */
	if (!IsA(left, Var) || !IsA(right, Var))
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
 *	  operator.  (Note: since we are only examining clauses that were
 *	  classified as joins, it is certain that the two Vars belong to
 *	  different relations... if we accepted more general clause structures
 *	  we might need to check that the two sides refer to different rels...)
 */
static void
check_hashjoinable(RestrictInfo *restrictinfo)
{
	Expr	   *clause = restrictinfo->clause;
	Var		   *left,
			   *right;
	Oid			opno;

	if (! is_opclause((Node *) clause))
		return;

	left = get_leftop(clause);
	right = get_rightop(clause);

	/* caution: is_opclause accepts more than I do, so check it */
	if (! right)
		return;					/* unary opclauses need not apply */
	if (!IsA(left, Var) || !IsA(right, Var))
		return;

	opno = ((Oper *) clause->oper)->opno;

	if (op_hashjoinable(opno,
						left->vartype,
						right->vartype))
	{
		restrictinfo->hashjoinoperator = opno;
	}
}
