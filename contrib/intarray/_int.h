#include "postgres.h"

#include <float.h>

#include "access/gist.h"
#include "access/itup.h"
#include "access/rtree.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "storage/bufpage.h"
#include "lib/stringinfo.h"

/* number ranges for compression */
#define MAXNUMRANGE 100

#define max(a,b)		((a) >	(b) ? (a) : (b))
#define min(a,b)		((a) <= (b) ? (a) : (b))
#define abs(a)			((a) <	(0) ? -(a) : (a))

/* dimension of array */
#define NDIM 1

/*
 * flags for gist__int_ops, use ArrayType->flags
 * which is unused (see array.h)
 */
#define LEAFKEY		(1<<31)
#define ISLEAFKEY(x)	( ((ArrayType*)(x))->flags & LEAFKEY )

/* useful macros for accessing int4 arrays */
#define ARRPTR(x)  ( (int4 *) ARR_DATA_PTR(x) )
#define ARRNELEMS(x)  ArrayGetNItems( ARR_NDIM(x), ARR_DIMS(x))

#define ARRISVOID(x) ( (x) ? ( ( ARR_NDIM(x) == NDIM ) ? ( ( ARRNELEMS( x ) ) ? 0 : 1 ) : ( ( ARR_NDIM(x) ) ? ( \
	ereport(ERROR, \
			(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR), \
			 errmsg("array must be one-dimensional, not %d dimensions", ARRNELEMS( x )))) \
	,1) : 0 )  ) : 0 )

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
#define BITBYTE 8
#define SIGLENINT  63			/* >122 => key will toast, so very slow!!! */
#define SIGLEN	( sizeof(int)*SIGLENINT )
#define SIGLENBIT (SIGLEN*BITBYTE)

typedef char BITVEC[SIGLEN];
typedef char *BITVECP;

#define SIGPTR(x)  ( (BITVECP) ARR_DATA_PTR(x) )


#define LOOPBYTE(a) \
		for(i=0;i<SIGLEN;i++) {\
				a;\
		}

#define LOOPBIT(a) \
		for(i=0;i<SIGLENBIT;i++) {\
				a;\
		}

/* beware of multiple evaluation of arguments to these macros! */
#define GETBYTE(x,i) ( *( (BITVECP)(x) + (int)( (i) / BITBYTE ) ) )
#define GETBITBYTE(x,i) ( (*((char*)(x)) >> (i)) & 0x01 )
#define CLRBIT(x,i)   GETBYTE(x,i) &= ~( 0x01 << ( (i) % BITBYTE ) )
#define SETBIT(x,i)   GETBYTE(x,i) |=  ( 0x01 << ( (i) % BITBYTE ) )
#define GETBIT(x,i) ( (GETBYTE(x,i) >> ( (i) % BITBYTE )) & 0x01 )
#define HASHVAL(val) (((unsigned int)(val)) % SIGLENBIT)
#define HASH(sign, val) SETBIT((sign), HASHVAL(val))

/*
 * type of index key
 */
typedef struct
{
	int4		len;
	int4		flag;
	char		data[1];
}	GISTTYPE;

#define ALLISTRUE		0x04

#define ISALLTRUE(x)	( ((GISTTYPE*)x)->flag & ALLISTRUE )

#define GTHDRSIZE		( sizeof(int4)*2  )
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
bool		isort(int4 *a, const int len);
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
	int4		len;
	int4		size;
	char		data[1];
}	QUERYTYPE;

#define HDRSIZEQT	( 2*sizeof(int4) )
#define COMPUTESIZE(size)	( HDRSIZEQT + size * sizeof(ITEM) )
#define GETQUERY(x)  (ITEM*)( (char*)(x)+HDRSIZEQT )

bool		signconsistent(QUERYTYPE * query, BITVEC sign, bool calcnot);
bool		execconsistent(QUERYTYPE * query, ArrayType *array, bool calcnot);



int			compASC(const void *a, const void *b);

int			compDESC(const void *a, const void *b);

#define QSORT(a, direction)										\
if (ARRNELEMS(a) > 1)											\
		qsort((void*)ARRPTR(a), ARRNELEMS(a),sizeof(int4),		\
				(direction) ? compASC : compDESC )
