/* contrib/ltree/ltree.h */

#ifndef __LTREE_H__
#define __LTREE_H__

#include "fmgr.h"
#include "tsearch/ts_locale.h"
#include "utils/memutils.h"

typedef struct
{
	uint16		len;
	char		name[FLEXIBLE_ARRAY_MEMBER];
} ltree_level;

#define LEVEL_HDRSIZE	(offsetof(ltree_level,name))
#define LEVEL_NEXT(x)	( (ltree_level*)( ((char*)(x)) + MAXALIGN(((ltree_level*)(x))->len + LEVEL_HDRSIZE) ) )

typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	uint16		numlevel;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} ltree;

#define LTREE_HDRSIZE	MAXALIGN( offsetof(ltree, data) )
#define LTREE_FIRST(x)	( (ltree_level*)( ((char*)(x))+LTREE_HDRSIZE ) )


/* lquery */

typedef struct
{
	int32		val;
	uint16		len;
	uint8		flag;
	char		name[FLEXIBLE_ARRAY_MEMBER];
} lquery_variant;

#define LVAR_HDRSIZE   MAXALIGN(offsetof(lquery_variant, name))
#define LVAR_NEXT(x)	( (lquery_variant*)( ((char*)(x)) + MAXALIGN(((lquery_variant*)(x))->len) + LVAR_HDRSIZE ) )

#define LVAR_ANYEND 0x01
#define LVAR_INCASE 0x02
#define LVAR_SUBLEXEME	0x04

typedef struct
{
	uint16		totallen;
	uint16		flag;
	uint16		numvar;
	uint16		low;
	uint16		high;
	char		variants[FLEXIBLE_ARRAY_MEMBER];
} lquery_level;

#define LQL_HDRSIZE MAXALIGN( offsetof(lquery_level,variants) )
#define LQL_NEXT(x) ( (lquery_level*)( ((char*)(x)) + MAXALIGN(((lquery_level*)(x))->totallen) ) )
#define LQL_FIRST(x)	( (lquery_variant*)( ((char*)(x))+LQL_HDRSIZE ) )

#define LQL_NOT		0x10
#ifdef LOWER_NODE
#define FLG_CANLOOKSIGN(x) ( ( (x) & ( LQL_NOT | LVAR_ANYEND | LVAR_SUBLEXEME ) ) == 0 )
#else
#define FLG_CANLOOKSIGN(x) ( ( (x) & ( LQL_NOT | LVAR_ANYEND | LVAR_SUBLEXEME | LVAR_INCASE ) ) == 0 )
#endif
#define LQL_CANLOOKSIGN(x) FLG_CANLOOKSIGN( ((lquery_level*)(x))->flag )

typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	uint16		numlevel;
	uint16		firstgood;
	uint16		flag;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} lquery;

#define LQUERY_HDRSIZE	 MAXALIGN( offsetof(lquery, data) )
#define LQUERY_FIRST(x)   ( (lquery_level*)( ((char*)(x))+LQUERY_HDRSIZE ) )

#define LQUERY_HASNOT		0x01

#define ISALNUM(x)	( t_isalpha(x) || t_isdigit(x)	|| ( pg_mblen(x) == 1 && t_iseq((x), '_') ) )

/* full text query */

/*
 * item in polish notation with back link
 * to left operand
 */
typedef struct ITEM
{
	int16		type;
	int16		left;
	int32		val;
	uint8		flag;
	/* user-friendly value */
	uint8		length;
	uint16		distance;
} ITEM;

/*
 *Storage:
 *		(len)(size)(array of ITEM)(array of operand in user-friendly form)
 */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		size;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} ltxtquery;

#define HDRSIZEQT		MAXALIGN(VARHDRSZ + sizeof(int32))
#define COMPUTESIZE(size,lenofoperand)	( HDRSIZEQT + (size) * sizeof(ITEM) + (lenofoperand) )
#define LTXTQUERY_TOO_BIG(size,lenofoperand) \
	((size) > (MaxAllocSize - HDRSIZEQT - (lenofoperand)) / sizeof(ITEM))
#define GETQUERY(x)  (ITEM*)( (char*)(x)+HDRSIZEQT )
#define GETOPERAND(x)	( (char*)GETQUERY(x) + ((ltxtquery*)x)->size * sizeof(ITEM) )

#define ISOPERATOR(x) ( (x)=='!' || (x)=='&' || (x)=='|' || (x)=='(' || (x)==')' )

#define END						0
#define ERR						1
#define VAL						2
#define OPR						3
#define OPEN					4
#define CLOSE					5
#define VALTRUE					6		/* for stop words */
#define VALFALSE				7


/* use in array iterator */
Datum		ltree_isparent(PG_FUNCTION_ARGS);
Datum		ltree_risparent(PG_FUNCTION_ARGS);
Datum		ltq_regex(PG_FUNCTION_ARGS);
Datum		ltq_rregex(PG_FUNCTION_ARGS);
Datum		lt_q_regex(PG_FUNCTION_ARGS);
Datum		lt_q_rregex(PG_FUNCTION_ARGS);
Datum		ltxtq_exec(PG_FUNCTION_ARGS);
Datum		ltxtq_rexec(PG_FUNCTION_ARGS);
Datum		_ltq_regex(PG_FUNCTION_ARGS);
Datum		_ltq_rregex(PG_FUNCTION_ARGS);
Datum		_lt_q_regex(PG_FUNCTION_ARGS);
Datum		_lt_q_rregex(PG_FUNCTION_ARGS);
Datum		_ltxtq_exec(PG_FUNCTION_ARGS);
Datum		_ltxtq_rexec(PG_FUNCTION_ARGS);
Datum		_ltree_isparent(PG_FUNCTION_ARGS);
Datum		_ltree_risparent(PG_FUNCTION_ARGS);

/* Concatenation functions */
Datum		ltree_addltree(PG_FUNCTION_ARGS);
Datum		ltree_addtext(PG_FUNCTION_ARGS);
Datum		ltree_textadd(PG_FUNCTION_ARGS);

/* Util function */
Datum		ltree_in(PG_FUNCTION_ARGS);

bool ltree_execute(ITEM *curitem, void *checkval,
			  bool calcnot, bool (*chkcond) (void *checkval, ITEM *val));

int			ltree_compare(const ltree *a, const ltree *b);
bool		inner_isparent(const ltree *c, const ltree *p);
bool compare_subnode(ltree_level *t, char *q, int len,
			int (*cmpptr) (const char *, const char *, size_t), bool anyend);
ltree	   *lca_inner(ltree **a, int len);
int			ltree_strncasecmp(const char *a, const char *b, size_t s);

#define PG_GETARG_LTREE(x)	((ltree*)DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(x))))
#define PG_GETARG_LTREE_COPY(x) ((ltree*)DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(x))))
#define PG_GETARG_LQUERY(x) ((lquery*)DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(x))))
#define PG_GETARG_LQUERY_COPY(x) ((lquery*)DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(x))))
#define PG_GETARG_LTXTQUERY(x) ((ltxtquery*)DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(x))))
#define PG_GETARG_LTXTQUERY_COPY(x) ((ltxtquery*)DatumGetPointer(PG_DETOAST_DATUM_COPY(PG_GETARG_DATUM(x))))

/* GiST support for ltree */

#define BITBYTE 8
#define SIGLENINT  2
#define SIGLEN	( sizeof(int32)*SIGLENINT )
#define SIGLENBIT (SIGLEN*BITBYTE)
typedef unsigned char BITVEC[SIGLEN];
typedef unsigned char *BITVECP;

#define LOOPBYTE \
			for(i=0;i<SIGLEN;i++)

#define GETBYTE(x,i) ( *( (BITVECP)(x) + (int)( (i) / BITBYTE ) ) )
#define GETBITBYTE(x,i) ( ((unsigned char)(x)) >> i & 0x01 )
#define CLRBIT(x,i)   GETBYTE(x,i) &= ~( 0x01 << ( (i) % BITBYTE ) )
#define SETBIT(x,i)   GETBYTE(x,i) |=  ( 0x01 << ( (i) % BITBYTE ) )
#define GETBIT(x,i) ( (GETBYTE(x,i) >> ( (i) % BITBYTE )) & 0x01 )

#define HASHVAL(val) (((unsigned int)(val)) % SIGLENBIT)
#define HASH(sign, val) SETBIT((sign), HASHVAL(val))

/*
 * type of index key for ltree. Tree are combined B-Tree and R-Tree
 * Storage:
 *	Leaf pages
 *		(len)(flag)(ltree)
 *	Non-Leaf
 *				 (len)(flag)(sign)(left_ltree)(right_ltree)
 *		ALLTRUE: (len)(flag)(left_ltree)(right_ltree)
 *
 */

typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	uint32		flag;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} ltree_gist;

#define LTG_ONENODE 0x01
#define LTG_ALLTRUE 0x02
#define LTG_NORIGHT 0x04

#define LTG_HDRSIZE MAXALIGN(VARHDRSZ + sizeof(uint32))
#define LTG_SIGN(x) ( (BITVECP)( ((char*)(x))+LTG_HDRSIZE ) )
#define LTG_NODE(x) ( (ltree*)( ((char*)(x))+LTG_HDRSIZE ) )
#define LTG_ISONENODE(x) ( ((ltree_gist*)(x))->flag & LTG_ONENODE )
#define LTG_ISALLTRUE(x) ( ((ltree_gist*)(x))->flag & LTG_ALLTRUE )
#define LTG_ISNORIGHT(x) ( ((ltree_gist*)(x))->flag & LTG_NORIGHT )
#define LTG_LNODE(x)	( (ltree*)( ( ((char*)(x))+LTG_HDRSIZE ) + ( LTG_ISALLTRUE(x) ? 0 : SIGLEN ) ) )
#define LTG_RENODE(x)	( (ltree*)( ((char*)LTG_LNODE(x)) + VARSIZE(LTG_LNODE(x))) )
#define LTG_RNODE(x)	( LTG_ISNORIGHT(x) ? LTG_LNODE(x) : LTG_RENODE(x) )

#define LTG_GETLNODE(x) ( LTG_ISONENODE(x) ? LTG_NODE(x) : LTG_LNODE(x) )
#define LTG_GETRNODE(x) ( LTG_ISONENODE(x) ? LTG_NODE(x) : LTG_RNODE(x) )


/* GiST support for ltree[] */

#define ASIGLENINT	(7)
#define ASIGLEN		(sizeof(int32)*ASIGLENINT)
#define ASIGLENBIT (ASIGLEN*BITBYTE)
typedef unsigned char ABITVEC[ASIGLEN];

#define ALOOPBYTE \
			for(i=0;i<ASIGLEN;i++)

#define AHASHVAL(val) (((unsigned int)(val)) % ASIGLENBIT)
#define AHASH(sign, val) SETBIT((sign), AHASHVAL(val))

/* type of key is the same to ltree_gist */

#endif
