/*-------------------------------------------------------------------------
 *
 * enum.c
 *	  I/O functions, operators, aggregates etc for enum types
 *
 * Copyright (c) 2006-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/enum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_enum.h"
#include "libpq/pqformat.h"
#include "storage/procarray.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


static Oid	enum_endpoint(Oid enumtypoid, ScanDirection direction);
static ArrayType *enum_range_internal(Oid enumtypoid, Oid lower, Oid upper);


/*
 * Disallow use of an uncommitted pg_enum tuple.
 *
 * We need to make sure that uncommitted enum values don't get into indexes.
 * If they did, and if we then rolled back the pg_enum addition, we'd have
 * broken the index because value comparisons will not work reliably without
 * an underlying pg_enum entry.  (Note that removal of the heap entry
 * containing an enum value is not sufficient to ensure that it doesn't appear
 * in upper levels of indexes.)  To do this we prevent an uncommitted row from
 * being used for any SQL-level purpose.  This is stronger than necessary,
 * since the value might not be getting inserted into a table or there might
 * be no index on its column, but it's easy to enforce centrally.
 *
 * However, it's okay to allow use of uncommitted values belonging to enum
 * types that were themselves created in the same transaction, because then
 * any such index would also be new and would go away altogether on rollback.
 * We don't implement that fully right now, but we do allow free use of enum
 * values created during CREATE TYPE AS ENUM, which are surely of the same
 * lifespan as the enum type.  (This case is required by "pg_restore -1".)
 * Values added by ALTER TYPE ADD VALUE are currently restricted, but could
 * be allowed if the enum type could be proven to have been created earlier
 * in the same transaction.  (Note that comparing tuple xmins would not work
 * for that, because the type tuple might have been updated in the current
 * transaction.  Subtransactions also create hazards to be accounted for.)
 *
 * This function needs to be called (directly or indirectly) in any of the
 * functions below that could return an enum value to SQL operations.
 */
static void
check_safe_enum_use(HeapTuple enumval_tup)
{
	TransactionId xmin;
	Form_pg_enum en = (Form_pg_enum) GETSTRUCT(enumval_tup);

	/*
	 * If the row is hinted as committed, it's surely safe.  This provides a
	 * fast path for all normal use-cases.
	 */
	if (HeapTupleHeaderXminCommitted(enumval_tup->t_data))
		return;

	/*
	 * Usually, a row would get hinted as committed when it's read or loaded
	 * into syscache; but just in case not, let's check the xmin directly.
	 */
	xmin = HeapTupleHeaderGetXmin(enumval_tup->t_data);
	if (!TransactionIdIsInProgress(xmin) &&
		TransactionIdDidCommit(xmin))
		return;

	/*
	 * Check if the enum value is uncommitted.  If not, it's safe, because it
	 * was made during CREATE TYPE AS ENUM and can't be shorter-lived than its
	 * owning type.  (This'd also be false for values made by other
	 * transactions; but the previous tests should have handled all of those.)
	 */
	if (!EnumUncommitted(en->oid))
		return;

	/*
	 * There might well be other tests we could do here to narrow down the
	 * unsafe conditions, but for now just raise an exception.
	 */
	ereport(ERROR,
			(errcode(ERRCODE_UNSAFE_NEW_ENUM_VALUE_USAGE),
			 errmsg("unsafe use of new value \"%s\" of enum type %s",
					NameStr(en->enumlabel),
					format_type_be(en->enumtypid)),
			 errhint("New enum values must be committed before they can be used.")));
}


/* Basic I/O support */

Datum
enum_in(PG_FUNCTION_ARGS)
{
	char	   *name = PG_GETARG_CSTRING(0);
	Oid			enumtypoid = PG_GETARG_OID(1);
	Node	   *escontext = fcinfo->context;
	Oid			enumoid;
	HeapTuple	tup;

	/* must check length to prevent Assert failure within SearchSysCache */
	if (strlen(name) >= NAMEDATALEN)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input value for enum %s: \"%s\"",
						format_type_be(enumtypoid),
						name)));

	tup = SearchSysCache2(ENUMTYPOIDNAME,
						  ObjectIdGetDatum(enumtypoid),
						  CStringGetDatum(name));
	if (!HeapTupleIsValid(tup))
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input value for enum %s: \"%s\"",
						format_type_be(enumtypoid),
						name)));

	/*
	 * Check it's safe to use in SQL.  Perhaps we should take the trouble to
	 * report "unsafe use" softly; but it's unclear that it's worth the
	 * trouble, or indeed that that is a legitimate bad-input case at all
	 * rather than an implementation shortcoming.
	 */
	check_safe_enum_use(tup);

	/*
	 * This comes from pg_enum.oid and stores system oids in user tables. This
	 * oid must be preserved by binary upgrades.
	 */
	enumoid = ((Form_pg_enum) GETSTRUCT(tup))->oid;

	ReleaseSysCache(tup);

	PG_RETURN_OID(enumoid);
}

Datum
enum_out(PG_FUNCTION_ARGS)
{
	Oid			enumval = PG_GETARG_OID(0);
	char	   *result;
	HeapTuple	tup;
	Form_pg_enum en;

	tup = SearchSysCache1(ENUMOID, ObjectIdGetDatum(enumval));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid internal value for enum: %u",
						enumval)));
	en = (Form_pg_enum) GETSTRUCT(tup);

	result = pstrdup(NameStr(en->enumlabel));

	ReleaseSysCache(tup);

	PG_RETURN_CSTRING(result);
}

/* Binary I/O support */
Datum
enum_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	Oid			enumtypoid = PG_GETARG_OID(1);
	Oid			enumoid;
	HeapTuple	tup;
	char	   *name;
	int			nbytes;

	name = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);

	/* must check length to prevent Assert failure within SearchSysCache */
	if (strlen(name) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input value for enum %s: \"%s\"",
						format_type_be(enumtypoid),
						name)));

	tup = SearchSysCache2(ENUMTYPOIDNAME,
						  ObjectIdGetDatum(enumtypoid),
						  CStringGetDatum(name));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input value for enum %s: \"%s\"",
						format_type_be(enumtypoid),
						name)));

	/* check it's safe to use in SQL */
	check_safe_enum_use(tup);

	enumoid = ((Form_pg_enum) GETSTRUCT(tup))->oid;

	ReleaseSysCache(tup);

	pfree(name);

	PG_RETURN_OID(enumoid);
}

Datum
enum_send(PG_FUNCTION_ARGS)
{
	Oid			enumval = PG_GETARG_OID(0);
	StringInfoData buf;
	HeapTuple	tup;
	Form_pg_enum en;

	tup = SearchSysCache1(ENUMOID, ObjectIdGetDatum(enumval));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid internal value for enum: %u",
						enumval)));
	en = (Form_pg_enum) GETSTRUCT(tup);

	pq_begintypsend(&buf);
	pq_sendtext(&buf, NameStr(en->enumlabel), strlen(NameStr(en->enumlabel)));

	ReleaseSysCache(tup);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/* Comparison functions and related */

/*
 * enum_cmp_internal is the common engine for all the visible comparison
 * functions, except for enum_eq and enum_ne which can just check for OID
 * equality directly.
 */
static int
enum_cmp_internal(Oid arg1, Oid arg2, FunctionCallInfo fcinfo)
{
	TypeCacheEntry *tcache;

	/*
	 * We don't need the typcache except in the hopefully-uncommon case that
	 * one or both Oids are odd.  This means that cursory testing of code that
	 * fails to pass flinfo to an enum comparison function might not disclose
	 * the oversight.  To make such errors more obvious, Assert that we have a
	 * place to cache even when we take a fast-path exit.
	 */
	Assert(fcinfo->flinfo != NULL);

	/* Equal OIDs are equal no matter what */
	if (arg1 == arg2)
		return 0;

	/* Fast path: even-numbered Oids are known to compare correctly */
	if ((arg1 & 1) == 0 && (arg2 & 1) == 0)
	{
		if (arg1 < arg2)
			return -1;
		else
			return 1;
	}

	/* Locate the typcache entry for the enum type */
	tcache = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
	if (tcache == NULL)
	{
		HeapTuple	enum_tup;
		Form_pg_enum en;
		Oid			typeoid;

		/* Get the OID of the enum type containing arg1 */
		enum_tup = SearchSysCache1(ENUMOID, ObjectIdGetDatum(arg1));
		if (!HeapTupleIsValid(enum_tup))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("invalid internal value for enum: %u",
							arg1)));
		en = (Form_pg_enum) GETSTRUCT(enum_tup);
		typeoid = en->enumtypid;
		ReleaseSysCache(enum_tup);
		/* Now locate and remember the typcache entry */
		tcache = lookup_type_cache(typeoid, 0);
		fcinfo->flinfo->fn_extra = (void *) tcache;
	}

	/* The remaining comparison logic is in typcache.c */
	return compare_values_of_enum(tcache, arg1, arg2);
}

Datum
enum_lt(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(enum_cmp_internal(a, b, fcinfo) < 0);
}

Datum
enum_le(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(enum_cmp_internal(a, b, fcinfo) <= 0);
}

Datum
enum_eq(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(a == b);
}

Datum
enum_ne(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(a != b);
}

Datum
enum_ge(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(enum_cmp_internal(a, b, fcinfo) >= 0);
}

Datum
enum_gt(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(enum_cmp_internal(a, b, fcinfo) > 0);
}

Datum
enum_smaller(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_OID(enum_cmp_internal(a, b, fcinfo) < 0 ? a : b);
}

Datum
enum_larger(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_OID(enum_cmp_internal(a, b, fcinfo) > 0 ? a : b);
}

Datum
enum_cmp(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	PG_RETURN_INT32(enum_cmp_internal(a, b, fcinfo));
}

/* Enum programming support functions */

/*
 * enum_endpoint: common code for enum_first/enum_last
 */
static Oid
enum_endpoint(Oid enumtypoid, ScanDirection direction)
{
	Relation	enum_rel;
	Relation	enum_idx;
	SysScanDesc enum_scan;
	HeapTuple	enum_tuple;
	ScanKeyData skey;
	Oid			minmax;

	/*
	 * Find the first/last enum member using pg_enum_typid_sortorder_index.
	 * Note we must not use the syscache.  See comments for RenumberEnumType
	 * in catalog/pg_enum.c for more info.
	 */
	ScanKeyInit(&skey,
				Anum_pg_enum_enumtypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(enumtypoid));

	enum_rel = table_open(EnumRelationId, AccessShareLock);
	enum_idx = index_open(EnumTypIdSortOrderIndexId, AccessShareLock);
	enum_scan = systable_beginscan_ordered(enum_rel, enum_idx, NULL,
										   1, &skey);

	enum_tuple = systable_getnext_ordered(enum_scan, direction);
	if (HeapTupleIsValid(enum_tuple))
	{
		/* check it's safe to use in SQL */
		check_safe_enum_use(enum_tuple);
		minmax = ((Form_pg_enum) GETSTRUCT(enum_tuple))->oid;
	}
	else
	{
		/* should only happen with an empty enum */
		minmax = InvalidOid;
	}

	systable_endscan_ordered(enum_scan);
	index_close(enum_idx, AccessShareLock);
	table_close(enum_rel, AccessShareLock);

	return minmax;
}

Datum
enum_first(PG_FUNCTION_ARGS)
{
	Oid			enumtypoid;
	Oid			min;

	/*
	 * We rely on being able to get the specific enum type from the calling
	 * expression tree.  Notice that the actual value of the argument isn't
	 * examined at all; in particular it might be NULL.
	 */
	enumtypoid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (enumtypoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine actual enum type")));

	/* Get the OID using the index */
	min = enum_endpoint(enumtypoid, ForwardScanDirection);

	if (!OidIsValid(min))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("enum %s contains no values",
						format_type_be(enumtypoid))));

	PG_RETURN_OID(min);
}

Datum
enum_last(PG_FUNCTION_ARGS)
{
	Oid			enumtypoid;
	Oid			max;

	/*
	 * We rely on being able to get the specific enum type from the calling
	 * expression tree.  Notice that the actual value of the argument isn't
	 * examined at all; in particular it might be NULL.
	 */
	enumtypoid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (enumtypoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine actual enum type")));

	/* Get the OID using the index */
	max = enum_endpoint(enumtypoid, BackwardScanDirection);

	if (!OidIsValid(max))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("enum %s contains no values",
						format_type_be(enumtypoid))));

	PG_RETURN_OID(max);
}

/* 2-argument variant of enum_range */
Datum
enum_range_bounds(PG_FUNCTION_ARGS)
{
	Oid			lower;
	Oid			upper;
	Oid			enumtypoid;

	if (PG_ARGISNULL(0))
		lower = InvalidOid;
	else
		lower = PG_GETARG_OID(0);
	if (PG_ARGISNULL(1))
		upper = InvalidOid;
	else
		upper = PG_GETARG_OID(1);

	/*
	 * We rely on being able to get the specific enum type from the calling
	 * expression tree.  The generic type mechanism should have ensured that
	 * both are of the same type.
	 */
	enumtypoid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (enumtypoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine actual enum type")));

	PG_RETURN_ARRAYTYPE_P(enum_range_internal(enumtypoid, lower, upper));
}

/* 1-argument variant of enum_range */
Datum
enum_range_all(PG_FUNCTION_ARGS)
{
	Oid			enumtypoid;

	/*
	 * We rely on being able to get the specific enum type from the calling
	 * expression tree.  Notice that the actual value of the argument isn't
	 * examined at all; in particular it might be NULL.
	 */
	enumtypoid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (enumtypoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine actual enum type")));

	PG_RETURN_ARRAYTYPE_P(enum_range_internal(enumtypoid,
											  InvalidOid, InvalidOid));
}

static ArrayType *
enum_range_internal(Oid enumtypoid, Oid lower, Oid upper)
{
	ArrayType  *result;
	Relation	enum_rel;
	Relation	enum_idx;
	SysScanDesc enum_scan;
	HeapTuple	enum_tuple;
	ScanKeyData skey;
	Datum	   *elems;
	int			max,
				cnt;
	bool		left_found;

	/*
	 * Scan the enum members in order using pg_enum_typid_sortorder_index.
	 * Note we must not use the syscache.  See comments for RenumberEnumType
	 * in catalog/pg_enum.c for more info.
	 */
	ScanKeyInit(&skey,
				Anum_pg_enum_enumtypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(enumtypoid));

	enum_rel = table_open(EnumRelationId, AccessShareLock);
	enum_idx = index_open(EnumTypIdSortOrderIndexId, AccessShareLock);
	enum_scan = systable_beginscan_ordered(enum_rel, enum_idx, NULL, 1, &skey);

	max = 64;
	elems = (Datum *) palloc(max * sizeof(Datum));
	cnt = 0;
	left_found = !OidIsValid(lower);

	while (HeapTupleIsValid(enum_tuple = systable_getnext_ordered(enum_scan, ForwardScanDirection)))
	{
		Oid			enum_oid = ((Form_pg_enum) GETSTRUCT(enum_tuple))->oid;

		if (!left_found && lower == enum_oid)
			left_found = true;

		if (left_found)
		{
			/* check it's safe to use in SQL */
			check_safe_enum_use(enum_tuple);

			if (cnt >= max)
			{
				max *= 2;
				elems = (Datum *) repalloc(elems, max * sizeof(Datum));
			}

			elems[cnt++] = ObjectIdGetDatum(enum_oid);
		}

		if (OidIsValid(upper) && upper == enum_oid)
			break;
	}

	systable_endscan_ordered(enum_scan);
	index_close(enum_idx, AccessShareLock);
	table_close(enum_rel, AccessShareLock);

	/* and build the result array */
	/* note this hardwires some details about the representation of Oid */
	result = construct_array(elems, cnt, enumtypoid,
							 sizeof(Oid), true, TYPALIGN_INT);

	pfree(elems);

	return result;
}
