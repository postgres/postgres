/*-------------------------------------------------------------------------
 *
 * ts_utils.h
 *	  helper utilities for tsearch
 *
 * Copyright (c) 1998-2016, PostgreSQL Global Development Group
 *
 * src/include/tsearch/ts_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PG_TS_UTILS_H_
#define _PG_TS_UTILS_H_

#include "nodes/pg_list.h"
#include "tsearch/ts_public.h"
#include "tsearch/ts_type.h"

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
extern void pushOperator(TSQueryParserState state, int8 oper, int16 distance);

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
 * TSQuery execution support
 *
 * TS_execute() executes a tsquery against data that can be represented in
 * various forms.  The TSExecuteCallback callback function is called to check
 * whether a given primitive tsquery value is matched in the data.
 */

/*
 * struct ExecPhraseData is passed to a TSExecuteCallback function if we need
 * lexeme position data (because of a phrase-match operator in the tsquery).
 * The callback should fill in position data when it returns true (success).
 * If it cannot return position data, it may leave "data" unchanged, but
 * then the caller of TS_execute() must pass the TS_EXEC_PHRASE_NO_POS flag
 * and must arrange for a later recheck with position data available.
 *
 * The reported lexeme positions must be sorted and unique.  Callers must only
 * consult the position bits of the pos array, ie, WEP_GETPOS(data->pos[i]).
 * This allows the returned "pos" to point directly to the WordEntryPos
 * portion of a tsvector value.  If "allocated" is true then the pos array
 * is palloc'd workspace and caller may free it when done.
 *
 * "negate" means that the pos array contains positions where the query does
 * not match, rather than positions where it does.  "width" is positive when
 * the match is wider than one lexeme.  Neither of these fields normally need
 * to be touched by TSExecuteCallback functions; they are used for
 * phrase-search processing within TS_execute.
 *
 * All fields of the ExecPhraseData struct are initially zeroed by caller.
 */
typedef struct ExecPhraseData
{
	int			npos;			/* number of positions reported */
	bool		allocated;		/* pos points to palloc'd data? */
	bool		negate;			/* positions are where query is NOT matched */
	WordEntryPos *pos;			/* ordered, non-duplicate lexeme positions */
	int			width;			/* width of match in lexemes, less 1 */
} ExecPhraseData;

/*
 * Signature for TSQuery lexeme check functions
 *
 * arg: opaque value passed through from caller of TS_execute
 * val: lexeme to test for presence of
 * data: to be filled with lexeme positions; NULL if position data not needed
 *
 * Return TRUE if lexeme is present in data, else FALSE.  If data is not
 * NULL, it should be filled with lexeme positions, but function can leave
 * it as zeroes if position data is not available.
 */
typedef bool (*TSExecuteCallback) (void *arg, QueryOperand *val,
											   ExecPhraseData *data);

/*
 * Flag bits for TS_execute
 */
#define TS_EXEC_EMPTY			(0x00)
/*
 * If TS_EXEC_CALC_NOT is not set, then NOT expressions are automatically
 * evaluated to be true.  Useful in cases where NOT cannot be accurately
 * computed (GiST) or it isn't important (ranking).  From TS_execute's
 * perspective, !CALC_NOT means that the TSExecuteCallback function might
 * return false-positive indications of a lexeme's presence.
 */
#define TS_EXEC_CALC_NOT		(0x01)
/*
 * If TS_EXEC_PHRASE_NO_POS is set, allow OP_PHRASE to be executed lossily
 * in the absence of position information: a TRUE result indicates that the
 * phrase might be present.  Without this flag, OP_PHRASE always returns
 * false if lexeme position information is not available.
 */
#define TS_EXEC_PHRASE_NO_POS	(0x02)
/* Obsolete spelling of TS_EXEC_PHRASE_NO_POS: */
#define TS_EXEC_PHRASE_AS_AND	TS_EXEC_PHRASE_NO_POS

extern bool TS_execute(QueryItem *curitem, void *arg, uint32 flags,
		   TSExecuteCallback chkcond);
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
extern Datum phraseto_tsquery_byid(PG_FUNCTION_ARGS);
extern Datum phraseto_tsquery(PG_FUNCTION_ARGS);

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
extern Datum gtsvector_consistent_oldsig(PG_FUNCTION_ARGS);

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
extern Datum gin_extract_tsquery_oldsig(PG_FUNCTION_ARGS);
extern Datum gin_tsquery_consistent_oldsig(PG_FUNCTION_ARGS);

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
extern TSQuery cleanup_tsquery_stopwords(TSQuery in);

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
extern Datum gtsquery_consistent_oldsig(PG_FUNCTION_ARGS);

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
