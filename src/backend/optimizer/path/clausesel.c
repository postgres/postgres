/*-------------------------------------------------------------------------
 *
 * clausesel.c
 *	  Routines to compute and set clause selectivities
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/clausesel.c,v 1.27 2000/01/09 00:26:31 tgl Exp $
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
 *		ROUTINES TO COMPUTE SELECTIVITIES
 ****************************************************************************/

/*
 * restrictlist_selec -
 *	  Compute the selectivity of an implicitly-ANDed list of RestrictInfo
 *	  clauses.
 *
 * This is the same as clauselist_selec except for the form of the input.
 */
Selectivity
restrictlist_selec(Query *root, List *restrictinfo_list)
{
	List	   *clauselist = get_actual_clauses(restrictinfo_list);
	Selectivity	result;

	result = clauselist_selec(root, clauselist);
	freeList(clauselist);
	return result;
}

/*
 * clauselist_selec -
 *	  Compute the selectivity of an implicitly-ANDed list of boolean
 *	  expression clauses.
 */
Selectivity
clauselist_selec(Query *root, List *clauses)
{
	Selectivity		s1 = 1.0;
	List		   *clause;

	/* Use the product of the selectivities of the subclauses.
	 * XXX this is probably too optimistic, since the subclauses
	 * are very likely not independent...
	 */
	foreach(clause, clauses)
	{
		Selectivity	s2 = compute_clause_selec(root, (Node *) lfirst(clause));
		s1 = s1 * s2;
	}
	return s1;
}

/*
 * compute_clause_selec -
 *	  Compute the selectivity of a general boolean expression clause.
 */
Selectivity
compute_clause_selec(Query *root, Node *clause)
{
	Selectivity		s1 = 1.0;	/* default for any unhandled clause type */

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
		 */
		s1 = restriction_selectivity(F_EQSEL,
									 BooleanEqualOperator,
									 getrelid(((Var *) clause)->varno,
											  root->rtable),
									 ((Var *) clause)->varattno,
									 Int8GetDatum(true),
									 SEL_CONSTANT | SEL_RIGHT);
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
		s1 = clauselist_selec(root, ((Expr *) clause)->args);
	}
	else if (or_clause(clause))
	{
		/*
		 * Selectivities for an 'or' clause are computed as s1+s2 - s1*s2
		 * to account for the probable overlap of selected tuple sets.
		 * XXX is this too conservative?
		 */
		List   *arg;
		s1 = 0.0;
		foreach(arg, ((Expr *) clause)->args)
		{
			Selectivity	s2 = compute_clause_selec(root, (Node *) lfirst(arg));
			s1 = s1 + s2 - s1 * s2;
		}
	}
	else if (is_opclause(clause))
	{
		if (NumRelids(clause) == 1)
		{
			/* The opclause is not a join clause, since there is only one
			 * relid in the clause.  The clause selectivity will be based on
			 * the operator selectivity and operand values.
			 */
			Oid			opno = ((Oper *) ((Expr *) clause)->oper)->opno;
			RegProcedure oprrest = get_oprrest(opno);

			/*
			 * if the oprrest procedure is missing for whatever reason, use a
			 * selectivity of 0.5
			 */
			if (!oprrest)
				s1 = (Selectivity) 0.5;
			else
			{
				int			relidx;
				AttrNumber	attno;
				Datum		constval;
				int			flag;
				Oid			reloid;

				get_relattval(clause, 0, &relidx, &attno, &constval, &flag);
				reloid = relidx ? getrelid(relidx, root->rtable) : InvalidOid;
				s1 = restriction_selectivity(oprrest, opno,
											 reloid, attno,
											 constval, flag);
			}
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

			/*
			 * if the oprjoin procedure is missing for whatever reason, use a
			 * selectivity of 0.5
			 */
			if (!oprjoin)
				s1 = (Selectivity) 0.5;
			else
			{
				int			relid1,
							relid2;
				AttrNumber	attno1,
							attno2;
				Oid			reloid1,
							reloid2;

				get_rels_atts(clause, &relid1, &attno1, &relid2, &attno2);
				reloid1 = relid1 ? getrelid(relid1, root->rtable) : InvalidOid;
				reloid2 = relid2 ? getrelid(relid2, root->rtable) : InvalidOid;
				s1 = join_selectivity(oprjoin, opno,
									  reloid1, attno1,
									  reloid2, attno2);
			}
		}
	}
	else if (is_funcclause(clause))
	{
		/*
		 * This is not an operator, so we guess at the selectivity. THIS
		 * IS A HACK TO GET V4 OUT THE DOOR.  FUNCS SHOULD BE ABLE TO HAVE
		 * SELECTIVITIES THEMSELVES.	   -- JMH 7/9/92
		 */
		s1 = (Selectivity) 0.3333333;
	}
	else if (is_subplan(clause))
	{
		/*
		 * Just for the moment! FIX ME! - vadim 02/04/98
		 */
		s1 = 1.0;
	}

	return s1;
}
