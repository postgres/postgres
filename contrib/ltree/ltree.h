/* contrib/ltree/ltree.h */

#ifndef __LTREE_H__
#define __LTREE_H__

#include "fmgr.h"
#include "tsearch/ts_locale.h"
#include "utils/memutils.h"


/* ltree */

/*
 * We want the maximum length of a label to be encoding-independent, so
 * set it somewhat arbitrarily at 1000 characters (not bytes), while using
 * uint16 fields to hold the byte length.
 */
#define LTREE_LABEL_MAX_CHARS 1000

/*
 * LOWER_NODE used to be defined in the Makefile via the compile flags.
 * However the MSVC build scripts neglected to do the same which resulted in
 * MSVC builds not using LOWER_NODE.  Since then, the MSVC scripts have been
 * modified to look for -D compile flags in Makefiles, so here, in order to
 * get the historic behavior of LOWER_NODE not being defined on MSVC, we only
 * define it when not building in that environment.  This is important as we
 * want to maintain the same LOWER_NODE behavior after a pg_upgrade.
 */
#ifndef _MSC_VER
#define LOWER_NODE
#endif

typedef struct
{
	uint16		len;			/* label string length in bytes */
	char		name[FLEXIBLE_ARRAY_MEMBER];
} ltree_level;

#define LEVEL_HDRSIZE	(offsetof(ltree_level,name))
#define LEVEL_NEXT(x)	( (ltree_level*)( ((char*)(x)) + MAXALIGN(((ltree_level*)(x))->len + LEVEL_HDRSIZE) ) )

typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	uint16		numlevel;		/* number of labels */
	/* Array of maxalign'd ltree_level structs follows: */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} ltree;

#define LTREE_HDRSIZE	MAXALIGN( offsetof(ltree, data) )
#define LTREE_FIRST(x)	( (ltree_level*)( ((char*)(x))+LTREE_HDRSIZE ) )
#define LTREE_MAX_LEVELS	PG_UINT16_MAX	/* ltree.numlevel is uint16 */


/* lquery */

/* lquery_variant: one branch of some OR'ed alternatives */
typedef struct
{
	int32		val;			/* CRC of label string */
	uint16		len;			/* label string length in bytes */
	uint8		flag;			/* see LVAR_xxx flags below */
	char		name[FLEXIBLE_ARRAY_MEMBER];
} lquery_variant;

/*
 * Note: these macros contain too many MAXALIGN calls and so will sometimes
 * overestimate the space needed for an lquery_variant.  However, we can't
 * change it without breaking on-disk compatibility for lquery.
 */
#define LVAR_HDRSIZE   MAXALIGN(offsetof(lquery_variant, name))
#define LVAR_NEXT(x)	( (lquery_variant*)( ((char*)(x)) + MAXALIGN(((lquery_variant*)(x))->len) + LVAR_HDRSIZE ) )

#define LVAR_ANYEND 0x01		/* '*' flag: prefix match */
#define LVAR_INCASE 0x02		/* '@' flag: case-insensitive match */
#define LVAR_SUBLEXEME	0x04	/* '%' flag: word-wise match */

/*
 * In an lquery_level, "flag" contains the union of the variants' flags
 * along with possible LQL_xxx flags; so those bit sets can't overlap.
 *
 * "low" and "high" are nominally the minimum and maximum number of matches.
 * However, for backwards compatibility with pre-v13 on-disk lqueries,
 * non-'*' levels (those with numvar > 0) only have valid low/high if the
 * LQL_COUNT flag is set; otherwise those fields are zero, but the behavior
 * is as if they were both 1.
 */
typedef struct
{
	uint16		totallen;		/* total length of this level, in bytes */
	uint16		flag;			/* see LQL_xxx and LVAR_xxx flags */
	uint16		numvar;			/* number of variants; 0 means '*' */
	uint16		low;			/* minimum repeat count */
	uint16		high;			/* maximum repeat count */
	/* Array of maxalign'd lquery_variant structs follows: */
	char		variants[FLEXIBLE_ARRAY_MEMBER];
} lquery_level;

#define LQL_HDRSIZE MAXALIGN( offsetof(lquery_level,variants) )
#define LQL_NEXT(x) ( (lquery_level*)( ((char*)(x)) + MAXALIGN(((lquery_level*)(x))->totallen) ) )
#define LQL_FIRST(x)	( (lquery_variant*)( ((char*)(x))+LQL_HDRSIZE ) )

#define LQL_NOT		0x10		/* level has '!' (NOT) prefix */
#define LQL_COUNT	0x20		/* level is non-'*' and has repeat counts */

#ifdef LOWER_NODE
#define FLG_CANLOOKSIGN(x) ( ( (x) & ( LQL_NOT | LVAR_ANYEND | LVAR_SUBLEXEME ) ) == 0 )
#else
#define FLG_CANLOOKSIGN(x) ( ( (x) & ( LQL_NOT | LVAR_ANYEND | LVAR_SUBLEXEME | LVAR_INCASE ) ) == 0 )
#endif
#define LQL_CANLOOKSIGN(x) FLG_CANLOOKSIGN( ((lquery_level*)(x))->flag )

typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	uint16		numlevel;		/* number of lquery_levels */
	uint16		firstgood;		/* number of leading simple-match levels */
	uint16		flag;			/* see LQUERY_xxx flags below */
	/* Array of maxalign'd lquery_level structs follows: */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} lquery;

#define LQUERY_HDRSIZE	 MAXALIGN( offsetof(lquery, data) )
#define LQUERY_FIRST(x)   ( (lquery_level*)( ((char*)(x))+LQUERY_HDRSIZE ) )
#define LQUERY_MAX_LEVELS	PG_UINT16_MAX	/* lquery.numlevel is uint16 */

#define LQUERY_HASNOT		0x01

/* valid label chars are alphanumerics, underscores and hyphens */
#define ISLABEL(x) ( t_isalnum(x) || t_iseq(x, '_') || t_iseq(x, '-') )

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
#define VALTRUE					6	/* for stop words */
#define VALFALSE				7


/* use in array iterator */
PGDLLEXPORT Datum ltree_isparent(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum ltree_risparent(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum ltq_regex(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum ltq_rregex(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum lt_q_regex(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum lt_q_rregex(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum ltxtq_exec(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum ltxtq_rexec(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum _ltq_regex(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum _ltq_rregex(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum _lt_q_regex(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum _lt_q_rregex(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum _ltxtq_exec(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum _ltxtq_rexec(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum _ltree_isparent(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum _ltree_risparent(PG_FUNCTION_ARGS);

/* Concatenation functions */
PGDLLEXPORT Datum ltree_addltree(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum ltree_addtext(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum ltree_textadd(PG_FUNCTION_ARGS);

/* Util function */
PGDLLEXPORT Datum ltree_in(PG_FUNCTION_ARGS);

bool		ltree_execute(ITEM *curitem, void *checkval,
						  bool calcnot, bool (*chkcond) (void *checkval, ITEM *val));

int			ltree_compare(const ltree *a, const ltree *b);
bool		inner_isparent(const ltree *c, const ltree *p);
bool		compare_subnode(ltree_level *t, char *qn, int len,
							int (*cmpptr) (const char *, const char *, size_t), bool anyend);
ltree	   *lca_inner(ltree **a, int len);
int			ltree_strncasecmp(const char *a, const char *b, size_t s);

/* fmgr macros for ltree objects */
#define DatumGetLtreeP(X)			((ltree *) PG_DETOAST_DATUM(X))
#define DatumGetLtreePCopy(X)		((ltree *) PG_DETOAST_DATUM_COPY(X))
#define PG_GETARG_LTREE_P(n)		DatumGetLtreeP(PG_GETARG_DATUM(n))
#define PG_GETARG_LTREE_P_COPY(n)	DatumGetLtreePCopy(PG_GETARG_DATUM(n))

#define DatumGetLqueryP(X)			((lquery *) PG_DETOAST_DATUM(X))
#define DatumGetLqueryPCopy(X)		((lquery *) PG_DETOAST_DATUM_COPY(X))
#define PG_GETARG_LQUERY_P(n)		DatumGetLqueryP(PG_GETARG_DATUM(n))
#define PG_GETARG_LQUERY_P_COPY(n)	DatumGetLqueryPCopy(PG_GETARG_DATUM(n))

#define DatumGetLtxtqueryP(X)			((ltxtquery *) PG_DETOAST_DATUM(X))
#define DatumGetLtxtqueryPCopy(X)		((ltxtquery *) PG_DETOAST_DATUM_COPY(X))
#define PG_GETARG_LTXTQUERY_P(n)		DatumGetLtxtqueryP(PG_GETARG_DATUM(n))
#define PG_GETARG_LTXTQUERY_P_COPY(n)	DatumGetLtxtqueryPCopy(PG_GETARG_DATUM(n))

/* GiST support for ltree */

#define BITBYTE 8
#define SIGLENBIT(siglen) ((siglen) * BITBYTE)
#define LTREE_SIGLEN_DEFAULT	(2 * sizeof(int32))
#define LTREE_SIGLEN_MAX		GISTMaxIndexKeySize
#define LTREE_GET_SIGLEN()		(PG_HAS_OPCLASS_OPTIONS() ? \
								 ((LtreeGistOptions *) PG_GET_OPCLASS_OPTIONS())->siglen : \
								 LTREE_SIGLEN_DEFAULT)

typedef unsigned char *BITVECP;

#define LOOPBYTE(siglen) \
			for(i = 0; i < (siglen); i++)

#define GETBYTE(x,i) ( *( (BITVECP)(x) + (int)( (i) / BITBYTE ) ) )
#define GETBITBYTE(x,i) ( ((unsigned char)(x)) >> i & 0x01 )
#define CLRBIT(x,i)   GETBYTE(x,i) &= ~( 0x01 << ( (i) % BITBYTE ) )
#define SETBIT(x,i)   GETBYTE(x,i) |=  ( 0x01 << ( (i) % BITBYTE ) )
#define GETBIT(x,i) ( (GETBYTE(x,i) >> ( (i) % BITBYTE )) & 0x01 )

#define HASHVAL(val, siglen) (((unsigned int)(val)) % SIGLENBIT(siglen))
#define HASH(sign, val, siglen) SETBIT((sign), HASHVAL(val, siglen))

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
#define LTG_LNODE(x, siglen)	( (ltree*)( ( ((char*)(x))+LTG_HDRSIZE ) + ( LTG_ISALLTRUE(x) ? 0 : (siglen) ) ) )
#define LTG_RENODE(x, siglen)	( (ltree*)( ((char*)LTG_LNODE(x, siglen)) + VARSIZE(LTG_LNODE(x, siglen))) )
#define LTG_RNODE(x, siglen)	( LTG_ISNORIGHT(x) ? LTG_LNODE(x, siglen) : LTG_RENODE(x, siglen) )

#define LTG_GETLNODE(x, siglen) ( LTG_ISONENODE(x) ? LTG_NODE(x) : LTG_LNODE(x, siglen) )
#define LTG_GETRNODE(x, siglen) ( LTG_ISONENODE(x) ? LTG_NODE(x) : LTG_RNODE(x, siglen) )

extern ltree_gist *ltree_gist_alloc(bool isalltrue, BITVECP sign, int siglen,
									ltree *left, ltree *right);

/* GiST support for ltree[] */

#define LTREE_ASIGLEN_DEFAULT	(7 * sizeof(int32))
#define LTREE_ASIGLEN_MAX		GISTMaxIndexKeySize
#define LTREE_GET_ASIGLEN()		(PG_HAS_OPCLASS_OPTIONS() ? \
								 ((LtreeGistOptions *) PG_GET_OPCLASS_OPTIONS())->siglen : \
								 LTREE_ASIGLEN_DEFAULT)
#define ASIGLENBIT(siglen)		((siglen) * BITBYTE)

#define ALOOPBYTE(siglen) \
			for (i = 0; i < (siglen); i++)

#define AHASHVAL(val, siglen) (((unsigned int)(val)) % ASIGLENBIT(siglen))
#define AHASH(sign, val, siglen) SETBIT((sign), AHASHVAL(val, siglen))

/* gist_ltree_ops and gist__ltree_ops opclass options */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int			siglen;			/* signature length in bytes */
} LtreeGistOptions;

/* type of key is the same to ltree_gist */

#endif
