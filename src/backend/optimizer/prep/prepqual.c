/*-------------------------------------------------------------------------
 *
 * prepqual.c
 *	  Routines for preprocessing qualification expressions
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/prep/prepqual.c,v 1.43 2004/05/30 23:40:29 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "utils/lsyscache.h"


static Node *flatten_andors_mutator(Node *node, void *context);
static void flatten_andors_and_walker(FastList *out_list, List *andlist);
static void flatten_andors_or_walker(FastList *out_list, List *orlist);
static List *pull_ands(List *andlist);
static void pull_ands_walker(FastList *out_list, List *andlist);
static List *pull_ors(List *orlist);
static void pull_ors_walker(FastList *out_list, List *orlist);
static Expr *find_nots(Expr *qual);
static Expr *push_nots(Expr *qual);
static Expr *find_duplicate_ors(Expr *qual);
static Expr *process_duplicate_ors(List *orlist);


/*
 * canonicalize_qual
 *	  Convert a qualification expression to the most useful form.
 *
 * The name of this routine is a holdover from a time when it would try to
 * force the expression into canonical AND-of-ORs or OR-of-ANDs form.
 * Eventually, we recognized that that had more theoretical purity than
 * actual usefulness, and so now the transformation doesn't involve any
 * notion of reaching a canonical form.
 *
 * Returns the modified qualification.
 */
Expr *
canonicalize_qual(Expr *qual)
{
	Expr	   *newqual;

	/* Quick exit for empty qual */
	if (qual == NULL)
		return NULL;

	/*
	 * Flatten AND and OR groups throughout the expression tree.
	 */
	newqual = (Expr *) flatten_andors((Node *) qual);

	/*
	 * Push down NOTs.	We do this only in the top-level boolean
	 * expression, without examining arguments of operators/functions.
	 * The main reason for doing this is to expose as much top-level AND/OR
	 * structure as we can, so there's no point in descending further.
	 */
	newqual = find_nots(newqual);

	/*
	 * Pull up redundant subclauses in OR-of-AND trees.  Again, we do this
	 * only within the top-level AND/OR structure.
	 */
	newqual = find_duplicate_ors(newqual);

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

/*
 * flatten_andors
 *	  Given an expression tree, simplify nested AND/OR clauses into flat
 *	  AND/OR clauses with more arguments.  The entire tree is processed.
 *
 * Returns the rebuilt expr (note original structure is not touched).
 *
 * This is exported so that other modules can perform the part of
 * canonicalize_qual processing that applies to entire trees, rather
 * than just the top-level boolean expressions.
 */
Node *
flatten_andors(Node *node)
{
	return flatten_andors_mutator(node, NULL);
}

static Node *
flatten_andors_mutator(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, BoolExpr))
	{
		BoolExpr   *bexpr = (BoolExpr *) node;

		if (bexpr->boolop == AND_EXPR)
		{
			FastList	out_list;

			FastListInit(&out_list);
			flatten_andors_and_walker(&out_list, bexpr->args);
			return (Node *) make_andclause(FastListValue(&out_list));
		}
		if (bexpr->boolop == OR_EXPR)
		{
			FastList	out_list;

			FastListInit(&out_list);
			flatten_andors_or_walker(&out_list, bexpr->args);
			return (Node *) make_orclause(FastListValue(&out_list));
		}
		/* else it's a NOT clause, fall through */
	}
	return expression_tree_mutator(node, flatten_andors_mutator, context);
}

static void
flatten_andors_and_walker(FastList *out_list, List *andlist)
{
	ListCell   *arg;

	foreach(arg, andlist)
	{
		Node	   *subexpr = (Node *) lfirst(arg);

		if (and_clause(subexpr))
			flatten_andors_and_walker(out_list, ((BoolExpr *) subexpr)->args);
		else
			FastAppend(out_list, flatten_andors(subexpr));
	}
}

static void
flatten_andors_or_walker(FastList *out_list, List *orlist)
{
	ListCell   *arg;

	foreach(arg, orlist)
	{
		Node	   *subexpr = (Node *) lfirst(arg);

		if (or_clause(subexpr))
			flatten_andors_or_walker(out_list, ((BoolExpr *) subexpr)->args);
		else
			FastAppend(out_list, flatten_andors(subexpr));
	}
}

/*
 * pull_ands
 *	  Recursively flatten nested AND clauses into a single and-clause list.
 *
 * Input is the arglist of an AND clause.
 * Returns the rebuilt arglist (note original list structure is not touched).
 */
static List *
pull_ands(List *andlist)
{
	FastList	out_list;

	FastListInit(&out_list);
	pull_ands_walker(&out_list, andlist);
	return FastListValue(&out_list);
}

static void
pull_ands_walker(FastList *out_list, List *andlist)
{
	ListCell   *arg;

	foreach(arg, andlist)
	{
		Node	   *subexpr = (Node *) lfirst(arg);

		if (and_clause(subexpr))
			pull_ands_walker(out_list, ((BoolExpr *) subexpr)->args);
		else
			FastAppend(out_list, subexpr);
	}
}

/*
 * pull_ors
 *	  Recursively flatten nested OR clauses into a single or-clause list.
 *
 * Input is the arglist of an OR clause.
 * Returns the rebuilt arglist (note original list structure is not touched).
 */
static List *
pull_ors(List *orlist)
{
	FastList	out_list;

	FastListInit(&out_list);
	pull_ors_walker(&out_list, orlist);
	return FastListValue(&out_list);
}

static void
pull_ors_walker(FastList *out_list, List *orlist)
{
	ListCell   *arg;

	foreach(arg, orlist)
	{
		Node	   *subexpr = (Node *) lfirst(arg);

		if (or_clause(subexpr))
			pull_ors_walker(out_list, ((BoolExpr *) subexpr)->args);
		else
			FastAppend(out_list, subexpr);
	}
}


/*
 * find_nots
 *	  Traverse the qualification, looking for NOTs to take care of.
 *	  For NOT clauses, apply push_nots() to try to push down the NOT.
 *	  For AND and OR clause types, simply recurse.  Otherwise stop
 *	  recursing (we do not worry about structure below the top AND/OR tree).
 *
 * Returns the modified qualification.	AND/OR flatness is preserved.
 */
static Expr *
find_nots(Expr *qual)
{
	if (qual == NULL)
		return NULL;

	if (and_clause((Node *) qual))
	{
		FastList	t_list;
		ListCell   *temp;

		FastListInit(&t_list);
		foreach(temp, ((BoolExpr *) qual)->args)
			FastAppend(&t_list, find_nots(lfirst(temp)));
		return make_andclause(pull_ands(FastListValue(&t_list)));
	}
	else if (or_clause((Node *) qual))
	{
		FastList	t_list;
		ListCell   *temp;

		FastListInit(&t_list);
		foreach(temp, ((BoolExpr *) qual)->args)
			FastAppend(&t_list, find_nots(lfirst(temp)));
		return make_orclause(pull_ors(FastListValue(&t_list)));
	}
	else if (not_clause((Node *) qual))
		return push_nots(get_notclausearg(qual));
	else
		return qual;
}

/*
 * push_nots
 *	  Push down a NOT as far as possible.
 *
 * Input is an expression to be negated (e.g., the argument of a NOT clause).
 * Returns a new qual equivalent to the negation of the given qual.
 */
static Expr *
push_nots(Expr *qual)
{
	if (qual == NULL)
		return make_notclause(qual);	/* XXX is this right?  Or
										 * possible? */

	/*
	 * Negate an operator clause if possible: (NOT (< A B)) => (> A B)
	 * Otherwise, retain the clause as it is (the NOT can't be pushed
	 * down any farther).
	 */
	if (is_opclause(qual))
	{
		OpExpr	   *opexpr = (OpExpr *) qual;
		Oid			negator = get_negator(opexpr->opno);

		if (negator)
			return make_opclause(negator,
								 opexpr->opresulttype,
								 opexpr->opretset,
								 (Expr *) get_leftop(qual),
								 (Expr *) get_rightop(qual));
		else
			return make_notclause(qual);
	}
	else if (and_clause((Node *) qual))
	{
		/*--------------------
		 * Apply DeMorgan's Laws:
		 *		(NOT (AND A B)) => (OR (NOT A) (NOT B))
		 *		(NOT (OR A B))	=> (AND (NOT A) (NOT B))
		 * i.e., swap AND for OR and negate all the subclauses.
		 *--------------------
		 */
		FastList	t_list;
		ListCell   *temp;

		FastListInit(&t_list);
		foreach(temp, ((BoolExpr *) qual)->args)
			FastAppend(&t_list, push_nots(lfirst(temp)));
		return make_orclause(pull_ors(FastListValue(&t_list)));
	}
	else if (or_clause((Node *) qual))
	{
		FastList	t_list;
		ListCell   *temp;

		FastListInit(&t_list);
		foreach(temp, ((BoolExpr *) qual)->args)
			FastAppend(&t_list, push_nots(lfirst(temp)));
		return make_andclause(pull_ands(FastListValue(&t_list)));
	}
	else if (not_clause((Node *) qual))
	{
		/*
		 * Another NOT cancels this NOT, so eliminate the NOT and
		 * stop negating this branch.
		 */
		return get_notclausearg(qual);
	}
	else
	{
		/*
		 * We don't know how to negate anything else, place a NOT at
		 * this level.
		 */
		return make_notclause(qual);
	}
}


/*--------------------
 * The following code attempts to apply the inverse OR distributive law:
 *		((A AND B) OR (A AND C))  =>  (A AND (B OR C))
 * That is, locate OR clauses in which every subclause contains an
 * identical term, and pull out the duplicated terms.
 *
 * This may seem like a fairly useless activity, but it turns out to be
 * applicable to many machine-generated queries, and there are also queries
 * in some of the TPC benchmarks that need it.  This was in fact almost the
 * sole useful side-effect of the old prepqual code that tried to force
 * the query into canonical AND-of-ORs form: the canonical equivalent of
 *		((A AND B) OR (A AND C))
 * is
 *		((A OR A) AND (A OR C) AND (B OR A) AND (B OR C))
 * which the code was able to simplify to
 *		(A AND (A OR C) AND (B OR A) AND (B OR C))
 * thus successfully extracting the common condition A --- but at the cost
 * of cluttering the qual with many redundant clauses.
 *--------------------
 */

/*
 * find_duplicate_ors
 *	  Given a qualification tree with the NOTs pushed down, search for
 *	  OR clauses to which the inverse OR distributive law might apply.
 *	  Only the top-level AND/OR structure is searched.
 *
 * Returns the modified qualification.  AND/OR flatness is preserved.
 */
static Expr *
find_duplicate_ors(Expr *qual)
{
	if (qual == NULL)
		return NULL;

	if (or_clause((Node *) qual))
	{
		List	   *orlist = NIL;
		ListCell   *temp;

		/* Recurse */
		foreach(temp, ((BoolExpr *) qual)->args)
			orlist = lappend(orlist, find_duplicate_ors(lfirst(temp)));
		/*
		 * Don't need pull_ors() since this routine will never introduce
		 * an OR where there wasn't one before.
		 */
		return process_duplicate_ors(orlist);
	}
	else if (and_clause((Node *) qual))
	{
		List	   *andlist = NIL;
		ListCell   *temp;

		/* Recurse */
		foreach(temp, ((BoolExpr *) qual)->args)
			andlist = lappend(andlist, find_duplicate_ors(lfirst(temp)));
		/* Flatten any ANDs introduced just below here */
		andlist = pull_ands(andlist);
		/* The AND list can't get shorter, so result is always an AND */
		return make_andclause(andlist);
	}
	else
		return qual;
}

/*
 * process_duplicate_ors
 *	  Given a list of exprs which are ORed together, try to apply
 *	  the inverse OR distributive law.
 *
 * Returns the resulting expression (could be an AND clause, an OR
 * clause, or maybe even a single subexpression).
 */
static Expr *
process_duplicate_ors(List *orlist)
{
	List	   *reference = NIL;
	int			num_subclauses = 0;
	List	   *winners;
	List	   *neworlist;
	ListCell   *temp;

	if (orlist == NIL)
		return NULL;			/* probably can't happen */
	if (list_length(orlist) == 1)	/* single-expression OR (can this happen?) */
		return linitial(orlist);

	/*
	 * Choose the shortest AND clause as the reference list --- obviously,
	 * any subclause not in this clause isn't in all the clauses.
	 * If we find a clause that's not an AND, we can treat it as a
	 * one-element AND clause, which necessarily wins as shortest.
	 */
	foreach(temp, orlist)
	{
		Expr	   *clause = (Expr *) lfirst(temp);

		if (and_clause((Node *) clause))
		{
			List	   *subclauses = ((BoolExpr *) clause)->args;
			int			nclauses = list_length(subclauses);

			if (reference == NIL || nclauses < num_subclauses)
			{
				reference = subclauses;
				num_subclauses = nclauses;
			}
		}
		else
		{
			reference = list_make1(clause);
			break;
		}
	}

	/*
	 * Just in case, eliminate any duplicates in the reference list.
	 */
	reference = list_union(NIL, reference);

	/*
	 * Check each element of the reference list to see if it's in all the
	 * OR clauses.  Build a new list of winning clauses.
	 */
	winners = NIL;
	foreach(temp, reference)
	{
		Expr	   *refclause = (Expr *) lfirst(temp);
		bool		win = true;
		ListCell   *temp2;

		foreach(temp2, orlist)
		{
			Expr	   *clause = (Expr *) lfirst(temp2);

			if (and_clause((Node *) clause))
			{
				if (!list_member(((BoolExpr *) clause)->args, refclause))
				{
					win = false;
					break;
				}
			}
			else
			{
				if (!equal(refclause, clause))
				{
					win = false;
					break;
				}
			}
		}

		if (win)
			winners = lappend(winners, refclause);
	}

	/*
	 * If no winners, we can't transform the OR
	 */
	if (winners == NIL)
		return make_orclause(orlist);

	/*
	 * Generate new OR list consisting of the remaining sub-clauses.
	 *
	 * If any clause degenerates to empty, then we have a situation like
	 * (A AND B) OR (A), which can be reduced to just A --- that is, the
	 * additional conditions in other arms of the OR are irrelevant.
	 *
	 * Note that because we use list_difference, any multiple occurrences of
	 * a winning clause in an AND sub-clause will be removed automatically.
	 */
	neworlist = NIL;
	foreach(temp, orlist)
	{
		Expr	   *clause = (Expr *) lfirst(temp);

		if (and_clause((Node *) clause))
		{
			List	   *subclauses = ((BoolExpr *) clause)->args;

			subclauses = list_difference(subclauses, winners);
			if (subclauses != NIL)
			{
				if (list_length(subclauses) == 1)
					neworlist = lappend(neworlist, linitial(subclauses));
				else
					neworlist = lappend(neworlist, make_andclause(subclauses));
			}
			else
			{
				neworlist = NIL;		/* degenerate case, see above */
				break;
			}
		}
		else
		{
			if (!list_member(winners, clause))
				neworlist = lappend(neworlist, clause);
			else
			{
				neworlist = NIL;		/* degenerate case, see above */
				break;
			}
		}
	}

	/*
	 * Append reduced OR to the winners list, if it's not degenerate, handling
	 * the special case of one element correctly (can that really happen?).
	 * Also be careful to maintain AND/OR flatness in case we pulled up a
	 * sub-sub-OR-clause.
	 */
	if (neworlist != NIL)
	{
		if (list_length(neworlist) == 1)
			winners = lappend(winners, linitial(neworlist));
		else
			winners = lappend(winners, make_orclause(pull_ors(neworlist)));
	}

	/*
	 * And return the constructed AND clause, again being wary of a single
	 * element and AND/OR flatness.
	 */
	if (list_length(winners) == 1)
		return (Expr *) linitial(winners);
	else
		return make_andclause(pull_ands(winners));
}
