/*-------------------------------------------------------------------------
 *
 * prepqual.c
 *	  Routines for preprocessing the parse tree qualification
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/prep/prepqual.c,v 1.18 1999/09/07 03:47:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "utils/lsyscache.h"

static Expr *flatten_andors(Expr *qual, bool deep);
static List *pull_ors(List *orlist);
static List *pull_ands(List *andlist);
static Expr *find_nots(Expr *qual);
static Expr *push_nots(Expr *qual);
static Expr *normalize(Expr *qual);
static List *or_normalize(List *orlist);
static List *distribute_args(List *item, List *args);
static List *qual_cleanup(Expr *qual);
static List *remove_duplicates(List *list);

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
 * cnfify
 *	  Convert a qualification to conjunctive normal form by applying
 *	  successive normalizations.
 *
 * Returns the modified qualification.
 *
 * If 'removeAndFlag' is true then it removes explicit AND at the top level,
 * producing a list of implicitly-ANDed conditions.  Otherwise, a normal
 * boolean expression is returned.
 *
 * NOTE: this routine is called by the planner (removeAndFlag = true)
 *		and from the rule manager (removeAndFlag = false).
 *
 */
List *
cnfify(Expr *qual, bool removeAndFlag)
{
	Expr	   *newqual = NULL;

	if (qual != NULL)
	{
		/* Flatten AND and OR groups throughout the tree.
		 * This improvement is always worthwhile.
		 */
		newqual = flatten_andors(qual, true);
		/* Push down NOTs.  We do this only in the top-level boolean
		 * expression, without examining arguments of operators/functions.
		 */
		newqual = find_nots(newqual);
		/* Pushing NOTs could have brought AND/ORs together, so do
		 * another flatten_andors (only in the top level); then normalize.
		 */
		newqual = normalize(flatten_andors(newqual, false));
		/* Do we need a flatten here?  Anyway, clean up after normalize. */
		newqual = (Expr *) qual_cleanup(flatten_andors(newqual, false));
		/* This flatten is almost surely a waste of time... */
		newqual = flatten_andors(newqual, false);

		if (removeAndFlag)
		{
			newqual = (Expr *) make_ands_implicit(newqual);
		}
	}

	return (List *) (newqual);
}

/*
 * find_nots
 *	  Traverse the qualification, looking for 'NOT's to take care of.
 *	  For 'NOT' clauses, apply push_not() to try to push down the 'NOT'.
 *	  For all other clause types, simply recurse.
 *
 * Returns the modified qualification.
 *
 */
static Expr *
find_nots(Expr *qual)
{
	if (qual == NULL)
		return NULL;

#ifdef NOT_USED
	/* recursing into operator expressions is probably not worth it. */
	if (is_opclause((Node *) qual))
	{
		Expr	   *left = (Expr *) get_leftop(qual);
		Expr	   *right = (Expr *) get_rightop(qual);

		if (right)
			return make_clause(qual->opType, qual->oper,
							   lcons(find_nots(left),
									 lcons(find_nots(right),
										   NIL)));
		else
			return make_clause(qual->opType, qual->oper,
							   lcons(find_nots(left),
									 NIL));
	}
#endif
	if (and_clause((Node *) qual))
	{
		List	   *t_list = NIL;
		List	   *temp;

		foreach(temp, qual->args)
			t_list = lappend(t_list, find_nots(lfirst(temp)));
		return make_andclause(t_list);
	}
	else if (or_clause((Node *) qual))
	{
		List	   *t_list = NIL;
		List	   *temp;

		foreach(temp, qual->args)
			t_list = lappend(t_list, find_nots(lfirst(temp)));
		return make_orclause(t_list);
	}
	else if (not_clause((Node *) qual))
		return push_nots(get_notclausearg(qual));
	else
		return qual;
}

/*
 * push_nots
 *	  Push down a 'NOT' as far as possible.
 *
 * Input is an expression to be negated (e.g., the argument of a NOT clause).
 * Returns a new qual equivalent to the negation of the given qual.
 */
static Expr *
push_nots(Expr *qual)
{
	if (qual == NULL)
		return make_notclause(qual); /* XXX is this right?  Or possible? */

	/*
	 * Negate an operator clause if possible: ("NOT" (< A B)) => (> A B)
	 * Otherwise, retain the clause as it is (the 'not' can't be pushed
	 * down any farther).
	 */
	if (is_opclause((Node *) qual))
	{
		Oper	   *oper = (Oper *) ((Expr *) qual)->oper;
		Oid			negator = get_negator(oper->opno);

		if (negator)
		{
			Oper	   *op = (Oper *) makeOper(negator,
											   InvalidOid,
											   oper->opresulttype,
											   0, NULL);
			return make_opclause(op, get_leftop(qual), get_rightop(qual));
		}
		else
			return make_notclause(qual);
	}
	else if (and_clause((Node *) qual))
	{
		/*
		 * Apply DeMorgan's Laws: ("NOT" ("AND" A B)) => ("OR" ("NOT" A)
		 * ("NOT" B)) ("NOT" ("OR" A B)) => ("AND" ("NOT" A) ("NOT" B))
		 * i.e., continue negating down through the clause's descendants.
		 */
		List	   *t_list = NIL;
		List	   *temp;

		foreach(temp, qual->args)
			t_list = lappend(t_list, push_nots(lfirst(temp)));
		return make_orclause(t_list);
	}
	else if (or_clause((Node *) qual))
	{
		List	   *t_list = NIL;
		List	   *temp;

		foreach(temp, qual->args)
			t_list = lappend(t_list, push_nots(lfirst(temp)));
		return make_andclause(t_list);
	}
	else if (not_clause((Node *) qual))
	{
		/*
		 * Another 'not' cancels this 'not', so eliminate the 'not' and
		 * stop negating this branch.  But search the subexpression for
		 * more 'not's to simplify.
		 */
		return find_nots(get_notclausearg(qual));
	}
	else
	{
		/*
		 * We don't know how to negate anything else, place a 'not' at
		 * this level.
		 */
		return make_notclause(qual);
	}
}

/*
 * normalize
 *	  Given a qualification tree with the 'not's pushed down, convert it
 *	  to a tree in CNF by repeatedly applying the rule:
 *				("OR" A ("AND" B C))  => ("AND" ("OR" A B) ("OR" A C))
 *	  bottom-up.
 *	  Note that 'or' clauses will always be turned into 'and' clauses
 *	  if they contain any 'and' subclauses.  XXX this is not always
 *	  an improvement...
 *
 * Returns the modified qualification.
 *
 */
static Expr *
normalize(Expr *qual)
{
	if (qual == NULL)
		return NULL;

	/* We used to recurse into opclauses here, but I see no reason to... */
	if (and_clause((Node *) qual))
	{
		List	   *t_list = NIL;
		List	   *temp;

		foreach(temp, qual->args)
			t_list = lappend(t_list, normalize(lfirst(temp)));
		return make_andclause(t_list);
	}
	else if (or_clause((Node *) qual))
	{
		/* XXX - let form, maybe incorrect */
		List	   *orlist = NIL;
		bool		has_andclause = false;
		List	   *temp;

		foreach(temp, qual->args)
			orlist = lappend(orlist, normalize(lfirst(temp)));
		foreach(temp, orlist)
		{
			if (and_clause(lfirst(temp)))
			{
				has_andclause = true;
				break;
			}
		}
		if (has_andclause)
			return make_andclause(or_normalize(orlist));
		else
			return make_orclause(orlist);
	}
	else if (not_clause((Node *) qual))
		return make_notclause(normalize(get_notclausearg(qual)));
	else
		return qual;
}

/*
 * qual_cleanup
 *	  Fix up a qualification by removing duplicate entries (left over from
 *	  normalization), and by removing 'and' and 'or' clauses which have only
 *	  one remaining subexpr (e.g., ("AND" A) => A).
 *
 * Returns the modified qualification.
 */
static List *
qual_cleanup(Expr *qual)
{
	if (qual == NULL)
		return NIL;

	if (is_opclause((Node *) qual))
	{
		Expr	   *left = (Expr *) get_leftop(qual);
		Expr	   *right = (Expr *) get_rightop(qual);

		if (right)
			return (List *) make_clause(qual->opType, qual->oper,
										lcons(qual_cleanup(left),
											  lcons(qual_cleanup(right),
													NIL)));
		else
			return (List *) make_clause(qual->opType, qual->oper,
										lcons(qual_cleanup(left),
											  NIL));
	}
	else if (and_clause((Node *) qual))
	{
		List	   *t_list = NIL;
		List	   *temp;
		List	   *new_and_args;

		foreach(temp, qual->args)
			t_list = lappend(t_list, qual_cleanup(lfirst(temp)));

		new_and_args = remove_duplicates(t_list);

		if (length(new_and_args) > 1)
			return (List *) make_andclause(new_and_args);
		else
			return lfirst(new_and_args);
	}
	else if (or_clause((Node *) qual))
	{
		List	   *t_list = NIL;
		List	   *temp;
		List	   *new_or_args;

		foreach(temp, qual->args)
			t_list = lappend(t_list, qual_cleanup(lfirst(temp)));

		new_or_args = remove_duplicates(t_list);

		if (length(new_or_args) > 1)
			return (List *) make_orclause(new_or_args);
		else
			return lfirst(new_or_args);
	}
	else if (not_clause((Node *) qual))
		return (List *) make_notclause((Expr *) qual_cleanup((Expr *) get_notclausearg(qual)));
	else
		return (List *) qual;
}

/*--------------------
 * flatten_andors
 *	  Given a qualification, simplify nested AND/OR clauses into flat
 *	  AND/OR clauses with more arguments.
 *
 * The parser regards AND and OR as purely binary operators, so a qual like
 *		(A = 1) OR (A = 2) OR (A = 3) ...
 * will produce a nested parsetree
 *		(OR (A = 1) (OR (A = 2) (OR (A = 3) ...)))
 * In reality, the optimizer and executor regard AND and OR as n-argument
 * operators, so this tree can be flattened to
 *		(OR (A = 1) (A = 2) (A = 3) ...)
 * which is the responsibility of this routine.
 *
 * If 'deep' is true, we search the whole tree for AND/ORs to simplify;
 * if not, we consider only the top-level AND/OR/NOT structure.
 *
 * Returns the rebuilt expr (note original list structure is not touched).
 *--------------------
 */
static Expr *
flatten_andors(Expr *qual, bool deep)
{
	if (qual == NULL)
		return NULL;

	if (and_clause((Node *) qual))
	{
		List	   *out_list = NIL;
		List	   *arg;

		foreach(arg, qual->args)
		{
			Expr   *subexpr = flatten_andors((Expr *) lfirst(arg), deep);

			/*
			 * Note: we can destructively nconc the subexpression's arglist
			 * because we know the recursive invocation of flatten_andors
			 * will have built a new arglist not shared with any other expr.
			 * Otherwise we'd need a listCopy here.
			 */
			if (and_clause((Node *) subexpr))
				out_list = nconc(out_list, subexpr->args);
			else
				out_list = lappend(out_list, subexpr);
		}
		return make_andclause(out_list);
	}
	else if (or_clause((Node *) qual))
	{
		List	   *out_list = NIL;
		List	   *arg;

		foreach(arg, qual->args)
		{
			Expr   *subexpr = flatten_andors((Expr *) lfirst(arg), deep);

			/*
			 * Note: we can destructively nconc the subexpression's arglist
			 * because we know the recursive invocation of flatten_andors
			 * will have built a new arglist not shared with any other expr.
			 * Otherwise we'd need a listCopy here.
			 */
			if (or_clause((Node *) subexpr))
				out_list = nconc(out_list, subexpr->args);
			else
				out_list = lappend(out_list, subexpr);
		}
		return make_orclause(out_list);
	}
	else if (not_clause((Node *) qual))
		return make_notclause(flatten_andors(get_notclausearg(qual), deep));
	else if (deep && is_opclause((Node *) qual))
	{
		Expr	   *left = (Expr *) get_leftop(qual);
		Expr	   *right = (Expr *) get_rightop(qual);

		if (right)
			return make_clause(qual->opType, qual->oper,
							   lcons(flatten_andors(left, deep),
									 lcons(flatten_andors(right, deep),
										   NIL)));
		else
			return make_clause(qual->opType, qual->oper,
							   lcons(flatten_andors(left, deep),
									 NIL));
	}
	else
		return qual;
}

/*
 * pull_ors
 *	  Pull the arguments of an 'or' clause nested within another 'or'
 *	  clause up into the argument list of the parent.
 *
 * Input is the arglist of an OR clause.
 * Returns the rebuilt arglist (note original list structure is not touched).
 */
static List *
pull_ors(List *orlist)
{
	List	   *out_list = NIL;
	List	   *arg;

	foreach(arg, orlist)
	{
		Expr   *subexpr = (Expr *) lfirst(arg);

		/*
		 * Note: we can destructively nconc the subexpression's arglist
		 * because we know the recursive invocation of pull_ors
		 * will have built a new arglist not shared with any other expr.
		 * Otherwise we'd need a listCopy here.
		 */
		if (or_clause((Node *) subexpr))
			out_list = nconc(out_list, pull_ors(subexpr->args));
		else
			out_list = lappend(out_list, subexpr);
	}
	return out_list;
}

/*
 * pull_ands
 *	  Pull the arguments of an 'and' clause nested within another 'and'
 *	  clause up into the argument list of the parent.
 *
 * Returns the modified list.
 */
static List *
pull_ands(List *andlist)
{
	List	   *out_list = NIL;
	List	   *arg;

	foreach(arg, andlist)
	{
		Expr   *subexpr = (Expr *) lfirst(arg);

		/*
		 * Note: we can destructively nconc the subexpression's arglist
		 * because we know the recursive invocation of pull_ands
		 * will have built a new arglist not shared with any other expr.
		 * Otherwise we'd need a listCopy here.
		 */
		if (and_clause((Node *) subexpr))
			out_list = nconc(out_list, pull_ands(subexpr->args));
		else
			out_list = lappend(out_list, subexpr);
	}
	return out_list;
}

/*
 * or_normalize
 *	  Given a list of exprs which are 'or'ed together, distribute any
 *	  'and' clauses.
 *
 * Returns the modified list.
 *
 */
static List *
or_normalize(List *orlist)
{
	List	   *distributable = NIL;
	List	   *new_orlist = NIL;
	List	   *temp = NIL;

	if (orlist == NIL)
		return NIL;

	foreach(temp, orlist)
	{
		if (and_clause(lfirst(temp)))
		{
			distributable = lfirst(temp);
			break;
		}
	}
	if (distributable)
		new_orlist = LispRemove(distributable, orlist);

	if (new_orlist)
	{
		return or_normalize(lcons(distribute_args(lfirst(new_orlist),
										((Expr *) distributable)->args),
								  lnext(new_orlist)));
	}
	else
		return orlist;
}

/*
 * distribute_args
 *	  Create new 'or' clauses by or'ing 'item' with each element of 'args'.
 *	  E.g.: (distribute-args A ("AND" B C)) => ("AND" ("OR" A B) ("OR" A C))
 *
 * Returns an 'and' clause.
 *
 */
static List *
distribute_args(List *item, List *args)
{
	List	   *t_list = NIL;
	List	   *temp;

	if (args == NULL)
		return item;

	foreach(temp, args)
	{
		List	   *n_list;

		n_list = or_normalize(pull_ors(lcons(item,
											 lcons(lfirst(temp),
												   NIL))));
		t_list = lappend(t_list, make_orclause(n_list));
	}
	return (List *) make_andclause(t_list);
}

/*
 * remove_duplicates
 */
static List *
remove_duplicates(List *list)
{
	List	   *result = NIL;
	List	   *i;

	if (length(list) == 1)
		return list;

	foreach(i, list)
	{
		if (! member(lfirst(i), result))
			result = lappend(result, lfirst(i));
	}
	return result;
}
