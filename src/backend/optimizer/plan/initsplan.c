/*-------------------------------------------------------------------------
 *
 * initsplan.c
 *	  Target list, qualification, joininfo initialization routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/initsplan.c,v 1.36 1999/08/10 03:00:14 tgl Exp $
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

static MergeOrder *mergejoinop(Expr *clause);
static Oid	hashjoinop(Expr *clause);


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
 * add_restrict_and_join_to_rels-
 *	  Initializes RestrictInfo and JoinInfo fields of relation entries for all
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
 * add_restrict_and_join_to_rel-
 *	  Add clause information to either the 'RestrictInfo' or 'JoinInfo' field
 *	  of a relation entry (depending on whether or not the clause is a join)
 *	  by creating a new RestrictInfo node and setting appropriate fields
 *	  within the nodes.
 */
static void
add_restrict_and_join_to_rel(Query *root, Node *clause)
{
	RestrictInfo *restrictinfo = makeNode(RestrictInfo);
	Relids		relids;
	List	   *vars;

	restrictinfo->clause = (Expr *) clause;
	restrictinfo->indexids = NIL;
	restrictinfo->mergejoinorder = (MergeOrder *) NULL;
	restrictinfo->hashjoinoperator = (Oid) 0;

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
 *	  the appropriate joininfo node (creating a new one and adding it to the
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
		JoinInfo   *joininfo;
		Relids		unjoined_relids = NIL;
		List	   *rel;

		/* Get the relids not equal to the current relid */
		foreach(rel, join_relids)
		{
			if (lfirsti(rel) != lfirsti(join_relid))
				unjoined_relids = lappendi(unjoined_relids, lfirsti(rel));
		}

		/*
		 * Find or make the joininfo node for this combination of rels
		 */
		joininfo = find_joininfo_node(get_base_rel(root, lfirsti(join_relid)),
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
 *	  Set the MergeJoinable or HashJoinable field for every joininfo node
 *	  (within a rel node) and the mergejoinorder or hashjoinop field for
 *	  each restrictinfo node (within a joininfo node) for all relations in a
 *	  query.
 *
 *	  Returns nothing.
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
				Expr	   *clause = restrictinfo->clause;

				if (is_joinable((Node *) clause))
				{
					if (_enable_mergejoin_)
					{
						MergeOrder *sortop = mergejoinop(clause);
						if (sortop)
						{
							restrictinfo->mergejoinorder = sortop;
							joininfo->mergejoinable = true;
						}
					}

					if (_enable_hashjoin_)
					{
						Oid			hashop = hashjoinop(clause);
						if (hashop)
						{
							restrictinfo->hashjoinoperator = hashop;
							joininfo->hashjoinable = true;
						}
					}
				}
			}
		}
	}
}

/*
 * mergejoinop
 *	  Returns a MergeOrder node for 'clause' iff 'clause' is mergejoinable,
 *	  i.e., both operands are single vars and the operator is
 *	  a mergejoinable operator.
 */
static MergeOrder *
mergejoinop(Expr *clause)
{
	Var		   *left,
			   *right;
	Oid			opno,
				leftOp,
				rightOp;
	bool		sortable;

	if (!is_opclause((Node *) clause))
		return NULL;

	left = get_leftop(clause);
	right = get_rightop(clause);

	/* caution: is_opclause accepts more than I do, so check it */
	if (!right)
		return NULL;			/* unary opclauses need not apply */
	if (!IsA(left, Var) || !IsA(right, Var))
		return NULL;

	opno = ((Oper *) clause->oper)->opno;

	sortable = op_mergejoinable(opno,
								left->vartype,
								right->vartype,
								&leftOp,
								&rightOp);

	if (sortable)
	{
		MergeOrder *morder = makeNode(MergeOrder);

		morder->join_operator = opno;
		morder->left_operator = leftOp;
		morder->right_operator = rightOp;
		morder->left_type = left->vartype;
		morder->right_type = right->vartype;
		return morder;
	}
	else
		return NULL;
}

/*
 * hashjoinop
 *	  Returns the hashjoin operator iff 'clause' is hashjoinable,
 *	  i.e., both operands are single vars and the operator is
 *	  a hashjoinable operator.
 */
static Oid
hashjoinop(Expr *clause)
{
	Var		   *left,
			   *right;

	if (!is_opclause((Node *) clause))
		return InvalidOid;

	left = get_leftop(clause);
	right = get_rightop(clause);

	/* caution: is_opclause accepts more than I do, so check it */
	if (!right)
		return InvalidOid;		/* unary opclauses need not apply */
	if (!IsA(left, Var) || !IsA(right, Var))
		return InvalidOid;

	return op_hashjoinable(((Oper *) clause->oper)->opno,
						   left->vartype,
						   right->vartype);
}
