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
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/like_support.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/stratnum.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/selfuncs.h"
#include "utils/varlena.h"


typedef enum
{
	Pattern_Type_Like,
	Pattern_Type_Like_IC,
	Pattern_Type_Regex,
	Pattern_Type_Regex_IC,
	Pattern_Type_Prefix
} Pattern_Type;

typedef enum
{
	Pattern_Prefix_None, Pattern_Prefix_Partial, Pattern_Prefix_Exact
} Pattern_Prefix_Status;

static Node *like_regex_support(Node *rawreq, Pattern_Type ptype);
static List *match_pattern_prefix(Node *leftop,
								  Node *rightop,
								  Pattern_Type ptype,
								  Oid expr_coll,
								  Oid opfamily,
								  Oid indexcollation);
static double patternsel_common(PlannerInfo *root,
								Oid oprid,
								Oid opfuncid,
								List *args,
								int varRelid,
								Oid collation,
								Pattern_Type ptype,
								bool negate);
static Pattern_Prefix_Status pattern_fixed_prefix(Const *patt,
												  Pattern_Type ptype,
												  Oid collation,
												  Const **prefix,
												  Selectivity *rest_selec);
static Selectivity prefix_selectivity(PlannerInfo *root,
									  VariableStatData *vardata,
									  Oid eqopr, Oid ltopr, Oid geopr,
									  Oid collation,
									  Const *prefixcon);
static Selectivity like_selectivity(const char *patt, int pattlen,
									bool case_insensitive);
static Selectivity regex_selectivity(const char *patt, int pattlen,
									 bool case_insensitive,
									 int fixed_prefix_len);
static int	pattern_char_isalpha(char c, bool is_multibyte,
								 pg_locale_t locale, bool locale_is_c);
static Const *make_greater_string(const Const *str_const, FmgrInfo *ltproc,
								  Oid collation);
static Datum string_to_datum(const char *str, Oid datatype);
static Const *string_to_const(const char *str, Oid datatype);
static Const *string_to_bytea_const(const char *str, size_t str_len);


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

	if (IsA(rawreq, SupportRequestSelectivity))
	{
		/*
		 * Make a selectivity estimate for a function call, just as we'd do if
		 * the call was via the corresponding operator.
		 */
		SupportRequestSelectivity *req = (SupportRequestSelectivity *) rawreq;
		Selectivity s1;

		if (req->is_join)
		{
			/*
			 * For the moment we just punt.  If patternjoinsel is ever
			 * improved to do better, this should be made to call it.
			 */
			s1 = DEFAULT_MATCH_SEL;
		}
		else
		{
			/* Share code with operator restriction selectivity functions */
			s1 = patternsel_common(req->root,
								   InvalidOid,
								   req->funcid,
								   req->args,
								   req->varRelid,
								   req->inputcollid,
								   ptype,
								   false);
		}
		req->selectivity = s1;
		ret = (Node *) req;
	}
	else if (IsA(rawreq, SupportRequestIndexCondition))
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
	Oid			eqopr;
	Oid			ltopr;
	Oid			geopr;
	bool		collation_aware;
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
	 * Not supported if the expression collation is nondeterministic.  The
	 * optimized equality or prefix tests use bytewise comparisons, which is
	 * not consistent with nondeterministic collations.  The actual
	 * pattern-matching implementation functions will later error out that
	 * pattern-matching is not supported with nondeterministic collations. (We
	 * could also error out here, but by doing it later we get more precise
	 * error messages.)  (It should be possible to support at least
	 * Pattern_Prefix_Exact, but no point as long as the actual
	 * pattern-matching implementations don't support it.)
	 *
	 * expr_coll is not set for a non-collation-aware data type such as bytea.
	 */
	if (expr_coll && !get_collation_isdeterministic(expr_coll))
		return NIL;

	/*
	 * Try to extract a fixed prefix from the pattern.
	 */
	pstatus = pattern_fixed_prefix(patt, ptype, expr_coll,
								   &prefix, NULL);

	/* fail if no fixed prefix */
	if (pstatus == Pattern_Prefix_None)
		return NIL;

	/*
	 * Identify the operators we want to use, based on the type of the
	 * left-hand argument.  Usually these are just the type's regular
	 * comparison operators, but if we are considering one of the semi-legacy
	 * "pattern" opclasses, use the "pattern" operators instead.  Those are
	 * not collation-sensitive but always use C collation, as we want.  The
	 * selected operators also determine the needed type of the prefix
	 * constant.
	 */
	ldatatype = exprType(leftop);
	switch (ldatatype)
	{
		case TEXTOID:
			if (opfamily == TEXT_PATTERN_BTREE_FAM_OID ||
				opfamily == TEXT_SPGIST_FAM_OID)
			{
				eqopr = TextEqualOperator;
				ltopr = TextPatternLessOperator;
				geopr = TextPatternGreaterEqualOperator;
				collation_aware = false;
			}
			else
			{
				eqopr = TextEqualOperator;
				ltopr = TextLessOperator;
				geopr = TextGreaterEqualOperator;
				collation_aware = true;
			}
			rdatatype = TEXTOID;
			break;
		case NAMEOID:

			/*
			 * Note that here, we need the RHS type to be text, so that the
			 * comparison value isn't improperly truncated to NAMEDATALEN.
			 */
			eqopr = NameEqualTextOperator;
			ltopr = NameLessTextOperator;
			geopr = NameGreaterEqualTextOperator;
			collation_aware = true;
			rdatatype = TEXTOID;
			break;
		case BPCHAROID:
			if (opfamily == BPCHAR_PATTERN_BTREE_FAM_OID)
			{
				eqopr = BpcharEqualOperator;
				ltopr = BpcharPatternLessOperator;
				geopr = BpcharPatternGreaterEqualOperator;
				collation_aware = false;
			}
			else
			{
				eqopr = BpcharEqualOperator;
				ltopr = BpcharLessOperator;
				geopr = BpcharGreaterEqualOperator;
				collation_aware = true;
			}
			rdatatype = BPCHAROID;
			break;
		case BYTEAOID:
			eqopr = ByteaEqualOperator;
			ltopr = ByteaLessOperator;
			geopr = ByteaGreaterEqualOperator;
			collation_aware = false;
			rdatatype = BYTEAOID;
			break;
		default:
			/* Can't get here unless we're attached to the wrong operator */
			return NIL;
	}

	/*
	 * If necessary, verify that the index's collation behavior is compatible.
	 * For an exact-match case, we don't have to be picky.  Otherwise, insist
	 * that the index collation be "C".  Note that here we are looking at the
	 * index's collation, not the expression's collation -- this test is *not*
	 * dependent on the LIKE/regex operator's collation.
	 */
	if (collation_aware)
	{
		if (!(pstatus == Pattern_Prefix_Exact ||
			  lc_collate_is_c(indexcollation)))
			return NIL;
	}

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
	 *
	 * Here and below, check to see whether the desired operator is actually
	 * supported by the index opclass, and fail quietly if not.  This allows
	 * us to not be concerned with specific opclasses (except for the legacy
	 * "pattern" cases); any index that correctly implements the operators
	 * will work.
	 */
	if (pstatus == Pattern_Prefix_Exact)
	{
		if (!op_in_opfamily(eqopr, opfamily))
			return NIL;
		expr = make_opclause(eqopr, BOOLOID, false,
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
	if (!op_in_opfamily(geopr, opfamily))
		return NIL;
	expr = make_opclause(geopr, BOOLOID, false,
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
	if (!op_in_opfamily(ltopr, opfamily))
		return result;
	fmgr_info(get_opcode(ltopr), &ltproc);
	greaterstr = make_greater_string(prefix, &ltproc, indexcollation);
	if (greaterstr)
	{
		expr = make_opclause(ltopr, BOOLOID, false,
							 (Expr *) leftop, (Expr *) greaterstr,
							 InvalidOid, indexcollation);
		result = lappend(result, expr);
	}

	return result;
}


/*
 * patternsel_common - generic code for pattern-match restriction selectivity.
 *
 * To support using this from either the operator or function paths, caller
 * may pass either operator OID or underlying function OID; we look up the
 * latter from the former if needed.  (We could just have patternsel() call
 * get_opcode(), but the work would be wasted if we don't have a need to
 * compare a fixed prefix to the pg_statistic data.)
 *
 * Note that oprid and/or opfuncid should be for the positive-match operator
 * even when negate is true.
 */
static double
patternsel_common(PlannerInfo *root,
				  Oid oprid,
				  Oid opfuncid,
				  List *args,
				  int varRelid,
				  Oid collation,
				  Pattern_Type ptype,
				  bool negate)
{
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Datum		constval;
	Oid			consttype;
	Oid			vartype;
	Oid			rdatatype;
	Oid			eqopr;
	Oid			ltopr;
	Oid			geopr;
	Pattern_Prefix_Status pstatus;
	Const	   *patt;
	Const	   *prefix = NULL;
	Selectivity rest_selec = 0;
	double		nullfrac = 0.0;
	double		result;

	/*
	 * Initialize result to the appropriate default estimate depending on
	 * whether it's a match or not-match operator.
	 */
	if (negate)
		result = 1.0 - DEFAULT_MATCH_SEL;
	else
		result = DEFAULT_MATCH_SEL;

	/*
	 * If expression is not variable op constant, then punt and return the
	 * default estimate.
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		return result;
	if (!varonleft || !IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		return result;
	}

	/*
	 * If the constant is NULL, assume operator is strict and return zero, ie,
	 * operator will never return TRUE.  (It's zero even for a negator op.)
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		return 0.0;
	}
	constval = ((Const *) other)->constvalue;
	consttype = ((Const *) other)->consttype;

	/*
	 * The right-hand const is type text or bytea for all supported operators.
	 * We do not expect to see binary-compatible types here, since
	 * const-folding should have relabeled the const to exactly match the
	 * operator's declared type.
	 */
	if (consttype != TEXTOID && consttype != BYTEAOID)
	{
		ReleaseVariableStats(vardata);
		return result;
	}

	/*
	 * Similarly, the exposed type of the left-hand side should be one of
	 * those we know.  (Do not look at vardata.atttype, which might be
	 * something binary-compatible but different.)	We can use it to identify
	 * the comparison operators and the required type of the comparison
	 * constant, much as in match_pattern_prefix().
	 */
	vartype = vardata.vartype;

	switch (vartype)
	{
		case TEXTOID:
			eqopr = TextEqualOperator;
			ltopr = TextLessOperator;
			geopr = TextGreaterEqualOperator;
			rdatatype = TEXTOID;
			break;
		case NAMEOID:

			/*
			 * Note that here, we need the RHS type to be text, so that the
			 * comparison value isn't improperly truncated to NAMEDATALEN.
			 */
			eqopr = NameEqualTextOperator;
			ltopr = NameLessTextOperator;
			geopr = NameGreaterEqualTextOperator;
			rdatatype = TEXTOID;
			break;
		case BPCHAROID:
			eqopr = BpcharEqualOperator;
			ltopr = BpcharLessOperator;
			geopr = BpcharGreaterEqualOperator;
			rdatatype = BPCHAROID;
			break;
		case BYTEAOID:
			eqopr = ByteaEqualOperator;
			ltopr = ByteaLessOperator;
			geopr = ByteaGreaterEqualOperator;
			rdatatype = BYTEAOID;
			break;
		default:
			/* Can't get here unless we're attached to the wrong operator */
			ReleaseVariableStats(vardata);
			return result;
	}

	/*
	 * Grab the nullfrac for use below.
	 */
	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		nullfrac = stats->stanullfrac;
	}

	/*
	 * Pull out any fixed prefix implied by the pattern, and estimate the
	 * fractional selectivity of the remainder of the pattern.  Unlike many
	 * other selectivity estimators, we use the pattern operator's actual
	 * collation for this step.  This is not because we expect the collation
	 * to make a big difference in the selectivity estimate (it seldom would),
	 * but because we want to be sure we cache compiled regexps under the
	 * right cache key, so that they can be re-used at runtime.
	 */
	patt = (Const *) other;
	pstatus = pattern_fixed_prefix(patt, ptype, collation,
								   &prefix, &rest_selec);

	/*
	 * If necessary, coerce the prefix constant to the right type.  The only
	 * case where we need to do anything is when converting text to bpchar.
	 * Those two types are binary-compatible, so relabeling the Const node is
	 * sufficient.
	 */
	if (prefix && prefix->consttype != rdatatype)
	{
		Assert(prefix->consttype == TEXTOID &&
			   rdatatype == BPCHAROID);
		prefix->consttype = rdatatype;
	}

	if (pstatus == Pattern_Prefix_Exact)
	{
		/*
		 * Pattern specifies an exact match, so estimate as for '='
		 */
		result = var_eq_const(&vardata, eqopr, collation, prefix->constvalue,
							  false, true, false);
	}
	else
	{
		/*
		 * Not exact-match pattern.  If we have a sufficiently large
		 * histogram, estimate selectivity for the histogram part of the
		 * population by counting matches in the histogram.  If not, estimate
		 * selectivity of the fixed prefix and remainder of pattern
		 * separately, then combine the two to get an estimate of the
		 * selectivity for the part of the column population represented by
		 * the histogram.  (For small histograms, we combine these
		 * approaches.)
		 *
		 * We then add up data for any most-common-values values; these are
		 * not in the histogram population, and we can get exact answers for
		 * them by applying the pattern operator, so there's no reason to
		 * approximate.  (If the MCVs cover a significant part of the total
		 * population, this gives us a big leg up in accuracy.)
		 */
		Selectivity selec;
		int			hist_size;
		FmgrInfo	opproc;
		double		mcv_selec,
					sumcommon;

		/* Try to use the histogram entries to get selectivity */
		if (!OidIsValid(opfuncid))
			opfuncid = get_opcode(oprid);
		fmgr_info(opfuncid, &opproc);

		selec = histogram_selectivity(&vardata, &opproc, collation,
									  constval, true,
									  10, 1, &hist_size);

		/* If not at least 100 entries, use the heuristic method */
		if (hist_size < 100)
		{
			Selectivity heursel;
			Selectivity prefixsel;

			if (pstatus == Pattern_Prefix_Partial)
				prefixsel = prefix_selectivity(root, &vardata,
											   eqopr, ltopr, geopr,
											   collation,
											   prefix);
			else
				prefixsel = 1.0;
			heursel = prefixsel * rest_selec;

			if (selec < 0)		/* fewer than 10 histogram entries? */
				selec = heursel;
			else
			{
				/*
				 * For histogram sizes from 10 to 100, we combine the
				 * histogram and heuristic selectivities, putting increasingly
				 * more trust in the histogram for larger sizes.
				 */
				double		hist_weight = hist_size / 100.0;

				selec = selec * hist_weight + heursel * (1.0 - hist_weight);
			}
		}

		/* In any case, don't believe extremely small or large estimates. */
		if (selec < 0.0001)
			selec = 0.0001;
		else if (selec > 0.9999)
			selec = 0.9999;

		/*
		 * If we have most-common-values info, add up the fractions of the MCV
		 * entries that satisfy MCV OP PATTERN.  These fractions contribute
		 * directly to the result selectivity.  Also add up the total fraction
		 * represented by MCV entries.
		 */
		mcv_selec = mcv_selectivity(&vardata, &opproc, collation,
									constval, true,
									&sumcommon);

		/*
		 * Now merge the results from the MCV and histogram calculations,
		 * realizing that the histogram covers only the non-null values that
		 * are not listed in MCV.
		 */
		selec *= 1.0 - nullfrac - sumcommon;
		selec += mcv_selec;
		result = selec;
	}

	/* now adjust if we wanted not-match rather than match */
	if (negate)
		result = 1.0 - result - nullfrac;

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(result);

	if (prefix)
	{
		pfree(DatumGetPointer(prefix->constvalue));
		pfree(prefix);
	}

	ReleaseVariableStats(vardata);

	return result;
}

/*
 * Fix impedance mismatch between SQL-callable functions and patternsel_common
 */
static double
patternsel(PG_FUNCTION_ARGS, Pattern_Type ptype, bool negate)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	Oid			collation = PG_GET_COLLATION();

	/*
	 * If this is for a NOT LIKE or similar operator, get the corresponding
	 * positive-match operator and work with that.
	 */
	if (negate)
	{
		operator = get_negator(operator);
		if (!OidIsValid(operator))
			elog(ERROR, "patternsel called for operator without a negator");
	}

	return patternsel_common(root,
							 operator,
							 InvalidOid,
							 args,
							 varRelid,
							 collation,
							 ptype,
							 negate);
}

/*
 *		regexeqsel		- Selectivity of regular-expression pattern match.
 */
Datum
regexeqsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Regex, false));
}

/*
 *		icregexeqsel	- Selectivity of case-insensitive regex match.
 */
Datum
icregexeqsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Regex_IC, false));
}

/*
 *		likesel			- Selectivity of LIKE pattern match.
 */
Datum
likesel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Like, false));
}

/*
 *		prefixsel			- selectivity of prefix operator
 */
Datum
prefixsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Prefix, false));
}

/*
 *
 *		iclikesel			- Selectivity of ILIKE pattern match.
 */
Datum
iclikesel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Like_IC, false));
}

/*
 *		regexnesel		- Selectivity of regular-expression pattern non-match.
 */
Datum
regexnesel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Regex, true));
}

/*
 *		icregexnesel	- Selectivity of case-insensitive regex non-match.
 */
Datum
icregexnesel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Regex_IC, true));
}

/*
 *		nlikesel		- Selectivity of LIKE pattern non-match.
 */
Datum
nlikesel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Like, true));
}

/*
 *		icnlikesel		- Selectivity of ILIKE pattern non-match.
 */
Datum
icnlikesel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Like_IC, true));
}

/*
 * patternjoinsel		- Generic code for pattern-match join selectivity.
 */
static double
patternjoinsel(PG_FUNCTION_ARGS, Pattern_Type ptype, bool negate)
{
	/* For the moment we just punt. */
	return negate ? (1.0 - DEFAULT_MATCH_SEL) : DEFAULT_MATCH_SEL;
}

/*
 *		regexeqjoinsel	- Join selectivity of regular-expression pattern match.
 */
Datum
regexeqjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Regex, false));
}

/*
 *		icregexeqjoinsel	- Join selectivity of case-insensitive regex match.
 */
Datum
icregexeqjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Regex_IC, false));
}

/*
 *		likejoinsel			- Join selectivity of LIKE pattern match.
 */
Datum
likejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Like, false));
}

/*
 *		prefixjoinsel			- Join selectivity of prefix operator
 */
Datum
prefixjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Prefix, false));
}

/*
 *		iclikejoinsel			- Join selectivity of ILIKE pattern match.
 */
Datum
iclikejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Like_IC, false));
}

/*
 *		regexnejoinsel	- Join selectivity of regex non-match.
 */
Datum
regexnejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Regex, true));
}

/*
 *		icregexnejoinsel	- Join selectivity of case-insensitive regex non-match.
 */
Datum
icregexnejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Regex_IC, true));
}

/*
 *		nlikejoinsel		- Join selectivity of LIKE pattern non-match.
 */
Datum
nlikejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Like, true));
}

/*
 *		icnlikejoinsel		- Join selectivity of ILIKE pattern non-match.
 */
Datum
icnlikejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Like_IC, true));
}


/*-------------------------------------------------------------------------
 *
 * Pattern analysis functions
 *
 * These routines support analysis of LIKE and regular-expression patterns
 * by the planner/optimizer.  It's important that they agree with the
 * regular-expression code in backend/regex/ and the LIKE code in
 * backend/utils/adt/like.c.  Also, the computation of the fixed prefix
 * must be conservative: if we report a string longer than the true fixed
 * prefix, the query may produce actually wrong answers, rather than just
 * getting a bad selectivity estimate!
 *
 *-------------------------------------------------------------------------
 */

/*
 * Extract the fixed prefix, if any, for a pattern.
 *
 * *prefix is set to a palloc'd prefix string (in the form of a Const node),
 *	or to NULL if no fixed prefix exists for the pattern.
 * If rest_selec is not NULL, *rest_selec is set to an estimate of the
 *	selectivity of the remainder of the pattern (without any fixed prefix).
 * The prefix Const has the same type (TEXT or BYTEA) as the input pattern.
 *
 * The return value distinguishes no fixed prefix, a partial prefix,
 * or an exact-match-only pattern.
 */

static Pattern_Prefix_Status
like_fixed_prefix(Const *patt_const, bool case_insensitive, Oid collation,
				  Const **prefix_const, Selectivity *rest_selec)
{
	char	   *match;
	char	   *patt;
	int			pattlen;
	Oid			typeid = patt_const->consttype;
	int			pos,
				match_pos;
	bool		is_multibyte = (pg_database_encoding_max_length() > 1);
	pg_locale_t locale = 0;
	bool		locale_is_c = false;

	/* the right-hand const is type text or bytea */
	Assert(typeid == BYTEAOID || typeid == TEXTOID);

	if (case_insensitive)
	{
		if (typeid == BYTEAOID)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("case insensitive matching not supported on type bytea")));

		/* If case-insensitive, we need locale info */
		if (lc_ctype_is_c(collation))
			locale_is_c = true;
		else if (collation != DEFAULT_COLLATION_OID)
		{
			if (!OidIsValid(collation))
			{
				/*
				 * This typically means that the parser could not resolve a
				 * conflict of implicit collations, so report it that way.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_COLLATION),
						 errmsg("could not determine which collation to use for ILIKE"),
						 errhint("Use the COLLATE clause to set the collation explicitly.")));
			}
			locale = pg_newlocale_from_collation(collation);
		}
	}

	if (typeid != BYTEAOID)
	{
		patt = TextDatumGetCString(patt_const->constvalue);
		pattlen = strlen(patt);
	}
	else
	{
		bytea	   *bstr = DatumGetByteaPP(patt_const->constvalue);

		pattlen = VARSIZE_ANY_EXHDR(bstr);
		patt = (char *) palloc(pattlen);
		memcpy(patt, VARDATA_ANY(bstr), pattlen);
		Assert((Pointer) bstr == DatumGetPointer(patt_const->constvalue));
	}

	match = palloc(pattlen + 1);
	match_pos = 0;
	for (pos = 0; pos < pattlen; pos++)
	{
		/* % and _ are wildcard characters in LIKE */
		if (patt[pos] == '%' ||
			patt[pos] == '_')
			break;

		/* Backslash escapes the next character */
		if (patt[pos] == '\\')
		{
			pos++;
			if (pos >= pattlen)
				break;
		}

		/* Stop if case-varying character (it's sort of a wildcard) */
		if (case_insensitive &&
			pattern_char_isalpha(patt[pos], is_multibyte, locale, locale_is_c))
			break;

		match[match_pos++] = patt[pos];
	}

	match[match_pos] = '\0';

	if (typeid != BYTEAOID)
		*prefix_const = string_to_const(match, typeid);
	else
		*prefix_const = string_to_bytea_const(match, match_pos);

	if (rest_selec != NULL)
		*rest_selec = like_selectivity(&patt[pos], pattlen - pos,
									   case_insensitive);

	pfree(patt);
	pfree(match);

	/* in LIKE, an empty pattern is an exact match! */
	if (pos == pattlen)
		return Pattern_Prefix_Exact;	/* reached end of pattern, so exact */

	if (match_pos > 0)
		return Pattern_Prefix_Partial;

	return Pattern_Prefix_None;
}

static Pattern_Prefix_Status
regex_fixed_prefix(Const *patt_const, bool case_insensitive, Oid collation,
				   Const **prefix_const, Selectivity *rest_selec)
{
	Oid			typeid = patt_const->consttype;
	char	   *prefix;
	bool		exact;

	/*
	 * Should be unnecessary, there are no bytea regex operators defined. As
	 * such, it should be noted that the rest of this function has *not* been
	 * made safe for binary (possibly NULL containing) strings.
	 */
	if (typeid == BYTEAOID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("regular-expression matching not supported on type bytea")));

	/* Use the regexp machinery to extract the prefix, if any */
	prefix = regexp_fixed_prefix(DatumGetTextPP(patt_const->constvalue),
								 case_insensitive, collation,
								 &exact);

	if (prefix == NULL)
	{
		*prefix_const = NULL;

		if (rest_selec != NULL)
		{
			char	   *patt = TextDatumGetCString(patt_const->constvalue);

			*rest_selec = regex_selectivity(patt, strlen(patt),
											case_insensitive,
											0);
			pfree(patt);
		}

		return Pattern_Prefix_None;
	}

	*prefix_const = string_to_const(prefix, typeid);

	if (rest_selec != NULL)
	{
		if (exact)
		{
			/* Exact match, so there's no additional selectivity */
			*rest_selec = 1.0;
		}
		else
		{
			char	   *patt = TextDatumGetCString(patt_const->constvalue);

			*rest_selec = regex_selectivity(patt, strlen(patt),
											case_insensitive,
											strlen(prefix));
			pfree(patt);
		}
	}

	pfree(prefix);

	if (exact)
		return Pattern_Prefix_Exact;	/* pattern specifies exact match */
	else
		return Pattern_Prefix_Partial;
}

static Pattern_Prefix_Status
pattern_fixed_prefix(Const *patt, Pattern_Type ptype, Oid collation,
					 Const **prefix, Selectivity *rest_selec)
{
	Pattern_Prefix_Status result;

	switch (ptype)
	{
		case Pattern_Type_Like:
			result = like_fixed_prefix(patt, false, collation,
									   prefix, rest_selec);
			break;
		case Pattern_Type_Like_IC:
			result = like_fixed_prefix(patt, true, collation,
									   prefix, rest_selec);
			break;
		case Pattern_Type_Regex:
			result = regex_fixed_prefix(patt, false, collation,
										prefix, rest_selec);
			break;
		case Pattern_Type_Regex_IC:
			result = regex_fixed_prefix(patt, true, collation,
										prefix, rest_selec);
			break;
		case Pattern_Type_Prefix:
			/* Prefix type work is trivial.  */
			result = Pattern_Prefix_Partial;
			*rest_selec = 1.0;	/* all */
			*prefix = makeConst(patt->consttype,
								patt->consttypmod,
								patt->constcollid,
								patt->constlen,
								datumCopy(patt->constvalue,
										  patt->constbyval,
										  patt->constlen),
								patt->constisnull,
								patt->constbyval);
			break;
		default:
			elog(ERROR, "unrecognized ptype: %d", (int) ptype);
			result = Pattern_Prefix_None;	/* keep compiler quiet */
			break;
	}
	return result;
}

/*
 * Estimate the selectivity of a fixed prefix for a pattern match.
 *
 * A fixed prefix "foo" is estimated as the selectivity of the expression
 * "variable >= 'foo' AND variable < 'fop'".
 *
 * The selectivity estimate is with respect to the portion of the column
 * population represented by the histogram --- the caller must fold this
 * together with info about MCVs and NULLs.
 *
 * We use the given comparison operators and collation to do the estimation.
 * The given variable and Const must be of the associated datatype(s).
 *
 * XXX Note: we make use of the upper bound to estimate operator selectivity
 * even if the locale is such that we cannot rely on the upper-bound string.
 * The selectivity only needs to be approximately right anyway, so it seems
 * more useful to use the upper-bound code than not.
 */
static Selectivity
prefix_selectivity(PlannerInfo *root, VariableStatData *vardata,
				   Oid eqopr, Oid ltopr, Oid geopr,
				   Oid collation,
				   Const *prefixcon)
{
	Selectivity prefixsel;
	FmgrInfo	opproc;
	Const	   *greaterstrcon;
	Selectivity eq_sel;

	/* Estimate the selectivity of "x >= prefix" */
	fmgr_info(get_opcode(geopr), &opproc);

	prefixsel = ineq_histogram_selectivity(root, vardata,
										   geopr, &opproc, true, true,
										   collation,
										   prefixcon->constvalue,
										   prefixcon->consttype);

	if (prefixsel < 0.0)
	{
		/* No histogram is present ... return a suitable default estimate */
		return DEFAULT_MATCH_SEL;
	}

	/*
	 * If we can create a string larger than the prefix, say "x < greaterstr".
	 */
	fmgr_info(get_opcode(ltopr), &opproc);
	greaterstrcon = make_greater_string(prefixcon, &opproc, collation);
	if (greaterstrcon)
	{
		Selectivity topsel;

		topsel = ineq_histogram_selectivity(root, vardata,
											ltopr, &opproc, false, false,
											collation,
											greaterstrcon->constvalue,
											greaterstrcon->consttype);

		/* ineq_histogram_selectivity worked before, it shouldn't fail now */
		Assert(topsel >= 0.0);

		/*
		 * Merge the two selectivities in the same way as for a range query
		 * (see clauselist_selectivity()).  Note that we don't need to worry
		 * about double-exclusion of nulls, since ineq_histogram_selectivity
		 * doesn't count those anyway.
		 */
		prefixsel = topsel + prefixsel - 1.0;
	}

	/*
	 * If the prefix is long then the two bounding values might be too close
	 * together for the histogram to distinguish them usefully, resulting in a
	 * zero estimate (plus or minus roundoff error). To avoid returning a
	 * ridiculously small estimate, compute the estimated selectivity for
	 * "variable = 'foo'", and clamp to that. (Obviously, the resultant
	 * estimate should be at least that.)
	 *
	 * We apply this even if we couldn't make a greater string.  That case
	 * suggests that the prefix is near the maximum possible, and thus
	 * probably off the end of the histogram, and thus we probably got a very
	 * small estimate from the >= condition; so we still need to clamp.
	 */
	eq_sel = var_eq_const(vardata, eqopr, collation, prefixcon->constvalue,
						  false, true, false);

	prefixsel = Max(prefixsel, eq_sel);

	return prefixsel;
}


/*
 * Estimate the selectivity of a pattern of the specified type.
 * Note that any fixed prefix of the pattern will have been removed already,
 * so actually we may be looking at just a fragment of the pattern.
 *
 * For now, we use a very simplistic approach: fixed characters reduce the
 * selectivity a good deal, character ranges reduce it a little,
 * wildcards (such as % for LIKE or .* for regex) increase it.
 */

#define FIXED_CHAR_SEL	0.20	/* about 1/5 */
#define CHAR_RANGE_SEL	0.25
#define ANY_CHAR_SEL	0.9		/* not 1, since it won't match end-of-string */
#define FULL_WILDCARD_SEL 5.0
#define PARTIAL_WILDCARD_SEL 2.0

static Selectivity
like_selectivity(const char *patt, int pattlen, bool case_insensitive)
{
	Selectivity sel = 1.0;
	int			pos;

	/* Skip any leading wildcard; it's already factored into initial sel */
	for (pos = 0; pos < pattlen; pos++)
	{
		if (patt[pos] != '%' && patt[pos] != '_')
			break;
	}

	for (; pos < pattlen; pos++)
	{
		/* % and _ are wildcard characters in LIKE */
		if (patt[pos] == '%')
			sel *= FULL_WILDCARD_SEL;
		else if (patt[pos] == '_')
			sel *= ANY_CHAR_SEL;
		else if (patt[pos] == '\\')
		{
			/* Backslash quotes the next character */
			pos++;
			if (pos >= pattlen)
				break;
			sel *= FIXED_CHAR_SEL;
		}
		else
			sel *= FIXED_CHAR_SEL;
	}
	/* Could get sel > 1 if multiple wildcards */
	if (sel > 1.0)
		sel = 1.0;
	return sel;
}

static Selectivity
regex_selectivity_sub(const char *patt, int pattlen, bool case_insensitive)
{
	Selectivity sel = 1.0;
	int			paren_depth = 0;
	int			paren_pos = 0;	/* dummy init to keep compiler quiet */
	int			pos;

	for (pos = 0; pos < pattlen; pos++)
	{
		if (patt[pos] == '(')
		{
			if (paren_depth == 0)
				paren_pos = pos;	/* remember start of parenthesized item */
			paren_depth++;
		}
		else if (patt[pos] == ')' && paren_depth > 0)
		{
			paren_depth--;
			if (paren_depth == 0)
				sel *= regex_selectivity_sub(patt + (paren_pos + 1),
											 pos - (paren_pos + 1),
											 case_insensitive);
		}
		else if (patt[pos] == '|' && paren_depth == 0)
		{
			/*
			 * If unquoted | is present at paren level 0 in pattern, we have
			 * multiple alternatives; sum their probabilities.
			 */
			sel += regex_selectivity_sub(patt + (pos + 1),
										 pattlen - (pos + 1),
										 case_insensitive);
			break;				/* rest of pattern is now processed */
		}
		else if (patt[pos] == '[')
		{
			bool		negclass = false;

			if (patt[++pos] == '^')
			{
				negclass = true;
				pos++;
			}
			if (patt[pos] == ']')	/* ']' at start of class is not special */
				pos++;
			while (pos < pattlen && patt[pos] != ']')
				pos++;
			if (paren_depth == 0)
				sel *= (negclass ? (1.0 - CHAR_RANGE_SEL) : CHAR_RANGE_SEL);
		}
		else if (patt[pos] == '.')
		{
			if (paren_depth == 0)
				sel *= ANY_CHAR_SEL;
		}
		else if (patt[pos] == '*' ||
				 patt[pos] == '?' ||
				 patt[pos] == '+')
		{
			/* Ought to be smarter about quantifiers... */
			if (paren_depth == 0)
				sel *= PARTIAL_WILDCARD_SEL;
		}
		else if (patt[pos] == '{')
		{
			while (pos < pattlen && patt[pos] != '}')
				pos++;
			if (paren_depth == 0)
				sel *= PARTIAL_WILDCARD_SEL;
		}
		else if (patt[pos] == '\\')
		{
			/* backslash quotes the next character */
			pos++;
			if (pos >= pattlen)
				break;
			if (paren_depth == 0)
				sel *= FIXED_CHAR_SEL;
		}
		else
		{
			if (paren_depth == 0)
				sel *= FIXED_CHAR_SEL;
		}
	}
	/* Could get sel > 1 if multiple wildcards */
	if (sel > 1.0)
		sel = 1.0;
	return sel;
}

static Selectivity
regex_selectivity(const char *patt, int pattlen, bool case_insensitive,
				  int fixed_prefix_len)
{
	Selectivity sel;

	/* If patt doesn't end with $, consider it to have a trailing wildcard */
	if (pattlen > 0 && patt[pattlen - 1] == '$' &&
		(pattlen == 1 || patt[pattlen - 2] != '\\'))
	{
		/* has trailing $ */
		sel = regex_selectivity_sub(patt, pattlen - 1, case_insensitive);
	}
	else
	{
		/* no trailing $ */
		sel = regex_selectivity_sub(patt, pattlen, case_insensitive);
		sel *= FULL_WILDCARD_SEL;
	}

	/*
	 * If there's a fixed prefix, discount its selectivity.  We have to be
	 * careful here since a very long prefix could result in pow's result
	 * underflowing to zero (in which case "sel" probably has as well).
	 */
	if (fixed_prefix_len > 0)
	{
		double		prefixsel = pow(FIXED_CHAR_SEL, fixed_prefix_len);

		if (prefixsel > 0.0)
			sel /= prefixsel;
	}

	/* Make sure result stays in range */
	CLAMP_PROBABILITY(sel);
	return sel;
}

/*
 * Check whether char is a letter (and, hence, subject to case-folding)
 *
 * In multibyte character sets or with ICU, we can't use isalpha, and it does
 * not seem worth trying to convert to wchar_t to use iswalpha or u_isalpha.
 * Instead, just assume any non-ASCII char is potentially case-varying, and
 * hard-wire knowledge of which ASCII chars are letters.
 */
static int
pattern_char_isalpha(char c, bool is_multibyte,
					 pg_locale_t locale, bool locale_is_c)
{
	if (locale_is_c)
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
	else if (is_multibyte && IS_HIGHBIT_SET(c))
		return true;
	else if (locale && locale->provider == COLLPROVIDER_ICU)
		return IS_HIGHBIT_SET(c) ||
			(c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
#ifdef HAVE_LOCALE_T
	else if (locale && locale->provider == COLLPROVIDER_LIBC)
		return isalpha_l((unsigned char) c, locale->info.lt);
#endif
	else
		return isalpha((unsigned char) c);
}


/*
 * For bytea, the increment function need only increment the current byte
 * (there are no multibyte characters to worry about).
 */
static bool
byte_increment(unsigned char *ptr, int len)
{
	if (*ptr >= 255)
		return false;
	(*ptr)++;
	return true;
}

/*
 * Try to generate a string greater than the given string or any
 * string it is a prefix of.  If successful, return a palloc'd string
 * in the form of a Const node; else return NULL.
 *
 * The caller must provide the appropriate "less than" comparison function
 * for testing the strings, along with the collation to use.
 *
 * The key requirement here is that given a prefix string, say "foo",
 * we must be able to generate another string "fop" that is greater than
 * all strings "foobar" starting with "foo".  We can test that we have
 * generated a string greater than the prefix string, but in non-C collations
 * that is not a bulletproof guarantee that an extension of the string might
 * not sort after it; an example is that "foo " is less than "foo!", but it
 * is not clear that a "dictionary" sort ordering will consider "foo!" less
 * than "foo bar".  CAUTION: Therefore, this function should be used only for
 * estimation purposes when working in a non-C collation.
 *
 * To try to catch most cases where an extended string might otherwise sort
 * before the result value, we determine which of the strings "Z", "z", "y",
 * and "9" is seen as largest by the collation, and append that to the given
 * prefix before trying to find a string that compares as larger.
 *
 * To search for a greater string, we repeatedly "increment" the rightmost
 * character, using an encoding-specific character incrementer function.
 * When it's no longer possible to increment the last character, we truncate
 * off that character and start incrementing the next-to-rightmost.
 * For example, if "z" were the last character in the sort order, then we
 * could produce "foo" as a string greater than "fonz".
 *
 * This could be rather slow in the worst case, but in most cases we
 * won't have to try more than one or two strings before succeeding.
 *
 * Note that it's important for the character incrementer not to be too anal
 * about producing every possible character code, since in some cases the only
 * way to get a larger string is to increment a previous character position.
 * So we don't want to spend too much time trying every possible character
 * code at the last position.  A good rule of thumb is to be sure that we
 * don't try more than 256*K values for a K-byte character (and definitely
 * not 256^K, which is what an exhaustive search would approach).
 */
static Const *
make_greater_string(const Const *str_const, FmgrInfo *ltproc, Oid collation)
{
	Oid			datatype = str_const->consttype;
	char	   *workstr;
	int			len;
	Datum		cmpstr;
	char	   *cmptxt = NULL;
	mbcharacter_incrementer charinc;

	/*
	 * Get a modifiable copy of the prefix string in C-string format, and set
	 * up the string we will compare to as a Datum.  In C locale this can just
	 * be the given prefix string, otherwise we need to add a suffix.  Type
	 * BYTEA sorts bytewise so it never needs a suffix either.
	 */
	if (datatype == BYTEAOID)
	{
		bytea	   *bstr = DatumGetByteaPP(str_const->constvalue);

		len = VARSIZE_ANY_EXHDR(bstr);
		workstr = (char *) palloc(len);
		memcpy(workstr, VARDATA_ANY(bstr), len);
		Assert((Pointer) bstr == DatumGetPointer(str_const->constvalue));
		cmpstr = str_const->constvalue;
	}
	else
	{
		if (datatype == NAMEOID)
			workstr = DatumGetCString(DirectFunctionCall1(nameout,
														  str_const->constvalue));
		else
			workstr = TextDatumGetCString(str_const->constvalue);
		len = strlen(workstr);
		if (lc_collate_is_c(collation) || len == 0)
			cmpstr = str_const->constvalue;
		else
		{
			/* If first time through, determine the suffix to use */
			static char suffixchar = 0;
			static Oid	suffixcollation = 0;

			if (!suffixchar || suffixcollation != collation)
			{
				char	   *best;

				best = "Z";
				if (varstr_cmp(best, 1, "z", 1, collation) < 0)
					best = "z";
				if (varstr_cmp(best, 1, "y", 1, collation) < 0)
					best = "y";
				if (varstr_cmp(best, 1, "9", 1, collation) < 0)
					best = "9";
				suffixchar = *best;
				suffixcollation = collation;
			}

			/* And build the string to compare to */
			if (datatype == NAMEOID)
			{
				cmptxt = palloc(len + 2);
				memcpy(cmptxt, workstr, len);
				cmptxt[len] = suffixchar;
				cmptxt[len + 1] = '\0';
				cmpstr = PointerGetDatum(cmptxt);
			}
			else
			{
				cmptxt = palloc(VARHDRSZ + len + 1);
				SET_VARSIZE(cmptxt, VARHDRSZ + len + 1);
				memcpy(VARDATA(cmptxt), workstr, len);
				*(VARDATA(cmptxt) + len) = suffixchar;
				cmpstr = PointerGetDatum(cmptxt);
			}
		}
	}

	/* Select appropriate character-incrementer function */
	if (datatype == BYTEAOID)
		charinc = byte_increment;
	else
		charinc = pg_database_encoding_character_incrementer();

	/* And search ... */
	while (len > 0)
	{
		int			charlen;
		unsigned char *lastchar;

		/* Identify the last character --- for bytea, just the last byte */
		if (datatype == BYTEAOID)
			charlen = 1;
		else
			charlen = len - pg_mbcliplen(workstr, len, len - 1);
		lastchar = (unsigned char *) (workstr + len - charlen);

		/*
		 * Try to generate a larger string by incrementing the last character
		 * (for BYTEA, we treat each byte as a character).
		 *
		 * Note: the incrementer function is expected to return true if it's
		 * generated a valid-per-the-encoding new character, otherwise false.
		 * The contents of the character on false return are unspecified.
		 */
		while (charinc(lastchar, charlen))
		{
			Const	   *workstr_const;

			if (datatype == BYTEAOID)
				workstr_const = string_to_bytea_const(workstr, len);
			else
				workstr_const = string_to_const(workstr, datatype);

			if (DatumGetBool(FunctionCall2Coll(ltproc,
											   collation,
											   cmpstr,
											   workstr_const->constvalue)))
			{
				/* Successfully made a string larger than cmpstr */
				if (cmptxt)
					pfree(cmptxt);
				pfree(workstr);
				return workstr_const;
			}

			/* No good, release unusable value and try again */
			pfree(DatumGetPointer(workstr_const->constvalue));
			pfree(workstr_const);
		}

		/*
		 * No luck here, so truncate off the last character and try to
		 * increment the next one.
		 */
		len -= charlen;
		workstr[len] = '\0';
	}

	/* Failed... */
	if (cmptxt)
		pfree(cmptxt);
	pfree(workstr);

	return NULL;
}

/*
 * Generate a Datum of the appropriate type from a C string.
 * Note that all of the supported types are pass-by-ref, so the
 * returned value should be pfree'd if no longer needed.
 */
static Datum
string_to_datum(const char *str, Oid datatype)
{
	Assert(str != NULL);

	/*
	 * We cheat a little by assuming that CStringGetTextDatum() will do for
	 * bpchar and varchar constants too...
	 */
	if (datatype == NAMEOID)
		return DirectFunctionCall1(namein, CStringGetDatum(str));
	else if (datatype == BYTEAOID)
		return DirectFunctionCall1(byteain, CStringGetDatum(str));
	else
		return CStringGetTextDatum(str);
}

/*
 * Generate a Const node of the appropriate type from a C string.
 */
static Const *
string_to_const(const char *str, Oid datatype)
{
	Datum		conval = string_to_datum(str, datatype);
	Oid			collation;
	int			constlen;

	/*
	 * We only need to support a few datatypes here, so hard-wire properties
	 * instead of incurring the expense of catalog lookups.
	 */
	switch (datatype)
	{
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
			collation = DEFAULT_COLLATION_OID;
			constlen = -1;
			break;

		case NAMEOID:
			collation = C_COLLATION_OID;
			constlen = NAMEDATALEN;
			break;

		case BYTEAOID:
			collation = InvalidOid;
			constlen = -1;
			break;

		default:
			elog(ERROR, "unexpected datatype in string_to_const: %u",
				 datatype);
			return NULL;
	}

	return makeConst(datatype, -1, collation, constlen,
					 conval, false, false);
}

/*
 * Generate a Const node of bytea type from a binary C string and a length.
 */
static Const *
string_to_bytea_const(const char *str, size_t str_len)
{
	bytea	   *bstr = palloc(VARHDRSZ + str_len);
	Datum		conval;

	memcpy(VARDATA(bstr), str, str_len);
	SET_VARSIZE(bstr, VARHDRSZ + str_len);
	conval = PointerGetDatum(bstr);

	return makeConst(BYTEAOID, -1, InvalidOid, -1, conval, false, false);
}
