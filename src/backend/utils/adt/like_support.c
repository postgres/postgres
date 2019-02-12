/*-------------------------------------------------------------------------
 *
 * like_support.c
 *	  Planner support functions for LIKE, regex, and related operators.
 *
 * These routines handle special optimization of operators that can be
 * used with index scans even though they are not known to the executor's
 * indexscan machinery.  The key idea is that these operators allow us
 * to derive approximate indexscan qual clauses, such that any tuples
 * that pass the operator clause itself must also satisfy the simpler
 * indexscan condition(s).  Then we can use the indexscan machinery
 * to avoid scanning as much of the table as we'd otherwise have to,
 * while applying the original operator as a qpqual condition to ensure
 * we deliver only the tuples we want.  (In essence, we're using a regular
 * index as if it were a lossy index.)
 *
 * An example of what we're doing is
 *			textfield LIKE 'abc%def'
 * from which we can generate the indexscanable conditions
 *			textfield >= 'abc' AND textfield < 'abd'
 * which allow efficient scanning of an index on textfield.
 * (In reality, character set and collation issues make the transformation
 * from LIKE to indexscan limits rather harder than one might think ...
 * but that's the basic idea.)
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/like_support.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/selfuncs.h"


static Node *like_regex_support(Node *rawreq, Pattern_Type ptype);
static List *match_pattern_prefix(Node *leftop,
					 Node *rightop,
					 Pattern_Type ptype,
					 Oid expr_coll,
					 Oid opfamily,
					 Oid indexcollation);


/*
 * Planner support functions for LIKE, regex, and related operators
 */
Datum
textlike_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(like_regex_support(rawreq, Pattern_Type_Like));
}

Datum
texticlike_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(like_regex_support(rawreq, Pattern_Type_Like_IC));
}

Datum
textregexeq_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(like_regex_support(rawreq, Pattern_Type_Regex));
}

Datum
texticregexeq_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(like_regex_support(rawreq, Pattern_Type_Regex_IC));
}

/* Common code for the above */
static Node *
like_regex_support(Node *rawreq, Pattern_Type ptype)
{
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestIndexCondition))
	{
		/* Try to convert operator/function call to index conditions */
		SupportRequestIndexCondition *req = (SupportRequestIndexCondition *) rawreq;

		/*
		 * Currently we have no "reverse" match operators with the pattern on
		 * the left, so we only need consider cases with the indexkey on the
		 * left.
		 */
		if (req->indexarg != 0)
			return NULL;

		if (is_opclause(req->node))
		{
			OpExpr	   *clause = (OpExpr *) req->node;

			Assert(list_length(clause->args) == 2);
			ret = (Node *)
				match_pattern_prefix((Node *) linitial(clause->args),
									 (Node *) lsecond(clause->args),
									 ptype,
									 clause->inputcollid,
									 req->opfamily,
									 req->indexcollation);
		}
		else if (is_funcclause(req->node))	/* be paranoid */
		{
			FuncExpr   *clause = (FuncExpr *) req->node;

			Assert(list_length(clause->args) == 2);
			ret = (Node *)
				match_pattern_prefix((Node *) linitial(clause->args),
									 (Node *) lsecond(clause->args),
									 ptype,
									 clause->inputcollid,
									 req->opfamily,
									 req->indexcollation);
		}
	}

	return ret;
}

/*
 * match_pattern_prefix
 *	  Try to generate an indexqual for a LIKE or regex operator.
 */
static List *
match_pattern_prefix(Node *leftop,
					 Node *rightop,
					 Pattern_Type ptype,
					 Oid expr_coll,
					 Oid opfamily,
					 Oid indexcollation)
{
	List	   *result;
	Const	   *patt;
	Const	   *prefix;
	Pattern_Prefix_Status pstatus;
	Oid			ldatatype;
	Oid			rdatatype;
	Oid			oproid;
	Expr	   *expr;
	FmgrInfo	ltproc;
	Const	   *greaterstr;

	/*
	 * Can't do anything with a non-constant or NULL pattern argument.
	 *
	 * Note that since we restrict ourselves to cases with a hard constant on
	 * the RHS, it's a-fortiori a pseudoconstant, and we don't need to worry
	 * about verifying that.
	 */
	if (!IsA(rightop, Const) ||
		((Const *) rightop)->constisnull)
		return NIL;
	patt = (Const *) rightop;

	/*
	 * Try to extract a fixed prefix from the pattern.
	 */
	pstatus = pattern_fixed_prefix(patt, ptype, expr_coll,
								   &prefix, NULL);

	/* fail if no fixed prefix */
	if (pstatus == Pattern_Prefix_None)
		return NIL;

	/*
	 * Must also check that index's opfamily supports the operators we will
	 * want to apply.  (A hash index, for example, will not support ">=".)
	 * Currently, only btree and spgist support the operators we need.
	 *
	 * Note: actually, in the Pattern_Prefix_Exact case, we only need "=" so a
	 * hash index would work.  Currently it doesn't seem worth checking for
	 * that, however.
	 *
	 * We insist on the opfamily being one of the specific ones we expect,
	 * else we'd do the wrong thing if someone were to make a reverse-sort
	 * opfamily with the same operators.
	 *
	 * The non-pattern opclasses will not sort the way we need in most non-C
	 * locales.  We can use such an index anyway for an exact match (simple
	 * equality), but not for prefix-match cases.  Note that here we are
	 * looking at the index's collation, not the expression's collation --
	 * this test is *not* dependent on the LIKE/regex operator's collation.
	 *
	 * While we're at it, identify the type the comparison constant(s) should
	 * have, based on the opfamily.
	 */
	switch (opfamily)
	{
		case TEXT_BTREE_FAM_OID:
			if (!(pstatus == Pattern_Prefix_Exact ||
				  lc_collate_is_c(indexcollation)))
				return NIL;
			rdatatype = TEXTOID;
			break;

		case TEXT_PATTERN_BTREE_FAM_OID:
		case TEXT_SPGIST_FAM_OID:
			rdatatype = TEXTOID;
			break;

		case BPCHAR_BTREE_FAM_OID:
			if (!(pstatus == Pattern_Prefix_Exact ||
				  lc_collate_is_c(indexcollation)))
				return NIL;
			rdatatype = BPCHAROID;
			break;

		case BPCHAR_PATTERN_BTREE_FAM_OID:
			rdatatype = BPCHAROID;
			break;

		case BYTEA_BTREE_FAM_OID:
			rdatatype = BYTEAOID;
			break;

		default:
			return NIL;
	}

	/* OK, prepare to create the indexqual(s) */
	ldatatype = exprType(leftop);

	/*
	 * If necessary, coerce the prefix constant to the right type.  The given
	 * prefix constant is either text or bytea type, therefore the only case
	 * where we need to do anything is when converting text to bpchar.  Those
	 * two types are binary-compatible, so relabeling the Const node is
	 * sufficient.
	 */
	if (prefix->consttype != rdatatype)
	{
		Assert(prefix->consttype == TEXTOID &&
			   rdatatype == BPCHAROID);
		prefix->consttype = rdatatype;
	}

	/*
	 * If we found an exact-match pattern, generate an "=" indexqual.
	 */
	if (pstatus == Pattern_Prefix_Exact)
	{
		oproid = get_opfamily_member(opfamily, ldatatype, rdatatype,
									 BTEqualStrategyNumber);
		if (oproid == InvalidOid)
			elog(ERROR, "no = operator for opfamily %u", opfamily);
		expr = make_opclause(oproid, BOOLOID, false,
							 (Expr *) leftop, (Expr *) prefix,
							 InvalidOid, indexcollation);
		result = list_make1(expr);
		return result;
	}

	/*
	 * Otherwise, we have a nonempty required prefix of the values.
	 *
	 * We can always say "x >= prefix".
	 */
	oproid = get_opfamily_member(opfamily, ldatatype, rdatatype,
								 BTGreaterEqualStrategyNumber);
	if (oproid == InvalidOid)
		elog(ERROR, "no >= operator for opfamily %u", opfamily);
	expr = make_opclause(oproid, BOOLOID, false,
						 (Expr *) leftop, (Expr *) prefix,
						 InvalidOid, indexcollation);
	result = list_make1(expr);

	/*-------
	 * If we can create a string larger than the prefix, we can say
	 * "x < greaterstr".  NB: we rely on make_greater_string() to generate
	 * a guaranteed-greater string, not just a probably-greater string.
	 * In general this is only guaranteed in C locale, so we'd better be
	 * using a C-locale index collation.
	 *-------
	 */
	oproid = get_opfamily_member(opfamily, ldatatype, rdatatype,
								 BTLessStrategyNumber);
	if (oproid == InvalidOid)
		elog(ERROR, "no < operator for opfamily %u", opfamily);
	fmgr_info(get_opcode(oproid), &ltproc);
	greaterstr = make_greater_string(prefix, &ltproc, indexcollation);
	if (greaterstr)
	{
		expr = make_opclause(oproid, BOOLOID, false,
							 (Expr *) leftop, (Expr *) greaterstr,
							 InvalidOid, indexcollation);
		result = lappend(result, expr);
	}

	return result;
}
