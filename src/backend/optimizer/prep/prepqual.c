/*-------------------------------------------------------------------------
 *
 * prepqual.c
 *	  Routines for preprocessing qualification expressions
 *
 *
 * The parser regards AND and OR as purely binary operators, so a qual like
 *		(A = 1) OR (A = 2) OR (A = 3) ...
 * will produce a nested parsetree
 *		(OR (A = 1) (OR (A = 2) (OR (A = 3) ...)))
 * In reality, the optimizer and executor regard AND and OR as N-argument
 * operators, so this tree can be flattened to
 *		(OR (A = 1) (A = 2) (A = 3) ...)
 *
 * Formerly, this module was responsible for doing the initial flattening,
 * but now we leave it to eval_const_expressions to do that since it has to
 * make a complete pass over the expression tree anyway.  Instead, we just
 * have to ensure that our manipulations preserve AND/OR flatness.
 * pull_ands() and pull_ors() are used to maintain flatness of the AND/OR
 * tree after local transformations that might introduce nested AND/ORs.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/prep/prepqual.c,v 1.58 2008/01/01 19:45:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "utils/lsyscache.h"


static List *pull_ands(List *andlist);
static List *pull_ors(List *orlist);
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
 * NOTE: we assume the input has already been through eval_const_expressions
 * and therefore possesses AND/OR flatness.  Formerly this function included
 * its own flattening logic, but that requires a useless extra pass over the
 * tree.
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
	 * Push down NOTs.	We do this only in the top-level boolean expression,
	 * without examining arguments of operators/functions. The main reason for
	 * doing this is to expose as much top-level AND/OR structure as we can,
	 * so there's no point in descending further.
	 */
	newqual = find_nots(qual);

	/*
	 * Pull up redundant subclauses in OR-of-AND trees.  Again, we do this
	 * only within the top-level AND/OR structure.
	 */
	newqual = find_duplicate_ors(newqual);

	return newqual;
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
	List	   *out_list = NIL;
	ListCell   *arg;

	foreach(arg, andlist)
	{
		Node	   *subexpr = (Node *) lfirst(arg);

		/*
		 * Note: we can destructively concat the subexpression's arglist
		 * because we know the recursive invocation of pull_ands will have
		 * built a new arglist not shared with any other expr. Otherwise we'd
		 * need a list_copy here.
		 */
		if (and_clause(subexpr))
			out_list = list_concat(out_list,
								   pull_ands(((BoolExpr *) subexpr)->args));
		else
			out_list = lappend(out_list, subexpr);
	}
	return out_list;
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
	List	   *out_list = NIL;
	ListCell   *arg;

	foreach(arg, orlist)
	{
		Node	   *subexpr = (Node *) lfirst(arg);

		/*
		 * Note: we can destructively concat the subexpression's arglist
		 * because we know the recursive invocation of pull_ors will have
		 * built a new arglist not shared with any other expr. Otherwise we'd
		 * need a list_copy here.
		 */
		if (or_clause(subexpr))
			out_list = list_concat(out_list,
								   pull_ors(((BoolExpr *) subexpr)->args));
		else
			out_list = lappend(out_list, subexpr);
	}
	return out_list;
}


/*
 * find_nots
 *	  Traverse the qualification, looking for NOTs to take care of.
 *	  For NOT clauses, apply push_nots() to try to push down the NOT.
 *	  For AND and OR clause types, simply recurse.	Otherwise stop
 *	  recursing (we do not worry about structure below the top AND/OR tree).
 *
 * Returns the modified qualification.	AND/OR flatness is preserved.
 */
static Expr *
find_nots(Expr *qual)
{
	if (and_clause((Node *) qual))
	{
		List	   *t_list = NIL;
		ListCell   *temp;

		foreach(temp, ((BoolExpr *) qual)->args)
			t_list = lappend(t_list, find_nots(lfirst(temp)));
		return make_andclause(pull_ands(t_list));
	}
	else if (or_clause((Node *) qual))
	{
		List	   *t_list = NIL;
		ListCell   *temp;

		foreach(temp, ((BoolExpr *) qual)->args)
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
 *	  Push down a NOT as far as possible.
 *
 * Input is an expression to be negated (e.g., the argument of a NOT clause).
 * Returns a new qual equivalent to the negation of the given qual.
 */
static Expr *
push_nots(Expr *qual)
{
	if (is_opclause(qual))
	{
		/*
		 * Negate an operator clause if possible: (NOT (< A B)) => (>= A B)
		 * Otherwise, retain the clause as it is (the NOT can't be pushed down
		 * any farther).
		 */
		OpExpr	   *opexpr = (OpExpr *) qual;
		Oid			negator = get_negator(opexpr->opno);

		if (negator)
		{
			OpExpr	   *newopexpr = makeNode(OpExpr);

			newopexpr->opno = negator;
			newopexpr->opfuncid = InvalidOid;
			newopexpr->opresulttype = opexpr->opresulttype;
			newopexpr->opretset = opexpr->opretset;
			newopexpr->args = opexpr->args;
			return (Expr *) newopexpr;
		}
		else
			return make_notclause(qual);
	}
	else if (qual && IsA(qual, ScalarArrayOpExpr))
	{
		/*
		 * Negate a ScalarArrayOpExpr if there is a negator for its operator;
		 * for example x = ANY (list) becomes x <> ALL (list). Otherwise,
		 * retain the clause as it is (the NOT can't be pushed down any
		 * farther).
		 */
		ScalarArrayOpExpr *saopexpr = (ScalarArrayOpExpr *) qual;
		Oid			negator = get_negator(saopexpr->opno);

		if (negator)
		{
			ScalarArrayOpExpr *newopexpr = makeNode(ScalarArrayOpExpr);

			newopexpr->opno = negator;
			newopexpr->opfuncid = InvalidOid;
			newopexpr->useOr = !saopexpr->useOr;
			newopexpr->args = saopexpr->args;
			return (Expr *) newopexpr;
		}
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
		List	   *t_list = NIL;
		ListCell   *temp;

		foreach(temp, ((BoolExpr *) qual)->args)
			t_list = lappend(t_list, push_nots(lfirst(temp)));
		return make_orclause(pull_ors(t_list));
	}
	else if (or_clause((Node *) qual))
	{
		List	   *t_list = NIL;
		ListCell   *temp;

		foreach(temp, ((BoolExpr *) qual)->args)
			t_list = lappend(t_list, push_nots(lfirst(temp)));
		return make_andclause(pull_ands(t_list));
	}
	else if (not_clause((Node *) qual))
	{
		/*
		 * Another NOT cancels this NOT, so eliminate the NOT and stop
		 * negating this branch.  But search the subexpression for more NOTs
		 * to simplify.
		 */
		return find_nots(get_notclausearg(qual));
	}
	else
	{
		/*
		 * We don't know how to negate anything else, place a NOT at this
		 * level.  No point in recursing deeper, either.
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
 * in some of the TPC benchmarks that need it.	This was in fact almost the
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
 * Returns the modified qualification.	AND/OR flatness is preserved.
 */
static Expr *
find_duplicate_ors(Expr *qual)
{
	if (or_clause((Node *) qual))
	{
		List	   *orlist = NIL;
		ListCell   *temp;

		/* Recurse */
		foreach(temp, ((BoolExpr *) qual)->args)
			orlist = lappend(orlist, find_duplicate_ors(lfirst(temp)));

		/*
		 * Don't need pull_ors() since this routine will never introduce an OR
		 * where there wasn't one before.
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
	if (list_length(orlist) == 1)		/* single-expression OR (can this
										 * happen?) */
		return linitial(orlist);

	/*
	 * Choose the shortest AND clause as the reference list --- obviously, any
	 * subclause not in this clause isn't in all the clauses. If we find a
	 * clause that's not an AND, we can treat it as a one-element AND clause,
	 * which necessarily wins as shortest.
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
	 * Check each element of the reference list to see if it's in all the OR
	 * clauses.  Build a new list of winning clauses.
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
	 * If any clause degenerates to empty, then we have a situation like (A
	 * AND B) OR (A), which can be reduced to just A --- that is, the
	 * additional conditions in other arms of the OR are irrelevant.
	 *
	 * Note that because we use list_difference, any multiple occurrences of a
	 * winning clause in an AND sub-clause will be removed automatically.
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
				neworlist = NIL;	/* degenerate case, see above */
				break;
			}
		}
		else
		{
			if (!list_member(winners, clause))
				neworlist = lappend(neworlist, clause);
			else
			{
				neworlist = NIL;	/* degenerate case, see above */
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
