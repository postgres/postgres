/*-------------------------------------------------------------------------
 *
 * predtest.c
 *	  Routines to attempt to prove logical implications between predicate
 *	  expressions.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/util/predtest.c,v 1.1 2005/06/10 22:25:36 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_amop.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "optimizer/clauses.h"
#include "optimizer/predtest.h"
#include "utils/catcache.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static bool predicate_implied_by_recurse(Node *clause, Node *predicate);
static bool predicate_implied_by_simple_clause(Expr *predicate, Node *clause);


/*
 * predicate_implied_by
 *	  Recursively checks whether the clauses in restrictinfo_list imply
 *	  that the given predicate is true.
 *
 *	  The top-level List structure of each list corresponds to an AND list.
 *	  We assume that eval_const_expressions() has been applied and so there
 *	  are no un-flattened ANDs or ORs (e.g., no AND immediately within an AND,
 *	  including AND just below the top-level List structure).
 *	  If this is not true we might fail to prove an implication that is
 *	  valid, but no worse consequences will ensue.
 */
bool
predicate_implied_by(List *predicate_list, List *restrictinfo_list)
{
	ListCell   *item;

	if (predicate_list == NIL)
		return true;			/* no predicate: implication is vacuous */
	if (restrictinfo_list == NIL)
		return false;			/* no restriction: implication must fail */

	/*
	 * In all cases where the predicate is an AND-clause,
	 * predicate_implied_by_recurse() will prefer to iterate over the
	 * predicate's components.  So we can just do that to start with here,
	 * and eliminate the need for predicate_implied_by_recurse() to handle
	 * a bare List on the predicate side.
	 *
	 * Logic is: restriction must imply each of the AND'ed predicate items.
	 */
	foreach(item, predicate_list)
	{
		if (!predicate_implied_by_recurse((Node *) restrictinfo_list,
										  lfirst(item)))
			return false;
	}
	return true;
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
 * down or eliminated where feasible by prepqual.c.
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
 * A bare List node on the restriction side is interpreted as an AND clause,
 * in order to handle the top-level restriction List properly.  However we
 * need not consider a List on the predicate side since predicate_implied_by()
 * already expanded it.
 *
 * We have to be prepared to handle RestrictInfo nodes in the restrictinfo
 * tree, though not in the predicate tree.
 *----------
 */
static bool
predicate_implied_by_recurse(Node *clause, Node *predicate)
{
	ListCell   *item;

	Assert(clause != NULL);
	/* skip through RestrictInfo */
	if (IsA(clause, RestrictInfo))
	{
		clause = (Node *) ((RestrictInfo *) clause)->clause;
		Assert(clause != NULL);
		Assert(!IsA(clause, RestrictInfo));
	}
	Assert(predicate != NULL);

	/*
	 * Since a restriction List clause is handled the same as an AND clause,
	 * we can avoid duplicate code like this:
	 */
	if (and_clause(clause))
		clause = (Node *) ((BoolExpr *) clause)->args;

	if (IsA(clause, List))
	{
		if (and_clause(predicate))
		{
			/* AND-clause => AND-clause if A implies each of B's items */
			foreach(item, ((BoolExpr *) predicate)->args)
			{
				if (!predicate_implied_by_recurse(clause, lfirst(item)))
					return false;
			}
			return true;
		}
		else if (or_clause(predicate))
		{
			/* AND-clause => OR-clause if A implies any of B's items */
			/* Needed to handle (x AND y) => ((x AND y) OR z) */
			foreach(item, ((BoolExpr *) predicate)->args)
			{
				if (predicate_implied_by_recurse(clause, lfirst(item)))
					return true;
			}
			/* Also check if any of A's items implies B */
			/* Needed to handle ((x OR y) AND z) => (x OR y) */
			foreach(item, (List *) clause)
			{
				if (predicate_implied_by_recurse(lfirst(item), predicate))
					return true;
			}
			return false;
		}
		else
		{
			/* AND-clause => atom if any of A's items implies B */
			foreach(item, (List *) clause)
			{
				if (predicate_implied_by_recurse(lfirst(item), predicate))
					return true;
			}
			return false;
		}
	}
	else if (or_clause(clause))
	{
		if (or_clause(predicate))
		{
			/*
			 * OR-clause => OR-clause if each of A's items implies any of
			 * B's items.  Messy but can't do it any more simply.
			 */
			foreach(item, ((BoolExpr *) clause)->args)
			{
				Node	   *citem = lfirst(item);
				ListCell   *item2;

				foreach(item2, ((BoolExpr *) predicate)->args)
				{
					if (predicate_implied_by_recurse(citem, lfirst(item2)))
						break;
				}
				if (item2 == NULL)
					return false; /* doesn't imply any of B's */
			}
			return true;
		}
		else
		{
			/* OR-clause => AND-clause if each of A's items implies B */
			/* OR-clause => atom if each of A's items implies B */
			foreach(item, ((BoolExpr *) clause)->args)
			{
				if (!predicate_implied_by_recurse(lfirst(item), predicate))
					return false;
			}
			return true;
		}
	}
	else
	{
		if (and_clause(predicate))
		{
			/* atom => AND-clause if A implies each of B's items */
			foreach(item, ((BoolExpr *) predicate)->args)
			{
				if (!predicate_implied_by_recurse(clause, lfirst(item)))
					return false;
			}
			return true;
		}
		else if (or_clause(predicate))
		{
			/* atom => OR-clause if A implies any of B's items */
			foreach(item, ((BoolExpr *) predicate)->args)
			{
				if (predicate_implied_by_recurse(clause, lfirst(item)))
					return true;
			}
			return false;
		}
		else
		{
			/* atom => atom is the base case */
			return predicate_implied_by_simple_clause((Expr *) predicate,
													  clause);
		}
	}
}


/*
 * Define an "operator implication table" for btree operators ("strategies").
 *
 * The strategy numbers defined by btree indexes (see access/skey.h) are:
 *		(1) <	(2) <=	 (3) =	 (4) >=   (5) >
 * and in addition we use (6) to represent <>.	<> is not a btree-indexable
 * operator, but we assume here that if the equality operator of a btree
 * opclass has a negator operator, the negator behaves as <> for the opclass.
 *
 * The interpretation of:
 *
 *		test_op = BT_implic_table[given_op-1][target_op-1]
 *
 * where test_op, given_op and target_op are strategy numbers (from 1 to 6)
 * of btree operators, is as follows:
 *
 *	 If you know, for some ATTR, that "ATTR given_op CONST1" is true, and you
 *	 want to determine whether "ATTR target_op CONST2" must also be true, then
 *	 you can use "CONST2 test_op CONST1" as a test.  If this test returns true,
 *	 then the target expression must be true; if the test returns false, then
 *	 the target expression may be false.
 *
 * An entry where test_op == 0 means the implication cannot be determined,
 * i.e., this test should always be considered false.
 */

#define BTLT BTLessStrategyNumber
#define BTLE BTLessEqualStrategyNumber
#define BTEQ BTEqualStrategyNumber
#define BTGE BTGreaterEqualStrategyNumber
#define BTGT BTGreaterStrategyNumber
#define BTNE 6

static const StrategyNumber
			BT_implic_table[6][6] = {
/*
 *			The target operator:
 *
 *	 LT    LE    EQ GE GT NE
 */
	{BTGE, BTGE, 0, 0, 0, BTGE},			/* LT */
	{BTGT, BTGE, 0, 0, 0, BTGT},			/* LE */
	{BTGT, BTGE, BTEQ, BTLE, BTLT, BTNE},	/* EQ */
	{0, 0, 0, BTLE, BTLT, BTLT},			/* GE */
	{0, 0, 0, BTLE, BTLE, BTLE},			/* GT */
	{0, 0, 0, 0, 0, BTEQ}					/* NE */
};


/*----------
 * predicate_implied_by_simple_clause
 *	  Does the predicate implication test for a "simple clause" predicate
 *	  and a "simple clause" restriction.
 *
 * We have three strategies for determining whether one simple clause
 * implies another:
 *
 * A simple and general way is to see if they are equal(); this works for any
 * kind of expression.	(Actually, there is an implied assumption that the
 * functions in the expression are immutable, ie dependent only on their input
 * arguments --- but this was checked for the predicate by CheckPredicate().)
 *
 * When the predicate is of the form "foo IS NOT NULL", we can conclude that
 * the predicate is implied if the clause is a strict operator or function
 * that has "foo" as an input.	In this case the clause must yield NULL when
 * "foo" is NULL, which we can take as equivalent to FALSE because we know
 * we are within an AND/OR subtree of a WHERE clause.  (Again, "foo" is
 * already known immutable, so the clause will certainly always fail.)
 *
 * Our other way works only for binary boolean opclauses of the form
 * "foo op constant", where "foo" is the same in both clauses.	The operators
 * and constants can be different but the operators must be in the same btree
 * operator class.	We use the above operator implication table to be able to
 * derive implications between nonidentical clauses.  (Note: "foo" is known
 * immutable, and constants are surely immutable, but we have to check that
 * the operators are too.  As of 8.0 it's possible for opclasses to contain
 * operators that are merely stable, and we dare not make deductions with
 * these.)
 *
 * Eventually, rtree operators could also be handled by defining an
 * appropriate "RT_implic_table" array.
 *----------
 */
static bool
predicate_implied_by_simple_clause(Expr *predicate, Node *clause)
{
	Node	   *leftop,
			   *rightop;
	Node	   *pred_var,
			   *clause_var;
	Const	   *pred_const,
			   *clause_const;
	bool		pred_var_on_left,
				clause_var_on_left,
				pred_op_negated;
	Oid			pred_op,
				clause_op,
				pred_op_negator,
				clause_op_negator,
				test_op = InvalidOid;
	Oid			opclass_id;
	bool		found = false;
	StrategyNumber pred_strategy,
				clause_strategy,
				test_strategy;
	Oid			clause_subtype;
	Expr	   *test_expr;
	ExprState  *test_exprstate;
	Datum		test_result;
	bool		isNull;
	CatCList   *catlist;
	int			i;
	EState	   *estate;
	MemoryContext oldcontext;

	/* First try the equal() test */
	if (equal((Node *) predicate, clause))
		return true;

	/* Next try the IS NOT NULL case */
	if (predicate && IsA(predicate, NullTest) &&
		((NullTest *) predicate)->nulltesttype == IS_NOT_NULL)
	{
		Expr	   *nonnullarg = ((NullTest *) predicate)->arg;

		if (is_opclause(clause) &&
			list_member(((OpExpr *) clause)->args, nonnullarg) &&
			op_strict(((OpExpr *) clause)->opno))
			return true;
		if (is_funcclause(clause) &&
			list_member(((FuncExpr *) clause)->args, nonnullarg) &&
			func_strict(((FuncExpr *) clause)->funcid))
			return true;
		return false;			/* we can't succeed below... */
	}

	/*
	 * Can't do anything more unless they are both binary opclauses with a
	 * Const on one side, and identical subexpressions on the other sides.
	 * Note we don't have to think about binary relabeling of the Const
	 * node, since that would have been folded right into the Const.
	 *
	 * If either Const is null, we also fail right away; this assumes that
	 * the test operator will always be strict.
	 */
	if (!is_opclause(predicate))
		return false;
	leftop = get_leftop(predicate);
	rightop = get_rightop(predicate);
	if (rightop == NULL)
		return false;			/* not a binary opclause */
	if (IsA(rightop, Const))
	{
		pred_var = leftop;
		pred_const = (Const *) rightop;
		pred_var_on_left = true;
	}
	else if (IsA(leftop, Const))
	{
		pred_var = rightop;
		pred_const = (Const *) leftop;
		pred_var_on_left = false;
	}
	else
		return false;			/* no Const to be found */
	if (pred_const->constisnull)
		return false;

	if (!is_opclause(clause))
		return false;
	leftop = get_leftop((Expr *) clause);
	rightop = get_rightop((Expr *) clause);
	if (rightop == NULL)
		return false;			/* not a binary opclause */
	if (IsA(rightop, Const))
	{
		clause_var = leftop;
		clause_const = (Const *) rightop;
		clause_var_on_left = true;
	}
	else if (IsA(leftop, Const))
	{
		clause_var = rightop;
		clause_const = (Const *) leftop;
		clause_var_on_left = false;
	}
	else
		return false;			/* no Const to be found */
	if (clause_const->constisnull)
		return false;

	/*
	 * Check for matching subexpressions on the non-Const sides.  We used
	 * to only allow a simple Var, but it's about as easy to allow any
	 * expression.	Remember we already know that the pred expression does
	 * not contain any non-immutable functions, so identical expressions
	 * should yield identical results.
	 */
	if (!equal(pred_var, clause_var))
		return false;

	/*
	 * Okay, get the operators in the two clauses we're comparing. Commute
	 * them if needed so that we can assume the variables are on the left.
	 */
	pred_op = ((OpExpr *) predicate)->opno;
	if (!pred_var_on_left)
	{
		pred_op = get_commutator(pred_op);
		if (!OidIsValid(pred_op))
			return false;
	}

	clause_op = ((OpExpr *) clause)->opno;
	if (!clause_var_on_left)
	{
		clause_op = get_commutator(clause_op);
		if (!OidIsValid(clause_op))
			return false;
	}

	/*
	 * Try to find a btree opclass containing the needed operators.
	 *
	 * We must find a btree opclass that contains both operators, else the
	 * implication can't be determined.  Also, the pred_op has to be of
	 * default subtype (implying left and right input datatypes are the
	 * same); otherwise it's unsafe to put the pred_const on the left side
	 * of the test.  Also, the opclass must contain a suitable test
	 * operator matching the clause_const's type (which we take to mean
	 * that it has the same subtype as the original clause_operator).
	 *
	 * If there are multiple matching opclasses, assume we can use any one to
	 * determine the logical relationship of the two operators and the
	 * correct corresponding test operator.  This should work for any
	 * logically consistent opclasses.
	 */
	catlist = SearchSysCacheList(AMOPOPID, 1,
								 ObjectIdGetDatum(pred_op),
								 0, 0, 0);

	/*
	 * If we couldn't find any opclass containing the pred_op, perhaps it
	 * is a <> operator.  See if it has a negator that is in an opclass.
	 */
	pred_op_negated = false;
	if (catlist->n_members == 0)
	{
		pred_op_negator = get_negator(pred_op);
		if (OidIsValid(pred_op_negator))
		{
			pred_op_negated = true;
			ReleaseSysCacheList(catlist);
			catlist = SearchSysCacheList(AMOPOPID, 1,
									   ObjectIdGetDatum(pred_op_negator),
										 0, 0, 0);
		}
	}

	/* Also may need the clause_op's negator */
	clause_op_negator = get_negator(clause_op);

	/* Now search the opclasses */
	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	pred_tuple = &catlist->members[i]->tuple;
		Form_pg_amop pred_form = (Form_pg_amop) GETSTRUCT(pred_tuple);
		HeapTuple	clause_tuple;

		opclass_id = pred_form->amopclaid;

		/* must be btree */
		if (!opclass_is_btree(opclass_id))
			continue;
		/* predicate operator must be default within this opclass */
		if (pred_form->amopsubtype != InvalidOid)
			continue;

		/* Get the predicate operator's btree strategy number */
		pred_strategy = (StrategyNumber) pred_form->amopstrategy;
		Assert(pred_strategy >= 1 && pred_strategy <= 5);

		if (pred_op_negated)
		{
			/* Only consider negators that are = */
			if (pred_strategy != BTEqualStrategyNumber)
				continue;
			pred_strategy = BTNE;
		}

		/*
		 * From the same opclass, find a strategy number for the
		 * clause_op, if possible
		 */
		clause_tuple = SearchSysCache(AMOPOPID,
									  ObjectIdGetDatum(clause_op),
									  ObjectIdGetDatum(opclass_id),
									  0, 0);
		if (HeapTupleIsValid(clause_tuple))
		{
			Form_pg_amop clause_form = (Form_pg_amop) GETSTRUCT(clause_tuple);

			/* Get the restriction clause operator's strategy/subtype */
			clause_strategy = (StrategyNumber) clause_form->amopstrategy;
			Assert(clause_strategy >= 1 && clause_strategy <= 5);
			clause_subtype = clause_form->amopsubtype;
			ReleaseSysCache(clause_tuple);
		}
		else if (OidIsValid(clause_op_negator))
		{
			clause_tuple = SearchSysCache(AMOPOPID,
									 ObjectIdGetDatum(clause_op_negator),
										  ObjectIdGetDatum(opclass_id),
										  0, 0);
			if (HeapTupleIsValid(clause_tuple))
			{
				Form_pg_amop clause_form = (Form_pg_amop) GETSTRUCT(clause_tuple);

				/* Get the restriction clause operator's strategy/subtype */
				clause_strategy = (StrategyNumber) clause_form->amopstrategy;
				Assert(clause_strategy >= 1 && clause_strategy <= 5);
				clause_subtype = clause_form->amopsubtype;
				ReleaseSysCache(clause_tuple);

				/* Only consider negators that are = */
				if (clause_strategy != BTEqualStrategyNumber)
					continue;
				clause_strategy = BTNE;
			}
			else
				continue;
		}
		else
			continue;

		/*
		 * Look up the "test" strategy number in the implication table
		 */
		test_strategy = BT_implic_table[clause_strategy - 1][pred_strategy - 1];
		if (test_strategy == 0)
		{
			/* Can't determine implication using this interpretation */
			continue;
		}

		/*
		 * See if opclass has an operator for the test strategy and the
		 * clause datatype.
		 */
		if (test_strategy == BTNE)
		{
			test_op = get_opclass_member(opclass_id, clause_subtype,
										 BTEqualStrategyNumber);
			if (OidIsValid(test_op))
				test_op = get_negator(test_op);
		}
		else
		{
			test_op = get_opclass_member(opclass_id, clause_subtype,
										 test_strategy);
		}
		if (OidIsValid(test_op))
		{
			/*
			 * Last check: test_op must be immutable.
			 *
			 * Note that we require only the test_op to be immutable, not the
			 * original clause_op.	(pred_op must be immutable, else it
			 * would not be allowed in an index predicate.)  Essentially
			 * we are assuming that the opclass is consistent even if it
			 * contains operators that are merely stable.
			 *
			 * XXX the above reasoning doesn't hold anymore if this routine
			 * is used to prove things that are not index predicates ...
			 */
			if (op_volatile(test_op) == PROVOLATILE_IMMUTABLE)
			{
				found = true;
				break;
			}
		}
	}

	ReleaseSysCacheList(catlist);

	if (!found)
	{
		/* couldn't find a btree opclass to interpret the operators */
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
							  (Expr *) clause_const);

	/* Prepare it for execution */
	test_exprstate = ExecPrepareExpr(test_expr, estate);

	/* And execute it. */
	test_result = ExecEvalExprSwitchContext(test_exprstate,
										  GetPerTupleExprContext(estate),
											&isNull, NULL);

	/* Get back to outer memory context */
	MemoryContextSwitchTo(oldcontext);

	/* Release all the junk we just created */
	FreeExecutorState(estate);

	if (isNull)
	{
		/* Treat a null result as false ... but it's a tad fishy ... */
		elog(DEBUG2, "null predicate test result");
		return false;
	}
	return DatumGetBool(test_result);
}
