/*-------------------------------------------------------------------------
 *
 * predtest.c
 *	  Routines to attempt to prove logical implications between predicate
 *	  expressions.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/predtest.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 * Proof attempts involving large arrays in ScalarArrayOpExpr nodes are
 * likely to require O(N^2) time, and more often than not fail anyway.
 * So we set an arbitrary limit on the number of array elements that
 * we will allow to be treated as an AND or OR clause.
 * XXX is it worth exposing this as a GUC knob?
 */
#define MAX_SAOP_ARRAY_SIZE		100

/*
 * To avoid redundant coding in predicate_implied_by_recurse and
 * predicate_refuted_by_recurse, we need to abstract out the notion of
 * iterating over the components of an expression that is logically an AND
 * or OR structure.  There are multiple sorts of expression nodes that can
 * be treated as ANDs or ORs, and we don't want to code each one separately.
 * Hence, these types and support routines.
 */
typedef enum
{
	CLASS_ATOM,					/* expression that's not AND or OR */
	CLASS_AND,					/* expression with AND semantics */
	CLASS_OR,					/* expression with OR semantics */
} PredClass;

typedef struct PredIterInfoData *PredIterInfo;

typedef struct PredIterInfoData
{
	/* node-type-specific iteration state */
	void	   *state;
	List	   *state_list;
	/* initialize to do the iteration */
	void		(*startup_fn) (Node *clause, PredIterInfo info);
	/* next-component iteration function */
	Node	   *(*next_fn) (PredIterInfo info);
	/* release resources when done with iteration */
	void		(*cleanup_fn) (PredIterInfo info);
} PredIterInfoData;

#define iterate_begin(item, clause, info)	\
	do { \
		Node   *item; \
		(info).startup_fn((clause), &(info)); \
		while ((item = (info).next_fn(&(info))) != NULL)

#define iterate_end(info)	\
		(info).cleanup_fn(&(info)); \
	} while (0)


static bool predicate_implied_by_recurse(Node *clause, Node *predicate,
										 bool weak);
static bool predicate_refuted_by_recurse(Node *clause, Node *predicate,
										 bool weak);
static PredClass predicate_classify(Node *clause, PredIterInfo info);
static void list_startup_fn(Node *clause, PredIterInfo info);
static Node *list_next_fn(PredIterInfo info);
static void list_cleanup_fn(PredIterInfo info);
static void boolexpr_startup_fn(Node *clause, PredIterInfo info);
static void arrayconst_startup_fn(Node *clause, PredIterInfo info);
static Node *arrayconst_next_fn(PredIterInfo info);
static void arrayconst_cleanup_fn(PredIterInfo info);
static void arrayexpr_startup_fn(Node *clause, PredIterInfo info);
static Node *arrayexpr_next_fn(PredIterInfo info);
static void arrayexpr_cleanup_fn(PredIterInfo info);
static bool predicate_implied_by_simple_clause(Expr *predicate, Node *clause,
											   bool weak);
static bool predicate_refuted_by_simple_clause(Expr *predicate, Node *clause,
											   bool weak);
static Node *extract_not_arg(Node *clause);
static Node *extract_strong_not_arg(Node *clause);
static bool clause_is_strict_for(Node *clause, Node *subexpr, bool allow_false);
static bool operator_predicate_proof(Expr *predicate, Node *clause,
									 bool refute_it, bool weak);
static bool operator_same_subexprs_proof(Oid pred_op, Oid clause_op,
										 bool refute_it);
static bool operator_same_subexprs_lookup(Oid pred_op, Oid clause_op,
										  bool refute_it);
static Oid	get_btree_test_op(Oid pred_op, Oid clause_op, bool refute_it);
static void InvalidateOprProofCacheCallBack(Datum arg, int cacheid, uint32 hashvalue);


/*
 * predicate_implied_by
 *	  Recursively checks whether the clauses in clause_list imply that the
 *	  given predicate is true.
 *
 * We support two definitions of implication:
 *
 * "Strong" implication: A implies B means that truth of A implies truth of B.
 * We use this to prove that a row satisfying one WHERE clause or index
 * predicate must satisfy another one.
 *
 * "Weak" implication: A implies B means that non-falsity of A implies
 * non-falsity of B ("non-false" means "either true or NULL").  We use this to
 * prove that a row satisfying one CHECK constraint must satisfy another one.
 *
 * Strong implication can also be used to prove that a WHERE clause implies a
 * CHECK constraint, although it will fail to prove a few cases where we could
 * safely conclude that the implication holds.  There's no support for proving
 * the converse case, since only a few kinds of CHECK constraint would allow
 * deducing anything.
 *
 * The top-level List structure of each list corresponds to an AND list.
 * We assume that eval_const_expressions() has been applied and so there
 * are no un-flattened ANDs or ORs (e.g., no AND immediately within an AND,
 * including AND just below the top-level List structure).
 * If this is not true we might fail to prove an implication that is
 * valid, but no worse consequences will ensue.
 *
 * We assume the predicate has already been checked to contain only
 * immutable functions and operators.  (In many current uses this is known
 * true because the predicate is part of an index predicate that has passed
 * CheckPredicate(); otherwise, the caller must check it.)  We dare not make
 * deductions based on non-immutable functions, because they might change
 * answers between the time we make the plan and the time we execute the plan.
 * Immutability of functions in the clause_list is checked here, if necessary.
 */
bool
predicate_implied_by(List *predicate_list, List *clause_list,
					 bool weak)
{
	Node	   *p,
			   *c;

	if (predicate_list == NIL)
		return true;			/* no predicate: implication is vacuous */
	if (clause_list == NIL)
		return false;			/* no restriction: implication must fail */

	/*
	 * If either input is a single-element list, replace it with its lone
	 * member; this avoids one useless level of AND-recursion.  We only need
	 * to worry about this at top level, since eval_const_expressions should
	 * have gotten rid of any trivial ANDs or ORs below that.
	 */
	if (list_length(predicate_list) == 1)
		p = (Node *) linitial(predicate_list);
	else
		p = (Node *) predicate_list;
	if (list_length(clause_list) == 1)
		c = (Node *) linitial(clause_list);
	else
		c = (Node *) clause_list;

	/* And away we go ... */
	return predicate_implied_by_recurse(c, p, weak);
}

/*
 * predicate_refuted_by
 *	  Recursively checks whether the clauses in clause_list refute the given
 *	  predicate (that is, prove it false).
 *
 * This is NOT the same as !(predicate_implied_by), though it is similar
 * in the technique and structure of the code.
 *
 * We support two definitions of refutation:
 *
 * "Strong" refutation: A refutes B means truth of A implies falsity of B.
 * We use this to disprove a CHECK constraint given a WHERE clause, i.e.,
 * prove that any row satisfying the WHERE clause would violate the CHECK
 * constraint.  (Observe we must prove B yields false, not just not-true.)
 *
 * "Weak" refutation: A refutes B means truth of A implies non-truth of B
 * (i.e., B must yield false or NULL).  We use this to detect mutually
 * contradictory WHERE clauses.
 *
 * Weak refutation can be proven in some cases where strong refutation doesn't
 * hold, so it's useful to use it when possible.  We don't currently have
 * support for disproving one CHECK constraint based on another one, nor for
 * disproving WHERE based on CHECK.  (As with implication, the last case
 * doesn't seem very practical.  CHECK-vs-CHECK might be useful, but isn't
 * currently needed anywhere.)
 *
 * The top-level List structure of each list corresponds to an AND list.
 * We assume that eval_const_expressions() has been applied and so there
 * are no un-flattened ANDs or ORs (e.g., no AND immediately within an AND,
 * including AND just below the top-level List structure).
 * If this is not true we might fail to prove an implication that is
 * valid, but no worse consequences will ensue.
 *
 * We assume the predicate has already been checked to contain only
 * immutable functions and operators.  We dare not make deductions based on
 * non-immutable functions, because they might change answers between the
 * time we make the plan and the time we execute the plan.
 * Immutability of functions in the clause_list is checked here, if necessary.
 */
bool
predicate_refuted_by(List *predicate_list, List *clause_list,
					 bool weak)
{
	Node	   *p,
			   *c;

	if (predicate_list == NIL)
		return false;			/* no predicate: no refutation is possible */
	if (clause_list == NIL)
		return false;			/* no restriction: refutation must fail */

	/*
	 * If either input is a single-element list, replace it with its lone
	 * member; this avoids one useless level of AND-recursion.  We only need
	 * to worry about this at top level, since eval_const_expressions should
	 * have gotten rid of any trivial ANDs or ORs below that.
	 */
	if (list_length(predicate_list) == 1)
		p = (Node *) linitial(predicate_list);
	else
		p = (Node *) predicate_list;
	if (list_length(clause_list) == 1)
		c = (Node *) linitial(clause_list);
	else
		c = (Node *) clause_list;

	/* And away we go ... */
	return predicate_refuted_by_recurse(c, p, weak);
}

/*----------
 * predicate_implied_by_recurse
 *	  Does the predicate implication test for non-NULL restriction and
 *	  predicate clauses.
 *
 * The logic followed here is ("=>" means "implies"):
 *	atom A => atom B iff:			predicate_implied_by_simple_clause says so
 *	atom A => AND-expr B iff:		A => each of B's components
 *	atom A => OR-expr B iff:		A => any of B's components
 *	AND-expr A => atom B iff:		any of A's components => B
 *	AND-expr A => AND-expr B iff:	A => each of B's components
 *	AND-expr A => OR-expr B iff:	A => any of B's components,
 *									*or* any of A's components => B
 *	OR-expr A => atom B iff:		each of A's components => B
 *	OR-expr A => AND-expr B iff:	A => each of B's components
 *	OR-expr A => OR-expr B iff:		each of A's components => any of B's
 *
 * An "atom" is anything other than an AND or OR node.  Notice that we don't
 * have any special logic to handle NOT nodes; these should have been pushed
 * down or eliminated where feasible during eval_const_expressions().
 *
 * All of these rules apply equally to strong or weak implication.
 *
 * We can't recursively expand either side first, but have to interleave
 * the expansions per the above rules, to be sure we handle all of these
 * examples:
 *		(x OR y) => (x OR y OR z)
 *		(x AND y AND z) => (x AND y)
 *		(x AND y) => ((x AND y) OR z)
 *		((x OR y) AND z) => (x OR y)
 * This is still not an exhaustive test, but it handles most normal cases
 * under the assumption that both inputs have been AND/OR flattened.
 *
 * We have to be prepared to handle RestrictInfo nodes in the restrictinfo
 * tree, though not in the predicate tree.
 *----------
 */
static bool
predicate_implied_by_recurse(Node *clause, Node *predicate,
							 bool weak)
{
	PredIterInfoData clause_info;
	PredIterInfoData pred_info;
	PredClass	pclass;
	bool		result;

	/* skip through RestrictInfo */
	Assert(clause != NULL);
	if (IsA(clause, RestrictInfo))
		clause = (Node *) ((RestrictInfo *) clause)->clause;

	pclass = predicate_classify(predicate, &pred_info);

	switch (predicate_classify(clause, &clause_info))
	{
		case CLASS_AND:
			switch (pclass)
			{
				case CLASS_AND:

					/*
					 * AND-clause => AND-clause if A implies each of B's items
					 */
					result = true;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (!predicate_implied_by_recurse(clause, pitem,
														  weak))
						{
							result = false;
							break;
						}
					}
					iterate_end(pred_info);
					return result;

				case CLASS_OR:

					/*
					 * AND-clause => OR-clause if A implies any of B's items
					 *
					 * Needed to handle (x AND y) => ((x AND y) OR z)
					 */
					result = false;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (predicate_implied_by_recurse(clause, pitem,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(pred_info);
					if (result)
						return result;

					/*
					 * Also check if any of A's items implies B
					 *
					 * Needed to handle ((x OR y) AND z) => (x OR y)
					 */
					iterate_begin(citem, clause, clause_info)
					{
						if (predicate_implied_by_recurse(citem, predicate,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(clause_info);
					return result;

				case CLASS_ATOM:

					/*
					 * AND-clause => atom if any of A's items implies B
					 */
					result = false;
					iterate_begin(citem, clause, clause_info)
					{
						if (predicate_implied_by_recurse(citem, predicate,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(clause_info);
					return result;
			}
			break;

		case CLASS_OR:
			switch (pclass)
			{
				case CLASS_OR:

					/*
					 * OR-clause => OR-clause if each of A's items implies any
					 * of B's items.  Messy but can't do it any more simply.
					 */
					result = true;
					iterate_begin(citem, clause, clause_info)
					{
						bool		presult = false;

						iterate_begin(pitem, predicate, pred_info)
						{
							if (predicate_implied_by_recurse(citem, pitem,
															 weak))
							{
								presult = true;
								break;
							}
						}
						iterate_end(pred_info);
						if (!presult)
						{
							result = false; /* doesn't imply any of B's */
							break;
						}
					}
					iterate_end(clause_info);
					return result;

				case CLASS_AND:
				case CLASS_ATOM:

					/*
					 * OR-clause => AND-clause if each of A's items implies B
					 *
					 * OR-clause => atom if each of A's items implies B
					 */
					result = true;
					iterate_begin(citem, clause, clause_info)
					{
						if (!predicate_implied_by_recurse(citem, predicate,
														  weak))
						{
							result = false;
							break;
						}
					}
					iterate_end(clause_info);
					return result;
			}
			break;

		case CLASS_ATOM:
			switch (pclass)
			{
				case CLASS_AND:

					/*
					 * atom => AND-clause if A implies each of B's items
					 */
					result = true;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (!predicate_implied_by_recurse(clause, pitem,
														  weak))
						{
							result = false;
							break;
						}
					}
					iterate_end(pred_info);
					return result;

				case CLASS_OR:

					/*
					 * atom => OR-clause if A implies any of B's items
					 */
					result = false;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (predicate_implied_by_recurse(clause, pitem,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(pred_info);
					return result;

				case CLASS_ATOM:

					/*
					 * atom => atom is the base case
					 */
					return
						predicate_implied_by_simple_clause((Expr *) predicate,
														   clause,
														   weak);
			}
			break;
	}

	/* can't get here */
	elog(ERROR, "predicate_classify returned a bogus value");
	return false;
}

/*----------
 * predicate_refuted_by_recurse
 *	  Does the predicate refutation test for non-NULL restriction and
 *	  predicate clauses.
 *
 * The logic followed here is ("R=>" means "refutes"):
 *	atom A R=> atom B iff:			predicate_refuted_by_simple_clause says so
 *	atom A R=> AND-expr B iff:		A R=> any of B's components
 *	atom A R=> OR-expr B iff:		A R=> each of B's components
 *	AND-expr A R=> atom B iff:		any of A's components R=> B
 *	AND-expr A R=> AND-expr B iff:	A R=> any of B's components,
 *									*or* any of A's components R=> B
 *	AND-expr A R=> OR-expr B iff:	A R=> each of B's components
 *	OR-expr A R=> atom B iff:		each of A's components R=> B
 *	OR-expr A R=> AND-expr B iff:	each of A's components R=> any of B's
 *	OR-expr A R=> OR-expr B iff:	A R=> each of B's components
 *
 * All of the above rules apply equally to strong or weak refutation.
 *
 * In addition, if the predicate is a NOT-clause then we can use
 *	A R=> NOT B if:					A => B
 * This works for several different SQL constructs that assert the non-truth
 * of their argument, ie NOT, IS FALSE, IS NOT TRUE, IS UNKNOWN, although some
 * of them require that we prove strong implication.  Likewise, we can use
 *	NOT A R=> B if:					B => A
 * but here we must be careful about strong vs. weak refutation and make
 * the appropriate type of implication proof (weak or strong respectively).
 *
 * Other comments are as for predicate_implied_by_recurse().
 *----------
 */
static bool
predicate_refuted_by_recurse(Node *clause, Node *predicate,
							 bool weak)
{
	PredIterInfoData clause_info;
	PredIterInfoData pred_info;
	PredClass	pclass;
	Node	   *not_arg;
	bool		result;

	/* skip through RestrictInfo */
	Assert(clause != NULL);
	if (IsA(clause, RestrictInfo))
		clause = (Node *) ((RestrictInfo *) clause)->clause;

	pclass = predicate_classify(predicate, &pred_info);

	switch (predicate_classify(clause, &clause_info))
	{
		case CLASS_AND:
			switch (pclass)
			{
				case CLASS_AND:

					/*
					 * AND-clause R=> AND-clause if A refutes any of B's items
					 *
					 * Needed to handle (x AND y) R=> ((!x OR !y) AND z)
					 */
					result = false;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (predicate_refuted_by_recurse(clause, pitem,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(pred_info);
					if (result)
						return result;

					/*
					 * Also check if any of A's items refutes B
					 *
					 * Needed to handle ((x OR y) AND z) R=> (!x AND !y)
					 */
					iterate_begin(citem, clause, clause_info)
					{
						if (predicate_refuted_by_recurse(citem, predicate,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(clause_info);
					return result;

				case CLASS_OR:

					/*
					 * AND-clause R=> OR-clause if A refutes each of B's items
					 */
					result = true;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (!predicate_refuted_by_recurse(clause, pitem,
														  weak))
						{
							result = false;
							break;
						}
					}
					iterate_end(pred_info);
					return result;

				case CLASS_ATOM:

					/*
					 * If B is a NOT-type clause, A R=> B if A => B's arg
					 *
					 * Since, for either type of refutation, we are starting
					 * with the premise that A is true, we can use a strong
					 * implication test in all cases.  That proves B's arg is
					 * true, which is more than we need for weak refutation if
					 * B is a simple NOT, but it allows not worrying about
					 * exactly which kind of negation clause we have.
					 */
					not_arg = extract_not_arg(predicate);
					if (not_arg &&
						predicate_implied_by_recurse(clause, not_arg,
													 false))
						return true;

					/*
					 * AND-clause R=> atom if any of A's items refutes B
					 */
					result = false;
					iterate_begin(citem, clause, clause_info)
					{
						if (predicate_refuted_by_recurse(citem, predicate,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(clause_info);
					return result;
			}
			break;

		case CLASS_OR:
			switch (pclass)
			{
				case CLASS_OR:

					/*
					 * OR-clause R=> OR-clause if A refutes each of B's items
					 */
					result = true;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (!predicate_refuted_by_recurse(clause, pitem,
														  weak))
						{
							result = false;
							break;
						}
					}
					iterate_end(pred_info);
					return result;

				case CLASS_AND:

					/*
					 * OR-clause R=> AND-clause if each of A's items refutes
					 * any of B's items.
					 */
					result = true;
					iterate_begin(citem, clause, clause_info)
					{
						bool		presult = false;

						iterate_begin(pitem, predicate, pred_info)
						{
							if (predicate_refuted_by_recurse(citem, pitem,
															 weak))
							{
								presult = true;
								break;
							}
						}
						iterate_end(pred_info);
						if (!presult)
						{
							result = false; /* citem refutes nothing */
							break;
						}
					}
					iterate_end(clause_info);
					return result;

				case CLASS_ATOM:

					/*
					 * If B is a NOT-type clause, A R=> B if A => B's arg
					 *
					 * Same logic as for the AND-clause case above.
					 */
					not_arg = extract_not_arg(predicate);
					if (not_arg &&
						predicate_implied_by_recurse(clause, not_arg,
													 false))
						return true;

					/*
					 * OR-clause R=> atom if each of A's items refutes B
					 */
					result = true;
					iterate_begin(citem, clause, clause_info)
					{
						if (!predicate_refuted_by_recurse(citem, predicate,
														  weak))
						{
							result = false;
							break;
						}
					}
					iterate_end(clause_info);
					return result;
			}
			break;

		case CLASS_ATOM:

			/*
			 * If A is a strong NOT-clause, A R=> B if B => A's arg
			 *
			 * Since A is strong, we may assume A's arg is false (not just
			 * not-true).  If B weakly implies A's arg, then B can be neither
			 * true nor null, so that strong refutation is proven.  If B
			 * strongly implies A's arg, then B cannot be true, so that weak
			 * refutation is proven.
			 */
			not_arg = extract_strong_not_arg(clause);
			if (not_arg &&
				predicate_implied_by_recurse(predicate, not_arg,
											 !weak))
				return true;

			switch (pclass)
			{
				case CLASS_AND:

					/*
					 * atom R=> AND-clause if A refutes any of B's items
					 */
					result = false;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (predicate_refuted_by_recurse(clause, pitem,
														 weak))
						{
							result = true;
							break;
						}
					}
					iterate_end(pred_info);
					return result;

				case CLASS_OR:

					/*
					 * atom R=> OR-clause if A refutes each of B's items
					 */
					result = true;
					iterate_begin(pitem, predicate, pred_info)
					{
						if (!predicate_refuted_by_recurse(clause, pitem,
														  weak))
						{
							result = false;
							break;
						}
					}
					iterate_end(pred_info);
					return result;

				case CLASS_ATOM:

					/*
					 * If B is a NOT-type clause, A R=> B if A => B's arg
					 *
					 * Same logic as for the AND-clause case above.
					 */
					not_arg = extract_not_arg(predicate);
					if (not_arg &&
						predicate_implied_by_recurse(clause, not_arg,
													 false))
						return true;

					/*
					 * atom R=> atom is the base case
					 */
					return
						predicate_refuted_by_simple_clause((Expr *) predicate,
														   clause,
														   weak);
			}
			break;
	}

	/* can't get here */
	elog(ERROR, "predicate_classify returned a bogus value");
	return false;
}


/*
 * predicate_classify
 *	  Classify an expression node as AND-type, OR-type, or neither (an atom).
 *
 * If the expression is classified as AND- or OR-type, then *info is filled
 * in with the functions needed to iterate over its components.
 *
 * This function also implements enforcement of MAX_SAOP_ARRAY_SIZE: if a
 * ScalarArrayOpExpr's array has too many elements, we just classify it as an
 * atom.  (This will result in its being passed as-is to the simple_clause
 * functions, many of which will fail to prove anything about it.) Note that we
 * cannot just stop after considering MAX_SAOP_ARRAY_SIZE elements; in general
 * that would result in wrong proofs, rather than failing to prove anything.
 */
static PredClass
predicate_classify(Node *clause, PredIterInfo info)
{
	/* Caller should not pass us NULL, nor a RestrictInfo clause */
	Assert(clause != NULL);
	Assert(!IsA(clause, RestrictInfo));

	/*
	 * If we see a List, assume it's an implicit-AND list; this is the correct
	 * semantics for lists of RestrictInfo nodes.
	 */
	if (IsA(clause, List))
	{
		info->startup_fn = list_startup_fn;
		info->next_fn = list_next_fn;
		info->cleanup_fn = list_cleanup_fn;
		return CLASS_AND;
	}

	/* Handle normal AND and OR boolean clauses */
	if (is_andclause(clause))
	{
		info->startup_fn = boolexpr_startup_fn;
		info->next_fn = list_next_fn;
		info->cleanup_fn = list_cleanup_fn;
		return CLASS_AND;
	}
	if (is_orclause(clause))
	{
		info->startup_fn = boolexpr_startup_fn;
		info->next_fn = list_next_fn;
		info->cleanup_fn = list_cleanup_fn;
		return CLASS_OR;
	}

	/* Handle ScalarArrayOpExpr */
	if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
		Node	   *arraynode = (Node *) lsecond(saop->args);

		/*
		 * We can break this down into an AND or OR structure, but only if we
		 * know how to iterate through expressions for the array's elements.
		 * We can do that if the array operand is a non-null constant or a
		 * simple ArrayExpr.
		 */
		if (arraynode && IsA(arraynode, Const) &&
			!((Const *) arraynode)->constisnull)
		{
			ArrayType  *arrayval;
			int			nelems;

			arrayval = DatumGetArrayTypeP(((Const *) arraynode)->constvalue);
			nelems = ArrayGetNItems(ARR_NDIM(arrayval), ARR_DIMS(arrayval));
			if (nelems <= MAX_SAOP_ARRAY_SIZE)
			{
				info->startup_fn = arrayconst_startup_fn;
				info->next_fn = arrayconst_next_fn;
				info->cleanup_fn = arrayconst_cleanup_fn;
				return saop->useOr ? CLASS_OR : CLASS_AND;
			}
		}
		else if (arraynode && IsA(arraynode, ArrayExpr) &&
				 !((ArrayExpr *) arraynode)->multidims &&
				 list_length(((ArrayExpr *) arraynode)->elements) <= MAX_SAOP_ARRAY_SIZE)
		{
			info->startup_fn = arrayexpr_startup_fn;
			info->next_fn = arrayexpr_next_fn;
			info->cleanup_fn = arrayexpr_cleanup_fn;
			return saop->useOr ? CLASS_OR : CLASS_AND;
		}
	}

	/* None of the above, so it's an atom */
	return CLASS_ATOM;
}

/*
 * PredIterInfo routines for iterating over regular Lists.  The iteration
 * state variable is the next ListCell to visit.
 */
static void
list_startup_fn(Node *clause, PredIterInfo info)
{
	info->state_list = (List *) clause;
	info->state = list_head(info->state_list);
}

static Node *
list_next_fn(PredIterInfo info)
{
	ListCell   *l = (ListCell *) info->state;
	Node	   *n;

	if (l == NULL)
		return NULL;
	n = lfirst(l);
	info->state = lnext(info->state_list, l);
	return n;
}

static void
list_cleanup_fn(PredIterInfo info)
{
	/* Nothing to clean up */
}

/*
 * BoolExpr needs its own startup function, but can use list_next_fn and
 * list_cleanup_fn.
 */
static void
boolexpr_startup_fn(Node *clause, PredIterInfo info)
{
	info->state_list = ((BoolExpr *) clause)->args;
	info->state = list_head(info->state_list);
}

/*
 * PredIterInfo routines for iterating over a ScalarArrayOpExpr with a
 * constant array operand.
 */
typedef struct
{
	OpExpr		opexpr;
	Const		const_expr;
	int			next_elem;
	int			num_elems;
	Datum	   *elem_values;
	bool	   *elem_nulls;
} ArrayConstIterState;

static void
arrayconst_startup_fn(Node *clause, PredIterInfo info)
{
	ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
	ArrayConstIterState *state;
	Const	   *arrayconst;
	ArrayType  *arrayval;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;

	/* Create working state struct */
	state = (ArrayConstIterState *) palloc(sizeof(ArrayConstIterState));
	info->state = state;

	/* Deconstruct the array literal */
	arrayconst = (Const *) lsecond(saop->args);
	arrayval = DatumGetArrayTypeP(arrayconst->constvalue);
	get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
						 &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arrayval,
					  ARR_ELEMTYPE(arrayval),
					  elmlen, elmbyval, elmalign,
					  &state->elem_values, &state->elem_nulls,
					  &state->num_elems);

	/* Set up a dummy OpExpr to return as the per-item node */
	state->opexpr.xpr.type = T_OpExpr;
	state->opexpr.opno = saop->opno;
	state->opexpr.opfuncid = saop->opfuncid;
	state->opexpr.opresulttype = BOOLOID;
	state->opexpr.opretset = false;
	state->opexpr.opcollid = InvalidOid;
	state->opexpr.inputcollid = saop->inputcollid;
	state->opexpr.args = list_copy(saop->args);

	/* Set up a dummy Const node to hold the per-element values */
	state->const_expr.xpr.type = T_Const;
	state->const_expr.consttype = ARR_ELEMTYPE(arrayval);
	state->const_expr.consttypmod = -1;
	state->const_expr.constcollid = arrayconst->constcollid;
	state->const_expr.constlen = elmlen;
	state->const_expr.constbyval = elmbyval;
	lsecond(state->opexpr.args) = &state->const_expr;

	/* Initialize iteration state */
	state->next_elem = 0;
}

static Node *
arrayconst_next_fn(PredIterInfo info)
{
	ArrayConstIterState *state = (ArrayConstIterState *) info->state;

	if (state->next_elem >= state->num_elems)
		return NULL;
	state->const_expr.constvalue = state->elem_values[state->next_elem];
	state->const_expr.constisnull = state->elem_nulls[state->next_elem];
	state->next_elem++;
	return (Node *) &(state->opexpr);
}

static void
arrayconst_cleanup_fn(PredIterInfo info)
{
	ArrayConstIterState *state = (ArrayConstIterState *) info->state;

	pfree(state->elem_values);
	pfree(state->elem_nulls);
	list_free(state->opexpr.args);
	pfree(state);
}

/*
 * PredIterInfo routines for iterating over a ScalarArrayOpExpr with a
 * one-dimensional ArrayExpr array operand.
 */
typedef struct
{
	OpExpr		opexpr;
	ListCell   *next;
} ArrayExprIterState;

static void
arrayexpr_startup_fn(Node *clause, PredIterInfo info)
{
	ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
	ArrayExprIterState *state;
	ArrayExpr  *arrayexpr;

	/* Create working state struct */
	state = (ArrayExprIterState *) palloc(sizeof(ArrayExprIterState));
	info->state = state;

	/* Set up a dummy OpExpr to return as the per-item node */
	state->opexpr.xpr.type = T_OpExpr;
	state->opexpr.opno = saop->opno;
	state->opexpr.opfuncid = saop->opfuncid;
	state->opexpr.opresulttype = BOOLOID;
	state->opexpr.opretset = false;
	state->opexpr.opcollid = InvalidOid;
	state->opexpr.inputcollid = saop->inputcollid;
	state->opexpr.args = list_copy(saop->args);

	/* Initialize iteration variable to first member of ArrayExpr */
	arrayexpr = (ArrayExpr *) lsecond(saop->args);
	info->state_list = arrayexpr->elements;
	state->next = list_head(arrayexpr->elements);
}

static Node *
arrayexpr_next_fn(PredIterInfo info)
{
	ArrayExprIterState *state = (ArrayExprIterState *) info->state;

	if (state->next == NULL)
		return NULL;
	lsecond(state->opexpr.args) = lfirst(state->next);
	state->next = lnext(info->state_list, state->next);
	return (Node *) &(state->opexpr);
}

static void
arrayexpr_cleanup_fn(PredIterInfo info)
{
	ArrayExprIterState *state = (ArrayExprIterState *) info->state;

	list_free(state->opexpr.args);
	pfree(state);
}


/*
 * predicate_implied_by_simple_clause
 *	  Does the predicate implication test for a "simple clause" predicate
 *	  and a "simple clause" restriction.
 *
 * We return true if able to prove the implication, false if not.
 */
static bool
predicate_implied_by_simple_clause(Expr *predicate, Node *clause,
								   bool weak)
{
	/* Allow interrupting long proof attempts */
	CHECK_FOR_INTERRUPTS();

	/*
	 * A simple and general rule is that a clause implies itself, hence we
	 * check if they are equal(); this works for any kind of expression, and
	 * for either implication definition.  (Actually, there is an implied
	 * assumption that the functions in the expression are immutable --- but
	 * this was checked for the predicate by the caller.)
	 */
	if (equal((Node *) predicate, clause))
		return true;

	/* Next we have some clause-type-specific strategies */
	switch (nodeTag(clause))
	{
		case T_OpExpr:
			{
				OpExpr	   *op = (OpExpr *) clause;

				/*----------
				 * For boolean x, "x = TRUE" is equivalent to "x", likewise
				 * "x = FALSE" is equivalent to "NOT x".  These can be worth
				 * checking because, while we preferentially simplify boolean
				 * comparisons down to "x" and "NOT x", the other form has to
				 * be dealt with anyway in the context of index conditions.
				 *
				 * We could likewise check whether the predicate is boolean
				 * equality to a constant; but there are no known use-cases
				 * for that at the moment, assuming that the predicate has
				 * been through constant-folding.
				 *----------
				 */
				if (op->opno == BooleanEqualOperator)
				{
					Node	   *rightop;

					Assert(list_length(op->args) == 2);
					rightop = lsecond(op->args);
					/* We might never see null Consts here, but better check */
					if (rightop && IsA(rightop, Const) &&
						!((Const *) rightop)->constisnull)
					{
						Node	   *leftop = linitial(op->args);

						if (DatumGetBool(((Const *) rightop)->constvalue))
						{
							/* X = true implies X */
							if (equal(predicate, leftop))
								return true;
						}
						else
						{
							/* X = false implies NOT X */
							if (is_notclause(predicate) &&
								equal(get_notclausearg(predicate), leftop))
								return true;
						}
					}
				}
			}
			break;
		default:
			break;
	}

	/* ... and some predicate-type-specific ones */
	switch (nodeTag(predicate))
	{
		case T_NullTest:
			{
				NullTest   *predntest = (NullTest *) predicate;

				switch (predntest->nulltesttype)
				{
					case IS_NOT_NULL:

						/*
						 * If the predicate is of the form "foo IS NOT NULL",
						 * and we are considering strong implication, we can
						 * conclude that the predicate is implied if the
						 * clause is strict for "foo", i.e., it must yield
						 * false or NULL when "foo" is NULL.  In that case
						 * truth of the clause ensures that "foo" isn't NULL.
						 * (Again, this is a safe conclusion because "foo"
						 * must be immutable.)  This doesn't work for weak
						 * implication, though.  Also, "row IS NOT NULL" does
						 * not act in the simple way we have in mind.
						 */
						if (!weak &&
							!predntest->argisrow &&
							clause_is_strict_for(clause,
												 (Node *) predntest->arg,
												 true))
							return true;
						break;
					case IS_NULL:
						break;
				}
			}
			break;
		default:
			break;
	}

	/*
	 * Finally, if both clauses are binary operator expressions, we may be
	 * able to prove something using the system's knowledge about operators;
	 * those proof rules are encapsulated in operator_predicate_proof().
	 */
	return operator_predicate_proof(predicate, clause, false, weak);
}

/*
 * predicate_refuted_by_simple_clause
 *	  Does the predicate refutation test for a "simple clause" predicate
 *	  and a "simple clause" restriction.
 *
 * We return true if able to prove the refutation, false if not.
 *
 * The main motivation for covering IS [NOT] NULL cases is to support using
 * IS NULL/IS NOT NULL as partition-defining constraints.
 */
static bool
predicate_refuted_by_simple_clause(Expr *predicate, Node *clause,
								   bool weak)
{
	/* Allow interrupting long proof attempts */
	CHECK_FOR_INTERRUPTS();

	/*
	 * A simple clause can't refute itself, so unlike the implication case,
	 * checking for equal() clauses isn't helpful.
	 *
	 * But relation_excluded_by_constraints() checks for self-contradictions
	 * in a list of clauses, so that we may get here with predicate and clause
	 * being actually pointer-equal, and that is worth eliminating quickly.
	 */
	if ((Node *) predicate == clause)
		return false;

	/* Next we have some clause-type-specific strategies */
	switch (nodeTag(clause))
	{
		case T_NullTest:
			{
				NullTest   *clausentest = (NullTest *) clause;

				/* row IS NULL does not act in the simple way we have in mind */
				if (clausentest->argisrow)
					return false;

				switch (clausentest->nulltesttype)
				{
					case IS_NULL:
						{
							switch (nodeTag(predicate))
							{
								case T_NullTest:
									{
										NullTest   *predntest = (NullTest *) predicate;

										/*
										 * row IS NULL does not act in the
										 * simple way we have in mind
										 */
										if (predntest->argisrow)
											return false;

										/*
										 * foo IS NULL refutes foo IS NOT
										 * NULL, at least in the non-row case,
										 * for both strong and weak refutation
										 */
										if (predntest->nulltesttype == IS_NOT_NULL &&
											equal(predntest->arg, clausentest->arg))
											return true;
									}
									break;
								default:
									break;
							}

							/*
							 * foo IS NULL weakly refutes any predicate that
							 * is strict for foo, since then the predicate
							 * must yield false or NULL (and since foo appears
							 * in the predicate, it's known immutable).
							 */
							if (weak &&
								clause_is_strict_for((Node *) predicate,
													 (Node *) clausentest->arg,
													 true))
								return true;

							return false;	/* we can't succeed below... */
						}
						break;
					case IS_NOT_NULL:
						break;
				}
			}
			break;
		default:
			break;
	}

	/* ... and some predicate-type-specific ones */
	switch (nodeTag(predicate))
	{
		case T_NullTest:
			{
				NullTest   *predntest = (NullTest *) predicate;

				/* row IS NULL does not act in the simple way we have in mind */
				if (predntest->argisrow)
					return false;

				switch (predntest->nulltesttype)
				{
					case IS_NULL:
						{
							switch (nodeTag(clause))
							{
								case T_NullTest:
									{
										NullTest   *clausentest = (NullTest *) clause;

										/*
										 * row IS NULL does not act in the
										 * simple way we have in mind
										 */
										if (clausentest->argisrow)
											return false;

										/*
										 * foo IS NOT NULL refutes foo IS NULL
										 * for both strong and weak refutation
										 */
										if (clausentest->nulltesttype == IS_NOT_NULL &&
											equal(clausentest->arg, predntest->arg))
											return true;
									}
									break;
								default:
									break;
							}

							/*
							 * When the predicate is of the form "foo IS
							 * NULL", we can conclude that the predicate is
							 * refuted if the clause is strict for "foo" (see
							 * notes for implication case).  That works for
							 * either strong or weak refutation.
							 */
							if (clause_is_strict_for(clause,
													 (Node *) predntest->arg,
													 true))
								return true;
						}
						break;
					case IS_NOT_NULL:
						break;
				}

				return false;	/* we can't succeed below... */
			}
			break;
		default:
			break;
	}

	/*
	 * Finally, if both clauses are binary operator expressions, we may be
	 * able to prove something using the system's knowledge about operators.
	 */
	return operator_predicate_proof(predicate, clause, true, weak);
}


/*
 * If clause asserts the non-truth of a subclause, return that subclause;
 * otherwise return NULL.
 */
static Node *
extract_not_arg(Node *clause)
{
	if (clause == NULL)
		return NULL;
	if (IsA(clause, BoolExpr))
	{
		BoolExpr   *bexpr = (BoolExpr *) clause;

		if (bexpr->boolop == NOT_EXPR)
			return (Node *) linitial(bexpr->args);
	}
	else if (IsA(clause, BooleanTest))
	{
		BooleanTest *btest = (BooleanTest *) clause;

		if (btest->booltesttype == IS_NOT_TRUE ||
			btest->booltesttype == IS_FALSE ||
			btest->booltesttype == IS_UNKNOWN)
			return (Node *) btest->arg;
	}
	return NULL;
}

/*
 * If clause asserts the falsity of a subclause, return that subclause;
 * otherwise return NULL.
 */
static Node *
extract_strong_not_arg(Node *clause)
{
	if (clause == NULL)
		return NULL;
	if (IsA(clause, BoolExpr))
	{
		BoolExpr   *bexpr = (BoolExpr *) clause;

		if (bexpr->boolop == NOT_EXPR)
			return (Node *) linitial(bexpr->args);
	}
	else if (IsA(clause, BooleanTest))
	{
		BooleanTest *btest = (BooleanTest *) clause;

		if (btest->booltesttype == IS_FALSE)
			return (Node *) btest->arg;
	}
	return NULL;
}


/*
 * Can we prove that "clause" returns NULL (or FALSE) if "subexpr" is
 * assumed to yield NULL?
 *
 * In most places in the planner, "strictness" refers to a guarantee that
 * an expression yields NULL output for a NULL input, and that's mostly what
 * we're looking for here.  However, at top level where the clause is known
 * to yield boolean, it may be sufficient to prove that it cannot return TRUE
 * when "subexpr" is NULL.  The caller should pass allow_false = true when
 * this weaker property is acceptable.  (When this function recurses
 * internally, we pass down allow_false = false since we need to prove actual
 * nullness of the subexpression.)
 *
 * We assume that the caller checked that least one of the input expressions
 * is immutable.  All of the proof rules here involve matching "subexpr" to
 * some portion of "clause", so that this allows assuming that "subexpr" is
 * immutable without a separate check.
 *
 * The base case is that clause and subexpr are equal().
 *
 * We can also report success if the subexpr appears as a subexpression
 * of "clause" in a place where it'd force nullness of the overall result.
 */
static bool
clause_is_strict_for(Node *clause, Node *subexpr, bool allow_false)
{
	ListCell   *lc;

	/* safety checks */
	if (clause == NULL || subexpr == NULL)
		return false;

	/*
	 * Look through any RelabelType nodes, so that we can match, say,
	 * varcharcol with lower(varcharcol::text).  (In general we could recurse
	 * through any nullness-preserving, immutable operation.)  We should not
	 * see stacked RelabelTypes here.
	 */
	if (IsA(clause, RelabelType))
		clause = (Node *) ((RelabelType *) clause)->arg;
	if (IsA(subexpr, RelabelType))
		subexpr = (Node *) ((RelabelType *) subexpr)->arg;

	/* Base case */
	if (equal(clause, subexpr))
		return true;

	/*
	 * If we have a strict operator or function, a NULL result is guaranteed
	 * if any input is forced NULL by subexpr.  This is OK even if the op or
	 * func isn't immutable, since it won't even be called on NULL input.
	 */
	if (is_opclause(clause) &&
		op_strict(((OpExpr *) clause)->opno))
	{
		foreach(lc, ((OpExpr *) clause)->args)
		{
			if (clause_is_strict_for((Node *) lfirst(lc), subexpr, false))
				return true;
		}
		return false;
	}
	if (is_funcclause(clause) &&
		func_strict(((FuncExpr *) clause)->funcid))
	{
		foreach(lc, ((FuncExpr *) clause)->args)
		{
			if (clause_is_strict_for((Node *) lfirst(lc), subexpr, false))
				return true;
		}
		return false;
	}

	/*
	 * CoerceViaIO is strict (whether or not the I/O functions it calls are).
	 * Likewise, ArrayCoerceExpr is strict for its array argument (regardless
	 * of what the per-element expression is), ConvertRowtypeExpr is strict at
	 * the row level, and CoerceToDomain is strict too.  These are worth
	 * checking mainly because it saves us having to explain to users why some
	 * type coercions are known strict and others aren't.
	 */
	if (IsA(clause, CoerceViaIO))
		return clause_is_strict_for((Node *) ((CoerceViaIO *) clause)->arg,
									subexpr, false);
	if (IsA(clause, ArrayCoerceExpr))
		return clause_is_strict_for((Node *) ((ArrayCoerceExpr *) clause)->arg,
									subexpr, false);
	if (IsA(clause, ConvertRowtypeExpr))
		return clause_is_strict_for((Node *) ((ConvertRowtypeExpr *) clause)->arg,
									subexpr, false);
	if (IsA(clause, CoerceToDomain))
		return clause_is_strict_for((Node *) ((CoerceToDomain *) clause)->arg,
									subexpr, false);

	/*
	 * ScalarArrayOpExpr is a special case.  Note that we'd only reach here
	 * with a ScalarArrayOpExpr clause if we failed to deconstruct it into an
	 * AND or OR tree, as for example if it has too many array elements.
	 */
	if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
		Node	   *scalarnode = (Node *) linitial(saop->args);
		Node	   *arraynode = (Node *) lsecond(saop->args);

		/*
		 * If we can prove the scalar input to be null, and the operator is
		 * strict, then the SAOP result has to be null --- unless the array is
		 * empty.  For an empty array, we'd get either false (for ANY) or true
		 * (for ALL).  So if allow_false = true then the proof succeeds anyway
		 * for the ANY case; otherwise we can only make the proof if we can
		 * prove the array non-empty.
		 */
		if (clause_is_strict_for(scalarnode, subexpr, false) &&
			op_strict(saop->opno))
		{
			int			nelems = 0;

			if (allow_false && saop->useOr)
				return true;	/* can succeed even if array is empty */

			if (arraynode && IsA(arraynode, Const))
			{
				Const	   *arrayconst = (Const *) arraynode;
				ArrayType  *arrval;

				/*
				 * If array is constant NULL then we can succeed, as in the
				 * case below.
				 */
				if (arrayconst->constisnull)
					return true;

				/* Otherwise, we can compute the number of elements. */
				arrval = DatumGetArrayTypeP(arrayconst->constvalue);
				nelems = ArrayGetNItems(ARR_NDIM(arrval), ARR_DIMS(arrval));
			}
			else if (arraynode && IsA(arraynode, ArrayExpr) &&
					 !((ArrayExpr *) arraynode)->multidims)
			{
				/*
				 * We can also reliably count the number of array elements if
				 * the input is a non-multidim ARRAY[] expression.
				 */
				nelems = list_length(((ArrayExpr *) arraynode)->elements);
			}

			/* Proof succeeds if array is definitely non-empty */
			if (nelems > 0)
				return true;
		}

		/*
		 * If we can prove the array input to be null, the proof succeeds in
		 * all cases, since ScalarArrayOpExpr will always return NULL for a
		 * NULL array.  Otherwise, we're done here.
		 */
		return clause_is_strict_for(arraynode, subexpr, false);
	}

	/*
	 * When recursing into an expression, we might find a NULL constant.
	 * That's certainly NULL, whether it matches subexpr or not.
	 */
	if (IsA(clause, Const))
		return ((Const *) clause)->constisnull;

	return false;
}


/*
 * Define "operator implication tables" for index operators ("cmptypes"),
 * and similar tables for refutation.
 *
 * The row compare numbers defined by indexes (see access/cmptype.h) are:
 *		1 <		2 <=	3 =		4 >=	5 >		6 <>
 * and in addition we use 6 to represent <>.  <> is not a btree-indexable
 * operator, but we assume here that if an equality operator of a btree
 * opfamily has a negator operator, the negator behaves as <> for the opfamily.
 * (This convention is also known to get_op_index_interpretation().)
 *
 * RC_implies_table[] and RC_refutes_table[] are used for cases where we have
 * two identical subexpressions and we want to know whether one operator
 * expression implies or refutes the other.  That is, if the "clause" is
 * EXPR1 clause_op EXPR2 and the "predicate" is EXPR1 pred_op EXPR2 for the
 * same two (immutable) subexpressions:
 *		RC_implies_table[clause_op-1][pred_op-1]
 *			is true if the clause implies the predicate
 *		RC_refutes_table[clause_op-1][pred_op-1]
 *			is true if the clause refutes the predicate
 * where clause_op and pred_op are cmptype numbers (from 1 to 6) in the
 * same opfamily.  For example, "x < y" implies "x <= y" and refutes
 * "x > y".
 *
 * RC_implic_table[] and RC_refute_table[] are used where we have two
 * constants that we need to compare.  The interpretation of:
 *
 *		test_op = RC_implic_table[clause_op-1][pred_op-1]
 *
 * where test_op, clause_op and pred_op are cmptypes (from 1 to 6)
 * of index operators, is as follows:
 *
 *	 If you know, for some EXPR, that "EXPR clause_op CONST1" is true, and you
 *	 want to determine whether "EXPR pred_op CONST2" must also be true, then
 *	 you can use "CONST2 test_op CONST1" as a test.  If this test returns true,
 *	 then the predicate expression must be true; if the test returns false,
 *	 then the predicate expression may be false.
 *
 * For example, if clause is "Quantity > 10" and pred is "Quantity > 5"
 * then we test "5 <= 10" which evals to true, so clause implies pred.
 *
 * Similarly, the interpretation of a RC_refute_table entry is:
 *
 *	 If you know, for some EXPR, that "EXPR clause_op CONST1" is true, and you
 *	 want to determine whether "EXPR pred_op CONST2" must be false, then
 *	 you can use "CONST2 test_op CONST1" as a test.  If this test returns true,
 *	 then the predicate expression must be false; if the test returns false,
 *	 then the predicate expression may be true.
 *
 * For example, if clause is "Quantity > 10" and pred is "Quantity < 5"
 * then we test "5 <= 10" which evals to true, so clause refutes pred.
 *
 * An entry where test_op == 0 means the implication cannot be determined.
 */

#define RCLT COMPARE_LT
#define RCLE COMPARE_LE
#define RCEQ COMPARE_EQ
#define RCGE COMPARE_GE
#define RCGT COMPARE_GT
#define RCNE COMPARE_NE

/* We use "none" for 0/false to make the tables align nicely */
#define none 0

static const bool RC_implies_table[6][6] = {
/*
 *			The predicate operator:
 *	 LT    LE	 EQ    GE	 GT    NE
 */
	{true, true, none, none, none, true},	/* LT */
	{none, true, none, none, none, none},	/* LE */
	{none, true, true, true, none, none},	/* EQ */
	{none, none, none, true, none, none},	/* GE */
	{none, none, none, true, true, true},	/* GT */
	{none, none, none, none, none, true}	/* NE */
};

static const bool RC_refutes_table[6][6] = {
/*
 *			The predicate operator:
 *	 LT    LE	 EQ    GE	 GT    NE
 */
	{none, none, true, true, true, none},	/* LT */
	{none, none, none, none, true, none},	/* LE */
	{true, none, none, none, true, true},	/* EQ */
	{true, none, none, none, none, none},	/* GE */
	{true, true, true, none, none, none},	/* GT */
	{none, none, true, none, none, none}	/* NE */
};

static const CompareType RC_implic_table[6][6] = {
/*
 *			The predicate operator:
 *	 LT    LE	 EQ    GE	 GT    NE
 */
	{RCGE, RCGE, none, none, none, RCGE},	/* LT */
	{RCGT, RCGE, none, none, none, RCGT},	/* LE */
	{RCGT, RCGE, RCEQ, RCLE, RCLT, RCNE},	/* EQ */
	{none, none, none, RCLE, RCLT, RCLT},	/* GE */
	{none, none, none, RCLE, RCLE, RCLE},	/* GT */
	{none, none, none, none, none, RCEQ}	/* NE */
};

static const CompareType RC_refute_table[6][6] = {
/*
 *			The predicate operator:
 *	 LT    LE	 EQ    GE	 GT    NE
 */
	{none, none, RCGE, RCGE, RCGE, none},	/* LT */
	{none, none, RCGT, RCGT, RCGE, none},	/* LE */
	{RCLE, RCLT, RCNE, RCGT, RCGE, RCEQ},	/* EQ */
	{RCLE, RCLT, RCLT, none, none, none},	/* GE */
	{RCLE, RCLE, RCLE, none, none, none},	/* GT */
	{none, none, RCEQ, none, none, none}	/* NE */
};


/*
 * operator_predicate_proof
 *	  Does the predicate implication or refutation test for a "simple clause"
 *	  predicate and a "simple clause" restriction, when both are operator
 *	  clauses using related operators and identical input expressions.
 *
 * When refute_it == false, we want to prove the predicate true;
 * when refute_it == true, we want to prove the predicate false.
 * (There is enough common code to justify handling these two cases
 * in one routine.)  We return true if able to make the proof, false
 * if not able to prove it.
 *
 * We mostly need not distinguish strong vs. weak implication/refutation here.
 * This depends on the assumption that a pair of related operators (i.e.,
 * commutators, negators, or btree opfamily siblings) will not return one NULL
 * and one non-NULL result for the same inputs.  Then, for the proof types
 * where we start with an assumption of truth of the clause, the predicate
 * operator could not return NULL either, so it doesn't matter whether we are
 * trying to make a strong or weak proof.  For weak implication, it could be
 * that the clause operator returned NULL, but then the predicate operator
 * would as well, so that the weak implication still holds.  This argument
 * doesn't apply in the case where we are considering two different constant
 * values, since then the operators aren't being given identical inputs.  But
 * we only support that for btree operators, for which we can assume that all
 * non-null inputs result in non-null outputs, so that it doesn't matter which
 * two non-null constants we consider.  If either constant is NULL, we have
 * to think harder, but sometimes the proof still works, as explained below.
 *
 * We can make proofs involving several expression forms (here "foo" and "bar"
 * represent subexpressions that are identical according to equal()):
 *	"foo op1 bar" refutes "foo op2 bar" if op1 is op2's negator
 *	"foo op1 bar" implies "bar op2 foo" if op1 is op2's commutator
 *	"foo op1 bar" refutes "bar op2 foo" if op1 is negator of op2's commutator
 *	"foo op1 bar" can imply/refute "foo op2 bar" based on btree semantics
 *	"foo op1 bar" can imply/refute "bar op2 foo" based on btree semantics
 *	"foo op1 const1" can imply/refute "foo op2 const2" based on btree semantics
 *
 * For the last three cases, op1 and op2 have to be members of the same btree
 * operator family.  When both subexpressions are identical, the idea is that,
 * for instance, x < y implies x <= y, independently of exactly what x and y
 * are.  If we have two different constants compared to the same expression
 * foo, we have to execute a comparison between the two constant values
 * in order to determine the result; for instance, foo < c1 implies foo < c2
 * if c1 <= c2.  We assume it's safe to compare the constants at plan time
 * if the comparison operator is immutable.
 *
 * Note: all the operators and subexpressions have to be immutable for the
 * proof to be safe.  We assume the predicate expression is entirely immutable,
 * so no explicit check on the subexpressions is needed here, but in some
 * cases we need an extra check of operator immutability.  In particular,
 * btree opfamilies can contain cross-type operators that are merely stable,
 * and we dare not make deductions with those.
 */
static bool
operator_predicate_proof(Expr *predicate, Node *clause,
						 bool refute_it, bool weak)
{
	OpExpr	   *pred_opexpr,
			   *clause_opexpr;
	Oid			pred_collation,
				clause_collation;
	Oid			pred_op,
				clause_op,
				test_op;
	Node	   *pred_leftop,
			   *pred_rightop,
			   *clause_leftop,
			   *clause_rightop;
	Const	   *pred_const,
			   *clause_const;
	Expr	   *test_expr;
	ExprState  *test_exprstate;
	Datum		test_result;
	bool		isNull;
	EState	   *estate;
	MemoryContext oldcontext;

	/*
	 * Both expressions must be binary opclauses, else we can't do anything.
	 *
	 * Note: in future we might extend this logic to other operator-based
	 * constructs such as DistinctExpr.  But the planner isn't very smart
	 * about DistinctExpr in general, and this probably isn't the first place
	 * to fix if you want to improve that.
	 */
	if (!is_opclause(predicate))
		return false;
	pred_opexpr = (OpExpr *) predicate;
	if (list_length(pred_opexpr->args) != 2)
		return false;
	if (!is_opclause(clause))
		return false;
	clause_opexpr = (OpExpr *) clause;
	if (list_length(clause_opexpr->args) != 2)
		return false;

	/*
	 * If they're marked with different collations then we can't do anything.
	 * This is a cheap test so let's get it out of the way early.
	 */
	pred_collation = pred_opexpr->inputcollid;
	clause_collation = clause_opexpr->inputcollid;
	if (pred_collation != clause_collation)
		return false;

	/* Grab the operator OIDs now too.  We may commute these below. */
	pred_op = pred_opexpr->opno;
	clause_op = clause_opexpr->opno;

	/*
	 * We have to match up at least one pair of input expressions.
	 */
	pred_leftop = (Node *) linitial(pred_opexpr->args);
	pred_rightop = (Node *) lsecond(pred_opexpr->args);
	clause_leftop = (Node *) linitial(clause_opexpr->args);
	clause_rightop = (Node *) lsecond(clause_opexpr->args);

	if (equal(pred_leftop, clause_leftop))
	{
		if (equal(pred_rightop, clause_rightop))
		{
			/* We have x op1 y and x op2 y */
			return operator_same_subexprs_proof(pred_op, clause_op, refute_it);
		}
		else
		{
			/* Fail unless rightops are both Consts */
			if (pred_rightop == NULL || !IsA(pred_rightop, Const))
				return false;
			pred_const = (Const *) pred_rightop;
			if (clause_rightop == NULL || !IsA(clause_rightop, Const))
				return false;
			clause_const = (Const *) clause_rightop;
		}
	}
	else if (equal(pred_rightop, clause_rightop))
	{
		/* Fail unless leftops are both Consts */
		if (pred_leftop == NULL || !IsA(pred_leftop, Const))
			return false;
		pred_const = (Const *) pred_leftop;
		if (clause_leftop == NULL || !IsA(clause_leftop, Const))
			return false;
		clause_const = (Const *) clause_leftop;
		/* Commute both operators so we can assume Consts are on the right */
		pred_op = get_commutator(pred_op);
		if (!OidIsValid(pred_op))
			return false;
		clause_op = get_commutator(clause_op);
		if (!OidIsValid(clause_op))
			return false;
	}
	else if (equal(pred_leftop, clause_rightop))
	{
		if (equal(pred_rightop, clause_leftop))
		{
			/* We have x op1 y and y op2 x */
			/* Commute pred_op that we can treat this like a straight match */
			pred_op = get_commutator(pred_op);
			if (!OidIsValid(pred_op))
				return false;
			return operator_same_subexprs_proof(pred_op, clause_op, refute_it);
		}
		else
		{
			/* Fail unless pred_rightop/clause_leftop are both Consts */
			if (pred_rightop == NULL || !IsA(pred_rightop, Const))
				return false;
			pred_const = (Const *) pred_rightop;
			if (clause_leftop == NULL || !IsA(clause_leftop, Const))
				return false;
			clause_const = (Const *) clause_leftop;
			/* Commute clause_op so we can assume Consts are on the right */
			clause_op = get_commutator(clause_op);
			if (!OidIsValid(clause_op))
				return false;
		}
	}
	else if (equal(pred_rightop, clause_leftop))
	{
		/* Fail unless pred_leftop/clause_rightop are both Consts */
		if (pred_leftop == NULL || !IsA(pred_leftop, Const))
			return false;
		pred_const = (Const *) pred_leftop;
		if (clause_rightop == NULL || !IsA(clause_rightop, Const))
			return false;
		clause_const = (Const *) clause_rightop;
		/* Commute pred_op so we can assume Consts are on the right */
		pred_op = get_commutator(pred_op);
		if (!OidIsValid(pred_op))
			return false;
	}
	else
	{
		/* Failed to match up any of the subexpressions, so we lose */
		return false;
	}

	/*
	 * We have two identical subexpressions, and two other subexpressions that
	 * are not identical but are both Consts; and we have commuted the
	 * operators if necessary so that the Consts are on the right.  We'll need
	 * to compare the Consts' values.  If either is NULL, we can't do that, so
	 * usually the proof fails ... but in some cases we can claim success.
	 */
	if (clause_const->constisnull)
	{
		/* If clause_op isn't strict, we can't prove anything */
		if (!op_strict(clause_op))
			return false;

		/*
		 * At this point we know that the clause returns NULL.  For proof
		 * types that assume truth of the clause, this means the proof is
		 * vacuously true (a/k/a "false implies anything").  That's all proof
		 * types except weak implication.
		 */
		if (!(weak && !refute_it))
			return true;

		/*
		 * For weak implication, it's still possible for the proof to succeed,
		 * if the predicate can also be proven NULL.  In that case we've got
		 * NULL => NULL which is valid for this proof type.
		 */
		if (pred_const->constisnull && op_strict(pred_op))
			return true;
		/* Else the proof fails */
		return false;
	}
	if (pred_const->constisnull)
	{
		/*
		 * If the pred_op is strict, we know the predicate yields NULL, which
		 * means the proof succeeds for either weak implication or weak
		 * refutation.
		 */
		if (weak && op_strict(pred_op))
			return true;
		/* Else the proof fails */
		return false;
	}

	/*
	 * Lookup the constant-comparison operator using the system catalogs and
	 * the operator implication tables.
	 */
	test_op = get_btree_test_op(pred_op, clause_op, refute_it);

	if (!OidIsValid(test_op))
	{
		/* couldn't find a suitable comparison operator */
		return false;
	}

	/*
	 * Evaluate the test.  For this we need an EState.
	 */
	estate = CreateExecutorState();

	/* We can use the estate's working context to avoid memory leaks. */
	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	/* Build expression tree */
	test_expr = make_opclause(test_op,
							  BOOLOID,
							  false,
							  (Expr *) pred_const,
							  (Expr *) clause_const,
							  InvalidOid,
							  pred_collation);

	/* Fill in opfuncids */
	fix_opfuncids((Node *) test_expr);

	/* Prepare it for execution */
	test_exprstate = ExecInitExpr(test_expr, NULL);

	/* And execute it. */
	test_result = ExecEvalExprSwitchContext(test_exprstate,
											GetPerTupleExprContext(estate),
											&isNull);

	/* Get back to outer memory context */
	MemoryContextSwitchTo(oldcontext);

	/* Release all the junk we just created */
	FreeExecutorState(estate);

	if (isNull)
	{
		/* Treat a null result as non-proof ... but it's a tad fishy ... */
		elog(DEBUG2, "null predicate test result");
		return false;
	}
	return DatumGetBool(test_result);
}


/*
 * operator_same_subexprs_proof
 *	  Assuming that EXPR1 clause_op EXPR2 is true, try to prove or refute
 *	  EXPR1 pred_op EXPR2.
 *
 * Return true if able to make the proof, false if not able to prove it.
 */
static bool
operator_same_subexprs_proof(Oid pred_op, Oid clause_op, bool refute_it)
{
	/*
	 * A simple and general rule is that the predicate is proven if clause_op
	 * and pred_op are the same, or refuted if they are each other's negators.
	 * We need not check immutability since the pred_op is already known
	 * immutable.  (Actually, by this point we may have the commutator of a
	 * known-immutable pred_op, but that should certainly be immutable too.
	 * Likewise we don't worry whether the pred_op's negator is immutable.)
	 *
	 * Note: the "same" case won't get here if we actually had EXPR1 clause_op
	 * EXPR2 and EXPR1 pred_op EXPR2, because the overall-expression-equality
	 * test in predicate_implied_by_simple_clause would have caught it.  But
	 * we can see the same operator after having commuted the pred_op.
	 */
	if (refute_it)
	{
		if (get_negator(pred_op) == clause_op)
			return true;
	}
	else
	{
		if (pred_op == clause_op)
			return true;
	}

	/*
	 * Otherwise, see if we can determine the implication by finding the
	 * operators' relationship via some btree opfamily.
	 */
	return operator_same_subexprs_lookup(pred_op, clause_op, refute_it);
}


/*
 * We use a lookaside table to cache the result of btree proof operator
 * lookups, since the actual lookup is pretty expensive and doesn't change
 * for any given pair of operators (at least as long as pg_amop doesn't
 * change).  A single hash entry stores both implication and refutation
 * results for a given pair of operators; but note we may have determined
 * only one of those sets of results as yet.
 */
typedef struct OprProofCacheKey
{
	Oid			pred_op;		/* predicate operator */
	Oid			clause_op;		/* clause operator */
} OprProofCacheKey;

typedef struct OprProofCacheEntry
{
	/* the hash lookup key MUST BE FIRST */
	OprProofCacheKey key;

	bool		have_implic;	/* do we know the implication result? */
	bool		have_refute;	/* do we know the refutation result? */
	bool		same_subexprs_implies;	/* X clause_op Y implies X pred_op Y? */
	bool		same_subexprs_refutes;	/* X clause_op Y refutes X pred_op Y? */
	Oid			implic_test_op; /* OID of the test operator, or 0 if none */
	Oid			refute_test_op; /* OID of the test operator, or 0 if none */
} OprProofCacheEntry;

static HTAB *OprProofCacheHash = NULL;


/*
 * lookup_proof_cache
 *	  Get, and fill in if necessary, the appropriate cache entry.
 */
static OprProofCacheEntry *
lookup_proof_cache(Oid pred_op, Oid clause_op, bool refute_it)
{
	OprProofCacheKey key;
	OprProofCacheEntry *cache_entry;
	bool		cfound;
	bool		same_subexprs = false;
	Oid			test_op = InvalidOid;
	bool		found = false;
	List	   *pred_op_infos,
			   *clause_op_infos;
	ListCell   *lcp,
			   *lcc;

	/*
	 * Find or make a cache entry for this pair of operators.
	 */
	if (OprProofCacheHash == NULL)
	{
		/* First time through: initialize the hash table */
		HASHCTL		ctl;

		ctl.keysize = sizeof(OprProofCacheKey);
		ctl.entrysize = sizeof(OprProofCacheEntry);
		OprProofCacheHash = hash_create("Btree proof lookup cache", 256,
										&ctl, HASH_ELEM | HASH_BLOBS);

		/* Arrange to flush cache on pg_amop changes */
		CacheRegisterSyscacheCallback(AMOPOPID,
									  InvalidateOprProofCacheCallBack,
									  (Datum) 0);
	}

	key.pred_op = pred_op;
	key.clause_op = clause_op;
	cache_entry = (OprProofCacheEntry *) hash_search(OprProofCacheHash,
													 &key,
													 HASH_ENTER, &cfound);
	if (!cfound)
	{
		/* new cache entry, set it invalid */
		cache_entry->have_implic = false;
		cache_entry->have_refute = false;
	}
	else
	{
		/* pre-existing cache entry, see if we know the answer yet */
		if (refute_it ? cache_entry->have_refute : cache_entry->have_implic)
			return cache_entry;
	}

	/*
	 * Try to find a btree opfamily containing the given operators.
	 *
	 * We must find a btree opfamily that contains both operators, else the
	 * implication can't be determined.  Also, the opfamily must contain a
	 * suitable test operator taking the operators' righthand datatypes.
	 *
	 * If there are multiple matching opfamilies, assume we can use any one to
	 * determine the logical relationship of the two operators and the correct
	 * corresponding test operator.  This should work for any logically
	 * consistent opfamilies.
	 *
	 * Note that we can determine the operators' relationship for
	 * same-subexprs cases even from an opfamily that lacks a usable test
	 * operator.  This can happen in cases with incomplete sets of cross-type
	 * comparison operators.
	 */
	clause_op_infos = get_op_index_interpretation(clause_op);
	if (clause_op_infos)
		pred_op_infos = get_op_index_interpretation(pred_op);
	else						/* no point in looking */
		pred_op_infos = NIL;

	foreach(lcp, pred_op_infos)
	{
		OpIndexInterpretation *pred_op_info = lfirst(lcp);
		Oid			opfamily_id = pred_op_info->opfamily_id;

		foreach(lcc, clause_op_infos)
		{
			OpIndexInterpretation *clause_op_info = lfirst(lcc);
			CompareType pred_cmptype,
						clause_cmptype,
						test_cmptype;

			/* Must find them in same opfamily */
			if (opfamily_id != clause_op_info->opfamily_id)
				continue;
			/* Lefttypes should match */
			Assert(clause_op_info->oplefttype == pred_op_info->oplefttype);

			pred_cmptype = pred_op_info->cmptype;
			clause_cmptype = clause_op_info->cmptype;

			/*
			 * Check to see if we can make a proof for same-subexpressions
			 * cases based on the operators' relationship in this opfamily.
			 */
			if (refute_it)
				same_subexprs |= RC_refutes_table[clause_cmptype - 1][pred_cmptype - 1];
			else
				same_subexprs |= RC_implies_table[clause_cmptype - 1][pred_cmptype - 1];

			/*
			 * Look up the "test" cmptype number in the implication table
			 */
			if (refute_it)
				test_cmptype = RC_refute_table[clause_cmptype - 1][pred_cmptype - 1];
			else
				test_cmptype = RC_implic_table[clause_cmptype - 1][pred_cmptype - 1];

			if (test_cmptype == 0)
			{
				/* Can't determine implication using this interpretation */
				continue;
			}

			/*
			 * See if opfamily has an operator for the test cmptype and the
			 * datatypes.
			 */
			if (test_cmptype == RCNE)
			{
				test_op = get_opfamily_member_for_cmptype(opfamily_id,
														  pred_op_info->oprighttype,
														  clause_op_info->oprighttype,
														  COMPARE_EQ);
				if (OidIsValid(test_op))
					test_op = get_negator(test_op);
			}
			else
			{
				test_op = get_opfamily_member_for_cmptype(opfamily_id,
														  pred_op_info->oprighttype,
														  clause_op_info->oprighttype,
														  test_cmptype);
			}

			if (!OidIsValid(test_op))
				continue;

			/*
			 * Last check: test_op must be immutable.
			 *
			 * Note that we require only the test_op to be immutable, not the
			 * original clause_op.  (pred_op is assumed to have been checked
			 * immutable by the caller.)  Essentially we are assuming that the
			 * opfamily is consistent even if it contains operators that are
			 * merely stable.
			 */
			if (op_volatile(test_op) == PROVOLATILE_IMMUTABLE)
			{
				found = true;
				break;
			}
		}

		if (found)
			break;
	}

	list_free_deep(pred_op_infos);
	list_free_deep(clause_op_infos);

	if (!found)
	{
		/* couldn't find a suitable comparison operator */
		test_op = InvalidOid;
	}

	/*
	 * If we think we were able to prove something about same-subexpressions
	 * cases, check to make sure the clause_op is immutable before believing
	 * it completely.  (Usually, the clause_op would be immutable if the
	 * pred_op is, but it's not entirely clear that this must be true in all
	 * cases, so let's check.)
	 */
	if (same_subexprs &&
		op_volatile(clause_op) != PROVOLATILE_IMMUTABLE)
		same_subexprs = false;

	/* Cache the results, whether positive or negative */
	if (refute_it)
	{
		cache_entry->refute_test_op = test_op;
		cache_entry->same_subexprs_refutes = same_subexprs;
		cache_entry->have_refute = true;
	}
	else
	{
		cache_entry->implic_test_op = test_op;
		cache_entry->same_subexprs_implies = same_subexprs;
		cache_entry->have_implic = true;
	}

	return cache_entry;
}

/*
 * operator_same_subexprs_lookup
 *	  Convenience subroutine to look up the cached answer for
 *	  same-subexpressions cases.
 */
static bool
operator_same_subexprs_lookup(Oid pred_op, Oid clause_op, bool refute_it)
{
	OprProofCacheEntry *cache_entry;

	cache_entry = lookup_proof_cache(pred_op, clause_op, refute_it);
	if (refute_it)
		return cache_entry->same_subexprs_refutes;
	else
		return cache_entry->same_subexprs_implies;
}

/*
 * get_btree_test_op
 *	  Identify the comparison operator needed for a btree-operator
 *	  proof or refutation involving comparison of constants.
 *
 * Given the truth of a clause "var clause_op const1", we are attempting to
 * prove or refute a predicate "var pred_op const2".  The identities of the
 * two operators are sufficient to determine the operator (if any) to compare
 * const2 to const1 with.
 *
 * Returns the OID of the operator to use, or InvalidOid if no proof is
 * possible.
 */
static Oid
get_btree_test_op(Oid pred_op, Oid clause_op, bool refute_it)
{
	OprProofCacheEntry *cache_entry;

	cache_entry = lookup_proof_cache(pred_op, clause_op, refute_it);
	if (refute_it)
		return cache_entry->refute_test_op;
	else
		return cache_entry->implic_test_op;
}


/*
 * Callback for pg_amop inval events
 */
static void
InvalidateOprProofCacheCallBack(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	OprProofCacheEntry *hentry;

	Assert(OprProofCacheHash != NULL);

	/* Currently we just reset all entries; hard to be smarter ... */
	hash_seq_init(&status, OprProofCacheHash);

	while ((hentry = (OprProofCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		hentry->have_implic = false;
		hentry->have_refute = false;
	}
}
