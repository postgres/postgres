/*-------------------------------------------------------------------------
 *
 * selfuncs.c
 *	  Selectivity functions and index cost estimation functions for
 *	  standard operators and index access methods.
 *
 *	  Selectivity routines are registered in the pg_operator catalog
 *	  in the "oprrest" and "oprjoin" attributes.
 *
 *	  Index cost functions are registered in the pg_am catalog
 *	  in the "amcostestimate" attribute.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/selfuncs.c
 *
 *-------------------------------------------------------------------------
 */

/*----------
 * Operator selectivity estimation functions are called to estimate the
 * selectivity of WHERE clauses whose top-level operator is their operator.
 * We divide the problem into two cases:
 *		Restriction clause estimation: the clause involves vars of just
 *			one relation.
 *		Join clause estimation: the clause involves vars of multiple rels.
 * Join selectivity estimation is far more difficult and usually less accurate
 * than restriction estimation.
 *
 * When dealing with the inner scan of a nestloop join, we consider the
 * join's joinclauses as restriction clauses for the inner relation, and
 * treat vars of the outer relation as parameters (a/k/a constants of unknown
 * values).  So, restriction estimators need to be able to accept an argument
 * telling which relation is to be treated as the variable.
 *
 * The call convention for a restriction estimator (oprrest function) is
 *
 *		Selectivity oprrest (PlannerInfo *root,
 *							 Oid operator,
 *							 List *args,
 *							 int varRelid);
 *
 * root: general information about the query (rtable and RelOptInfo lists
 * are particularly important for the estimator).
 * operator: OID of the specific operator in question.
 * args: argument list from the operator clause.
 * varRelid: if not zero, the relid (rtable index) of the relation to
 * be treated as the variable relation.  May be zero if the args list
 * is known to contain vars of only one relation.
 *
 * This is represented at the SQL level (in pg_proc) as
 *
 *		float8 oprrest (internal, oid, internal, int4);
 *
 * The result is a selectivity, that is, a fraction (0 to 1) of the rows
 * of the relation that are expected to produce a TRUE result for the
 * given operator.
 *
 * The call convention for a join estimator (oprjoin function) is similar
 * except that varRelid is not needed, and instead join information is
 * supplied:
 *
 *		Selectivity oprjoin (PlannerInfo *root,
 *							 Oid operator,
 *							 List *args,
 *							 JoinType jointype,
 *							 SpecialJoinInfo *sjinfo);
 *
 *		float8 oprjoin (internal, oid, internal, int2, internal);
 *
 * (Before Postgres 8.4, join estimators had only the first four of these
 * parameters.	That signature is still allowed, but deprecated.)  The
 * relationship between jointype and sjinfo is explained in the comments for
 * clause_selectivity() --- the short version is that jointype is usually
 * best ignored in favor of examining sjinfo.
 *
 * Join selectivity for regular inner and outer joins is defined as the
 * fraction (0 to 1) of the cross product of the relations that is expected
 * to produce a TRUE result for the given operator.  For both semi and anti
 * joins, however, the selectivity is defined as the fraction of the left-hand
 * side relation's rows that are expected to have a match (ie, at least one
 * row with a TRUE result) in the right-hand side.
 *
 * For both oprrest and oprjoin functions, the operator's input collation OID
 * (if any) is passed using the standard fmgr mechanism, so that the estimator
 * function can fetch it with PG_GET_COLLATION().  Note, however, that all
 * statistics in pg_statistic are currently built using the database's default
 * collation.  Thus, in most cases where we are looking at statistics, we
 * should ignore the actual operator collation and use DEFAULT_COLLATION_OID.
 * We expect that the error induced by doing this is usually not large enough
 * to justify complicating matters.
 *----------
 */

#include "postgres.h"

#include <ctype.h>
#include <math.h>

#include "access/gin.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/index.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/predtest.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/nabstime.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tqual.h"
#include "utils/typcache.h"


/* Hooks for plugins to get control when we ask for stats */
get_relation_stats_hook_type get_relation_stats_hook = NULL;
get_index_stats_hook_type get_index_stats_hook = NULL;

static double var_eq_const(VariableStatData *vardata, Oid operator,
			 Datum constval, bool constisnull,
			 bool varonleft);
static double var_eq_non_const(VariableStatData *vardata, Oid operator,
				 Node *other,
				 bool varonleft);
static double ineq_histogram_selectivity(PlannerInfo *root,
						   VariableStatData *vardata,
						   FmgrInfo *opproc, bool isgt,
						   Datum constval, Oid consttype);
static double eqjoinsel_inner(Oid operator,
				VariableStatData *vardata1, VariableStatData *vardata2);
static double eqjoinsel_semi(Oid operator,
			   VariableStatData *vardata1, VariableStatData *vardata2,
			   RelOptInfo *inner_rel);
static bool convert_to_scalar(Datum value, Oid valuetypid, double *scaledvalue,
				  Datum lobound, Datum hibound, Oid boundstypid,
				  double *scaledlobound, double *scaledhibound);
static double convert_numeric_to_scalar(Datum value, Oid typid);
static void convert_string_to_scalar(char *value,
						 double *scaledvalue,
						 char *lobound,
						 double *scaledlobound,
						 char *hibound,
						 double *scaledhibound);
static void convert_bytea_to_scalar(Datum value,
						double *scaledvalue,
						Datum lobound,
						double *scaledlobound,
						Datum hibound,
						double *scaledhibound);
static double convert_one_string_to_scalar(char *value,
							 int rangelo, int rangehi);
static double convert_one_bytea_to_scalar(unsigned char *value, int valuelen,
							int rangelo, int rangehi);
static char *convert_string_datum(Datum value, Oid typid);
static double convert_timevalue_to_scalar(Datum value, Oid typid);
static void examine_simple_variable(PlannerInfo *root, Var *var,
						VariableStatData *vardata);
static bool get_variable_range(PlannerInfo *root, VariableStatData *vardata,
				   Oid sortop, Datum *min, Datum *max);
static bool get_actual_variable_range(PlannerInfo *root,
						  VariableStatData *vardata,
						  Oid sortop,
						  Datum *min, Datum *max);
static RelOptInfo *find_join_input_rel(PlannerInfo *root, Relids relids);
static Selectivity prefix_selectivity(PlannerInfo *root,
				   VariableStatData *vardata,
				   Oid vartype, Oid opfamily, Const *prefixcon);
static Selectivity like_selectivity(const char *patt, int pattlen,
				 bool case_insensitive);
static Selectivity regex_selectivity(const char *patt, int pattlen,
				  bool case_insensitive,
				  int fixed_prefix_len);
static Datum string_to_datum(const char *str, Oid datatype);
static Const *string_to_const(const char *str, Oid datatype);
static Const *string_to_bytea_const(const char *str, size_t str_len);
static List *add_predicate_to_quals(IndexOptInfo *index, List *indexQuals);


/*
 *		eqsel			- Selectivity of "=" for any data types.
 *
 * Note: this routine is also used to estimate selectivity for some
 * operators that are not "=" but have comparable selectivity behavior,
 * such as "~=" (geometric approximate-match).	Even for "=", we must
 * keep in mind that the left and right datatypes may differ.
 */
Datum
eqsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	double		selec;

	/*
	 * If expression is not variable = something or something = variable, then
	 * punt and return a default estimate.
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(DEFAULT_EQ_SEL);

	/*
	 * We can do a lot better if the something is a constant.  (Note: the
	 * Const might result from estimation rather than being a simple constant
	 * in the query.)
	 */
	if (IsA(other, Const))
		selec = var_eq_const(&vardata, operator,
							 ((Const *) other)->constvalue,
							 ((Const *) other)->constisnull,
							 varonleft);
	else
		selec = var_eq_non_const(&vardata, operator, other,
								 varonleft);

	ReleaseVariableStats(vardata);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 * var_eq_const --- eqsel for var = const case
 *
 * This is split out so that some other estimation functions can use it.
 */
static double
var_eq_const(VariableStatData *vardata, Oid operator,
			 Datum constval, bool constisnull,
			 bool varonleft)
{
	double		selec;
	bool		isdefault;

	/*
	 * If the constant is NULL, assume operator is strict and return zero, ie,
	 * operator will never return TRUE.
	 */
	if (constisnull)
		return 0.0;

	/*
	 * If we matched the var to a unique index or DISTINCT clause, assume
	 * there is exactly one match regardless of anything else.	(This is
	 * slightly bogus, since the index or clause's equality operator might be
	 * different from ours, but it's much more likely to be right than
	 * ignoring the information.)
	 */
	if (vardata->isunique && vardata->rel && vardata->rel->tuples >= 1.0)
		return 1.0 / vardata->rel->tuples;

	if (HeapTupleIsValid(vardata->statsTuple))
	{
		Form_pg_statistic stats;
		Datum	   *values;
		int			nvalues;
		float4	   *numbers;
		int			nnumbers;
		bool		match = false;
		int			i;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);

		/*
		 * Is the constant "=" to any of the column's most common values?
		 * (Although the given operator may not really be "=", we will assume
		 * that seeing whether it returns TRUE is an appropriate test.	If you
		 * don't like this, maybe you shouldn't be using eqsel for your
		 * operator...)
		 */
		if (get_attstatsslot(vardata->statsTuple,
							 vardata->atttype, vardata->atttypmod,
							 STATISTIC_KIND_MCV, InvalidOid,
							 NULL,
							 &values, &nvalues,
							 &numbers, &nnumbers))
		{
			FmgrInfo	eqproc;

			fmgr_info(get_opcode(operator), &eqproc);

			for (i = 0; i < nvalues; i++)
			{
				/* be careful to apply operator right way 'round */
				if (varonleft)
					match = DatumGetBool(FunctionCall2Coll(&eqproc,
													   DEFAULT_COLLATION_OID,
														   values[i],
														   constval));
				else
					match = DatumGetBool(FunctionCall2Coll(&eqproc,
													   DEFAULT_COLLATION_OID,
														   constval,
														   values[i]));
				if (match)
					break;
			}
		}
		else
		{
			/* no most-common-value info available */
			values = NULL;
			numbers = NULL;
			i = nvalues = nnumbers = 0;
		}

		if (match)
		{
			/*
			 * Constant is "=" to this common value.  We know selectivity
			 * exactly (or as exactly as ANALYZE could calculate it, anyway).
			 */
			selec = numbers[i];
		}
		else
		{
			/*
			 * Comparison is against a constant that is neither NULL nor any
			 * of the common values.  Its selectivity cannot be more than
			 * this:
			 */
			double		sumcommon = 0.0;
			double		otherdistinct;

			for (i = 0; i < nnumbers; i++)
				sumcommon += numbers[i];
			selec = 1.0 - sumcommon - stats->stanullfrac;
			CLAMP_PROBABILITY(selec);

			/*
			 * and in fact it's probably a good deal less. We approximate that
			 * all the not-common values share this remaining fraction
			 * equally, so we divide by the number of other distinct values.
			 */
			otherdistinct = get_variable_numdistinct(vardata, &isdefault) - nnumbers;
			if (otherdistinct > 1)
				selec /= otherdistinct;

			/*
			 * Another cross-check: selectivity shouldn't be estimated as more
			 * than the least common "most common value".
			 */
			if (nnumbers > 0 && selec > numbers[nnumbers - 1])
				selec = numbers[nnumbers - 1];
		}

		free_attstatsslot(vardata->atttype, values, nvalues,
						  numbers, nnumbers);
	}
	else
	{
		/*
		 * No ANALYZE stats available, so make a guess using estimated number
		 * of distinct values and assuming they are equally common. (The guess
		 * is unlikely to be very good, but we do know a few special cases.)
		 */
		selec = 1.0 / get_variable_numdistinct(vardata, &isdefault);
	}

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 * var_eq_non_const --- eqsel for var = something-other-than-const case
 */
static double
var_eq_non_const(VariableStatData *vardata, Oid operator,
				 Node *other,
				 bool varonleft)
{
	double		selec;
	bool		isdefault;

	/*
	 * If we matched the var to a unique index or DISTINCT clause, assume
	 * there is exactly one match regardless of anything else.	(This is
	 * slightly bogus, since the index or clause's equality operator might be
	 * different from ours, but it's much more likely to be right than
	 * ignoring the information.)
	 */
	if (vardata->isunique && vardata->rel && vardata->rel->tuples >= 1.0)
		return 1.0 / vardata->rel->tuples;

	if (HeapTupleIsValid(vardata->statsTuple))
	{
		Form_pg_statistic stats;
		double		ndistinct;
		float4	   *numbers;
		int			nnumbers;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);

		/*
		 * Search is for a value that we do not know a priori, but we will
		 * assume it is not NULL.  Estimate the selectivity as non-null
		 * fraction divided by number of distinct values, so that we get a
		 * result averaged over all possible values whether common or
		 * uncommon.  (Essentially, we are assuming that the not-yet-known
		 * comparison value is equally likely to be any of the possible
		 * values, regardless of their frequency in the table.	Is that a good
		 * idea?)
		 */
		selec = 1.0 - stats->stanullfrac;
		ndistinct = get_variable_numdistinct(vardata, &isdefault);
		if (ndistinct > 1)
			selec /= ndistinct;

		/*
		 * Cross-check: selectivity should never be estimated as more than the
		 * most common value's.
		 */
		if (get_attstatsslot(vardata->statsTuple,
							 vardata->atttype, vardata->atttypmod,
							 STATISTIC_KIND_MCV, InvalidOid,
							 NULL,
							 NULL, NULL,
							 &numbers, &nnumbers))
		{
			if (nnumbers > 0 && selec > numbers[0])
				selec = numbers[0];
			free_attstatsslot(vardata->atttype, NULL, 0, numbers, nnumbers);
		}
	}
	else
	{
		/*
		 * No ANALYZE stats available, so make a guess using estimated number
		 * of distinct values and assuming they are equally common. (The guess
		 * is unlikely to be very good, but we do know a few special cases.)
		 */
		selec = 1.0 / get_variable_numdistinct(vardata, &isdefault);
	}

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 *		neqsel			- Selectivity of "!=" for any data types.
 *
 * This routine is also used for some operators that are not "!="
 * but have comparable selectivity behavior.  See above comments
 * for eqsel().
 */
Datum
neqsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	Oid			eqop;
	float8		result;

	/*
	 * We want 1 - eqsel() where the equality operator is the one associated
	 * with this != operator, that is, its negator.
	 */
	eqop = get_negator(operator);
	if (eqop)
	{
		result = DatumGetFloat8(DirectFunctionCall4(eqsel,
													PointerGetDatum(root),
													ObjectIdGetDatum(eqop),
													PointerGetDatum(args),
													Int32GetDatum(varRelid)));
	}
	else
	{
		/* Use default selectivity (should we raise an error instead?) */
		result = DEFAULT_EQ_SEL;
	}
	result = 1.0 - result;
	PG_RETURN_FLOAT8(result);
}

/*
 *	scalarineqsel		- Selectivity of "<", "<=", ">", ">=" for scalars.
 *
 * This is the guts of both scalarltsel and scalargtsel.  The caller has
 * commuted the clause, if necessary, so that we can treat the variable as
 * being on the left.  The caller must also make sure that the other side
 * of the clause is a non-null Const, and dissect same into a value and
 * datatype.
 *
 * This routine works for any datatype (or pair of datatypes) known to
 * convert_to_scalar().  If it is applied to some other datatype,
 * it will return a default estimate.
 */
static double
scalarineqsel(PlannerInfo *root, Oid operator, bool isgt,
			  VariableStatData *vardata, Datum constval, Oid consttype)
{
	Form_pg_statistic stats;
	FmgrInfo	opproc;
	double		mcv_selec,
				hist_selec,
				sumcommon;
	double		selec;

	if (!HeapTupleIsValid(vardata->statsTuple))
	{
		/* no stats available, so default result */
		return DEFAULT_INEQ_SEL;
	}
	stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);

	fmgr_info(get_opcode(operator), &opproc);

	/*
	 * If we have most-common-values info, add up the fractions of the MCV
	 * entries that satisfy MCV OP CONST.  These fractions contribute directly
	 * to the result selectivity.  Also add up the total fraction represented
	 * by MCV entries.
	 */
	mcv_selec = mcv_selectivity(vardata, &opproc, constval, true,
								&sumcommon);

	/*
	 * If there is a histogram, determine which bin the constant falls in, and
	 * compute the resulting contribution to selectivity.
	 */
	hist_selec = ineq_histogram_selectivity(root, vardata, &opproc, isgt,
											constval, consttype);

	/*
	 * Now merge the results from the MCV and histogram calculations,
	 * realizing that the histogram covers only the non-null values that are
	 * not listed in MCV.
	 */
	selec = 1.0 - stats->stanullfrac - sumcommon;

	if (hist_selec >= 0.0)
		selec *= hist_selec;
	else
	{
		/*
		 * If no histogram but there are values not accounted for by MCV,
		 * arbitrarily assume half of them will match.
		 */
		selec *= 0.5;
	}

	selec += mcv_selec;

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 *	mcv_selectivity			- Examine the MCV list for selectivity estimates
 *
 * Determine the fraction of the variable's MCV population that satisfies
 * the predicate (VAR OP CONST), or (CONST OP VAR) if !varonleft.  Also
 * compute the fraction of the total column population represented by the MCV
 * list.  This code will work for any boolean-returning predicate operator.
 *
 * The function result is the MCV selectivity, and the fraction of the
 * total population is returned into *sumcommonp.  Zeroes are returned
 * if there is no MCV list.
 */
double
mcv_selectivity(VariableStatData *vardata, FmgrInfo *opproc,
				Datum constval, bool varonleft,
				double *sumcommonp)
{
	double		mcv_selec,
				sumcommon;
	Datum	   *values;
	int			nvalues;
	float4	   *numbers;
	int			nnumbers;
	int			i;

	mcv_selec = 0.0;
	sumcommon = 0.0;

	if (HeapTupleIsValid(vardata->statsTuple) &&
		get_attstatsslot(vardata->statsTuple,
						 vardata->atttype, vardata->atttypmod,
						 STATISTIC_KIND_MCV, InvalidOid,
						 NULL,
						 &values, &nvalues,
						 &numbers, &nnumbers))
	{
		for (i = 0; i < nvalues; i++)
		{
			if (varonleft ?
				DatumGetBool(FunctionCall2Coll(opproc,
											   DEFAULT_COLLATION_OID,
											   values[i],
											   constval)) :
				DatumGetBool(FunctionCall2Coll(opproc,
											   DEFAULT_COLLATION_OID,
											   constval,
											   values[i])))
				mcv_selec += numbers[i];
			sumcommon += numbers[i];
		}
		free_attstatsslot(vardata->atttype, values, nvalues,
						  numbers, nnumbers);
	}

	*sumcommonp = sumcommon;
	return mcv_selec;
}

/*
 *	histogram_selectivity	- Examine the histogram for selectivity estimates
 *
 * Determine the fraction of the variable's histogram entries that satisfy
 * the predicate (VAR OP CONST), or (CONST OP VAR) if !varonleft.
 *
 * This code will work for any boolean-returning predicate operator, whether
 * or not it has anything to do with the histogram sort operator.  We are
 * essentially using the histogram just as a representative sample.  However,
 * small histograms are unlikely to be all that representative, so the caller
 * should be prepared to fall back on some other estimation approach when the
 * histogram is missing or very small.	It may also be prudent to combine this
 * approach with another one when the histogram is small.
 *
 * If the actual histogram size is not at least min_hist_size, we won't bother
 * to do the calculation at all.  Also, if the n_skip parameter is > 0, we
 * ignore the first and last n_skip histogram elements, on the grounds that
 * they are outliers and hence not very representative.  Typical values for
 * these parameters are 10 and 1.
 *
 * The function result is the selectivity, or -1 if there is no histogram
 * or it's smaller than min_hist_size.
 *
 * The output parameter *hist_size receives the actual histogram size,
 * or zero if no histogram.  Callers may use this number to decide how
 * much faith to put in the function result.
 *
 * Note that the result disregards both the most-common-values (if any) and
 * null entries.  The caller is expected to combine this result with
 * statistics for those portions of the column population.	It may also be
 * prudent to clamp the result range, ie, disbelieve exact 0 or 1 outputs.
 */
double
histogram_selectivity(VariableStatData *vardata, FmgrInfo *opproc,
					  Datum constval, bool varonleft,
					  int min_hist_size, int n_skip,
					  int *hist_size)
{
	double		result;
	Datum	   *values;
	int			nvalues;

	/* check sanity of parameters */
	Assert(n_skip >= 0);
	Assert(min_hist_size > 2 * n_skip);

	if (HeapTupleIsValid(vardata->statsTuple) &&
		get_attstatsslot(vardata->statsTuple,
						 vardata->atttype, vardata->atttypmod,
						 STATISTIC_KIND_HISTOGRAM, InvalidOid,
						 NULL,
						 &values, &nvalues,
						 NULL, NULL))
	{
		*hist_size = nvalues;
		if (nvalues >= min_hist_size)
		{
			int			nmatch = 0;
			int			i;

			for (i = n_skip; i < nvalues - n_skip; i++)
			{
				if (varonleft ?
					DatumGetBool(FunctionCall2Coll(opproc,
												   DEFAULT_COLLATION_OID,
												   values[i],
												   constval)) :
					DatumGetBool(FunctionCall2Coll(opproc,
												   DEFAULT_COLLATION_OID,
												   constval,
												   values[i])))
					nmatch++;
			}
			result = ((double) nmatch) / ((double) (nvalues - 2 * n_skip));
		}
		else
			result = -1;
		free_attstatsslot(vardata->atttype, values, nvalues, NULL, 0);
	}
	else
	{
		*hist_size = 0;
		result = -1;
	}

	return result;
}

/*
 *	ineq_histogram_selectivity	- Examine the histogram for scalarineqsel
 *
 * Determine the fraction of the variable's histogram population that
 * satisfies the inequality condition, ie, VAR < CONST or VAR > CONST.
 *
 * Returns -1 if there is no histogram (valid results will always be >= 0).
 *
 * Note that the result disregards both the most-common-values (if any) and
 * null entries.  The caller is expected to combine this result with
 * statistics for those portions of the column population.
 */
static double
ineq_histogram_selectivity(PlannerInfo *root,
						   VariableStatData *vardata,
						   FmgrInfo *opproc, bool isgt,
						   Datum constval, Oid consttype)
{
	double		hist_selec;
	Oid			hist_op;
	Datum	   *values;
	int			nvalues;

	hist_selec = -1.0;

	/*
	 * Someday, ANALYZE might store more than one histogram per rel/att,
	 * corresponding to more than one possible sort ordering defined for the
	 * column type.  However, to make that work we will need to figure out
	 * which staop to search for --- it's not necessarily the one we have at
	 * hand!  (For example, we might have a '<=' operator rather than the '<'
	 * operator that will appear in staop.)  For now, assume that whatever
	 * appears in pg_statistic is sorted the same way our operator sorts, or
	 * the reverse way if isgt is TRUE.
	 */
	if (HeapTupleIsValid(vardata->statsTuple) &&
		get_attstatsslot(vardata->statsTuple,
						 vardata->atttype, vardata->atttypmod,
						 STATISTIC_KIND_HISTOGRAM, InvalidOid,
						 &hist_op,
						 &values, &nvalues,
						 NULL, NULL))
	{
		if (nvalues > 1)
		{
			/*
			 * Use binary search to find proper location, ie, the first slot
			 * at which the comparison fails.  (If the given operator isn't
			 * actually sort-compatible with the histogram, you'll get garbage
			 * results ... but probably not any more garbage-y than you would
			 * from the old linear search.)
			 *
			 * If the binary search accesses the first or last histogram
			 * entry, we try to replace that endpoint with the true column min
			 * or max as found by get_actual_variable_range().	This
			 * ameliorates misestimates when the min or max is moving as a
			 * result of changes since the last ANALYZE.  Note that this could
			 * result in effectively including MCVs into the histogram that
			 * weren't there before, but we don't try to correct for that.
			 */
			double		histfrac;
			int			lobound = 0;	/* first possible slot to search */
			int			hibound = nvalues;		/* last+1 slot to search */
			bool		have_end = false;

			/*
			 * If there are only two histogram entries, we'll want up-to-date
			 * values for both.  (If there are more than two, we need at most
			 * one of them to be updated, so we deal with that within the
			 * loop.)
			 */
			if (nvalues == 2)
				have_end = get_actual_variable_range(root,
													 vardata,
													 hist_op,
													 &values[0],
													 &values[1]);

			while (lobound < hibound)
			{
				int			probe = (lobound + hibound) / 2;
				bool		ltcmp;

				/*
				 * If we find ourselves about to compare to the first or last
				 * histogram entry, first try to replace it with the actual
				 * current min or max (unless we already did so above).
				 */
				if (probe == 0 && nvalues > 2)
					have_end = get_actual_variable_range(root,
														 vardata,
														 hist_op,
														 &values[0],
														 NULL);
				else if (probe == nvalues - 1 && nvalues > 2)
					have_end = get_actual_variable_range(root,
														 vardata,
														 hist_op,
														 NULL,
														 &values[probe]);

				ltcmp = DatumGetBool(FunctionCall2Coll(opproc,
													   DEFAULT_COLLATION_OID,
													   values[probe],
													   constval));
				if (isgt)
					ltcmp = !ltcmp;
				if (ltcmp)
					lobound = probe + 1;
				else
					hibound = probe;
			}

			if (lobound <= 0)
			{
				/* Constant is below lower histogram boundary. */
				histfrac = 0.0;
			}
			else if (lobound >= nvalues)
			{
				/* Constant is above upper histogram boundary. */
				histfrac = 1.0;
			}
			else
			{
				int			i = lobound;
				double		val,
							high,
							low;
				double		binfrac;

				/*
				 * We have values[i-1] <= constant <= values[i].
				 *
				 * Convert the constant and the two nearest bin boundary
				 * values to a uniform comparison scale, and do a linear
				 * interpolation within this bin.
				 */
				if (convert_to_scalar(constval, consttype, &val,
									  values[i - 1], values[i],
									  vardata->vartype,
									  &low, &high))
				{
					if (high <= low)
					{
						/* cope if bin boundaries appear identical */
						binfrac = 0.5;
					}
					else if (val <= low)
						binfrac = 0.0;
					else if (val >= high)
						binfrac = 1.0;
					else
					{
						binfrac = (val - low) / (high - low);

						/*
						 * Watch out for the possibility that we got a NaN or
						 * Infinity from the division.	This can happen
						 * despite the previous checks, if for example "low"
						 * is -Infinity.
						 */
						if (isnan(binfrac) ||
							binfrac < 0.0 || binfrac > 1.0)
							binfrac = 0.5;
					}
				}
				else
				{
					/*
					 * Ideally we'd produce an error here, on the grounds that
					 * the given operator shouldn't have scalarXXsel
					 * registered as its selectivity func unless we can deal
					 * with its operand types.	But currently, all manner of
					 * stuff is invoking scalarXXsel, so give a default
					 * estimate until that can be fixed.
					 */
					binfrac = 0.5;
				}

				/*
				 * Now, compute the overall selectivity across the values
				 * represented by the histogram.  We have i-1 full bins and
				 * binfrac partial bin below the constant.
				 */
				histfrac = (double) (i - 1) + binfrac;
				histfrac /= (double) (nvalues - 1);
			}

			/*
			 * Now histfrac = fraction of histogram entries below the
			 * constant.
			 *
			 * Account for "<" vs ">"
			 */
			hist_selec = isgt ? (1.0 - histfrac) : histfrac;

			/*
			 * The histogram boundaries are only approximate to begin with,
			 * and may well be out of date anyway.	Therefore, don't believe
			 * extremely small or large selectivity estimates --- unless we
			 * got actual current endpoint values from the table.
			 */
			if (have_end)
				CLAMP_PROBABILITY(hist_selec);
			else
			{
				if (hist_selec < 0.0001)
					hist_selec = 0.0001;
				else if (hist_selec > 0.9999)
					hist_selec = 0.9999;
			}
		}

		free_attstatsslot(vardata->atttype, values, nvalues, NULL, 0);
	}

	return hist_selec;
}

/*
 *		scalarltsel		- Selectivity of "<" (also "<=") for scalars.
 */
Datum
scalarltsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Datum		constval;
	Oid			consttype;
	bool		isgt;
	double		selec;

	/*
	 * If expression is not variable op something or something op variable,
	 * then punt and return a default estimate.
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);

	/*
	 * Can't do anything useful if the something is not a constant, either.
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
	}

	/*
	 * If the constant is NULL, assume operator is strict and return zero, ie,
	 * operator will never return TRUE.
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}
	constval = ((Const *) other)->constvalue;
	consttype = ((Const *) other)->consttype;

	/*
	 * Force the var to be on the left to simplify logic in scalarineqsel.
	 */
	if (varonleft)
	{
		/* we have var < other */
		isgt = false;
	}
	else
	{
		/* we have other < var, commute to make var > other */
		operator = get_commutator(operator);
		if (!operator)
		{
			/* Use default selectivity (should we raise an error instead?) */
			ReleaseVariableStats(vardata);
			PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
		}
		isgt = true;
	}

	selec = scalarineqsel(root, operator, isgt, &vardata, constval, consttype);

	ReleaseVariableStats(vardata);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 *		scalargtsel		- Selectivity of ">" (also ">=") for integers.
 */
Datum
scalargtsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Datum		constval;
	Oid			consttype;
	bool		isgt;
	double		selec;

	/*
	 * If expression is not variable op something or something op variable,
	 * then punt and return a default estimate.
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);

	/*
	 * Can't do anything useful if the something is not a constant, either.
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
	}

	/*
	 * If the constant is NULL, assume operator is strict and return zero, ie,
	 * operator will never return TRUE.
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}
	constval = ((Const *) other)->constvalue;
	consttype = ((Const *) other)->consttype;

	/*
	 * Force the var to be on the left to simplify logic in scalarineqsel.
	 */
	if (varonleft)
	{
		/* we have var > other */
		isgt = true;
	}
	else
	{
		/* we have other > var, commute to make var < other */
		operator = get_commutator(operator);
		if (!operator)
		{
			/* Use default selectivity (should we raise an error instead?) */
			ReleaseVariableStats(vardata);
			PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
		}
		isgt = false;
	}

	selec = scalarineqsel(root, operator, isgt, &vardata, constval, consttype);

	ReleaseVariableStats(vardata);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 * patternsel			- Generic code for pattern-match selectivity.
 */
static double
patternsel(PG_FUNCTION_ARGS, Pattern_Type ptype, bool negate)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	Oid			collation = PG_GET_COLLATION();
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Datum		constval;
	Oid			consttype;
	Oid			vartype;
	Oid			opfamily;
	Pattern_Prefix_Status pstatus;
	Const	   *patt;
	Const	   *prefix = NULL;
	Selectivity rest_selec = 0;
	double		result;

	/*
	 * If this is for a NOT LIKE or similar operator, get the corresponding
	 * positive-match operator and work with that.	Set result to the correct
	 * default estimate, too.
	 */
	if (negate)
	{
		operator = get_negator(operator);
		if (!OidIsValid(operator))
			elog(ERROR, "patternsel called for operator without a negator");
		result = 1.0 - DEFAULT_MATCH_SEL;
	}
	else
	{
		result = DEFAULT_MATCH_SEL;
	}

	/*
	 * If expression is not variable op constant, then punt and return a
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
	 * something binary-compatible but different.)	We can use it to choose
	 * the index opfamily from which we must draw the comparison operators.
	 *
	 * NOTE: It would be more correct to use the PATTERN opfamilies than the
	 * simple ones, but at the moment ANALYZE will not generate statistics for
	 * the PATTERN operators.  But our results are so approximate anyway that
	 * it probably hardly matters.
	 */
	vartype = vardata.vartype;

	switch (vartype)
	{
		case TEXTOID:
			opfamily = TEXT_BTREE_FAM_OID;
			break;
		case BPCHAROID:
			opfamily = BPCHAR_BTREE_FAM_OID;
			break;
		case NAMEOID:
			opfamily = NAME_BTREE_FAM_OID;
			break;
		case BYTEAOID:
			opfamily = BYTEA_BTREE_FAM_OID;
			break;
		default:
			ReleaseVariableStats(vardata);
			return result;
	}

	/*
	 * Pull out any fixed prefix implied by the pattern, and estimate the
	 * fractional selectivity of the remainder of the pattern.	Unlike many of
	 * the other functions in this file, we use the pattern operator's actual
	 * collation for this step.  This is not because we expect the collation
	 * to make a big difference in the selectivity estimate (it seldom would),
	 * but because we want to be sure we cache compiled regexps under the
	 * right cache key, so that they can be re-used at runtime.
	 */
	patt = (Const *) other;
	pstatus = pattern_fixed_prefix(patt, ptype, collation,
								   &prefix, &rest_selec);

	/*
	 * If necessary, coerce the prefix constant to the right type.
	 */
	if (prefix && prefix->consttype != vartype)
	{
		char	   *prefixstr;

		switch (prefix->consttype)
		{
			case TEXTOID:
				prefixstr = TextDatumGetCString(prefix->constvalue);
				break;
			case BYTEAOID:
				prefixstr = DatumGetCString(DirectFunctionCall1(byteaout,
														prefix->constvalue));
				break;
			default:
				elog(ERROR, "unrecognized consttype: %u",
					 prefix->consttype);
				ReleaseVariableStats(vardata);
				return result;
		}
		prefix = string_to_const(prefixstr, vartype);
		pfree(prefixstr);
	}

	if (pstatus == Pattern_Prefix_Exact)
	{
		/*
		 * Pattern specifies an exact match, so pretend operator is '='
		 */
		Oid			eqopr = get_opfamily_member(opfamily, vartype, vartype,
												BTEqualStrategyNumber);

		if (eqopr == InvalidOid)
			elog(ERROR, "no = operator for opfamily %u", opfamily);
		result = var_eq_const(&vardata, eqopr, prefix->constvalue,
							  false, true);
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
		double		nullfrac,
					mcv_selec,
					sumcommon;

		/* Try to use the histogram entries to get selectivity */
		fmgr_info(get_opcode(operator), &opproc);

		selec = histogram_selectivity(&vardata, &opproc, constval, true,
									  10, 1, &hist_size);

		/* If not at least 100 entries, use the heuristic method */
		if (hist_size < 100)
		{
			Selectivity heursel;
			Selectivity prefixsel;

			if (pstatus == Pattern_Prefix_Partial)
				prefixsel = prefix_selectivity(root, &vardata, vartype,
											   opfamily, prefix);
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
		 * directly to the result selectivity.	Also add up the total fraction
		 * represented by MCV entries.
		 */
		mcv_selec = mcv_selectivity(&vardata, &opproc, constval, true,
									&sumcommon);

		if (HeapTupleIsValid(vardata.statsTuple))
			nullfrac = ((Form_pg_statistic) GETSTRUCT(vardata.statsTuple))->stanullfrac;
		else
			nullfrac = 0.0;

		/*
		 * Now merge the results from the MCV and histogram calculations,
		 * realizing that the histogram covers only the non-null values that
		 * are not listed in MCV.
		 */
		selec *= 1.0 - nullfrac - sumcommon;
		selec += mcv_selec;

		/* result should be in range, but make sure... */
		CLAMP_PROBABILITY(selec);
		result = selec;
	}

	if (prefix)
	{
		pfree(DatumGetPointer(prefix->constvalue));
		pfree(prefix);
	}

	ReleaseVariableStats(vardata);

	return negate ? (1.0 - result) : result;
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
 *		booltestsel		- Selectivity of BooleanTest Node.
 */
Selectivity
booltestsel(PlannerInfo *root, BoolTestType booltesttype, Node *arg,
			int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo)
{
	VariableStatData vardata;
	double		selec;

	examine_variable(root, arg, varRelid, &vardata);

	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;
		double		freq_null;
		Datum	   *values;
		int			nvalues;
		float4	   *numbers;
		int			nnumbers;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		freq_null = stats->stanullfrac;

		if (get_attstatsslot(vardata.statsTuple,
							 vardata.atttype, vardata.atttypmod,
							 STATISTIC_KIND_MCV, InvalidOid,
							 NULL,
							 &values, &nvalues,
							 &numbers, &nnumbers)
			&& nnumbers > 0)
		{
			double		freq_true;
			double		freq_false;

			/*
			 * Get first MCV frequency and derive frequency for true.
			 */
			if (DatumGetBool(values[0]))
				freq_true = numbers[0];
			else
				freq_true = 1.0 - numbers[0] - freq_null;

			/*
			 * Next derive frequency for false. Then use these as appropriate
			 * to derive frequency for each case.
			 */
			freq_false = 1.0 - freq_true - freq_null;

			switch (booltesttype)
			{
				case IS_UNKNOWN:
					/* select only NULL values */
					selec = freq_null;
					break;
				case IS_NOT_UNKNOWN:
					/* select non-NULL values */
					selec = 1.0 - freq_null;
					break;
				case IS_TRUE:
					/* select only TRUE values */
					selec = freq_true;
					break;
				case IS_NOT_TRUE:
					/* select non-TRUE values */
					selec = 1.0 - freq_true;
					break;
				case IS_FALSE:
					/* select only FALSE values */
					selec = freq_false;
					break;
				case IS_NOT_FALSE:
					/* select non-FALSE values */
					selec = 1.0 - freq_false;
					break;
				default:
					elog(ERROR, "unrecognized booltesttype: %d",
						 (int) booltesttype);
					selec = 0.0;	/* Keep compiler quiet */
					break;
			}

			free_attstatsslot(vardata.atttype, values, nvalues,
							  numbers, nnumbers);
		}
		else
		{
			/*
			 * No most-common-value info available. Still have null fraction
			 * information, so use it for IS [NOT] UNKNOWN. Otherwise adjust
			 * for null fraction and assume a 50-50 split of TRUE and FALSE.
			 */
			switch (booltesttype)
			{
				case IS_UNKNOWN:
					/* select only NULL values */
					selec = freq_null;
					break;
				case IS_NOT_UNKNOWN:
					/* select non-NULL values */
					selec = 1.0 - freq_null;
					break;
				case IS_TRUE:
				case IS_FALSE:
					/* Assume we select half of the non-NULL values */
					selec = (1.0 - freq_null) / 2.0;
					break;
				case IS_NOT_TRUE:
				case IS_NOT_FALSE:
					/* Assume we select NULLs plus half of the non-NULLs */
					/* equiv. to freq_null + (1.0 - freq_null) / 2.0 */
					selec = (freq_null + 1.0) / 2.0;
					break;
				default:
					elog(ERROR, "unrecognized booltesttype: %d",
						 (int) booltesttype);
					selec = 0.0;	/* Keep compiler quiet */
					break;
			}
		}
	}
	else
	{
		/*
		 * If we can't get variable statistics for the argument, perhaps
		 * clause_selectivity can do something with it.  We ignore the
		 * possibility of a NULL value when using clause_selectivity, and just
		 * assume the value is either TRUE or FALSE.
		 */
		switch (booltesttype)
		{
			case IS_UNKNOWN:
				selec = DEFAULT_UNK_SEL;
				break;
			case IS_NOT_UNKNOWN:
				selec = DEFAULT_NOT_UNK_SEL;
				break;
			case IS_TRUE:
			case IS_NOT_FALSE:
				selec = (double) clause_selectivity(root, arg,
													varRelid,
													jointype, sjinfo);
				break;
			case IS_FALSE:
			case IS_NOT_TRUE:
				selec = 1.0 - (double) clause_selectivity(root, arg,
														  varRelid,
														  jointype, sjinfo);
				break;
			default:
				elog(ERROR, "unrecognized booltesttype: %d",
					 (int) booltesttype);
				selec = 0.0;	/* Keep compiler quiet */
				break;
		}
	}

	ReleaseVariableStats(vardata);

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	return (Selectivity) selec;
}

/*
 *		nulltestsel		- Selectivity of NullTest Node.
 */
Selectivity
nulltestsel(PlannerInfo *root, NullTestType nulltesttype, Node *arg,
			int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo)
{
	VariableStatData vardata;
	double		selec;

	examine_variable(root, arg, varRelid, &vardata);

	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;
		double		freq_null;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		freq_null = stats->stanullfrac;

		switch (nulltesttype)
		{
			case IS_NULL:

				/*
				 * Use freq_null directly.
				 */
				selec = freq_null;
				break;
			case IS_NOT_NULL:

				/*
				 * Select not unknown (not null) values. Calculate from
				 * freq_null.
				 */
				selec = 1.0 - freq_null;
				break;
			default:
				elog(ERROR, "unrecognized nulltesttype: %d",
					 (int) nulltesttype);
				return (Selectivity) 0; /* keep compiler quiet */
		}
	}
	else
	{
		/*
		 * No ANALYZE stats available, so make a guess
		 */
		switch (nulltesttype)
		{
			case IS_NULL:
				selec = DEFAULT_UNK_SEL;
				break;
			case IS_NOT_NULL:
				selec = DEFAULT_NOT_UNK_SEL;
				break;
			default:
				elog(ERROR, "unrecognized nulltesttype: %d",
					 (int) nulltesttype);
				return (Selectivity) 0; /* keep compiler quiet */
		}
	}

	ReleaseVariableStats(vardata);

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	return (Selectivity) selec;
}

/*
 * strip_array_coercion - strip binary-compatible relabeling from an array expr
 *
 * For array values, the parser normally generates ArrayCoerceExpr conversions,
 * but it seems possible that RelabelType might show up.  Also, the planner
 * is not currently tense about collapsing stacked ArrayCoerceExpr nodes,
 * so we need to be ready to deal with more than one level.
 */
static Node *
strip_array_coercion(Node *node)
{
	for (;;)
	{
		if (node && IsA(node, ArrayCoerceExpr) &&
			((ArrayCoerceExpr *) node)->elemfuncid == InvalidOid)
		{
			node = (Node *) ((ArrayCoerceExpr *) node)->arg;
		}
		else if (node && IsA(node, RelabelType))
		{
			/* We don't really expect this case, but may as well cope */
			node = (Node *) ((RelabelType *) node)->arg;
		}
		else
			break;
	}
	return node;
}

/*
 *		scalararraysel		- Selectivity of ScalarArrayOpExpr Node.
 */
Selectivity
scalararraysel(PlannerInfo *root,
			   ScalarArrayOpExpr *clause,
			   bool is_join_clause,
			   int varRelid,
			   JoinType jointype,
			   SpecialJoinInfo *sjinfo)
{
	Oid			operator = clause->opno;
	bool		useOr = clause->useOr;
	bool		isEquality = false;
	bool		isInequality = false;
	Node	   *leftop;
	Node	   *rightop;
	Oid			nominal_element_type;
	Oid			nominal_element_collation;
	TypeCacheEntry *typentry;
	RegProcedure oprsel;
	FmgrInfo	oprselproc;
	Selectivity s1;
	Selectivity s1disjoint;

	/* First, deconstruct the expression */
	Assert(list_length(clause->args) == 2);
	leftop = (Node *) linitial(clause->args);
	rightop = (Node *) lsecond(clause->args);

	/* get nominal (after relabeling) element type of rightop */
	nominal_element_type = get_base_element_type(exprType(rightop));
	if (!OidIsValid(nominal_element_type))
		return (Selectivity) 0.5;		/* probably shouldn't happen */
	/* get nominal collation, too, for generating constants */
	nominal_element_collation = exprCollation(rightop);

	/* look through any binary-compatible relabeling of rightop */
	rightop = strip_array_coercion(rightop);

	/*
	 * Detect whether the operator is the default equality or inequality
	 * operator of the array element type.
	 */
	typentry = lookup_type_cache(nominal_element_type, TYPECACHE_EQ_OPR);
	if (OidIsValid(typentry->eq_opr))
	{
		if (operator == typentry->eq_opr)
			isEquality = true;
		else if (get_negator(operator) == typentry->eq_opr)
			isInequality = true;
	}

	/*
	 * If it is equality or inequality, we might be able to estimate this as a
	 * form of array containment; for instance "const = ANY(column)" can be
	 * treated as "ARRAY[const] <@ column".  scalararraysel_containment tries
	 * that, and returns the selectivity estimate if successful, or -1 if not.
	 */
	if ((isEquality || isInequality) && !is_join_clause)
	{
		s1 = scalararraysel_containment(root, leftop, rightop,
										nominal_element_type,
										isEquality, useOr, varRelid);
		if (s1 >= 0.0)
			return s1;
	}

	/*
	 * Look up the underlying operator's selectivity estimator. Punt if it
	 * hasn't got one.
	 */
	if (is_join_clause)
		oprsel = get_oprjoin(operator);
	else
		oprsel = get_oprrest(operator);
	if (!oprsel)
		return (Selectivity) 0.5;
	fmgr_info(oprsel, &oprselproc);

	/*
	 * In the array-containment check above, we must only believe that an
	 * operator is equality or inequality if it is the default btree equality
	 * operator (or its negator) for the element type, since those are the
	 * operators that array containment will use.  But in what follows, we can
	 * be a little laxer, and also believe that any operators using eqsel() or
	 * neqsel() as selectivity estimator act like equality or inequality.
	 */
	if (oprsel == F_EQSEL || oprsel == F_EQJOINSEL)
		isEquality = true;
	else if (oprsel == F_NEQSEL || oprsel == F_NEQJOINSEL)
		isInequality = true;

	/*
	 * We consider three cases:
	 *
	 * 1. rightop is an Array constant: deconstruct the array, apply the
	 * operator's selectivity function for each array element, and merge the
	 * results in the same way that clausesel.c does for AND/OR combinations.
	 *
	 * 2. rightop is an ARRAY[] construct: apply the operator's selectivity
	 * function for each element of the ARRAY[] construct, and merge.
	 *
	 * 3. otherwise, make a guess ...
	 */
	if (rightop && IsA(rightop, Const))
	{
		Datum		arraydatum = ((Const *) rightop)->constvalue;
		bool		arrayisnull = ((Const *) rightop)->constisnull;
		ArrayType  *arrayval;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		int			num_elems;
		Datum	   *elem_values;
		bool	   *elem_nulls;
		int			i;

		if (arrayisnull)		/* qual can't succeed if null array */
			return (Selectivity) 0.0;
		arrayval = DatumGetArrayTypeP(arraydatum);
		get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
							 &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arrayval,
						  ARR_ELEMTYPE(arrayval),
						  elmlen, elmbyval, elmalign,
						  &elem_values, &elem_nulls, &num_elems);

		/*
		 * For generic operators, we assume the probability of success is
		 * independent for each array element.	But for "= ANY" or "<> ALL",
		 * if the array elements are distinct (which'd typically be the case)
		 * then the probabilities are disjoint, and we should just sum them.
		 *
		 * If we were being really tense we would try to confirm that the
		 * elements are all distinct, but that would be expensive and it
		 * doesn't seem to be worth the cycles; it would amount to penalizing
		 * well-written queries in favor of poorly-written ones.  However, we
		 * do protect ourselves a little bit by checking whether the
		 * disjointness assumption leads to an impossible (out of range)
		 * probability; if so, we fall back to the normal calculation.
		 */
		s1 = s1disjoint = (useOr ? 0.0 : 1.0);

		for (i = 0; i < num_elems; i++)
		{
			List	   *args;
			Selectivity s2;

			args = list_make2(leftop,
							  makeConst(nominal_element_type,
										-1,
										nominal_element_collation,
										elmlen,
										elem_values[i],
										elem_nulls[i],
										elmbyval));
			if (is_join_clause)
				s2 = DatumGetFloat8(FunctionCall5Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
												  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
													  Int16GetDatum(jointype),
												   PointerGetDatum(sjinfo)));
			else
				s2 = DatumGetFloat8(FunctionCall4Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
												  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
												   Int32GetDatum(varRelid)));

			if (useOr)
			{
				s1 = s1 + s2 - s1 * s2;
				if (isEquality)
					s1disjoint += s2;
			}
			else
			{
				s1 = s1 * s2;
				if (isInequality)
					s1disjoint += s2 - 1.0;
			}
		}

		/* accept disjoint-probability estimate if in range */
		if ((useOr ? isEquality : isInequality) &&
			s1disjoint >= 0.0 && s1disjoint <= 1.0)
			s1 = s1disjoint;
	}
	else if (rightop && IsA(rightop, ArrayExpr) &&
			 !((ArrayExpr *) rightop)->multidims)
	{
		ArrayExpr  *arrayexpr = (ArrayExpr *) rightop;
		int16		elmlen;
		bool		elmbyval;
		ListCell   *l;

		get_typlenbyval(arrayexpr->element_typeid,
						&elmlen, &elmbyval);

		/*
		 * We use the assumption of disjoint probabilities here too, although
		 * the odds of equal array elements are rather higher if the elements
		 * are not all constants (which they won't be, else constant folding
		 * would have reduced the ArrayExpr to a Const).  In this path it's
		 * critical to have the sanity check on the s1disjoint estimate.
		 */
		s1 = s1disjoint = (useOr ? 0.0 : 1.0);

		foreach(l, arrayexpr->elements)
		{
			Node	   *elem = (Node *) lfirst(l);
			List	   *args;
			Selectivity s2;

			/*
			 * Theoretically, if elem isn't of nominal_element_type we should
			 * insert a RelabelType, but it seems unlikely that any operator
			 * estimation function would really care ...
			 */
			args = list_make2(leftop, elem);
			if (is_join_clause)
				s2 = DatumGetFloat8(FunctionCall5Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
												  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
													  Int16GetDatum(jointype),
												   PointerGetDatum(sjinfo)));
			else
				s2 = DatumGetFloat8(FunctionCall4Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
												  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
												   Int32GetDatum(varRelid)));

			if (useOr)
			{
				s1 = s1 + s2 - s1 * s2;
				if (isEquality)
					s1disjoint += s2;
			}
			else
			{
				s1 = s1 * s2;
				if (isInequality)
					s1disjoint += s2 - 1.0;
			}
		}

		/* accept disjoint-probability estimate if in range */
		if ((useOr ? isEquality : isInequality) &&
			s1disjoint >= 0.0 && s1disjoint <= 1.0)
			s1 = s1disjoint;
	}
	else
	{
		CaseTestExpr *dummyexpr;
		List	   *args;
		Selectivity s2;
		int			i;

		/*
		 * We need a dummy rightop to pass to the operator selectivity
		 * routine.  It can be pretty much anything that doesn't look like a
		 * constant; CaseTestExpr is a convenient choice.
		 */
		dummyexpr = makeNode(CaseTestExpr);
		dummyexpr->typeId = nominal_element_type;
		dummyexpr->typeMod = -1;
		dummyexpr->collation = clause->inputcollid;
		args = list_make2(leftop, dummyexpr);
		if (is_join_clause)
			s2 = DatumGetFloat8(FunctionCall5Coll(&oprselproc,
												  clause->inputcollid,
												  PointerGetDatum(root),
												  ObjectIdGetDatum(operator),
												  PointerGetDatum(args),
												  Int16GetDatum(jointype),
												  PointerGetDatum(sjinfo)));
		else
			s2 = DatumGetFloat8(FunctionCall4Coll(&oprselproc,
												  clause->inputcollid,
												  PointerGetDatum(root),
												  ObjectIdGetDatum(operator),
												  PointerGetDatum(args),
												  Int32GetDatum(varRelid)));
		s1 = useOr ? 0.0 : 1.0;

		/*
		 * Arbitrarily assume 10 elements in the eventual array value (see
		 * also estimate_array_length).  We don't risk an assumption of
		 * disjoint probabilities here.
		 */
		for (i = 0; i < 10; i++)
		{
			if (useOr)
				s1 = s1 + s2 - s1 * s2;
			else
				s1 = s1 * s2;
		}
	}

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(s1);

	return s1;
}

/*
 * Estimate number of elements in the array yielded by an expression.
 *
 * It's important that this agree with scalararraysel.
 */
int
estimate_array_length(Node *arrayexpr)
{
	/* look through any binary-compatible relabeling of arrayexpr */
	arrayexpr = strip_array_coercion(arrayexpr);

	if (arrayexpr && IsA(arrayexpr, Const))
	{
		Datum		arraydatum = ((Const *) arrayexpr)->constvalue;
		bool		arrayisnull = ((Const *) arrayexpr)->constisnull;
		ArrayType  *arrayval;

		if (arrayisnull)
			return 0;
		arrayval = DatumGetArrayTypeP(arraydatum);
		return ArrayGetNItems(ARR_NDIM(arrayval), ARR_DIMS(arrayval));
	}
	else if (arrayexpr && IsA(arrayexpr, ArrayExpr) &&
			 !((ArrayExpr *) arrayexpr)->multidims)
	{
		return list_length(((ArrayExpr *) arrayexpr)->elements);
	}
	else
	{
		/* default guess --- see also scalararraysel */
		return 10;
	}
}

/*
 *		rowcomparesel		- Selectivity of RowCompareExpr Node.
 *
 * We estimate RowCompare selectivity by considering just the first (high
 * order) columns, which makes it equivalent to an ordinary OpExpr.  While
 * this estimate could be refined by considering additional columns, it
 * seems unlikely that we could do a lot better without multi-column
 * statistics.
 */
Selectivity
rowcomparesel(PlannerInfo *root,
			  RowCompareExpr *clause,
			  int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo)
{
	Selectivity s1;
	Oid			opno = linitial_oid(clause->opnos);
	Oid			inputcollid = linitial_oid(clause->inputcollids);
	List	   *opargs;
	bool		is_join_clause;

	/* Build equivalent arg list for single operator */
	opargs = list_make2(linitial(clause->largs), linitial(clause->rargs));

	/*
	 * Decide if it's a join clause.  This should match clausesel.c's
	 * treat_as_join_clause(), except that we intentionally consider only the
	 * leading columns and not the rest of the clause.
	 */
	if (varRelid != 0)
	{
		/*
		 * Caller is forcing restriction mode (eg, because we are examining an
		 * inner indexscan qual).
		 */
		is_join_clause = false;
	}
	else if (sjinfo == NULL)
	{
		/*
		 * It must be a restriction clause, since it's being evaluated at a
		 * scan node.
		 */
		is_join_clause = false;
	}
	else
	{
		/*
		 * Otherwise, it's a join if there's more than one relation used.
		 */
		is_join_clause = (NumRelids((Node *) opargs) > 1);
	}

	if (is_join_clause)
	{
		/* Estimate selectivity for a join clause. */
		s1 = join_selectivity(root, opno,
							  opargs,
							  inputcollid,
							  jointype,
							  sjinfo);
	}
	else
	{
		/* Estimate selectivity for a restriction clause. */
		s1 = restriction_selectivity(root, opno,
									 opargs,
									 inputcollid,
									 varRelid);
	}

	return s1;
}

/*
 *		eqjoinsel		- Join selectivity of "="
 */
Datum
eqjoinsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);

#ifdef NOT_USED
	JoinType	jointype = (JoinType) PG_GETARG_INT16(3);
#endif
	SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) PG_GETARG_POINTER(4);
	double		selec;
	VariableStatData vardata1;
	VariableStatData vardata2;
	bool		join_is_reversed;
	RelOptInfo *inner_rel;

	get_join_variables(root, args, sjinfo,
					   &vardata1, &vardata2, &join_is_reversed);

	switch (sjinfo->jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
		case JOIN_FULL:
			selec = eqjoinsel_inner(operator, &vardata1, &vardata2);
			break;
		case JOIN_SEMI:
		case JOIN_ANTI:

			/*
			 * Look up the join's inner relation.  min_righthand is sufficient
			 * information because neither SEMI nor ANTI joins permit any
			 * reassociation into or out of their RHS, so the righthand will
			 * always be exactly that set of rels.
			 */
			inner_rel = find_join_input_rel(root, sjinfo->min_righthand);

			if (!join_is_reversed)
				selec = eqjoinsel_semi(operator, &vardata1, &vardata2,
									   inner_rel);
			else
				selec = eqjoinsel_semi(get_commutator(operator),
									   &vardata2, &vardata1,
									   inner_rel);
			break;
		default:
			/* other values not expected here */
			elog(ERROR, "unrecognized join type: %d",
				 (int) sjinfo->jointype);
			selec = 0;			/* keep compiler quiet */
			break;
	}

	ReleaseVariableStats(vardata1);
	ReleaseVariableStats(vardata2);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 * eqjoinsel_inner --- eqjoinsel for normal inner join
 *
 * We also use this for LEFT/FULL outer joins; it's not presently clear
 * that it's worth trying to distinguish them here.
 */
static double
eqjoinsel_inner(Oid operator,
				VariableStatData *vardata1, VariableStatData *vardata2)
{
	double		selec;
	double		nd1;
	double		nd2;
	bool		isdefault1;
	bool		isdefault2;
	Form_pg_statistic stats1 = NULL;
	Form_pg_statistic stats2 = NULL;
	bool		have_mcvs1 = false;
	Datum	   *values1 = NULL;
	int			nvalues1 = 0;
	float4	   *numbers1 = NULL;
	int			nnumbers1 = 0;
	bool		have_mcvs2 = false;
	Datum	   *values2 = NULL;
	int			nvalues2 = 0;
	float4	   *numbers2 = NULL;
	int			nnumbers2 = 0;

	nd1 = get_variable_numdistinct(vardata1, &isdefault1);
	nd2 = get_variable_numdistinct(vardata2, &isdefault2);

	if (HeapTupleIsValid(vardata1->statsTuple))
	{
		stats1 = (Form_pg_statistic) GETSTRUCT(vardata1->statsTuple);
		have_mcvs1 = get_attstatsslot(vardata1->statsTuple,
									  vardata1->atttype,
									  vardata1->atttypmod,
									  STATISTIC_KIND_MCV,
									  InvalidOid,
									  NULL,
									  &values1, &nvalues1,
									  &numbers1, &nnumbers1);
	}

	if (HeapTupleIsValid(vardata2->statsTuple))
	{
		stats2 = (Form_pg_statistic) GETSTRUCT(vardata2->statsTuple);
		have_mcvs2 = get_attstatsslot(vardata2->statsTuple,
									  vardata2->atttype,
									  vardata2->atttypmod,
									  STATISTIC_KIND_MCV,
									  InvalidOid,
									  NULL,
									  &values2, &nvalues2,
									  &numbers2, &nnumbers2);
	}

	if (have_mcvs1 && have_mcvs2)
	{
		/*
		 * We have most-common-value lists for both relations.	Run through
		 * the lists to see which MCVs actually join to each other with the
		 * given operator.	This allows us to determine the exact join
		 * selectivity for the portion of the relations represented by the MCV
		 * lists.  We still have to estimate for the remaining population, but
		 * in a skewed distribution this gives us a big leg up in accuracy.
		 * For motivation see the analysis in Y. Ioannidis and S.
		 * Christodoulakis, "On the propagation of errors in the size of join
		 * results", Technical Report 1018, Computer Science Dept., University
		 * of Wisconsin, Madison, March 1991 (available from ftp.cs.wisc.edu).
		 */
		FmgrInfo	eqproc;
		bool	   *hasmatch1;
		bool	   *hasmatch2;
		double		nullfrac1 = stats1->stanullfrac;
		double		nullfrac2 = stats2->stanullfrac;
		double		matchprodfreq,
					matchfreq1,
					matchfreq2,
					unmatchfreq1,
					unmatchfreq2,
					otherfreq1,
					otherfreq2,
					totalsel1,
					totalsel2;
		int			i,
					nmatches;

		fmgr_info(get_opcode(operator), &eqproc);
		hasmatch1 = (bool *) palloc0(nvalues1 * sizeof(bool));
		hasmatch2 = (bool *) palloc0(nvalues2 * sizeof(bool));

		/*
		 * Note we assume that each MCV will match at most one member of the
		 * other MCV list.	If the operator isn't really equality, there could
		 * be multiple matches --- but we don't look for them, both for speed
		 * and because the math wouldn't add up...
		 */
		matchprodfreq = 0.0;
		nmatches = 0;
		for (i = 0; i < nvalues1; i++)
		{
			int			j;

			for (j = 0; j < nvalues2; j++)
			{
				if (hasmatch2[j])
					continue;
				if (DatumGetBool(FunctionCall2Coll(&eqproc,
												   DEFAULT_COLLATION_OID,
												   values1[i],
												   values2[j])))
				{
					hasmatch1[i] = hasmatch2[j] = true;
					matchprodfreq += numbers1[i] * numbers2[j];
					nmatches++;
					break;
				}
			}
		}
		CLAMP_PROBABILITY(matchprodfreq);
		/* Sum up frequencies of matched and unmatched MCVs */
		matchfreq1 = unmatchfreq1 = 0.0;
		for (i = 0; i < nvalues1; i++)
		{
			if (hasmatch1[i])
				matchfreq1 += numbers1[i];
			else
				unmatchfreq1 += numbers1[i];
		}
		CLAMP_PROBABILITY(matchfreq1);
		CLAMP_PROBABILITY(unmatchfreq1);
		matchfreq2 = unmatchfreq2 = 0.0;
		for (i = 0; i < nvalues2; i++)
		{
			if (hasmatch2[i])
				matchfreq2 += numbers2[i];
			else
				unmatchfreq2 += numbers2[i];
		}
		CLAMP_PROBABILITY(matchfreq2);
		CLAMP_PROBABILITY(unmatchfreq2);
		pfree(hasmatch1);
		pfree(hasmatch2);

		/*
		 * Compute total frequency of non-null values that are not in the MCV
		 * lists.
		 */
		otherfreq1 = 1.0 - nullfrac1 - matchfreq1 - unmatchfreq1;
		otherfreq2 = 1.0 - nullfrac2 - matchfreq2 - unmatchfreq2;
		CLAMP_PROBABILITY(otherfreq1);
		CLAMP_PROBABILITY(otherfreq2);

		/*
		 * We can estimate the total selectivity from the point of view of
		 * relation 1 as: the known selectivity for matched MCVs, plus
		 * unmatched MCVs that are assumed to match against random members of
		 * relation 2's non-MCV population, plus non-MCV values that are
		 * assumed to match against random members of relation 2's unmatched
		 * MCVs plus non-MCV values.
		 */
		totalsel1 = matchprodfreq;
		if (nd2 > nvalues2)
			totalsel1 += unmatchfreq1 * otherfreq2 / (nd2 - nvalues2);
		if (nd2 > nmatches)
			totalsel1 += otherfreq1 * (otherfreq2 + unmatchfreq2) /
				(nd2 - nmatches);
		/* Same estimate from the point of view of relation 2. */
		totalsel2 = matchprodfreq;
		if (nd1 > nvalues1)
			totalsel2 += unmatchfreq2 * otherfreq1 / (nd1 - nvalues1);
		if (nd1 > nmatches)
			totalsel2 += otherfreq2 * (otherfreq1 + unmatchfreq1) /
				(nd1 - nmatches);

		/*
		 * Use the smaller of the two estimates.  This can be justified in
		 * essentially the same terms as given below for the no-stats case: to
		 * a first approximation, we are estimating from the point of view of
		 * the relation with smaller nd.
		 */
		selec = (totalsel1 < totalsel2) ? totalsel1 : totalsel2;
	}
	else
	{
		/*
		 * We do not have MCV lists for both sides.  Estimate the join
		 * selectivity as MIN(1/nd1,1/nd2)*(1-nullfrac1)*(1-nullfrac2). This
		 * is plausible if we assume that the join operator is strict and the
		 * non-null values are about equally distributed: a given non-null
		 * tuple of rel1 will join to either zero or N2*(1-nullfrac2)/nd2 rows
		 * of rel2, so total join rows are at most
		 * N1*(1-nullfrac1)*N2*(1-nullfrac2)/nd2 giving a join selectivity of
		 * not more than (1-nullfrac1)*(1-nullfrac2)/nd2. By the same logic it
		 * is not more than (1-nullfrac1)*(1-nullfrac2)/nd1, so the expression
		 * with MIN() is an upper bound.  Using the MIN() means we estimate
		 * from the point of view of the relation with smaller nd (since the
		 * larger nd is determining the MIN).  It is reasonable to assume that
		 * most tuples in this rel will have join partners, so the bound is
		 * probably reasonably tight and should be taken as-is.
		 *
		 * XXX Can we be smarter if we have an MCV list for just one side? It
		 * seems that if we assume equal distribution for the other side, we
		 * end up with the same answer anyway.
		 */
		double		nullfrac1 = stats1 ? stats1->stanullfrac : 0.0;
		double		nullfrac2 = stats2 ? stats2->stanullfrac : 0.0;

		selec = (1.0 - nullfrac1) * (1.0 - nullfrac2);
		if (nd1 > nd2)
			selec /= nd1;
		else
			selec /= nd2;
	}

	if (have_mcvs1)
		free_attstatsslot(vardata1->atttype, values1, nvalues1,
						  numbers1, nnumbers1);
	if (have_mcvs2)
		free_attstatsslot(vardata2->atttype, values2, nvalues2,
						  numbers2, nnumbers2);

	return selec;
}

/*
 * eqjoinsel_semi --- eqjoinsel for semi join
 *
 * (Also used for anti join, which we are supposed to estimate the same way.)
 * Caller has ensured that vardata1 is the LHS variable.
 */
static double
eqjoinsel_semi(Oid operator,
			   VariableStatData *vardata1, VariableStatData *vardata2,
			   RelOptInfo *inner_rel)
{
	double		selec;
	double		nd1;
	double		nd2;
	bool		isdefault1;
	bool		isdefault2;
	Form_pg_statistic stats1 = NULL;
	bool		have_mcvs1 = false;
	Datum	   *values1 = NULL;
	int			nvalues1 = 0;
	float4	   *numbers1 = NULL;
	int			nnumbers1 = 0;
	bool		have_mcvs2 = false;
	Datum	   *values2 = NULL;
	int			nvalues2 = 0;
	float4	   *numbers2 = NULL;
	int			nnumbers2 = 0;

	nd1 = get_variable_numdistinct(vardata1, &isdefault1);
	nd2 = get_variable_numdistinct(vardata2, &isdefault2);

	/*
	 * We clamp nd2 to be not more than what we estimate the inner relation's
	 * size to be.	This is intuitively somewhat reasonable since obviously
	 * there can't be more than that many distinct values coming from the
	 * inner rel.  The reason for the asymmetry (ie, that we don't clamp nd1
	 * likewise) is that this is the only pathway by which restriction clauses
	 * applied to the inner rel will affect the join result size estimate,
	 * since set_joinrel_size_estimates will multiply SEMI/ANTI selectivity by
	 * only the outer rel's size.  If we clamped nd1 we'd be double-counting
	 * the selectivity of outer-rel restrictions.
	 *
	 * We can apply this clamping both with respect to the base relation from
	 * which the join variable comes (if there is just one), and to the
	 * immediate inner input relation of the current join.
	 */
	if (vardata2->rel)
		nd2 = Min(nd2, vardata2->rel->rows);
	nd2 = Min(nd2, inner_rel->rows);

	if (HeapTupleIsValid(vardata1->statsTuple))
	{
		stats1 = (Form_pg_statistic) GETSTRUCT(vardata1->statsTuple);
		have_mcvs1 = get_attstatsslot(vardata1->statsTuple,
									  vardata1->atttype,
									  vardata1->atttypmod,
									  STATISTIC_KIND_MCV,
									  InvalidOid,
									  NULL,
									  &values1, &nvalues1,
									  &numbers1, &nnumbers1);
	}

	if (HeapTupleIsValid(vardata2->statsTuple))
	{
		have_mcvs2 = get_attstatsslot(vardata2->statsTuple,
									  vardata2->atttype,
									  vardata2->atttypmod,
									  STATISTIC_KIND_MCV,
									  InvalidOid,
									  NULL,
									  &values2, &nvalues2,
									  &numbers2, &nnumbers2);
	}

	if (have_mcvs1 && have_mcvs2 && OidIsValid(operator))
	{
		/*
		 * We have most-common-value lists for both relations.	Run through
		 * the lists to see which MCVs actually join to each other with the
		 * given operator.	This allows us to determine the exact join
		 * selectivity for the portion of the relations represented by the MCV
		 * lists.  We still have to estimate for the remaining population, but
		 * in a skewed distribution this gives us a big leg up in accuracy.
		 */
		FmgrInfo	eqproc;
		bool	   *hasmatch1;
		bool	   *hasmatch2;
		double		nullfrac1 = stats1->stanullfrac;
		double		matchfreq1,
					uncertainfrac,
					uncertain;
		int			i,
					nmatches,
					clamped_nvalues2;

		/*
		 * The clamping above could have resulted in nd2 being less than
		 * nvalues2; in which case, we assume that precisely the nd2 most
		 * common values in the relation will appear in the join input, and so
		 * compare to only the first nd2 members of the MCV list.  Of course
		 * this is frequently wrong, but it's the best bet we can make.
		 */
		clamped_nvalues2 = Min(nvalues2, nd2);

		fmgr_info(get_opcode(operator), &eqproc);
		hasmatch1 = (bool *) palloc0(nvalues1 * sizeof(bool));
		hasmatch2 = (bool *) palloc0(clamped_nvalues2 * sizeof(bool));

		/*
		 * Note we assume that each MCV will match at most one member of the
		 * other MCV list.	If the operator isn't really equality, there could
		 * be multiple matches --- but we don't look for them, both for speed
		 * and because the math wouldn't add up...
		 */
		nmatches = 0;
		for (i = 0; i < nvalues1; i++)
		{
			int			j;

			for (j = 0; j < clamped_nvalues2; j++)
			{
				if (hasmatch2[j])
					continue;
				if (DatumGetBool(FunctionCall2Coll(&eqproc,
												   DEFAULT_COLLATION_OID,
												   values1[i],
												   values2[j])))
				{
					hasmatch1[i] = hasmatch2[j] = true;
					nmatches++;
					break;
				}
			}
		}
		/* Sum up frequencies of matched MCVs */
		matchfreq1 = 0.0;
		for (i = 0; i < nvalues1; i++)
		{
			if (hasmatch1[i])
				matchfreq1 += numbers1[i];
		}
		CLAMP_PROBABILITY(matchfreq1);
		pfree(hasmatch1);
		pfree(hasmatch2);

		/*
		 * Now we need to estimate the fraction of relation 1 that has at
		 * least one join partner.	We know for certain that the matched MCVs
		 * do, so that gives us a lower bound, but we're really in the dark
		 * about everything else.  Our crude approach is: if nd1 <= nd2 then
		 * assume all non-null rel1 rows have join partners, else assume for
		 * the uncertain rows that a fraction nd2/nd1 have join partners. We
		 * can discount the known-matched MCVs from the distinct-values counts
		 * before doing the division.
		 *
		 * Crude as the above is, it's completely useless if we don't have
		 * reliable ndistinct values for both sides.  Hence, if either nd1 or
		 * nd2 is default, punt and assume half of the uncertain rows have
		 * join partners.
		 */
		if (!isdefault1 && !isdefault2)
		{
			nd1 -= nmatches;
			nd2 -= nmatches;
			if (nd1 <= nd2 || nd2 < 0)
				uncertainfrac = 1.0;
			else
				uncertainfrac = nd2 / nd1;
		}
		else
			uncertainfrac = 0.5;
		uncertain = 1.0 - matchfreq1 - nullfrac1;
		CLAMP_PROBABILITY(uncertain);
		selec = matchfreq1 + uncertainfrac * uncertain;
	}
	else
	{
		/*
		 * Without MCV lists for both sides, we can only use the heuristic
		 * about nd1 vs nd2.
		 */
		double		nullfrac1 = stats1 ? stats1->stanullfrac : 0.0;

		if (!isdefault1 && !isdefault2)
		{
			if (nd1 <= nd2 || nd2 < 0)
				selec = 1.0 - nullfrac1;
			else
				selec = (nd2 / nd1) * (1.0 - nullfrac1);
		}
		else
			selec = 0.5 * (1.0 - nullfrac1);
	}

	if (have_mcvs1)
		free_attstatsslot(vardata1->atttype, values1, nvalues1,
						  numbers1, nnumbers1);
	if (have_mcvs2)
		free_attstatsslot(vardata2->atttype, values2, nvalues2,
						  numbers2, nnumbers2);

	return selec;
}

/*
 *		neqjoinsel		- Join selectivity of "!="
 */
Datum
neqjoinsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	JoinType	jointype = (JoinType) PG_GETARG_INT16(3);
	SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) PG_GETARG_POINTER(4);
	Oid			eqop;
	float8		result;

	/*
	 * We want 1 - eqjoinsel() where the equality operator is the one
	 * associated with this != operator, that is, its negator.
	 */
	eqop = get_negator(operator);
	if (eqop)
	{
		result = DatumGetFloat8(DirectFunctionCall5(eqjoinsel,
													PointerGetDatum(root),
													ObjectIdGetDatum(eqop),
													PointerGetDatum(args),
													Int16GetDatum(jointype),
													PointerGetDatum(sjinfo)));
	}
	else
	{
		/* Use default selectivity (should we raise an error instead?) */
		result = DEFAULT_EQ_SEL;
	}
	result = 1.0 - result;
	PG_RETURN_FLOAT8(result);
}

/*
 *		scalarltjoinsel - Join selectivity of "<" and "<=" for scalars
 */
Datum
scalarltjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
}

/*
 *		scalargtjoinsel - Join selectivity of ">" and ">=" for scalars
 */
Datum
scalargtjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
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

/*
 * mergejoinscansel			- Scan selectivity of merge join.
 *
 * A merge join will stop as soon as it exhausts either input stream.
 * Therefore, if we can estimate the ranges of both input variables,
 * we can estimate how much of the input will actually be read.  This
 * can have a considerable impact on the cost when using indexscans.
 *
 * Also, we can estimate how much of each input has to be read before the
 * first join pair is found, which will affect the join's startup time.
 *
 * clause should be a clause already known to be mergejoinable.  opfamily,
 * strategy, and nulls_first specify the sort ordering being used.
 *
 * The outputs are:
 *		*leftstart is set to the fraction of the left-hand variable expected
 *		 to be scanned before the first join pair is found (0 to 1).
 *		*leftend is set to the fraction of the left-hand variable expected
 *		 to be scanned before the join terminates (0 to 1).
 *		*rightstart, *rightend similarly for the right-hand variable.
 */
void
mergejoinscansel(PlannerInfo *root, Node *clause,
				 Oid opfamily, int strategy, bool nulls_first,
				 Selectivity *leftstart, Selectivity *leftend,
				 Selectivity *rightstart, Selectivity *rightend)
{
	Node	   *left,
			   *right;
	VariableStatData leftvar,
				rightvar;
	int			op_strategy;
	Oid			op_lefttype;
	Oid			op_righttype;
	Oid			opno,
				lsortop,
				rsortop,
				lstatop,
				rstatop,
				ltop,
				leop,
				revltop,
				revleop;
	bool		isgt;
	Datum		leftmin,
				leftmax,
				rightmin,
				rightmax;
	double		selec;

	/* Set default results if we can't figure anything out. */
	/* XXX should default "start" fraction be a bit more than 0? */
	*leftstart = *rightstart = 0.0;
	*leftend = *rightend = 1.0;

	/* Deconstruct the merge clause */
	if (!is_opclause(clause))
		return;					/* shouldn't happen */
	opno = ((OpExpr *) clause)->opno;
	left = get_leftop((Expr *) clause);
	right = get_rightop((Expr *) clause);
	if (!right)
		return;					/* shouldn't happen */

	/* Look for stats for the inputs */
	examine_variable(root, left, 0, &leftvar);
	examine_variable(root, right, 0, &rightvar);

	/* Extract the operator's declared left/right datatypes */
	get_op_opfamily_properties(opno, opfamily, false,
							   &op_strategy,
							   &op_lefttype,
							   &op_righttype);
	Assert(op_strategy == BTEqualStrategyNumber);

	/*
	 * Look up the various operators we need.  If we don't find them all, it
	 * probably means the opfamily is broken, but we just fail silently.
	 *
	 * Note: we expect that pg_statistic histograms will be sorted by the '<'
	 * operator, regardless of which sort direction we are considering.
	 */
	switch (strategy)
	{
		case BTLessStrategyNumber:
			isgt = false;
			if (op_lefttype == op_righttype)
			{
				/* easy case */
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTLessStrategyNumber);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTLessEqualStrategyNumber);
				lsortop = ltop;
				rsortop = ltop;
				lstatop = lsortop;
				rstatop = rsortop;
				revltop = ltop;
				revleop = leop;
			}
			else
			{
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTLessStrategyNumber);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTLessEqualStrategyNumber);
				lsortop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  BTLessStrategyNumber);
				rsortop = get_opfamily_member(opfamily,
											  op_righttype, op_righttype,
											  BTLessStrategyNumber);
				lstatop = lsortop;
				rstatop = rsortop;
				revltop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  BTLessStrategyNumber);
				revleop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  BTLessEqualStrategyNumber);
			}
			break;
		case BTGreaterStrategyNumber:
			/* descending-order case */
			isgt = true;
			if (op_lefttype == op_righttype)
			{
				/* easy case */
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTGreaterStrategyNumber);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTGreaterEqualStrategyNumber);
				lsortop = ltop;
				rsortop = ltop;
				lstatop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  BTLessStrategyNumber);
				rstatop = lstatop;
				revltop = ltop;
				revleop = leop;
			}
			else
			{
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTGreaterStrategyNumber);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   BTGreaterEqualStrategyNumber);
				lsortop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  BTGreaterStrategyNumber);
				rsortop = get_opfamily_member(opfamily,
											  op_righttype, op_righttype,
											  BTGreaterStrategyNumber);
				lstatop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  BTLessStrategyNumber);
				rstatop = get_opfamily_member(opfamily,
											  op_righttype, op_righttype,
											  BTLessStrategyNumber);
				revltop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  BTGreaterStrategyNumber);
				revleop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  BTGreaterEqualStrategyNumber);
			}
			break;
		default:
			goto fail;			/* shouldn't get here */
	}

	if (!OidIsValid(lsortop) ||
		!OidIsValid(rsortop) ||
		!OidIsValid(lstatop) ||
		!OidIsValid(rstatop) ||
		!OidIsValid(ltop) ||
		!OidIsValid(leop) ||
		!OidIsValid(revltop) ||
		!OidIsValid(revleop))
		goto fail;				/* insufficient info in catalogs */

	/* Try to get ranges of both inputs */
	if (!isgt)
	{
		if (!get_variable_range(root, &leftvar, lstatop,
								&leftmin, &leftmax))
			goto fail;			/* no range available from stats */
		if (!get_variable_range(root, &rightvar, rstatop,
								&rightmin, &rightmax))
			goto fail;			/* no range available from stats */
	}
	else
	{
		/* need to swap the max and min */
		if (!get_variable_range(root, &leftvar, lstatop,
								&leftmax, &leftmin))
			goto fail;			/* no range available from stats */
		if (!get_variable_range(root, &rightvar, rstatop,
								&rightmax, &rightmin))
			goto fail;			/* no range available from stats */
	}

	/*
	 * Now, the fraction of the left variable that will be scanned is the
	 * fraction that's <= the right-side maximum value.  But only believe
	 * non-default estimates, else stick with our 1.0.
	 */
	selec = scalarineqsel(root, leop, isgt, &leftvar,
						  rightmax, op_righttype);
	if (selec != DEFAULT_INEQ_SEL)
		*leftend = selec;

	/* And similarly for the right variable. */
	selec = scalarineqsel(root, revleop, isgt, &rightvar,
						  leftmax, op_lefttype);
	if (selec != DEFAULT_INEQ_SEL)
		*rightend = selec;

	/*
	 * Only one of the two "end" fractions can really be less than 1.0;
	 * believe the smaller estimate and reset the other one to exactly 1.0. If
	 * we get exactly equal estimates (as can easily happen with self-joins),
	 * believe neither.
	 */
	if (*leftend > *rightend)
		*leftend = 1.0;
	else if (*leftend < *rightend)
		*rightend = 1.0;
	else
		*leftend = *rightend = 1.0;

	/*
	 * Also, the fraction of the left variable that will be scanned before the
	 * first join pair is found is the fraction that's < the right-side
	 * minimum value.  But only believe non-default estimates, else stick with
	 * our own default.
	 */
	selec = scalarineqsel(root, ltop, isgt, &leftvar,
						  rightmin, op_righttype);
	if (selec != DEFAULT_INEQ_SEL)
		*leftstart = selec;

	/* And similarly for the right variable. */
	selec = scalarineqsel(root, revltop, isgt, &rightvar,
						  leftmin, op_lefttype);
	if (selec != DEFAULT_INEQ_SEL)
		*rightstart = selec;

	/*
	 * Only one of the two "start" fractions can really be more than zero;
	 * believe the larger estimate and reset the other one to exactly 0.0. If
	 * we get exactly equal estimates (as can easily happen with self-joins),
	 * believe neither.
	 */
	if (*leftstart < *rightstart)
		*leftstart = 0.0;
	else if (*leftstart > *rightstart)
		*rightstart = 0.0;
	else
		*leftstart = *rightstart = 0.0;

	/*
	 * If the sort order is nulls-first, we're going to have to skip over any
	 * nulls too.  These would not have been counted by scalarineqsel, and we
	 * can safely add in this fraction regardless of whether we believe
	 * scalarineqsel's results or not.  But be sure to clamp the sum to 1.0!
	 */
	if (nulls_first)
	{
		Form_pg_statistic stats;

		if (HeapTupleIsValid(leftvar.statsTuple))
		{
			stats = (Form_pg_statistic) GETSTRUCT(leftvar.statsTuple);
			*leftstart += stats->stanullfrac;
			CLAMP_PROBABILITY(*leftstart);
			*leftend += stats->stanullfrac;
			CLAMP_PROBABILITY(*leftend);
		}
		if (HeapTupleIsValid(rightvar.statsTuple))
		{
			stats = (Form_pg_statistic) GETSTRUCT(rightvar.statsTuple);
			*rightstart += stats->stanullfrac;
			CLAMP_PROBABILITY(*rightstart);
			*rightend += stats->stanullfrac;
			CLAMP_PROBABILITY(*rightend);
		}
	}

	/* Disbelieve start >= end, just in case that can happen */
	if (*leftstart >= *leftend)
	{
		*leftstart = 0.0;
		*leftend = 1.0;
	}
	if (*rightstart >= *rightend)
	{
		*rightstart = 0.0;
		*rightend = 1.0;
	}

fail:
	ReleaseVariableStats(leftvar);
	ReleaseVariableStats(rightvar);
}


/*
 * Helper routine for estimate_num_groups: add an item to a list of
 * GroupVarInfos, but only if it's not known equal to any of the existing
 * entries.
 */
typedef struct
{
	Node	   *var;			/* might be an expression, not just a Var */
	RelOptInfo *rel;			/* relation it belongs to */
	double		ndistinct;		/* # distinct values */
} GroupVarInfo;

static List *
add_unique_group_var(PlannerInfo *root, List *varinfos,
					 Node *var, VariableStatData *vardata)
{
	GroupVarInfo *varinfo;
	double		ndistinct;
	bool		isdefault;
	ListCell   *lc;

	ndistinct = get_variable_numdistinct(vardata, &isdefault);

	/* cannot use foreach here because of possible list_delete */
	lc = list_head(varinfos);
	while (lc)
	{
		varinfo = (GroupVarInfo *) lfirst(lc);

		/* must advance lc before list_delete possibly pfree's it */
		lc = lnext(lc);

		/* Drop exact duplicates */
		if (equal(var, varinfo->var))
			return varinfos;

		/*
		 * Drop known-equal vars, but only if they belong to different
		 * relations (see comments for estimate_num_groups)
		 */
		if (vardata->rel != varinfo->rel &&
			exprs_known_equal(root, var, varinfo->var))
		{
			if (varinfo->ndistinct <= ndistinct)
			{
				/* Keep older item, forget new one */
				return varinfos;
			}
			else
			{
				/* Delete the older item */
				varinfos = list_delete_ptr(varinfos, varinfo);
			}
		}
	}

	varinfo = (GroupVarInfo *) palloc(sizeof(GroupVarInfo));

	varinfo->var = var;
	varinfo->rel = vardata->rel;
	varinfo->ndistinct = ndistinct;
	varinfos = lappend(varinfos, varinfo);
	return varinfos;
}

/*
 * estimate_num_groups		- Estimate number of groups in a grouped query
 *
 * Given a query having a GROUP BY clause, estimate how many groups there
 * will be --- ie, the number of distinct combinations of the GROUP BY
 * expressions.
 *
 * This routine is also used to estimate the number of rows emitted by
 * a DISTINCT filtering step; that is an isomorphic problem.  (Note:
 * actually, we only use it for DISTINCT when there's no grouping or
 * aggregation ahead of the DISTINCT.)
 *
 * Inputs:
 *	root - the query
 *	groupExprs - list of expressions being grouped by
 *	input_rows - number of rows estimated to arrive at the group/unique
 *		filter step
 *
 * Given the lack of any cross-correlation statistics in the system, it's
 * impossible to do anything really trustworthy with GROUP BY conditions
 * involving multiple Vars.  We should however avoid assuming the worst
 * case (all possible cross-product terms actually appear as groups) since
 * very often the grouped-by Vars are highly correlated.  Our current approach
 * is as follows:
 *	1.	Expressions yielding boolean are assumed to contribute two groups,
 *		independently of their content, and are ignored in the subsequent
 *		steps.	This is mainly because tests like "col IS NULL" break the
 *		heuristic used in step 2 especially badly.
 *	2.	Reduce the given expressions to a list of unique Vars used.  For
 *		example, GROUP BY a, a + b is treated the same as GROUP BY a, b.
 *		It is clearly correct not to count the same Var more than once.
 *		It is also reasonable to treat f(x) the same as x: f() cannot
 *		increase the number of distinct values (unless it is volatile,
 *		which we consider unlikely for grouping), but it probably won't
 *		reduce the number of distinct values much either.
 *		As a special case, if a GROUP BY expression can be matched to an
 *		expressional index for which we have statistics, then we treat the
 *		whole expression as though it were just a Var.
 *	3.	If the list contains Vars of different relations that are known equal
 *		due to equivalence classes, then drop all but one of the Vars from each
 *		known-equal set, keeping the one with smallest estimated # of values
 *		(since the extra values of the others can't appear in joined rows).
 *		Note the reason we only consider Vars of different relations is that
 *		if we considered ones of the same rel, we'd be double-counting the
 *		restriction selectivity of the equality in the next step.
 *	4.	For Vars within a single source rel, we multiply together the numbers
 *		of values, clamp to the number of rows in the rel (divided by 10 if
 *		more than one Var), and then multiply by the selectivity of the
 *		restriction clauses for that rel.  When there's more than one Var,
 *		the initial product is probably too high (it's the worst case) but
 *		clamping to a fraction of the rel's rows seems to be a helpful
 *		heuristic for not letting the estimate get out of hand.  (The factor
 *		of 10 is derived from pre-Postgres-7.4 practice.)  Multiplying
 *		by the restriction selectivity is effectively assuming that the
 *		restriction clauses are independent of the grouping, which is a crummy
 *		assumption, but it's hard to do better.
 *	5.	If there are Vars from multiple rels, we repeat step 4 for each such
 *		rel, and multiply the results together.
 * Note that rels not containing grouped Vars are ignored completely, as are
 * join clauses.  Such rels cannot increase the number of groups, and we
 * assume such clauses do not reduce the number either (somewhat bogus,
 * but we don't have the info to do better).
 */
double
estimate_num_groups(PlannerInfo *root, List *groupExprs, double input_rows)
{
	List	   *varinfos = NIL;
	double		numdistinct;
	ListCell   *l;

	/*
	 * We don't ever want to return an estimate of zero groups, as that tends
	 * to lead to division-by-zero and other unpleasantness.  The input_rows
	 * estimate is usually already at least 1, but clamp it just in case it
	 * isn't.
	 */
	input_rows = clamp_row_est(input_rows);

	/*
	 * If no grouping columns, there's exactly one group.  (This can't happen
	 * for normal cases with GROUP BY or DISTINCT, but it is possible for
	 * corner cases with set operations.)
	 */
	if (groupExprs == NIL)
		return 1.0;

	/*
	 * Count groups derived from boolean grouping expressions.	For other
	 * expressions, find the unique Vars used, treating an expression as a Var
	 * if we can find stats for it.  For each one, record the statistical
	 * estimate of number of distinct values (total in its table, without
	 * regard for filtering).
	 */
	numdistinct = 1.0;

	foreach(l, groupExprs)
	{
		Node	   *groupexpr = (Node *) lfirst(l);
		VariableStatData vardata;
		List	   *varshere;
		ListCell   *l2;

		/* Short-circuit for expressions returning boolean */
		if (exprType(groupexpr) == BOOLOID)
		{
			numdistinct *= 2.0;
			continue;
		}

		/*
		 * If examine_variable is able to deduce anything about the GROUP BY
		 * expression, treat it as a single variable even if it's really more
		 * complicated.
		 */
		examine_variable(root, groupexpr, 0, &vardata);
		if (HeapTupleIsValid(vardata.statsTuple) || vardata.isunique)
		{
			varinfos = add_unique_group_var(root, varinfos,
											groupexpr, &vardata);
			ReleaseVariableStats(vardata);
			continue;
		}
		ReleaseVariableStats(vardata);

		/*
		 * Else pull out the component Vars.  Handle PlaceHolderVars by
		 * recursing into their arguments (effectively assuming that the
		 * PlaceHolderVar doesn't change the number of groups, which boils
		 * down to ignoring the possible addition of nulls to the result set).
		 */
		varshere = pull_var_clause(groupexpr,
								   PVC_RECURSE_AGGREGATES,
								   PVC_RECURSE_PLACEHOLDERS);

		/*
		 * If we find any variable-free GROUP BY item, then either it is a
		 * constant (and we can ignore it) or it contains a volatile function;
		 * in the latter case we punt and assume that each input row will
		 * yield a distinct group.
		 */
		if (varshere == NIL)
		{
			if (contain_volatile_functions(groupexpr))
				return input_rows;
			continue;
		}

		/*
		 * Else add variables to varinfos list
		 */
		foreach(l2, varshere)
		{
			Node	   *var = (Node *) lfirst(l2);

			examine_variable(root, var, 0, &vardata);
			varinfos = add_unique_group_var(root, varinfos, var, &vardata);
			ReleaseVariableStats(vardata);
		}
	}

	/*
	 * If now no Vars, we must have an all-constant or all-boolean GROUP BY
	 * list.
	 */
	if (varinfos == NIL)
	{
		/* Guard against out-of-range answers */
		if (numdistinct > input_rows)
			numdistinct = input_rows;
		return numdistinct;
	}

	/*
	 * Group Vars by relation and estimate total numdistinct.
	 *
	 * For each iteration of the outer loop, we process the frontmost Var in
	 * varinfos, plus all other Vars in the same relation.	We remove these
	 * Vars from the newvarinfos list for the next iteration. This is the
	 * easiest way to group Vars of same rel together.
	 */
	do
	{
		GroupVarInfo *varinfo1 = (GroupVarInfo *) linitial(varinfos);
		RelOptInfo *rel = varinfo1->rel;
		double		reldistinct = varinfo1->ndistinct;
		double		relmaxndistinct = reldistinct;
		int			relvarcount = 1;
		List	   *newvarinfos = NIL;

		/*
		 * Get the product of numdistinct estimates of the Vars for this rel.
		 * Also, construct new varinfos list of remaining Vars.
		 */
		for_each_cell(l, lnext(list_head(varinfos)))
		{
			GroupVarInfo *varinfo2 = (GroupVarInfo *) lfirst(l);

			if (varinfo2->rel == varinfo1->rel)
			{
				reldistinct *= varinfo2->ndistinct;
				if (relmaxndistinct < varinfo2->ndistinct)
					relmaxndistinct = varinfo2->ndistinct;
				relvarcount++;
			}
			else
			{
				/* not time to process varinfo2 yet */
				newvarinfos = lcons(varinfo2, newvarinfos);
			}
		}

		/*
		 * Sanity check --- don't divide by zero if empty relation.
		 */
		Assert(rel->reloptkind == RELOPT_BASEREL);
		if (rel->tuples > 0)
		{
			/*
			 * Clamp to size of rel, or size of rel / 10 if multiple Vars. The
			 * fudge factor is because the Vars are probably correlated but we
			 * don't know by how much.  We should never clamp to less than the
			 * largest ndistinct value for any of the Vars, though, since
			 * there will surely be at least that many groups.
			 */
			double		clamp = rel->tuples;

			if (relvarcount > 1)
			{
				clamp *= 0.1;
				if (clamp < relmaxndistinct)
				{
					clamp = relmaxndistinct;
					/* for sanity in case some ndistinct is too large: */
					if (clamp > rel->tuples)
						clamp = rel->tuples;
				}
			}
			if (reldistinct > clamp)
				reldistinct = clamp;

			/*
			 * Multiply by restriction selectivity.
			 */
			reldistinct *= rel->rows / rel->tuples;

			/*
			 * Update estimate of total distinct groups.
			 */
			numdistinct *= reldistinct;
		}

		varinfos = newvarinfos;
	} while (varinfos != NIL);

	numdistinct = ceil(numdistinct);

	/* Guard against out-of-range answers */
	if (numdistinct > input_rows)
		numdistinct = input_rows;
	if (numdistinct < 1.0)
		numdistinct = 1.0;

	return numdistinct;
}

/*
 * Estimate hash bucketsize fraction (ie, number of entries in a bucket
 * divided by total tuples in relation) if the specified expression is used
 * as a hash key.
 *
 * XXX This is really pretty bogus since we're effectively assuming that the
 * distribution of hash keys will be the same after applying restriction
 * clauses as it was in the underlying relation.  However, we are not nearly
 * smart enough to figure out how the restrict clauses might change the
 * distribution, so this will have to do for now.
 *
 * We are passed the number of buckets the executor will use for the given
 * input relation.	If the data were perfectly distributed, with the same
 * number of tuples going into each available bucket, then the bucketsize
 * fraction would be 1/nbuckets.  But this happy state of affairs will occur
 * only if (a) there are at least nbuckets distinct data values, and (b)
 * we have a not-too-skewed data distribution.	Otherwise the buckets will
 * be nonuniformly occupied.  If the other relation in the join has a key
 * distribution similar to this one's, then the most-loaded buckets are
 * exactly those that will be probed most often.  Therefore, the "average"
 * bucket size for costing purposes should really be taken as something close
 * to the "worst case" bucket size.  We try to estimate this by adjusting the
 * fraction if there are too few distinct data values, and then scaling up
 * by the ratio of the most common value's frequency to the average frequency.
 *
 * If no statistics are available, use a default estimate of 0.1.  This will
 * discourage use of a hash rather strongly if the inner relation is large,
 * which is what we want.  We do not want to hash unless we know that the
 * inner rel is well-dispersed (or the alternatives seem much worse).
 */
Selectivity
estimate_hash_bucketsize(PlannerInfo *root, Node *hashkey, double nbuckets)
{
	VariableStatData vardata;
	double		estfract,
				ndistinct,
				stanullfrac,
				mcvfreq,
				avgfreq;
	bool		isdefault;
	float4	   *numbers;
	int			nnumbers;

	examine_variable(root, hashkey, 0, &vardata);

	/* Get number of distinct values */
	ndistinct = get_variable_numdistinct(&vardata, &isdefault);

	/* If ndistinct isn't real, punt and return 0.1, per comments above */
	if (isdefault)
	{
		ReleaseVariableStats(vardata);
		return (Selectivity) 0.1;
	}

	/* Get fraction that are null */
	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		stanullfrac = stats->stanullfrac;
	}
	else
		stanullfrac = 0.0;

	/* Compute avg freq of all distinct data values in raw relation */
	avgfreq = (1.0 - stanullfrac) / ndistinct;

	/*
	 * Adjust ndistinct to account for restriction clauses.  Observe we are
	 * assuming that the data distribution is affected uniformly by the
	 * restriction clauses!
	 *
	 * XXX Possibly better way, but much more expensive: multiply by
	 * selectivity of rel's restriction clauses that mention the target Var.
	 */
	if (vardata.rel)
		ndistinct *= vardata.rel->rows / vardata.rel->tuples;

	/*
	 * Initial estimate of bucketsize fraction is 1/nbuckets as long as the
	 * number of buckets is less than the expected number of distinct values;
	 * otherwise it is 1/ndistinct.
	 */
	if (ndistinct > nbuckets)
		estfract = 1.0 / nbuckets;
	else
		estfract = 1.0 / ndistinct;

	/*
	 * Look up the frequency of the most common value, if available.
	 */
	mcvfreq = 0.0;

	if (HeapTupleIsValid(vardata.statsTuple))
	{
		if (get_attstatsslot(vardata.statsTuple,
							 vardata.atttype, vardata.atttypmod,
							 STATISTIC_KIND_MCV, InvalidOid,
							 NULL,
							 NULL, NULL,
							 &numbers, &nnumbers))
		{
			/*
			 * The first MCV stat is for the most common value.
			 */
			if (nnumbers > 0)
				mcvfreq = numbers[0];
			free_attstatsslot(vardata.atttype, NULL, 0,
							  numbers, nnumbers);
		}
	}

	/*
	 * Adjust estimated bucketsize upward to account for skewed distribution.
	 */
	if (avgfreq > 0.0 && mcvfreq > avgfreq)
		estfract *= mcvfreq / avgfreq;

	/*
	 * Clamp bucketsize to sane range (the above adjustment could easily
	 * produce an out-of-range result).  We set the lower bound a little above
	 * zero, since zero isn't a very sane result.
	 */
	if (estfract < 1.0e-6)
		estfract = 1.0e-6;
	else if (estfract > 1.0)
		estfract = 1.0;

	ReleaseVariableStats(vardata);

	return (Selectivity) estfract;
}


/*-------------------------------------------------------------------------
 *
 * Support routines
 *
 *-------------------------------------------------------------------------
 */

/*
 * convert_to_scalar
 *	  Convert non-NULL values of the indicated types to the comparison
 *	  scale needed by scalarineqsel().
 *	  Returns "true" if successful.
 *
 * XXX this routine is a hack: ideally we should look up the conversion
 * subroutines in pg_type.
 *
 * All numeric datatypes are simply converted to their equivalent
 * "double" values.  (NUMERIC values that are outside the range of "double"
 * are clamped to +/- HUGE_VAL.)
 *
 * String datatypes are converted by convert_string_to_scalar(),
 * which is explained below.  The reason why this routine deals with
 * three values at a time, not just one, is that we need it for strings.
 *
 * The bytea datatype is just enough different from strings that it has
 * to be treated separately.
 *
 * The several datatypes representing absolute times are all converted
 * to Timestamp, which is actually a double, and then we just use that
 * double value.  Note this will give correct results even for the "special"
 * values of Timestamp, since those are chosen to compare correctly;
 * see timestamp_cmp.
 *
 * The several datatypes representing relative times (intervals) are all
 * converted to measurements expressed in seconds.
 */
static bool
convert_to_scalar(Datum value, Oid valuetypid, double *scaledvalue,
				  Datum lobound, Datum hibound, Oid boundstypid,
				  double *scaledlobound, double *scaledhibound)
{
	/*
	 * Both the valuetypid and the boundstypid should exactly match the
	 * declared input type(s) of the operator we are invoked for, so we just
	 * error out if either is not recognized.
	 *
	 * XXX The histogram we are interpolating between points of could belong
	 * to a column that's only binary-compatible with the declared type. In
	 * essence we are assuming that the semantics of binary-compatible types
	 * are enough alike that we can use a histogram generated with one type's
	 * operators to estimate selectivity for the other's.  This is outright
	 * wrong in some cases --- in particular signed versus unsigned
	 * interpretation could trip us up.  But it's useful enough in the
	 * majority of cases that we do it anyway.	Should think about more
	 * rigorous ways to do it.
	 */
	switch (valuetypid)
	{
			/*
			 * Built-in numeric types
			 */
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
		case OIDOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGCLASSOID:
		case REGTYPEOID:
		case REGCONFIGOID:
		case REGDICTIONARYOID:
			*scaledvalue = convert_numeric_to_scalar(value, valuetypid);
			*scaledlobound = convert_numeric_to_scalar(lobound, boundstypid);
			*scaledhibound = convert_numeric_to_scalar(hibound, boundstypid);
			return true;

			/*
			 * Built-in string types
			 */
		case CHAROID:
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
		case NAMEOID:
			{
				char	   *valstr = convert_string_datum(value, valuetypid);
				char	   *lostr = convert_string_datum(lobound, boundstypid);
				char	   *histr = convert_string_datum(hibound, boundstypid);

				convert_string_to_scalar(valstr, scaledvalue,
										 lostr, scaledlobound,
										 histr, scaledhibound);
				pfree(valstr);
				pfree(lostr);
				pfree(histr);
				return true;
			}

			/*
			 * Built-in bytea type
			 */
		case BYTEAOID:
			{
				convert_bytea_to_scalar(value, scaledvalue,
										lobound, scaledlobound,
										hibound, scaledhibound);
				return true;
			}

			/*
			 * Built-in time types
			 */
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case ABSTIMEOID:
		case DATEOID:
		case INTERVALOID:
		case RELTIMEOID:
		case TINTERVALOID:
		case TIMEOID:
		case TIMETZOID:
			*scaledvalue = convert_timevalue_to_scalar(value, valuetypid);
			*scaledlobound = convert_timevalue_to_scalar(lobound, boundstypid);
			*scaledhibound = convert_timevalue_to_scalar(hibound, boundstypid);
			return true;

			/*
			 * Built-in network types
			 */
		case INETOID:
		case CIDROID:
		case MACADDROID:
			*scaledvalue = convert_network_to_scalar(value, valuetypid);
			*scaledlobound = convert_network_to_scalar(lobound, boundstypid);
			*scaledhibound = convert_network_to_scalar(hibound, boundstypid);
			return true;
	}
	/* Don't know how to convert */
	*scaledvalue = *scaledlobound = *scaledhibound = 0;
	return false;
}

/*
 * Do convert_to_scalar()'s work for any numeric data type.
 */
static double
convert_numeric_to_scalar(Datum value, Oid typid)
{
	switch (typid)
	{
		case BOOLOID:
			return (double) DatumGetBool(value);
		case INT2OID:
			return (double) DatumGetInt16(value);
		case INT4OID:
			return (double) DatumGetInt32(value);
		case INT8OID:
			return (double) DatumGetInt64(value);
		case FLOAT4OID:
			return (double) DatumGetFloat4(value);
		case FLOAT8OID:
			return (double) DatumGetFloat8(value);
		case NUMERICOID:
			/* Note: out-of-range values will be clamped to +-HUGE_VAL */
			return (double)
				DatumGetFloat8(DirectFunctionCall1(numeric_float8_no_overflow,
												   value));
		case OIDOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGCLASSOID:
		case REGTYPEOID:
		case REGCONFIGOID:
		case REGDICTIONARYOID:
			/* we can treat OIDs as integers... */
			return (double) DatumGetObjectId(value);
	}

	/*
	 * Can't get here unless someone tries to use scalarltsel/scalargtsel on
	 * an operator with one numeric and one non-numeric operand.
	 */
	elog(ERROR, "unsupported type: %u", typid);
	return 0;
}

/*
 * Do convert_to_scalar()'s work for any character-string data type.
 *
 * String datatypes are converted to a scale that ranges from 0 to 1,
 * where we visualize the bytes of the string as fractional digits.
 *
 * We do not want the base to be 256, however, since that tends to
 * generate inflated selectivity estimates; few databases will have
 * occurrences of all 256 possible byte values at each position.
 * Instead, use the smallest and largest byte values seen in the bounds
 * as the estimated range for each byte, after some fudging to deal with
 * the fact that we probably aren't going to see the full range that way.
 *
 * An additional refinement is that we discard any common prefix of the
 * three strings before computing the scaled values.  This allows us to
 * "zoom in" when we encounter a narrow data range.  An example is a phone
 * number database where all the values begin with the same area code.
 * (Actually, the bounds will be adjacent histogram-bin-boundary values,
 * so this is more likely to happen than you might think.)
 */
static void
convert_string_to_scalar(char *value,
						 double *scaledvalue,
						 char *lobound,
						 double *scaledlobound,
						 char *hibound,
						 double *scaledhibound)
{
	int			rangelo,
				rangehi;
	char	   *sptr;

	rangelo = rangehi = (unsigned char) hibound[0];
	for (sptr = lobound; *sptr; sptr++)
	{
		if (rangelo > (unsigned char) *sptr)
			rangelo = (unsigned char) *sptr;
		if (rangehi < (unsigned char) *sptr)
			rangehi = (unsigned char) *sptr;
	}
	for (sptr = hibound; *sptr; sptr++)
	{
		if (rangelo > (unsigned char) *sptr)
			rangelo = (unsigned char) *sptr;
		if (rangehi < (unsigned char) *sptr)
			rangehi = (unsigned char) *sptr;
	}
	/* If range includes any upper-case ASCII chars, make it include all */
	if (rangelo <= 'Z' && rangehi >= 'A')
	{
		if (rangelo > 'A')
			rangelo = 'A';
		if (rangehi < 'Z')
			rangehi = 'Z';
	}
	/* Ditto lower-case */
	if (rangelo <= 'z' && rangehi >= 'a')
	{
		if (rangelo > 'a')
			rangelo = 'a';
		if (rangehi < 'z')
			rangehi = 'z';
	}
	/* Ditto digits */
	if (rangelo <= '9' && rangehi >= '0')
	{
		if (rangelo > '0')
			rangelo = '0';
		if (rangehi < '9')
			rangehi = '9';
	}

	/*
	 * If range includes less than 10 chars, assume we have not got enough
	 * data, and make it include regular ASCII set.
	 */
	if (rangehi - rangelo < 9)
	{
		rangelo = ' ';
		rangehi = 127;
	}

	/*
	 * Now strip any common prefix of the three strings.
	 */
	while (*lobound)
	{
		if (*lobound != *hibound || *lobound != *value)
			break;
		lobound++, hibound++, value++;
	}

	/*
	 * Now we can do the conversions.
	 */
	*scaledvalue = convert_one_string_to_scalar(value, rangelo, rangehi);
	*scaledlobound = convert_one_string_to_scalar(lobound, rangelo, rangehi);
	*scaledhibound = convert_one_string_to_scalar(hibound, rangelo, rangehi);
}

static double
convert_one_string_to_scalar(char *value, int rangelo, int rangehi)
{
	int			slen = strlen(value);
	double		num,
				denom,
				base;

	if (slen <= 0)
		return 0.0;				/* empty string has scalar value 0 */

	/*
	 * Since base is at least 10, need not consider more than about 20 chars
	 */
	if (slen > 20)
		slen = 20;

	/* Convert initial characters to fraction */
	base = rangehi - rangelo + 1;
	num = 0.0;
	denom = base;
	while (slen-- > 0)
	{
		int			ch = (unsigned char) *value++;

		if (ch < rangelo)
			ch = rangelo - 1;
		else if (ch > rangehi)
			ch = rangehi + 1;
		num += ((double) (ch - rangelo)) / denom;
		denom *= base;
	}

	return num;
}

/*
 * Convert a string-type Datum into a palloc'd, null-terminated string.
 *
 * When using a non-C locale, we must pass the string through strxfrm()
 * before continuing, so as to generate correct locale-specific results.
 */
static char *
convert_string_datum(Datum value, Oid typid)
{
	char	   *val;

	switch (typid)
	{
		case CHAROID:
			val = (char *) palloc(2);
			val[0] = DatumGetChar(value);
			val[1] = '\0';
			break;
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
			val = TextDatumGetCString(value);
			break;
		case NAMEOID:
			{
				NameData   *nm = (NameData *) DatumGetPointer(value);

				val = pstrdup(NameStr(*nm));
				break;
			}
		default:

			/*
			 * Can't get here unless someone tries to use scalarltsel on an
			 * operator with one string and one non-string operand.
			 */
			elog(ERROR, "unsupported type: %u", typid);
			return NULL;
	}

	if (!lc_collate_is_c(DEFAULT_COLLATION_OID))
	{
		char	   *xfrmstr;
		size_t		xfrmlen;
		size_t xfrmlen2 PG_USED_FOR_ASSERTS_ONLY;

		/*
		 * Note: originally we guessed at a suitable output buffer size, and
		 * only needed to call strxfrm twice if our guess was too small.
		 * However, it seems that some versions of Solaris have buggy strxfrm
		 * that can write past the specified buffer length in that scenario.
		 * So, do it the dumb way for portability.
		 *
		 * Yet other systems (e.g., glibc) sometimes return a smaller value
		 * from the second call than the first; thus the Assert must be <= not
		 * == as you'd expect.  Can't any of these people program their way
		 * out of a paper bag?
		 *
		 * XXX: strxfrm doesn't support UTF-8 encoding on Win32, it can return
		 * bogus data or set an error. This is not really a problem unless it
		 * crashes since it will only give an estimation error and nothing
		 * fatal.
		 */
#if _MSC_VER == 1400			/* VS.Net 2005 */

		/*
		 *
		 * http://connect.microsoft.com/VisualStudio/feedback/ViewFeedback.aspx?
		 * FeedbackID=99694 */
		{
			char		x[1];

			xfrmlen = strxfrm(x, val, 0);
		}
#else
		xfrmlen = strxfrm(NULL, val, 0);
#endif
#ifdef WIN32

		/*
		 * On Windows, strxfrm returns INT_MAX when an error occurs. Instead
		 * of trying to allocate this much memory (and fail), just return the
		 * original string unmodified as if we were in the C locale.
		 */
		if (xfrmlen == INT_MAX)
			return val;
#endif
		xfrmstr = (char *) palloc(xfrmlen + 1);
		xfrmlen2 = strxfrm(xfrmstr, val, xfrmlen + 1);
		Assert(xfrmlen2 <= xfrmlen);
		pfree(val);
		val = xfrmstr;
	}

	return val;
}

/*
 * Do convert_to_scalar()'s work for any bytea data type.
 *
 * Very similar to convert_string_to_scalar except we can't assume
 * null-termination and therefore pass explicit lengths around.
 *
 * Also, assumptions about likely "normal" ranges of characters have been
 * removed - a data range of 0..255 is always used, for now.  (Perhaps
 * someday we will add information about actual byte data range to
 * pg_statistic.)
 */
static void
convert_bytea_to_scalar(Datum value,
						double *scaledvalue,
						Datum lobound,
						double *scaledlobound,
						Datum hibound,
						double *scaledhibound)
{
	int			rangelo,
				rangehi,
				valuelen = VARSIZE(DatumGetPointer(value)) - VARHDRSZ,
				loboundlen = VARSIZE(DatumGetPointer(lobound)) - VARHDRSZ,
				hiboundlen = VARSIZE(DatumGetPointer(hibound)) - VARHDRSZ,
				i,
				minlen;
	unsigned char *valstr = (unsigned char *) VARDATA(DatumGetPointer(value)),
			   *lostr = (unsigned char *) VARDATA(DatumGetPointer(lobound)),
			   *histr = (unsigned char *) VARDATA(DatumGetPointer(hibound));

	/*
	 * Assume bytea data is uniformly distributed across all byte values.
	 */
	rangelo = 0;
	rangehi = 255;

	/*
	 * Now strip any common prefix of the three strings.
	 */
	minlen = Min(Min(valuelen, loboundlen), hiboundlen);
	for (i = 0; i < minlen; i++)
	{
		if (*lostr != *histr || *lostr != *valstr)
			break;
		lostr++, histr++, valstr++;
		loboundlen--, hiboundlen--, valuelen--;
	}

	/*
	 * Now we can do the conversions.
	 */
	*scaledvalue = convert_one_bytea_to_scalar(valstr, valuelen, rangelo, rangehi);
	*scaledlobound = convert_one_bytea_to_scalar(lostr, loboundlen, rangelo, rangehi);
	*scaledhibound = convert_one_bytea_to_scalar(histr, hiboundlen, rangelo, rangehi);
}

static double
convert_one_bytea_to_scalar(unsigned char *value, int valuelen,
							int rangelo, int rangehi)
{
	double		num,
				denom,
				base;

	if (valuelen <= 0)
		return 0.0;				/* empty string has scalar value 0 */

	/*
	 * Since base is 256, need not consider more than about 10 chars (even
	 * this many seems like overkill)
	 */
	if (valuelen > 10)
		valuelen = 10;

	/* Convert initial characters to fraction */
	base = rangehi - rangelo + 1;
	num = 0.0;
	denom = base;
	while (valuelen-- > 0)
	{
		int			ch = *value++;

		if (ch < rangelo)
			ch = rangelo - 1;
		else if (ch > rangehi)
			ch = rangehi + 1;
		num += ((double) (ch - rangelo)) / denom;
		denom *= base;
	}

	return num;
}

/*
 * Do convert_to_scalar()'s work for any timevalue data type.
 */
static double
convert_timevalue_to_scalar(Datum value, Oid typid)
{
	switch (typid)
	{
		case TIMESTAMPOID:
			return DatumGetTimestamp(value);
		case TIMESTAMPTZOID:
			return DatumGetTimestampTz(value);
		case ABSTIMEOID:
			return DatumGetTimestamp(DirectFunctionCall1(abstime_timestamp,
														 value));
		case DATEOID:
			return date2timestamp_no_overflow(DatumGetDateADT(value));
		case INTERVALOID:
			{
				Interval   *interval = DatumGetIntervalP(value);

				/*
				 * Convert the month part of Interval to days using assumed
				 * average month length of 365.25/12.0 days.  Not too
				 * accurate, but plenty good enough for our purposes.
				 */
#ifdef HAVE_INT64_TIMESTAMP
				return interval->time + interval->day * (double) USECS_PER_DAY +
					interval->month * ((DAYS_PER_YEAR / (double) MONTHS_PER_YEAR) * USECS_PER_DAY);
#else
				return interval->time + interval->day * SECS_PER_DAY +
					interval->month * ((DAYS_PER_YEAR / (double) MONTHS_PER_YEAR) * (double) SECS_PER_DAY);
#endif
			}
		case RELTIMEOID:
#ifdef HAVE_INT64_TIMESTAMP
			return (DatumGetRelativeTime(value) * 1000000.0);
#else
			return DatumGetRelativeTime(value);
#endif
		case TINTERVALOID:
			{
				TimeInterval tinterval = DatumGetTimeInterval(value);

#ifdef HAVE_INT64_TIMESTAMP
				if (tinterval->status != 0)
					return ((tinterval->data[1] - tinterval->data[0]) * 1000000.0);
#else
				if (tinterval->status != 0)
					return tinterval->data[1] - tinterval->data[0];
#endif
				return 0;		/* for lack of a better idea */
			}
		case TIMEOID:
			return DatumGetTimeADT(value);
		case TIMETZOID:
			{
				TimeTzADT  *timetz = DatumGetTimeTzADTP(value);

				/* use GMT-equivalent time */
#ifdef HAVE_INT64_TIMESTAMP
				return (double) (timetz->time + (timetz->zone * 1000000.0));
#else
				return (double) (timetz->time + timetz->zone);
#endif
			}
	}

	/*
	 * Can't get here unless someone tries to use scalarltsel/scalargtsel on
	 * an operator with one timevalue and one non-timevalue operand.
	 */
	elog(ERROR, "unsupported type: %u", typid);
	return 0;
}


/*
 * get_restriction_variable
 *		Examine the args of a restriction clause to see if it's of the
 *		form (variable op pseudoconstant) or (pseudoconstant op variable),
 *		where "variable" could be either a Var or an expression in vars of a
 *		single relation.  If so, extract information about the variable,
 *		and also indicate which side it was on and the other argument.
 *
 * Inputs:
 *	root: the planner info
 *	args: clause argument list
 *	varRelid: see specs for restriction selectivity functions
 *
 * Outputs: (these are valid only if TRUE is returned)
 *	*vardata: gets information about variable (see examine_variable)
 *	*other: gets other clause argument, aggressively reduced to a constant
 *	*varonleft: set TRUE if variable is on the left, FALSE if on the right
 *
 * Returns TRUE if a variable is identified, otherwise FALSE.
 *
 * Note: if there are Vars on both sides of the clause, we must fail, because
 * callers are expecting that the other side will act like a pseudoconstant.
 */
bool
get_restriction_variable(PlannerInfo *root, List *args, int varRelid,
						 VariableStatData *vardata, Node **other,
						 bool *varonleft)
{
	Node	   *left,
			   *right;
	VariableStatData rdata;

	/* Fail if not a binary opclause (probably shouldn't happen) */
	if (list_length(args) != 2)
		return false;

	left = (Node *) linitial(args);
	right = (Node *) lsecond(args);

	/*
	 * Examine both sides.	Note that when varRelid is nonzero, Vars of other
	 * relations will be treated as pseudoconstants.
	 */
	examine_variable(root, left, varRelid, vardata);
	examine_variable(root, right, varRelid, &rdata);

	/*
	 * If one side is a variable and the other not, we win.
	 */
	if (vardata->rel && rdata.rel == NULL)
	{
		*varonleft = true;
		*other = estimate_expression_value(root, rdata.var);
		/* Assume we need no ReleaseVariableStats(rdata) here */
		return true;
	}

	if (vardata->rel == NULL && rdata.rel)
	{
		*varonleft = false;
		*other = estimate_expression_value(root, vardata->var);
		/* Assume we need no ReleaseVariableStats(*vardata) here */
		*vardata = rdata;
		return true;
	}

	/* Ooops, clause has wrong structure (probably var op var) */
	ReleaseVariableStats(*vardata);
	ReleaseVariableStats(rdata);

	return false;
}

/*
 * get_join_variables
 *		Apply examine_variable() to each side of a join clause.
 *		Also, attempt to identify whether the join clause has the same
 *		or reversed sense compared to the SpecialJoinInfo.
 *
 * We consider the join clause "normal" if it is "lhs_var OP rhs_var",
 * or "reversed" if it is "rhs_var OP lhs_var".  In complicated cases
 * where we can't tell for sure, we default to assuming it's normal.
 */
void
get_join_variables(PlannerInfo *root, List *args, SpecialJoinInfo *sjinfo,
				   VariableStatData *vardata1, VariableStatData *vardata2,
				   bool *join_is_reversed)
{
	Node	   *left,
			   *right;

	if (list_length(args) != 2)
		elog(ERROR, "join operator should take two arguments");

	left = (Node *) linitial(args);
	right = (Node *) lsecond(args);

	examine_variable(root, left, 0, vardata1);
	examine_variable(root, right, 0, vardata2);

	if (vardata1->rel &&
		bms_is_subset(vardata1->rel->relids, sjinfo->syn_righthand))
		*join_is_reversed = true;		/* var1 is on RHS */
	else if (vardata2->rel &&
			 bms_is_subset(vardata2->rel->relids, sjinfo->syn_lefthand))
		*join_is_reversed = true;		/* var2 is on LHS */
	else
		*join_is_reversed = false;
}

/*
 * examine_variable
 *		Try to look up statistical data about an expression.
 *		Fill in a VariableStatData struct to describe the expression.
 *
 * Inputs:
 *	root: the planner info
 *	node: the expression tree to examine
 *	varRelid: see specs for restriction selectivity functions
 *
 * Outputs: *vardata is filled as follows:
 *	var: the input expression (with any binary relabeling stripped, if
 *		it is or contains a variable; but otherwise the type is preserved)
 *	rel: RelOptInfo for relation containing variable; NULL if expression
 *		contains no Vars (NOTE this could point to a RelOptInfo of a
 *		subquery, not one in the current query).
 *	statsTuple: the pg_statistic entry for the variable, if one exists;
 *		otherwise NULL.
 *	freefunc: pointer to a function to release statsTuple with.
 *	vartype: exposed type of the expression; this should always match
 *		the declared input type of the operator we are estimating for.
 *	atttype, atttypmod: type data to pass to get_attstatsslot().  This is
 *		commonly the same as the exposed type of the variable argument,
 *		but can be different in binary-compatible-type cases.
 *	isunique: TRUE if we were able to match the var to a unique index or a
 *		single-column DISTINCT clause, implying its values are unique for
 *		this query.  (Caution: this should be trusted for statistical
 *		purposes only, since we do not check indimmediate nor verify that
 *		the exact same definition of equality applies.)
 *
 * Caller is responsible for doing ReleaseVariableStats() before exiting.
 */
void
examine_variable(PlannerInfo *root, Node *node, int varRelid,
				 VariableStatData *vardata)
{
	Node	   *basenode;
	Relids		varnos;
	RelOptInfo *onerel;

	/* Make sure we don't return dangling pointers in vardata */
	MemSet(vardata, 0, sizeof(VariableStatData));

	/* Save the exposed type of the expression */
	vardata->vartype = exprType(node);

	/* Look inside any binary-compatible relabeling */

	if (IsA(node, RelabelType))
		basenode = (Node *) ((RelabelType *) node)->arg;
	else
		basenode = node;

	/* Fast path for a simple Var */

	if (IsA(basenode, Var) &&
		(varRelid == 0 || varRelid == ((Var *) basenode)->varno))
	{
		Var		   *var = (Var *) basenode;

		/* Set up result fields other than the stats tuple */
		vardata->var = basenode;	/* return Var without relabeling */
		vardata->rel = find_base_rel(root, var->varno);
		vardata->atttype = var->vartype;
		vardata->atttypmod = var->vartypmod;
		vardata->isunique = has_unique_index(vardata->rel, var->varattno);

		/* Try to locate some stats */
		examine_simple_variable(root, var, vardata);

		return;
	}

	/*
	 * Okay, it's a more complicated expression.  Determine variable
	 * membership.	Note that when varRelid isn't zero, only vars of that
	 * relation are considered "real" vars.
	 */
	varnos = pull_varnos(basenode);

	onerel = NULL;

	switch (bms_membership(varnos))
	{
		case BMS_EMPTY_SET:
			/* No Vars at all ... must be pseudo-constant clause */
			break;
		case BMS_SINGLETON:
			if (varRelid == 0 || bms_is_member(varRelid, varnos))
			{
				onerel = find_base_rel(root,
					   (varRelid ? varRelid : bms_singleton_member(varnos)));
				vardata->rel = onerel;
				node = basenode;	/* strip any relabeling */
			}
			/* else treat it as a constant */
			break;
		case BMS_MULTIPLE:
			if (varRelid == 0)
			{
				/* treat it as a variable of a join relation */
				vardata->rel = find_join_rel(root, varnos);
				node = basenode;	/* strip any relabeling */
			}
			else if (bms_is_member(varRelid, varnos))
			{
				/* ignore the vars belonging to other relations */
				vardata->rel = find_base_rel(root, varRelid);
				node = basenode;	/* strip any relabeling */
				/* note: no point in expressional-index search here */
			}
			/* else treat it as a constant */
			break;
	}

	bms_free(varnos);

	vardata->var = node;
	vardata->atttype = exprType(node);
	vardata->atttypmod = exprTypmod(node);

	if (onerel)
	{
		/*
		 * We have an expression in vars of a single relation.	Try to match
		 * it to expressional index columns, in hopes of finding some
		 * statistics.
		 *
		 * XXX it's conceivable that there are multiple matches with different
		 * index opfamilies; if so, we need to pick one that matches the
		 * operator we are estimating for.	FIXME later.
		 */
		ListCell   *ilist;

		foreach(ilist, onerel->indexlist)
		{
			IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);
			ListCell   *indexpr_item;
			int			pos;

			indexpr_item = list_head(index->indexprs);
			if (indexpr_item == NULL)
				continue;		/* no expressions here... */

			for (pos = 0; pos < index->ncolumns; pos++)
			{
				if (index->indexkeys[pos] == 0)
				{
					Node	   *indexkey;

					if (indexpr_item == NULL)
						elog(ERROR, "too few entries in indexprs list");
					indexkey = (Node *) lfirst(indexpr_item);
					if (indexkey && IsA(indexkey, RelabelType))
						indexkey = (Node *) ((RelabelType *) indexkey)->arg;
					if (equal(node, indexkey))
					{
						/*
						 * Found a match ... is it a unique index? Tests here
						 * should match has_unique_index().
						 */
						if (index->unique &&
							index->ncolumns == 1 &&
							(index->indpred == NIL || index->predOK))
							vardata->isunique = true;

						/*
						 * Has it got stats?  We only consider stats for
						 * non-partial indexes, since partial indexes probably
						 * don't reflect whole-relation statistics; the above
						 * check for uniqueness is the only info we take from
						 * a partial index.
						 *
						 * An index stats hook, however, must make its own
						 * decisions about what to do with partial indexes.
						 */
						if (get_index_stats_hook &&
							(*get_index_stats_hook) (root, index->indexoid,
													 pos + 1, vardata))
						{
							/*
							 * The hook took control of acquiring a stats
							 * tuple.  If it did supply a tuple, it'd better
							 * have supplied a freefunc.
							 */
							if (HeapTupleIsValid(vardata->statsTuple) &&
								!vardata->freefunc)
								elog(ERROR, "no function provided to release variable stats with");
						}
						else if (index->indpred == NIL)
						{
							vardata->statsTuple =
								SearchSysCache3(STATRELATTINH,
										   ObjectIdGetDatum(index->indexoid),
												Int16GetDatum(pos + 1),
												BoolGetDatum(false));
							vardata->freefunc = ReleaseSysCache;
						}
						if (vardata->statsTuple)
							break;
					}
					indexpr_item = lnext(indexpr_item);
				}
			}
			if (vardata->statsTuple)
				break;
		}
	}
}

/*
 * examine_simple_variable
 *		Handle a simple Var for examine_variable
 *
 * This is split out as a subroutine so that we can recurse to deal with
 * Vars referencing subqueries.
 *
 * We already filled in all the fields of *vardata except for the stats tuple.
 */
static void
examine_simple_variable(PlannerInfo *root, Var *var,
						VariableStatData *vardata)
{
	RangeTblEntry *rte = root->simple_rte_array[var->varno];

	Assert(IsA(rte, RangeTblEntry));

	if (get_relation_stats_hook &&
		(*get_relation_stats_hook) (root, rte, var->varattno, vardata))
	{
		/*
		 * The hook took control of acquiring a stats tuple.  If it did supply
		 * a tuple, it'd better have supplied a freefunc.
		 */
		if (HeapTupleIsValid(vardata->statsTuple) &&
			!vardata->freefunc)
			elog(ERROR, "no function provided to release variable stats with");
	}
	else if (rte->rtekind == RTE_RELATION)
	{
		/*
		 * Plain table or parent of an inheritance appendrel, so look up the
		 * column in pg_statistic
		 */
		vardata->statsTuple = SearchSysCache3(STATRELATTINH,
											  ObjectIdGetDatum(rte->relid),
											  Int16GetDatum(var->varattno),
											  BoolGetDatum(rte->inh));
		vardata->freefunc = ReleaseSysCache;
	}
	else if (rte->rtekind == RTE_SUBQUERY && !rte->inh)
	{
		/*
		 * Plain subquery (not one that was converted to an appendrel).
		 */
		Query	   *subquery = rte->subquery;
		RelOptInfo *rel;
		TargetEntry *ste;

		/*
		 * Punt if subquery uses set operations or GROUP BY, as these will
		 * mash underlying columns' stats beyond recognition.  (Set ops are
		 * particularly nasty; if we forged ahead, we would return stats
		 * relevant to only the leftmost subselect...)	DISTINCT is also
		 * problematic, but we check that later because there is a possibility
		 * of learning something even with it.
		 */
		if (subquery->setOperations ||
			subquery->groupClause)
			return;

		/*
		 * OK, fetch RelOptInfo for subquery.  Note that we don't change the
		 * rel returned in vardata, since caller expects it to be a rel of the
		 * caller's query level.  Because we might already be recursing, we
		 * can't use that rel pointer either, but have to look up the Var's
		 * rel afresh.
		 */
		rel = find_base_rel(root, var->varno);

		/* If the subquery hasn't been planned yet, we have to punt */
		if (rel->subroot == NULL)
			return;
		Assert(IsA(rel->subroot, PlannerInfo));

		/*
		 * Switch our attention to the subquery as mangled by the planner. It
		 * was okay to look at the pre-planning version for the tests above,
		 * but now we need a Var that will refer to the subroot's live
		 * RelOptInfos.  For instance, if any subquery pullup happened during
		 * planning, Vars in the targetlist might have gotten replaced, and we
		 * need to see the replacement expressions.
		 */
		subquery = rel->subroot->parse;
		Assert(IsA(subquery, Query));

		/* Get the subquery output expression referenced by the upper Var */
		ste = get_tle_by_resno(subquery->targetList, var->varattno);
		if (ste == NULL || ste->resjunk)
			elog(ERROR, "subquery %s does not have attribute %d",
				 rte->eref->aliasname, var->varattno);
		var = (Var *) ste->expr;

		/*
		 * If subquery uses DISTINCT, we can't make use of any stats for the
		 * variable ... but, if it's the only DISTINCT column, we are entitled
		 * to consider it unique.  We do the test this way so that it works
		 * for cases involving DISTINCT ON.
		 */
		if (subquery->distinctClause)
		{
			if (list_length(subquery->distinctClause) == 1 &&
				targetIsInSortList(ste, InvalidOid, subquery->distinctClause))
				vardata->isunique = true;
			/* cannot go further */
			return;
		}

		/*
		 * If the sub-query originated from a view with the security_barrier
		 * attribute, we must not look at the variable's statistics, though it
		 * seems all right to notice the existence of a DISTINCT clause. So
		 * stop here.
		 *
		 * This is probably a harsher restriction than necessary; it's
		 * certainly OK for the selectivity estimator (which is a C function,
		 * and therefore omnipotent anyway) to look at the statistics.	But
		 * many selectivity estimators will happily *invoke the operator
		 * function* to try to work out a good estimate - and that's not OK.
		 * So for now, don't dig down for stats.
		 */
		if (rte->security_barrier)
			return;

		/* Can only handle a simple Var of subquery's query level */
		if (var && IsA(var, Var) &&
			var->varlevelsup == 0)
		{
			/*
			 * OK, recurse into the subquery.  Note that the original setting
			 * of vardata->isunique (which will surely be false) is left
			 * unchanged in this situation.  That's what we want, since even
			 * if the underlying column is unique, the subquery may have
			 * joined to other tables in a way that creates duplicates.
			 */
			examine_simple_variable(rel->subroot, var, vardata);
		}
	}
	else
	{
		/*
		 * Otherwise, the Var comes from a FUNCTION, VALUES, or CTE RTE.  (We
		 * won't see RTE_JOIN here because join alias Vars have already been
		 * flattened.)	There's not much we can do with function outputs, but
		 * maybe someday try to be smarter about VALUES and/or CTEs.
		 */
	}
}

/*
 * get_variable_numdistinct
 *	  Estimate the number of distinct values of a variable.
 *
 * vardata: results of examine_variable
 * *isdefault: set to TRUE if the result is a default rather than based on
 * anything meaningful.
 *
 * NB: be careful to produce an integral result, since callers may compare
 * the result to exact integer counts.
 */
double
get_variable_numdistinct(VariableStatData *vardata, bool *isdefault)
{
	double		stadistinct;
	double		ntuples;

	*isdefault = false;

	/*
	 * Determine the stadistinct value to use.	There are cases where we can
	 * get an estimate even without a pg_statistic entry, or can get a better
	 * value than is in pg_statistic.
	 */
	if (HeapTupleIsValid(vardata->statsTuple))
	{
		/* Use the pg_statistic entry */
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
		stadistinct = stats->stadistinct;
	}
	else if (vardata->vartype == BOOLOID)
	{
		/*
		 * Special-case boolean columns: presumably, two distinct values.
		 *
		 * Are there any other datatypes we should wire in special estimates
		 * for?
		 */
		stadistinct = 2.0;
	}
	else
	{
		/*
		 * We don't keep statistics for system columns, but in some cases we
		 * can infer distinctness anyway.
		 */
		if (vardata->var && IsA(vardata->var, Var))
		{
			switch (((Var *) vardata->var)->varattno)
			{
				case ObjectIdAttributeNumber:
				case SelfItemPointerAttributeNumber:
					stadistinct = -1.0; /* unique */
					break;
				case TableOidAttributeNumber:
					stadistinct = 1.0;	/* only 1 value */
					break;
				default:
					stadistinct = 0.0;	/* means "unknown" */
					break;
			}
		}
		else
			stadistinct = 0.0;	/* means "unknown" */

		/*
		 * XXX consider using estimate_num_groups on expressions?
		 */
	}

	/*
	 * If there is a unique index or DISTINCT clause for the variable, assume
	 * it is unique no matter what pg_statistic says; the statistics could be
	 * out of date, or we might have found a partial unique index that proves
	 * the var is unique for this query.
	 */
	if (vardata->isunique)
		stadistinct = -1.0;

	/*
	 * If we had an absolute estimate, use that.
	 */
	if (stadistinct > 0.0)
		return stadistinct;

	/*
	 * Otherwise we need to get the relation size; punt if not available.
	 */
	if (vardata->rel == NULL)
	{
		*isdefault = true;
		return DEFAULT_NUM_DISTINCT;
	}
	ntuples = vardata->rel->tuples;
	if (ntuples <= 0.0)
	{
		*isdefault = true;
		return DEFAULT_NUM_DISTINCT;
	}

	/*
	 * If we had a relative estimate, use that.
	 */
	if (stadistinct < 0.0)
		return floor((-stadistinct * ntuples) + 0.5);

	/*
	 * With no data, estimate ndistinct = ntuples if the table is small, else
	 * use default.  We use DEFAULT_NUM_DISTINCT as the cutoff for "small" so
	 * that the behavior isn't discontinuous.
	 */
	if (ntuples < DEFAULT_NUM_DISTINCT)
		return ntuples;

	*isdefault = true;
	return DEFAULT_NUM_DISTINCT;
}

/*
 * get_variable_range
 *		Estimate the minimum and maximum value of the specified variable.
 *		If successful, store values in *min and *max, and return TRUE.
 *		If no data available, return FALSE.
 *
 * sortop is the "<" comparison operator to use.  This should generally
 * be "<" not ">", as only the former is likely to be found in pg_statistic.
 */
static bool
get_variable_range(PlannerInfo *root, VariableStatData *vardata, Oid sortop,
				   Datum *min, Datum *max)
{
	Datum		tmin = 0;
	Datum		tmax = 0;
	bool		have_data = false;
	int16		typLen;
	bool		typByVal;
	Datum	   *values;
	int			nvalues;
	int			i;

	/*
	 * XXX It's very tempting to try to use the actual column min and max, if
	 * we can get them relatively-cheaply with an index probe.	However, since
	 * this function is called many times during join planning, that could
	 * have unpleasant effects on planning speed.  Need more investigation
	 * before enabling this.
	 */
#ifdef NOT_USED
	if (get_actual_variable_range(root, vardata, sortop, min, max))
		return true;
#endif

	if (!HeapTupleIsValid(vardata->statsTuple))
	{
		/* no stats available, so default result */
		return false;
	}

	get_typlenbyval(vardata->atttype, &typLen, &typByVal);

	/*
	 * If there is a histogram, grab the first and last values.
	 *
	 * If there is a histogram that is sorted with some other operator than
	 * the one we want, fail --- this suggests that there is data we can't
	 * use.
	 */
	if (get_attstatsslot(vardata->statsTuple,
						 vardata->atttype, vardata->atttypmod,
						 STATISTIC_KIND_HISTOGRAM, sortop,
						 NULL,
						 &values, &nvalues,
						 NULL, NULL))
	{
		if (nvalues > 0)
		{
			tmin = datumCopy(values[0], typByVal, typLen);
			tmax = datumCopy(values[nvalues - 1], typByVal, typLen);
			have_data = true;
		}
		free_attstatsslot(vardata->atttype, values, nvalues, NULL, 0);
	}
	else if (get_attstatsslot(vardata->statsTuple,
							  vardata->atttype, vardata->atttypmod,
							  STATISTIC_KIND_HISTOGRAM, InvalidOid,
							  NULL,
							  &values, &nvalues,
							  NULL, NULL))
	{
		free_attstatsslot(vardata->atttype, values, nvalues, NULL, 0);
		return false;
	}

	/*
	 * If we have most-common-values info, look for extreme MCVs.  This is
	 * needed even if we also have a histogram, since the histogram excludes
	 * the MCVs.  However, usually the MCVs will not be the extreme values, so
	 * avoid unnecessary data copying.
	 */
	if (get_attstatsslot(vardata->statsTuple,
						 vardata->atttype, vardata->atttypmod,
						 STATISTIC_KIND_MCV, InvalidOid,
						 NULL,
						 &values, &nvalues,
						 NULL, NULL))
	{
		bool		tmin_is_mcv = false;
		bool		tmax_is_mcv = false;
		FmgrInfo	opproc;

		fmgr_info(get_opcode(sortop), &opproc);

		for (i = 0; i < nvalues; i++)
		{
			if (!have_data)
			{
				tmin = tmax = values[i];
				tmin_is_mcv = tmax_is_mcv = have_data = true;
				continue;
			}
			if (DatumGetBool(FunctionCall2Coll(&opproc,
											   DEFAULT_COLLATION_OID,
											   values[i], tmin)))
			{
				tmin = values[i];
				tmin_is_mcv = true;
			}
			if (DatumGetBool(FunctionCall2Coll(&opproc,
											   DEFAULT_COLLATION_OID,
											   tmax, values[i])))
			{
				tmax = values[i];
				tmax_is_mcv = true;
			}
		}
		if (tmin_is_mcv)
			tmin = datumCopy(tmin, typByVal, typLen);
		if (tmax_is_mcv)
			tmax = datumCopy(tmax, typByVal, typLen);
		free_attstatsslot(vardata->atttype, values, nvalues, NULL, 0);
	}

	*min = tmin;
	*max = tmax;
	return have_data;
}


/*
 * get_actual_variable_range
 *		Attempt to identify the current *actual* minimum and/or maximum
 *		of the specified variable, by looking for a suitable btree index
 *		and fetching its low and/or high values.
 *		If successful, store values in *min and *max, and return TRUE.
 *		(Either pointer can be NULL if that endpoint isn't needed.)
 *		If no data available, return FALSE.
 *
 * sortop is the "<" comparison operator to use.
 */
static bool
get_actual_variable_range(PlannerInfo *root, VariableStatData *vardata,
						  Oid sortop,
						  Datum *min, Datum *max)
{
	bool		have_data = false;
	RelOptInfo *rel = vardata->rel;
	RangeTblEntry *rte;
	ListCell   *lc;

	/* No hope if no relation or it doesn't have indexes */
	if (rel == NULL || rel->indexlist == NIL)
		return false;
	/* If it has indexes it must be a plain relation */
	rte = root->simple_rte_array[rel->relid];
	Assert(rte->rtekind == RTE_RELATION);

	/* Search through the indexes to see if any match our problem */
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);
		ScanDirection indexscandir;

		/* Ignore non-btree indexes */
		if (index->relam != BTREE_AM_OID)
			continue;

		/*
		 * Ignore partial indexes --- we only want stats that cover the entire
		 * relation.
		 */
		if (index->indpred != NIL)
			continue;

		/*
		 * The index list might include hypothetical indexes inserted by a
		 * get_relation_info hook --- don't try to access them.
		 */
		if (index->hypothetical)
			continue;

		/*
		 * The first index column must match the desired variable and sort
		 * operator --- but we can use a descending-order index.
		 */
		if (!match_index_to_operand(vardata->var, 0, index))
			continue;
		switch (get_op_opfamily_strategy(sortop, index->sortopfamily[0]))
		{
			case BTLessStrategyNumber:
				if (index->reverse_sort[0])
					indexscandir = BackwardScanDirection;
				else
					indexscandir = ForwardScanDirection;
				break;
			case BTGreaterStrategyNumber:
				if (index->reverse_sort[0])
					indexscandir = ForwardScanDirection;
				else
					indexscandir = BackwardScanDirection;
				break;
			default:
				/* index doesn't match the sortop */
				continue;
		}

		/*
		 * Found a suitable index to extract data from.  We'll need an EState
		 * and a bunch of other infrastructure.
		 */
		{
			EState	   *estate;
			ExprContext *econtext;
			MemoryContext tmpcontext;
			MemoryContext oldcontext;
			Relation	heapRel;
			Relation	indexRel;
			IndexInfo  *indexInfo;
			TupleTableSlot *slot;
			int16		typLen;
			bool		typByVal;
			ScanKeyData scankeys[1];
			IndexScanDesc index_scan;
			HeapTuple	tup;
			Datum		values[INDEX_MAX_KEYS];
			bool		isnull[INDEX_MAX_KEYS];

			estate = CreateExecutorState();
			econtext = GetPerTupleExprContext(estate);
			/* Make sure any cruft is generated in the econtext's memory */
			tmpcontext = econtext->ecxt_per_tuple_memory;
			oldcontext = MemoryContextSwitchTo(tmpcontext);

			/*
			 * Open the table and index so we can read from them.  We should
			 * already have at least AccessShareLock on the table, but not
			 * necessarily on the index.
			 */
			heapRel = heap_open(rte->relid, NoLock);
			indexRel = index_open(index->indexoid, AccessShareLock);

			/* extract index key information from the index's pg_index info */
			indexInfo = BuildIndexInfo(indexRel);

			/* some other stuff */
			slot = MakeSingleTupleTableSlot(RelationGetDescr(heapRel));
			econtext->ecxt_scantuple = slot;
			get_typlenbyval(vardata->atttype, &typLen, &typByVal);

			/* set up an IS NOT NULL scan key so that we ignore nulls */
			ScanKeyEntryInitialize(&scankeys[0],
								   SK_ISNULL | SK_SEARCHNOTNULL,
								   1,	/* index col to scan */
								   InvalidStrategy,		/* no strategy */
								   InvalidOid,	/* no strategy subtype */
								   InvalidOid,	/* no collation */
								   InvalidOid,	/* no reg proc for this */
								   (Datum) 0);	/* constant */

			have_data = true;

			/* If min is requested ... */
			if (min)
			{
				index_scan = index_beginscan(heapRel, indexRel, SnapshotNow,
											 1, 0);
				index_rescan(index_scan, scankeys, 1, NULL, 0);

				/* Fetch first tuple in sortop's direction */
				if ((tup = index_getnext(index_scan,
										 indexscandir)) != NULL)
				{
					/* Extract the index column values from the heap tuple */
					ExecStoreTuple(tup, slot, InvalidBuffer, false);
					FormIndexDatum(indexInfo, slot, estate,
								   values, isnull);

					/* Shouldn't have got a null, but be careful */
					if (isnull[0])
						elog(ERROR, "found unexpected null value in index \"%s\"",
							 RelationGetRelationName(indexRel));

					/* Copy the index column value out to caller's context */
					MemoryContextSwitchTo(oldcontext);
					*min = datumCopy(values[0], typByVal, typLen);
					MemoryContextSwitchTo(tmpcontext);
				}
				else
					have_data = false;

				index_endscan(index_scan);
			}

			/* If max is requested, and we didn't find the index is empty */
			if (max && have_data)
			{
				index_scan = index_beginscan(heapRel, indexRel, SnapshotNow,
											 1, 0);
				index_rescan(index_scan, scankeys, 1, NULL, 0);

				/* Fetch first tuple in reverse direction */
				if ((tup = index_getnext(index_scan,
										 -indexscandir)) != NULL)
				{
					/* Extract the index column values from the heap tuple */
					ExecStoreTuple(tup, slot, InvalidBuffer, false);
					FormIndexDatum(indexInfo, slot, estate,
								   values, isnull);

					/* Shouldn't have got a null, but be careful */
					if (isnull[0])
						elog(ERROR, "found unexpected null value in index \"%s\"",
							 RelationGetRelationName(indexRel));

					/* Copy the index column value out to caller's context */
					MemoryContextSwitchTo(oldcontext);
					*max = datumCopy(values[0], typByVal, typLen);
					MemoryContextSwitchTo(tmpcontext);
				}
				else
					have_data = false;

				index_endscan(index_scan);
			}

			/* Clean everything up */
			ExecDropSingleTupleTableSlot(slot);

			index_close(indexRel, AccessShareLock);
			heap_close(heapRel, NoLock);

			MemoryContextSwitchTo(oldcontext);
			FreeExecutorState(estate);

			/* And we're done */
			break;
		}
	}

	return have_data;
}

/*
 * find_join_input_rel
 *		Look up the input relation for a join.
 *
 * We assume that the input relation's RelOptInfo must have been constructed
 * already.
 */
static RelOptInfo *
find_join_input_rel(PlannerInfo *root, Relids relids)
{
	RelOptInfo *rel = NULL;

	switch (bms_membership(relids))
	{
		case BMS_EMPTY_SET:
			/* should not happen */
			break;
		case BMS_SINGLETON:
			rel = find_base_rel(root, bms_singleton_member(relids));
			break;
		case BMS_MULTIPLE:
			rel = find_join_rel(root, relids);
			break;
	}

	if (rel == NULL)
		elog(ERROR, "could not find RelOptInfo for given relids");

	return rel;
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
 * Note that the prefix-analysis functions are called from
 * backend/optimizer/path/indxpath.c as well as from routines in this file.
 *
 *-------------------------------------------------------------------------
 */

/*
 * Check whether char is a letter (and, hence, subject to case-folding)
 *
 * In multibyte character sets, we can't use isalpha, and it does not seem
 * worth trying to convert to wchar_t to use iswalpha.	Instead, just assume
 * any multibyte char is potentially case-varying.
 */
static int
pattern_char_isalpha(char c, bool is_multibyte,
					 pg_locale_t locale, bool locale_is_c)
{
	if (locale_is_c)
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
	else if (is_multibyte && IS_HIGHBIT_SET(c))
		return true;
#ifdef HAVE_LOCALE_T
	else if (locale)
		return isalpha_l((unsigned char) c, locale);
#endif
	else
		return isalpha((unsigned char) c);
}

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
		bytea	   *bstr = DatumGetByteaP(patt_const->constvalue);

		pattlen = VARSIZE(bstr) - VARHDRSZ;
		patt = (char *) palloc(pattlen);
		memcpy(patt, VARDATA(bstr), pattlen);
		if ((Pointer) bstr != DatumGetPointer(patt_const->constvalue))
			pfree(bstr);
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

Pattern_Prefix_Status
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
		default:
			elog(ERROR, "unrecognized ptype: %d", (int) ptype);
			result = Pattern_Prefix_None;		/* keep compiler quiet */
			break;
	}
	return result;
}

/*
 * Estimate the selectivity of a fixed prefix for a pattern match.
 *
 * A fixed prefix "foo" is estimated as the selectivity of the expression
 * "variable >= 'foo' AND variable < 'fop'" (see also indxpath.c).
 *
 * The selectivity estimate is with respect to the portion of the column
 * population represented by the histogram --- the caller must fold this
 * together with info about MCVs and NULLs.
 *
 * We use the >= and < operators from the specified btree opfamily to do the
 * estimation.	The given variable and Const must be of the associated
 * datatype.
 *
 * XXX Note: we make use of the upper bound to estimate operator selectivity
 * even if the locale is such that we cannot rely on the upper-bound string.
 * The selectivity only needs to be approximately right anyway, so it seems
 * more useful to use the upper-bound code than not.
 */
static Selectivity
prefix_selectivity(PlannerInfo *root, VariableStatData *vardata,
				   Oid vartype, Oid opfamily, Const *prefixcon)
{
	Selectivity prefixsel;
	Oid			cmpopr;
	FmgrInfo	opproc;
	Const	   *greaterstrcon;
	Selectivity eq_sel;

	cmpopr = get_opfamily_member(opfamily, vartype, vartype,
								 BTGreaterEqualStrategyNumber);
	if (cmpopr == InvalidOid)
		elog(ERROR, "no >= operator for opfamily %u", opfamily);
	fmgr_info(get_opcode(cmpopr), &opproc);

	prefixsel = ineq_histogram_selectivity(root, vardata, &opproc, true,
										   prefixcon->constvalue,
										   prefixcon->consttype);

	if (prefixsel < 0.0)
	{
		/* No histogram is present ... return a suitable default estimate */
		return DEFAULT_MATCH_SEL;
	}

	/*-------
	 * If we can create a string larger than the prefix, say
	 *	"x < greaterstr".
	 *-------
	 */
	cmpopr = get_opfamily_member(opfamily, vartype, vartype,
								 BTLessStrategyNumber);
	if (cmpopr == InvalidOid)
		elog(ERROR, "no < operator for opfamily %u", opfamily);
	fmgr_info(get_opcode(cmpopr), &opproc);
	greaterstrcon = make_greater_string(prefixcon, &opproc,
										DEFAULT_COLLATION_OID);
	if (greaterstrcon)
	{
		Selectivity topsel;

		topsel = ineq_histogram_selectivity(root, vardata, &opproc, false,
											greaterstrcon->constvalue,
											greaterstrcon->consttype);

		/* ineq_histogram_selectivity worked before, it shouldn't fail now */
		Assert(topsel >= 0.0);

		/*
		 * Merge the two selectivities in the same way as for a range query
		 * (see clauselist_selectivity()).	Note that we don't need to worry
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
	cmpopr = get_opfamily_member(opfamily, vartype, vartype,
								 BTEqualStrategyNumber);
	if (cmpopr == InvalidOid)
		elog(ERROR, "no = operator for opfamily %u", opfamily);
	eq_sel = var_eq_const(vardata, cmpopr, prefixcon->constvalue,
						  false, true);

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
			if (patt[pos] == ']')		/* ']' at start of class is not
										 * special */
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

	/* If there's a fixed prefix, discount its selectivity */
	if (fixed_prefix_len > 0)
		sel /= pow(FIXED_CHAR_SEL, fixed_prefix_len);

	/* Make sure result stays in range */
	CLAMP_PROBABILITY(sel);
	return sel;
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
 * than "foo bar".	CAUTION: Therefore, this function should be used only for
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
Const *
make_greater_string(const Const *str_const, FmgrInfo *ltproc, Oid collation)
{
	Oid			datatype = str_const->consttype;
	char	   *workstr;
	int			len;
	Datum		cmpstr;
	text	   *cmptxt = NULL;
	mbcharacter_incrementer charinc;

	/*
	 * Get a modifiable copy of the prefix string in C-string format, and set
	 * up the string we will compare to as a Datum.  In C locale this can just
	 * be the given prefix string, otherwise we need to add a suffix.  Types
	 * NAME and BYTEA sort bytewise so they don't need a suffix either.
	 */
	if (datatype == NAMEOID)
	{
		workstr = DatumGetCString(DirectFunctionCall1(nameout,
													  str_const->constvalue));
		len = strlen(workstr);
		cmpstr = str_const->constvalue;
	}
	else if (datatype == BYTEAOID)
	{
		bytea	   *bstr = DatumGetByteaP(str_const->constvalue);

		len = VARSIZE(bstr) - VARHDRSZ;
		workstr = (char *) palloc(len);
		memcpy(workstr, VARDATA(bstr), len);
		if ((Pointer) bstr != DatumGetPointer(str_const->constvalue))
			pfree(bstr);
		cmpstr = str_const->constvalue;
	}
	else
	{
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
			cmptxt = (text *) palloc(VARHDRSZ + len + 1);
			SET_VARSIZE(cmptxt, VARHDRSZ + len + 1);
			memcpy(VARDATA(cmptxt), workstr, len);
			*(VARDATA(cmptxt) + len) = suffixchar;
			cmpstr = PointerGetDatum(cmptxt);
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
			collation = InvalidOid;
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

/*-------------------------------------------------------------------------
 *
 * Index cost estimation functions
 *
 *-------------------------------------------------------------------------
 */

/*
 * genericcostestimate is a general-purpose estimator that can be used for
 * most index types.  In some cases we use genericcostestimate as the base
 * code and then incorporate additional index-type-specific knowledge in
 * the type-specific calling function.	To avoid code duplication, we make
 * genericcostestimate return a number of intermediate values as well as
 * its preliminary estimates of the output cost values.  The GenericCosts
 * struct includes all these values.
 *
 * Callers should initialize all fields of GenericCosts to zero.  In addition,
 * they can set numIndexTuples to some positive value if they have a better
 * than default way of estimating the number of leaf index tuples visited.
 */
typedef struct
{
	/* These are the values the cost estimator must return to the planner */
	Cost		indexStartupCost;		/* index-related startup cost */
	Cost		indexTotalCost; /* total index-related scan cost */
	Selectivity indexSelectivity;		/* selectivity of index */
	double		indexCorrelation;		/* order correlation of index */

	/* Intermediate values we obtain along the way */
	double		numIndexPages;	/* number of leaf pages visited */
	double		numIndexTuples; /* number of leaf tuples visited */
	double		spc_random_page_cost;	/* relevant random_page_cost value */
	double		num_sa_scans;	/* # indexscans from ScalarArrayOps */
} GenericCosts;

static void
genericcostestimate(PlannerInfo *root,
					IndexPath *path,
					double loop_count,
					GenericCosts *costs)
{
	IndexOptInfo *index = path->indexinfo;
	List	   *indexQuals = path->indexquals;
	List	   *indexOrderBys = path->indexorderbys;
	Cost		indexStartupCost;
	Cost		indexTotalCost;
	Selectivity indexSelectivity;
	double		indexCorrelation;
	double		numIndexPages;
	double		numIndexTuples;
	double		spc_random_page_cost;
	double		num_sa_scans;
	double		num_outer_scans;
	double		num_scans;
	QualCost	index_qual_cost;
	double		qual_op_cost;
	double		qual_arg_cost;
	List	   *selectivityQuals;
	ListCell   *l;

	/*
	 * If the index is partial, AND the index predicate with the explicitly
	 * given indexquals to produce a more accurate idea of the index
	 * selectivity.
	 */
	selectivityQuals = add_predicate_to_quals(index, indexQuals);

	/*
	 * Check for ScalarArrayOpExpr index quals, and estimate the number of
	 * index scans that will be performed.
	 */
	num_sa_scans = 1;
	foreach(l, indexQuals)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		if (IsA(rinfo->clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) rinfo->clause;
			int			alength = estimate_array_length(lsecond(saop->args));

			if (alength > 1)
				num_sa_scans *= alength;
		}
	}

	/* Estimate the fraction of main-table tuples that will be visited */
	indexSelectivity = clauselist_selectivity(root, selectivityQuals,
											  index->rel->relid,
											  JOIN_INNER,
											  NULL);

	/*
	 * If caller didn't give us an estimate, estimate the number of index
	 * tuples that will be visited.  We do it in this rather peculiar-looking
	 * way in order to get the right answer for partial indexes.
	 */
	numIndexTuples = costs->numIndexTuples;
	if (numIndexTuples <= 0.0)
	{
		numIndexTuples = indexSelectivity * index->rel->tuples;

		/*
		 * The above calculation counts all the tuples visited across all
		 * scans induced by ScalarArrayOpExpr nodes.  We want to consider the
		 * average per-indexscan number, so adjust.  This is a handy place to
		 * round to integer, too.  (If caller supplied tuple estimate, it's
		 * responsible for handling these considerations.)
		 */
		numIndexTuples = rint(numIndexTuples / num_sa_scans);
	}

	/*
	 * We can bound the number of tuples by the index size in any case. Also,
	 * always estimate at least one tuple is touched, even when
	 * indexSelectivity estimate is tiny.
	 */
	if (numIndexTuples > index->tuples)
		numIndexTuples = index->tuples;
	if (numIndexTuples < 1.0)
		numIndexTuples = 1.0;

	/*
	 * Estimate the number of index pages that will be retrieved.
	 *
	 * We use the simplistic method of taking a pro-rata fraction of the total
	 * number of index pages.  In effect, this counts only leaf pages and not
	 * any overhead such as index metapage or upper tree levels.
	 *
	 * In practice access to upper index levels is often nearly free because
	 * those tend to stay in cache under load; moreover, the cost involved is
	 * highly dependent on index type.	We therefore ignore such costs here
	 * and leave it to the caller to add a suitable charge if needed.
	 */
	if (index->pages > 1 && index->tuples > 1)
		numIndexPages = ceil(numIndexTuples * index->pages / index->tuples);
	else
		numIndexPages = 1.0;

	/* fetch estimated page cost for schema containing index */
	get_tablespace_page_costs(index->reltablespace,
							  &spc_random_page_cost,
							  NULL);

	/*
	 * Now compute the disk access costs.
	 *
	 * The above calculations are all per-index-scan.  However, if we are in a
	 * nestloop inner scan, we can expect the scan to be repeated (with
	 * different search keys) for each row of the outer relation.  Likewise,
	 * ScalarArrayOpExpr quals result in multiple index scans.	This creates
	 * the potential for cache effects to reduce the number of disk page
	 * fetches needed.	We want to estimate the average per-scan I/O cost in
	 * the presence of caching.
	 *
	 * We use the Mackert-Lohman formula (see costsize.c for details) to
	 * estimate the total number of page fetches that occur.  While this
	 * wasn't what it was designed for, it seems a reasonable model anyway.
	 * Note that we are counting pages not tuples anymore, so we take N = T =
	 * index size, as if there were one "tuple" per page.
	 */
	num_outer_scans = loop_count;
	num_scans = num_sa_scans * num_outer_scans;

	if (num_scans > 1)
	{
		double		pages_fetched;

		/* total page fetches ignoring cache effects */
		pages_fetched = numIndexPages * num_scans;

		/* use Mackert and Lohman formula to adjust for cache effects */
		pages_fetched = index_pages_fetched(pages_fetched,
											index->pages,
											(double) index->pages,
											root);

		/*
		 * Now compute the total disk access cost, and then report a pro-rated
		 * share for each outer scan.  (Don't pro-rate for ScalarArrayOpExpr,
		 * since that's internal to the indexscan.)
		 */
		indexTotalCost = (pages_fetched * spc_random_page_cost)
			/ num_outer_scans;
	}
	else
	{
		/*
		 * For a single index scan, we just charge spc_random_page_cost per
		 * page touched.
		 */
		indexTotalCost = numIndexPages * spc_random_page_cost;
	}

	/*
	 * CPU cost: any complex expressions in the indexquals will need to be
	 * evaluated once at the start of the scan to reduce them to runtime keys
	 * to pass to the index AM (see nodeIndexscan.c).  We model the per-tuple
	 * CPU costs as cpu_index_tuple_cost plus one cpu_operator_cost per
	 * indexqual operator.	Because we have numIndexTuples as a per-scan
	 * number, we have to multiply by num_sa_scans to get the correct result
	 * for ScalarArrayOpExpr cases.  Similarly add in costs for any index
	 * ORDER BY expressions.
	 *
	 * Note: this neglects the possible costs of rechecking lossy operators.
	 * Detecting that that might be needed seems more expensive than it's
	 * worth, though, considering all the other inaccuracies here ...
	 */
	cost_qual_eval(&index_qual_cost, indexQuals, root);
	qual_arg_cost = index_qual_cost.startup + index_qual_cost.per_tuple;
	cost_qual_eval(&index_qual_cost, indexOrderBys, root);
	qual_arg_cost += index_qual_cost.startup + index_qual_cost.per_tuple;
	qual_op_cost = cpu_operator_cost *
		(list_length(indexQuals) + list_length(indexOrderBys));
	qual_arg_cost -= qual_op_cost;
	if (qual_arg_cost < 0)		/* just in case... */
		qual_arg_cost = 0;

	indexStartupCost = qual_arg_cost;
	indexTotalCost += qual_arg_cost;
	indexTotalCost += numIndexTuples * num_sa_scans * (cpu_index_tuple_cost + qual_op_cost);

	/*
	 * Generic assumption about index correlation: there isn't any.
	 */
	indexCorrelation = 0.0;

	/*
	 * Return everything to caller.
	 */
	costs->indexStartupCost = indexStartupCost;
	costs->indexTotalCost = indexTotalCost;
	costs->indexSelectivity = indexSelectivity;
	costs->indexCorrelation = indexCorrelation;
	costs->numIndexPages = numIndexPages;
	costs->numIndexTuples = numIndexTuples;
	costs->spc_random_page_cost = spc_random_page_cost;
	costs->num_sa_scans = num_sa_scans;
}

/*
 * If the index is partial, add its predicate to the given qual list.
 *
 * ANDing the index predicate with the explicitly given indexquals produces
 * a more accurate idea of the index's selectivity.  However, we need to be
 * careful not to insert redundant clauses, because clauselist_selectivity()
 * is easily fooled into computing a too-low selectivity estimate.	Our
 * approach is to add only the predicate clause(s) that cannot be proven to
 * be implied by the given indexquals.	This successfully handles cases such
 * as a qual "x = 42" used with a partial index "WHERE x >= 40 AND x < 50".
 * There are many other cases where we won't detect redundancy, leading to a
 * too-low selectivity estimate, which will bias the system in favor of using
 * partial indexes where possible.	That is not necessarily bad though.
 *
 * Note that indexQuals contains RestrictInfo nodes while the indpred
 * does not, so the output list will be mixed.	This is OK for both
 * predicate_implied_by() and clauselist_selectivity(), but might be
 * problematic if the result were passed to other things.
 */
static List *
add_predicate_to_quals(IndexOptInfo *index, List *indexQuals)
{
	List	   *predExtraQuals = NIL;
	ListCell   *lc;

	if (index->indpred == NIL)
		return indexQuals;

	foreach(lc, index->indpred)
	{
		Node	   *predQual = (Node *) lfirst(lc);
		List	   *oneQual = list_make1(predQual);

		if (!predicate_implied_by(oneQual, indexQuals))
			predExtraQuals = list_concat(predExtraQuals, oneQual);
	}
	/* list_concat avoids modifying the passed-in indexQuals list */
	return list_concat(predExtraQuals, indexQuals);
}


Datum
btcostestimate(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	IndexPath  *path = (IndexPath *) PG_GETARG_POINTER(1);
	double		loop_count = PG_GETARG_FLOAT8(2);
	Cost	   *indexStartupCost = (Cost *) PG_GETARG_POINTER(3);
	Cost	   *indexTotalCost = (Cost *) PG_GETARG_POINTER(4);
	Selectivity *indexSelectivity = (Selectivity *) PG_GETARG_POINTER(5);
	double	   *indexCorrelation = (double *) PG_GETARG_POINTER(6);
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs;
	Oid			relid;
	AttrNumber	colnum;
	VariableStatData vardata;
	double		numIndexTuples;
	Cost		descentCost;
	List	   *indexBoundQuals;
	int			indexcol;
	bool		eqQualHere;
	bool		found_saop;
	bool		found_is_null_op;
	double		num_sa_scans;
	ListCell   *lcc,
			   *lci;

	/*
	 * For a btree scan, only leading '=' quals plus inequality quals for the
	 * immediately next attribute contribute to index selectivity (these are
	 * the "boundary quals" that determine the starting and stopping points of
	 * the index scan).  Additional quals can suppress visits to the heap, so
	 * it's OK to count them in indexSelectivity, but they should not count
	 * for estimating numIndexTuples.  So we must examine the given indexquals
	 * to find out which ones count as boundary quals.	We rely on the
	 * knowledge that they are given in index column order.
	 *
	 * For a RowCompareExpr, we consider only the first column, just as
	 * rowcomparesel() does.
	 *
	 * If there's a ScalarArrayOpExpr in the quals, we'll actually perform N
	 * index scans not one, but the ScalarArrayOpExpr's operator can be
	 * considered to act the same as it normally does.
	 */
	indexBoundQuals = NIL;
	indexcol = 0;
	eqQualHere = false;
	found_saop = false;
	found_is_null_op = false;
	num_sa_scans = 1;
	forboth(lcc, path->indexquals, lci, path->indexqualcols)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lcc);
		Expr	   *clause;
		Node	   *leftop,
				   *rightop PG_USED_FOR_ASSERTS_ONLY;
		Oid			clause_op;
		int			op_strategy;
		bool		is_null_op = false;

		if (indexcol != lfirst_int(lci))
		{
			/* Beginning of a new column's quals */
			if (!eqQualHere)
				break;			/* done if no '=' qual for indexcol */
			eqQualHere = false;
			indexcol++;
			if (indexcol != lfirst_int(lci))
				break;			/* no quals at all for indexcol */
		}

		Assert(IsA(rinfo, RestrictInfo));
		clause = rinfo->clause;

		if (IsA(clause, OpExpr))
		{
			leftop = get_leftop(clause);
			rightop = get_rightop(clause);
			clause_op = ((OpExpr *) clause)->opno;
		}
		else if (IsA(clause, RowCompareExpr))
		{
			RowCompareExpr *rc = (RowCompareExpr *) clause;

			leftop = (Node *) linitial(rc->largs);
			rightop = (Node *) linitial(rc->rargs);
			clause_op = linitial_oid(rc->opnos);
		}
		else if (IsA(clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

			leftop = (Node *) linitial(saop->args);
			rightop = (Node *) lsecond(saop->args);
			clause_op = saop->opno;
			found_saop = true;
		}
		else if (IsA(clause, NullTest))
		{
			NullTest   *nt = (NullTest *) clause;

			leftop = (Node *) nt->arg;
			rightop = NULL;
			clause_op = InvalidOid;
			if (nt->nulltesttype == IS_NULL)
			{
				found_is_null_op = true;
				is_null_op = true;
			}
		}
		else
		{
			elog(ERROR, "unsupported indexqual type: %d",
				 (int) nodeTag(clause));
			continue;			/* keep compiler quiet */
		}

		if (match_index_to_operand(leftop, indexcol, index))
		{
			/* clause_op is correct */
		}
		else
		{
			Assert(match_index_to_operand(rightop, indexcol, index));
			/* Must flip operator to get the opfamily member */
			clause_op = get_commutator(clause_op);
		}

		/* check for equality operator */
		if (OidIsValid(clause_op))
		{
			op_strategy = get_op_opfamily_strategy(clause_op,
												   index->opfamily[indexcol]);
			Assert(op_strategy != 0);	/* not a member of opfamily?? */
			if (op_strategy == BTEqualStrategyNumber)
				eqQualHere = true;
		}
		else if (is_null_op)
		{
			/* IS NULL is like = for purposes of selectivity determination */
			eqQualHere = true;
		}
		/* count up number of SA scans induced by indexBoundQuals only */
		if (IsA(clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
			int			alength = estimate_array_length(lsecond(saop->args));

			if (alength > 1)
				num_sa_scans *= alength;
		}
		indexBoundQuals = lappend(indexBoundQuals, rinfo);
	}

	/*
	 * If index is unique and we found an '=' clause for each column, we can
	 * just assume numIndexTuples = 1 and skip the expensive
	 * clauselist_selectivity calculations.  However, a ScalarArrayOp or
	 * NullTest invalidates that theory, even though it sets eqQualHere.
	 */
	if (index->unique &&
		indexcol == index->ncolumns - 1 &&
		eqQualHere &&
		!found_saop &&
		!found_is_null_op)
		numIndexTuples = 1.0;
	else
	{
		List	   *selectivityQuals;
		Selectivity btreeSelectivity;

		/*
		 * If the index is partial, AND the index predicate with the
		 * index-bound quals to produce a more accurate idea of the number of
		 * rows covered by the bound conditions.
		 */
		selectivityQuals = add_predicate_to_quals(index, indexBoundQuals);

		btreeSelectivity = clauselist_selectivity(root, selectivityQuals,
												  index->rel->relid,
												  JOIN_INNER,
												  NULL);
		numIndexTuples = btreeSelectivity * index->rel->tuples;

		/*
		 * As in genericcostestimate(), we have to adjust for any
		 * ScalarArrayOpExpr quals included in indexBoundQuals, and then round
		 * to integer.
		 */
		numIndexTuples = rint(numIndexTuples / num_sa_scans);
	}

	/*
	 * Now do generic index cost estimation.
	 */
	MemSet(&costs, 0, sizeof(costs));
	costs.numIndexTuples = numIndexTuples;

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * Add a CPU-cost component to represent the costs of initial btree
	 * descent.  We don't charge any I/O cost for touching upper btree levels,
	 * since they tend to stay in cache, but we still have to do about log2(N)
	 * comparisons to descend a btree of N leaf tuples.  We charge one
	 * cpu_operator_cost per comparison.
	 *
	 * If there are ScalarArrayOpExprs, charge this once per SA scan.  The
	 * ones after the first one are not startup cost so far as the overall
	 * plan is concerned, so add them only to "total" cost.
	 */
	if (index->tuples > 1)		/* avoid computing log(0) */
	{
		descentCost = ceil(log(index->tuples) / log(2.0)) * cpu_operator_cost;
		costs.indexStartupCost += descentCost;
		costs.indexTotalCost += costs.num_sa_scans * descentCost;
	}

	/*
	 * Even though we're not charging I/O cost for touching upper btree pages,
	 * it's still reasonable to charge some CPU cost per page descended
	 * through.  Moreover, if we had no such charge at all, bloated indexes
	 * would appear to have the same search cost as unbloated ones, at least
	 * in cases where only a single leaf page is expected to be visited.  This
	 * cost is somewhat arbitrarily set at 50x cpu_operator_cost per page
	 * touched.  The number of such pages is btree tree height plus one (ie,
	 * we charge for the leaf page too).  As above, charge once per SA scan.
	 */
	descentCost = (index->tree_height + 1) * 50.0 * cpu_operator_cost;
	costs.indexStartupCost += descentCost;
	costs.indexTotalCost += costs.num_sa_scans * descentCost;

	/*
	 * If we can get an estimate of the first column's ordering correlation C
	 * from pg_statistic, estimate the index correlation as C for a
	 * single-column index, or C * 0.75 for multiple columns. (The idea here
	 * is that multiple columns dilute the importance of the first column's
	 * ordering, but don't negate it entirely.  Before 8.0 we divided the
	 * correlation by the number of columns, but that seems too strong.)
	 */
	MemSet(&vardata, 0, sizeof(vardata));

	if (index->indexkeys[0] != 0)
	{
		/* Simple variable --- look to stats for the underlying table */
		RangeTblEntry *rte = planner_rt_fetch(index->rel->relid, root);

		Assert(rte->rtekind == RTE_RELATION);
		relid = rte->relid;
		Assert(relid != InvalidOid);
		colnum = index->indexkeys[0];

		if (get_relation_stats_hook &&
			(*get_relation_stats_hook) (root, rte, colnum, &vardata))
		{
			/*
			 * The hook took control of acquiring a stats tuple.  If it did
			 * supply a tuple, it'd better have supplied a freefunc.
			 */
			if (HeapTupleIsValid(vardata.statsTuple) &&
				!vardata.freefunc)
				elog(ERROR, "no function provided to release variable stats with");
		}
		else
		{
			vardata.statsTuple = SearchSysCache3(STATRELATTINH,
												 ObjectIdGetDatum(relid),
												 Int16GetDatum(colnum),
												 BoolGetDatum(rte->inh));
			vardata.freefunc = ReleaseSysCache;
		}
	}
	else
	{
		/* Expression --- maybe there are stats for the index itself */
		relid = index->indexoid;
		colnum = 1;

		if (get_index_stats_hook &&
			(*get_index_stats_hook) (root, relid, colnum, &vardata))
		{
			/*
			 * The hook took control of acquiring a stats tuple.  If it did
			 * supply a tuple, it'd better have supplied a freefunc.
			 */
			if (HeapTupleIsValid(vardata.statsTuple) &&
				!vardata.freefunc)
				elog(ERROR, "no function provided to release variable stats with");
		}
		else
		{
			vardata.statsTuple = SearchSysCache3(STATRELATTINH,
												 ObjectIdGetDatum(relid),
												 Int16GetDatum(colnum),
												 BoolGetDatum(false));
			vardata.freefunc = ReleaseSysCache;
		}
	}

	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Oid			sortop;
		float4	   *numbers;
		int			nnumbers;

		sortop = get_opfamily_member(index->opfamily[0],
									 index->opcintype[0],
									 index->opcintype[0],
									 BTLessStrategyNumber);
		if (OidIsValid(sortop) &&
			get_attstatsslot(vardata.statsTuple, InvalidOid, 0,
							 STATISTIC_KIND_CORRELATION,
							 sortop,
							 NULL,
							 NULL, NULL,
							 &numbers, &nnumbers))
		{
			double		varCorrelation;

			Assert(nnumbers == 1);
			varCorrelation = numbers[0];

			if (index->reverse_sort[0])
				varCorrelation = -varCorrelation;

			if (index->ncolumns > 1)
				costs.indexCorrelation = varCorrelation * 0.75;
			else
				costs.indexCorrelation = varCorrelation;

			free_attstatsslot(InvalidOid, NULL, 0, numbers, nnumbers);
		}
	}

	ReleaseVariableStats(vardata);

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;

	PG_RETURN_VOID();
}

Datum
hashcostestimate(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	IndexPath  *path = (IndexPath *) PG_GETARG_POINTER(1);
	double		loop_count = PG_GETARG_FLOAT8(2);
	Cost	   *indexStartupCost = (Cost *) PG_GETARG_POINTER(3);
	Cost	   *indexTotalCost = (Cost *) PG_GETARG_POINTER(4);
	Selectivity *indexSelectivity = (Selectivity *) PG_GETARG_POINTER(5);
	double	   *indexCorrelation = (double *) PG_GETARG_POINTER(6);
	GenericCosts costs;

	MemSet(&costs, 0, sizeof(costs));

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * A hash index has no descent costs as such, since the index AM can go
	 * directly to the target bucket after computing the hash value.  There
	 * are a couple of other hash-specific costs that we could conceivably add
	 * here, though:
	 *
	 * Ideally we'd charge spc_random_page_cost for each page in the target
	 * bucket, not just the numIndexPages pages that genericcostestimate
	 * thought we'd visit.  However in most cases we don't know which bucket
	 * that will be.  There's no point in considering the average bucket size
	 * because the hash AM makes sure that's always one page.
	 *
	 * Likewise, we could consider charging some CPU for each index tuple in
	 * the bucket, if we knew how many there were.	But the per-tuple cost is
	 * just a hash value comparison, not a general datatype-dependent
	 * comparison, so any such charge ought to be quite a bit less than
	 * cpu_operator_cost; which makes it probably not worth worrying about.
	 *
	 * A bigger issue is that chance hash-value collisions will result in
	 * wasted probes into the heap.  We don't currently attempt to model this
	 * cost on the grounds that it's rare, but maybe it's not rare enough.
	 * (Any fix for this ought to consider the generic lossy-operator problem,
	 * though; it's not entirely hash-specific.)
	 */

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;

	PG_RETURN_VOID();
}

Datum
gistcostestimate(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	IndexPath  *path = (IndexPath *) PG_GETARG_POINTER(1);
	double		loop_count = PG_GETARG_FLOAT8(2);
	Cost	   *indexStartupCost = (Cost *) PG_GETARG_POINTER(3);
	Cost	   *indexTotalCost = (Cost *) PG_GETARG_POINTER(4);
	Selectivity *indexSelectivity = (Selectivity *) PG_GETARG_POINTER(5);
	double	   *indexCorrelation = (double *) PG_GETARG_POINTER(6);
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs;
	Cost		descentCost;

	MemSet(&costs, 0, sizeof(costs));

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * We model index descent costs similarly to those for btree, but to do
	 * that we first need an idea of the tree height.  We somewhat arbitrarily
	 * assume that the fanout is 100, meaning the tree height is at most
	 * log100(index->pages).
	 *
	 * Although this computation isn't really expensive enough to require
	 * caching, we might as well use index->tree_height to cache it.
	 */
	if (index->tree_height < 0) /* unknown? */
	{
		if (index->pages > 1)	/* avoid computing log(0) */
			index->tree_height = (int) (log(index->pages) / log(100.0));
		else
			index->tree_height = 0;
	}

	/*
	 * Add a CPU-cost component to represent the costs of initial descent. We
	 * just use log(N) here not log2(N) since the branching factor isn't
	 * necessarily two anyway.	As for btree, charge once per SA scan.
	 */
	if (index->tuples > 1)		/* avoid computing log(0) */
	{
		descentCost = ceil(log(index->tuples)) * cpu_operator_cost;
		costs.indexStartupCost += descentCost;
		costs.indexTotalCost += costs.num_sa_scans * descentCost;
	}

	/*
	 * Likewise add a per-page charge, calculated the same as for btrees.
	 */
	descentCost = (index->tree_height + 1) * 50.0 * cpu_operator_cost;
	costs.indexStartupCost += descentCost;
	costs.indexTotalCost += costs.num_sa_scans * descentCost;

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;

	PG_RETURN_VOID();
}

Datum
spgcostestimate(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	IndexPath  *path = (IndexPath *) PG_GETARG_POINTER(1);
	double		loop_count = PG_GETARG_FLOAT8(2);
	Cost	   *indexStartupCost = (Cost *) PG_GETARG_POINTER(3);
	Cost	   *indexTotalCost = (Cost *) PG_GETARG_POINTER(4);
	Selectivity *indexSelectivity = (Selectivity *) PG_GETARG_POINTER(5);
	double	   *indexCorrelation = (double *) PG_GETARG_POINTER(6);
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs;
	Cost		descentCost;

	MemSet(&costs, 0, sizeof(costs));

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * We model index descent costs similarly to those for btree, but to do
	 * that we first need an idea of the tree height.  We somewhat arbitrarily
	 * assume that the fanout is 100, meaning the tree height is at most
	 * log100(index->pages).
	 *
	 * Although this computation isn't really expensive enough to require
	 * caching, we might as well use index->tree_height to cache it.
	 */
	if (index->tree_height < 0) /* unknown? */
	{
		if (index->pages > 1)	/* avoid computing log(0) */
			index->tree_height = (int) (log(index->pages) / log(100.0));
		else
			index->tree_height = 0;
	}

	/*
	 * Add a CPU-cost component to represent the costs of initial descent. We
	 * just use log(N) here not log2(N) since the branching factor isn't
	 * necessarily two anyway.	As for btree, charge once per SA scan.
	 */
	if (index->tuples > 1)		/* avoid computing log(0) */
	{
		descentCost = ceil(log(index->tuples)) * cpu_operator_cost;
		costs.indexStartupCost += descentCost;
		costs.indexTotalCost += costs.num_sa_scans * descentCost;
	}

	/*
	 * Likewise add a per-page charge, calculated the same as for btrees.
	 */
	descentCost = (index->tree_height + 1) * 50.0 * cpu_operator_cost;
	costs.indexStartupCost += descentCost;
	costs.indexTotalCost += costs.num_sa_scans * descentCost;

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;

	PG_RETURN_VOID();
}


/*
 * Support routines for gincostestimate
 */

typedef struct
{
	bool		haveFullScan;
	double		partialEntries;
	double		exactEntries;
	double		searchEntries;
	double		arrayScans;
} GinQualCounts;

/* Find the index column matching "op"; return its index, or -1 if no match */
static int
find_index_column(Node *op, IndexOptInfo *index)
{
	int			i;

	for (i = 0; i < index->ncolumns; i++)
	{
		if (match_index_to_operand(op, i, index))
			return i;
	}

	return -1;
}

/*
 * Estimate the number of index terms that need to be searched for while
 * testing the given GIN query, and increment the counts in *counts
 * appropriately.  If the query is unsatisfiable, return false.
 */
static bool
gincost_pattern(IndexOptInfo *index, int indexcol,
				Oid clause_op, Datum query,
				GinQualCounts *counts)
{
	Oid			extractProcOid;
	Oid			collation;
	int			strategy_op;
	Oid			lefttype,
				righttype;
	int32		nentries = 0;
	bool	   *partial_matches = NULL;
	Pointer    *extra_data = NULL;
	bool	   *nullFlags = NULL;
	int32		searchMode = GIN_SEARCH_MODE_DEFAULT;
	int32		i;

	/*
	 * Get the operator's strategy number and declared input data types within
	 * the index opfamily.	(We don't need the latter, but we use
	 * get_op_opfamily_properties because it will throw error if it fails to
	 * find a matching pg_amop entry.)
	 */
	get_op_opfamily_properties(clause_op, index->opfamily[indexcol], false,
							   &strategy_op, &lefttype, &righttype);

	/*
	 * GIN always uses the "default" support functions, which are those with
	 * lefttype == righttype == the opclass' opcintype (see
	 * IndexSupportInitialize in relcache.c).
	 */
	extractProcOid = get_opfamily_proc(index->opfamily[indexcol],
									   index->opcintype[indexcol],
									   index->opcintype[indexcol],
									   GIN_EXTRACTQUERY_PROC);

	if (!OidIsValid(extractProcOid))
	{
		/* should not happen; throw same error as index_getprocinfo */
		elog(ERROR, "missing support function %d for attribute %d of index \"%s\"",
			 GIN_EXTRACTQUERY_PROC, indexcol + 1,
			 get_rel_name(index->indexoid));
	}

	/*
	 * Choose collation to pass to extractProc (should match initGinState).
	 */
	if (OidIsValid(index->indexcollations[indexcol]))
		collation = index->indexcollations[indexcol];
	else
		collation = DEFAULT_COLLATION_OID;

	OidFunctionCall7Coll(extractProcOid,
						 collation,
						 query,
						 PointerGetDatum(&nentries),
						 UInt16GetDatum(strategy_op),
						 PointerGetDatum(&partial_matches),
						 PointerGetDatum(&extra_data),
						 PointerGetDatum(&nullFlags),
						 PointerGetDatum(&searchMode));

	if (nentries <= 0 && searchMode == GIN_SEARCH_MODE_DEFAULT)
	{
		/* No match is possible */
		return false;
	}

	for (i = 0; i < nentries; i++)
	{
		/*
		 * For partial match we haven't any information to estimate number of
		 * matched entries in index, so, we just estimate it as 100
		 */
		if (partial_matches && partial_matches[i])
			counts->partialEntries += 100;
		else
			counts->exactEntries++;

		counts->searchEntries++;
	}

	if (searchMode == GIN_SEARCH_MODE_INCLUDE_EMPTY)
	{
		/* Treat "include empty" like an exact-match item */
		counts->exactEntries++;
		counts->searchEntries++;
	}
	else if (searchMode != GIN_SEARCH_MODE_DEFAULT)
	{
		/* It's GIN_SEARCH_MODE_ALL */
		counts->haveFullScan = true;
	}

	return true;
}

/*
 * Estimate the number of index terms that need to be searched for while
 * testing the given GIN index clause, and increment the counts in *counts
 * appropriately.  If the query is unsatisfiable, return false.
 */
static bool
gincost_opexpr(IndexOptInfo *index, OpExpr *clause, GinQualCounts *counts)
{
	Node	   *leftop = get_leftop((Expr *) clause);
	Node	   *rightop = get_rightop((Expr *) clause);
	Oid			clause_op = clause->opno;
	int			indexcol;
	Node	   *operand;

	/* Locate the operand being compared to the index column */
	if ((indexcol = find_index_column(leftop, index)) >= 0)
	{
		operand = rightop;
	}
	else if ((indexcol = find_index_column(rightop, index)) >= 0)
	{
		operand = leftop;
		clause_op = get_commutator(clause_op);
	}
	else
	{
		elog(ERROR, "could not match index to operand");
		operand = NULL;			/* keep compiler quiet */
	}

	if (IsA(operand, RelabelType))
		operand = (Node *) ((RelabelType *) operand)->arg;

	/*
	 * It's impossible to call extractQuery method for unknown operand. So
	 * unless operand is a Const we can't do much; just assume there will be
	 * one ordinary search entry from the operand at runtime.
	 */
	if (!IsA(operand, Const))
	{
		counts->exactEntries++;
		counts->searchEntries++;
		return true;
	}

	/* If Const is null, there can be no matches */
	if (((Const *) operand)->constisnull)
		return false;

	/* Otherwise, apply extractQuery and get the actual term counts */
	return gincost_pattern(index, indexcol, clause_op,
						   ((Const *) operand)->constvalue,
						   counts);
}

/*
 * Estimate the number of index terms that need to be searched for while
 * testing the given GIN index clause, and increment the counts in *counts
 * appropriately.  If the query is unsatisfiable, return false.
 *
 * A ScalarArrayOpExpr will give rise to N separate indexscans at runtime,
 * each of which involves one value from the RHS array, plus all the
 * non-array quals (if any).  To model this, we average the counts across
 * the RHS elements, and add the averages to the counts in *counts (which
 * correspond to per-indexscan costs).	We also multiply counts->arrayScans
 * by N, causing gincostestimate to scale up its estimates accordingly.
 */
static bool
gincost_scalararrayopexpr(IndexOptInfo *index, ScalarArrayOpExpr *clause,
						  double numIndexEntries,
						  GinQualCounts *counts)
{
	Node	   *leftop = (Node *) linitial(clause->args);
	Node	   *rightop = (Node *) lsecond(clause->args);
	Oid			clause_op = clause->opno;
	int			indexcol;
	ArrayType  *arrayval;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	int			numElems;
	Datum	   *elemValues;
	bool	   *elemNulls;
	GinQualCounts arraycounts;
	int			numPossible = 0;
	int			i;

	Assert(clause->useOr);

	/* index column must be on the left */
	if ((indexcol = find_index_column(leftop, index)) < 0)
		elog(ERROR, "could not match index to operand");

	if (IsA(rightop, RelabelType))
		rightop = (Node *) ((RelabelType *) rightop)->arg;

	/*
	 * It's impossible to call extractQuery method for unknown operand. So
	 * unless operand is a Const we can't do much; just assume there will be
	 * one ordinary search entry from each array entry at runtime, and fall
	 * back on a probably-bad estimate of the number of array entries.
	 */
	if (!IsA(rightop, Const))
	{
		counts->exactEntries++;
		counts->searchEntries++;
		counts->arrayScans *= estimate_array_length(rightop);
		return true;
	}

	/* If Const is null, there can be no matches */
	if (((Const *) rightop)->constisnull)
		return false;

	/* Otherwise, extract the array elements and iterate over them */
	arrayval = DatumGetArrayTypeP(((Const *) rightop)->constvalue);
	get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
						 &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arrayval,
					  ARR_ELEMTYPE(arrayval),
					  elmlen, elmbyval, elmalign,
					  &elemValues, &elemNulls, &numElems);

	memset(&arraycounts, 0, sizeof(arraycounts));

	for (i = 0; i < numElems; i++)
	{
		GinQualCounts elemcounts;

		/* NULL can't match anything, so ignore, as the executor will */
		if (elemNulls[i])
			continue;

		/* Otherwise, apply extractQuery and get the actual term counts */
		memset(&elemcounts, 0, sizeof(elemcounts));

		if (gincost_pattern(index, indexcol, clause_op, elemValues[i],
							&elemcounts))
		{
			/* We ignore array elements that are unsatisfiable patterns */
			numPossible++;

			if (elemcounts.haveFullScan)
			{
				/*
				 * Full index scan will be required.  We treat this as if
				 * every key in the index had been listed in the query; is
				 * that reasonable?
				 */
				elemcounts.partialEntries = 0;
				elemcounts.exactEntries = numIndexEntries;
				elemcounts.searchEntries = numIndexEntries;
			}
			arraycounts.partialEntries += elemcounts.partialEntries;
			arraycounts.exactEntries += elemcounts.exactEntries;
			arraycounts.searchEntries += elemcounts.searchEntries;
		}
	}

	if (numPossible == 0)
	{
		/* No satisfiable patterns in the array */
		return false;
	}

	/*
	 * Now add the averages to the global counts.  This will give us an
	 * estimate of the average number of terms searched for in each indexscan,
	 * including contributions from both array and non-array quals.
	 */
	counts->partialEntries += arraycounts.partialEntries / numPossible;
	counts->exactEntries += arraycounts.exactEntries / numPossible;
	counts->searchEntries += arraycounts.searchEntries / numPossible;

	counts->arrayScans *= numPossible;

	return true;
}

/*
 * GIN has search behavior completely different from other index types
 */
Datum
gincostestimate(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	IndexPath  *path = (IndexPath *) PG_GETARG_POINTER(1);
	double		loop_count = PG_GETARG_FLOAT8(2);
	Cost	   *indexStartupCost = (Cost *) PG_GETARG_POINTER(3);
	Cost	   *indexTotalCost = (Cost *) PG_GETARG_POINTER(4);
	Selectivity *indexSelectivity = (Selectivity *) PG_GETARG_POINTER(5);
	double	   *indexCorrelation = (double *) PG_GETARG_POINTER(6);
	IndexOptInfo *index = path->indexinfo;
	List	   *indexQuals = path->indexquals;
	List	   *indexOrderBys = path->indexorderbys;
	ListCell   *l;
	List	   *selectivityQuals;
	double		numPages = index->pages,
				numTuples = index->tuples;
	double		numEntryPages,
				numDataPages,
				numPendingPages,
				numEntries;
	GinQualCounts counts;
	bool		matchPossible;
	double		entryPagesFetched,
				dataPagesFetched,
				dataPagesFetchedBySel;
	double		qual_op_cost,
				qual_arg_cost,
				spc_random_page_cost,
				outer_scans;
	QualCost	index_qual_cost;
	Relation	indexRel;
	GinStatsData ginStats;

	/*
	 * Obtain statistic information from the meta page
	 */
	indexRel = index_open(index->indexoid, AccessShareLock);
	ginGetStats(indexRel, &ginStats);
	index_close(indexRel, AccessShareLock);

	numEntryPages = ginStats.nEntryPages;
	numDataPages = ginStats.nDataPages;
	numPendingPages = ginStats.nPendingPages;
	numEntries = ginStats.nEntries;

	/*
	 * nPendingPages can be trusted, but the other fields are as of the last
	 * VACUUM.	Scale them by the ratio numPages / nTotalPages to account for
	 * growth since then.  If the fields are zero (implying no VACUUM at all,
	 * and an index created pre-9.1), assume all pages are entry pages.
	 */
	if (ginStats.nTotalPages == 0 || ginStats.nEntryPages == 0)
	{
		numEntryPages = numPages;
		numDataPages = 0;
		numEntries = numTuples; /* bogus, but no other info available */
	}
	else
	{
		double		scale = numPages / ginStats.nTotalPages;

		numEntryPages = ceil(numEntryPages * scale);
		numDataPages = ceil(numDataPages * scale);
		numEntries = ceil(numEntries * scale);
		/* ensure we didn't round up too much */
		numEntryPages = Min(numEntryPages, numPages);
		numDataPages = Min(numDataPages, numPages - numEntryPages);
	}

	/* In an empty index, numEntries could be zero.  Avoid divide-by-zero */
	if (numEntries < 1)
		numEntries = 1;

	/*
	 * Include predicate in selectivityQuals (should match
	 * genericcostestimate)
	 */
	if (index->indpred != NIL)
	{
		List	   *predExtraQuals = NIL;

		foreach(l, index->indpred)
		{
			Node	   *predQual = (Node *) lfirst(l);
			List	   *oneQual = list_make1(predQual);

			if (!predicate_implied_by(oneQual, indexQuals))
				predExtraQuals = list_concat(predExtraQuals, oneQual);
		}
		/* list_concat avoids modifying the passed-in indexQuals list */
		selectivityQuals = list_concat(predExtraQuals, indexQuals);
	}
	else
		selectivityQuals = indexQuals;

	/* Estimate the fraction of main-table tuples that will be visited */
	*indexSelectivity = clauselist_selectivity(root, selectivityQuals,
											   index->rel->relid,
											   JOIN_INNER,
											   NULL);

	/* fetch estimated page cost for schema containing index */
	get_tablespace_page_costs(index->reltablespace,
							  &spc_random_page_cost,
							  NULL);

	/*
	 * Generic assumption about index correlation: there isn't any.
	 */
	*indexCorrelation = 0.0;

	/*
	 * Examine quals to estimate number of search entries & partial matches
	 */
	memset(&counts, 0, sizeof(counts));
	counts.arrayScans = 1;
	matchPossible = true;

	foreach(l, indexQuals)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);
		Expr	   *clause;

		Assert(IsA(rinfo, RestrictInfo));
		clause = rinfo->clause;
		if (IsA(clause, OpExpr))
		{
			matchPossible = gincost_opexpr(index,
										   (OpExpr *) clause,
										   &counts);
			if (!matchPossible)
				break;
		}
		else if (IsA(clause, ScalarArrayOpExpr))
		{
			matchPossible = gincost_scalararrayopexpr(index,
												(ScalarArrayOpExpr *) clause,
													  numEntries,
													  &counts);
			if (!matchPossible)
				break;
		}
		else
		{
			/* shouldn't be anything else for a GIN index */
			elog(ERROR, "unsupported GIN indexqual type: %d",
				 (int) nodeTag(clause));
		}
	}

	/* Fall out if there were any provably-unsatisfiable quals */
	if (!matchPossible)
	{
		*indexStartupCost = 0;
		*indexTotalCost = 0;
		*indexSelectivity = 0;
		PG_RETURN_VOID();
	}

	if (counts.haveFullScan || indexQuals == NIL)
	{
		/*
		 * Full index scan will be required.  We treat this as if every key in
		 * the index had been listed in the query; is that reasonable?
		 */
		counts.partialEntries = 0;
		counts.exactEntries = numEntries;
		counts.searchEntries = numEntries;
	}

	/* Will we have more than one iteration of a nestloop scan? */
	outer_scans = loop_count;

	/*
	 * Compute cost to begin scan, first of all, pay attention to pending
	 * list.
	 */
	entryPagesFetched = numPendingPages;

	/*
	 * Estimate number of entry pages read.  We need to do
	 * counts.searchEntries searches.  Use a power function as it should be,
	 * but tuples on leaf pages usually is much greater. Here we include all
	 * searches in entry tree, including search of first entry in partial
	 * match algorithm
	 */
	entryPagesFetched += ceil(counts.searchEntries * rint(pow(numEntryPages, 0.15)));

	/*
	 * Add an estimate of entry pages read by partial match algorithm. It's a
	 * scan over leaf pages in entry tree.	We haven't any useful stats here,
	 * so estimate it as proportion.
	 */
	entryPagesFetched += ceil(numEntryPages * counts.partialEntries / numEntries);

	/*
	 * Partial match algorithm reads all data pages before doing actual scan,
	 * so it's a startup cost. Again, we haven't any useful stats here, so,
	 * estimate it as proportion
	 */
	dataPagesFetched = ceil(numDataPages * counts.partialEntries / numEntries);

	/*
	 * Calculate cache effects if more than one scan due to nestloops or array
	 * quals.  The result is pro-rated per nestloop scan, but the array qual
	 * factor shouldn't be pro-rated (compare genericcostestimate).
	 */
	if (outer_scans > 1 || counts.arrayScans > 1)
	{
		entryPagesFetched *= outer_scans * counts.arrayScans;
		entryPagesFetched = index_pages_fetched(entryPagesFetched,
												(BlockNumber) numEntryPages,
												numEntryPages, root);
		entryPagesFetched /= outer_scans;
		dataPagesFetched *= outer_scans * counts.arrayScans;
		dataPagesFetched = index_pages_fetched(dataPagesFetched,
											   (BlockNumber) numDataPages,
											   numDataPages, root);
		dataPagesFetched /= outer_scans;
	}

	/*
	 * Here we use random page cost because logically-close pages could be far
	 * apart on disk.
	 */
	*indexStartupCost = (entryPagesFetched + dataPagesFetched) * spc_random_page_cost;

	/*
	 * Now we compute the number of data pages fetched while the scan
	 * proceeds.
	 */

	/* data pages scanned for each exact (non-partial) matched entry */
	dataPagesFetched = ceil(numDataPages * counts.exactEntries / numEntries);

	/*
	 * Estimate number of data pages read, using selectivity estimation and
	 * capacity of data page.
	 */
	dataPagesFetchedBySel = ceil(*indexSelectivity *
								 (numTuples / (BLCKSZ / SizeOfIptrData)));

	if (dataPagesFetchedBySel > dataPagesFetched)
	{
		/*
		 * At least one of entries is very frequent and, unfortunately, we
		 * couldn't get statistic about entries (only tsvector has such
		 * statistics). So, we obviously have too small estimation of pages
		 * fetched from data tree. Re-estimate it from known capacity of data
		 * pages
		 */
		dataPagesFetched = dataPagesFetchedBySel;
	}

	/* Account for cache effects, the same as above */
	if (outer_scans > 1 || counts.arrayScans > 1)
	{
		dataPagesFetched *= outer_scans * counts.arrayScans;
		dataPagesFetched = index_pages_fetched(dataPagesFetched,
											   (BlockNumber) numDataPages,
											   numDataPages, root);
		dataPagesFetched /= outer_scans;
	}

	/* And apply random_page_cost as the cost per page */
	*indexTotalCost = *indexStartupCost +
		dataPagesFetched * spc_random_page_cost;

	/*
	 * Add on index qual eval costs, much as in genericcostestimate
	 */
	cost_qual_eval(&index_qual_cost, indexQuals, root);
	qual_arg_cost = index_qual_cost.startup + index_qual_cost.per_tuple;
	cost_qual_eval(&index_qual_cost, indexOrderBys, root);
	qual_arg_cost += index_qual_cost.startup + index_qual_cost.per_tuple;
	qual_op_cost = cpu_operator_cost *
		(list_length(indexQuals) + list_length(indexOrderBys));
	qual_arg_cost -= qual_op_cost;
	if (qual_arg_cost < 0)		/* just in case... */
		qual_arg_cost = 0;

	*indexStartupCost += qual_arg_cost;
	*indexTotalCost += qual_arg_cost;
	*indexTotalCost += (numTuples * *indexSelectivity) * (cpu_index_tuple_cost + qual_op_cost);

	PG_RETURN_VOID();
}
