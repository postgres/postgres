/*-------------------------------------------------------------------------
 *
 * initsplan.c
 *	  Target list, qualification, joininfo initialization routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/initsplan.c,v 1.29 1999/02/22 05:26:21 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/relation.h"
#include "nodes/makefuncs.h"

#include "access/htup.h"

#include "catalog/pg_type.h"

#include "utils/lsyscache.h"
#include "utils/palloc.h"

#include "optimizer/internal.h"
#include "optimizer/planmain.h"
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"

extern int	Quiet;

static void add_restrict_and_join_to_rel(Query *root, List *clause);
static void add_join_info_to_rels(Query *root, RestrictInfo *restrictinfo,
					  Relids join_relids);
static void add_vars_to_targetlist(Query *root, List *vars, Relids join_relids);

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
	List	   *tlist_vars = NIL;
	List	   *l = NIL;
	List	   *tvar = NIL;

	foreach(l, tlist)
	{
		TargetEntry *entry = (TargetEntry *) lfirst(l);

		tlist_vars = nconc(tlist_vars, pull_var_clause(entry->expr));
	}

	/* now, the target list only contains Var nodes */
	foreach(tvar, tlist_vars)
	{
		Var		   *var = (Var *) lfirst(tvar);
		Index		varno;
		RelOptInfo *result;

		varno = var->varno;
		result = get_base_rel(root, varno);

		add_var_to_tlist(result, var);
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
	List	   *l;
	int			varno;

	varno = 1;
	foreach(l, root->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);
		Relids		relids;
		RelOptInfo *result;
		Var		   *var;

		relids = lconsi(varno, NIL);
		if (rte->inFromCl && !rel_member(relids, root->base_rel_list))
		{
			var = makeVar(varno, ObjectIdAttributeNumber,
						  OIDOID, -1, 0, varno, ObjectIdAttributeNumber);
			/* add it to base_rel_list */
			result = get_base_rel(root, varno);
			add_var_to_tlist(result, var);
		}
		pfree(relids);
		varno++;
	}

	return;
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
 *	  Returns nothing of interest.
 */
void
add_restrict_and_join_to_rels(Query *root, List *clauses)
{
	List	   *clause;

	foreach(clause, clauses)
		add_restrict_and_join_to_rel(root, lfirst(clause));
	return;
}

/*
 * add_restrict_and_join_to_rel-
 *	  Add clause information to either the 'RestrictInfo' or 'JoinInfo' field
 *	  of a relation entry(depending on whether or not the clause is a join)
 *	  by creating a new RestrictInfo node and setting appropriate fields
 *	  within the nodes.
 *
 *	  Returns nothing of interest.
 */
static void
add_restrict_and_join_to_rel(Query *root, List *clause)
{
	Relids		relids;
	List	   *vars;
	RestrictInfo *restrictinfo = makeNode(RestrictInfo);

	/*
	 * Retrieve all relids and vars contained within the clause.
	 */
	clause_get_relids_vars((Node *) clause, &relids, &vars);

	restrictinfo->clause = (Expr *) clause;
	restrictinfo->notclause = contains_not((Node *) clause);
	restrictinfo->selectivity = 0;
	restrictinfo->indexids = NIL;
	restrictinfo->mergejoinorder = (MergeOrder *) NULL;
	restrictinfo->hashjoinoperator = (Oid) 0;

	if (length(relids) == 1)
	{
		/*
		 * There is only one relation participating in 'clause', so
		 * 'clause' must be a restriction clause.
		 */
		RelOptInfo *rel = get_base_rel(root, lfirsti(relids));

		/*
		 * The selectivity of the clause must be computed regardless of
		 * whether it's a restriction or a join clause
		 */
		if (is_funcclause((Node *) clause))

			/*
			 * XXX If we have a func clause set selectivity to 1/3, really
			 * need a true selectivity function.
			 */
			restrictinfo->selectivity = (Cost) 0.3333333;
		else
			restrictinfo->selectivity = compute_clause_selec(root, (Node *) clause, NIL);

		rel->restrictinfo = lcons(restrictinfo, rel->restrictinfo);
	}
	else
	{
		/*
		 * 'clause' is a join clause, since there is more than one atom in
		 * the relid list.
		 */
		if (is_funcclause((Node *) clause))
			/*
			 * XXX If we have a func clause set selectivity to 1/3, really
			 * need a true selectivity function.
			 */
			restrictinfo->selectivity = (Cost) 0.3333333;
		else
			restrictinfo->selectivity = compute_clause_selec(root, (Node *) clause, NIL);

		add_join_info_to_rels(root, restrictinfo, relids);
		/* we are going to be doing a join, so add var to targetlist */
		add_vars_to_targetlist(root, vars, relids);
	}
}

/*
 * add_join_info_to_rels
 *	  For every relation participating in a join clause, add 'restrictinfo' to
 *	  the appropriate joininfo node(creating a new one and adding it to the
 *	  appropriate rel node if necessary).
 *
 * 'restrictinfo' describes the join clause
 * 'join_relids' is the list of relations participating in the join clause
 *
 */
static void
add_join_info_to_rels(Query *root, RestrictInfo *restrictinfo,
						Relids join_relids)
{
	List	   *join_relid;

	/* For every relid, find the rel, and add the proper join entries */
	foreach(join_relid, join_relids)
	{
		JoinInfo   *joininfo;
		Relids		unjoined_relids = NIL;
		List 		*rel;

		/* Get the relids not equal to the current relid */
		foreach(rel, join_relids)
		{
			if (lfirsti(rel) != lfirsti(join_relid))
				unjoined_relids = lappendi(unjoined_relids, lfirsti(rel));
		}

		joininfo = find_joininfo_node(get_base_rel(root, lfirsti(join_relid)),
									  unjoined_relids);
		joininfo->jinfo_restrictinfo = lcons(copyObject((void *) restrictinfo),
											 joininfo->jinfo_restrictinfo);
	}
}

/*
 * add_vars_to_targetlist
 *	  For each variable appearing in a clause,
 *	  (1) If a targetlist entry for the variable is not already present in
 *		  the appropriate relation's target list, add one.
 *	  (2) If a targetlist entry is already present, but the var is part of a
 *		  join clause, add the relids of the join relations to the JoinList
 *		  entry of the targetlist entry.
 *
 *	  'vars' is the list of var nodes
 *	  'join_relids' is the list of relids appearing in the join clause
 *		(if this is a join clause)
 *
 *	  Returns nothing.
 */
static void
add_vars_to_targetlist(Query *root, List *vars, Relids join_relids)
{
	Var		   *var;
	List	   *temp = NIL;
	RelOptInfo *rel = (RelOptInfo *) NULL;
	TargetEntry *tlistentry;

	foreach(temp, vars)
	{
		var = (Var *) lfirst(temp);
		rel = get_base_rel(root, var->varno);
		tlistentry = tlistentry_member(var, rel->targetlist);
		if (tlistentry == NULL)
			/* add a new entry */
			add_var_to_tlist(rel, var);
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
 *	  each restrictinfo node(within a joininfo node) for all relations in a
 *	  query.
 *
 *	  Returns nothing.
 */
void
set_joininfo_mergeable_hashable(List *rel_list)
{
	List	   *x,
			   *y,
			   *z;
	RelOptInfo *rel;
	JoinInfo   *joininfo;
	RestrictInfo *restrictinfo;
	Expr	   *clause;

	foreach(x, rel_list)
	{
		rel = (RelOptInfo *) lfirst(x);
		foreach(y, rel->joininfo)
		{
			joininfo = (JoinInfo *) lfirst(y);
			foreach(z, joininfo->jinfo_restrictinfo)
			{
				restrictinfo = (RestrictInfo *) lfirst(z);
				clause = restrictinfo->clause;
				if (is_joinable((Node *) clause))
				{
					MergeOrder *sortop = (MergeOrder *) NULL;
					Oid			hashop = (Oid) NULL;

					if (_enable_mergejoin_)
						sortop = mergejoinop(clause);
					if (sortop)
					{
						restrictinfo->mergejoinorder = sortop;
						joininfo->mergejoinable = true;
					}

					if (_enable_hashjoin_)
						hashop = hashjoinop(clause);
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

/*
 * mergejoinop
 *	  Returns the mergejoin operator of an operator iff 'clause' is
 *	  mergejoinable, i.e., both operands are single vars and the operator is
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

	if (!is_opclause((Node*) clause))
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
 *	  Returns the hashjoin operator of an operator iff 'clause' is
 *	  hashjoinable, i.e., both operands are single vars and the operator is
 *	  a hashjoinable operator.
 */
static Oid
hashjoinop(Expr *clause)
{
	Var		   *left,
			   *right;

	if (!is_opclause((Node*) clause))
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
