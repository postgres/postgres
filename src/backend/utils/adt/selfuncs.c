/*-------------------------------------------------------------------------
 *
 * selfuncs.c
 *	  Selectivity functions for system catalogs and builtin types
 *
 *	  These routines are registered in the operator catalog in the
 *	  "oprrest" and "oprjoin" attributes.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/selfuncs.c,v 1.36 1999/08/01 04:54:22 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "parser/parse_oper.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/* N is not a valid var/constant or relation id */
#define NONVALUE(N)		((N) == -1)

/* are we looking at a functional index selectivity request? */
#define FunctionalSelectivity(nIndKeys,attNum) ((attNum)==InvalidAttrNumber)

/* default selectivity estimate for inequalities such as "A < b" */
#define DEFAULT_INEQ_SEL  (1.0 / 3.0)

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
static double getattdisbursion(Oid relid, AttrNumber attnum);


/*
 *		eqsel			- Selectivity of "=" for any data types.
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
		*result = 0.1;
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

		if (getattstatistics(relid, attno, typid, typmod,
							 &nullfrac, &commonfrac, &commonval,
							 NULL, NULL))
		{
			if (flag & SEL_CONSTANT)
			{
				/* Is the constant the same as the most common value? */
				HeapTuple	oprtuple;
				Oid			ltype,
							rtype;
				Operator	func_operator;
				bool		mostcommon = false;

				/* get left and right datatypes of the operator */
				oprtuple = get_operator_tuple(opid);
				if (! HeapTupleIsValid(oprtuple))
					elog(ERROR, "eqsel: no tuple for operator %u", opid);
				ltype = ((Form_pg_operator) GETSTRUCT(oprtuple))->oprleft;
				rtype = ((Form_pg_operator) GETSTRUCT(oprtuple))->oprright;

				/* and find appropriate equality operator (no, it ain't
				 * necessarily opid itself...)
				 */
				func_operator = oper("=", ltype, rtype, true);

				if (func_operator != NULL)
				{
					RegProcedure eqproc = ((Form_pg_operator) GETSTRUCT(func_operator))->oprcode;
					if (flag & SEL_RIGHT) /* given value on the right? */
						mostcommon = (bool)
							DatumGetUInt8(fmgr(eqproc, commonval, value));
					else
						mostcommon = (bool)
							DatumGetUInt8(fmgr(eqproc, value, commonval));
				}

				if (mostcommon)
				{
					/* Search is for the most common value.  We know the
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
					/* and in fact it's probably less, so apply a fudge
					 * factor.
					 */
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
			 * the disbursion stat (if we have that, which is unlikely...)
			 */
			selec = getattdisbursion(relid, attno);
		}

		*result = (float64data) selec;
	}
	return result;
}

/*
 *		neqsel			- Selectivity of "!=" for any data types.
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
 *		intltsel		- Selectivity of "<" (also "<=") for integers.
 *						  Should work for both longs and shorts.
 */
float64
intltsel(Oid opid,
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
		long		val,
					high,
					low,
					numerator,
					denominator;

		/* get left and right datatypes of the operator */
		oprtuple = get_operator_tuple(opid);
		if (! HeapTupleIsValid(oprtuple))
			elog(ERROR, "intltsel: no tuple for operator %u", opid);
		ltype = ((Form_pg_operator) GETSTRUCT(oprtuple))->oprleft;
		rtype = ((Form_pg_operator) GETSTRUCT(oprtuple))->oprright;

		/*
		 * TEMPORARY HACK: this code is currently getting called for
		 * a bunch of non-integral types.  Give a default estimate if
		 * either side is not pass-by-val.  Need better solution.
		 */
		if (! get_typbyval(ltype) || ! get_typbyval(rtype))
		{
			*result = DEFAULT_INEQ_SEL;
			return result;
		}

		/* Deduce type of the constant, and convert to uniform "long" format.
		 * Note that constant might well be a different type than attribute.
		 * XXX this ought to use a type-specific "convert to double" op.
		 */
		typid = (flag & SEL_RIGHT) ? rtype : ltype;
		switch (get_typlen(typid))
		{
			case 1:
				val = (long) DatumGetUInt8(value);
				break;
			case 2:
				val = (long) DatumGetInt16(value);
				break;
			case 4:
				val = (long) DatumGetInt32(value);
				break;
			default:
				elog(ERROR, "intltsel: unsupported type %u", typid);
				*result = DEFAULT_INEQ_SEL;
				return result;
		}

		/* Now get info about the attribute */
		getattproperties(relid, attno,
						 &typid, &typlen, &typbyval, &typmod);

		if (! getattstatistics(relid, attno, typid, typmod,
							   NULL, NULL, NULL,
							   &loval, &hival))
		{
			*result = DEFAULT_INEQ_SEL;
			return result;
		}
		/*
		 * Convert loval/hival to common "long int" representation.
		 */
		switch (typlen)
		{
			case 1:
				low = (long) DatumGetUInt8(loval);
				high = (long) DatumGetUInt8(hival);
				break;
			case 2:
				low = (long) DatumGetInt16(loval);
				high = (long) DatumGetInt16(hival);
				break;
			case 4:
				low = (long) DatumGetInt32(loval);
				high = (long) DatumGetInt32(hival);
				break;
			default:
				elog(ERROR, "intltsel: unsupported type %u", typid);
				*result = DEFAULT_INEQ_SEL;
				return result;
		}
		if (val < low || val > high)
		{
			/* If given value is outside the statistical range,
			 * assume we have out-of-date stats and return a default guess.
			 * We could return a small or large value if we trusted the stats
			 * more.   XXX change this eventually.
			 */
			*result = DEFAULT_INEQ_SEL;
		}
		else
		{
			denominator = high - low;
			if (denominator <= 0)
				denominator = 1;
			if (flag & SEL_RIGHT)
				numerator = val - low;
			else
				numerator = high - val;
			if (numerator <= 0)	/* never return a zero estimate! */
				numerator = 1;
			if (numerator >= denominator)
				*result = 1.0;
			else
				*result = (double) numerator / (double) denominator;
		}
		if (! typbyval)
		{
			pfree(DatumGetPointer(hival));
			pfree(DatumGetPointer(loval));
		}
	}
	return result;
}

/*
 *		intgtsel		- Selectivity of ">" (also ">=") for integers.
 *						  Should work for both longs and shorts.
 */
float64
intgtsel(Oid opid,
		 Oid relid,
		 AttrNumber attno,
		 Datum value,
		 int32 flag)
{
	float64		result;

	/* Compute selectivity of "<", then invert --- but only if we
	 * were able to produce a non-default estimate.
	 */
	result = intltsel(opid, relid, attno, value, flag);
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
				max;

	result = (float64) palloc(sizeof(float64data));
	if (NONVALUE(attno1) || NONVALUE(relid1) ||
		NONVALUE(attno2) || NONVALUE(relid2))
		*result = 0.1;
	else
	{
		num1 = getattdisbursion(relid1, attno1);
		num2 = getattdisbursion(relid2, attno2);
		max = (num1 > num2) ? num1 : num2;
		if (max <= 0)
			*result = 1.0;
		else
			*result = max;
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
 *		intltjoinsel	- Join selectivity of "<" and "<="
 */
float64
intltjoinsel(Oid opid,
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
 *		intgtjoinsel	- Join selectivity of ">" and ">="
 */
float64
intgtjoinsel(Oid opid,
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
 *
 * XXX currently, this does a linear search of pg_statistic because there
 * is no index nor syscache for pg_statistic.  FIX THIS!
 */
static bool
getattstatistics(Oid relid, AttrNumber attnum, Oid typid, int32 typmod,
				 double *nullfrac,
				 double *commonfrac,
				 Datum *commonval,
				 Datum *loval,
				 Datum *hival)
{
	Relation	rel;
	HeapScanDesc scan;
	static ScanKeyData key[2] = {
		{0, Anum_pg_statistic_starelid, F_OIDEQ, {0, 0, F_OIDEQ}},
		{0, Anum_pg_statistic_staattnum, F_INT2EQ, {0, 0, F_INT2EQ}}
	};
	bool		isnull;
	HeapTuple	tuple;
	HeapTuple	typeTuple;
	FmgrInfo	inputproc;

	rel = heap_openr(StatisticRelationName);

	key[0].sk_argument = ObjectIdGetDatum(relid);
	key[1].sk_argument = Int16GetDatum((int16) attnum);

	scan = heap_beginscan(rel, 0, SnapshotNow, 2, key);
	tuple = heap_getnext(scan, 0);
	if (!HeapTupleIsValid(tuple))
	{
		/* no such stats entry */
		heap_endscan(scan);
		heap_close(rel);
		return false;
	}

	/* We assume that there will only be one entry in pg_statistic
	 * for the given rel/att.  Someday, VACUUM might store more than one...
	 */
	if (nullfrac)
		*nullfrac = ((Form_pg_statistic) GETSTRUCT(tuple))->stanullfrac;
	if (commonfrac)
		*commonfrac = ((Form_pg_statistic) GETSTRUCT(tuple))->stacommonfrac;

	/* Get the type input proc for the column datatype */
	typeTuple = SearchSysCacheTuple(TYPOID,
									ObjectIdGetDatum(typid),
									0, 0, 0);
	if (! HeapTupleIsValid(typeTuple))
		elog(ERROR, "getattstatistics: Cache lookup failed for type %u",
			 typid);
	fmgr_info(((Form_pg_type) GETSTRUCT(typeTuple))->typinput, &inputproc);

	/* Values are variable-length fields, so cannot access as struct fields.
	 * Must do it the hard way with heap_getattr.
	 */
	if (commonval)
	{
		text *val = (text *) heap_getattr(tuple,
										  Anum_pg_statistic_stacommonval,
										  RelationGetDescr(rel),
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
				(*fmgr_faddr(&inputproc)) (strval, typid, typmod);
			pfree(strval);
		}
	}

	if (loval)
	{
		text *val = (text *) heap_getattr(tuple,
										  Anum_pg_statistic_staloval,
										  RelationGetDescr(rel),
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
				(*fmgr_faddr(&inputproc)) (strval, typid, typmod);
			pfree(strval);
		}
	}

	if (hival)
	{
		text *val = (text *) heap_getattr(tuple,
										  Anum_pg_statistic_stahival,
										  RelationGetDescr(rel),
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
				(*fmgr_faddr(&inputproc)) (strval, typid, typmod);
			pfree(strval);
		}
	}

	heap_endscan(scan);
	heap_close(rel);
	return true;
}

/*
 * getattdisbursion
 *	  Retrieve the disbursion statistic for an attribute,
 *	  or produce an estimate if no info is available.
 */
static double
getattdisbursion(Oid relid, AttrNumber attnum)
{
	HeapTuple	atp;
	double		disbursion;
	int32		ntuples;

	atp = SearchSysCacheTuple(ATTNUM,
							  ObjectIdGetDatum(relid),
							  Int16GetDatum(attnum),
							  0, 0);
	if (!HeapTupleIsValid(atp))
	{
		/* this should not happen */
		elog(ERROR, "getattdisbursion: no attribute tuple %u %d",
			 relid, attnum);
		return 0.1;
	}

	disbursion = ((Form_pg_attribute) GETSTRUCT(atp))->attdisbursion;
	if (disbursion > 0.0)
		return disbursion;

	/* VACUUM ANALYZE has not stored a disbursion statistic for us.
	 * Produce an estimate = 1/numtuples.  This may produce
	 * unreasonably small estimates for large tables, so limit
	 * the estimate to no less than 0.01.
	 */
	atp = SearchSysCacheTuple(RELOID,
							  ObjectIdGetDatum(relid),
							  0, 0, 0);
	if (!HeapTupleIsValid(atp))
	{
		/* this should not happen */
		elog(ERROR, "getattdisbursion: no relation tuple %u", relid);
		return 0.1;
	}

	ntuples = ((Form_pg_class) GETSTRUCT(atp))->reltuples;

	if (ntuples > 0)
		disbursion = 1.0 / (double) ntuples;

	if (disbursion < 0.01)
		disbursion = 0.01;

	return disbursion;
}

float64
btreesel(Oid operatorObjectId,
		 Oid indrelid,
		 AttrNumber attributeNumber,
		 char *constValue,
		 int32 constFlag,
		 int32 nIndexKeys,
		 Oid indexrelid)
{
	float64		result;

	if (FunctionalSelectivity(nIndexKeys, attributeNumber))
	{

		/*
		 * Need to call the functions selectivity function here.  For now
		 * simply assume it's 1/3 since functions don't currently have
		 * selectivity functions
		 */
		result = (float64) palloc(sizeof(float64data));
		*result = 1.0 / 3.0;
	}
	else
	{
		RegProcedure oprrest = get_oprrest(operatorObjectId);

		/*
		 * Operators used for indexes should have selectivity estimators.
		 * (An alternative is to default to 0.5, as the optimizer does in
		 * dealing with operators occurring in WHERE clauses, but if you
		 * are going to the trouble of making index support you probably
		 * don't want to miss the benefits of a good selectivity estimate.)
		 */
		if (!oprrest)
		{
#if 1
			/*
			 * XXX temporary fix for 6.5: rtree operators are missing their
			 * selectivity estimators, so return a default estimate instead.
			 * Ugh.
			 */
			result = (float64) palloc(sizeof(float64data));
			*result = 0.5;
#else
			elog(ERROR,
				 "Operator %u must have a restriction selectivity estimator to be used in an index",
				 operatorObjectId);
#endif
		}
		else
			result = (float64) fmgr(oprrest,
									(char *) operatorObjectId,
									(char *) indrelid,
									(char *) (int) attributeNumber,
									(char *) constValue,
									(char *) constFlag,
									NULL);
	}

	if (!PointerIsValid(result))
		elog(ERROR, "Btree Selectivity: bad pointer");
	if (*result < 0.0 || *result > 1.0)
		elog(ERROR, "Btree Selectivity: bad value %lf", *result);

	return result;
}

float64
btreenpage(Oid operatorObjectId,
		   Oid indrelid,
		   AttrNumber attributeNumber,
		   char *constValue,
		   int32 constFlag,
		   int32 nIndexKeys,
		   Oid indexrelid)
{
	float64		temp,
				result;
	float64data tempData;
	HeapTuple	atp;
	int			npage;

	if (FunctionalSelectivity(nIndexKeys, attributeNumber))
	{

		/*
		 * Need to call the functions selectivity function here.  For now
		 * simply assume it's 1/3 since functions don't currently have
		 * selectivity functions
		 */
		tempData = 1.0 / 3.0;
		temp = &tempData;
	}
	else
	{
		RegProcedure oprrest = get_oprrest(operatorObjectId);

		/*
		 * Operators used for indexes should have selectivity estimators.
		 * (An alternative is to default to 0.5, as the optimizer does in
		 * dealing with operators occurring in WHERE clauses, but if you
		 * are going to the trouble of making index support you probably
		 * don't want to miss the benefits of a good selectivity estimate.)
		 */
		if (!oprrest)
		{
#if 1
			/*
			 * XXX temporary fix for 6.5: rtree operators are missing their
			 * selectivity estimators, so return a default estimate instead.
			 * Ugh.
			 */
			tempData = 0.5;
			temp = &tempData;
#else
			elog(ERROR,
				 "Operator %u must have a restriction selectivity estimator to be used in an index",
				 operatorObjectId);
#endif
		}
		else
			temp = (float64) fmgr(oprrest,
								  (char *) operatorObjectId,
								  (char *) indrelid,
								  (char *) (int) attributeNumber,
								  (char *) constValue,
								  (char *) constFlag,
								  NULL);
	}

	atp = SearchSysCacheTuple(RELOID,
							  ObjectIdGetDatum(indexrelid),
							  0, 0, 0);
	if (!HeapTupleIsValid(atp))
	{
		elog(ERROR, "btreenpage: no index tuple %u", indexrelid);
		return 0;
	}

	npage = ((Form_pg_class) GETSTRUCT(atp))->relpages;
	result = (float64) palloc(sizeof(float64data));
	*result = *temp * npage;
	return result;
}

float64
hashsel(Oid operatorObjectId,
		Oid indrelid,
		AttrNumber attributeNumber,
		char *constValue,
		int32 constFlag,
		int32 nIndexKeys,
		Oid indexrelid)
{

	float64		result;
	float64data resultData;
	HeapTuple	atp;
	int			ntuples;

	if (FunctionalSelectivity(nIndexKeys, attributeNumber))
	{

		/*
		 * Need to call the functions selectivity function here.  For now
		 * simply use 1/Number of Tuples since functions don't currently
		 * have selectivity functions
		 */

		atp = SearchSysCacheTuple(RELOID,
								  ObjectIdGetDatum(indexrelid),
								  0, 0, 0);
		if (!HeapTupleIsValid(atp))
		{
			elog(ERROR, "hashsel: no index tuple %u", indexrelid);
			return 0;
		}
		ntuples = ((Form_pg_class) GETSTRUCT(atp))->reltuples;
		if (ntuples > 0)
			resultData = 1.0 / (float64data) ntuples;
		else
			resultData = (float64data) (1.0 / 100.0);
		result = &resultData;

	}
	else
	{
		RegProcedure oprrest = get_oprrest(operatorObjectId);

		/*
		 * Operators used for indexes should have selectivity estimators.
		 * (An alternative is to default to 0.5, as the optimizer does in
		 * dealing with operators occurring in WHERE clauses, but if you
		 * are going to the trouble of making index support you probably
		 * don't want to miss the benefits of a good selectivity estimate.)
		 */
		if (!oprrest)
			elog(ERROR,
				 "Operator %u must have a restriction selectivity estimator to be used in a hash index",
				 operatorObjectId);

		result = (float64) fmgr(oprrest,
								(char *) operatorObjectId,
								(char *) indrelid,
								(char *) (int) attributeNumber,
								(char *) constValue,
								(char *) constFlag,
								NULL);
	}

	if (!PointerIsValid(result))
		elog(ERROR, "Hash Table Selectivity: bad pointer");
	if (*result < 0.0 || *result > 1.0)
		elog(ERROR, "Hash Table Selectivity: bad value %lf", *result);

	return result;


}

float64
hashnpage(Oid operatorObjectId,
		  Oid indrelid,
		  AttrNumber attributeNumber,
		  char *constValue,
		  int32 constFlag,
		  int32 nIndexKeys,
		  Oid indexrelid)
{
	float64		temp,
				result;
	float64data tempData;
	HeapTuple	atp;
	int			npage;
	int			ntuples;

	atp = SearchSysCacheTuple(RELOID,
							  ObjectIdGetDatum(indexrelid),
							  0, 0, 0);
	if (!HeapTupleIsValid(atp))
	{
		elog(ERROR, "hashsel: no index tuple %u", indexrelid);
		return 0;
	}


	if (FunctionalSelectivity(nIndexKeys, attributeNumber))
	{

		/*
		 * Need to call the functions selectivity function here.  For now,
		 * use 1/Number of Tuples since functions don't currently have
		 * selectivity functions
		 */

		ntuples = ((Form_pg_class) GETSTRUCT(atp))->reltuples;
		if (ntuples > 0)
			tempData = 1.0 / (float64data) ntuples;
		else
			tempData = (float64data) (1.0 / 100.0);
		temp = &tempData;

	}
	else
	{
		RegProcedure oprrest = get_oprrest(operatorObjectId);

		/*
		 * Operators used for indexes should have selectivity estimators.
		 * (An alternative is to default to 0.5, as the optimizer does in
		 * dealing with operators occurring in WHERE clauses, but if you
		 * are going to the trouble of making index support you probably
		 * don't want to miss the benefits of a good selectivity estimate.)
		 */
		if (!oprrest)
			elog(ERROR,
				 "Operator %u must have a restriction selectivity estimator to be used in a hash index",
				 operatorObjectId);

		temp = (float64) fmgr(oprrest,
							  (char *) operatorObjectId,
							  (char *) indrelid,
							  (char *) (int) attributeNumber,
							  (char *) constValue,
							  (char *) constFlag,
							  NULL);
	}

	npage = ((Form_pg_class) GETSTRUCT(atp))->relpages;
	result = (float64) palloc(sizeof(float64data));
	*result = *temp * npage;
	return result;
}


float64
rtsel(Oid operatorObjectId,
	  Oid indrelid,
	  AttrNumber attributeNumber,
	  char *constValue,
	  int32 constFlag,
	  int32 nIndexKeys,
	  Oid indexrelid)
{
	return (btreesel(operatorObjectId, indrelid, attributeNumber,
					 constValue, constFlag, nIndexKeys, indexrelid));
}

float64
rtnpage(Oid operatorObjectId,
		Oid indrelid,
		AttrNumber attributeNumber,
		char *constValue,
		int32 constFlag,
		int32 nIndexKeys,
		Oid indexrelid)
{
	return (btreenpage(operatorObjectId, indrelid, attributeNumber,
					   constValue, constFlag, nIndexKeys, indexrelid));
}

float64
gistsel(Oid operatorObjectId,
		Oid indrelid,
		AttrNumber attributeNumber,
		char *constValue,
		int32 constFlag,
		int32 nIndexKeys,
		Oid indexrelid)
{
	return (btreesel(operatorObjectId, indrelid, attributeNumber,
					 constValue, constFlag, nIndexKeys, indexrelid));
}

float64
gistnpage(Oid operatorObjectId,
		  Oid indrelid,
		  AttrNumber attributeNumber,
		  char *constValue,
		  int32 constFlag,
		  int32 nIndexKeys,
		  Oid indexrelid)
{
	return (btreenpage(operatorObjectId, indrelid, attributeNumber,
					   constValue, constFlag, nIndexKeys, indexrelid));
}
