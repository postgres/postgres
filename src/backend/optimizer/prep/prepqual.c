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
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepqual.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_operator.h"
#include "common/hashfn.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/queryjumble.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_oper.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

int			or_to_any_transform_limit = 5;

static List *pull_ands(List *andlist);
static List *pull_ors(List *orlist);
static Expr *find_duplicate_ors(Expr *qual, bool is_check);
static Expr *process_duplicate_ors(List *orlist);
static List *transform_or_to_any(List *orlist);


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
					newopexpr->hashfuncid = InvalidOid;
					newopexpr->negfuncid = InvalidOid;
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
 * The key for grouping similar operator expressions in transform_or_to_any().
 */
typedef struct OrClauseGroupKey
{
	/* We need this to put this structure into list together with other nodes */
	NodeTag		type;

	/* The expression of the variable side of operator */
	Expr	   *expr;
	/* The operator of the operator expression */
	Oid			opno;
	/* The collation of the operator expression */
	Oid			inputcollid;
	/* The type of constant side of operator */
	Oid			consttype;
} OrClauseGroupKey;

/*
 * The group of similar operator expressions in transform_or_to_any().
 */
typedef struct OrClauseGroupEntry
{
	OrClauseGroupKey key;

	/* The list of constant sides of operators */
	List	   *consts;

	/*
	 * List of source expressions.  We need this for convenience in case we
	 * will give up on transformation.
	 */
	List	   *exprs;
} OrClauseGroupEntry;

/*
 * The hash function for OrClauseGroupKey.
 */
static uint32
orclause_hash(const void *data, Size keysize)
{
	OrClauseGroupKey *key = (OrClauseGroupKey *) data;
	uint64		exprHash;

	Assert(keysize == sizeof(OrClauseGroupKey));
	Assert(IsA(data, Invalid));

	(void) JumbleExpr(key->expr, &exprHash);

	return hash_combine((uint32) exprHash,
						hash_combine((uint32) key->opno,
									 hash_combine((uint32) key->consttype,
												  (uint32) key->inputcollid)));
}

/*
 * The copy function for OrClauseGroupKey.
 */
static void *
orclause_keycopy(void *dest, const void *src, Size keysize)
{
	OrClauseGroupKey *src_key = (OrClauseGroupKey *) src;
	OrClauseGroupKey *dst_key = (OrClauseGroupKey *) dest;

	Assert(sizeof(OrClauseGroupKey) == keysize);
	Assert(IsA(src, Invalid));

	dst_key->type = T_Invalid;
	dst_key->expr = src_key->expr;
	dst_key->opno = src_key->opno;
	dst_key->consttype = src_key->consttype;
	dst_key->inputcollid = src_key->inputcollid;

	return dst_key;
}

/*
 * The equality function for OrClauseGroupKey.
 */
static int
orclause_match(const void *data1, const void *data2, Size keysize)
{
	OrClauseGroupKey *key1 = (OrClauseGroupKey *) data1;
	OrClauseGroupKey *key2 = (OrClauseGroupKey *) data2;

	Assert(sizeof(OrClauseGroupKey) == keysize);
	Assert(IsA(key1, Invalid));
	Assert(IsA(key2, Invalid));

	if (key1->opno == key2->opno &&
		key1->consttype == key2->consttype &&
		key1->inputcollid == key2->inputcollid &&
		equal(key1->expr, key2->expr))
		return 0;

	return 1;
}

/*
 * transform_or_to_any -
 *	  Discover the args of an OR expression and try to group similar OR
 *	  expressions to SAOP expressions.
 *
 * This transformation groups two-sided equality expression.  One side of
 * such an expression must be a plain constant or constant expression.  The
 * other side must be a variable expression without volatile functions.
 * To group quals, opno, inputcollid of variable expression, and type of
 * constant expression must be equal too.
 *
 * The grouping technique is based on the equivalence of variable sides of
 * the expression: using exprId and equal() routine, it groups constant sides
 * of similar clauses into an array.  After the grouping procedure, each
 * couple ('variable expression' and 'constant array') forms a new SAOP
 * operation, which is added to the args list of the returning expression.
 */
static List *
transform_or_to_any(List *orlist)
{
	List	   *neworlist = NIL;
	List	   *entries = NIL;
	ListCell   *lc;
	HASHCTL		info;
	HTAB	   *or_group_htab = NULL;
	int			len_ors = list_length(orlist);
	OrClauseGroupEntry *entry = NULL;

	Assert(or_to_any_transform_limit >= 0 &&
		   len_ors >= or_to_any_transform_limit);

	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(OrClauseGroupKey);
	info.entrysize = sizeof(OrClauseGroupEntry);
	info.hash = orclause_hash;
	info.keycopy = orclause_keycopy;
	info.match = orclause_match;
	or_group_htab = hash_create("OR Groups",
								len_ors,
								&info,
								HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_KEYCOPY);

	foreach(lc, orlist)
	{
		Node	   *orqual = lfirst(lc);
		Node	   *const_expr;
		Node	   *nconst_expr;
		OrClauseGroupKey hashkey;
		bool		found;
		Oid			opno;
		Oid			consttype;
		Node	   *leftop,
				   *rightop;

		if (!IsA(orqual, OpExpr))
		{
			entries = lappend(entries, orqual);
			continue;
		}

		opno = ((OpExpr *) orqual)->opno;
		if (get_op_rettype(opno) != BOOLOID)
		{
			/* Only operator returning boolean suits OR -> ANY transformation */
			entries = lappend(entries, orqual);
			continue;
		}

		/*
		 * Detect the constant side of the clause. Recall non-constant
		 * expression can be made not only with Vars, but also with Params,
		 * which is not bonded with any relation. Thus, we detect the const
		 * side - if another side is constant too, the orqual couldn't be an
		 * OpExpr.  Get pointers to constant and expression sides of the qual.
		 */
		leftop = get_leftop(orqual);
		if (IsA(leftop, RelabelType))
			leftop = (Node *) ((RelabelType *) leftop)->arg;
		rightop = get_rightop(orqual);
		if (IsA(rightop, RelabelType))
			rightop = (Node *) ((RelabelType *) rightop)->arg;

		if (IsA(leftop, Const))
		{
			opno = get_commutator(opno);

			if (!OidIsValid(opno))
			{
				/* commutator doesn't exist, we can't reverse the order */
				entries = lappend(entries, orqual);
				continue;
			}

			nconst_expr = get_rightop(orqual);
			const_expr = get_leftop(orqual);
		}
		else if (IsA(rightop, Const))
		{
			const_expr = get_rightop(orqual);
			nconst_expr = get_leftop(orqual);
		}
		else
		{
			entries = lappend(entries, orqual);
			continue;
		}

		/*
		 * Forbid transformation for composite types, records, and volatile
		 * expressions.
		 */
		consttype = exprType(const_expr);
		if (type_is_rowtype(exprType(const_expr)) ||
			type_is_rowtype(consttype) ||
			contain_volatile_functions((Node *) nconst_expr))
		{
			entries = lappend(entries, orqual);
			continue;
		}

		/*
		 * At this point we definitely have a transformable clause. Classify
		 * it and add into specific group of clauses, or create new group.
		 */
		hashkey.type = T_Invalid;
		hashkey.expr = (Expr *) nconst_expr;
		hashkey.opno = opno;
		hashkey.consttype = consttype;
		hashkey.inputcollid = exprCollation(const_expr);
		entry = hash_search(or_group_htab, &hashkey, HASH_ENTER, &found);

		if (unlikely(found))
		{
			entry->consts = lappend(entry->consts, const_expr);
			entry->exprs = lappend(entry->exprs, orqual);
		}
		else
		{
			entry->consts = list_make1(const_expr);
			entry->exprs = list_make1(orqual);

			/*
			 * Add the entry to the list.  It is needed exclusively to manage
			 * the problem with the order of transformed clauses in explain.
			 * Hash value can depend on the platform and version.  Hence,
			 * sequental scan of the hash table would prone to change the
			 * order of clauses in lists and, as a result, break regression
			 * tests accidentially.
			 */
			entries = lappend(entries, entry);
		}
	}

	/* Let's convert each group of clauses to an ANY expression. */

	/*
	 * Go through the list of groups and convert each, where number of consts
	 * more than 1. trivial groups move to OR-list again
	 */
	foreach(lc, entries)
	{
		Oid			scalar_type;
		Oid			array_type;

		if (!IsA(lfirst(lc), Invalid))
		{
			neworlist = lappend(neworlist, lfirst(lc));
			continue;
		}

		entry = (OrClauseGroupEntry *) lfirst(lc);

		Assert(list_length(entry->consts) > 0);
		Assert(list_length(entry->exprs) == list_length(entry->consts));

		if (list_length(entry->consts) == 1)
		{
			/*
			 * Only one element returns origin expression into the BoolExpr
			 * args list unchanged.
			 */
			list_free(entry->consts);
			neworlist = list_concat(neworlist, entry->exprs);
			continue;
		}

		/*
		 * Do the transformation.
		 */
		scalar_type = entry->key.consttype;
		array_type = OidIsValid(scalar_type) ? get_array_type(scalar_type) :
			InvalidOid;

		if (OidIsValid(array_type))
		{
			/*
			 * OK: coerce all the right-hand non-Var inputs to the common type
			 * and build an ArrayExpr for them.
			 */
			List	   *aexprs = NIL;
			ArrayExpr  *newa = NULL;
			ScalarArrayOpExpr *saopexpr = NULL;
			HeapTuple	opertup;
			Form_pg_operator operform;
			List	   *namelist = NIL;

			foreach(lc, entry->consts)
			{
				Node	   *node = (Node *) lfirst(lc);

				node = coerce_to_common_type(NULL, node, scalar_type,
											 "OR ANY Transformation");
				aexprs = lappend(aexprs, node);
			}

			newa = makeNode(ArrayExpr);
			/* array_collid will be set by parse_collate.c */
			newa->element_typeid = scalar_type;
			newa->array_typeid = array_type;
			newa->multidims = false;
			newa->elements = aexprs;
			newa->location = -1;

			/*
			 * Try to cast this expression to Const. Due to current strict
			 * transformation rules it should be done [almost] every time.
			 */
			newa = (ArrayExpr *) eval_const_expressions(NULL, (Node *) newa);

			opertup = SearchSysCache1(OPEROID,
									  ObjectIdGetDatum(entry->key.opno));
			if (!HeapTupleIsValid(opertup))
				elog(ERROR, "cache lookup failed for operator %u",
					 entry->key.opno);

			operform = (Form_pg_operator) GETSTRUCT(opertup);
			if (!OperatorIsVisible(entry->key.opno))
				namelist = lappend(namelist, makeString(get_namespace_name(operform->oprnamespace)));

			namelist = lappend(namelist, makeString(pstrdup(NameStr(operform->oprname))));
			ReleaseSysCache(opertup);

			saopexpr =
				(ScalarArrayOpExpr *)
				make_scalar_array_op(NULL,
									 namelist,
									 true,
									 (Node *) entry->key.expr,
									 (Node *) newa,
									 -1);
			saopexpr->inputcollid = entry->key.inputcollid;

			neworlist = lappend(neworlist, (void *) saopexpr);
		}
		else
		{
			/*
			 * If the const node's (right side of operator expression) type
			 * don't have “true” array type, then we cannnot do the
			 * transformation. We simply concatenate the expression node.
			 */
			list_free(entry->consts);
			neworlist = list_concat(neworlist, entry->exprs);
		}
	}
	hash_destroy(or_group_htab);
	list_free(entries);

	/* One more trick: assemble correct clause */
	return neworlist;
}

/*
 * canonicalize_qual
 *	  Convert a qualification expression to the most useful form.
 *
 * This is primarily intended to be used on top-level WHERE (or JOIN/ON)
 * clauses.  It can also be used on top-level CHECK constraints, for which
 * pass is_check = true.  DO NOT call it on any expression that is not known
 * to be one or the other, as it might apply inappropriate simplifications.
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
canonicalize_qual(Expr *qual, bool is_check)
{
	Expr	   *newqual;

	/* Quick exit for empty qual */
	if (qual == NULL)
		return NULL;

	/* This should not be invoked on quals in implicit-AND format */
	Assert(!IsA(qual, List));

	/*
	 * Pull up redundant subclauses in OR-of-AND trees.  We do this only
	 * within the top-level AND/OR structure; there's no point in looking
	 * deeper.  Also remove any NULL constants in the top-level structure.
	 */
	newqual = find_duplicate_ors(qual, is_check);

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

		if (is_andclause(subexpr))
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

		if (is_orclause(subexpr))
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
 * structure, eg in a WHERE clause, "x OR NULL::boolean" is reduced to "x".
 * In general that would change the result, so eval_const_expressions can't
 * do it; but at top level of WHERE, we don't need to distinguish between
 * FALSE and NULL results, so it's valid to treat NULL::boolean the same
 * as FALSE and then simplify AND/OR accordingly.  Conversely, in a top-level
 * CHECK constraint, we may treat a NULL the same as TRUE.
 *
 * Returns the modified qualification.  AND/OR flatness is preserved.
 */
static Expr *
find_duplicate_ors(Expr *qual, bool is_check)
{
	if (is_orclause(qual))
	{
		List	   *orlist = NIL;
		ListCell   *temp;

		/* Recurse */
		foreach(temp, ((BoolExpr *) qual)->args)
		{
			Expr	   *arg = (Expr *) lfirst(temp);

			arg = find_duplicate_ors(arg, is_check);

			/* Get rid of any constant inputs */
			if (arg && IsA(arg, Const))
			{
				Const	   *carg = (Const *) arg;

				if (is_check)
				{
					/* Within OR in CHECK, drop constant FALSE */
					if (!carg->constisnull && !DatumGetBool(carg->constvalue))
						continue;
					/* Constant TRUE or NULL, so OR reduces to TRUE */
					return (Expr *) makeBoolConst(true, false);
				}
				else
				{
					/* Within OR in WHERE, drop constant FALSE or NULL */
					if (carg->constisnull || !DatumGetBool(carg->constvalue))
						continue;
					/* Constant TRUE, so OR reduces to TRUE */
					return arg;
				}
			}

			orlist = lappend(orlist, arg);
		}

		/* Flatten any ORs pulled up to just below here */
		orlist = pull_ors(orlist);

		/* Now we can look for duplicate ORs */
		return process_duplicate_ors(orlist);
	}
	else if (is_andclause(qual))
	{
		List	   *andlist = NIL;
		ListCell   *temp;

		/* Recurse */
		foreach(temp, ((BoolExpr *) qual)->args)
		{
			Expr	   *arg = (Expr *) lfirst(temp);

			arg = find_duplicate_ors(arg, is_check);

			/* Get rid of any constant inputs */
			if (arg && IsA(arg, Const))
			{
				Const	   *carg = (Const *) arg;

				if (is_check)
				{
					/* Within AND in CHECK, drop constant TRUE or NULL */
					if (carg->constisnull || DatumGetBool(carg->constvalue))
						continue;
					/* Constant FALSE, so AND reduces to FALSE */
					return arg;
				}
				else
				{
					/* Within AND in WHERE, drop constant TRUE */
					if (!carg->constisnull && DatumGetBool(carg->constvalue))
						continue;
					/* Constant FALSE or NULL, so AND reduces to FALSE */
					return (Expr *) makeBoolConst(false, false);
				}
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

		if (is_andclause(clause))
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

			if (is_andclause(clause))
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
	 * If no winners, we can't do OR-to-ANY transformation.
	 */
	if (winners == NIL)
	{
		/*
		 * Make an attempt to group similar OR clauses into SAOP if the list
		 * is lengthy enough.
		 */
		if (or_to_any_transform_limit >= 0 &&
			list_length(orlist) >= or_to_any_transform_limit)
			orlist = transform_or_to_any(orlist);

		/* Transformation could group all OR clauses to a single SAOP */
		return (list_length(orlist) == 1) ?
			(Expr *) linitial(orlist) : make_orclause(orlist);
	}

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

		if (is_andclause(clause))
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

	/* Make an attempt to group similar OR clauses into ANY operation */
	if (or_to_any_transform_limit >= 0 &&
		list_length(neworlist) >= or_to_any_transform_limit)
		neworlist = transform_or_to_any(neworlist);

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
