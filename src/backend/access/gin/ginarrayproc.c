/*-------------------------------------------------------------------------
 *
 * ginvacuum.c
 *    support function for GIN's indexing of any array
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *          $PostgreSQL: pgsql/src/backend/access/gin/ginarrayproc.c,v 1.1 2006/05/02 11:28:54 teodor Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/builtins.h"
#include "access/gin.h"

#define GinOverlapStrategy		1
#define GinContainsStrategy		2
#define GinContainedStrategy	3

#define	ARRAYCHECK(x) do { 									\
	if ( ARR_HASNULL(x) )									\
		ereport(ERROR, 										\
			(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), 		\
			 errmsg("array must not contain nulls"))); 		\
															\
	if ( ARR_NDIM(x) != 1 && ARR_NDIM(x) != 0 ) 			\
		ereport(ERROR, 										\
			(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR), 		\
			 errmsg("array must be one-dimensional"))); 	\
} while(0) 

/*
 * Function used as extractValue and extractQuery both
 */
Datum
ginarrayextract(PG_FUNCTION_ARGS) {
	ArrayType	*array;
	uint32	*nentries = (uint32*)PG_GETARG_POINTER(1); 
	Datum	*entries = NULL;
	int16 	elmlen;
	bool	elmbyval;
	char	elmalign;

	/* we should guarantee that array will not be destroyed during all operation */
	array = PG_GETARG_ARRAYTYPE_P_COPY(0);

	ARRAYCHECK(array);

	get_typlenbyvalalign(ARR_ELEMTYPE(array),
		 &elmlen, &elmbyval, &elmalign);

	deconstruct_array(array,
		ARR_ELEMTYPE(array),
		elmlen, elmbyval, elmalign,
		&entries, NULL, (int*)nentries);

	/* we should not free array, entries[i] points into it */
	PG_RETURN_POINTER(entries);
}

Datum
ginarrayconsistent(PG_FUNCTION_ARGS) {
	bool	*check = (bool*)PG_GETARG_POINTER(0);
	StrategyNumber 	strategy = PG_GETARG_UINT16(1);
	ArrayType   *query = PG_GETARG_ARRAYTYPE_P(2);
	int res=FALSE, i, nentries=ArrayGetNItems(ARR_NDIM(query), ARR_DIMS(query));

	/* we can do not check array carefully, it's done by previous ginarrayextract call */

	switch( strategy ) {
		case GinOverlapStrategy:
		case GinContainedStrategy:
			/* at least one element in check[] is true, so result = true */ 
			res = TRUE;
			break;
		case GinContainsStrategy:
			res = TRUE;
			for(i=0;i<nentries;i++)
				if ( !check[i] ) {
					res = FALSE;
					break;
				}
			break;
		default:
			elog(ERROR, "ginarrayconsistent: unknown strategy number: %d", strategy);
	}

	PG_RETURN_BOOL(res);
}

static TypeCacheEntry*
fillTypeCacheEntry( TypeCacheEntry *typentry, Oid element_type ) {
	if ( typentry && typentry->type_id == element_type )
		return typentry;

	typentry = lookup_type_cache(element_type,	TYPECACHE_EQ_OPR_FINFO);
	if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				errmsg("could not identify an equality operator for type %s", format_type_be(element_type))));

	return typentry;
}

static bool
typeEQ(FunctionCallInfoData *locfcinfo, Datum a, Datum b) {
	locfcinfo->arg[0] = a;
	locfcinfo->arg[1] = b;
	locfcinfo->argnull[0] = false;
	locfcinfo->argnull[1] = false;
	locfcinfo->isnull = false;

	return DatumGetBool(FunctionCallInvoke(locfcinfo));
}

static bool
ginArrayOverlap(TypeCacheEntry *typentry, ArrayType *a, ArrayType *b) {
	Datum 	*da, *db;
	int		na, nb, j, i;
	FunctionCallInfoData locfcinfo;
	
	if ( ARR_ELEMTYPE(a) != ARR_ELEMTYPE(b) )
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			errmsg("cannot compare arrays of different element types")));

	ARRAYCHECK(a);
	ARRAYCHECK(b);

	deconstruct_array(a,
		ARR_ELEMTYPE(a),
		typentry->typlen, typentry->typbyval, typentry->typalign,
		&da, NULL, &na);
	deconstruct_array(b,
		ARR_ELEMTYPE(b),
		typentry->typlen, typentry->typbyval, typentry->typalign,
		&db, NULL, &nb);

	InitFunctionCallInfoData(locfcinfo, &typentry->eq_opr_finfo, 2,
		NULL, NULL);

	for(i=0;i<na;i++) {
		for(j=0;j<nb;j++) {
			if ( typeEQ(&locfcinfo, da[i], db[j]) ) {
					pfree( da );
					pfree( db );
					return TRUE;
			}
		}
	}

	pfree( da );
	pfree( db );

	return FALSE;
}

static bool
ginArrayContains(TypeCacheEntry *typentry, ArrayType *a, ArrayType *b) {
	Datum 	*da, *db;
	int		na, nb, j, i, n = 0;
	FunctionCallInfoData locfcinfo;
	
	if ( ARR_ELEMTYPE(a) != ARR_ELEMTYPE(b) )
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			errmsg("cannot compare arrays of different element types")));

	ARRAYCHECK(a);
	ARRAYCHECK(b);

	deconstruct_array(a,
		ARR_ELEMTYPE(a),
		typentry->typlen, typentry->typbyval, typentry->typalign,
		&da, NULL, &na);
	deconstruct_array(b,
		ARR_ELEMTYPE(b),
		typentry->typlen, typentry->typbyval, typentry->typalign,
		&db, NULL, &nb);

	InitFunctionCallInfoData(locfcinfo, &typentry->eq_opr_finfo, 2,
		NULL, NULL);

	for(i=0;i<nb;i++) {
		for(j=0;j<na;j++) {
			if ( typeEQ(&locfcinfo, db[i], da[j]) ) {
				n++;
				break;
			}
		}
	}

	pfree( da );
	pfree( db );

	return ( n==nb ) ? TRUE : FALSE;
}

Datum
arrayoverlap(PG_FUNCTION_ARGS) {
	ArrayType   *a = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType   *b = PG_GETARG_ARRAYTYPE_P(1);
	TypeCacheEntry *typentry = fillTypeCacheEntry( fcinfo->flinfo->fn_extra, ARR_ELEMTYPE(a) ); 
	bool res;

	fcinfo->flinfo->fn_extra = (void*)typentry;

	res = ginArrayOverlap( typentry, a, b ); 

	PG_FREE_IF_COPY(a,0);
	PG_FREE_IF_COPY(b,1);

	PG_RETURN_BOOL(res);
}

Datum
arraycontains(PG_FUNCTION_ARGS) {
	ArrayType   *a = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType   *b = PG_GETARG_ARRAYTYPE_P(1);
	TypeCacheEntry *typentry = fillTypeCacheEntry( fcinfo->flinfo->fn_extra, ARR_ELEMTYPE(a) ); 
	bool res;

	fcinfo->flinfo->fn_extra = (void*)typentry;

	res = ginArrayContains( typentry, a, b ); 

	PG_FREE_IF_COPY(a,0);
	PG_FREE_IF_COPY(b,1);

	PG_RETURN_BOOL(res);
}

Datum
arraycontained(PG_FUNCTION_ARGS) {
	ArrayType   *a = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType   *b = PG_GETARG_ARRAYTYPE_P(1);
	TypeCacheEntry *typentry = fillTypeCacheEntry( fcinfo->flinfo->fn_extra, ARR_ELEMTYPE(a) ); 
	bool res;

	fcinfo->flinfo->fn_extra = (void*)typentry;

	res = ginArrayContains( typentry, b, a ); 

	PG_FREE_IF_COPY(a,0);
	PG_FREE_IF_COPY(b,1);

	PG_RETURN_BOOL(res);
}

