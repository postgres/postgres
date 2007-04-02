/*-------------------------------------------------------------------------
 *
 * enum.c
 *    I/O functions, operators, aggregates etc for enum types
 *
 * Copyright (c) 2006-2007, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *    $PostgreSQL: pgsql/src/backend/utils/adt/enum.c,v 1.2 2007/04/02 22:14:17 adunstan Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_enum.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static Oid	cstring_enum(char *name, Oid enumtypoid);
static char *enum_cstring(Oid enumval);
static ArrayType *enum_range_internal(Oid enumtypoid, Oid lower, Oid upper);
static int	enum_elem_cmp(const void *left, const void *right);


/* Basic I/O support */

Datum
enum_in(PG_FUNCTION_ARGS)
{
    char *name = PG_GETARG_CSTRING(0);
    Oid enumtypoid = PG_GETARG_OID(1);

    PG_RETURN_OID(cstring_enum(name, enumtypoid));
}

/* guts of enum_in and text-to-enum */
static Oid
cstring_enum(char *name, Oid enumtypoid)
{
	HeapTuple tup;
	Oid enumoid;

	/* must check length to prevent Assert failure within SearchSysCache */

	if (strlen(name) >= NAMEDATALEN)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid input value for enum %s: \"%s\"",
						format_type_be(enumtypoid),
                        name)));

	tup = SearchSysCache(ENUMTYPOIDNAME,
						 ObjectIdGetDatum(enumtypoid),
						 CStringGetDatum(name),
						 0, 0);
    if (tup == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid input value for enum %s: \"%s\"",
						format_type_be(enumtypoid),
                        name)));

	enumoid = HeapTupleGetOid(tup);

	ReleaseSysCache(tup);
	return enumoid;
}

Datum
enum_out(PG_FUNCTION_ARGS)
{
    Oid enumoid = PG_GETARG_OID(0);

    PG_RETURN_CSTRING(enum_cstring(enumoid));
}

/* guts of enum_out and enum-to-text */
static char *
enum_cstring(Oid enumval)
{
	HeapTuple tup;
	Form_pg_enum en;
	char *label;

	tup = SearchSysCache(ENUMOID,
						 ObjectIdGetDatum(enumval),
						 0, 0, 0);
    if (tup == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("invalid internal value for enum: %u",
                        enumval)));
	en = (Form_pg_enum) GETSTRUCT(tup);

	label = pstrdup(NameStr(en->enumlabel));

	ReleaseSysCache(tup);
	return label;
}

/* Comparison functions and related */

Datum
enum_lt(PG_FUNCTION_ARGS)
{
	Oid a = PG_GETARG_OID(0);
	Oid b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(a < b);
}

Datum
enum_le(PG_FUNCTION_ARGS)
{
	Oid a = PG_GETARG_OID(0);
	Oid b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(a <= b);
}

Datum
enum_eq(PG_FUNCTION_ARGS)
{
	Oid a = PG_GETARG_OID(0);
	Oid b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(a == b);
}

Datum
enum_ne(PG_FUNCTION_ARGS)
{
	Oid a = PG_GETARG_OID(0);
	Oid b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(a != b);
}

Datum
enum_ge(PG_FUNCTION_ARGS)
{
	Oid a = PG_GETARG_OID(0);
	Oid b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(a >= b);
}

Datum
enum_gt(PG_FUNCTION_ARGS)
{
	Oid a = PG_GETARG_OID(0);
	Oid b = PG_GETARG_OID(1);

	PG_RETURN_BOOL(a > b);
}

Datum
enum_smaller(PG_FUNCTION_ARGS)
{
	Oid a = PG_GETARG_OID(0);
	Oid b = PG_GETARG_OID(1);

	PG_RETURN_OID(a <= b ? a : b);
}

Datum
enum_larger(PG_FUNCTION_ARGS)
{
	Oid a = PG_GETARG_OID(0);
	Oid b = PG_GETARG_OID(1);

	PG_RETURN_OID(a >= b ? a : b);
}

Datum
enum_cmp(PG_FUNCTION_ARGS)
{
	Oid a = PG_GETARG_OID(0);
	Oid b = PG_GETARG_OID(1);

	if (a > b)
		PG_RETURN_INT32(1);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(-1);
}

/* Casts between text and enum */

Datum
enum_text(PG_FUNCTION_ARGS)
{
	Oid enumval = PG_GETARG_OID(0);
	text *result;
	char *cstr;
	int len;

	cstr = enum_cstring(enumval);
	len = strlen(cstr);
	result = (text *) palloc(VARHDRSZ + len);
	SET_VARSIZE(result, VARHDRSZ + len);
	memcpy(VARDATA(result), cstr, len);
	pfree(cstr);
	PG_RETURN_TEXT_P(result);
}

Datum
text_enum(PG_FUNCTION_ARGS)
{
	text *textval = PG_GETARG_TEXT_P(0);
	Oid enumtypoid;
	char *str;

	/*
	 * We rely on being able to get the specific enum type from the calling
	 * expression tree.
	 */
	enumtypoid = get_fn_expr_rettype(fcinfo->flinfo);
	if (enumtypoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine actual enum type")));

	str = DatumGetCString(DirectFunctionCall1(textout,
											  PointerGetDatum(textval)));
    PG_RETURN_OID(cstring_enum(str, enumtypoid));
}

/* Enum programming support functions */

Datum
enum_first(PG_FUNCTION_ARGS)
{
	Oid enumtypoid;
	Oid min = InvalidOid;
	CatCList *list;
	int num, i;

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

	list = SearchSysCacheList(ENUMTYPOIDNAME, 1,
							  ObjectIdGetDatum(enumtypoid),
							  0, 0, 0);
	num = list->n_members;
	for (i = 0; i < num; i++)
	{
		Oid valoid = HeapTupleHeaderGetOid(list->members[i]->tuple.t_data);
		if (!OidIsValid(min) || valoid < min)
			min = valoid;
	}

	ReleaseCatCacheList(list);

    if (!OidIsValid(min))		/* should not happen */
		elog(ERROR, "no values found for enum %s",
			 format_type_be(enumtypoid));

	PG_RETURN_OID(min);
}

Datum
enum_last(PG_FUNCTION_ARGS)
{
    Oid enumtypoid;
    Oid max = InvalidOid;
    CatCList *list;
    int num, i;

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

    list = SearchSysCacheList(ENUMTYPOIDNAME, 1,
                              ObjectIdGetDatum(enumtypoid),
							  0, 0, 0);
    num = list->n_members;
    for (i = 0; i < num; i++)
    {
		Oid valoid = HeapTupleHeaderGetOid(list->members[i]->tuple.t_data);
        if(!OidIsValid(max) || valoid > max)
            max = valoid;
    }

	ReleaseCatCacheList(list);

    if (!OidIsValid(max))		/* should not happen */
		elog(ERROR, "no values found for enum %s",
			 format_type_be(enumtypoid));

    PG_RETURN_OID(max);
}

/* 2-argument variant of enum_range */
Datum
enum_range_bounds(PG_FUNCTION_ARGS)
{
	Oid lower;
	Oid upper;
	Oid enumtypoid;

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
	Oid enumtypoid;

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
	ArrayType *result;
    CatCList *list;
    int total, i, j;
    Datum *elems;

	list = SearchSysCacheList(ENUMTYPOIDNAME, 1,
                              ObjectIdGetDatum(enumtypoid),
							  0, 0, 0);
	total = list->n_members;

	elems = (Datum *) palloc(total * sizeof(Datum));

	j = 0;
    for (i = 0; i < total; i++)
    {
		Oid val = HeapTupleGetOid(&(list->members[i]->tuple));

		if ((!OidIsValid(lower) || lower <= val) &&
			(!OidIsValid(upper) || val <= upper))
            elems[j++] = ObjectIdGetDatum(val);
    }

	/* shouldn't need the cache anymore */
	ReleaseCatCacheList(list);

	/* sort results into OID order */
	qsort(elems, j, sizeof(Datum), enum_elem_cmp);

	/* note this hardwires some details about the representation of Oid */
	result = construct_array(elems, j, enumtypoid, sizeof(Oid), true, 'i');

	pfree(elems);

	return result;
}

/* qsort comparison function for Datums that are OIDs */
static int
enum_elem_cmp(const void *left, const void *right)
{
	Oid l = DatumGetObjectId(*((const Datum *) left));
	Oid r = DatumGetObjectId(*((const Datum *) right));

	if (l < r)
		return -1;
	if (l > r)
		return 1;
	return 0;
}
