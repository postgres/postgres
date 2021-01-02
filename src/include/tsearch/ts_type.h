/*-------------------------------------------------------------------------
 *
 * ts_type.h
 *	  Definitions for the tsvector and tsquery types
 *
 * Copyright (c) 1998-2021, PostgreSQL Global Development Group
 *
 * src/include/tsearch/ts_type.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PG_TSTYPE_H_
#define _PG_TSTYPE_H_

#include "fmgr.h"
#include "utils/memutils.h"


/*
 * TSVector type.
 *
 * Structure of tsvector datatype:
 * 1) standard varlena header
 * 2) int32		size - number of lexemes (WordEntry array entries)
 * 3) Array of WordEntry - one per lexeme; must be sorted according to
 *				tsCompareString() (ie, memcmp of lexeme strings).
 *				WordEntry->pos gives the number of bytes from end of WordEntry
 *				array to start of lexeme's string, which is of length len.
 * 4) Per-lexeme data storage:
 *	  lexeme string (not null-terminated)
 *	  if haspos is true:
 *		padding byte if necessary to make the position data 2-byte aligned
 *		uint16			number of positions that follow
 *		WordEntryPos[]	positions
 *
 * The positions for each lexeme must be sorted.
 *
 * Note, tsvectorsend/recv believe that sizeof(WordEntry) == 4
 */

typedef struct
{
	uint32
				haspos:1,
				len:11,			/* MAX 2Kb */
				pos:20;			/* MAX 1Mb */
} WordEntry;

#define MAXSTRLEN ( (1<<11) - 1)
#define MAXSTRPOS ( (1<<20) - 1)

extern int	compareWordEntryPos(const void *a, const void *b);

/*
 * Equivalent to
 * typedef struct {
 *		uint16
 *			weight:2,
 *			pos:14;
 * }
 */

typedef uint16 WordEntryPos;

typedef struct
{
	uint16		npos;
	WordEntryPos pos[FLEXIBLE_ARRAY_MEMBER];
} WordEntryPosVector;

/* WordEntryPosVector with exactly 1 entry */
typedef struct
{
	uint16		npos;
	WordEntryPos pos[1];
} WordEntryPosVector1;


#define WEP_GETWEIGHT(x)	( (x) >> 14 )
#define WEP_GETPOS(x)		( (x) & 0x3fff )

#define WEP_SETWEIGHT(x,v)	( (x) = ( (v) << 14 ) | ( (x) & 0x3fff ) )
#define WEP_SETPOS(x,v)		( (x) = ( (x) & 0xc000 ) | ( (v) & 0x3fff ) )

#define MAXENTRYPOS (1<<14)
#define MAXNUMPOS	(256)
#define LIMITPOS(x) ( ( (x) >= MAXENTRYPOS ) ? (MAXENTRYPOS-1) : (x) )

/* This struct represents a complete tsvector datum */
typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		size;
	WordEntry	entries[FLEXIBLE_ARRAY_MEMBER];
	/* lexemes follow the entries[] array */
} TSVectorData;

typedef TSVectorData *TSVector;

#define DATAHDRSIZE (offsetof(TSVectorData, entries))
#define CALCDATASIZE(nentries, lenstr) (DATAHDRSIZE + (nentries) * sizeof(WordEntry) + (lenstr) )

/* pointer to start of a tsvector's WordEntry array */
#define ARRPTR(x)	( (x)->entries )

/* pointer to start of a tsvector's lexeme storage */
#define STRPTR(x)	( (char *) &(x)->entries[(x)->size] )

#define _POSVECPTR(x, e)	((WordEntryPosVector *)(STRPTR(x) + SHORTALIGN((e)->pos + (e)->len)))
#define POSDATALEN(x,e) ( ( (e)->haspos ) ? (_POSVECPTR(x,e)->npos) : 0 )
#define POSDATAPTR(x,e) (_POSVECPTR(x,e)->pos)

/*
 * fmgr interface macros
 */

#define DatumGetTSVector(X)			((TSVector) PG_DETOAST_DATUM(X))
#define DatumGetTSVectorCopy(X)		((TSVector) PG_DETOAST_DATUM_COPY(X))
#define TSVectorGetDatum(X)			PointerGetDatum(X)
#define PG_GETARG_TSVECTOR(n)		DatumGetTSVector(PG_GETARG_DATUM(n))
#define PG_GETARG_TSVECTOR_COPY(n)	DatumGetTSVectorCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_TSVECTOR(x)		return TSVectorGetDatum(x)


/*
 * TSQuery
 *
 *
 */

typedef int8 QueryItemType;

/* Valid values for QueryItemType: */
#define QI_VAL 1
#define QI_OPR 2
#define QI_VALSTOP 3			/* This is only used in an intermediate stack
								 * representation in parse_tsquery. It's not a
								 * legal type elsewhere. */

/*
 * QueryItem is one node in tsquery - operator or operand.
 */
typedef struct
{
	QueryItemType type;			/* operand or kind of operator (ts_tokentype) */
	uint8		weight;			/* weights of operand to search. It's a
								 * bitmask of allowed weights. if it =0 then
								 * any weight are allowed. Weights and bit
								 * map: A: 1<<3 B: 1<<2 C: 1<<1 D: 1<<0 */
	bool		prefix;			/* true if it's a prefix search */
	int32		valcrc;			/* XXX: pg_crc32 would be a more appropriate
								 * data type, but we use comparisons to signed
								 * integers in the code. They would need to be
								 * changed as well. */

	/* pointer to text value of operand, must correlate with WordEntry */
	uint32
				length:12,
				distance:20;
} QueryOperand;


/*
 * Legal values for QueryOperator.operator.
 */
#define OP_NOT			1
#define OP_AND			2
#define OP_OR			3
#define OP_PHRASE		4		/* highest code, tsquery_cleanup.c */
#define OP_COUNT		4

extern const int tsearch_op_priority[OP_COUNT];

/* get operation priority  by its code*/
#define OP_PRIORITY(x)	( tsearch_op_priority[(x) - 1] )
/* get QueryOperator priority */
#define QO_PRIORITY(x)	OP_PRIORITY(((QueryOperator *) (x))->oper)

typedef struct
{
	QueryItemType type;
	int8		oper;			/* see above */
	int16		distance;		/* distance between agrs for OP_PHRASE */
	uint32		left;			/* pointer to left operand. Right operand is
								 * item + 1, left operand is placed
								 * item+item->left */
} QueryOperator;

/*
 * Note: TSQuery is 4-bytes aligned, so make sure there's no fields
 * inside QueryItem requiring 8-byte alignment, like int64.
 */
typedef union
{
	QueryItemType type;
	QueryOperator qoperator;
	QueryOperand qoperand;
} QueryItem;

/*
 * Storage:
 *	(len)(size)(array of QueryItem)(operands as '\0'-terminated c-strings)
 */

typedef struct
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		size;			/* number of QueryItems */
	char		data[FLEXIBLE_ARRAY_MEMBER];	/* data starts here */
} TSQueryData;

typedef TSQueryData *TSQuery;

#define HDRSIZETQ	( VARHDRSZ + sizeof(int32) )

/* Computes the size of header and all QueryItems. size is the number of
 * QueryItems, and lenofoperand is the total length of all operands
 */
#define COMPUTESIZE(size, lenofoperand) ( HDRSIZETQ + (size) * sizeof(QueryItem) + (lenofoperand) )
#define TSQUERY_TOO_BIG(size, lenofoperand) \
	((size) > (MaxAllocSize - HDRSIZETQ - (lenofoperand)) / sizeof(QueryItem))

/* Returns a pointer to the first QueryItem in a TSQuery */
#define GETQUERY(x)  ((QueryItem*)( (char*)(x)+HDRSIZETQ ))

/* Returns a pointer to the beginning of operands in a TSQuery */
#define GETOPERAND(x)	( (char*)GETQUERY(x) + ((TSQuery)(x))->size * sizeof(QueryItem) )

/*
 * fmgr interface macros
 * Note, TSQuery type marked as plain storage, so it can't be toasted
 * but PG_DETOAST_DATUM_COPY is used for simplicity
 */

#define DatumGetTSQuery(X)			((TSQuery) DatumGetPointer(X))
#define DatumGetTSQueryCopy(X)		((TSQuery) PG_DETOAST_DATUM_COPY(X))
#define TSQueryGetDatum(X)			PointerGetDatum(X)
#define PG_GETARG_TSQUERY(n)		DatumGetTSQuery(PG_GETARG_DATUM(n))
#define PG_GETARG_TSQUERY_COPY(n)	DatumGetTSQueryCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_TSQUERY(x)		return TSQueryGetDatum(x)

#endif							/* _PG_TSTYPE_H_ */
