/*-------------------------------------------------------------------------
 *
 * array.h
 *	  Declarations for Postgres arrays.
 *
 * A standard varlena array has the following internal structure:
 *	  <vl_len_>		- standard varlena header word
 *	  <ndim>		- number of dimensions of the array
 *	  <dataoffset>	- offset to stored data, or 0 if no nulls bitmap
 *	  <elemtype>	- element type OID
 *	  <dimensions>	- length of each array axis (C array of int)
 *	  <lower bnds>	- lower boundary of each dimension (C array of int)
 *	  <null bitmap> - bitmap showing locations of nulls (OPTIONAL)
 *	  <actual data> - whatever is the stored data
 *
 * The <dimensions> and <lower bnds> arrays each have ndim elements.
 *
 * The <null bitmap> may be omitted if the array contains no NULL elements.
 * If it is absent, the <dataoffset> field is zero and the offset to the
 * stored data must be computed on-the-fly.  If the bitmap is present,
 * <dataoffset> is nonzero and is equal to the offset from the array start
 * to the first data element (including any alignment padding).  The bitmap
 * follows the same conventions as tuple null bitmaps, ie, a 1 indicates
 * a non-null entry and the LSB of each bitmap byte is used first.
 *
 * The actual data starts on a MAXALIGN boundary.  Individual items in the
 * array are aligned as specified by the array element type.  They are
 * stored in row-major order (last subscript varies most rapidly).
 *
 * NOTE: it is important that array elements of toastable datatypes NOT be
 * toasted, since the tupletoaster won't know they are there.  (We could
 * support compressed toasted items; only out-of-line items are dangerous.
 * However, it seems preferable to store such items uncompressed and allow
 * the toaster to compress the whole array as one input.)
 *
 *
 * The OIDVECTOR and INT2VECTOR datatypes are storage-compatible with
 * generic arrays, but they support only one-dimensional arrays with no
 * nulls (and no null bitmap).
 *
 * There are also some "fixed-length array" datatypes, such as NAME and
 * POINT.  These are simply a sequence of a fixed number of items each
 * of a fixed-length datatype, with no overhead; the item size must be
 * a multiple of its alignment requirement, because we do no padding.
 * We support subscripting on these types, but array_in() and array_out()
 * only work with varlena arrays.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/array.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ARRAY_H
#define ARRAY_H

#include "fmgr.h"

/*
 * Arrays are varlena objects, so must meet the varlena convention that
 * the first int32 of the object contains the total object size in bytes.
 * Be sure to use VARSIZE() and SET_VARSIZE() to access it, though!
 *
 * CAUTION: if you change the header for ordinary arrays you will also
 * need to change the headers for oidvector and int2vector!
 */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			ndim;			/* # of dimensions */
	int32		dataoffset;		/* offset to data, or 0 if no bitmap */
	Oid			elemtype;		/* element type OID */
} ArrayType;

/*
 * working state for accumArrayResult() and friends
 */
typedef struct ArrayBuildState
{
	MemoryContext mcontext;		/* where all the temp stuff is kept */
	Datum	   *dvalues;		/* array of accumulated Datums */
	bool	   *dnulls;			/* array of is-null flags for Datums */
	int			alen;			/* allocated length of above arrays */
	int			nelems;			/* number of valid entries in above arrays */
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
	Oid			typioparam;
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

/* ArrayIteratorData is private in arrayfuncs.c */
typedef struct ArrayIteratorData *ArrayIterator;

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
 * That is: if the third axis of an array has elements 5 through 8, then
 * ARR_DIMS(a)[2] == 4 and ARR_LBOUND(a)[2] == 5.
 *
 * Unlike C, the default lower bound is 1.
 */
#define ARR_SIZE(a)				VARSIZE(a)
#define ARR_NDIM(a)				((a)->ndim)
#define ARR_HASNULL(a)			((a)->dataoffset != 0)
#define ARR_ELEMTYPE(a)			((a)->elemtype)

#define ARR_DIMS(a) \
		((int *) (((char *) (a)) + sizeof(ArrayType)))
#define ARR_LBOUND(a) \
		((int *) (((char *) (a)) + sizeof(ArrayType) + \
				  sizeof(int) * ARR_NDIM(a)))

#define ARR_NULLBITMAP(a) \
		(ARR_HASNULL(a) ? \
		 (bits8 *) (((char *) (a)) + sizeof(ArrayType) + \
					2 * sizeof(int) * ARR_NDIM(a)) \
		 : (bits8 *) NULL)

/*
 * The total array header size (in bytes) for an array with the specified
 * number of dimensions and total number of items.
 */
#define ARR_OVERHEAD_NONULLS(ndims) \
		MAXALIGN(sizeof(ArrayType) + 2 * sizeof(int) * (ndims))
#define ARR_OVERHEAD_WITHNULLS(ndims, nitems) \
		MAXALIGN(sizeof(ArrayType) + 2 * sizeof(int) * (ndims) + \
				 ((nitems) + 7) / 8)

#define ARR_DATA_OFFSET(a) \
		(ARR_HASNULL(a) ? (a)->dataoffset : ARR_OVERHEAD_NONULLS(ARR_NDIM(a)))

/*
 * Returns a pointer to the actual array data.
 */
#define ARR_DATA_PTR(a) \
		(((char *) (a)) + ARR_DATA_OFFSET(a))


/*
 * GUC parameter
 */
extern bool Array_nulls;

/*
 * prototypes for functions defined in arrayfuncs.c
 */
extern Datum array_in(PG_FUNCTION_ARGS);
extern Datum array_out(PG_FUNCTION_ARGS);
extern Datum array_recv(PG_FUNCTION_ARGS);
extern Datum array_send(PG_FUNCTION_ARGS);
extern Datum array_eq(PG_FUNCTION_ARGS);
extern Datum array_ne(PG_FUNCTION_ARGS);
extern Datum array_lt(PG_FUNCTION_ARGS);
extern Datum array_gt(PG_FUNCTION_ARGS);
extern Datum array_le(PG_FUNCTION_ARGS);
extern Datum array_ge(PG_FUNCTION_ARGS);
extern Datum btarraycmp(PG_FUNCTION_ARGS);
extern Datum hash_array(PG_FUNCTION_ARGS);
extern Datum arrayoverlap(PG_FUNCTION_ARGS);
extern Datum arraycontains(PG_FUNCTION_ARGS);
extern Datum arraycontained(PG_FUNCTION_ARGS);
extern Datum array_ndims(PG_FUNCTION_ARGS);
extern Datum array_dims(PG_FUNCTION_ARGS);
extern Datum array_lower(PG_FUNCTION_ARGS);
extern Datum array_upper(PG_FUNCTION_ARGS);
extern Datum array_length(PG_FUNCTION_ARGS);
extern Datum array_cardinality(PG_FUNCTION_ARGS);
extern Datum array_larger(PG_FUNCTION_ARGS);
extern Datum array_smaller(PG_FUNCTION_ARGS);
extern Datum generate_subscripts(PG_FUNCTION_ARGS);
extern Datum generate_subscripts_nodir(PG_FUNCTION_ARGS);
extern Datum array_fill(PG_FUNCTION_ARGS);
extern Datum array_fill_with_lower_bounds(PG_FUNCTION_ARGS);
extern Datum array_unnest(PG_FUNCTION_ARGS);
extern Datum array_remove(PG_FUNCTION_ARGS);
extern Datum array_replace(PG_FUNCTION_ARGS);

extern Datum array_ref(ArrayType *array, int nSubscripts, int *indx,
		  int arraytyplen, int elmlen, bool elmbyval, char elmalign,
		  bool *isNull);
extern ArrayType *array_set(ArrayType *array, int nSubscripts, int *indx,
		  Datum dataValue, bool isNull,
		  int arraytyplen, int elmlen, bool elmbyval, char elmalign);
extern ArrayType *array_get_slice(ArrayType *array, int nSubscripts,
				int *upperIndx, int *lowerIndx,
				int arraytyplen, int elmlen, bool elmbyval, char elmalign);
extern ArrayType *array_set_slice(ArrayType *array, int nSubscripts,
				int *upperIndx, int *lowerIndx,
				ArrayType *srcArray, bool isNull,
				int arraytyplen, int elmlen, bool elmbyval, char elmalign);

extern Datum array_map(FunctionCallInfo fcinfo, Oid inpType, Oid retType,
		  ArrayMapState *amstate);

extern void array_bitmap_copy(bits8 *destbitmap, int destoffset,
				  const bits8 *srcbitmap, int srcoffset,
				  int nitems);

extern ArrayType *construct_array(Datum *elems, int nelems,
				Oid elmtype,
				int elmlen, bool elmbyval, char elmalign);
extern ArrayType *construct_md_array(Datum *elems,
				   bool *nulls,
				   int ndims,
				   int *dims,
				   int *lbs,
				   Oid elmtype, int elmlen, bool elmbyval, char elmalign);
extern ArrayType *construct_empty_array(Oid elmtype);
extern void deconstruct_array(ArrayType *array,
				  Oid elmtype,
				  int elmlen, bool elmbyval, char elmalign,
				  Datum **elemsp, bool **nullsp, int *nelemsp);
extern bool array_contains_nulls(ArrayType *array);
extern ArrayBuildState *accumArrayResult(ArrayBuildState *astate,
				 Datum dvalue, bool disnull,
				 Oid element_type,
				 MemoryContext rcontext);
extern Datum makeArrayResult(ArrayBuildState *astate,
				MemoryContext rcontext);
extern Datum makeMdArrayResult(ArrayBuildState *astate, int ndims,
				  int *dims, int *lbs, MemoryContext rcontext, bool release);

extern ArrayIterator array_create_iterator(ArrayType *arr, int slice_ndim);
extern bool array_iterate(ArrayIterator iterator, Datum *value, bool *isnull);
extern void array_free_iterator(ArrayIterator iterator);

/*
 * prototypes for functions defined in arrayutils.c
 */

extern int	ArrayGetOffset(int n, const int *dim, const int *lb, const int *indx);
extern int	ArrayGetOffset0(int n, const int *tup, const int *scale);
extern int	ArrayGetNItems(int ndim, const int *dims);
extern void mda_get_range(int n, int *span, const int *st, const int *endp);
extern void mda_get_prod(int n, const int *range, int *prod);
extern void mda_get_offset_values(int n, int *dist, const int *prod, const int *span);
extern int	mda_next_tuple(int n, int *curr, const int *span);
extern int32 *ArrayGetIntegerTypmods(ArrayType *arr, int *n);

/*
 * prototypes for functions defined in array_userfuncs.c
 */
extern Datum array_push(PG_FUNCTION_ARGS);
extern Datum array_cat(PG_FUNCTION_ARGS);

extern ArrayType *create_singleton_array(FunctionCallInfo fcinfo,
					   Oid element_type,
					   Datum element,
					   bool isNull,
					   int ndims);

extern Datum array_agg_transfn(PG_FUNCTION_ARGS);
extern Datum array_agg_finalfn(PG_FUNCTION_ARGS);

/*
 * prototypes for functions defined in array_typanalyze.c
 */
extern Datum array_typanalyze(PG_FUNCTION_ARGS);

#endif   /* ARRAY_H */
