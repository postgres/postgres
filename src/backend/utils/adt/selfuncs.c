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
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/selfuncs.c,v 1.89 2001/05/09 23:13:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>
#include <math.h>
#ifdef USE_LOCALE
#include <locale.h>
#endif

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/int8.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/* N is not a valid var/constant or relation id */
#define NONVALUE(N)		((N) == 0)

/* default selectivity estimate for equalities such as "A = b" */
#define DEFAULT_EQ_SEL	0.01

/* default selectivity estimate for inequalities such as "A < b" */
#define DEFAULT_INEQ_SEL  (1.0 / 3.0)

/* default selectivity estimate for pattern-match operators such as LIKE */
#define DEFAULT_MATCH_SEL	0.01

static bool convert_to_scalar(Datum value, Oid valuetypid, double *scaledvalue,
				  Datum lobound, Datum hibound, Oid boundstypid,
				  double *scaledlobound, double *scaledhibound);
static double convert_numeric_to_scalar(Datum value, Oid typid);
static void convert_string_to_scalar(unsigned char *value,
						 double *scaledvalue,
						 unsigned char *lobound,
						 double *scaledlobound,
						 unsigned char *hibound,
						 double *scaledhibound);
static double convert_one_string_to_scalar(unsigned char *value,
							 int rangelo, int rangehi);
static unsigned char *convert_string_datum(Datum value, Oid typid);
static double convert_timevalue_to_scalar(Datum value, Oid typid);
static void getattproperties(Oid relid, AttrNumber attnum,
							 Oid *typid, int32 *typmod);
static double get_att_numdistinct(Oid relid, AttrNumber attnum, Oid typid,
								  Form_pg_statistic stats);
static Selectivity prefix_selectivity(char *prefix,
				   Oid relid,
				   AttrNumber attno,
				   Oid datatype);
static Selectivity pattern_selectivity(char *patt, Pattern_Type ptype);
static bool string_lessthan(const char *str1, const char *str2,
				Oid datatype);
static Oid	find_operator(const char *opname, Oid datatype);
static Datum string_to_datum(const char *str, Oid datatype);


/*
 *		eqsel			- Selectivity of "=" for any data types.
 *
 * Note: this routine is also used to estimate selectivity for some
 * operators that are not "=" but have comparable selectivity behavior,
 * such as "~=" (geometric approximate-match).	Even for "=", we must
 * keep in mind that the left and right datatypes may differ, so the type
 * of the given constant "value" may be different from the type of the
 * attribute.
 */
Datum
eqsel(PG_FUNCTION_ARGS)
{
	Oid			opid = PG_GETARG_OID(0);
	Oid			relid = PG_GETARG_OID(1);
	AttrNumber	attno = PG_GETARG_INT16(2);
	Datum		value = PG_GETARG_DATUM(3);
	int32		flag = PG_GETARG_INT32(4);
	Oid			typid;
	int32		typmod;
	HeapTuple	statsTuple;
	Datum	   *values;
	int			nvalues;
	float4	   *numbers;
	int			nnumbers;
	double		selec;

	if (NONVALUE(relid) || NONVALUE(attno))
		PG_RETURN_FLOAT8(DEFAULT_EQ_SEL);

	/* get info about the attribute */
	getattproperties(relid, attno, &typid, &typmod);

	/* get stats for the attribute, if available */
	statsTuple = SearchSysCache(STATRELATT,
								ObjectIdGetDatum(relid),
								Int16GetDatum(attno),
								0, 0);
	if (HeapTupleIsValid(statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(statsTuple);

		if (flag & SEL_CONSTANT)
		{
			bool	match = false;
			int		i;

			/*
			 * Is the constant "=" to any of the column's most common
			 * values?  (Although the given operator may not really be
			 * "=", we will assume that seeing whether it returns TRUE
			 * is an appropriate test.  If you don't like this, maybe you
			 * shouldn't be using eqsel for your operator...)
			 */
			if (get_attstatsslot(statsTuple, typid, typmod,
								 STATISTIC_KIND_MCV, InvalidOid,
								 &values, &nvalues,
								 &numbers, &nnumbers))
			{
				FmgrInfo	eqproc;

				fmgr_info(get_opcode(opid), &eqproc);

				for (i = 0; i < nvalues; i++)
				{
					/* be careful to apply operator right way 'round */
					if (flag & SEL_RIGHT)
						match = DatumGetBool(FunctionCall2(&eqproc,
														   values[i],
														   value));
					else
						match = DatumGetBool(FunctionCall2(&eqproc,
														   value,
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
				 * Constant is "=" to this common value.  We know
				 * selectivity exactly (or as exactly as VACUUM
				 * could calculate it, anyway).
				 */
				selec = numbers[i];
			}
			else
			{
				/*
				 * Comparison is against a constant that is neither
				 * NULL nor any of the common values.  Its selectivity
				 * cannot be more than this:
				 */
				double	sumcommon = 0.0;
				double	otherdistinct;

				for (i = 0; i < nnumbers; i++)
					sumcommon += numbers[i];
				selec = 1.0 - sumcommon - stats->stanullfrac;
				/*
				 * and in fact it's probably a good deal less.
				 * We approximate that all the not-common values
				 * share this remaining fraction equally, so we
				 * divide by the number of other distinct values.
				 */
				otherdistinct = get_att_numdistinct(relid, attno,
													typid, stats)
					- nnumbers;
				if (otherdistinct > 1)
					selec /= otherdistinct;
				/*
				 * Another cross-check: selectivity shouldn't be
				 * estimated as more than the least common
				 * "most common value".
				 */
				if (nnumbers > 0 && selec > numbers[nnumbers-1])
					selec = numbers[nnumbers-1];
			}

			free_attstatsslot(typid, values, nvalues, numbers, nnumbers);
		}
		else
		{
			double		ndistinct;

			/*
			 * Search is for a value that we do not know a priori, but
			 * we will assume it is not NULL.  Estimate the selectivity
			 * as non-null fraction divided by number of distinct values,
			 * so that we get a result averaged over all possible values
			 * whether common or uncommon.  (Essentially, we are assuming
			 * that the not-yet-known comparison value is equally likely
			 * to be any of the possible values, regardless of their
			 * frequency in the table.  Is that a good idea?)
			 */
			selec = 1.0 - stats->stanullfrac;
			ndistinct = get_att_numdistinct(relid, attno, typid, stats);
			if (ndistinct > 1)
				selec /= ndistinct;
			/*
			 * Cross-check: selectivity should never be
			 * estimated as more than the most common value's.
			 */
			if (get_attstatsslot(statsTuple, typid, typmod,
								 STATISTIC_KIND_MCV, InvalidOid,
								 NULL, NULL,
								 &numbers, &nnumbers))
			{
				if (nnumbers > 0 && selec > numbers[0])
					selec = numbers[0];
				free_attstatsslot(typid, NULL, 0, numbers, nnumbers);
			}
		}

		ReleaseSysCache(statsTuple);
	}
	else
	{
		/*
		 * No VACUUM ANALYZE stats available, so make a guess using
		 * estimated number of distinct values and assuming they are
		 * equally common.  (The guess is unlikely to be very good,
		 * but we do know a few special cases.)
		 */
		selec = 1.0 / get_att_numdistinct(relid, attno, typid, NULL);
	}

	/* result should be in range, but make sure... */
	if (selec < 0.0)
		selec = 0.0;
	else if (selec > 1.0)
		selec = 1.0;

	PG_RETURN_FLOAT8((float8) selec);
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
	Oid			opid = PG_GETARG_OID(0);
	Oid			relid = PG_GETARG_OID(1);
	AttrNumber	attno = PG_GETARG_INT16(2);
	Datum		value = PG_GETARG_DATUM(3);
	int32		flag = PG_GETARG_INT32(4);
	Oid			eqopid;
	float8		result;

	/*
	 * We want 1 - eqsel() where the equality operator is the one
	 * associated with this != operator, that is, its negator.
	 */
	eqopid = get_negator(opid);
	if (eqopid)
	{
		result = DatumGetFloat8(DirectFunctionCall5(eqsel,
												ObjectIdGetDatum(eqopid),
												 ObjectIdGetDatum(relid),
													Int16GetDatum(attno),
													value,
													Int32GetDatum(flag)));
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
 *		scalarltsel		- Selectivity of "<" (also "<=") for scalars.
 *
 * This routine works for any datatype (or pair of datatypes) known to
 * convert_to_scalar().  If it is applied to some other datatype,
 * it will return a default estimate.
 */
Datum
scalarltsel(PG_FUNCTION_ARGS)
{
	Oid			opid = PG_GETARG_OID(0);
	Oid			relid = PG_GETARG_OID(1);
	AttrNumber	attno = PG_GETARG_INT16(2);
	Datum		value = PG_GETARG_DATUM(3);
	int32		flag = PG_GETARG_INT32(4);
	bool		isgt;
	HeapTuple	oprTuple;
	HeapTuple	statsTuple;
	Form_pg_statistic stats;
	Oid			contype;
	FmgrInfo	opproc;
	Oid			typid;
	int32		typmod;
	Datum	   *values;
	int			nvalues;
	float4	   *numbers;
	int			nnumbers;
	double		mcv_selec,
				hist_selec,
				sumcommon;
	double		selec;
	int			i;

	if (NONVALUE(relid) || NONVALUE(attno))
		PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);

	/* Can't do anything useful if no constant to compare against, either */
	if (!(flag & SEL_CONSTANT))
		PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);

	/*
	 * Force the constant to be on the right to simplify later logic.
	 * This means that we may be dealing with either "<" or ">" cases.
	 */
	if (flag & SEL_RIGHT)
	{
		/* we have x < const */
		isgt = false;
	}
	else
	{
		/* we have const < x, commute to make x > const */
		opid = get_commutator(opid);
		if (!opid)
		{
			/* Use default selectivity (should we raise an error instead?) */
			PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
		}
		isgt = true;
	}

	/*
	 * The constant might not be the same datatype as the column;
	 * look at the operator's input types to find out what it is.
	 * Also set up to be able to call the operator's execution proc.
	 */
	oprTuple = SearchSysCache(OPEROID,
							  ObjectIdGetDatum(opid),
							  0, 0, 0);
	if (!HeapTupleIsValid(oprTuple))
		elog(ERROR, "scalarltsel: no tuple for operator %u", opid);
	contype = ((Form_pg_operator) GETSTRUCT(oprTuple))->oprright;
	fmgr_info(((Form_pg_operator) GETSTRUCT(oprTuple))->oprcode, &opproc);
	ReleaseSysCache(oprTuple);

	/* Now get info and stats about the attribute */
	getattproperties(relid, attno, &typid, &typmod);

	statsTuple = SearchSysCache(STATRELATT,
								ObjectIdGetDatum(relid),
								Int16GetDatum(attno),
								0, 0);
	if (!HeapTupleIsValid(statsTuple))
	{
		/* no stats available, so default result */
		PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
	}
	stats = (Form_pg_statistic) GETSTRUCT(statsTuple);

	/*
	 * If we have most-common-values info, add up the fractions of the
	 * MCV entries that satisfy MCV OP CONST.  These fractions contribute
	 * directly to the result selectivity.  Also add up the total fraction
	 * represented by MCV entries.
	 */
	mcv_selec = 0.0;
	sumcommon = 0.0;

	if (get_attstatsslot(statsTuple, typid, typmod,
						 STATISTIC_KIND_MCV, InvalidOid,
						 &values, &nvalues,
						 &numbers, &nnumbers))
	{
		for (i = 0; i < nvalues; i++)
		{
			if (DatumGetBool(FunctionCall2(&opproc,
										   values[i],
										   value)))
				mcv_selec += numbers[i];
			sumcommon += numbers[i];
		}
		free_attstatsslot(typid, values, nvalues, numbers, nnumbers);
	}

	/*
	 * If there is a histogram, determine which bin the constant falls in,
	 * and compute the resulting contribution to selectivity.
	 *
	 * Someday, VACUUM might store more than one histogram per rel/att,
	 * corresponding to more than one possible sort ordering defined for
	 * the column type.  However, to make that work we will need to figure
	 * out which staop to search for --- it's not necessarily the one we
	 * have at hand!  (For example, we might have a '<=' operator rather
	 * than the '<' operator that will appear in staop.)  For now, assume
	 * that whatever appears in pg_statistic is sorted the same way our
	 * operator sorts.
	 */
	hist_selec = 0.0;

	if (get_attstatsslot(statsTuple, typid, typmod,
						 STATISTIC_KIND_HISTOGRAM, InvalidOid,
						 &values, &nvalues,
						 NULL, NULL))
	{
		if (nvalues > 1)
		{
			double	histfrac;
			bool	ltcmp;

			ltcmp = DatumGetBool(FunctionCall2(&opproc,
											   values[0],
											   value));
			if (isgt)
				ltcmp = !ltcmp;
			if (!ltcmp)
			{
				/* Constant is below lower histogram boundary. */
				histfrac = 0.0;
			}
			else
			{
				/*
				 * Scan to find proper location.  This could be made faster
				 * by using a binary-search method, but it's probably not
				 * worth the trouble for typical histogram sizes.
				 */
				for (i = 1; i < nvalues; i++)
				{
					ltcmp = DatumGetBool(FunctionCall2(&opproc,
													   values[i],
													   value));
					if (isgt)
						ltcmp = !ltcmp;
					if (!ltcmp)
						break;
				}
				if (i >= nvalues)
				{
					/* Constant is above upper histogram boundary. */
					histfrac = 1.0;
				}
				else
				{
					double		val,
								high,
								low;
					double		binfrac;

					/*
					 * We have values[i-1] < constant < values[i].
					 *
					 * Convert the constant and the two nearest bin boundary
					 * values to a uniform comparison scale, and do a linear
					 * interpolation within this bin.
					 */
					if (convert_to_scalar(value, contype, &val,
										  values[i-1], values[i], typid,
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
							binfrac = (val - low) / (high - low);
					}
					else
					{
						/*
						 * Ideally we'd produce an error here, on the grounds
						 * that the given operator shouldn't have scalarltsel
						 * registered as its selectivity func unless we can
						 * deal with its operand types.  But currently, all
						 * manner of stuff is invoking scalarltsel, so give a
						 * default estimate until that can be fixed.
						 */
						binfrac = 0.5;
					}
					/*
					 * Now, compute the overall selectivity across the values
					 * represented by the histogram.  We have i-1 full bins
					 * and binfrac partial bin below the constant.
					 */
					histfrac = (double) (i-1) + binfrac;
					histfrac /= (double) (nvalues - 1);
				}
			}
			/*
			 * Now histfrac = fraction of histogram entries below the constant.
			 *
			 * Account for "<" vs ">"
			 */
			hist_selec = isgt ? (1.0 - histfrac) : histfrac;
			/*
			 * The histogram boundaries are only approximate to begin
			 * with, and may well be out of date anyway.  Therefore,
			 * don't believe extremely small or large selectivity
			 * estimates.
			 */
			if (hist_selec < 0.001)
				hist_selec = 0.001;
			else if (hist_selec > 0.999)
				hist_selec = 0.999;
		}

		free_attstatsslot(typid, values, nvalues, NULL, 0);
	}

	/*
	 * Now merge the results from the MCV and histogram calculations,
	 * realizing that the histogram covers only the non-null values that
	 * are not listed in MCV.
	 */
	selec = 1.0 - stats->stanullfrac - sumcommon;

	if (hist_selec > 0.0)
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

	ReleaseSysCache(statsTuple);

	/* result should be in range, but make sure... */
	if (selec < 0.0)
		selec = 0.0;
	else if (selec > 1.0)
		selec = 1.0;

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 *		scalargtsel		- Selectivity of ">" (also ">=") for integers.
 *
 * See above comments for scalarltsel.
 */
Datum
scalargtsel(PG_FUNCTION_ARGS)
{
	Oid			opid = PG_GETARG_OID(0);
	Oid			relid = PG_GETARG_OID(1);
	AttrNumber	attno = PG_GETARG_INT16(2);
	Datum		value = PG_GETARG_DATUM(3);
	int32		flag = PG_GETARG_INT32(4);
	Oid			ltopid;

	/*
	 * Commute so that we have a "<" or "<=" operator, then apply
	 * scalarltsel.
	 */
	ltopid = get_commutator(opid);
	if (!ltopid)
	{
		/* Use default selectivity (should we raise an error instead?) */
		PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
	}

	flag ^= SEL_RIGHT;
	return DirectFunctionCall5(scalarltsel,
							   ObjectIdGetDatum(ltopid),
							   ObjectIdGetDatum(relid),
							   Int16GetDatum(attno),
							   value,
							   Int32GetDatum(flag));
}

/*
 * patternsel			- Generic code for pattern-match selectivity.
 */
static Datum
patternsel(PG_FUNCTION_ARGS, Pattern_Type ptype)
{
	Oid			opid = PG_GETARG_OID(0);
	Oid			relid = PG_GETARG_OID(1);
	AttrNumber	attno = PG_GETARG_INT16(2);
	Datum		value = PG_GETARG_DATUM(3);
	int32		flag = PG_GETARG_INT32(4);
	float8		result;

	/* Must have a constant for the pattern, or cannot learn anything */
	if ((flag & (SEL_CONSTANT | SEL_RIGHT)) != (SEL_CONSTANT | SEL_RIGHT))
		result = DEFAULT_MATCH_SEL;
	else
	{
		HeapTuple	oprTuple;
		Oid			ltype,
					rtype;
		char	   *patt;
		Pattern_Prefix_Status pstatus;
		char	   *prefix;
		char	   *rest;

		/*
		 * Get left and right datatypes of the operator so we know what
		 * type the attribute is.
		 */
		oprTuple = SearchSysCache(OPEROID,
								  ObjectIdGetDatum(opid),
								  0, 0, 0);
		if (!HeapTupleIsValid(oprTuple))
			elog(ERROR, "patternsel: no tuple for operator %u", opid);
		ltype = ((Form_pg_operator) GETSTRUCT(oprTuple))->oprleft;
		rtype = ((Form_pg_operator) GETSTRUCT(oprTuple))->oprright;
		ReleaseSysCache(oprTuple);

		/* the right-hand const is type text for all supported operators */
		Assert(rtype == TEXTOID);
		patt = DatumGetCString(DirectFunctionCall1(textout, value));

		/* divide pattern into fixed prefix and remainder */
		pstatus = pattern_fixed_prefix(patt, ptype, &prefix, &rest);

		if (pstatus == Pattern_Prefix_Exact)
		{

			/*
			 * Pattern specifies an exact match, so pretend operator is
			 * '='
			 */
			Oid			eqopr = find_operator("=", ltype);
			Datum		eqcon;

			if (eqopr == InvalidOid)
				elog(ERROR, "patternsel: no = operator for type %u", ltype);
			eqcon = string_to_datum(prefix, ltype);
			result = DatumGetFloat8(DirectFunctionCall5(eqsel,
												 ObjectIdGetDatum(eqopr),
												 ObjectIdGetDatum(relid),
													Int16GetDatum(attno),
														eqcon,
							   Int32GetDatum(SEL_CONSTANT | SEL_RIGHT)));
			pfree(DatumGetPointer(eqcon));
		}
		else
		{

			/*
			 * Not exact-match pattern.  We estimate selectivity of the
			 * fixed prefix and remainder of pattern separately, then
			 * combine the two.
			 */
			Selectivity prefixsel;
			Selectivity restsel;
			Selectivity selec;

			if (pstatus == Pattern_Prefix_Partial)
				prefixsel = prefix_selectivity(prefix, relid, attno, ltype);
			else
				prefixsel = 1.0;
			restsel = pattern_selectivity(rest, ptype);
			selec = prefixsel * restsel;
			/* result should be in range, but make sure... */
			if (selec < 0.0)
				selec = 0.0;
			else if (selec > 1.0)
				selec = 1.0;
			result = (float8) selec;
		}
		if (prefix)
			pfree(prefix);
		pfree(patt);
	}
	PG_RETURN_FLOAT8(result);
}

/*
 *		regexeqsel		- Selectivity of regular-expression pattern match.
 */
Datum
regexeqsel(PG_FUNCTION_ARGS)
{
	return patternsel(fcinfo, Pattern_Type_Regex);
}

/*
 *		icregexeqsel	- Selectivity of case-insensitive regex match.
 */
Datum
icregexeqsel(PG_FUNCTION_ARGS)
{
	return patternsel(fcinfo, Pattern_Type_Regex_IC);
}

/*
 *		likesel			- Selectivity of LIKE pattern match.
 */
Datum
likesel(PG_FUNCTION_ARGS)
{
	return patternsel(fcinfo, Pattern_Type_Like);
}

/*
 *		iclikesel			- Selectivity of ILIKE pattern match.
 */
Datum
iclikesel(PG_FUNCTION_ARGS)
{
	return patternsel(fcinfo, Pattern_Type_Like_IC);
}

/*
 *		regexnesel		- Selectivity of regular-expression pattern non-match.
 */
Datum
regexnesel(PG_FUNCTION_ARGS)
{
	float8		result;

	result = DatumGetFloat8(patternsel(fcinfo, Pattern_Type_Regex));
	result = 1.0 - result;
	PG_RETURN_FLOAT8(result);
}

/*
 *		icregexnesel	- Selectivity of case-insensitive regex non-match.
 */
Datum
icregexnesel(PG_FUNCTION_ARGS)
{
	float8		result;

	result = DatumGetFloat8(patternsel(fcinfo, Pattern_Type_Regex_IC));
	result = 1.0 - result;
	PG_RETURN_FLOAT8(result);
}

/*
 *		nlikesel		- Selectivity of LIKE pattern non-match.
 */
Datum
nlikesel(PG_FUNCTION_ARGS)
{
	float8		result;

	result = DatumGetFloat8(patternsel(fcinfo, Pattern_Type_Like));
	result = 1.0 - result;
	PG_RETURN_FLOAT8(result);
}

/*
 *		icnlikesel		- Selectivity of ILIKE pattern non-match.
 */
Datum
icnlikesel(PG_FUNCTION_ARGS)
{
	float8		result;

	result = DatumGetFloat8(patternsel(fcinfo, Pattern_Type_Like_IC));
	result = 1.0 - result;
	PG_RETURN_FLOAT8(result);
}

/*
 *		eqjoinsel		- Join selectivity of "="
 */
Datum
eqjoinsel(PG_FUNCTION_ARGS)
{
#ifdef NOT_USED					/* see neqjoinsel() before removing me! */
	Oid			opid = PG_GETARG_OID(0);
#endif
	Oid			relid1 = PG_GETARG_OID(1);
	AttrNumber	attno1 = PG_GETARG_INT16(2);
	Oid			relid2 = PG_GETARG_OID(3);
	AttrNumber	attno2 = PG_GETARG_INT16(4);
	bool		unknown1 = NONVALUE(relid1) || NONVALUE(attno1);
	bool		unknown2 = NONVALUE(relid2) || NONVALUE(attno2);
	double		selec;

	if (unknown1 && unknown2)
		selec = DEFAULT_EQ_SEL;
	else
	{
		Oid			typid1;
		Oid			typid2;
		int32		typmod1;
		int32		typmod2;
		HeapTuple	statsTuple1 = NULL;
		HeapTuple	statsTuple2 = NULL;
		Form_pg_statistic stats1 = NULL;
		Form_pg_statistic stats2 = NULL;
		double		nd1,
					nd2;

		if (unknown1)
		{
			nd1 = 100.0;
		}
		else
		{
			/* get info about the attribute */
			getattproperties(relid1, attno1, &typid1, &typmod1);

			/* get stats for the attribute, if available */
			statsTuple1 = SearchSysCache(STATRELATT,
										 ObjectIdGetDatum(relid1),
										 Int16GetDatum(attno1),
										 0, 0);
			if (HeapTupleIsValid(statsTuple1))
				stats1 = (Form_pg_statistic) GETSTRUCT(statsTuple1);

			nd1 = get_att_numdistinct(relid1, attno1, typid1, stats1);
		}

		if (unknown2)
		{
			nd2 = 100.0;
		}
		else
		{
			/* get info about the attribute */
			getattproperties(relid2, attno2, &typid2, &typmod2);

			/* get stats for the attribute, if available */
			statsTuple2 = SearchSysCache(STATRELATT,
										 ObjectIdGetDatum(relid2),
										 Int16GetDatum(attno2),
										 0, 0);
			if (HeapTupleIsValid(statsTuple2))
				stats2 = (Form_pg_statistic) GETSTRUCT(statsTuple2);

			nd2 = get_att_numdistinct(relid2, attno2, typid2, stats2);
		}

		/*
		 * Estimate the join selectivity as 1 / sqrt(nd1*nd2)
		 * (can we produce any theory for this)?
		 *
		 * XXX possibility to do better: if both attributes have histograms
		 * then we could determine the exact join selectivity between the
		 * MCV sets, and only have to assume the join behavior of the non-MCV
		 * values.  This could be a big win when the MCVs cover a large part
		 * of the population.
		 *
		 * XXX what about nulls?
		 */
		selec = 1.0 / sqrt(nd1 * nd2);
		if (selec > 1.0)
			selec = 1.0;

		if (HeapTupleIsValid(statsTuple1))
			ReleaseSysCache(statsTuple1);
		if (HeapTupleIsValid(statsTuple2))
			ReleaseSysCache(statsTuple2);

	}
	PG_RETURN_FLOAT8((float8) selec);
}

/*
 *		neqjoinsel		- Join selectivity of "!="
 */
Datum
neqjoinsel(PG_FUNCTION_ARGS)
{
	float8		result;

	/*
	 * XXX we skip looking up the negator operator here because we know
	 * eqjoinsel() won't look at it anyway.  If eqjoinsel() ever does
	 * look, this routine will need to look more like neqsel() does.
	 */
	result = DatumGetFloat8(eqjoinsel(fcinfo));
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
 *		regexeqjoinsel	- Join selectivity of regular-expression pattern match.
 */
Datum
regexeqjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_MATCH_SEL);
}

/*
 *		icregexeqjoinsel	- Join selectivity of case-insensitive regex match.
 */
Datum
icregexeqjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_MATCH_SEL);
}

/*
 *		likejoinsel			- Join selectivity of LIKE pattern match.
 */
Datum
likejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_MATCH_SEL);
}

/*
 *		iclikejoinsel			- Join selectivity of ILIKE pattern match.
 */
Datum
iclikejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_MATCH_SEL);
}

/*
 *		regexnejoinsel	- Join selectivity of regex non-match.
 */
Datum
regexnejoinsel(PG_FUNCTION_ARGS)
{
	float8		result;

	result = DatumGetFloat8(regexeqjoinsel(fcinfo));
	result = 1.0 - result;
	PG_RETURN_FLOAT8(result);
}

/*
 *		icregexnejoinsel	- Join selectivity of case-insensitive regex non-match.
 */
Datum
icregexnejoinsel(PG_FUNCTION_ARGS)
{
	float8		result;

	result = DatumGetFloat8(icregexeqjoinsel(fcinfo));
	result = 1.0 - result;
	PG_RETURN_FLOAT8(result);
}

/*
 *		nlikejoinsel		- Join selectivity of LIKE pattern non-match.
 */
Datum
nlikejoinsel(PG_FUNCTION_ARGS)
{
	float8		result;

	result = DatumGetFloat8(likejoinsel(fcinfo));
	result = 1.0 - result;
	PG_RETURN_FLOAT8(result);
}

/*
 *		icnlikejoinsel		- Join selectivity of ILIKE pattern non-match.
 */
Datum
icnlikejoinsel(PG_FUNCTION_ARGS)
{
	float8		result;

	result = DatumGetFloat8(iclikejoinsel(fcinfo));
	result = 1.0 - result;
	PG_RETURN_FLOAT8(result);
}


/*
 * convert_to_scalar
 *	  Convert non-NULL values of the indicated types to the comparison
 *	  scale needed by scalarltsel()/scalargtsel().
 *	  Returns "true" if successful.
 *
 * All numeric datatypes are simply converted to their equivalent
 * "double" values.  XXX what about NUMERIC values that are outside
 * the range of "double"?
 *
 * String datatypes are converted by convert_string_to_scalar(),
 * which is explained below.  The reason why this routine deals with
 * three values at a time, not just one, is that we need it for strings.
 *
 * The several datatypes representing absolute times are all converted
 * to Timestamp, which is actually a double, and then we just use that
 * double value.  Note this will give bad results for the various "special"
 * values of Timestamp --- what can we do with those?
 *
 * The several datatypes representing relative times (intervals) are all
 * converted to measurements expressed in seconds.
 */
static bool
convert_to_scalar(Datum value, Oid valuetypid, double *scaledvalue,
				  Datum lobound, Datum hibound, Oid boundstypid,
				  double *scaledlobound, double *scaledhibound)
{
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
				unsigned char *valstr = convert_string_datum(value, valuetypid);
				unsigned char *lostr = convert_string_datum(lobound, boundstypid);
				unsigned char *histr = convert_string_datum(hibound, boundstypid);

				convert_string_to_scalar(valstr, scaledvalue,
										 lostr, scaledlobound,
										 histr, scaledhibound);
				pfree(valstr);
				pfree(lostr);
				pfree(histr);
				return true;
			}

			/*
			 * Built-in time types
			 */
		case TIMESTAMPOID:
		case ABSTIMEOID:
		case DATEOID:
		case INTERVALOID:
		case RELTIMEOID:
		case TINTERVALOID:
		case TIMEOID:
			*scaledvalue = convert_timevalue_to_scalar(value, valuetypid);
			*scaledlobound = convert_timevalue_to_scalar(lobound, boundstypid);
			*scaledhibound = convert_timevalue_to_scalar(hibound, boundstypid);
			return true;
	}
	/* Don't know how to convert */
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
			return (double) DatumGetFloat8(DirectFunctionCall1(numeric_float8,
															   value));
		case OIDOID:
		case REGPROCOID:
			/* we can treat OIDs as integers... */
			return (double) DatumGetObjectId(value);
	}

	/*
	 * Can't get here unless someone tries to use scalarltsel/scalargtsel
	 * on an operator with one numeric and one non-numeric operand.
	 */
	elog(ERROR, "convert_numeric_to_scalar: unsupported type %u", typid);
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
convert_string_to_scalar(unsigned char *value,
						 double *scaledvalue,
						 unsigned char *lobound,
						 double *scaledlobound,
						 unsigned char *hibound,
						 double *scaledhibound)
{
	int			rangelo,
				rangehi;
	unsigned char *sptr;

	rangelo = rangehi = hibound[0];
	for (sptr = lobound; *sptr; sptr++)
	{
		if (rangelo > *sptr)
			rangelo = *sptr;
		if (rangehi < *sptr)
			rangehi = *sptr;
	}
	for (sptr = hibound; *sptr; sptr++)
	{
		if (rangelo > *sptr)
			rangelo = *sptr;
		if (rangehi < *sptr)
			rangehi = *sptr;
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
convert_one_string_to_scalar(unsigned char *value, int rangelo, int rangehi)
{
	int			slen = strlen((char *) value);
	double		num,
				denom,
				base;

	if (slen <= 0)
		return 0.0;				/* empty string has scalar value 0 */

	/*
	 * Since base is at least 10, need not consider more than about 20
	 * chars
	 */
	if (slen > 20)
		slen = 20;

	/* Convert initial characters to fraction */
	base = rangehi - rangelo + 1;
	num = 0.0;
	denom = base;
	while (slen-- > 0)
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
 * Convert a string-type Datum into a palloc'd, null-terminated string.
 *
 * If USE_LOCALE is defined, we must pass the string through strxfrm()
 * before continuing, so as to generate correct locale-specific results.
 */
static unsigned char *
convert_string_datum(Datum value, Oid typid)
{
	char	   *val;

#ifdef USE_LOCALE
	char	   *xfrmstr;
	size_t		xfrmsize;
	size_t		xfrmlen;

#endif

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
			{
				char	   *str = (char *) VARDATA(DatumGetPointer(value));
				int			strlength = VARSIZE(DatumGetPointer(value)) - VARHDRSZ;

				val = (char *) palloc(strlength + 1);
				memcpy(val, str, strlength);
				val[strlength] = '\0';
				break;
			}
		case NAMEOID:
			{
				NameData   *nm = (NameData *) DatumGetPointer(value);

				val = pstrdup(NameStr(*nm));
				break;
			}
		default:

			/*
			 * Can't get here unless someone tries to use scalarltsel on
			 * an operator with one string and one non-string operand.
			 */
			elog(ERROR, "convert_string_datum: unsupported type %u", typid);
			return NULL;
	}

#ifdef USE_LOCALE
	/* Guess that transformed string is not much bigger than original */
	xfrmsize = strlen(val) + 32;/* arbitrary pad value here... */
	xfrmstr = (char *) palloc(xfrmsize);
	xfrmlen = strxfrm(xfrmstr, val, xfrmsize);
	if (xfrmlen >= xfrmsize)
	{
		/* Oops, didn't make it */
		pfree(xfrmstr);
		xfrmstr = (char *) palloc(xfrmlen + 1);
		xfrmlen = strxfrm(xfrmstr, val, xfrmlen + 1);
	}
	pfree(val);
	val = xfrmstr;
#endif

	return (unsigned char *) val;
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
		case ABSTIMEOID:
			return DatumGetTimestamp(DirectFunctionCall1(abstime_timestamp,
														 value));
		case DATEOID:
			return DatumGetTimestamp(DirectFunctionCall1(date_timestamp,
														 value));
		case INTERVALOID:
			{
				Interval   *interval = DatumGetIntervalP(value);

				/*
				 * Convert the month part of Interval to days using
				 * assumed average month length of 365.25/12.0 days.  Not
				 * too accurate, but plenty good enough for our purposes.
				 */
				return interval->time +
					interval->month * (365.25 / 12.0 * 24.0 * 60.0 * 60.0);
			}
		case RELTIMEOID:
			return DatumGetRelativeTime(value);
		case TINTERVALOID:
			{
				TimeInterval interval = DatumGetTimeInterval(value);

				if (interval->status != 0)
					return interval->data[1] - interval->data[0];
				return 0;		/* for lack of a better idea */
			}
		case TIMEOID:
			return DatumGetTimeADT(value);
	}

	/*
	 * Can't get here unless someone tries to use scalarltsel/scalargtsel
	 * on an operator with one timevalue and one non-timevalue operand.
	 */
	elog(ERROR, "convert_timevalue_to_scalar: unsupported type %u", typid);
	return 0;
}


/*
 * getattproperties
 *	  Retrieve pg_attribute properties for an attribute,
 *	  including type OID and typmod.
 */
static void
getattproperties(Oid relid, AttrNumber attnum,
				 Oid *typid, int32 *typmod)
{
	HeapTuple	atp;
	Form_pg_attribute att_tup;

	atp = SearchSysCache(ATTNUM,
						 ObjectIdGetDatum(relid),
						 Int16GetDatum(attnum),
						 0, 0);
	if (!HeapTupleIsValid(atp))
		elog(ERROR, "getattproperties: no attribute tuple %u %d",
			 relid, (int) attnum);
	att_tup = (Form_pg_attribute) GETSTRUCT(atp);

	*typid = att_tup->atttypid;
	*typmod = att_tup->atttypmod;

	ReleaseSysCache(atp);
}

/*
 * get_att_numdistinct
 *
 *	  Estimate the number of distinct values of an attribute.
 *
 * relid, attnum: identify the attribute to examine.
 * typid: type of attribute.
 * stats: pg_statistic tuple for attribute, or NULL if not available.
 *
 * XXX possible future improvement: look to see if there is a unique
 * index on the attribute.  If so, we can estimate ndistinct = ntuples.
 * This should probably override any info from pg_statistic.
 */
static double
get_att_numdistinct(Oid relid, AttrNumber attnum, Oid typid,
					Form_pg_statistic stats)
{
	HeapTuple	reltup;
	double		ntuples;

	/*
	 * Special-case boolean columns: presumably, two distinct values.
	 *
	 * Are there any other cases we should wire in special estimates for?
	 */
	if (typid == BOOLOID)
		return 2.0;

	/*
	 * If VACUUM ANALYZE determined a fixed estimate, use it.
	 */
	if (stats && stats->stadistinct > 0.0)
		return stats->stadistinct;

	/*
	 * Otherwise we need to get the relation size.
	 */
	reltup = SearchSysCache(RELOID,
							ObjectIdGetDatum(relid),
							0, 0, 0);
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "get_att_numdistinct: no relation tuple %u", relid);

	ntuples = ((Form_pg_class) GETSTRUCT(reltup))->reltuples;

	ReleaseSysCache(reltup);

	if (ntuples <= 0.0)
		return 100.0;			/* no data available; return a default */

	/*
	 * If VACUUM ANALYZE determined a scaled estimate, use it.
	 */
	if (stats && stats->stadistinct < 0.0)
		return - stats->stadistinct * ntuples;

	/*
	 * VACUUM ANALYZE does not compute stats for system attributes,
	 * but some of them can reasonably be assumed unique anyway.
	 */
	switch (attnum)
	{
		case ObjectIdAttributeNumber:
		case SelfItemPointerAttributeNumber:
			return ntuples;
		case TableOidAttributeNumber:
			return 1.0;
	}

	/*
	 * Estimate ndistinct = ntuples if the table is small, else 100.
	 */
	if (ntuples < 100.0)
		return ntuples;

	return 100.0;
}

/*-------------------------------------------------------------------------
 *
 * Pattern analysis functions
 *
 * These routines support analysis of LIKE and regular-expression patterns
 * by the planner/optimizer.  It's important that they agree with the
 * regular-expression code in backend/regex/ and the LIKE code in
 * backend/utils/adt/like.c.
 *
 * Note that the prefix-analysis functions are called from
 * backend/optimizer/path/indxpath.c as well as from routines in this file.
 *
 *-------------------------------------------------------------------------
 */

/*
 * Extract the fixed prefix, if any, for a pattern.
 * *prefix is set to a palloc'd prefix string,
 * or to NULL if no fixed prefix exists for the pattern.
 * *rest is set to point to the remainder of the pattern after the
 * portion describing the fixed prefix.
 * The return value distinguishes no fixed prefix, a partial prefix,
 * or an exact-match-only pattern.
 */

static Pattern_Prefix_Status
like_fixed_prefix(char *patt, bool case_insensitive,
				  char **prefix, char **rest)
{
	char	   *match;
	int			pos,
				match_pos;

	*prefix = match = palloc(strlen(patt) + 1);
	match_pos = 0;

	for (pos = 0; patt[pos]; pos++)
	{
		/* % and _ are wildcard characters in LIKE */
		if (patt[pos] == '%' ||
			patt[pos] == '_')
			break;
		/* Backslash quotes the next character */
		if (patt[pos] == '\\')
		{
			pos++;
			if (patt[pos] == '\0')
				break;
		}

		/*
		 * XXX I suspect isalpha() is not an adequately locale-sensitive
		 * test for characters that can vary under case folding?
		 */
		if (case_insensitive && isalpha((unsigned char) patt[pos]))
			break;

		/*
		 * NOTE: this code used to think that %% meant a literal %, but
		 * textlike() itself does not think that, and the SQL92 spec
		 * doesn't say any such thing either.
		 */
		match[match_pos++] = patt[pos];
	}

	match[match_pos] = '\0';
	*rest = &patt[pos];

	/* in LIKE, an empty pattern is an exact match! */
	if (patt[pos] == '\0')
		return Pattern_Prefix_Exact;	/* reached end of pattern, so
										 * exact */

	if (match_pos > 0)
		return Pattern_Prefix_Partial;

	pfree(match);
	*prefix = NULL;
	return Pattern_Prefix_None;
}

static Pattern_Prefix_Status
regex_fixed_prefix(char *patt, bool case_insensitive,
				   char **prefix, char **rest)
{
	char	   *match;
	int			pos,
				match_pos,
				paren_depth;

	/* Pattern must be anchored left */
	if (patt[0] != '^')
	{
		*prefix = NULL;
		*rest = patt;
		return Pattern_Prefix_None;
	}

	/*
	 * If unquoted | is present at paren level 0 in pattern, then there
	 * are multiple alternatives for the start of the string.
	 */
	paren_depth = 0;
	for (pos = 1; patt[pos]; pos++)
	{
		if (patt[pos] == '|' && paren_depth == 0)
		{
			*prefix = NULL;
			*rest = patt;
			return Pattern_Prefix_None;
		}
		else if (patt[pos] == '(')
			paren_depth++;
		else if (patt[pos] == ')' && paren_depth > 0)
			paren_depth--;
		else if (patt[pos] == '\\')
		{
			/* backslash quotes the next character */
			pos++;
			if (patt[pos] == '\0')
				break;
		}
	}

	/* OK, allocate space for pattern */
	*prefix = match = palloc(strlen(patt) + 1);
	match_pos = 0;

	/* note start at pos 1 to skip leading ^ */
	for (pos = 1; patt[pos]; pos++)
	{

		/*
		 * Check for characters that indicate multiple possible matches
		 * here. XXX I suspect isalpha() is not an adequately
		 * locale-sensitive test for characters that can vary under case
		 * folding?
		 */
		if (patt[pos] == '.' ||
			patt[pos] == '(' ||
			patt[pos] == '[' ||
			patt[pos] == '$' ||
			(case_insensitive && isalpha((unsigned char) patt[pos])))
			break;

		/*
		 * Check for quantifiers.  Except for +, this means the preceding
		 * character is optional, so we must remove it from the prefix
		 * too!
		 */
		if (patt[pos] == '*' ||
			patt[pos] == '?' ||
			patt[pos] == '{')
		{
			if (match_pos > 0)
				match_pos--;
			pos--;
			break;
		}
		if (patt[pos] == '+')
		{
			pos--;
			break;
		}
		if (patt[pos] == '\\')
		{
			/* backslash quotes the next character */
			pos++;
			if (patt[pos] == '\0')
				break;
		}
		match[match_pos++] = patt[pos];
	}

	match[match_pos] = '\0';
	*rest = &patt[pos];

	if (patt[pos] == '$' && patt[pos + 1] == '\0')
	{
		*rest = &patt[pos + 1];
		return Pattern_Prefix_Exact;	/* pattern specifies exact match */
	}

	if (match_pos > 0)
		return Pattern_Prefix_Partial;

	pfree(match);
	*prefix = NULL;
	return Pattern_Prefix_None;
}

Pattern_Prefix_Status
pattern_fixed_prefix(char *patt, Pattern_Type ptype,
					 char **prefix, char **rest)
{
	Pattern_Prefix_Status result;

	switch (ptype)
	{
		case Pattern_Type_Like:
			result = like_fixed_prefix(patt, false, prefix, rest);
			break;
		case Pattern_Type_Like_IC:
			result = like_fixed_prefix(patt, true, prefix, rest);
			break;
		case Pattern_Type_Regex:
			result = regex_fixed_prefix(patt, false, prefix, rest);
			break;
		case Pattern_Type_Regex_IC:
			result = regex_fixed_prefix(patt, true, prefix, rest);
			break;
		default:
			elog(ERROR, "pattern_fixed_prefix: bogus ptype");
			result = Pattern_Prefix_None;		/* keep compiler quiet */
			break;
	}
	return result;
}

/*
 * Estimate the selectivity of a fixed prefix for a pattern match.
 *
 * A fixed prefix "foo" is estimated as the selectivity of the expression
 * "var >= 'foo' AND var < 'fop'" (see also indxqual.c).
 *
 * XXX Note: we make use of the upper bound to estimate operator selectivity
 * even if the locale is such that we cannot rely on the upper-bound string.
 * The selectivity only needs to be approximately right anyway, so it seems
 * more useful to use the upper-bound code than not.
 */
static Selectivity
prefix_selectivity(char *prefix,
				   Oid relid,
				   AttrNumber attno,
				   Oid datatype)
{
	Selectivity prefixsel;
	Oid			cmpopr;
	Datum		prefixcon;
	char	   *greaterstr;

	cmpopr = find_operator(">=", datatype);
	if (cmpopr == InvalidOid)
		elog(ERROR, "prefix_selectivity: no >= operator for type %u",
			 datatype);
	prefixcon = string_to_datum(prefix, datatype);
	/* Assume scalargtsel is appropriate for all supported types */
	prefixsel = DatumGetFloat8(DirectFunctionCall5(scalargtsel,
												ObjectIdGetDatum(cmpopr),
												 ObjectIdGetDatum(relid),
												   Int16GetDatum(attno),
												   prefixcon,
							   Int32GetDatum(SEL_CONSTANT | SEL_RIGHT)));
	pfree(DatumGetPointer(prefixcon));

	/*-------
	 * If we can create a string larger than the prefix, say
	 *	"x < greaterstr".
	 *-------
	 */
	greaterstr = make_greater_string(prefix, datatype);
	if (greaterstr)
	{
		Selectivity topsel;

		cmpopr = find_operator("<", datatype);
		if (cmpopr == InvalidOid)
			elog(ERROR, "prefix_selectivity: no < operator for type %u",
				 datatype);
		prefixcon = string_to_datum(greaterstr, datatype);
		/* Assume scalarltsel is appropriate for all supported types */
		topsel = DatumGetFloat8(DirectFunctionCall5(scalarltsel,
												ObjectIdGetDatum(cmpopr),
												 ObjectIdGetDatum(relid),
													Int16GetDatum(attno),
													prefixcon,
							   Int32GetDatum(SEL_CONSTANT | SEL_RIGHT)));
		pfree(DatumGetPointer(prefixcon));
		pfree(greaterstr);

		/*
		 * Merge the two selectivities in the same way as for a range
		 * query (see clauselist_selectivity()).
		 */
		prefixsel = topsel + prefixsel - 1.0;

		/*
		 * A zero or slightly negative prefixsel should be converted into
		 * a small positive value; we probably are dealing with a very
		 * tight range and got a bogus result due to roundoff errors.
		 * However, if prefixsel is very negative, then we probably have
		 * default selectivity estimates on one or both sides of the
		 * range.  In that case, insert a not-so-wildly-optimistic default
		 * estimate.
		 */
		if (prefixsel <= 0.0)
		{
			if (prefixsel < -0.01)
			{

				/*
				 * No data available --- use a default estimate that is
				 * small, but not real small.
				 */
				prefixsel = 0.01;
			}
			else
			{

				/*
				 * It's just roundoff error; use a small positive value
				 */
				prefixsel = 1.0e-10;
			}
		}
	}

	return prefixsel;
}


/*
 * Estimate the selectivity of a pattern of the specified type.
 * Note that any fixed prefix of the pattern will have been removed already.
 *
 * For now, we use a very simplistic approach: fixed characters reduce the
 * selectivity a good deal, character ranges reduce it a little,
 * wildcards (such as % for LIKE or .* for regex) increase it.
 */

#define FIXED_CHAR_SEL	0.04	/* about 1/25 */
#define CHAR_RANGE_SEL	0.25
#define ANY_CHAR_SEL	0.9		/* not 1, since it won't match
								 * end-of-string */
#define FULL_WILDCARD_SEL 5.0
#define PARTIAL_WILDCARD_SEL 2.0

static Selectivity
like_selectivity(char *patt, bool case_insensitive)
{
	Selectivity sel = 1.0;
	int			pos;

	/* Skip any leading %; it's already factored into initial sel */
	pos = (*patt == '%') ? 1 : 0;
	for (; patt[pos]; pos++)
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
			if (patt[pos] == '\0')
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
regex_selectivity_sub(char *patt, int pattlen, bool case_insensitive)
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
				paren_pos = pos;/* remember start of parenthesized item */
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
			 * If unquoted | is present at paren level 0 in pattern, we
			 * have multiple alternatives; sum their probabilities.
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
regex_selectivity(char *patt, bool case_insensitive)
{
	Selectivity sel;
	int			pattlen = strlen(patt);

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
		if (sel > 1.0)
			sel = 1.0;
	}
	return sel;
}

static Selectivity
pattern_selectivity(char *patt, Pattern_Type ptype)
{
	Selectivity result;

	switch (ptype)
	{
		case Pattern_Type_Like:
			result = like_selectivity(patt, false);
			break;
		case Pattern_Type_Like_IC:
			result = like_selectivity(patt, true);
			break;
		case Pattern_Type_Regex:
			result = regex_selectivity(patt, false);
			break;
		case Pattern_Type_Regex_IC:
			result = regex_selectivity(patt, true);
			break;
		default:
			elog(ERROR, "pattern_selectivity: bogus ptype");
			result = 1.0;		/* keep compiler quiet */
			break;
	}
	return result;
}

/*
 * Test whether the database's LOCALE setting is safe for LIKE/regexp index
 * optimization.  The key requirement here is that given a prefix string,
 * say "foo", we must be able to generate another string "fop" that is
 * greater than all strings "foobar" starting with "foo".  Unfortunately,
 * many non-C locales have bizarre collation rules in which "fop" > "foo"
 * is not sufficient to ensure "fop" > "foobar".  Until we can come up
 * with a more bulletproof way of generating the upper-bound string,
 * disable the optimization in locales where it is not known to be safe.
 */
bool
locale_is_like_safe(void)
{
#ifdef USE_LOCALE
	/* Cache result so we only have to compute it once */
	static int	result = -1;
	char	   *localeptr;

	if (result >= 0)
		return (bool) result;
	localeptr = setlocale(LC_COLLATE, NULL);
	if (!localeptr)
		elog(STOP, "Invalid LC_COLLATE setting");

	/*
	 * Currently we accept only "C" and "POSIX" (do any systems still
	 * return "POSIX"?).  Which other locales allow safe optimization?
	 */
	if (strcmp(localeptr, "C") == 0)
		result = true;
	else if (strcmp(localeptr, "POSIX") == 0)
		result = true;
	else
		result = false;
	return (bool) result;
#else							/* not USE_LOCALE */
				return true;	/* We must be in C locale, which is OK */
#endif	 /* USE_LOCALE */
}

/*
 * Try to generate a string greater than the given string or any string it is
 * a prefix of.  If successful, return a palloc'd string; else return NULL.
 *
 * To work correctly in non-ASCII locales with weird collation orders,
 * we cannot simply increment "foo" to "fop" --- we have to check whether
 * we actually produced a string greater than the given one.  If not,
 * increment the righthand byte again and repeat.  If we max out the righthand
 * byte, truncate off the last character and start incrementing the next.
 * For example, if "z" were the last character in the sort order, then we
 * could produce "foo" as a string greater than "fonz".
 *
 * This could be rather slow in the worst case, but in most cases we won't
 * have to try more than one or two strings before succeeding.
 *
 * XXX this is actually not sufficient, since it only copes with the case
 * where individual characters collate in an order different from their
 * numeric code assignments.  It does not handle cases where there are
 * cross-character effects, such as specially sorted digraphs, multiple
 * sort passes, etc.  For now, we just shut down the whole thing in locales
 * that do such things :-(
 */
char *
make_greater_string(const char *str, Oid datatype)
{
	char	   *workstr;
	int			len;

	/*
	 * Make a modifiable copy, which will be our return value if
	 * successful
	 */
	workstr = pstrdup((char *) str);

	while ((len = strlen(workstr)) > 0)
	{
		unsigned char *lastchar = (unsigned char *) (workstr + len - 1);

		/*
		 * Try to generate a larger string by incrementing the last byte.
		 */
		while (*lastchar < (unsigned char) 255)
		{
			(*lastchar)++;
			if (string_lessthan(str, workstr, datatype))
				return workstr; /* Success! */
		}

		/*
		 * Truncate off the last character, which might be more than 1
		 * byte in MULTIBYTE case.
		 */
#ifdef MULTIBYTE
		len = pg_mbcliplen((const unsigned char *) workstr, len, len - 1);
		workstr[len] = '\0';
#else
		*lastchar = '\0';
#endif
	}

	/* Failed... */
	pfree(workstr);
	return NULL;
}

/*
 * Test whether two strings are "<" according to the rules of the given
 * datatype.  We do this the hard way, ie, actually calling the type's
 * "<" operator function, to ensure we get the right result...
 */
static bool
string_lessthan(const char *str1, const char *str2, Oid datatype)
{
	Datum		datum1 = string_to_datum(str1, datatype);
	Datum		datum2 = string_to_datum(str2, datatype);
	bool		result;

	switch (datatype)
	{
		case TEXTOID:
			result = DatumGetBool(DirectFunctionCall2(text_lt,
													  datum1, datum2));
			break;

		case BPCHAROID:
			result = DatumGetBool(DirectFunctionCall2(bpcharlt,
													  datum1, datum2));
			break;

		case VARCHAROID:
			result = DatumGetBool(DirectFunctionCall2(varcharlt,
													  datum1, datum2));
			break;

		case NAMEOID:
			result = DatumGetBool(DirectFunctionCall2(namelt,
													  datum1, datum2));
			break;

		default:
			elog(ERROR, "string_lessthan: unexpected datatype %u", datatype);
			result = false;
			break;
	}

	pfree(DatumGetPointer(datum1));
	pfree(DatumGetPointer(datum2));

	return result;
}

/* See if there is a binary op of the given name for the given datatype */
static Oid
find_operator(const char *opname, Oid datatype)
{
	return GetSysCacheOid(OPERNAME,
						  PointerGetDatum(opname),
						  ObjectIdGetDatum(datatype),
						  ObjectIdGetDatum(datatype),
						  CharGetDatum('b'));
}

/*
 * Generate a Datum of the appropriate type from a C string.
 * Note that all of the supported types are pass-by-ref, so the
 * returned value should be pfree'd if no longer needed.
 */
static Datum
string_to_datum(const char *str, Oid datatype)
{

	/*
	 * We cheat a little by assuming that textin() will do for bpchar and
	 * varchar constants too...
	 */
	if (datatype == NAMEOID)
		return DirectFunctionCall1(namein, CStringGetDatum(str));
	else
		return DirectFunctionCall1(textin, CStringGetDatum(str));
}

/*-------------------------------------------------------------------------
 *
 * Index cost estimation functions
 *
 * genericcostestimate is a general-purpose estimator for use when we
 * don't have any better idea about how to estimate.  Index-type-specific
 * knowledge can be incorporated in the type-specific routines.
 *
 *-------------------------------------------------------------------------
 */

static void
genericcostestimate(Query *root, RelOptInfo *rel,
					IndexOptInfo *index, List *indexQuals,
					Cost *indexStartupCost,
					Cost *indexTotalCost,
					Selectivity *indexSelectivity,
					double *indexCorrelation)
{
	double		numIndexTuples;
	double		numIndexPages;

	/* Estimate the fraction of main-table tuples that will be visited */
	*indexSelectivity = clauselist_selectivity(root, indexQuals,
											   lfirsti(rel->relids));

	/* Estimate the number of index tuples that will be visited */
	numIndexTuples = *indexSelectivity * index->tuples;

	/* Estimate the number of index pages that will be retrieved */
	numIndexPages = *indexSelectivity * index->pages;

	/*
	 * Always estimate at least one tuple and page are touched, even when
	 * indexSelectivity estimate is tiny.
	 */
	if (numIndexTuples < 1.0)
		numIndexTuples = 1.0;
	if (numIndexPages < 1.0)
		numIndexPages = 1.0;

	/*
	 * Compute the index access cost.
	 *
	 * Our generic assumption is that the index pages will be read
	 * sequentially, so they have cost 1.0 each, not random_page_cost.
	 * Also, we charge for evaluation of the indexquals at each index
	 * tuple. All the costs are assumed to be paid incrementally during
	 * the scan.
	 */
	*indexStartupCost = 0;
	*indexTotalCost = numIndexPages +
		(cpu_index_tuple_cost + cost_qual_eval(indexQuals)) * numIndexTuples;

	/*
	 * Generic assumption about index correlation: there isn't any.
	 */
	*indexCorrelation = 0.0;
}


Datum
btcostestimate(PG_FUNCTION_ARGS)
{
	Query	   *root = (Query *) PG_GETARG_POINTER(0);
	RelOptInfo *rel = (RelOptInfo *) PG_GETARG_POINTER(1);
	IndexOptInfo *index = (IndexOptInfo *) PG_GETARG_POINTER(2);
	List	   *indexQuals = (List *) PG_GETARG_POINTER(3);
	Cost	   *indexStartupCost = (Cost *) PG_GETARG_POINTER(4);
	Cost	   *indexTotalCost = (Cost *) PG_GETARG_POINTER(5);
	Selectivity *indexSelectivity = (Selectivity *) PG_GETARG_POINTER(6);
	double	   *indexCorrelation = (double *) PG_GETARG_POINTER(7);

	genericcostestimate(root, rel, index, indexQuals,
						indexStartupCost, indexTotalCost,
						indexSelectivity, indexCorrelation);

	/*
	 * If it's a functional index, leave the default zero-correlation
	 * estimate in place.  If not, and if we can get an estimate for
	 * the first variable's ordering correlation C from pg_statistic,
	 * estimate the index correlation as C / number-of-columns.
	 * (The idea here is that multiple columns dilute the importance
	 * of the first column's ordering, but don't negate it entirely.)
	 */
	if (index->indproc == InvalidOid)
	{
		Oid			relid;
		HeapTuple	tuple;

		relid = getrelid(lfirsti(rel->relids), root->rtable);
		Assert(relid != InvalidOid);
		tuple = SearchSysCache(STATRELATT,
							   ObjectIdGetDatum(relid),
							   Int16GetDatum(index->indexkeys[0]),
							   0, 0);
		if (HeapTupleIsValid(tuple))
		{
			Oid		typid;
			int32	typmod;
			float4 *numbers;
			int		nnumbers;

			get_atttypetypmod(relid, index->indexkeys[0],
							  &typid, &typmod);
			if (get_attstatsslot(tuple, typid, typmod,
								 STATISTIC_KIND_CORRELATION,
								 index->ordering[0],
								 NULL, NULL, &numbers, &nnumbers))
			{
				double	varCorrelation;
				int		nKeys;

				Assert(nnumbers == 1);
				varCorrelation = numbers[0];
				for (nKeys = 1; index->indexkeys[nKeys] != 0; nKeys++)
					/*skip*/;

				*indexCorrelation = varCorrelation / nKeys;

				free_attstatsslot(typid, NULL, 0, numbers, nnumbers);
			}
			ReleaseSysCache(tuple);
		}
	}

	PG_RETURN_VOID();
}

Datum
rtcostestimate(PG_FUNCTION_ARGS)
{
	Query	   *root = (Query *) PG_GETARG_POINTER(0);
	RelOptInfo *rel = (RelOptInfo *) PG_GETARG_POINTER(1);
	IndexOptInfo *index = (IndexOptInfo *) PG_GETARG_POINTER(2);
	List	   *indexQuals = (List *) PG_GETARG_POINTER(3);
	Cost	   *indexStartupCost = (Cost *) PG_GETARG_POINTER(4);
	Cost	   *indexTotalCost = (Cost *) PG_GETARG_POINTER(5);
	Selectivity *indexSelectivity = (Selectivity *) PG_GETARG_POINTER(6);
	double	   *indexCorrelation = (double *) PG_GETARG_POINTER(7);

	genericcostestimate(root, rel, index, indexQuals,
						indexStartupCost, indexTotalCost,
						indexSelectivity, indexCorrelation);

	PG_RETURN_VOID();
}

Datum
hashcostestimate(PG_FUNCTION_ARGS)
{
	Query	   *root = (Query *) PG_GETARG_POINTER(0);
	RelOptInfo *rel = (RelOptInfo *) PG_GETARG_POINTER(1);
	IndexOptInfo *index = (IndexOptInfo *) PG_GETARG_POINTER(2);
	List	   *indexQuals = (List *) PG_GETARG_POINTER(3);
	Cost	   *indexStartupCost = (Cost *) PG_GETARG_POINTER(4);
	Cost	   *indexTotalCost = (Cost *) PG_GETARG_POINTER(5);
	Selectivity *indexSelectivity = (Selectivity *) PG_GETARG_POINTER(6);
	double	   *indexCorrelation = (double *) PG_GETARG_POINTER(7);

	genericcostestimate(root, rel, index, indexQuals,
						indexStartupCost, indexTotalCost,
						indexSelectivity, indexCorrelation);

	PG_RETURN_VOID();
}

Datum
gistcostestimate(PG_FUNCTION_ARGS)
{
	Query	   *root = (Query *) PG_GETARG_POINTER(0);
	RelOptInfo *rel = (RelOptInfo *) PG_GETARG_POINTER(1);
	IndexOptInfo *index = (IndexOptInfo *) PG_GETARG_POINTER(2);
	List	   *indexQuals = (List *) PG_GETARG_POINTER(3);
	Cost	   *indexStartupCost = (Cost *) PG_GETARG_POINTER(4);
	Cost	   *indexTotalCost = (Cost *) PG_GETARG_POINTER(5);
	Selectivity *indexSelectivity = (Selectivity *) PG_GETARG_POINTER(6);
	double	   *indexCorrelation = (double *) PG_GETARG_POINTER(7);

	genericcostestimate(root, rel, index, indexQuals,
						indexStartupCost, indexTotalCost,
						indexSelectivity, indexCorrelation);

	PG_RETURN_VOID();
}
