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
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/prep/prepqual.c,v 1.38 2003/08/08 21:41:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "utils/lsyscache.h"

static Expr *flatten_andors(Expr *qual);
static void flatten_andors_and_walker(FastList *out_list, List *andlist);
static void flatten_andors_or_walker(FastList *out_list, List *orlist);
static List *pull_ands(List *andlist);
static void pull_ands_walker(FastList *out_list, List *andlist);
static List *pull_ors(List *orlist);
static void pull_ors_walker(FastList *out_list, List *orlist);
static Expr *find_nots(Expr *qual);
static Expr *push_nots(Expr *qual);
static Expr *find_ors(Expr *qual);
static Expr *or_normalize(List *orlist);
static Expr *find_ands(Expr *qual);
static Expr *and_normalize(List *andlist);
static Expr *qual_cleanup(Expr *qual);
static List *remove_duplicates(List *list);
static void count_bool_nodes(Expr *qual, double *nodes,
				 double *cnfnodes, double *dnfnodes);

/*****************************************************************************
 *
 *		CNF/DNF CONVERSION ROUTINES
 *
 *		These routines convert an arbitrary boolean expression into
 *		conjunctive normal form or disjunctive normal form.
 *
 *		Normalization is only carried out in the top AND/OR/NOT portion
 *		of the given tree; we do not attempt to normalize boolean expressions
 *		that may appear as arguments of operators or functions in the tree.
 *
 *		Query qualifications (WHERE clauses) are ordinarily transformed into
 *		CNF, ie, AND-of-ORs form, because then the optimizer can use any one
 *		of the independent AND clauses as a filtering qualification.  However,
 *		quals that are naturally expressed as OR-of-ANDs can suffer an
 *		exponential growth in size in this transformation, so we also consider
 *		converting to DNF (OR-of-ANDs), and we may also leave well enough alone
 *		if both transforms cause unreasonable growth.  The OR-of-ANDs format
 *		is useful for indexscan implementation, so we prefer that format when
 *		there is just one relation involved.
 *
 *		canonicalize_qual() does "smart" conversion to either CNF or DNF, per
 *		the above considerations, while cnfify() and dnfify() simply perform
 *		the demanded transformation.  The latter two may become dead code
 *		eventually.
 *****************************************************************************/


/*
 * canonicalize_qual
 *	  Convert a qualification to the most useful normalized form.
 *
 * Returns the modified qualification.
 *
 * If 'removeAndFlag' is true then it removes explicit AND at the top level,
 * producing a list of implicitly-ANDed conditions.  Otherwise, a regular
 * boolean expression is returned.	Since most callers pass 'true', we
 * prefer to declare the result as List *, not Expr *.
 *
 * XXX This code could be much smarter, at the cost of also being slower,
 * if we tried to compute selectivities and/or see whether there are
 * actually indexes to support an indexscan implementation of a DNF qual.
 * We could even try converting the CNF clauses that mention a single
 * relation into a single DNF clause to see if that looks cheaper to
 * implement.  For now, though, we just try to avoid doing anything
 * quite as stupid as unconditionally converting to CNF was...
 */
List *
canonicalize_qual(Expr *qual, bool removeAndFlag)
{
	Expr	   *newqual;
	double		nodes,
				cnfnodes,
				dnfnodes;
	bool		cnfok,
				dnfok;

	if (qual == NULL)
		return NIL;

	/*
	 * Flatten AND and OR groups throughout the tree. This improvement is
	 * always worthwhile, so do it unconditionally.
	 */
	qual = flatten_andors(qual);

	/*
	 * Push down NOTs.	We do this only in the top-level boolean
	 * expression, without examining arguments of operators/functions.
	 * Even so, it might not be a win if we are unable to find negators
	 * for all the operators involved; perhaps we should compare before-
	 * and-after tree sizes?
	 */
	newqual = find_nots(qual);

	/*
	 * Choose whether to convert to CNF, or DNF, or leave well enough
	 * alone.
	 *
	 * We make an approximate estimate of the number of bottom-level nodes
	 * that will appear in the CNF and DNF forms of the query.
	 */
	count_bool_nodes(newqual, &nodes, &cnfnodes, &dnfnodes);

	/*
	 * First heuristic is to forget about *both* normal forms if there are
	 * a huge number of terms in the qual clause.  This would only happen
	 * with machine-generated queries, presumably; and most likely such a
	 * query is already in either CNF or DNF.
	 */
	cnfok = dnfok = true;
	if (nodes >= 500.0)
		cnfok = dnfok = false;

	/*
	 * Second heuristic is to forget about either CNF or DNF if it shows
	 * unreasonable growth compared to the original form of the qual,
	 * where we define "unreasonable" a tad arbitrarily as 4x more
	 * operators.
	 */
	if (cnfnodes >= 4.0 * nodes)
		cnfok = false;
	if (dnfnodes >= 4.0 * nodes)
		dnfok = false;

	/*
	 * Third heuristic is to prefer DNF if top level is already an OR, and
	 * only one relation is mentioned, and DNF is no larger than the CNF
	 * representation.	(Pretty shaky; can we improve on this?)
	 */
	if (cnfok && dnfok && dnfnodes <= cnfnodes &&
		or_clause((Node *) newqual) &&
		NumRelids((Node *) newqual) == 1)
		cnfok = false;

	/*
	 * Otherwise, we prefer CNF.
	 *
	 * XXX obviously, these rules could be improved upon.
	 */
	if (cnfok)
	{
		/*
		 * Normalize into conjunctive normal form, and clean up the
		 * result.
		 */
		newqual = qual_cleanup(find_ors(newqual));
	}
	else if (dnfok)
	{
		/*
		 * Normalize into disjunctive normal form, and clean up the
		 * result.
		 */
		newqual = qual_cleanup(find_ands(newqual));
	}

	/* Convert to implicit-AND list if requested */
	if (removeAndFlag)
		newqual = (Expr *) make_ands_implicit(newqual);

	return (List *) newqual;
}

/*
 * cnfify
 *	  Convert a qualification to conjunctive normal form by applying
 *	  successive normalizations.
 *
 * Returns the modified qualification.
 *
 * If 'removeAndFlag' is true then it removes explicit AND at the top level,
 * producing a list of implicitly-ANDed conditions.  Otherwise, a regular
 * boolean expression is returned.	Since most callers pass 'true', we
 * prefer to declare the result as List *, not Expr *.
 */
List *
cnfify(Expr *qual, bool removeAndFlag)
{
	Expr	   *newqual;

	if (qual == NULL)
		return NIL;

	/*
	 * Flatten AND and OR groups throughout the tree. This improvement is
	 * always worthwhile.
	 */
	newqual = flatten_andors(qual);

	/*
	 * Push down NOTs.	We do this only in the top-level boolean
	 * expression, without examining arguments of operators/functions.
	 */
	newqual = find_nots(newqual);
	/* Normalize into conjunctive normal form. */
	newqual = find_ors(newqual);
	/* Clean up the result. */
	newqual = qual_cleanup(newqual);

	if (removeAndFlag)
		newqual = (Expr *) make_ands_implicit(newqual);

	return (List *) newqual;
}

#ifdef NOT_USED
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
static Expr *
dnfify(Expr *qual)
{
	Expr	   *newqual;

	if (qual == NULL)
		return NULL;

	/*
	 * Flatten AND and OR groups throughout the tree. This improvement is
	 * always worthwhile.
	 */
	newqual = flatten_andors(qual);

	/*
	 * Push down NOTs.	We do this only in the top-level boolean
	 * expression, without examining arguments of operators/functions.
	 */
	newqual = find_nots(newqual);
	/* Normalize into disjunctive normal form. */
	newqual = find_ands(newqual);
	/* Clean up the result. */
	newqual = qual_cleanup(newqual);

	return newqual;
}
#endif

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
		FastList	out_list;

		FastListInit(&out_list);
		flatten_andors_and_walker(&out_list, ((BoolExpr *) qual)->args);
		return make_andclause(FastListValue(&out_list));
	}
	else if (or_clause((Node *) qual))
	{
		FastList	out_list;

		FastListInit(&out_list);
		flatten_andors_or_walker(&out_list, ((BoolExpr *) qual)->args);
		return make_orclause(FastListValue(&out_list));
	}
	else if (not_clause((Node *) qual))
		return make_notclause(flatten_andors(get_notclausearg(qual)));
	else if (is_opclause(qual))
	{
		OpExpr	   *opexpr = (OpExpr *) qual;
		Expr	   *left = (Expr *) get_leftop(qual);
		Expr	   *right = (Expr *) get_rightop(qual);

		return make_opclause(opexpr->opno,
							 opexpr->opresulttype,
							 opexpr->opretset,
							 flatten_andors(left),
							 flatten_andors(right));
	}
	else
		return qual;
}

static void
flatten_andors_and_walker(FastList *out_list, List *andlist)
{
	List	   *arg;

	foreach(arg, andlist)
	{
		Expr	   *subexpr = (Expr *) lfirst(arg);

		if (and_clause((Node *) subexpr))
			flatten_andors_and_walker(out_list, ((BoolExpr *) subexpr)->args);
		else
			FastAppend(out_list, flatten_andors(subexpr));
	}
}

static void
flatten_andors_or_walker(FastList *out_list, List *orlist)
{
	List	   *arg;

	foreach(arg, orlist)
	{
		Expr	   *subexpr = (Expr *) lfirst(arg);

		if (or_clause((Node *) subexpr))
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
	List	   *arg;

	foreach(arg, andlist)
	{
		Expr	   *subexpr = (Expr *) lfirst(arg);

		if (and_clause((Node *) subexpr))
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
	List	   *arg;

	foreach(arg, orlist)
	{
		Expr	   *subexpr = (Expr *) lfirst(arg);

		if (or_clause((Node *) subexpr))
			pull_ors_walker(out_list, ((BoolExpr *) subexpr)->args);
		else
			FastAppend(out_list, subexpr);
	}
}

/*
 * find_nots
 *	  Traverse the qualification, looking for 'NOT's to take care of.
 *	  For 'NOT' clauses, apply push_not() to try to push down the 'NOT'.
 *	  For all other clause types, simply recurse.
 *
 * Returns the modified qualification.	AND/OR flatness is preserved.
 */
static Expr *
find_nots(Expr *qual)
{
	if (qual == NULL)
		return NULL;

#ifdef NOT_USED
	/* recursing into operator expressions is probably not worth it. */
	if (is_opclause(qual))
	{
		OpExpr	   *opexpr = (OpExpr *) qual;
		Expr	   *left = (Expr *) get_leftop(qual);
		Expr	   *right = (Expr *) get_rightop(qual);

		return make_opclause(opexpr->opno,
							 opexpr->opresulttype,
							 opexpr->opretset,
							 find_nots(left),
							 find_nots(right));
	}
#endif
	if (and_clause((Node *) qual))
	{
		FastList	t_list;
		List	   *temp;

		FastListInit(&t_list);
		foreach(temp, ((BoolExpr *) qual)->args)
			FastAppend(&t_list, find_nots(lfirst(temp)));
		return make_andclause(pull_ands(FastListValue(&t_list)));
	}
	else if (or_clause((Node *) qual))
	{
		FastList	t_list;
		List	   *temp;

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
 *	  Push down a 'NOT' as far as possible.
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
	 * Negate an operator clause if possible: ("NOT" (< A B)) => (> A B)
	 * Otherwise, retain the clause as it is (the 'not' can't be pushed
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
		 *		("NOT" ("AND" A B)) => ("OR" ("NOT" A) ("NOT" B))
		 *		("NOT" ("OR" A B))	=> ("AND" ("NOT" A) ("NOT" B))
		 * i.e., swap AND for OR and negate all the subclauses.
		 *--------------------
		 */
		FastList	t_list;
		List	   *temp;

		FastListInit(&t_list);
		foreach(temp, ((BoolExpr *) qual)->args)
			FastAppend(&t_list, push_nots(lfirst(temp)));
		return make_orclause(pull_ors(FastListValue(&t_list)));
	}
	else if (or_clause((Node *) qual))
	{
		FastList	t_list;
		List	   *temp;

		FastListInit(&t_list);
		foreach(temp, ((BoolExpr *) qual)->args)
			FastAppend(&t_list, push_nots(lfirst(temp)));
		return make_andclause(pull_ands(FastListValue(&t_list)));
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
 * Returns the modified qualification.	AND/OR flatness is preserved.
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

		foreach(temp, ((BoolExpr *) qual)->args)
			andlist = lappend(andlist, find_ors(lfirst(temp)));
		return make_andclause(pull_ands(andlist));
	}
	else if (or_clause((Node *) qual))
	{
		List	   *orlist = NIL;
		List	   *temp;

		foreach(temp, ((BoolExpr *) qual)->args)
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
	 * If we have a choice of AND clauses, pick the one with the most
	 * subclauses.	Because we initialized num_subclauses = 1, any AND
	 * clauses with only one arg will be ignored as useless.
	 */
	foreach(temp, orlist)
	{
		Expr	   *clause = lfirst(temp);

		if (and_clause((Node *) clause))
		{
			int			nclauses = length(((BoolExpr *) clause)->args);

			if (nclauses > num_subclauses)
			{
				distributable = clause;
				num_subclauses = nclauses;
			}
		}
	}

	/* if there's no suitable AND clause, we can't transform the OR */
	if (!distributable)
		return make_orclause(orlist);

	/*
	 * Caution: lremove destructively modifies the input orlist. This
	 * should be OK, since or_normalize is only called with freshly
	 * constructed lists that are not referenced elsewhere.
	 */
	orlist = lremove(distributable, orlist);

	foreach(temp, ((BoolExpr *) distributable)->args)
	{
		Expr	   *andclause = lfirst(temp);
		List	   *neworlist;

		/*
		 * We are going to insert the orlist into multiple places in the
		 * result expression.  For most expression types, it'd be OK to
		 * just have multiple links to the same subtree, but this fails
		 * badly for SubLinks (and perhaps other cases?).  For safety, we
		 * make a distinct copy for each place the orlist is inserted.
		 */
		if (lnext(temp) == NIL)
			neworlist = orlist; /* can use original tree at the end */
		else
			neworlist = copyObject(orlist);

		/*
		 * pull_ors is needed here in case andclause has a top-level OR.
		 * Then we recursively apply or_normalize, since there might be an
		 * AND subclause in the resulting OR-list.
		 */
		andclause = or_normalize(pull_ors(lcons(andclause, neworlist)));
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
 * Returns the modified qualification.	AND/OR flatness is preserved.
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

		foreach(temp, ((BoolExpr *) qual)->args)
			orlist = lappend(orlist, find_ands(lfirst(temp)));
		return make_orclause(pull_ors(orlist));
	}
	else if (and_clause((Node *) qual))
	{
		List	   *andlist = NIL;
		List	   *temp;

		foreach(temp, ((BoolExpr *) qual)->args)
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
		return lfirst(andlist); /* single-expression AND (can this
								 * happen?) */

	/*
	 * If we have a choice of OR clauses, pick the one with the most
	 * subclauses.	Because we initialized num_subclauses = 1, any OR
	 * clauses with only one arg will be ignored as useless.
	 */
	foreach(temp, andlist)
	{
		Expr	   *clause = lfirst(temp);

		if (or_clause((Node *) clause))
		{
			int			nclauses = length(((BoolExpr *) clause)->args);

			if (nclauses > num_subclauses)
			{
				distributable = clause;
				num_subclauses = nclauses;
			}
		}
	}

	/* if there's no suitable OR clause, we can't transform the AND */
	if (!distributable)
		return make_andclause(andlist);

	/*
	 * Caution: lremove destructively modifies the input andlist. This
	 * should be OK, since and_normalize is only called with freshly
	 * constructed lists that are not referenced elsewhere.
	 */
	andlist = lremove(distributable, andlist);

	foreach(temp, ((BoolExpr *) distributable)->args)
	{
		Expr	   *orclause = lfirst(temp);
		List	   *newandlist;

		/*
		 * We are going to insert the andlist into multiple places in the
		 * result expression.  For most expression types, it'd be OK to
		 * just have multiple links to the same subtree, but this fails
		 * badly for SubLinks (and perhaps other cases?).  For safety, we
		 * make a distinct copy for each place the andlist is inserted.
		 */
		if (lnext(temp) == NIL)
			newandlist = andlist;		/* can use original tree at the
										 * end */
		else
			newandlist = copyObject(andlist);

		/*
		 * pull_ands is needed here in case orclause has a top-level AND.
		 * Then we recursively apply and_normalize, since there might be
		 * an OR subclause in the resulting AND-list.
		 */
		orclause = and_normalize(pull_ands(lcons(orclause, newandlist)));
		orclauses = lappend(orclauses, orclause);
	}

	/* pull_ors is needed in case any sub-and_normalize succeeded */
	return make_orclause(pull_ors(orclauses));
}

/*
 * qual_cleanup
 *	  Fix up a qualification by removing duplicate entries (which could be
 *	  created during normalization, if identical subexpressions from different
 *	  parts of the tree are brought together).	Also, check for AND and OR
 *	  clauses with only one remaining subexpression, and simplify.
 *
 * Returns the modified qualification.
 */
static Expr *
qual_cleanup(Expr *qual)
{
	if (qual == NULL)
		return NULL;

	if (and_clause((Node *) qual))
	{
		List	   *andlist = NIL;
		List	   *temp;

		foreach(temp, ((BoolExpr *) qual)->args)
			andlist = lappend(andlist, qual_cleanup(lfirst(temp)));

		andlist = remove_duplicates(pull_ands(andlist));

		if (length(andlist) > 1)
			return make_andclause(andlist);
		else
			return lfirst(andlist);
	}
	else if (or_clause((Node *) qual))
	{
		List	   *orlist = NIL;
		List	   *temp;

		foreach(temp, ((BoolExpr *) qual)->args)
			orlist = lappend(orlist, qual_cleanup(lfirst(temp)));

		orlist = remove_duplicates(pull_ors(orlist));

		if (length(orlist) > 1)
			return make_orclause(orlist);
		else
			return lfirst(orlist);
	}
	else if (not_clause((Node *) qual))
		return make_notclause(qual_cleanup(get_notclausearg(qual)));
	else
		return qual;
}

/*
 * remove_duplicates
 */
static List *
remove_duplicates(List *list)
{
	List	   *result = NIL;
	List	   *i;

	if (length(list) <= 1)
		return list;

	foreach(i, list)
	{
		if (!member(lfirst(i), result))
			result = lappend(result, lfirst(i));
	}
	return result;
}

/*
 * count_bool_nodes
 *		Support for heuristics in canonicalize_qual(): count the
 *		number of nodes that are inputs to the top level AND/OR/NOT
 *		part of a qual tree, and estimate how many nodes will appear
 *		in the CNF'ified or DNF'ified equivalent of the expression.
 *
 * This is just an approximate calculation; it doesn't deal with NOTs
 * very well, and of course it cannot detect possible simplifications
 * from eliminating duplicate subclauses.  The idea is just to cheaply
 * determine whether CNF will be markedly worse than DNF or vice versa.
 *
 * The counts/estimates are represented as doubles to avoid risk of overflow.
 */
static void
count_bool_nodes(Expr *qual,
				 double *nodes,
				 double *cnfnodes,
				 double *dnfnodes)
{
	List	   *temp;
	double		subnodes,
				subcnfnodes,
				subdnfnodes;

	if (and_clause((Node *) qual))
	{
		*nodes = *cnfnodes = 0.0;
		*dnfnodes = 1.0;		/* DNF nodes will be product of sub-counts */

		foreach(temp, ((BoolExpr *) qual)->args)
		{
			count_bool_nodes(lfirst(temp),
							 &subnodes, &subcnfnodes, &subdnfnodes);
			*nodes += subnodes;
			*cnfnodes += subcnfnodes;
			*dnfnodes *= subdnfnodes;
		}

		/*
		 * we could get dnfnodes < cnfnodes here, if all the sub-nodes are
		 * simple ones with count 1.  Make sure dnfnodes isn't too small.
		 */
		if (*dnfnodes < *cnfnodes)
			*dnfnodes = *cnfnodes;
	}
	else if (or_clause((Node *) qual))
	{
		*nodes = *dnfnodes = 0.0;
		*cnfnodes = 1.0;		/* CNF nodes will be product of sub-counts */

		foreach(temp, ((BoolExpr *) qual)->args)
		{
			count_bool_nodes(lfirst(temp),
							 &subnodes, &subcnfnodes, &subdnfnodes);
			*nodes += subnodes;
			*cnfnodes *= subcnfnodes;
			*dnfnodes += subdnfnodes;
		}

		/*
		 * we could get cnfnodes < dnfnodes here, if all the sub-nodes are
		 * simple ones with count 1.  Make sure cnfnodes isn't too small.
		 */
		if (*cnfnodes < *dnfnodes)
			*cnfnodes = *dnfnodes;
	}
	else if (not_clause((Node *) qual))
	{
		count_bool_nodes(get_notclausearg(qual),
						 nodes, cnfnodes, dnfnodes);
	}
	else if (contain_subplans((Node *) qual))
	{
		/*
		 * charge extra for subexpressions containing sub-SELECTs, to
		 * discourage us from rearranging them in a way that might
		 * generate N copies of a subselect rather than one.  The magic
		 * constant here interacts with the "4x maximum growth" heuristic
		 * in canonicalize_qual().
		 */
		*nodes = 1.0;
		*cnfnodes = *dnfnodes = 25.0;
	}
	else
	{
		/* anything else counts 1 for my purposes */
		*nodes = *cnfnodes = *dnfnodes = 1.0;
	}
}
