/*-------------------------------------------------------------------------
 *
 * clausesel.c
 *	  Routines to compute and set clause selectivities
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/clausesel.c,v 1.24 1999/07/24 23:21:09 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/internal.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"


/****************************************************************************
 *		ROUTINES TO SET CLAUSE SELECTIVITIES
 ****************************************************************************/

/*
 * set_clause_selectivities -
 *	  Sets the selectivity field for each clause in 'restrictinfo-list'
 *	  to 'new-selectivity'.  If the selectivity has already been set,
 *	  change it only if the new one is better.
 */
void
set_clause_selectivities(List *restrictinfo_list, Cost new_selectivity)
{
	List	   *rlist;

	foreach(rlist, restrictinfo_list)
	{
		RestrictInfo *clausenode = (RestrictInfo *) lfirst(rlist);
		Cost		cost_clause = clausenode->selectivity;

		if (cost_clause <= 0 || new_selectivity < cost_clause)
			clausenode->selectivity = new_selectivity;
	}
}

/*
 * product_selec -
 *	  Multiplies the selectivities of each clause in 'restrictinfo-list'.
 *
 * Returns a flonum corresponding to the selectivity of 'restrictinfo-list'.
 */
Cost
product_selec(List *restrictinfo_list)
{
	Cost		result = (Cost) 1.0;
	List	   *rlist;

	foreach(rlist, restrictinfo_list)
	{
		result *= ((RestrictInfo *) lfirst(rlist))->selectivity;
	}
	return result;
}

/*
 * set_rest_relselec -
 *	  Scans through clauses on each relation and assigns a selectivity to
 *	  those clauses that haven't been assigned a selectivity by an index.
 *
 * MODIFIES: selectivities of the various rel's restrictinfo slots.
 */
void
set_rest_relselec(Query *root, List *rel_list)
{
	List	   *x;

	foreach(x, rel_list)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(x);
		set_rest_selec(root, rel->restrictinfo);
	}
}

/*
 * set_rest_selec -
 *	  Sets the selectivity fields for those clauses within a single
 *	  relation's 'restrictinfo-list' that haven't already been set.
 */
void
set_rest_selec(Query *root, List *restrictinfo_list)
{
	List	   *rlist;

	foreach(rlist, restrictinfo_list)
	{
		RestrictInfo *clause = (RestrictInfo *) lfirst(rlist);

		if (clause->selectivity <= 0)
		{
			clause->selectivity =
				compute_clause_selec(root, (Node *) clause->clause);
		}
	}
}

/****************************************************************************
 *		ROUTINES TO COMPUTE SELECTIVITIES
 ****************************************************************************/

/*
 * compute_clause_selec -
 *	  Computes the selectivity of a clause.
 */
Cost
compute_clause_selec(Query *root, Node *clause)
{
	Cost		s1 = 1.0;		/* default for any unhandled clause type */

	if (clause == NULL)
		return s1;
	if (IsA(clause, Var))
	{
		/*
		 * we have a bool Var.	This is exactly equivalent to the clause:
		 * reln.attribute = 't' so we compute the selectivity as if that
		 * is what we have. The magic #define constants are a hack.  I
		 * didn't want to have to do system cache look ups to find out all
		 * of that info.
		 *
		 * XXX why are we using varno and varoattno?  Seems like it should
		 * be varno/varattno or varnoold/varoattno, not mix & match...
		 */
		Oid			relid = getrelid(((Var *) clause)->varno,
									 root->rtable);

		s1 = restriction_selectivity(F_EQSEL,
									 BooleanEqualOperator,
									 relid,
									 ((Var *) clause)->varoattno,
									 "t",
									 _SELEC_CONSTANT_RIGHT_);
	}
	else if (IsA(clause, Param))
	{
		/* XXX any way to do better? */
		s1 = 1.0;
	}
	else if (IsA(clause, Const))
	{
		/* bool constant is pretty easy... */
		s1 = ((bool) ((Const *) clause)->constvalue) ? 1.0 : 0.0;
	}
	else if (not_clause(clause))
	{
		/* inverse of the selectivity of the underlying clause */
		s1 = 1.0 - compute_clause_selec(root,
										(Node *) get_notclausearg((Expr *) clause));
	}
	else if (and_clause(clause))
	{
		/* Use the product of the selectivities of the subclauses.
		 * XXX this is probably too optimistic, since the subclauses
		 * are very likely not independent...
		 */
		List   *arg;
		s1 = 1.0;
		foreach(arg, ((Expr *) clause)->args)
		{
			Cost		s2 = compute_clause_selec(root, (Node *) lfirst(arg));
			s1 = s1 * s2;
		}
	}
	else if (or_clause(clause))
	{
		/* Selectivities for an 'or' clause are computed as s1+s2 - s1*s2
		 * to account for the probable overlap of selected tuple sets.
		 * XXX is this too conservative?
		 */
		List   *arg;
		s1 = 0.0;
		foreach(arg, ((Expr *) clause)->args)
		{
			Cost		s2 = compute_clause_selec(root, (Node *) lfirst(arg));
			s1 = s1 + s2 - s1 * s2;
		}
	}
	else if (is_funcclause(clause))
	{
		/*
		 * This is not an operator, so we guess at the selectivity. THIS
		 * IS A HACK TO GET V4 OUT THE DOOR.  FUNCS SHOULD BE ABLE TO HAVE
		 * SELECTIVITIES THEMSELVES.	   -- JMH 7/9/92
		 */
		s1 = (Cost) 0.3333333;
	}
	else if (is_subplan(clause))
	{
		/*
		 * Just for the moment! FIX ME! - vadim 02/04/98
		 */
		s1 = 1.0;
	}
	else if (is_opclause(clause))
	{
		if (NumRelids(clause) == 1)
		{
			/* The clause is not a join clause, since there is only one
			 * relid in the clause.  The clause selectivity will be based on
			 * the operator selectivity and operand values.
			 */
			Oid			opno = ((Oper *) ((Expr *) clause)->oper)->opno;
			RegProcedure oprrest = get_oprrest(opno);
			Oid			relid;
			int			relidx;
			AttrNumber	attno;
			Datum		constval;
			int			flag;

			get_relattval(clause, &relidx, &attno, &constval, &flag);
			relid = getrelid(relidx, root->rtable);

			/*
			 * if the oprrest procedure is missing for whatever reason, use a
			 * selectivity of 0.5
			 */
			if (!oprrest)
				s1 = (Cost) 0.5;
			else if (attno == InvalidAttrNumber)
			{
				/*
				 * attno can be Invalid if the clause had a function in it,
				 * i.e.   WHERE myFunc(f) = 10
				 */
				/* this should be FIXED somehow to use function selectivity */
				s1 = (Cost) (0.5);
			}
			else
				s1 = (Cost) restriction_selectivity(oprrest,
													opno,
													relid,
													attno,
													(char *) constval,
													flag);
		}
		else
		{
			/*
			 * The clause must be a join clause.  The clause selectivity will
			 * be based on the relations to be scanned and the attributes they
			 * are to be joined on.
			 */
			Oid			opno = ((Oper *) ((Expr *) clause)->oper)->opno;
			RegProcedure oprjoin = get_oprjoin(opno);
			int			relid1,
						relid2;
			AttrNumber	attno1,
						attno2;

			get_rels_atts(clause, &relid1, &attno1, &relid2, &attno2);
			relid1 = getrelid(relid1, root->rtable);
			relid2 = getrelid(relid2, root->rtable);

			/*
			 * if the oprjoin procedure is missing for whatever reason, use a
			 * selectivity of 0.5
			 */
			if (!oprjoin)
				s1 = (Cost) (0.5);
			else
				s1 = (Cost) join_selectivity(oprjoin,
											 opno,
											 relid1,
											 attno1,
											 relid2,
											 attno2);
		}
	}

	return s1;
}
