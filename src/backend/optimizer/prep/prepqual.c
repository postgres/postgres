/*-------------------------------------------------------------------------
 *
 * prepqual.c--
 *	  Routines for preprocessing the parse tree qualification
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/prep/prepqual.c,v 1.4 1997/09/07 04:44:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/makefuncs.h"

#include "optimizer/internal.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"

#include "utils/lsyscache.h"

static Expr    *pull_args(Expr * qual);
static List    *pull_ors(List * orlist);
static List    *pull_ands(List * andlist);
static Expr    *find_nots(Expr * qual);
static Expr    *push_nots(Expr * qual);
static Expr    *normalize(Expr * qual);
static List    *or_normalize(List * orlist);
static List    *distribute_args(List * item, List * args);
static List    *qualcleanup(Expr * qual);
static List    *remove_ands(Expr * qual);
static List    *remove_duplicates(List * list);

/*
 * preprocess-qualification--
 *	  Driver routine for modifying the parse tree qualification.
 *
 * Returns the new base qualification and the existential qualification
 * in existentialQualPtr.
 *
 *	XXX right now, update_clauses() does nothing so
 *		preprocess-qualification simply converts the qual in conjunctive
 *		normal form  (see cnfify() below )
 */
List		   *
preprocess_qualification(Expr * qual, List * tlist, List ** existentialQualPtr)
{
	List		   *cnf_qual = cnfify(qual, true);

/*
	List *existential_qual =
		update_clauses(intCons(_query_result_relation_,
								update_relations(tlist)),
					   cnf_qual,
					   _query_command_type_);
	if (existential_qual) {
		*existentialQualPtr = existential_qual;
		return set_difference(cnf_qual, existential_qual);
	} else {
		*existentialQualPtr = NIL;
		return cnf_qual;
	}
*/
	/* update_clauses() is not working right now */
	*existentialQualPtr = NIL;
	return cnf_qual;

}

/*****************************************************************************
 *
 *		CNF CONVERSION ROUTINES
 *
 *		NOTES:
 *		The basic algorithms for normalizing the qualification are taken
 *		from ingres/source/qrymod/norml.c
 *
 *		Remember that the initial qualification may consist of ARBITRARY
 *		combinations of clauses.  In addition, before this routine is called,
 *		the qualification will contain explicit "AND"s.
 *
 *****************************************************************************/


/*
 * cnfify--
 *	  Convert a qualification to conjunctive normal form by applying
 *	  successive normalizations.
 *
 * Returns the modified qualification with an extra level of nesting.
 *
 * If 'removeAndFlag' is true then it removes the explicit ANDs.
 *
 * NOTE: this routine is called by the planner (removeAndFlag = true)
 *		and from the rule manager (removeAndFlag = false).
 *
 */
List		   *
cnfify(Expr * qual, bool removeAndFlag)
{
	Expr		   *newqual = NULL;

	if (qual != NULL)
	{
		newqual = find_nots(pull_args(qual));
		newqual = normalize(pull_args(newqual));
		newqual = (Expr *) qualcleanup(pull_args(newqual));
		newqual = pull_args(newqual);;

		if (removeAndFlag)
		{
			if (and_clause((Node *) newqual))
				newqual = (Expr *) remove_ands(newqual);
			else
				newqual = (Expr *) remove_ands(make_andclause(lcons(newqual, NIL)));
		}
	}
	else if (qual != NULL)
		newqual = (Expr *) lcons(qual, NIL);

	return (List *) (newqual);
}

/*
 * pull-args--
 *	  Given a qualification, eliminate nested 'and' and 'or' clauses.
 *
 * Returns the modified qualification.
 *
 */
static Expr    *
pull_args(Expr * qual)
{
	if (qual == NULL)
		return (NULL);

	if (is_opclause((Node *) qual))
	{
		return (make_clause(qual->opType, qual->oper,
							lcons(pull_args((Expr *) get_leftop(qual)),
							 lcons(pull_args((Expr *) get_rightop(qual)),
								   NIL))));
	}
	else if (and_clause((Node *) qual))
	{
		List		   *temp = NIL;
		List		   *t_list = NIL;

		foreach(temp, qual->args)
			t_list = lappend(t_list, pull_args(lfirst(temp)));
		return (make_andclause(pull_ands(t_list)));
	}
	else if (or_clause((Node *) qual))
	{
		List		   *temp = NIL;
		List		   *t_list = NIL;

		foreach(temp, qual->args)
			t_list = lappend(t_list, pull_args(lfirst(temp)));
		return (make_orclause(pull_ors(t_list)));
	}
	else if (not_clause((Node *) qual))
	{
		return (make_notclause(pull_args(get_notclausearg(qual))));
	}
	else
	{
		return (qual);
	}
}

/*
 * pull-ors--
 *	  Pull the arguments of an 'or' clause nested within another 'or'
 *	  clause up into the argument list of the parent.
 *
 * Returns the modified list.
 */
static List    *
pull_ors(List * orlist)
{
	if (orlist == NIL)
		return (NIL);

	if (or_clause(lfirst(orlist)))
	{
		List		   *args = ((Expr *) lfirst(orlist))->args;

		return (pull_ors(nconc(copyObject((Node *) args),
							   copyObject((Node *) lnext(orlist)))));
	}
	else
	{
		return (lcons(lfirst(orlist), pull_ors(lnext(orlist))));
	}
}

/*
 * pull-ands--
 *	  Pull the arguments of an 'and' clause nested within another 'and'
 *	  clause up into the argument list of the parent.
 *
 * Returns the modified list.
 */
static List    *
pull_ands(List * andlist)
{
	if (andlist == NIL)
		return (NIL);

	if (and_clause(lfirst(andlist)))
	{
		List		   *args = ((Expr *) lfirst(andlist))->args;

		return (pull_ands(nconc(copyObject((Node *) args),
								copyObject((Node *) lnext(andlist)))));
	}
	else
	{
		return (lcons(lfirst(andlist), pull_ands(lnext(andlist))));
	}
}

/*
 * find-nots--
 *	  Traverse the qualification, looking for 'not's to take care of.
 *	  For 'not' clauses, remove the 'not' and push it down to the clauses'
 *	  descendants.
 *	  For all other clause types, simply recurse.
 *
 * Returns the modified qualification.
 *
 */
static Expr    *
find_nots(Expr * qual)
{
	if (qual == NULL)
		return (NULL);

	if (is_opclause((Node *) qual))
	{
		return (make_clause(qual->opType, qual->oper,
							lcons(find_nots((Expr *) get_leftop(qual)),
							 lcons(find_nots((Expr *) get_rightop(qual)),
								   NIL))));
	}
	else if (and_clause((Node *) qual))
	{
		List		   *temp = NIL;
		List		   *t_list = NIL;

		foreach(temp, qual->args)
		{
			t_list = lappend(t_list, find_nots(lfirst(temp)));
		}

		return (make_andclause(t_list));
	}
	else if (or_clause((Node *) qual))
	{
		List		   *temp = NIL;
		List		   *t_list = NIL;

		foreach(temp, qual->args)
		{
			t_list = lappend(t_list, find_nots(lfirst(temp)));
		}
		return (make_orclause(t_list));
	}
	else if (not_clause((Node *) qual))
		return (push_nots(get_notclausearg(qual)));
	else
		return (qual);
}

/*
 * push-nots--
 *	  Negate the descendants of a 'not' clause.
 *
 * Returns the modified qualification.
 *
 */
static Expr    *
push_nots(Expr * qual)
{
	if (qual == NULL)
		return (NULL);

	/*
	 * Negate an operator clause if possible: ("NOT" (< A B)) => (> A B)
	 * Otherwise, retain the clause as it is (the 'not' can't be pushed
	 * down any farther).
	 */
	if (is_opclause((Node *) qual))
	{
		Oper		   *oper = (Oper *) ((Expr *) qual)->oper;
		Oid				negator = get_negator(oper->opno);

		if (negator)
		{
			Oper		   *op = (Oper *) makeOper(negator,
												   InvalidOid,
												   oper->opresulttype,
												   0, NULL);

			op->op_fcache = (FunctionCache *) NULL;
			return
				(make_opclause(op, get_leftop(qual), get_rightop(qual)));
		}
		else
		{
			return (make_notclause(qual));
		}
	}
	else if (and_clause((Node *) qual))
	{

		/*
		 * Apply DeMorgan's Laws: ("NOT" ("AND" A B)) => ("OR" ("NOT" A)
		 * ("NOT" B)) ("NOT" ("OR" A B)) => ("AND" ("NOT" A) ("NOT" B))
		 * i.e., continue negating down through the clause's descendants.
		 */
		List		   *temp = NIL;
		List		   *t_list = NIL;

		foreach(temp, qual->args)
		{
			t_list = lappend(t_list, push_nots(lfirst(temp)));
		}
		return (make_orclause(t_list));
	}
	else if (or_clause((Node *) qual))
	{
		List		   *temp = NIL;
		List		   *t_list = NIL;

		foreach(temp, qual->args)
		{
			t_list = lappend(t_list, push_nots(lfirst(temp)));
		}
		return (make_andclause(t_list));
	}
	else if (not_clause((Node *) qual))

		/*
		 * Another 'not' cancels this 'not', so eliminate the 'not' and
		 * stop negating this branch.
		 */
		return (find_nots(get_notclausearg(qual)));
	else

		/*
		 * We don't know how to negate anything else, place a 'not' at
		 * this level.
		 */
		return (make_notclause(qual));
}

/*
 * normalize--
 *	  Given a qualification tree with the 'not's pushed down, convert it
 *	  to a tree in CNF by repeatedly applying the rule:
 *				("OR" A ("AND" B C))  => ("AND" ("OR" A B) ("OR" A C))
 *	  bottom-up.
 *	  Note that 'or' clauses will always be turned into 'and' clauses.
 *
 * Returns the modified qualification.
 *
 */
static Expr    *
normalize(Expr * qual)
{
	if (qual == NULL)
		return (NULL);

	if (is_opclause((Node *) qual))
	{
		Expr		   *expr = (Expr *) qual;

		return (make_clause(expr->opType, expr->oper,
							lcons(normalize((Expr *) get_leftop(qual)),
							 lcons(normalize((Expr *) get_rightop(qual)),
								   NIL))));
	}
	else if (and_clause((Node *) qual))
	{
		List		   *temp = NIL;
		List		   *t_list = NIL;

		foreach(temp, qual->args)
		{
			t_list = lappend(t_list, normalize(lfirst(temp)));
		}
		return (make_andclause(t_list));
	}
	else if (or_clause((Node *) qual))
	{
		/* XXX - let form, maybe incorrect */
		List		   *orlist = NIL;
		List		   *temp = NIL;
		bool			has_andclause = FALSE;

		foreach(temp, qual->args)
		{
			orlist = lappend(orlist, normalize(lfirst(temp)));
		}
		foreach(temp, orlist)
		{
			if (and_clause(lfirst(temp)))
			{
				has_andclause = TRUE;
				break;
			}
		}
		if (has_andclause == TRUE)
			return (make_andclause(or_normalize(orlist)));
		else
			return (make_orclause(orlist));

	}
	else if (not_clause((Node *) qual))
		return (make_notclause(normalize(get_notclausearg(qual))));
	else
		return (qual);
}

/*
 * or-normalize--
 *	  Given a list of exprs which are 'or'ed together, distribute any
 *	  'and' clauses.
 *
 * Returns the modified list.
 *
 */
static List    *
or_normalize(List * orlist)
{
	List		   *distributable = NIL;
	List		   *new_orlist = NIL;
	List		   *temp = NIL;

	if (orlist == NIL)
		return NIL;

	foreach(temp, orlist)
	{
		if (and_clause(lfirst(temp)))
			distributable = lfirst(temp);
	}
	if (distributable)
		new_orlist = LispRemove(distributable, orlist);

	if (new_orlist)
	{
		return
			(or_normalize(lcons(distribute_args(lfirst(new_orlist),
										 ((Expr *) distributable)->args),
								lnext(new_orlist))));
	}
	else
	{
		return (orlist);
	}
}

/*
 * distribute-args--
 *	  Create new 'or' clauses by or'ing 'item' with each element of 'args'.
 *	  E.g.: (distribute-args A ("AND" B C)) => ("AND" ("OR" A B) ("OR" A C))
 *
 * Returns an 'and' clause.
 *
 */
static List    *
distribute_args(List * item, List * args)
{
	List		   *or_list = NIL;
	List		   *n_list = NIL;
	List		   *temp = NIL;
	List		   *t_list = NIL;

	if (args == NULL)
		return (item);

	foreach(temp, args)
	{
		n_list = or_normalize(pull_ors(lcons(item,
											 lcons(lfirst(temp), NIL))));
		or_list = (List *) make_orclause(n_list);
		t_list = lappend(t_list, or_list);
	}
	return ((List *) make_andclause(t_list));
}

/*
 * qualcleanup--
 *	  Fix up a qualification by removing duplicate entries (left over from
 *	  normalization), and by removing 'and' and 'or' clauses which have only
 *	  one valid expr (e.g., ("AND" A) => A).
 *
 * Returns the modified qualfication.
 *
 */
static List    *
qualcleanup(Expr * qual)
{
	if (qual == NULL)
		return (NIL);

	if (is_opclause((Node *) qual))
	{
		return ((List *) make_clause(qual->opType, qual->oper,
							lcons(qualcleanup((Expr *) get_leftop(qual)),
						   lcons(qualcleanup((Expr *) get_rightop(qual)),
								 NIL))));
	}
	else if (and_clause((Node *) qual))
	{
		List		   *temp = NIL;
		List		   *t_list = NIL;
		List		   *new_and_args = NIL;

		foreach(temp, qual->args)
			t_list = lappend(t_list, qualcleanup(lfirst(temp)));

		new_and_args = remove_duplicates(t_list);

		if (length(new_and_args) > 1)
			return ((List *) make_andclause(new_and_args));
		else
			return (lfirst(new_and_args));
	}
	else if (or_clause((Node *) qual))
	{
		List		   *temp = NIL;
		List		   *t_list = NIL;
		List		   *new_or_args = NIL;

		foreach(temp, qual->args)
			t_list = lappend(t_list, qualcleanup(lfirst(temp)));

		new_or_args = remove_duplicates(t_list);


		if (length(new_or_args) > 1)
			return ((List *) make_orclause(new_or_args));
		else
			return (lfirst(new_or_args));
	}
	else if (not_clause((Node *) qual))
		return ((List *) make_notclause((Expr *) qualcleanup((Expr *) get_notclausearg(qual))));

	else
		return ((List *) qual);
}

/*
 * remove-ands--
 *	  Remove the explicit "AND"s from the qualification:
 *				("AND" A B) => (A B)
 *
 * RETURNS : qual
 * MODIFIES: qual
 */
static List    *
remove_ands(Expr * qual)
{
	List		   *t_list = NIL;

	if (qual == NULL)
		return (NIL);
	if (is_opclause((Node *) qual))
	{
		return ((List *) make_clause(qual->opType, qual->oper,
							lcons(remove_ands((Expr *) get_leftop(qual)),
						   lcons(remove_ands((Expr *) get_rightop(qual)),
								 NIL))));
	}
	else if (and_clause((Node *) qual))
	{
		List		   *temp = NIL;

		foreach(temp, qual->args)
			t_list = lappend(t_list, remove_ands(lfirst(temp)));
		return (t_list);
	}
	else if (or_clause((Node *) qual))
	{
		List		   *temp = NIL;

		foreach(temp, qual->args)
			t_list = lappend(t_list, remove_ands(lfirst(temp)));
		return ((List *) make_orclause((List *) t_list));
	}
	else if (not_clause((Node *) qual))
	{
		return ((List *) make_notclause((Expr *) remove_ands((Expr *) get_notclausearg(qual))));
	}
	else
	{
		return ((List *) qual);
	}
}

/*****************************************************************************
 *
 *		EXISTENTIAL QUALIFICATIONS
 *
 *****************************************************************************/

/*
 * update-relations--
 *	  Returns the range table indices (i.e., varnos) for all relations which
 *	  are referenced in the target list.
 *
 */
#ifdef NOT_USED
static List    *
update_relations(List * tlist)
{
	return (NIL);
}

#endif

/*****************************************************************************
 *
 *
 *
 *****************************************************************************/

static List    *
remove_duplicates(List * list)
{
	List		   *i;
	List		   *j;
	List		   *result = NIL;
	bool			there_exists_duplicate = false;

	if (length(list) == 1)
		return (list);

	foreach(i, list)
	{
		if (i != NIL)
		{
			foreach(j, lnext(i))
			{
				if (equal(lfirst(i), lfirst(j)))
					there_exists_duplicate = true;
			}
			if (!there_exists_duplicate)
				result = lappend(result, lfirst(i));

			there_exists_duplicate = false;
		}
	}
	return (result);
}
