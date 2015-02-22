/*-------------------------------------------------------------------------
 *
 * prepqual.c
 *	  Routines for preprocessing qualification expressions
 *
 *
 * While the parser will produce flattened (N-argument) AND/OR trees from
 * simple sequences of AND'ed or OR'ed clauses, there might be an AND clause
 * directly underneath another AND, or OR underneath OR, if the input was
 * oddly parenthesized.  Also, rule expansion and subquery flattening could
 * produce such parsetrees.  The planner wants to flatten all such cases
 * to ensure consistent optimization behavior.
 *
 * Formerly, this module was responsible for doing the initial flattening,
 * but now we leave it to eval_const_expressions to do that since it has to
 * make a complete pass over the expression tree anyway.  Instead, we just
 * have to ensure that our manipulations preserve AND/OR flatness.
 * pull_ands() and pull_ors() are used to maintain flatness of the AND/OR
 * tree after local transformations that might introduce nested AND/ORs.
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepqual.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "utils/lsyscache.h"


static List *pull_ands(List *andlist);
static List *pull_ors(List *orlist);
static Expr *find_duplicate_ors(Expr *qual);
static Expr *process_duplicate_ors(List *orlist);


/*
 * negate_clause
 *	  Negate a Boolean expression.
 *
 * Input is a clause to be negated (e.g., the argument of a NOT clause).
 * Returns a new clause equivalent to the negation of the given clause.
 *
 * Although this can be invoked on its own, it's mainly intended as a helper
 * for eval_const_expressions(), and that context drives several design
 * decisions.  In particular, if the input is already AND/OR flat, we must
 * preserve that property.  We also don't bother to recurse in situations
 * where we can assume that lower-level executions of eval_const_expressions
 * would already have simplified sub-clauses of the input.
 *
 * The difference between this and a simple make_notclause() is that this
 * tries to get rid of the NOT node by logical simplification.  It's clearly
 * always a win if the NOT node can be eliminated altogether.  However, our
 * use of DeMorgan's laws could result in having more NOT nodes rather than
 * fewer.  We do that unconditionally anyway, because in WHERE clauses it's
 * important to expose as much top-level AND/OR structure as possible.
 * Also, eliminating an intermediate NOT may allow us to flatten two levels
 * of AND or OR together that we couldn't have otherwise.  Finally, one of
 * the motivations for doing this is to ensure that logically equivalent
 * expressions will be seen as physically equal(), so we should always apply
 * the same transformations.
 */
Node *
negate_clause(Node *node)
{
	if (node == NULL)			/* should not happen */
		elog(ERROR, "can't negate an empty subexpression");
	switch (nodeTag(node))
	{
		case T_Const:
			{
				Const	   *c = (Const *) node;

				/* NOT NULL is still NULL */
				if (c->constisnull)
					return makeBoolConst(false, true);
				/* otherwise pretty easy */
				return makeBoolConst(!DatumGetBool(c->constvalue), false);
			}
			break;
		case T_OpExpr:
			{
				/*
				 * Negate operator if possible: (NOT (< A B)) => (>= A B)
				 */
				OpExpr	   *opexpr = (OpExpr *) node;
				Oid			negator = get_negator(opexpr->opno);

				if (negator)
				{
					OpExpr	   *newopexpr = makeNode(OpExpr);

					newopexpr->opno = negator;
					newopexpr->opfuncid = InvalidOid;
					newopexpr->opresulttype = opexpr->opresulttype;
					newopexpr->opretset = opexpr->opretset;
					newopexpr->opcollid = opexpr->opcollid;
					newopexpr->inputcollid = opexpr->inputcollid;
					newopexpr->args = opexpr->args;
					newopexpr->location = opexpr->location;
					return (Node *) newopexpr;
				}
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				/*
				 * Negate a ScalarArrayOpExpr if its operator has a negator;
				 * for example x = ANY (list) becomes x <> ALL (list)
				 */
				ScalarArrayOpExpr *saopexpr = (ScalarArrayOpExpr *) node;
				Oid			negator = get_negator(saopexpr->opno);

				if (negator)
				{
					ScalarArrayOpExpr *newopexpr = makeNode(ScalarArrayOpExpr);

					newopexpr->opno = negator;
					newopexpr->opfuncid = InvalidOid;
					newopexpr->useOr = !saopexpr->useOr;
					newopexpr->inputcollid = saopexpr->inputcollid;
					newopexpr->args = saopexpr->args;
					newopexpr->location = saopexpr->location;
					return (Node *) newopexpr;
				}
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;

				switch (expr->boolop)
				{
						/*--------------------
						 * Apply DeMorgan's Laws:
						 *		(NOT (AND A B)) => (OR (NOT A) (NOT B))
						 *		(NOT (OR A B))	=> (AND (NOT A) (NOT B))
						 * i.e., swap AND for OR and negate each subclause.
						 *
						 * If the input is already AND/OR flat and has no NOT
						 * directly above AND or OR, this transformation preserves
						 * those properties.  For example, if no direct child of
						 * the given AND clause is an AND or a NOT-above-OR, then
						 * the recursive calls of negate_clause() can't return any
						 * OR clauses.  So we needn't call pull_ors() before
						 * building a new OR clause.  Similarly for the OR case.
						 *--------------------
						 */
					case AND_EXPR:
						{
							List	   *nargs = NIL;
							ListCell   *lc;

							foreach(lc, expr->args)
							{
								nargs = lappend(nargs,
												negate_clause(lfirst(lc)));
							}
							return (Node *) make_orclause(nargs);
						}
						break;
					case OR_EXPR:
						{
							List	   *nargs = NIL;
							ListCell   *lc;

							foreach(lc, expr->args)
							{
								nargs = lappend(nargs,
												negate_clause(lfirst(lc)));
							}
							return (Node *) make_andclause(nargs);
						}
						break;
					case NOT_EXPR:

						/*
						 * NOT underneath NOT: they cancel.  We assume the
						 * input is already simplified, so no need to recurse.
						 */
						return (Node *) linitial(expr->args);
					default:
						elog(ERROR, "unrecognized boolop: %d",
							 (int) expr->boolop);
						break;
				}
			}
			break;
		case T_NullTest:
			{
				NullTest   *expr = (NullTest *) node;

				/*
				 * In the rowtype case, the two flavors of NullTest are *not*
				 * logical inverses, so we can't simplify.  But it does work
				 * for scalar datatypes.
				 */
				if (!expr->argisrow)
				{
					NullTest   *newexpr = makeNode(NullTest);

					newexpr->arg = expr->arg;
					newexpr->nulltesttype = (expr->nulltesttype == IS_NULL ?
											 IS_NOT_NULL : IS_NULL);
					newexpr->argisrow = expr->argisrow;
					newexpr->location = expr->location;
					return (Node *) newexpr;
				}
			}
			break;
		case T_BooleanTest:
			{
				BooleanTest *expr = (BooleanTest *) node;
				BooleanTest *newexpr = makeNode(BooleanTest);

				newexpr->arg = expr->arg;
				switch (expr->booltesttype)
				{
					case IS_TRUE:
						newexpr->booltesttype = IS_NOT_TRUE;
						break;
					case IS_NOT_TRUE:
						newexpr->booltesttype = IS_TRUE;
						break;
					case IS_FALSE:
						newexpr->booltesttype = IS_NOT_FALSE;
						break;
					case IS_NOT_FALSE:
						newexpr->booltesttype = IS_FALSE;
						break;
					case IS_UNKNOWN:
						newexpr->booltesttype = IS_NOT_UNKNOWN;
						break;
					case IS_NOT_UNKNOWN:
						newexpr->booltesttype = IS_UNKNOWN;
						break;
					default:
						elog(ERROR, "unrecognized booltesttype: %d",
							 (int) expr->booltesttype);
						break;
				}
				newexpr->location = expr->location;
				return (Node *) newexpr;
			}
			break;
		default:
			/* else fall through */
			break;
	}

	/*
	 * Otherwise we don't know how to simplify this, so just tack on an
	 * explicit NOT node.
	 */
	return (Node *) make_notclause((Expr *) node);
}


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
	 * Pull up redundant subclauses in OR-of-AND trees.  We do this only
	 * within the top-level AND/OR structure; there's no point in looking
	 * deeper.  Also remove any NULL constants in the top-level structure.
	 */
	newqual = find_duplicate_ors(qual);

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
 * While at it, we remove any NULL constants within the top-level AND/OR
 * structure, eg "x OR NULL::boolean" is reduced to "x".  In general that
 * would change the result, so eval_const_expressions can't do it; but at
 * top level of WHERE, we don't need to distinguish between FALSE and NULL
 * results, so it's valid to treat NULL::boolean the same as FALSE and then
 * simplify AND/OR accordingly.
 *
 * Returns the modified qualification.  AND/OR flatness is preserved.
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
		{
			Expr	   *arg = (Expr *) lfirst(temp);

			arg = find_duplicate_ors(arg);

			/* Get rid of any constant inputs */
			if (arg && IsA(arg, Const))
			{
				Const	   *carg = (Const *) arg;

				/* Drop constant FALSE or NULL */
				if (carg->constisnull || !DatumGetBool(carg->constvalue))
					continue;
				/* constant TRUE, so OR reduces to TRUE */
				return arg;
			}

			orlist = lappend(orlist, arg);
		}

		/* Flatten any ORs pulled up to just below here */
		orlist = pull_ors(orlist);

		/* Now we can look for duplicate ORs */
		return process_duplicate_ors(orlist);
	}
	else if (and_clause((Node *) qual))
	{
		List	   *andlist = NIL;
		ListCell   *temp;

		/* Recurse */
		foreach(temp, ((BoolExpr *) qual)->args)
		{
			Expr	   *arg = (Expr *) lfirst(temp);

			arg = find_duplicate_ors(arg);

			/* Get rid of any constant inputs */
			if (arg && IsA(arg, Const))
			{
				Const	   *carg = (Const *) arg;

				/* Drop constant TRUE */
				if (!carg->constisnull && DatumGetBool(carg->constvalue))
					continue;
				/* constant FALSE or NULL, so AND reduces to FALSE */
				return (Expr *) makeBoolConst(false, false);
			}

			andlist = lappend(andlist, arg);
		}

		/* Flatten any ANDs introduced just below here */
		andlist = pull_ands(andlist);

		/* AND of no inputs reduces to TRUE */
		if (andlist == NIL)
			return (Expr *) makeBoolConst(true, false);

		/* Single-expression AND just reduces to that expression */
		if (list_length(andlist) == 1)
			return (Expr *) linitial(andlist);

		/* Else we still need an AND node */
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

	/* OR of no inputs reduces to FALSE */
	if (orlist == NIL)
		return (Expr *) makeBoolConst(false, false);

	/* Single-expression OR just reduces to that expression */
	if (list_length(orlist) == 1)
		return (Expr *) linitial(orlist);

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
