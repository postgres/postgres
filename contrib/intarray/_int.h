/*
 * contrib/intarray/_int.h
 */
#ifndef ___INT_H__
#define ___INT_H__

#include "utils/array.h"
#include "utils/memutils.h"

/* number ranges for compression */
#define G_INT_NUMRANGES_DEFAULT		100
#define G_INT_NUMRANGES_MAX			((GISTMaxIndexKeySize - VARHDRSZ) / \
									 (2 * sizeof(int32)))
#define G_INT_GET_NUMRANGES()		(PG_HAS_OPCLASS_OPTIONS() ? \
									 ((GISTIntArrayOptions *) PG_GET_OPCLASS_OPTIONS())->num_ranges : \
									 G_INT_NUMRANGES_DEFAULT)

/* gist_int_ops opclass options */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			num_ranges;		/* number of ranges */
} GISTIntArrayOptions;

/* useful macros for accessing int4 arrays */
#define ARRPTR(x)  ( (int32 *) ARR_DATA_PTR(x) )
#define ARRNELEMS(x)  ArrayGetNItems(ARR_NDIM(x), ARR_DIMS(x))

/* reject arrays we can't handle; to wit, those containing nulls */
#define CHECKARRVALID(x) \
	do { \
		if (ARR_HASNULL(x) && array_contains_nulls(x)) \
			ereport(ERROR, \
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), \
					 errmsg("array must not contain nulls"))); \
	} while(0)

#define ARRISEMPTY(x)  (ARRNELEMS(x) == 0)

/* sort the elements of the array */
#define SORT(x) \
	do { \
		int		_nelems_ = ARRNELEMS(x); \
		if (_nelems_ > 1) \
			isort(ARRPTR(x), _nelems_); \
	} while(0)

/* sort the elements of the array and remove duplicates */
#define PREPAREARR(x) \
	do { \
		int		_nelems_ = ARRNELEMS(x); \
		if (_nelems_ > 1) \
			if (isort(ARRPTR(x), _nelems_)) \
				(x) = _int_unique(x); \
	} while(0)

/* "wish" function */
#define WISH_F(a,b,c) (double)( -(double)(((a)-(b))*((a)-(b))*((a)-(b)))*(c) )


/* bigint defines */
#define SIGLEN_DEFAULT		(63 * 4)
#define SIGLEN_MAX			GISTMaxIndexKeySize
#define SIGLENBIT(siglen)	((siglen) * BITS_PER_BYTE)
#define GET_SIGLEN()		(PG_HAS_OPCLASS_OPTIONS() ? \
							 ((GISTIntArrayBigOptions *) PG_GET_OPCLASS_OPTIONS())->siglen : \
							 SIGLEN_DEFAULT)

typedef char *BITVECP;

#define LOOPBYTE(siglen) \
			for (i = 0; i < siglen; i++)

/* beware of multiple evaluation of arguments to these macros! */
#define GETBYTE(x,i) ( *( (BITVECP)(x) + (int)( (i) / BITS_PER_BYTE ) ) )
#define GETBITBYTE(x,i) ( (*((char*)(x)) >> (i)) & 0x01 )
#define CLRBIT(x,i)   GETBYTE(x,i) &= ~( 0x01 << ( (i) % BITS_PER_BYTE ) )
#define SETBIT(x,i)   GETBYTE(x,i) |=  ( 0x01 << ( (i) % BITS_PER_BYTE ) )
#define GETBIT(x,i) ( (GETBYTE(x,i) >> ( (i) % BITS_PER_BYTE )) & 0x01 )
#define HASHVAL(val, siglen) (((unsigned int)(val)) % SIGLENBIT(siglen))
#define HASH(sign, val, siglen) SETBIT((sign), HASHVAL(val, siglen))

/* gist_intbig_ops opclass options */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			siglen;			/* signature length in bytes */
} GISTIntArrayBigOptions;

/*
 * type of index key
 */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		flag;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} GISTTYPE;

#define ALLISTRUE		0x04

#define ISALLTRUE(x)	( ((GISTTYPE*)x)->flag & ALLISTRUE )

#define GTHDRSIZE		(VARHDRSZ + sizeof(int32))
#define CALCGTSIZE(flag, siglen) ( GTHDRSIZE+(((flag) & ALLISTRUE) ? 0 : (siglen)) )

#define GETSIGN(x)		( (BITVECP)( (char*)x+GTHDRSIZE ) )

/*
 * useful functions
 */
bool		isort(int32 *a, int len);
ArrayType  *new_intArrayType(int num);
ArrayType  *copy_intArrayType(ArrayType *a);
ArrayType  *resize_intArrayType(ArrayType *a, int num);
int			internal_size(int *a, int len);
ArrayType  *_int_unique(ArrayType *a);
int32		intarray_match_first(ArrayType *a, int32 elem);
ArrayType  *intarray_add_elem(ArrayType *a, int32 elem);
ArrayType  *intarray_concat_arrays(ArrayType *a, ArrayType *b);
ArrayType  *int_to_intset(int32 elem);
bool		inner_int_overlap(ArrayType *a, ArrayType *b);
bool		inner_int_contains(ArrayType *a, ArrayType *b);
ArrayType  *inner_int_union(ArrayType *a, ArrayType *b);
ArrayType  *inner_int_inter(ArrayType *a, ArrayType *b);
void		rt__int_size(ArrayType *a, float *size);
void		gensign(BITVECP sign, int *a, int len, int siglen);


/*****************************************************************************
 *			Boolean Search
 *****************************************************************************/

#define BooleanSearchStrategy	20

/*
 * item in polish notation with back link
 * to left operand
 */
typedef struct ITEM
{
	int16		type;
	int16		left;
	int32		val;
} ITEM;

typedef struct QUERYTYPE
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		size;			/* number of ITEMs */
	ITEM		items[FLEXIBLE_ARRAY_MEMBER];
} QUERYTYPE;

#define HDRSIZEQT	offsetof(QUERYTYPE, items)
#define COMPUTESIZE(size)	( HDRSIZEQT + (size) * sizeof(ITEM) )
#define QUERYTYPEMAXITEMS	((MaxAllocSize - HDRSIZEQT) / sizeof(ITEM))
#define GETQUERY(x)  ( (x)->items )

/* "type" codes for ITEM */
#define END		0
#define ERR		1
#define VAL		2
#define OPR		3
#define OPEN	4
#define CLOSE	5

/* fmgr macros for QUERYTYPE objects */
#define DatumGetQueryTypeP(X)		  ((QUERYTYPE *) PG_DETOAST_DATUM(X))
#define DatumGetQueryTypePCopy(X)	  ((QUERYTYPE *) PG_DETOAST_DATUM_COPY(X))
#define PG_GETARG_QUERYTYPE_P(n)	  DatumGetQueryTypeP(PG_GETARG_DATUM(n))
#define PG_GETARG_QUERYTYPE_P_COPY(n) DatumGetQueryTypePCopy(PG_GETARG_DATUM(n))

bool		signconsistent(QUERYTYPE *query, BITVECP sign, int siglen, bool calcnot);
bool		execconsistent(QUERYTYPE *query, ArrayType *array, bool calcnot);

bool		gin_bool_consistent(QUERYTYPE *query, bool *check);
bool		query_has_required_values(QUERYTYPE *query);

int			compASC(const void *a, const void *b);
int			compDESC(const void *a, const void *b);

/* sort, either ascending or descending */
#define QSORT(a, direction) \
	do { \
		int		_nelems_ = ARRNELEMS(a); \
		if (_nelems_ > 1) \
			qsort((void*) ARRPTR(a), _nelems_, sizeof(int32), \
				  (direction) ? compASC : compDESC ); \
	} while(0)

#endif							/* ___INT_H__ */
