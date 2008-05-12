#ifndef ___INT_H__
#define ___INT_H__

#include "utils/array.h"

/* number ranges for compression */
#define MAXNUMRANGE 100

/* dimension of array */
#define NDIM 1

/* useful macros for accessing int4 arrays */
#define ARRPTR(x)  ( (int4 *) ARR_DATA_PTR(x) )
#define ARRNELEMS(x)  ArrayGetNItems(ARR_NDIM(x), ARR_DIMS(x))

/* reject arrays we can't handle; but allow a NULL or empty array */
#define CHECKARRVALID(x) \
	do { \
		if (x) { \
			if (ARR_NDIM(x) != NDIM && ARR_NDIM(x) != 0) \
				ereport(ERROR, \
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR), \
						 errmsg("array must be one-dimensional"))); \
			if (ARR_HASNULL(x)) \
				ereport(ERROR, \
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), \
						 errmsg("array must not contain nulls"))); \
		} \
	} while(0)

#define ARRISVOID(x)  ((x) == NULL || ARRNELEMS(x) == 0)

#define SORT(x) \
	do { \
		 if ( ARRNELEMS( x ) > 1 ) \
			isort( ARRPTR( x ), ARRNELEMS( x ) ); \
	} while(0)

#define PREPAREARR(x) \
	do { \
		 if ( ARRNELEMS( x ) > 1 ) \
			if ( isort( ARRPTR( x ), ARRNELEMS( x ) ) ) \
				x = _int_unique( x ); \
	} while(0)

/* "wish" function */
#define WISH_F(a,b,c) (double)( -(double)(((a)-(b))*((a)-(b))*((a)-(b)))*(c) )


/* bigint defines */
#define SIGLENINT  63			/* >122 => key will toast, so very slow!!! */
#define SIGLEN	( sizeof(int)*SIGLENINT )
#define SIGLENBIT (SIGLEN*BITS_PER_BYTE)

typedef char BITVEC[SIGLEN];
typedef char *BITVECP;

#define LOOPBYTE \
			for(i=0;i<SIGLEN;i++)

/* beware of multiple evaluation of arguments to these macros! */
#define GETBYTE(x,i) ( *( (BITVECP)(x) + (int)( (i) / BITS_PER_BYTE ) ) )
#define GETBITBYTE(x,i) ( (*((char*)(x)) >> (i)) & 0x01 )
#define CLRBIT(x,i)   GETBYTE(x,i) &= ~( 0x01 << ( (i) % BITS_PER_BYTE ) )
#define SETBIT(x,i)   GETBYTE(x,i) |=  ( 0x01 << ( (i) % BITS_PER_BYTE ) )
#define GETBIT(x,i) ( (GETBYTE(x,i) >> ( (i) % BITS_PER_BYTE )) & 0x01 )
#define HASHVAL(val) (((unsigned int)(val)) % SIGLENBIT)
#define HASH(sign, val) SETBIT((sign), HASHVAL(val))

/*
 * type of index key
 */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int4		flag;
	char		data[1];
}	GISTTYPE;

#define ALLISTRUE		0x04

#define ISALLTRUE(x)	( ((GISTTYPE*)x)->flag & ALLISTRUE )

#define GTHDRSIZE		(VARHDRSZ + sizeof(int4))
#define CALCGTSIZE(flag) ( GTHDRSIZE+(((flag) & ALLISTRUE) ? 0 : SIGLEN) )

#define GETSIGN(x)		( (BITVECP)( (char*)x+GTHDRSIZE ) )

/*
** types for functions
*/
typedef ArrayType *(*formarray) (ArrayType *, ArrayType *);
typedef void (*formfloat) (ArrayType *, float *);

/*
** useful function
*/
bool		isort(int4 *a, int len);
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
void		gensign(BITVEC sign, int *a, int len);


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
	int2		type;
	int2		left;
	int4		val;
}	ITEM;

typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int4		size;
	char		data[1];
}	QUERYTYPE;

#define HDRSIZEQT	(VARHDRSZ + sizeof(int4))
#define COMPUTESIZE(size)	( HDRSIZEQT + size * sizeof(ITEM) )
#define GETQUERY(x)  (ITEM*)( (char*)(x)+HDRSIZEQT )

#define END		0
#define ERR		1
#define VAL		2
#define OPR		3
#define OPEN	4
#define CLOSE	5

bool		signconsistent(QUERYTYPE * query, BITVEC sign, bool calcnot);
bool		execconsistent(QUERYTYPE * query, ArrayType *array, bool calcnot);
bool		ginconsistent(QUERYTYPE * query, bool *check);
int4		shorterquery(ITEM * q, int4 len);

int			compASC(const void *a, const void *b);

int			compDESC(const void *a, const void *b);

#define QSORT(a, direction)										\
if (ARRNELEMS(a) > 1)											\
		qsort((void*)ARRPTR(a), ARRNELEMS(a),sizeof(int4),		\
				(direction) ? compASC : compDESC )

#endif /* ___INT_H__ */
