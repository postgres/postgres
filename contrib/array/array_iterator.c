/*
 * array_iterator.c --
 *
 * This file defines a new group of operators which take an
 * array and a scalar value, iterate a scalar operator over the
 * elements of the array and the value and compute a result as
 * the logical OR or AND of the results.
 * For example array_int4eq returns true if some of the elements
 * of an array of int4 is equal to the given value:
 *
 *	array_int4eq({1,2,3}, 1)  -->  true
 *	array_int4eq({1,2,3}, 4)  -->  false
 *
 * If we have defined T array types and O scalar operators
 * we can define T x O array operators, each of them has a name
 * like "array_<basetype><operation>" and takes an array of type T
 * iterating the operator O over all the elements. Note however
 * that some of the possible combination are invalid, for example
 * the array_int4_like because there is no like operator for int4.
 * It is now possible to write queries which look inside the arrays:
 *
 *      create table t(id int4[], txt text[]);
 *	select * from t where t.id *= 123;
 *	select * from t where t.txt *~ '[a-z]';
 *	select * from t where t.txt[1:3] **~ '[a-z]';
 *
 * Copyright (c) 1996, Massimo Dal Zotto <dz@cs.unitn.it>
 */

#include <ctype.h>
#include <stdio.h>
#include <sys/types.h>
#include <string.h>

#include "postgres.h"
#include "pg_type.h"
#include "miscadmin.h"
#include "syscache.h"
#include "access/xact.h"
#include "utils/builtins.h"
#include "utils/elog.h"

static int32
array_iterator(Oid elemtype, Oid proc, int and, ArrayType *array, Datum value)
{
    HeapTuple typ_tuple;
    TypeTupleForm typ_struct;
    bool typbyval;
    int typlen;
    func_ptr proc_fn;
    int pronargs;
    int nitems, i, result;
    int ndim, *dim;
    char *p;

    /* Sanity checks */
    if ((array == (ArrayType *) NULL)
	|| (ARR_IS_LO(array) == true)) {
	/* elog(NOTICE, "array_iterator: array is null"); */
        return (0);
    }
    ndim = ARR_NDIM(array);
    dim = ARR_DIMS(array);
    nitems = getNitems(ndim, dim);
    if (nitems == 0) {
	/* elog(NOTICE, "array_iterator: nitems = 0"); */
        return (0);
    }

    /* Lookup element type information */
    typ_tuple = SearchSysCacheTuple(TYPOID, ObjectIdGetDatum(elemtype),0,0,0);
    if (!HeapTupleIsValid(typ_tuple)) {
        elog(WARN,"array_iterator: cache lookup failed for type %d", elemtype);
        return 0;
    }
    typ_struct = (TypeTupleForm) GETSTRUCT(typ_tuple);
    typlen   = typ_struct->typlen;
    typbyval = typ_struct->typbyval;

    /* Lookup the function entry point */
    proc_fn == (func_ptr) NULL;
    fmgr_info(proc, &proc_fn, &pronargs);
    if ((proc_fn == NULL) || (pronargs != 2)) {
	elog(WARN, "array_iterator: fmgr_info lookup failed for oid %d", proc);
        return (0);
    }

    /* Scan the array and apply the operator to each element */
    result = 0;
    p = ARR_DATA_PTR(array);
    for (i = 0; i < nitems; i++) {
        if (typbyval) {
            switch(typlen) {
	      case 1:
		result = (int) (*proc_fn)(*p, value);
		break;
	    case 2:
		result = (int) (*proc_fn)(* (int16 *) p, value);
		break;
	    case 3:
	    case 4:
		result = (int) (*proc_fn)(* (int32 *) p, value);
		break;
            }
            p += typlen;
        } else {
	    result = (int) (*proc_fn)(p, value);
            if (typlen > 0) {
		p += typlen;
	    } else {
                p += INTALIGN(* (int32 *) p);
	    }
        }
	if (result) {
	    if (!and) {
		return (1);
	    }
	} else {
	    if (and) {
		return (0);
	    }
	}
    }

    if (and && result) {
	return (1);
    } else {
	return (0);
    }
}

/*
 * Iterators for type _text
 */

int32
array_texteq(ArrayType *array, char* value)
{
    return array_iterator((Oid) 25,	/* text */
			  (Oid) 67,	/* texteq */
			  0,		/* logical or */
			  array, (Datum)value);
}

int32
array_all_texteq(ArrayType *array, char* value)
{
    return array_iterator((Oid) 25,	/* text */
			  (Oid) 67,	/* texteq */
			  1,		/* logical and */
			  array, (Datum)value);
}

int32
array_textregexeq(ArrayType *array, char* value)
{
    return array_iterator((Oid) 25,	/* text */
			  (Oid) 81,	/* textregexeq */
			  0,		/* logical or */
			  array, (Datum)value);
}

int32
array_all_textregexeq(ArrayType *array, char* value)
{
    return array_iterator((Oid) 25,	/* text */
			  (Oid) 81,	/* textregexeq */
			  1,		/* logical and */
			  array, (Datum)value);
}

/*
 * Iterators for type _char16. Note that the regexp operators
 * take the second argument of type text.
 */

int32
array_char16eq(ArrayType *array, char* value)
{
    return array_iterator((Oid) 20,	/* char16 */
			  (Oid) 490,	/* char16eq */
			  0,		/* logical or */
			  array, (Datum)value);
}

int32
array_all_char16eq(ArrayType *array, char* value)
{
    return array_iterator((Oid) 20,	/* char16 */
			  (Oid) 490,	/* char16eq */
			  1,		/* logical and */
			  array, (Datum)value);
}

int32
array_char16regexeq(ArrayType *array, char* value)
{
    return array_iterator((Oid) 20,	/* char16 */
			  (Oid) 700,	/* char16regexeq */
			  0,		/* logical or */
			  array, (Datum)value);
}

int32
array_all_char16regexeq(ArrayType *array, char* value)
{
    return array_iterator((Oid) 20,	/* char16 */
			  (Oid) 700,	/* char16regexeq */
			  1,		/* logical and */
			  array, (Datum)value);
}

/*
 * Iterators for type _int4
 */

int32
array_int4eq(ArrayType *array, int4 value)
{
    return array_iterator((Oid) 23,	/* int4 */
			  (Oid) 65,	/* int4eq */
			  0,		/* logical or */
			  array, (Datum)value);
}

int32
array_all_int4eq(ArrayType *array, int4 value)
{
    return array_iterator((Oid) 23,	/* int4 */
			  (Oid) 65,	/* int4eq */
			  1,		/* logical and */
			  array, (Datum)value);
}

int32
array_int4gt(ArrayType *array, int4 value)
{
    return array_iterator((Oid) 23,	/* int4 */
			  (Oid) 147,	/* int4gt */
			  0,		/* logical or */
			  array, (Datum)value);
}

int32
array_all_int4gt(ArrayType *array, int4 value)
{
    return array_iterator((Oid) 23,	/* int4 */
			  (Oid) 147,	/* int4gt */
			  1,		/* logical and */
			  array, (Datum)value);
}
