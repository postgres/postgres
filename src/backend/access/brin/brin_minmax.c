/*
 * brin_minmax.c
 *		Implementation of Min/Max opclass for BRIN
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_minmax.c
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/brin_internal.h"
#include "access/brin_tuple.h"
#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "catalog/pg_amop.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


typedef struct MinmaxOpaque
{
	Oid			cached_subtype;
	FmgrInfo	strategy_procinfos[BTMaxStrategyNumber];
} MinmaxOpaque;

Datum		brin_minmax_opcinfo(PG_FUNCTION_ARGS);
Datum		brin_minmax_add_value(PG_FUNCTION_ARGS);
Datum		brin_minmax_consistent(PG_FUNCTION_ARGS);
Datum		brin_minmax_union(PG_FUNCTION_ARGS);
static FmgrInfo *minmax_get_strategy_procinfo(BrinDesc *bdesc, uint16 attno,
							 Oid subtype, uint16 strategynum);


Datum
brin_minmax_opcinfo(PG_FUNCTION_ARGS)
{
	Oid			typoid = PG_GETARG_OID(0);
	BrinOpcInfo *result;

	/*
	 * opaque->strategy_procinfos is initialized lazily; here it is set to
	 * all-uninitialized by palloc0 which sets fn_oid to InvalidOid.
	 */

	result = palloc0(MAXALIGN(SizeofBrinOpcInfo(2)) +
					 sizeof(MinmaxOpaque));
	result->oi_nstored = 2;
	result->oi_opaque = (MinmaxOpaque *)
		MAXALIGN((char *) result + SizeofBrinOpcInfo(2));
	result->oi_typcache[0] = result->oi_typcache[1] =
		lookup_type_cache(typoid, 0);

	PG_RETURN_POINTER(result);
}

/*
 * Examine the given index tuple (which contains partial status of a certain
 * page range) by comparing it to the given value that comes from another heap
 * tuple.  If the new value is outside the min/max range specified by the
 * existing tuple values, update the index tuple and return true.  Otherwise,
 * return false and do not modify in this case.
 */
Datum
brin_minmax_add_value(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	Datum		newval = PG_GETARG_DATUM(2);
	bool		isnull = PG_GETARG_DATUM(3);
	Oid			colloid = PG_GET_COLLATION();
	FmgrInfo   *cmpFn;
	Datum		compar;
	bool		updated = false;
	Form_pg_attribute attr;
	AttrNumber	attno;

	/*
	 * If the new value is null, we record that we saw it if it's the first
	 * one; otherwise, there's nothing to do.
	 */
	if (isnull)
	{
		if (column->bv_hasnulls)
			PG_RETURN_BOOL(false);

		column->bv_hasnulls = true;
		PG_RETURN_BOOL(true);
	}

	attno = column->bv_attno;
	attr = bdesc->bd_tupdesc->attrs[attno - 1];

	/*
	 * If the recorded value is null, store the new value (which we know to be
	 * not null) as both minimum and maximum, and we're done.
	 */
	if (column->bv_allnulls)
	{
		column->bv_values[0] = datumCopy(newval, attr->attbyval, attr->attlen);
		column->bv_values[1] = datumCopy(newval, attr->attbyval, attr->attlen);
		column->bv_allnulls = false;
		PG_RETURN_BOOL(true);
	}

	/*
	 * Otherwise, need to compare the new value with the existing boundaries
	 * and update them accordingly.  First check if it's less than the
	 * existing minimum.
	 */
	cmpFn = minmax_get_strategy_procinfo(bdesc, attno, attr->atttypid,
										 BTLessStrategyNumber);
	compar = FunctionCall2Coll(cmpFn, colloid, newval, column->bv_values[0]);
	if (DatumGetBool(compar))
	{
		if (!attr->attbyval)
			pfree(DatumGetPointer(column->bv_values[0]));
		column->bv_values[0] = datumCopy(newval, attr->attbyval, attr->attlen);
		updated = true;
	}

	/*
	 * And now compare it to the existing maximum.
	 */
	cmpFn = minmax_get_strategy_procinfo(bdesc, attno, attr->atttypid,
										 BTGreaterStrategyNumber);
	compar = FunctionCall2Coll(cmpFn, colloid, newval, column->bv_values[1]);
	if (DatumGetBool(compar))
	{
		if (!attr->attbyval)
			pfree(DatumGetPointer(column->bv_values[1]));
		column->bv_values[1] = datumCopy(newval, attr->attbyval, attr->attlen);
		updated = true;
	}

	PG_RETURN_BOOL(updated);
}

/*
 * Given an index tuple corresponding to a certain page range and a scan key,
 * return whether the scan key is consistent with the index tuple's min/max
 * values.  Return true if so, false otherwise.
 */
Datum
brin_minmax_consistent(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	ScanKey		key = (ScanKey) PG_GETARG_POINTER(2);
	Oid			colloid = PG_GET_COLLATION(),
				subtype;
	AttrNumber	attno;
	Datum		value;
	Datum		matches;
	FmgrInfo   *finfo;

	Assert(key->sk_attno == column->bv_attno);

	/* handle IS NULL/IS NOT NULL tests */
	if (key->sk_flags & SK_ISNULL)
	{
		if (key->sk_flags & SK_SEARCHNULL)
		{
			if (column->bv_allnulls || column->bv_hasnulls)
				PG_RETURN_BOOL(true);
			PG_RETURN_BOOL(false);
		}

		/*
		 * For IS NOT NULL, we can only skip ranges that are known to have
		 * only nulls.
		 */
		if (key->sk_flags & SK_SEARCHNOTNULL)
			PG_RETURN_BOOL(!column->bv_allnulls);

		/*
		 * Neither IS NULL nor IS NOT NULL was used; assume all indexable
		 * operators are strict and return false.
		 */
		PG_RETURN_BOOL(false);
	}

	/* if the range is all empty, it cannot possibly be consistent */
	if (column->bv_allnulls)
		PG_RETURN_BOOL(false);

	attno = key->sk_attno;
	subtype = key->sk_subtype;
	value = key->sk_argument;
	switch (key->sk_strategy)
	{
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
			finfo = minmax_get_strategy_procinfo(bdesc, attno, subtype,
												 key->sk_strategy);
			matches = FunctionCall2Coll(finfo, colloid, column->bv_values[0],
										value);
			break;
		case BTEqualStrategyNumber:

			/*
			 * In the equality case (WHERE col = someval), we want to return
			 * the current page range if the minimum value in the range <=
			 * scan key, and the maximum value >= scan key.
			 */
			finfo = minmax_get_strategy_procinfo(bdesc, attno, subtype,
												 BTLessEqualStrategyNumber);
			matches = FunctionCall2Coll(finfo, colloid, column->bv_values[0],
										value);
			if (!DatumGetBool(matches))
				break;
			/* max() >= scankey */
			finfo = minmax_get_strategy_procinfo(bdesc, attno, subtype,
											   BTGreaterEqualStrategyNumber);
			matches = FunctionCall2Coll(finfo, colloid, column->bv_values[1],
										value);
			break;
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			finfo = minmax_get_strategy_procinfo(bdesc, attno, subtype,
												 key->sk_strategy);
			matches = FunctionCall2Coll(finfo, colloid, column->bv_values[1],
										value);
			break;
		default:
			/* shouldn't happen */
			elog(ERROR, "invalid strategy number %d", key->sk_strategy);
			matches = 0;
			break;
	}

	PG_RETURN_DATUM(matches);
}

/*
 * Given two BrinValues, update the first of them as a union of the summary
 * values contained in both.  The second one is untouched.
 */
Datum
brin_minmax_union(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *col_a = (BrinValues *) PG_GETARG_POINTER(1);
	BrinValues *col_b = (BrinValues *) PG_GETARG_POINTER(2);
	Oid			colloid = PG_GET_COLLATION();
	AttrNumber	attno;
	Form_pg_attribute attr;
	FmgrInfo   *finfo;
	bool		needsadj;

	Assert(col_a->bv_attno == col_b->bv_attno);

	/* Adjust "hasnulls" */
	if (!col_a->bv_hasnulls && col_b->bv_hasnulls)
		col_a->bv_hasnulls = true;

	/* If there are no values in B, there's nothing left to do */
	if (col_b->bv_allnulls)
		PG_RETURN_VOID();

	attno = col_a->bv_attno;
	attr = bdesc->bd_tupdesc->attrs[attno - 1];

	/*
	 * Adjust "allnulls".  If A doesn't have values, just copy the values from
	 * B into A, and we're done.  We cannot run the operators in this case,
	 * because values in A might contain garbage.  Note we already established
	 * that B contains values.
	 */
	if (col_a->bv_allnulls)
	{
		col_a->bv_allnulls = false;
		col_a->bv_values[0] = datumCopy(col_b->bv_values[0],
										attr->attbyval, attr->attlen);
		col_a->bv_values[1] = datumCopy(col_b->bv_values[1],
										attr->attbyval, attr->attlen);
		PG_RETURN_VOID();
	}

	/* Adjust minimum, if B's min is less than A's min */
	finfo = minmax_get_strategy_procinfo(bdesc, attno, attr->atttypid,
										 BTLessStrategyNumber);
	needsadj = FunctionCall2Coll(finfo, colloid, col_b->bv_values[0],
								 col_a->bv_values[0]);
	if (needsadj)
	{
		if (!attr->attbyval)
			pfree(DatumGetPointer(col_a->bv_values[0]));
		col_a->bv_values[0] = datumCopy(col_b->bv_values[0],
										attr->attbyval, attr->attlen);
	}

	/* Adjust maximum, if B's max is greater than A's max */
	finfo = minmax_get_strategy_procinfo(bdesc, attno, attr->atttypid,
										 BTGreaterStrategyNumber);
	needsadj = FunctionCall2Coll(finfo, colloid, col_b->bv_values[1],
								 col_a->bv_values[1]);
	if (needsadj)
	{
		if (!attr->attbyval)
			pfree(DatumGetPointer(col_a->bv_values[1]));
		col_a->bv_values[1] = datumCopy(col_b->bv_values[1],
										attr->attbyval, attr->attlen);
	}

	PG_RETURN_VOID();
}

/*
 * Cache and return the procedure for the given strategy.
 *
 * Note: this function mirrors inclusion_get_strategy_procinfo; see notes
 * there.  If changes are made here, see that function too.
 */
static FmgrInfo *
minmax_get_strategy_procinfo(BrinDesc *bdesc, uint16 attno, Oid subtype,
							 uint16 strategynum)
{
	MinmaxOpaque *opaque;

	Assert(strategynum >= 1 &&
		   strategynum <= BTMaxStrategyNumber);

	opaque = (MinmaxOpaque *) bdesc->bd_info[attno - 1]->oi_opaque;

	/*
	 * We cache the procedures for the previous subtype in the opaque struct,
	 * to avoid repetitive syscache lookups.  If the subtype changed,
	 * invalidate all the cached entries.
	 */
	if (opaque->cached_subtype != subtype)
	{
		uint16		i;

		for (i = 1; i <= BTMaxStrategyNumber; i++)
			opaque->strategy_procinfos[i - 1].fn_oid = InvalidOid;
		opaque->cached_subtype = subtype;
	}

	if (opaque->strategy_procinfos[strategynum - 1].fn_oid == InvalidOid)
	{
		Form_pg_attribute attr;
		HeapTuple	tuple;
		Oid			opfamily,
					oprid;
		bool		isNull;

		opfamily = bdesc->bd_index->rd_opfamily[attno - 1];
		attr = bdesc->bd_tupdesc->attrs[attno - 1];
		tuple = SearchSysCache4(AMOPSTRATEGY, ObjectIdGetDatum(opfamily),
								ObjectIdGetDatum(attr->atttypid),
								ObjectIdGetDatum(subtype),
								Int16GetDatum(strategynum));

		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 strategynum, attr->atttypid, subtype, opfamily);

		oprid = DatumGetObjectId(SysCacheGetAttr(AMOPSTRATEGY, tuple,
											 Anum_pg_amop_amopopr, &isNull));
		ReleaseSysCache(tuple);
		Assert(!isNull && RegProcedureIsValid(oprid));

		fmgr_info_cxt(get_opcode(oprid),
					  &opaque->strategy_procinfos[strategynum - 1],
					  bdesc->bd_context);
	}

	return &opaque->strategy_procinfos[strategynum - 1];
}
