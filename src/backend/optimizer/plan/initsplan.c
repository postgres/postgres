/*-------------------------------------------------------------------------
 *
 * initsplan.c--
 *	  Target list, qualification, joininfo initialization routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/plan/initsplan.c,v 1.21 1999/02/03 20:15:38 momjian Exp $
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

static void add_clause_to_rels(Query *root, List *clause);
static void add_join_info_to_rels(Query *root, RestrictInfo * restrictinfo,
					  List *join_relids);
static void add_vars_to_targetlist(Query *root, List *vars, List *join_relids);

static MergeOrder *mergejoinop(Expr *clause);
static Oid	hashjoinop(Expr *clause);


/*****************************************************************************
 *
 *	 TARGET LISTS
 *
 *****************************************************************************/

/*
 * init-base-rel-tlist--
 *	  Creates rel nodes for every relation mentioned in the target list
 *	  'tlist' (if a node hasn't already been created) and adds them to
 *	  *query-relation-list*.  Creates targetlist entries for each member of
 *	  'tlist' and adds them to the tlist field of the appropriate rel node.
 *
 *	  Returns nothing.
 */
void
init_base_rels_tlist(Query *root, List *tlist)
{
	List	   *tlist_vars = NIL;
	List	   *l = NIL;
	List	   *tvar = NIL;

	foreach(l, tlist)
	{
		TargetEntry *entry = (TargetEntry *) lfirst(l);

		tlist_vars = append(tlist_vars, pull_var_clause(entry->expr));
	}

	/* now, the target list only contains Var nodes */
	foreach(tvar, tlist_vars)
	{
		Var		   *var;
		Index		varno;
		RelOptInfo *result;

		var = (Var *) lfirst(tvar);
		varno = var->varno;
		result = get_base_rel(root, varno);

		add_tl_element(result, var);
	}
}

/*
 * add_missing-vars-to-tlist--
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
		List	   *relids;
		RelOptInfo *result;
		Var		   *var;

		relids = lconsi(varno, NIL);
		if (rte->inFromCl && !rel_member(relids, root->base_rel_list))
		{
			var = makeVar(varno, ObjectIdAttributeNumber,
						  OIDOID, -1, 0, varno, ObjectIdAttributeNumber);
			/* add it to base_rel_list */
			result = get_base_rel(root, varno);
			add_tl_element(result, var);
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
 * init-base-rels-qual--
 *	  Initializes RestrictInfo and JoinInfo fields of relation entries for all
 *	  relations appearing within clauses.  Creates new relation entries if
 *	  necessary, adding them to *query-relation-list*.
 *
 *	  Returns nothing of interest.
 */
void
init_base_rels_qual(Query *root, List *clauses)
{
	List	   *clause;

	foreach(clause, clauses)
		add_clause_to_rels(root, lfirst(clause));
	return;
}

/*
 * add-clause-to-rels--
 *	  Add clause information to either the 'RestrictInfo' or 'JoinInfo' field
 *	  of a relation entry(depending on whether or not the clause is a join)
 *	  by creating a new RestrictInfo node and setting appropriate fields
 *	  within the nodes.
 *
 *	  Returns nothing of interest.
 */
static void
add_clause_to_rels(Query *root, List *clause)
{
	List	   *relids;
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
		RelOptInfo *rel = get_base_rel(root, lfirsti(relids));

		/*
		 * There is only one relation participating in 'clause', so
		 * 'clause' must be a restriction clause.
		 */

		/*
		 * the selectivity of the clause must be computed regardless of
		 * whether it's a restriction or a join clause
		 */
		if (is_funcclause((Node *) clause))
		{

			/*
			 * XXX If we have a func clause set selectivity to 1/3, really
			 * need a true selectivity function.
			 */
			restrictinfo->selectivity = (Cost) 0.3333333;
		}
		else
		{
			restrictinfo->selectivity =
				compute_clause_selec(root, (Node *) clause, NIL);
		}
		rel->restrictinfo = lcons(restrictinfo, rel->restrictinfo);
	}
	else
	{

		/*
		 * 'clause' is a join clause, since there is more than one atom in
		 * the relid list.
		 */

		if (is_funcclause((Node *) clause))
		{

			/*
			 * XXX If we have a func clause set selectivity to 1/3, really
			 * need a true selectivity function.
			 */
			restrictinfo->selectivity = (Cost) 0.3333333;
		}
		else
		{
			restrictinfo->selectivity =
				compute_clause_selec(root, (Node *) clause, NIL);
		}
		add_join_info_to_rels(root, restrictinfo, relids);
		/* we are going to be doing a join, so add var to targetlist */
		add_vars_to_targetlist(root, vars, relids);
	}
}

/*
 * add-join-info-to-rels--
 *	  For every relation participating in a join clause, add 'restrictinfo' to
 *	  the appropriate joininfo node(creating a new one and adding it to the
 *	  appropriate rel node if necessary).
 *
 * 'restrictinfo' describes the join clause
 * 'join-relids' is the list of relations participating in the join clause
 *
 * Returns nothing.
 *
 */
static void
add_join_info_to_rels(Query *root, RestrictInfo * restrictinfo, List *join_relids)
{
	List	   *join_relid;

	foreach(join_relid, join_relids)
	{
		JoinInfo   *joininfo;
		List	   *other_rels = NIL;
		List	   *rel;

		foreach(rel, join_relids)
		{
			if (lfirsti(rel) != lfirsti(join_relid))
				other_rels = lappendi(other_rels, lfirsti(rel));
		}

		joininfo = find_joininfo_node(get_base_rel(root, lfirsti(join_relid)),
									  other_rels);
		joininfo->jinfo_restrictinfo =
			lcons(copyObject((void *) restrictinfo), joininfo->jinfo_restrictinfo);

	}
}

/*
 * add-vars-to-targetlist--
 *	  For each variable appearing in a clause,
 *	  (1) If a targetlist entry for the variable is not already present in
 *		  the appropriate relation's target list, add one.
 *	  (2) If a targetlist entry is already present, but the var is part of a
 *		  join clause, add the relids of the join relations to the JoinList
 *		  entry of the targetlist entry.
 *
 *	  'vars' is the list of var nodes
 *	  'join-relids' is the list of relids appearing in the join clause
 *		(if this is a join clause)
 *
 *	  Returns nothing.
 */
static void
add_vars_to_targetlist(Query *root, List *vars, List *join_relids)
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
			add_tl_element(rel, var);
	}
}

/*****************************************************************************
 *
 *	 JOININFO
 *
 *****************************************************************************/

/*
 * init-join-info--
 *	  Set the MergeJoinable or HashJoinable field for every joininfo node
 *	  (within a rel node) and the MergeJoinOrder or HashJoinOp field for
 *	  each restrictinfo node(within a joininfo node) for all relations in a
 *	  query.
 *
 *	  Returns nothing.
 */
void
init_join_info(List *rel_list)
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
					if (_enable_hashjoin_)
						hashop = hashjoinop(clause);

					if (sortop)
					{
						restrictinfo->mergejoinorder = sortop;
						joininfo->mergejoinable = true;
					}
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
 * mergejoinop--
 *	  Returns the mergejoin operator of an operator iff 'clause' is
 *	  mergejoinable, i.e., both operands are single vars and the operator is
 *	  a mergejoinable operator.
 */
static MergeOrder *
mergejoinop(Expr *clause)
{
	Oid			leftOp,
				rightOp;
	bool		sortable;

	sortable = op_mergejoinable(((Oper *) clause->oper)->opno,
								(get_leftop(clause))->vartype,
								(get_rightop(clause))->vartype,
								&leftOp,
								&rightOp);

	if (sortable)
	{
		MergeOrder *morder = makeNode(MergeOrder);

		morder->join_operator = ((Oper *) clause->oper)->opno;
		morder->left_operator = leftOp;
		morder->right_operator = rightOp;
		morder->left_type = (get_leftop(clause))->vartype;
		morder->right_type = (get_rightop(clause))->vartype;
		return morder;
	}
	else
		return NULL;
}

/*
 * hashjoinop--
 *	  Returns the hashjoin operator of an operator iff 'clause' is
 *	  hashjoinable, i.e., both operands are single vars and the operator is
 *	  a hashjoinable operator.
 */
static Oid
hashjoinop(Expr *clause)
{
	return (op_hashjoinable(((Oper *) clause->oper)->opno,
							(get_leftop(clause))->vartype,
							(get_rightop(clause))->vartype));
}
