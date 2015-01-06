/*-------------------------------------------------------------------------
 *
 * ts_utils.h
 *	  helper utilities for tsearch
 *
 * Copyright (c) 1998-2015, PostgreSQL Global Development Group
 *
 * src/include/tsearch/ts_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PG_TS_UTILS_H_
#define _PG_TS_UTILS_H_

#include "tsearch/ts_type.h"
#include "tsearch/ts_public.h"
#include "nodes/pg_list.h"

/*
 * Common parse definitions for tsvector and tsquery
 */

/* tsvector parser support. */

struct TSVectorParseStateData;	/* opaque struct in tsvector_parser.c */
typedef struct TSVectorParseStateData *TSVectorParseState;

extern TSVectorParseState init_tsvector_parser(char *input,
					 bool oprisdelim,
					 bool is_tsquery);
extern void reset_tsvector_parser(TSVectorParseState state, char *input);
extern bool gettoken_tsvector(TSVectorParseState state,
				  char **token, int *len,
				  WordEntryPos **pos, int *poslen,
				  char **endptr);
extern void close_tsvector_parser(TSVectorParseState state);

/* parse_tsquery */

struct TSQueryParserStateData;	/* private in backend/utils/adt/tsquery.c */
typedef struct TSQueryParserStateData *TSQueryParserState;

typedef void (*PushFunction) (Datum opaque, TSQueryParserState state,
										  char *token, int tokenlen,
										  int16 tokenweights,	/* bitmap as described
																 * in QueryOperand
																 * struct */
										  bool prefix);

extern TSQuery parse_tsquery(char *buf,
			  PushFunction pushval,
			  Datum opaque, bool isplain);

/* Functions for use by PushFunction implementations */
extern void pushValue(TSQueryParserState state,
		  char *strval, int lenval, int16 weight, bool prefix);
extern void pushStop(TSQueryParserState state);
extern void pushOperator(TSQueryParserState state, int8 oper);

/*
 * parse plain text and lexize words
 */
typedef struct
{
	uint16		len;
	uint16		nvariant;
	union
	{
		uint16		pos;

		/*
		 * When apos array is used, apos[0] is the number of elements in the
		 * array (excluding apos[0]), and alen is the allocated size of the
		 * array.
		 */
		uint16	   *apos;
	}			pos;
	uint16		flags;			/* currently, only TSL_PREFIX */
	char	   *word;
	uint32		alen;
} ParsedWord;

typedef struct
{
	ParsedWord *words;
	int32		lenwords;
	int32		curwords;
	int32		pos;
} ParsedText;

extern void parsetext(Oid cfgId, ParsedText *prs, char *buf, int32 buflen);

/*
 * headline framework, flow in common to generate:
 *	1 parse text with hlparsetext
 *	2 parser-specific function to find part
 *	3 generateHeadline to generate result text
 */

extern void hlparsetext(Oid cfgId, HeadlineParsedText *prs, TSQuery query,
			char *buf, int32 buflen);
extern text *generateHeadline(HeadlineParsedText *prs);

/*
 * Common check function for tsvector @@ tsquery
 */
extern bool TS_execute(QueryItem *curitem, void *checkval, bool calcnot,
		   bool (*chkcond) (void *checkval, QueryOperand *val));
extern bool tsquery_requires_match(QueryItem *curitem);

/*
 * to_ts* - text transformation to tsvector, tsquery
 */
extern TSVector make_tsvector(ParsedText *prs);
extern int32 tsCompareString(char *a, int lena, char *b, int lenb, bool prefix);

extern Datum to_tsvector_byid(PG_FUNCTION_ARGS);
extern Datum to_tsvector(PG_FUNCTION_ARGS);
extern Datum to_tsquery_byid(PG_FUNCTION_ARGS);
extern Datum to_tsquery(PG_FUNCTION_ARGS);
extern Datum plainto_tsquery_byid(PG_FUNCTION_ARGS);
extern Datum plainto_tsquery(PG_FUNCTION_ARGS);

/*
 * GiST support function
 */

extern Datum gtsvector_compress(PG_FUNCTION_ARGS);
extern Datum gtsvector_decompress(PG_FUNCTION_ARGS);
extern Datum gtsvector_consistent(PG_FUNCTION_ARGS);
extern Datum gtsvector_union(PG_FUNCTION_ARGS);
extern Datum gtsvector_same(PG_FUNCTION_ARGS);
extern Datum gtsvector_penalty(PG_FUNCTION_ARGS);
extern Datum gtsvector_picksplit(PG_FUNCTION_ARGS);

/*
 * IO functions for pseudotype gtsvector
 * used internally in tsvector GiST opclass
 */
extern Datum gtsvectorin(PG_FUNCTION_ARGS);
extern Datum gtsvectorout(PG_FUNCTION_ARGS);

/*
 * GIN support function
 */

extern Datum gin_extract_tsvector(PG_FUNCTION_ARGS);
extern Datum gin_cmp_tslexeme(PG_FUNCTION_ARGS);
extern Datum gin_cmp_prefix(PG_FUNCTION_ARGS);
extern Datum gin_extract_tsquery(PG_FUNCTION_ARGS);
extern Datum gin_tsquery_consistent(PG_FUNCTION_ARGS);
extern Datum gin_tsquery_triconsistent(PG_FUNCTION_ARGS);
extern Datum gin_extract_tsvector_2args(PG_FUNCTION_ARGS);
extern Datum gin_extract_tsquery_5args(PG_FUNCTION_ARGS);
extern Datum gin_tsquery_consistent_6args(PG_FUNCTION_ARGS);

/*
 * Possible strategy numbers for indexes
 *	  TSearchStrategyNumber  - (tsvector|text) @@ tsquery
 *	  TSearchWithClassStrategyNumber  - tsvector @@@ tsquery
 */
#define TSearchStrategyNumber			1
#define TSearchWithClassStrategyNumber	2

/*
 * TSQuery Utilities
 */
extern QueryItem *clean_NOT(QueryItem *ptr, int32 *len);
extern QueryItem *clean_fakeval(QueryItem *ptr, int32 *len);

typedef struct QTNode
{
	QueryItem  *valnode;
	uint32		flags;
	int32		nchild;
	char	   *word;
	uint32		sign;
	struct QTNode **child;
} QTNode;

/* bits in QTNode.flags */
#define QTN_NEEDFREE	0x01
#define QTN_NOCHANGE	0x02
#define QTN_WORDFREE	0x04

typedef uint64 TSQuerySign;

#define TSQS_SIGLEN  (sizeof(TSQuerySign)*BITS_PER_BYTE)

#define TSQuerySignGetDatum(X)		Int64GetDatum((int64) (X))
#define DatumGetTSQuerySign(X)		((TSQuerySign) DatumGetInt64(X))
#define PG_RETURN_TSQUERYSIGN(X)	return TSQuerySignGetDatum(X)
#define PG_GETARG_TSQUERYSIGN(n)	DatumGetTSQuerySign(PG_GETARG_DATUM(n))


extern QTNode *QT2QTN(QueryItem *in, char *operand);
extern TSQuery QTN2QT(QTNode *in);
extern void QTNFree(QTNode *in);
extern void QTNSort(QTNode *in);
extern void QTNTernary(QTNode *in);
extern void QTNBinary(QTNode *in);
extern int	QTNodeCompare(QTNode *an, QTNode *bn);
extern QTNode *QTNCopy(QTNode *in);
extern void QTNClearFlags(QTNode *in, uint32 flags);
extern bool QTNEq(QTNode *a, QTNode *b);
extern TSQuerySign makeTSQuerySign(TSQuery a);
extern QTNode *findsubquery(QTNode *root, QTNode *ex, QTNode *subs,
			 bool *isfind);

/*
 * TSQuery GiST support
 */
extern Datum gtsquery_compress(PG_FUNCTION_ARGS);
extern Datum gtsquery_decompress(PG_FUNCTION_ARGS);
extern Datum gtsquery_consistent(PG_FUNCTION_ARGS);
extern Datum gtsquery_union(PG_FUNCTION_ARGS);
extern Datum gtsquery_same(PG_FUNCTION_ARGS);
extern Datum gtsquery_penalty(PG_FUNCTION_ARGS);
extern Datum gtsquery_picksplit(PG_FUNCTION_ARGS);

/*
 * Parser interface to SQL
 */
extern Datum ts_token_type_byid(PG_FUNCTION_ARGS);
extern Datum ts_token_type_byname(PG_FUNCTION_ARGS);
extern Datum ts_parse_byid(PG_FUNCTION_ARGS);
extern Datum ts_parse_byname(PG_FUNCTION_ARGS);

/*
 * Default word parser
 */

extern Datum prsd_start(PG_FUNCTION_ARGS);
extern Datum prsd_nexttoken(PG_FUNCTION_ARGS);
extern Datum prsd_end(PG_FUNCTION_ARGS);
extern Datum prsd_headline(PG_FUNCTION_ARGS);
extern Datum prsd_lextype(PG_FUNCTION_ARGS);

/*
 * Dictionary interface to SQL
 */
extern Datum ts_lexize(PG_FUNCTION_ARGS);

/*
 * Simple built-in dictionary
 */
extern Datum dsimple_init(PG_FUNCTION_ARGS);
extern Datum dsimple_lexize(PG_FUNCTION_ARGS);

/*
 * Synonym built-in dictionary
 */
extern Datum dsynonym_init(PG_FUNCTION_ARGS);
extern Datum dsynonym_lexize(PG_FUNCTION_ARGS);

/*
 * ISpell dictionary
 */
extern Datum dispell_init(PG_FUNCTION_ARGS);
extern Datum dispell_lexize(PG_FUNCTION_ARGS);

/*
 * Thesaurus
 */
extern Datum thesaurus_init(PG_FUNCTION_ARGS);
extern Datum thesaurus_lexize(PG_FUNCTION_ARGS);

/*
 * headline
 */
extern Datum ts_headline_byid_opt(PG_FUNCTION_ARGS);
extern Datum ts_headline_byid(PG_FUNCTION_ARGS);
extern Datum ts_headline(PG_FUNCTION_ARGS);
extern Datum ts_headline_opt(PG_FUNCTION_ARGS);

/*
 * current cfg
 */
extern Datum get_current_ts_config(PG_FUNCTION_ARGS);

#endif   /* _PG_TS_UTILS_H_ */
