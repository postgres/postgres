/*-------------------------------------------------------------------------
 *
 * array.h
 *	  Utilities for the new array code. Contains prototypes from the
 *	  following files:
 *				utils/adt/arrayfuncs.c
 *				utils/adt/arrayutils.c
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: array.h,v 1.29 2001/03/22 04:01:10 momjian Exp $
 *
 * NOTES
 *	  XXX the data array should be MAXALIGN'd -- currently we only INTALIGN
 *	  which is NOT good enough for, eg, arrays of Interval.  Changing this
 *	  will break existing user tables so hold off until we have some other
 *	  reason to break user tables (like WAL).
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
} ArrayType;

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
 * ARR_DIMS(a)[2] == 6 and ARR_LBOUND[2] == 5.
 *
 * Unlike C, the default lower bound is 1.
 */
#define ARR_SIZE(a)				(((ArrayType *) (a))->size)
#define ARR_NDIM(a)				(((ArrayType *) (a))->ndim)

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
extern Datum array_eq(PG_FUNCTION_ARGS);
extern Datum array_dims(PG_FUNCTION_ARGS);

extern Datum array_ref(ArrayType *array, int nSubscripts, int *indx,
		  bool elmbyval, int elmlen,
		  int arraylen, bool *isNull);
extern ArrayType *array_set(ArrayType *array, int nSubscripts, int *indx,
		  Datum dataValue,
		  bool elmbyval, int elmlen,
		  int arraylen, bool *isNull);
extern ArrayType *array_get_slice(ArrayType *array, int nSubscripts,
				int *upperIndx, int *lowerIndx,
				bool elmbyval, int elmlen,
				int arraylen, bool *isNull);
extern ArrayType *array_set_slice(ArrayType *array, int nSubscripts,
				int *upperIndx, int *lowerIndx,
				ArrayType *srcArray,
				bool elmbyval, int elmlen,
				int arraylen, bool *isNull);

extern Datum array_map(FunctionCallInfo fcinfo, Oid inpType, Oid retType);

extern ArrayType *construct_array(Datum *elems, int nelems,
				bool elmbyval, int elmlen, char elmalign);
extern void deconstruct_array(ArrayType *array,
				  bool elmbyval, int elmlen, char elmalign,
				  Datum **elemsp, int *nelemsp);


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


#endif	 /* ARRAY_H */
