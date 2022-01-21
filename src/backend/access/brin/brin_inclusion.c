/*
 * brin_inclusion.c
 *		Implementation of inclusion opclasses for BRIN
 *
 * This module provides framework BRIN support functions for the "inclusion"
 * operator classes.  A few SQL-level support functions are also required for
 * each opclass.
 *
 * The "inclusion" BRIN strategy is useful for types that support R-Tree
 * operations.  This implementation is a straight mapping of those operations
 * to the block-range nature of BRIN, with two exceptions: (a) we explicitly
 * support "empty" elements: at least with range types, we need to consider
 * emptiness separately from regular R-Tree strategies; and (b) we need to
 * consider "unmergeable" elements, that is, a set of elements for whose union
 * no representation exists.  The only case where that happens as of this
 * writing is the INET type, where IPv6 values cannot be merged with IPv4
 * values.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_inclusion.c
 */
#include "postgres.h"

#include "access/brin_internal.h"
#include "access/brin_tuple.h"
#include "access/genam.h"
#include "access/skey.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * Additional SQL level support functions
 *
 * Procedure numbers must not use values reserved for BRIN itself; see
 * brin_internal.h.
 */
#define		INCLUSION_MAX_PROCNUMS	4	/* maximum support procs we need */
#define		PROCNUM_MERGE			11	/* required */
#define		PROCNUM_MERGEABLE		12	/* optional */
#define		PROCNUM_CONTAINS		13	/* optional */
#define		PROCNUM_EMPTY			14	/* optional */


/*
 * Subtract this from procnum to obtain index in InclusionOpaque arrays
 * (Must be equal to minimum of private procnums).
 */
#define		PROCNUM_BASE			11

/*-
 * The values stored in the bv_values arrays correspond to:
 *
 * INCLUSION_UNION
 *		the union of the values in the block range
 * INCLUSION_UNMERGEABLE
 *		whether the values in the block range cannot be merged
 *		(e.g. an IPv6 address amidst IPv4 addresses)
 * INCLUSION_CONTAINS_EMPTY
 *		whether an empty value is present in any tuple
 *		in the block range
 */
#define INCLUSION_UNION				0
#define INCLUSION_UNMERGEABLE		1
#define INCLUSION_CONTAINS_EMPTY	2


typedef struct InclusionOpaque
{
	FmgrInfo	extra_procinfos[INCLUSION_MAX_PROCNUMS];
	bool		extra_proc_missing[INCLUSION_MAX_PROCNUMS];
	Oid			cached_subtype;
	FmgrInfo	strategy_procinfos[RTMaxStrategyNumber];
} InclusionOpaque;

static FmgrInfo *inclusion_get_procinfo(BrinDesc *bdesc, uint16 attno,
										uint16 procnum);
static FmgrInfo *inclusion_get_strategy_procinfo(BrinDesc *bdesc, uint16 attno,
												 Oid subtype, uint16 strategynum);


/*
 * BRIN inclusion OpcInfo function
 */
Datum
brin_inclusion_opcinfo(PG_FUNCTION_ARGS)
{
	Oid			typoid = PG_GETARG_OID(0);
	BrinOpcInfo *result;
	TypeCacheEntry *bool_typcache = lookup_type_cache(BOOLOID, 0);

	/*
	 * All members of opaque are initialized lazily; both procinfo arrays
	 * start out as non-initialized by having fn_oid be InvalidOid, and
	 * "missing" to false, by zeroing here.  strategy_procinfos elements can
	 * be invalidated when cached_subtype changes by zeroing fn_oid.
	 * extra_procinfo entries are never invalidated, but if a lookup fails
	 * (which is expected), extra_proc_missing is set to true, indicating not
	 * to look it up again.
	 */
	result = palloc0(MAXALIGN(SizeofBrinOpcInfo(3)) + sizeof(InclusionOpaque));
	result->oi_nstored = 3;
	result->oi_regular_nulls = true;
	result->oi_opaque = (InclusionOpaque *)
		MAXALIGN((char *) result + SizeofBrinOpcInfo(3));

	/* the union */
	result->oi_typcache[INCLUSION_UNION] =
		lookup_type_cache(typoid, 0);

	/* includes elements that are not mergeable */
	result->oi_typcache[INCLUSION_UNMERGEABLE] = bool_typcache;

	/* includes the empty element */
	result->oi_typcache[INCLUSION_CONTAINS_EMPTY] = bool_typcache;

	PG_RETURN_POINTER(result);
}

/*
 * BRIN inclusion add value function
 *
 * Examine the given index tuple (which contains partial status of a certain
 * page range) by comparing it to the given value that comes from another heap
 * tuple.  If the new value is outside the union specified by the existing
 * tuple values, update the index tuple and return true.  Otherwise, return
 * false and do not modify in this case.
 */
Datum
brin_inclusion_add_value(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	Datum		newval = PG_GETARG_DATUM(2);
	bool		isnull PG_USED_FOR_ASSERTS_ONLY = PG_GETARG_BOOL(3);
	Oid			colloid = PG_GET_COLLATION();
	FmgrInfo   *finfo;
	Datum		result;
	bool		new = false;
	AttrNumber	attno;
	Form_pg_attribute attr;

	Assert(!isnull);

	attno = column->bv_attno;
	attr = TupleDescAttr(bdesc->bd_tupdesc, attno - 1);

	/*
	 * If the recorded value is null, copy the new value (which we know to be
	 * not null), and we're almost done.
	 */
	if (column->bv_allnulls)
	{
		column->bv_values[INCLUSION_UNION] =
			datumCopy(newval, attr->attbyval, attr->attlen);
		column->bv_values[INCLUSION_UNMERGEABLE] = BoolGetDatum(false);
		column->bv_values[INCLUSION_CONTAINS_EMPTY] = BoolGetDatum(false);
		column->bv_allnulls = false;
		new = true;
	}

	/*
	 * No need for further processing if the block range is marked as
	 * containing unmergeable values.
	 */
	if (DatumGetBool(column->bv_values[INCLUSION_UNMERGEABLE]))
		PG_RETURN_BOOL(false);

	/*
	 * If the opclass supports the concept of empty values, test the passed
	 * new value for emptiness; if it returns true, we need to set the
	 * "contains empty" flag in the element (unless already set).
	 */
	finfo = inclusion_get_procinfo(bdesc, attno, PROCNUM_EMPTY);
	if (finfo != NULL && DatumGetBool(FunctionCall1Coll(finfo, colloid, newval)))
	{
		if (!DatumGetBool(column->bv_values[INCLUSION_CONTAINS_EMPTY]))
		{
			column->bv_values[INCLUSION_CONTAINS_EMPTY] = BoolGetDatum(true);
			PG_RETURN_BOOL(true);
		}

		PG_RETURN_BOOL(false);
	}

	if (new)
		PG_RETURN_BOOL(true);

	/* Check if the new value is already contained. */
	finfo = inclusion_get_procinfo(bdesc, attno, PROCNUM_CONTAINS);
	if (finfo != NULL &&
		DatumGetBool(FunctionCall2Coll(finfo, colloid,
									   column->bv_values[INCLUSION_UNION],
									   newval)))
		PG_RETURN_BOOL(false);

	/*
	 * Check if the new value is mergeable to the existing union.  If it is
	 * not, mark the value as containing unmergeable elements and get out.
	 *
	 * Note: at this point we could remove the value from the union, since
	 * it's not going to be used any longer.  However, the BRIN framework
	 * doesn't allow for the value not being present.  Improve someday.
	 */
	finfo = inclusion_get_procinfo(bdesc, attno, PROCNUM_MERGEABLE);
	if (finfo != NULL &&
		!DatumGetBool(FunctionCall2Coll(finfo, colloid,
										column->bv_values[INCLUSION_UNION],
										newval)))
	{
		column->bv_values[INCLUSION_UNMERGEABLE] = BoolGetDatum(true);
		PG_RETURN_BOOL(true);
	}

	/* Finally, merge the new value to the existing union. */
	finfo = inclusion_get_procinfo(bdesc, attno, PROCNUM_MERGE);
	Assert(finfo != NULL);
	result = FunctionCall2Coll(finfo, colloid,
							   column->bv_values[INCLUSION_UNION], newval);
	if (!attr->attbyval &&
		DatumGetPointer(result) != DatumGetPointer(column->bv_values[INCLUSION_UNION]))
	{
		pfree(DatumGetPointer(column->bv_values[INCLUSION_UNION]));

		if (result == newval)
			result = datumCopy(result, attr->attbyval, attr->attlen);
	}
	column->bv_values[INCLUSION_UNION] = result;

	PG_RETURN_BOOL(true);
}

/*
 * BRIN inclusion consistent function
 *
 * We're no longer dealing with NULL keys in the consistent function, that is
 * now handled by the AM code. That means we should not get any all-NULL ranges
 * either, because those can't be consistent with regular (not [IS] NULL) keys.
 *
 * All of the strategies are optional.
 */
Datum
brin_inclusion_consistent(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	ScanKey		key = (ScanKey) PG_GETARG_POINTER(2);
	Oid			colloid = PG_GET_COLLATION(),
				subtype;
	Datum		unionval;
	AttrNumber	attno;
	Datum		query;
	FmgrInfo   *finfo;
	Datum		result;

	/* This opclass uses the old signature with only three arguments. */
	Assert(PG_NARGS() == 3);

	/* Should not be dealing with all-NULL ranges. */
	Assert(!column->bv_allnulls);

	/* It has to be checked, if it contains elements that are not mergeable. */
	if (DatumGetBool(column->bv_values[INCLUSION_UNMERGEABLE]))
		PG_RETURN_BOOL(true);

	attno = key->sk_attno;
	subtype = key->sk_subtype;
	query = key->sk_argument;
	unionval = column->bv_values[INCLUSION_UNION];
	switch (key->sk_strategy)
	{
			/*
			 * Placement strategies
			 *
			 * These are implemented by logically negating the result of the
			 * converse placement operator; for this to work, the converse
			 * operator must be part of the opclass.  An error will be thrown
			 * by inclusion_get_strategy_procinfo() if the required strategy
			 * is not part of the opclass.
			 *
			 * These all return false if either argument is empty, so there is
			 * no need to check for empty elements.
			 */

		case RTLeftStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTOverRightStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		case RTOverLeftStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTRightStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		case RTOverRightStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTLeftStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		case RTRightStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTOverLeftStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		case RTBelowStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTOverAboveStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		case RTOverBelowStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTAboveStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		case RTOverAboveStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTBelowStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		case RTAboveStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTOverBelowStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

			/*
			 * Overlap and contains strategies
			 *
			 * These strategies are simple enough that we can simply call the
			 * operator and return its result.  Empty elements don't change
			 * the result.
			 */

		case RTOverlapStrategyNumber:
		case RTContainsStrategyNumber:
		case RTContainsElemStrategyNumber:
		case RTSubStrategyNumber:
		case RTSubEqualStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													key->sk_strategy);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_DATUM(result);

			/*
			 * Contained by strategies
			 *
			 * We cannot just call the original operator for the contained by
			 * strategies because some elements can be contained even though
			 * the union is not; instead we use the overlap operator.
			 *
			 * We check for empty elements separately as they are not merged
			 * to the union but contained by everything.
			 */

		case RTContainedByStrategyNumber:
		case RTSuperStrategyNumber:
		case RTSuperEqualStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTOverlapStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			if (DatumGetBool(result))
				PG_RETURN_BOOL(true);

			PG_RETURN_DATUM(column->bv_values[INCLUSION_CONTAINS_EMPTY]);

			/*
			 * Adjacent strategy
			 *
			 * We test for overlap first but to be safe we need to call the
			 * actual adjacent operator also.
			 *
			 * An empty element cannot be adjacent to any other, so there is
			 * no need to check for it.
			 */

		case RTAdjacentStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTOverlapStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			if (DatumGetBool(result))
				PG_RETURN_BOOL(true);

			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTAdjacentStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_DATUM(result);

			/*
			 * Basic comparison strategies
			 *
			 * It is straightforward to support the equality strategies with
			 * the contains operator.  Generally, inequality strategies do not
			 * make much sense for the types which will be used with the
			 * inclusion BRIN family of opclasses, but it is possible to
			 * implement them with logical negation of the left-of and
			 * right-of operators.
			 *
			 * NB: These strategies cannot be used with geometric datatypes
			 * that use comparison of areas!  The only exception is the "same"
			 * strategy.
			 *
			 * Empty elements are considered to be less than the others.  We
			 * cannot use the empty support function to check the query is an
			 * empty element, because the query can be another data type than
			 * the empty support function argument.  So we will return true,
			 * if there is a possibility that empty elements will change the
			 * result.
			 */

		case RTLessStrategyNumber:
		case RTLessEqualStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTRightStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			if (!DatumGetBool(result))
				PG_RETURN_BOOL(true);

			PG_RETURN_DATUM(column->bv_values[INCLUSION_CONTAINS_EMPTY]);

		case RTSameStrategyNumber:
		case RTEqualStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTContainsStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			if (DatumGetBool(result))
				PG_RETURN_BOOL(true);

			PG_RETURN_DATUM(column->bv_values[INCLUSION_CONTAINS_EMPTY]);

		case RTGreaterEqualStrategyNumber:
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTLeftStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			if (!DatumGetBool(result))
				PG_RETURN_BOOL(true);

			PG_RETURN_DATUM(column->bv_values[INCLUSION_CONTAINS_EMPTY]);

		case RTGreaterStrategyNumber:
			/* no need to check for empty elements */
			finfo = inclusion_get_strategy_procinfo(bdesc, attno, subtype,
													RTLeftStrategyNumber);
			result = FunctionCall2Coll(finfo, colloid, unionval, query);
			PG_RETURN_BOOL(!DatumGetBool(result));

		default:
			/* shouldn't happen */
			elog(ERROR, "invalid strategy number %d", key->sk_strategy);
			PG_RETURN_BOOL(false);
	}
}

/*
 * BRIN inclusion union function
 *
 * Given two BrinValues, update the first of them as a union of the summary
 * values contained in both.  The second one is untouched.
 */
Datum
brin_inclusion_union(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *col_a = (BrinValues *) PG_GETARG_POINTER(1);
	BrinValues *col_b = (BrinValues *) PG_GETARG_POINTER(2);
	Oid			colloid = PG_GET_COLLATION();
	AttrNumber	attno;
	Form_pg_attribute attr;
	FmgrInfo   *finfo;
	Datum		result;

	Assert(col_a->bv_attno == col_b->bv_attno);
	Assert(!col_a->bv_allnulls && !col_b->bv_allnulls);

	attno = col_a->bv_attno;
	attr = TupleDescAttr(bdesc->bd_tupdesc, attno - 1);

	/* If B includes empty elements, mark A similarly, if needed. */
	if (!DatumGetBool(col_a->bv_values[INCLUSION_CONTAINS_EMPTY]) &&
		DatumGetBool(col_b->bv_values[INCLUSION_CONTAINS_EMPTY]))
		col_a->bv_values[INCLUSION_CONTAINS_EMPTY] = BoolGetDatum(true);

	/* Check if A includes elements that are not mergeable. */
	if (DatumGetBool(col_a->bv_values[INCLUSION_UNMERGEABLE]))
		PG_RETURN_VOID();

	/* If B includes elements that are not mergeable, mark A similarly. */
	if (DatumGetBool(col_b->bv_values[INCLUSION_UNMERGEABLE]))
	{
		col_a->bv_values[INCLUSION_UNMERGEABLE] = BoolGetDatum(true);
		PG_RETURN_VOID();
	}

	/* Check if A and B are mergeable; if not, mark A unmergeable. */
	finfo = inclusion_get_procinfo(bdesc, attno, PROCNUM_MERGEABLE);
	if (finfo != NULL &&
		!DatumGetBool(FunctionCall2Coll(finfo, colloid,
										col_a->bv_values[INCLUSION_UNION],
										col_b->bv_values[INCLUSION_UNION])))
	{
		col_a->bv_values[INCLUSION_UNMERGEABLE] = BoolGetDatum(true);
		PG_RETURN_VOID();
	}

	/* Finally, merge B to A. */
	finfo = inclusion_get_procinfo(bdesc, attno, PROCNUM_MERGE);
	Assert(finfo != NULL);
	result = FunctionCall2Coll(finfo, colloid,
							   col_a->bv_values[INCLUSION_UNION],
							   col_b->bv_values[INCLUSION_UNION]);
	if (!attr->attbyval &&
		DatumGetPointer(result) != DatumGetPointer(col_a->bv_values[INCLUSION_UNION]))
	{
		pfree(DatumGetPointer(col_a->bv_values[INCLUSION_UNION]));

		if (result == col_b->bv_values[INCLUSION_UNION])
			result = datumCopy(result, attr->attbyval, attr->attlen);
	}
	col_a->bv_values[INCLUSION_UNION] = result;

	PG_RETURN_VOID();
}

/*
 * Cache and return inclusion opclass support procedure
 *
 * Return the procedure corresponding to the given function support number
 * or null if it is not exists.
 */
static FmgrInfo *
inclusion_get_procinfo(BrinDesc *bdesc, uint16 attno, uint16 procnum)
{
	InclusionOpaque *opaque;
	uint16		basenum = procnum - PROCNUM_BASE;

	/*
	 * We cache these in the opaque struct, to avoid repetitive syscache
	 * lookups.
	 */
	opaque = (InclusionOpaque *) bdesc->bd_info[attno - 1]->oi_opaque;

	/*
	 * If we already searched for this proc and didn't find it, don't bother
	 * searching again.
	 */
	if (opaque->extra_proc_missing[basenum])
		return NULL;

	if (opaque->extra_procinfos[basenum].fn_oid == InvalidOid)
	{
		if (RegProcedureIsValid(index_getprocid(bdesc->bd_index, attno,
												procnum)))
		{
			fmgr_info_copy(&opaque->extra_procinfos[basenum],
						   index_getprocinfo(bdesc->bd_index, attno, procnum),
						   bdesc->bd_context);
		}
		else
		{
			opaque->extra_proc_missing[basenum] = true;
			return NULL;
		}
	}

	return &opaque->extra_procinfos[basenum];
}

/*
 * Cache and return the procedure of the given strategy
 *
 * Return the procedure corresponding to the given sub-type and strategy
 * number.  The data type of the index will be used as the left hand side of
 * the operator and the given sub-type will be used as the right hand side.
 * Throws an error if the pg_amop row does not exist, but that should not
 * happen with a properly configured opclass.
 *
 * It always throws an error when the data type of the opclass is different
 * from the data type of the column or the expression.  That happens when the
 * column data type has implicit cast to the opclass data type.  We don't
 * bother casting types, because this situation can easily be avoided by
 * setting storage data type to that of the opclass.  The same problem does not
 * apply to the data type of the right hand side, because the type in the
 * ScanKey always matches the opclass' one.
 *
 * Note: this function mirrors minmax_get_strategy_procinfo; if changes are
 * made here, see that function too.
 */
static FmgrInfo *
inclusion_get_strategy_procinfo(BrinDesc *bdesc, uint16 attno, Oid subtype,
								uint16 strategynum)
{
	InclusionOpaque *opaque;

	Assert(strategynum >= 1 &&
		   strategynum <= RTMaxStrategyNumber);

	opaque = (InclusionOpaque *) bdesc->bd_info[attno - 1]->oi_opaque;

	/*
	 * We cache the procedures for the last sub-type in the opaque struct, to
	 * avoid repetitive syscache lookups.  If the sub-type is changed,
	 * invalidate all the cached entries.
	 */
	if (opaque->cached_subtype != subtype)
	{
		uint16		i;

		for (i = 1; i <= RTMaxStrategyNumber; i++)
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
		attr = TupleDescAttr(bdesc->bd_tupdesc, attno - 1);
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
