/*-------------------------------------------------------------------------
 *
 * prepqual.c
 *	  Routines for preprocessing qualification expressions
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/prep/prepqual.c,v 1.19 1999/09/12 18:08:17 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "utils/lsyscache.h"

static Expr *flatten_andors(Expr *qual);
static List *pull_ors(List *orlist);
static List *pull_ands(List *andlist);
static Expr *find_nots(Expr *qual);
static Expr *push_nots(Expr *qual);
static Expr *find_ors(Expr *qual);
static Expr *or_normalize(List *orlist);
static Expr *find_ands(Expr *qual);
static Expr *and_normalize(List *andlist);

/*****************************************************************************
 *
 *		CNF/DNF CONVERSION ROUTINES
 *
 *		These routines convert an arbitrary boolean expression into
 *		conjunctive normal form or disjunctive normal form.
 *
 *		The result of these routines differs from a "true" CNF/DNF in that
 *		we do not bother to detect common subexpressions; e.g., ("AND" A A)
 *		does not get simplified to A.  Testing for identical subexpressions
 *		is a waste of time if the query is written intelligently, and it
 *		takes an unreasonable amount of time if there are many subexpressions
 *		(since it's roughly O(N^2) in the number of subexpressions).
 *
 *		Because of that restriction, it would be unwise to apply dnfify()
 *		to the result of cnfify() or vice versa.  Instead apply both to
 *		the original user-written qual expression.
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
 * producing a list of implicitly-ANDed conditions.  Otherwise, a regular
 * boolean expression is returned.  Since most callers pass 'true', we
 * prefer to declare the result as List *, not Expr *.
 */
List *
cnfify(Expr *qual, bool removeAndFlag)
{
	Expr	   *newqual;

	if (qual == NULL)
		return NIL;

	/* Flatten AND and OR groups throughout the tree.
	 * This improvement is always worthwhile.
	 */
	newqual = flatten_andors(qual);
	/* Push down NOTs.  We do this only in the top-level boolean
	 * expression, without examining arguments of operators/functions.
	 */
	newqual = find_nots(newqual);
	/* Normalize into conjunctive normal form. */
	newqual = find_ors(newqual);

	if (removeAndFlag)
	{
		newqual = (Expr *) make_ands_implicit(newqual);
	}

	return (List *) newqual;
}

/*
 * dnfify
 *	  Convert a qualification to disjunctive normal form by applying
 *	  successive normalizations.
 *
 * Returns the modified qualification.
 *
 * We do not offer a 'removeOrFlag' in this case; the usages are
 * different.
 */
Expr *
dnfify(Expr *qual)
{
	Expr	   *newqual;

	if (qual == NULL)
		return NULL;

	/* Flatten AND and OR groups throughout the tree.
	 * This improvement is always worthwhile.
	 */
	newqual = flatten_andors(qual);
	/* Push down NOTs.  We do this only in the top-level boolean
	 * expression, without examining arguments of operators/functions.
	 */
	newqual = find_nots(newqual);
	/* Normalize into disjunctive normal form. */
	newqual = find_ands(newqual);

	return newqual;
}

/*--------------------
 * The parser regards AND and OR as purely binary operators, so a qual like
 *		(A = 1) OR (A = 2) OR (A = 3) ...
 * will produce a nested parsetree
 *		(OR (A = 1) (OR (A = 2) (OR (A = 3) ...)))
 * In reality, the optimizer and executor regard AND and OR as n-argument
 * operators, so this tree can be flattened to
 *		(OR (A = 1) (A = 2) (A = 3) ...)
 * which is the responsibility of the routines below.
 *
 * flatten_andors() does the basic transformation with no initial assumptions.
 * pull_ands() and pull_ors() are used to maintain flatness of the AND/OR
 * tree after local transformations that might introduce nested AND/ORs.
 *--------------------
 */

/*--------------------
 * flatten_andors
 *	  Given a qualification, simplify nested AND/OR clauses into flat
 *	  AND/OR clauses with more arguments.
 *
 * Returns the rebuilt expr (note original list structure is not touched).
 *--------------------
 */
static Expr *
flatten_andors(Expr *qual)
{
	if (qual == NULL)
		return NULL;

	if (and_clause((Node *) qual))
	{
		List	   *out_list = NIL;
		List	   *arg;

		foreach(arg, qual->args)
		{
			Expr   *subexpr = flatten_andors((Expr *) lfirst(arg));

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
			Expr   *subexpr = flatten_andors((Expr *) lfirst(arg));

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
		return make_notclause(flatten_andors(get_notclausearg(qual)));
	else if (is_opclause((Node *) qual))
	{
		Expr	   *left = (Expr *) get_leftop(qual);
		Expr	   *right = (Expr *) get_rightop(qual);

		if (right)
			return make_clause(qual->opType, qual->oper,
							   lcons(flatten_andors(left),
									 lcons(flatten_andors(right),
										   NIL)));
		else
			return make_clause(qual->opType, qual->oper,
							   lcons(flatten_andors(left),
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
 * find_nots
 *	  Traverse the qualification, looking for 'NOT's to take care of.
 *	  For 'NOT' clauses, apply push_not() to try to push down the 'NOT'.
 *	  For all other clause types, simply recurse.
 *
 * Returns the modified qualification.  AND/OR flatness is preserved.
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
		return make_andclause(pull_ands(t_list));
	}
	else if (or_clause((Node *) qual))
	{
		List	   *t_list = NIL;
		List	   *temp;

		foreach(temp, qual->args)
			t_list = lappend(t_list, find_nots(lfirst(temp)));
		return make_orclause(pull_ors(t_list));
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
		/*--------------------
		 * Apply DeMorgan's Laws:
		 *		("NOT" ("AND" A B)) => ("OR" ("NOT" A) ("NOT" B))
		 *		("NOT" ("OR" A B))  => ("AND" ("NOT" A) ("NOT" B))
		 * i.e., swap AND for OR and negate all the subclauses.
		 *--------------------
		 */
		List	   *t_list = NIL;
		List	   *temp;

		foreach(temp, qual->args)
			t_list = lappend(t_list, push_nots(lfirst(temp)));
		return make_orclause(pull_ors(t_list));
	}
	else if (or_clause((Node *) qual))
	{
		List	   *t_list = NIL;
		List	   *temp;

		foreach(temp, qual->args)
			t_list = lappend(t_list, push_nots(lfirst(temp)));
		return make_andclause(pull_ands(t_list));
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
 * find_ors
 *	  Given a qualification tree with the 'not's pushed down, convert it
 *	  to a tree in CNF by repeatedly applying the rule:
 *				("OR" A ("AND" B C))  => ("AND" ("OR" A B) ("OR" A C))
 *
 *	  Note that 'or' clauses will always be turned into 'and' clauses
 *	  if they contain any 'and' subclauses.
 *
 * Returns the modified qualification.  AND/OR flatness is preserved.
 */
static Expr *
find_ors(Expr *qual)
{
	if (qual == NULL)
		return NULL;

	/* We used to recurse into opclauses here, but I see no reason to... */
	if (and_clause((Node *) qual))
	{
		List	   *andlist = NIL;
		List	   *temp;

		foreach(temp, qual->args)
			andlist = lappend(andlist, find_ors(lfirst(temp)));
		return make_andclause(pull_ands(andlist));
	}
	else if (or_clause((Node *) qual))
	{
		List	   *orlist = NIL;
		List	   *temp;

		foreach(temp, qual->args)
			orlist = lappend(orlist, find_ors(lfirst(temp)));
		return or_normalize(pull_ors(orlist));
	}
	else if (not_clause((Node *) qual))
		return make_notclause(find_ors(get_notclausearg(qual)));
	else
		return qual;
}

/*
 * or_normalize
 *	  Given a list of exprs which are 'or'ed together, try to apply
 *	  the distributive law
 *				("OR" A ("AND" B C))  => ("AND" ("OR" A B) ("OR" A C))
 *	  to convert the top-level OR clause to a top-level AND clause.
 *
 * Returns the resulting expression (could be an AND clause, an OR
 * clause, or maybe even a single subexpression).
 */
static Expr *
or_normalize(List *orlist)
{
	Expr	   *distributable = NULL;
	int			num_subclauses = 1;
	List	   *andclauses = NIL;
	List	   *temp;

	if (orlist == NIL)
		return NULL;			/* probably can't happen */
	if (lnext(orlist) == NIL)
		return lfirst(orlist);	/* single-expression OR (can this happen?) */

	/*
	 * If we have a choice of AND clauses, pick the one with the
	 * most subclauses.  Because we initialized num_subclauses = 1,
	 * any AND clauses with only one arg will be ignored as useless.
	 */
	foreach(temp, orlist)
	{
		Expr   *clause = lfirst(temp);

		if (and_clause((Node *) clause))
		{
			int		nclauses = length(clause->args);

			if (nclauses > num_subclauses)
			{
				distributable = clause;
				num_subclauses = nclauses;
			}
		}
	}

	/* if there's no suitable AND clause, we can't transform the OR */
	if (! distributable)
		return make_orclause(orlist);

	/* Caution: lremove destructively modifies the input orlist.
	 * This should be OK, since or_normalize is only called with
	 * freshly constructed lists that are not referenced elsewhere.
	 */
	orlist = lremove(distributable, orlist);

	foreach(temp, distributable->args)
	{
		Expr	   *andclause = lfirst(temp);

		/* pull_ors is needed here in case andclause has a top-level OR.
		 * Then we recursively apply or_normalize, since there might
		 * be an AND subclause in the resulting OR-list.
		 * Note: we rely on pull_ors to build a fresh list,
		 * and not damage the given orlist.
		 */
		andclause = or_normalize(pull_ors(lcons(andclause, orlist)));
		andclauses = lappend(andclauses, andclause);
	}

	/* pull_ands is needed in case any sub-or_normalize succeeded */
	return make_andclause(pull_ands(andclauses));
}

/*
 * find_ands
 *	  Given a qualification tree with the 'not's pushed down, convert it
 *	  to a tree in DNF by repeatedly applying the rule:
 *				("AND" A ("OR" B C))  => ("OR" ("AND" A B) ("AND" A C))
 *
 *	  Note that 'and' clauses will always be turned into 'or' clauses
 *	  if they contain any 'or' subclauses.
 *
 * Returns the modified qualification.  AND/OR flatness is preserved.
 */
static Expr *
find_ands(Expr *qual)
{
	if (qual == NULL)
		return NULL;

	/* We used to recurse into opclauses here, but I see no reason to... */
	if (or_clause((Node *) qual))
	{
		List	   *orlist = NIL;
		List	   *temp;

		foreach(temp, qual->args)
			orlist = lappend(orlist, find_ands(lfirst(temp)));
		return make_orclause(pull_ors(orlist));
	}
	else if (and_clause((Node *) qual))
	{
		List	   *andlist = NIL;
		List	   *temp;

		foreach(temp, qual->args)
			andlist = lappend(andlist, find_ands(lfirst(temp)));
		return and_normalize(pull_ands(andlist));
	}
	else if (not_clause((Node *) qual))
		return make_notclause(find_ands(get_notclausearg(qual)));
	else
		return qual;
}

/*
 * and_normalize
 *	  Given a list of exprs which are 'and'ed together, try to apply
 *	  the distributive law
 *				("AND" A ("OR" B C))  => ("OR" ("AND" A B) ("AND" A C))
 *	  to convert the top-level AND clause to a top-level OR clause.
 *
 * Returns the resulting expression (could be an AND clause, an OR
 * clause, or maybe even a single subexpression).
 */
static Expr *
and_normalize(List *andlist)
{
	Expr	   *distributable = NULL;
	int			num_subclauses = 1;
	List	   *orclauses = NIL;
	List	   *temp;

	if (andlist == NIL)
		return NULL;			/* probably can't happen */
	if (lnext(andlist) == NIL)
		return lfirst(andlist);	/* single-expression AND (can this happen?) */

	/*
	 * If we have a choice of OR clauses, pick the one with the
	 * most subclauses.  Because we initialized num_subclauses = 1,
	 * any OR clauses with only one arg will be ignored as useless.
	 */
	foreach(temp, andlist)
	{
		Expr   *clause = lfirst(temp);

		if (or_clause((Node *) clause))
		{
			int		nclauses = length(clause->args);

			if (nclauses > num_subclauses)
			{
				distributable = clause;
				num_subclauses = nclauses;
			}
		}
	}

	/* if there's no suitable OR clause, we can't transform the AND */
	if (! distributable)
		return make_andclause(andlist);

	/* Caution: lremove destructively modifies the input andlist.
	 * This should be OK, since and_normalize is only called with
	 * freshly constructed lists that are not referenced elsewhere.
	 */
	andlist = lremove(distributable, andlist);

	foreach(temp, distributable->args)
	{
		Expr	   *orclause = lfirst(temp);

		/* pull_ands is needed here in case orclause has a top-level AND.
		 * Then we recursively apply and_normalize, since there might
		 * be an OR subclause in the resulting AND-list.
		 * Note: we rely on pull_ands to build a fresh list,
		 * and not damage the given andlist.
		 */
		orclause = and_normalize(pull_ands(lcons(orclause, andlist)));
		orclauses = lappend(orclauses, orclause);
	}

	/* pull_ors is needed in case any sub-and_normalize succeeded */
	return make_orclause(pull_ors(orclauses));
}
