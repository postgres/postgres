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
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/selfuncs.c,v 1.61 2000/03/23 00:55:42 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "optimizer/cost.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "utils/builtins.h"
#include "utils/int8.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/* N is not a valid var/constant or relation id */
#define NONVALUE(N)		((N) == 0)

/* are we looking at a functional index selectivity request? */
#define FunctionalSelectivity(nIndKeys,attNum) ((attNum)==InvalidAttrNumber)

/* default selectivity estimate for equalities such as "A = b" */
#define DEFAULT_EQ_SEL  0.01

/* default selectivity estimate for inequalities such as "A < b" */
#define DEFAULT_INEQ_SEL  (1.0 / 3.0)

static bool convert_string_to_scalar(char *str, int strlength,
									 double *scaleval);
static void getattproperties(Oid relid, AttrNumber attnum,
							 Oid *typid,
							 int *typlen,
							 bool *typbyval,
							 int32 *typmod);
static bool getattstatistics(Oid relid, AttrNumber attnum,
							 Oid typid, int32 typmod,
							 double *nullfrac,
							 double *commonfrac,
							 Datum *commonval,
							 Datum *loval,
							 Datum *hival);


/*
 *		eqsel			- Selectivity of "=" for any data types.
 *
 * Note: this routine is also used to estimate selectivity for some
 * operators that are not "=" but have comparable selectivity behavior,
 * such as "~~" (text LIKE).  Even for "=" we must keep in mind that
 * the left and right datatypes may differ, so the type of the given
 * constant "value" may be different from the type of the attribute.
 */
float64
eqsel(Oid opid,
	  Oid relid,
	  AttrNumber attno,
	  Datum value,
	  int32 flag)
{
	float64		result;

	result = (float64) palloc(sizeof(float64data));
	if (NONVALUE(attno) || NONVALUE(relid))
		*result = DEFAULT_EQ_SEL;
	else
	{
		Oid			typid;
		int			typlen;
		bool		typbyval;
		int32		typmod;
		double		nullfrac;
		double		commonfrac;
		Datum		commonval;
		double		selec;

		/* get info about the attribute */
		getattproperties(relid, attno,
						 &typid, &typlen, &typbyval, &typmod);

		/* get stats for the attribute, if available */
		if (getattstatistics(relid, attno, typid, typmod,
							 &nullfrac, &commonfrac, &commonval,
							 NULL, NULL))
		{
			if (flag & SEL_CONSTANT)
			{
				/* Is the constant "=" to the column's most common value?
				 * (Although the operator may not really be "=",
				 * we will assume that seeing whether it returns TRUE
				 * for the most common value is useful information.
				 * If you don't like it, maybe you shouldn't be using
				 * eqsel for your operator...)
				 */
				RegProcedure	eqproc = get_opcode(opid);
				bool			mostcommon;

				if (eqproc == (RegProcedure) NULL)
					elog(ERROR, "eqsel: no procedure for operator %u",
						 opid);

				/* be careful to apply operator right way 'round */
				if (flag & SEL_RIGHT)
					mostcommon = (bool)
						DatumGetUInt8(fmgr(eqproc, commonval, value));
				else
					mostcommon = (bool)
						DatumGetUInt8(fmgr(eqproc, value, commonval));

				if (mostcommon)
				{
					/* Constant is "=" to the most common value.  We know
					 * selectivity exactly (or as exactly as VACUUM could
					 * calculate it, anyway).
					 */
					selec = commonfrac;
				}
				else
				{
					/* Comparison is against a constant that is neither the
					 * most common value nor null.  Its selectivity cannot
					 * be more than this:
					 */
					selec = 1.0 - commonfrac - nullfrac;
					if (selec > commonfrac)
						selec = commonfrac;
					/* and in fact it's probably less, so we should apply
					 * a fudge factor.  The only case where we don't is
					 * for a boolean column, where indeed we have estimated
					 * the less-common value's frequency exactly!
					 */
					if (typid != BOOLOID)
						selec *= 0.5;
				}
			}
			else
			{
				/* Search is for a value that we do not know a priori,
				 * but we will assume it is not NULL.  Selectivity
				 * cannot be more than this:
				 */
				selec = 1.0 - nullfrac;
				if (selec > commonfrac)
					selec = commonfrac;
				/* and in fact it's probably less, so apply a fudge
				 * factor.
				 */
				selec *= 0.5;
			}

			/* result should be in range, but make sure... */
			if (selec < 0.0)
				selec = 0.0;
			else if (selec > 1.0)
				selec = 1.0;

			if (! typbyval)
				pfree(DatumGetPointer(commonval));
		}
		else
		{
			/* No VACUUM ANALYZE stats available, so make a guess using
			 * the disbursion stat (if we have that, which is unlikely
			 * for a normal attribute; but for a system attribute we may
			 * be able to estimate it).
			 */
			selec = get_attdisbursion(relid, attno, 0.01);
		}

		*result = (float64data) selec;
	}
	return result;
}

/*
 *		neqsel			- Selectivity of "!=" for any data types.
 *
 * This routine is also used for some operators that are not "!="
 * but have comparable selectivity behavior.  See above comments
 * for eqsel().
 */
float64
neqsel(Oid opid,
	   Oid relid,
	   AttrNumber attno,
	   Datum value,
	   int32 flag)
{
	float64		result;

	result = eqsel(opid, relid, attno, value, flag);
	*result = 1.0 - *result;
	return result;
}

/*
 *		scalarltsel		- Selectivity of "<" (also "<=") for scalars.
 *
 * This routine works for any datatype (or pair of datatypes) known to
 * convert_to_scalar().  If it is applied to some other datatype,
 * it will return a default estimate.
 */
float64
scalarltsel(Oid opid,
			Oid relid,
			AttrNumber attno,
			Datum value,
			int32 flag)
{
	float64		result;

	result = (float64) palloc(sizeof(float64data));
	if (! (flag & SEL_CONSTANT) || NONVALUE(attno) || NONVALUE(relid))
		*result = DEFAULT_INEQ_SEL;
	else
	{
		HeapTuple	oprtuple;
		Oid			ltype,
					rtype;
		Oid			typid;
		int			typlen;
		bool		typbyval;
		int32		typmod;
		Datum		hival,
					loval;
		double		val,
					high,
					low,
					numerator,
					denominator;

		/* Get left and right datatypes of the operator so we know
		 * what type the constant is.
		 */
		oprtuple = get_operator_tuple(opid);
		if (! HeapTupleIsValid(oprtuple))
			elog(ERROR, "scalarltsel: no tuple for operator %u", opid);
		ltype = ((Form_pg_operator) GETSTRUCT(oprtuple))->oprleft;
		rtype = ((Form_pg_operator) GETSTRUCT(oprtuple))->oprright;

		/* Convert the constant to a uniform comparison scale. */
		if (! convert_to_scalar(value,
								((flag & SEL_RIGHT) ? rtype : ltype),
								&val))
		{
			/* Ideally we'd produce an error here, on the grounds that the
			 * given operator shouldn't have scalarltsel registered as its
			 * selectivity func unless we can deal with its operand types.
			 * But currently, all manner of stuff is invoking scalarltsel,
			 * so give a default estimate until that can be fixed.
			 */
			*result = DEFAULT_INEQ_SEL;
			return result;
		}

		/* Now get info and stats about the attribute */
		getattproperties(relid, attno,
						 &typid, &typlen, &typbyval, &typmod);

		if (! getattstatistics(relid, attno, typid, typmod,
							   NULL, NULL, NULL,
							   &loval, &hival))
		{
			/* no stats available, so default result */
			*result = DEFAULT_INEQ_SEL;
			return result;
		}

		/* Convert the attribute's loval/hival to common scale. */
		if (! convert_to_scalar(loval, typid, &low) ||
			! convert_to_scalar(hival, typid, &high))
		{
			/* See above comments... */
			if (! typbyval)
			{
				pfree(DatumGetPointer(hival));
				pfree(DatumGetPointer(loval));
			}

			*result = DEFAULT_INEQ_SEL;
			return result;
		}

		/* release temp storage if needed */
		if (! typbyval)
		{
			pfree(DatumGetPointer(hival));
			pfree(DatumGetPointer(loval));
		}

		if (high <= low)
		{
			/* If we trusted the stats fully, we could return a small or
			 * large selec depending on which side of the single data point
			 * the constant is on.  But it seems better to assume that the
			 * stats are wrong and return a default...
			 */
			*result = DEFAULT_INEQ_SEL;
		}
		else if (val < low || val > high)
		{
			/* If given value is outside the statistical range, return a
			 * small or large value; but not 0.0/1.0 since there is a chance
			 * the stats are out of date.
			 */
			if (flag & SEL_RIGHT)
				*result = (val < low) ? 0.001 : 0.999;
			else
				*result = (val < low) ? 0.999 : 0.001;
		}
		else
		{
			denominator = high - low;
			if (flag & SEL_RIGHT)
				numerator = val - low;
			else
				numerator = high - val;
			*result = numerator / denominator;
		}
	}
	return result;
}

/*
 *		scalargtsel		- Selectivity of ">" (also ">=") for integers.
 *
 * See above comments for scalarltsel.
 */
float64
scalargtsel(Oid opid,
			Oid relid,
			AttrNumber attno,
			Datum value,
			int32 flag)
{
	float64		result;

	/* Compute selectivity of "<", then invert --- but only if we
	 * were able to produce a non-default estimate.
	 */
	result = scalarltsel(opid, relid, attno, value, flag);
	if (*result != DEFAULT_INEQ_SEL)
		*result = 1.0 - *result;
	return result;
}

/*
 *		eqjoinsel		- Join selectivity of "="
 */
float64
eqjoinsel(Oid opid,
		  Oid relid1,
		  AttrNumber attno1,
		  Oid relid2,
		  AttrNumber attno2)
{
	float64		result;
	float64data num1,
				num2,
				min;
	bool		unknown1 = NONVALUE(relid1) || NONVALUE(attno1);
	bool		unknown2 = NONVALUE(relid2) || NONVALUE(attno2);

	result = (float64) palloc(sizeof(float64data));
	if (unknown1 && unknown2)
		*result = DEFAULT_EQ_SEL;
	else
	{
		num1 = unknown1 ? 1.0 : get_attdisbursion(relid1, attno1, 0.01);
		num2 = unknown2 ? 1.0 : get_attdisbursion(relid2, attno2, 0.01);
		/*
		 * The join selectivity cannot be more than num2, since each
		 * tuple in table 1 could match no more than num2 fraction of
		 * tuples in table 2 (and that's only if the table-1 tuple
		 * matches the most common value in table 2, so probably it's
		 * less).  By the same reasoning it is not more than num1.
		 * The min is therefore an upper bound.
		 *
		 * If we know the disbursion of only one side, use it; the reasoning
		 * above still works.
		 *
		 * XXX can we make a better estimate here?  Using the nullfrac
		 * statistic might be helpful, for example.  Assuming the operator
		 * is strict (does not succeed for null inputs) then the selectivity
		 * couldn't be more than (1-nullfrac1)*(1-nullfrac2), which might
		 * be usefully small if there are many nulls.  How about applying
		 * the operator to the most common values?
		 */
		min = (num1 < num2) ? num1 : num2;
		*result = min;
	}
	return result;
}

/*
 *		neqjoinsel		- Join selectivity of "!="
 */
float64
neqjoinsel(Oid opid,
		   Oid relid1,
		   AttrNumber attno1,
		   Oid relid2,
		   AttrNumber attno2)
{
	float64		result;

	result = eqjoinsel(opid, relid1, attno1, relid2, attno2);
	*result = 1.0 - *result;
	return result;
}

/*
 *		scalarltjoinsel	- Join selectivity of "<" and "<=" for scalars
 */
float64
scalarltjoinsel(Oid opid,
				Oid relid1,
				AttrNumber attno1,
				Oid relid2,
				AttrNumber attno2)
{
	float64		result;

	result = (float64) palloc(sizeof(float64data));
	*result = DEFAULT_INEQ_SEL;
	return result;
}

/*
 *		scalargtjoinsel	- Join selectivity of ">" and ">=" for scalars
 */
float64
scalargtjoinsel(Oid opid,
				Oid relid1,
				AttrNumber attno1,
				Oid relid2,
				AttrNumber attno2)
{
	float64		result;

	result = (float64) palloc(sizeof(float64data));
	*result = DEFAULT_INEQ_SEL;
	return result;
}

/*
 * convert_to_scalar
 *	  Convert a non-NULL value of the indicated type to the comparison
 *	  scale needed by scalarltsel()/scalargtsel().
 *	  Returns "true" if successful.
 *
 * All numeric datatypes are simply converted to their equivalent
 * "double" values.
 *
 * String datatypes are converted by convert_string_to_scalar(),
 * which is explained below.
 *
 * The several datatypes representing absolute times are all converted
 * to Timestamp, which is actually a double, and then we just use that
 * double value.  Note this will give bad results for the various "special"
 * values of Timestamp --- what can we do with those?
 *
 * The several datatypes representing relative times (intervals) are all
 * converted to measurements expressed in seconds.
 */
bool
convert_to_scalar(Datum value, Oid typid,
				  double *scaleval)
{
	switch (typid)
	{
		/*
		 * Built-in numeric types
		 */
		case BOOLOID:
			*scaleval = (double) DatumGetUInt8(value);
			return true;
		case INT2OID:
			*scaleval = (double) DatumGetInt16(value);
			return true;
		case INT4OID:
			*scaleval = (double) DatumGetInt32(value);
			return true;
		case INT8OID:
			*scaleval = (double) (* i8tod((int64 *) DatumGetPointer(value)));
			return true;
		case FLOAT4OID:
			*scaleval = (double) (* DatumGetFloat32(value));
			return true;
		case FLOAT8OID:
			*scaleval = (double) (* DatumGetFloat64(value));
			return true;
		case NUMERICOID:
			*scaleval = (double) (* numeric_float8((Numeric) DatumGetPointer(value)));
			return true;
		case OIDOID:
		case REGPROCOID:
			/* we can treat OIDs as integers... */
			*scaleval = (double) DatumGetObjectId(value);
			return true;

		/*
		 * Built-in string types
		 */
		case CHAROID:
		{
			char	ch = DatumGetChar(value);

			return convert_string_to_scalar(&ch, 1, scaleval);
		}
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
		{
			char   *str = (char *) VARDATA(DatumGetPointer(value));
			int		strlength = VARSIZE(DatumGetPointer(value)) - VARHDRSZ;

			return convert_string_to_scalar(str, strlength, scaleval);
		}
		case NAMEOID:
		{
			NameData   *nm = (NameData *) DatumGetPointer(value);

			return convert_string_to_scalar(NameStr(*nm), strlen(NameStr(*nm)),
											scaleval);
		}

		/*
		 * Built-in absolute-time types
		 */
		case TIMESTAMPOID:
			*scaleval = * ((Timestamp *) DatumGetPointer(value));
			return true;
		case ABSTIMEOID:
			*scaleval = * abstime_timestamp(value);
			return true;
		case DATEOID:
			*scaleval = * date_timestamp(value);
			return true;

		/*
		 * Built-in relative-time types
		 */
		case INTERVALOID:
		{
			Interval   *interval = (Interval *) DatumGetPointer(value);

			/*
			 * Convert the month part of Interval to days using assumed
			 * average month length of 365.25/12.0 days.  Not too accurate,
			 * but plenty good enough for our purposes.
			 */
			*scaleval = interval->time +
				interval->month * (365.25/12.0 * 24.0 * 60.0 * 60.0);
			return true;
		}
		case RELTIMEOID:
			*scaleval = (RelativeTime) DatumGetInt32(value);
			return true;
		case TINTERVALOID:
		{
			TimeInterval	interval = (TimeInterval) DatumGetPointer(value);

			if (interval->status != 0)
			{
				*scaleval = interval->data[1] - interval->data[0];
				return true;
			}
			break;
		}
		case TIMEOID:
			*scaleval = * ((TimeADT *) DatumGetPointer(value));
			return true;

		default:
		{
			/*
			 * See whether there is a registered type-conversion function,
			 * namely a procedure named "float8" with the right signature.
			 * If so, assume we can convert the value to the numeric scale.
			 *
			 * NOTE: there are no such procedures in the standard distribution,
			 * except with argument types that we already dealt with above.
			 * This code is just here as an escape for user-defined types.
			 */
			Oid			oid_array[FUNC_MAX_ARGS];
			HeapTuple	ftup;

			MemSet(oid_array, 0, FUNC_MAX_ARGS * sizeof(Oid));
			oid_array[0] = typid;
			ftup = SearchSysCacheTuple(PROCNAME,
									   PointerGetDatum("float8"),
									   Int32GetDatum(1),
									   PointerGetDatum(oid_array),
									   0);
			if (HeapTupleIsValid(ftup) &&
				((Form_pg_proc) GETSTRUCT(ftup))->prorettype == FLOAT8OID)
			{
				RegProcedure convertproc = (RegProcedure) ftup->t_data->t_oid;
				Datum converted = (Datum) fmgr(convertproc, value);
				*scaleval = (double) (* DatumGetFloat64(converted));
				return true;
			}
			break;
		}
	}
	/* Don't know how to convert */
	return false;
}

/*
 * Do convert_to_scalar()'s work for any character-string data type.
 *
 * String datatypes are converted to a scale that ranges from 0 to 1, where
 * we visualize the bytes of the string as fractional base-256 digits.
 * It's sufficient to consider the first few bytes, since double has only
 * limited precision (and we can't expect huge accuracy in our selectivity
 * predictions anyway!)
 *
 * If USE_LOCALE is defined, we must pass the string through strxfrm()
 * before doing the computation, so as to generate correct locale-specific
 * results.
 */
static bool
convert_string_to_scalar(char *str, int strlength,
						 double *scaleval)
{
	unsigned char   *sptr;
	int				slen;
#ifdef USE_LOCALE
	char		   *rawstr;
	char		   *xfrmstr;
	size_t			xfrmsize;
	size_t			xfrmlen;
#endif
	double			num,
					denom;

	if (strlength <= 0)
	{
		*scaleval = 0;			/* empty string has scalar value 0 */
		return true;
	}

#ifdef USE_LOCALE
	/* Need a null-terminated string to pass to strxfrm() */
	rawstr = (char *) palloc(strlength + 1);
	memcpy(rawstr, str, strlength);
	rawstr[strlength] = '\0';

	/* Guess that transformed string is not much bigger */
	xfrmsize = strlength + 32;	/* arbitrary pad value here... */
	xfrmstr = (char *) palloc(xfrmsize);
	xfrmlen = strxfrm(xfrmstr, rawstr, xfrmsize);
	if (xfrmlen >= xfrmsize)
	{
		/* Oops, didn't make it */
		pfree(xfrmstr);
		xfrmstr = (char *) palloc(xfrmlen+1);
		xfrmlen = strxfrm(xfrmstr, rawstr, xfrmlen+1);
	}
	pfree(rawstr);

	sptr = (unsigned char *) xfrmstr;
	slen = xfrmlen;
#else
	sptr = (unsigned char *) str;
	slen = strlength;
#endif

	/* No need to consider more than about 8 bytes (sizeof double) */
	if (slen > 8)
		slen = 8;

	/* Convert initial characters to fraction */
	num = 0.0;
	denom = 256.0;
	while (slen-- > 0)
	{
		num += ((double) (*sptr++)) / denom;
		denom *= 256.0;
	}

#ifdef USE_LOCALE
	pfree(xfrmstr);
#endif

	*scaleval = num;
	return true;
}


/*
 * getattproperties
 *	  Retrieve pg_attribute properties for an attribute,
 *	  including type OID, type len, type byval flag, typmod.
 */
static void
getattproperties(Oid relid, AttrNumber attnum,
				 Oid *typid, int *typlen, bool *typbyval, int32 *typmod)
{
	HeapTuple	atp;
	Form_pg_attribute att_tup;

	atp = SearchSysCacheTuple(ATTNUM,
							  ObjectIdGetDatum(relid),
							  Int16GetDatum(attnum),
							  0, 0);
	if (! HeapTupleIsValid(atp))
		elog(ERROR, "getattproperties: no attribute tuple %u %d",
			 relid, (int) attnum);
	att_tup = (Form_pg_attribute) GETSTRUCT(atp);

	*typid = att_tup->atttypid;
	*typlen = att_tup->attlen;
	*typbyval = att_tup->attbyval;
	*typmod = att_tup->atttypmod;
}

/*
 * getattstatistics
 *	  Retrieve the pg_statistic data for an attribute.
 *	  Returns 'false' if no stats are available.
 *
 * Inputs:
 * 'relid' and 'attnum' are the relation and attribute number.
 * 'typid' and 'typmod' are the type and typmod of the column,
 * which the caller must already have looked up.
 *
 * Outputs:
 * The available stats are nullfrac, commonfrac, commonval, loval, hival.
 * The caller need not retrieve all five --- pass NULL pointers for the
 * unwanted values.
 *
 * commonval, loval, hival are returned as Datums holding the internal
 * representation of the values.  (Note that these should be pfree'd
 * after use if the data type is not by-value.)
 */
static bool
getattstatistics(Oid relid,
				 AttrNumber attnum,
				 Oid typid,
				 int32 typmod,
				 double *nullfrac,
				 double *commonfrac,
				 Datum *commonval,
				 Datum *loval,
				 Datum *hival)
{
	HeapTuple	tuple;
	HeapTuple	typeTuple;
	FmgrInfo	inputproc;
	Oid			typelem;
	bool		isnull;

	/*
	 * We assume that there will only be one entry in pg_statistic for
	 * the given rel/att, so we search WITHOUT considering the staop
	 * column.  Someday, VACUUM might store more than one entry per rel/att,
	 * corresponding to more than one possible sort ordering defined for
	 * the column type.  However, to make that work we will need to figure
	 * out which staop to search for --- it's not necessarily the one we
	 * have at hand!  (For example, we might have a '>' operator rather than
	 * the '<' operator that will appear in staop.)
	 */
	tuple = SearchSysCacheTuple(STATRELID,
								ObjectIdGetDatum(relid),
								Int16GetDatum((int16) attnum),
								0,
								0);
	if (!HeapTupleIsValid(tuple))
	{
		/* no such stats entry */
		return false;
	}

	if (nullfrac)
		*nullfrac = ((Form_pg_statistic) GETSTRUCT(tuple))->stanullfrac;
	if (commonfrac)
		*commonfrac = ((Form_pg_statistic) GETSTRUCT(tuple))->stacommonfrac;

	/* Get the type input proc for the column datatype */
	typeTuple = SearchSysCacheTuple(TYPEOID,
									ObjectIdGetDatum(typid),
									0, 0, 0);
	if (! HeapTupleIsValid(typeTuple))
		elog(ERROR, "getattstatistics: Cache lookup failed for type %u",
			 typid);
	fmgr_info(((Form_pg_type) GETSTRUCT(typeTuple))->typinput, &inputproc);
	typelem = ((Form_pg_type) GETSTRUCT(typeTuple))->typelem;

	/* Values are variable-length fields, so cannot access as struct fields.
	 * Must do it the hard way with SysCacheGetAttr.
	 */
	if (commonval)
	{
		text *val = (text *) SysCacheGetAttr(STATRELID, tuple,
											 Anum_pg_statistic_stacommonval,
											 &isnull);
		if (isnull)
		{
			elog(DEBUG, "getattstatistics: stacommonval is null");
			*commonval = PointerGetDatum(NULL);
		}
		else
		{
			char *strval = textout(val);
			*commonval = (Datum)
				(*fmgr_faddr(&inputproc)) (strval, typelem, typmod);
			pfree(strval);
		}
	}

	if (loval)
	{
		text *val = (text *) SysCacheGetAttr(STATRELID, tuple,
											 Anum_pg_statistic_staloval,
											 &isnull);
		if (isnull)
		{
			elog(DEBUG, "getattstatistics: staloval is null");
			*loval = PointerGetDatum(NULL);
		}
		else
		{
			char *strval = textout(val);
			*loval = (Datum)
				(*fmgr_faddr(&inputproc)) (strval, typelem, typmod);
			pfree(strval);
		}
	}

	if (hival)
	{
		text *val = (text *) SysCacheGetAttr(STATRELID, tuple,
											 Anum_pg_statistic_stahival,
											 &isnull);
		if (isnull)
		{
			elog(DEBUG, "getattstatistics: stahival is null");
			*hival = PointerGetDatum(NULL);
		}
		else
		{
			char *strval = textout(val);
			*hival = (Datum)
				(*fmgr_faddr(&inputproc)) (strval, typelem, typmod);
			pfree(strval);
		}
	}

	return true;
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
					Selectivity *indexSelectivity)
{
	double numIndexTuples;
	double numIndexPages;

	/* Estimate the fraction of main-table tuples that will be visited */
    *indexSelectivity = clauselist_selectivity(root, indexQuals,
											   lfirsti(rel->relids));

	/* Estimate the number of index tuples that will be visited */
	numIndexTuples = *indexSelectivity * index->tuples;

	/* Estimate the number of index pages that will be retrieved */
	numIndexPages = *indexSelectivity * index->pages;

	/*
	 * Compute the index access cost.
	 *
	 * Our generic assumption is that the index pages will be read
	 * sequentially, so they have cost 1.0 each, not random_page_cost.
	 * Also, we charge for evaluation of the indexquals at each index tuple.
     * All the costs are assumed to be paid incrementally during the scan.
     */
    *indexStartupCost = 0;
    *indexTotalCost = numIndexPages +
		(cpu_index_tuple_cost + cost_qual_eval(indexQuals)) * numIndexTuples;
}

/*
 * For first cut, just use generic function for all index types.
 */

void
btcostestimate(Query *root, RelOptInfo *rel,
			   IndexOptInfo *index, List *indexQuals,
			   Cost *indexStartupCost,
			   Cost *indexTotalCost,
			   Selectivity *indexSelectivity)
{
	genericcostestimate(root, rel, index, indexQuals,
						indexStartupCost, indexTotalCost, indexSelectivity);
}

void
rtcostestimate(Query *root, RelOptInfo *rel,
			   IndexOptInfo *index, List *indexQuals,
			   Cost *indexStartupCost,
			   Cost *indexTotalCost,
			   Selectivity *indexSelectivity)
{
	genericcostestimate(root, rel, index, indexQuals,
						indexStartupCost, indexTotalCost, indexSelectivity);
}

void
hashcostestimate(Query *root, RelOptInfo *rel,
				 IndexOptInfo *index, List *indexQuals,
				 Cost *indexStartupCost,
				 Cost *indexTotalCost,
				 Selectivity *indexSelectivity)
{
	genericcostestimate(root, rel, index, indexQuals,
						indexStartupCost, indexTotalCost, indexSelectivity);
}

void
gistcostestimate(Query *root, RelOptInfo *rel,
				 IndexOptInfo *index, List *indexQuals,
				 Cost *indexStartupCost,
				 Cost *indexTotalCost,
				 Selectivity *indexSelectivity)
{
	genericcostestimate(root, rel, index, indexQuals,
						indexStartupCost, indexTotalCost, indexSelectivity);
}
