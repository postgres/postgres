/*-------------------------------------------------------------------------
 *
 * array.h
 *	  Utilities for the new array code. Contains prototypes from the
 *	  following files:
 *				utils/adt/arrayfuncs.c
 *				utils/adt/arrayutils.c
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: array.h,v 1.45.4.1 2005/03/24 21:51:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ARRAY_H
#define ARRAY_H

#include "fmgr.h"

/*
 * Arrays are varlena objects, so must meet the varlena convention that
 * the first int32 of the object contains the total object size in bytes.
 */
typedef struct
{
	int32		size;			/* total array size (varlena requirement) */
	int			ndim;			/* # of dimensions */
	int			flags;			/* implementation flags */
	/* flags field is currently unused, always zero. */
	Oid			elemtype;		/* element type OID */
} ArrayType;

typedef struct ArrayBuildState
{
	MemoryContext mcontext;		/* where all the temp stuff is kept */
	Datum	   *dvalues;		/* array of accumulated Datums */

	/*
	 * The allocated size of dvalues[] is always a multiple of
	 * ARRAY_ELEMS_CHUNKSIZE
	 */
#define ARRAY_ELEMS_CHUNKSIZE	64
	int			nelems;			/* number of valid Datums in dvalues[] */
	Oid			element_type;	/* data type of the Datums */
	int16		typlen;			/* needed info about datatype */
	bool		typbyval;
	char		typalign;
} ArrayBuildState;

/*
 * structure to cache type metadata needed for array manipulation
 */
typedef struct ArrayMetaState
{
	Oid			element_type;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char		typdelim;
	Oid			typelem;
	Oid			typiofunc;
	FmgrInfo	proc;
} ArrayMetaState;

/*
 * private state needed by array_map (here because caller must provide it)
 */
typedef struct ArrayMapState
{
	ArrayMetaState inp_extra;
	ArrayMetaState ret_extra;
} ArrayMapState;

/*
 * fmgr macros for array objects
 */
#define DatumGetArrayTypeP(X)		  ((ArrayType *) PG_DETOAST_DATUM(X))
#define DatumGetArrayTypePCopy(X)	  ((ArrayType *) PG_DETOAST_DATUM_COPY(X))
#define PG_GETARG_ARRAYTYPE_P(n)	  DatumGetArrayTypeP(PG_GETARG_DATUM(n))
#define PG_GETARG_ARRAYTYPE_P_COPY(n) DatumGetArrayTypePCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_ARRAYTYPE_P(x)	  PG_RETURN_POINTER(x)

/*
 * Access macros for array header fields.
 *
 * ARR_DIMS returns a pointer to an array of array dimensions (number of
 * elements along the various array axes).
 *
 * ARR_LBOUND returns a pointer to an array of array lower bounds.
 *
 * That is: if the third axis of an array has elements 5 through 10, then
 * ARR_DIMS(a)[2] == 6 and ARR_LBOUND(a)[2] == 5.
 *
 * Unlike C, the default lower bound is 1.
 */
#define ARR_SIZE(a)				(((ArrayType *) (a))->size)
#define ARR_NDIM(a)				(((ArrayType *) (a))->ndim)
#define ARR_ELEMTYPE(a)			(((ArrayType *) (a))->elemtype)

#define ARR_DIMS(a) \
		((int *) (((char *) (a)) + sizeof(ArrayType)))
#define ARR_LBOUND(a) \
		((int *) (((char *) (a)) + sizeof(ArrayType) + \
				  (sizeof(int) * ARR_NDIM(a))))

/*
 * The total array header size for an array of dimension n (in bytes).
 */
#define ARR_OVERHEAD(n) \
		(MAXALIGN(sizeof(ArrayType) + 2 * sizeof(int) * (n)))

/*
 * Returns a pointer to the actual array data.
 */
#define ARR_DATA_PTR(a) \
		(((char *) (a)) + ARR_OVERHEAD(ARR_NDIM(a)))


/*
 * prototypes for functions defined in arrayfuncs.c
 */
extern Datum array_in(PG_FUNCTION_ARGS);
extern Datum array_out(PG_FUNCTION_ARGS);
extern Datum array_recv(PG_FUNCTION_ARGS);
extern Datum array_send(PG_FUNCTION_ARGS);
extern Datum array_length_coerce(PG_FUNCTION_ARGS);
extern Datum array_eq(PG_FUNCTION_ARGS);
extern Datum array_ne(PG_FUNCTION_ARGS);
extern Datum array_lt(PG_FUNCTION_ARGS);
extern Datum array_gt(PG_FUNCTION_ARGS);
extern Datum array_le(PG_FUNCTION_ARGS);
extern Datum array_ge(PG_FUNCTION_ARGS);
extern Datum btarraycmp(PG_FUNCTION_ARGS);
extern Datum array_dims(PG_FUNCTION_ARGS);
extern Datum array_lower(PG_FUNCTION_ARGS);
extern Datum array_upper(PG_FUNCTION_ARGS);
extern Datum array_type_coerce(PG_FUNCTION_ARGS);

extern Datum array_ref(ArrayType *array, int nSubscripts, int *indx,
		  int arraylen, int elmlen, bool elmbyval, char elmalign,
		  bool *isNull);
extern ArrayType *array_set(ArrayType *array, int nSubscripts, int *indx,
		  Datum dataValue,
		  int arraylen, int elmlen, bool elmbyval, char elmalign,
		  bool *isNull);
extern ArrayType *array_get_slice(ArrayType *array, int nSubscripts,
				int *upperIndx, int *lowerIndx,
				int arraylen, int elmlen, bool elmbyval, char elmalign,
				bool *isNull);
extern ArrayType *array_set_slice(ArrayType *array, int nSubscripts,
				int *upperIndx, int *lowerIndx,
				ArrayType *srcArray,
				int arraylen, int elmlen, bool elmbyval, char elmalign,
				bool *isNull);

extern Datum array_map(FunctionCallInfo fcinfo, Oid inpType, Oid retType,
					   ArrayMapState *amstate);

extern ArrayType *construct_array(Datum *elems, int nelems,
				Oid elmtype,
				int elmlen, bool elmbyval, char elmalign);
extern ArrayType *construct_md_array(Datum *elems,
				   int ndims,
				   int *dims,
				   int *lbs,
				   Oid elmtype, int elmlen, bool elmbyval, char elmalign);
extern void deconstruct_array(ArrayType *array,
				  Oid elmtype,
				  int elmlen, bool elmbyval, char elmalign,
				  Datum **elemsp, int *nelemsp);
extern ArrayBuildState *accumArrayResult(ArrayBuildState *astate,
				 Datum dvalue, bool disnull,
				 Oid element_type,
				 MemoryContext rcontext);
extern Datum makeArrayResult(ArrayBuildState *astate,
				MemoryContext rcontext);
extern Datum makeMdArrayResult(ArrayBuildState *astate, int ndims,
				  int *dims, int *lbs, MemoryContext rcontext);

/*
 * prototypes for functions defined in arrayutils.c
 */

extern int	ArrayGetOffset(int n, int *dim, int *lb, int *indx);
extern int	ArrayGetOffset0(int n, int *tup, int *scale);
extern int	ArrayGetNItems(int n, int *a);
extern void mda_get_range(int n, int *span, int *st, int *endp);
extern void mda_get_prod(int n, int *range, int *prod);
extern void mda_get_offset_values(int n, int *dist, int *prod, int *span);
extern int	mda_next_tuple(int n, int *curr, int *span);

/*
 * prototypes for functions defined in array_userfuncs.c
 */
extern Datum array_push(PG_FUNCTION_ARGS);
extern Datum array_cat(PG_FUNCTION_ARGS);

extern ArrayType *create_singleton_array(FunctionCallInfo fcinfo,
					   Oid element_type,
					   Datum element,
					   int ndims);

#endif   /* ARRAY_H */
