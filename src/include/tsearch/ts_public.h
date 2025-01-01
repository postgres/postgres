/*-------------------------------------------------------------------------
 *
 * ts_public.h
 *	  Public interface to various tsearch modules, such as
 *	  parsers and dictionaries.
 *
 * Copyright (c) 1998-2025, PostgreSQL Global Development Group
 *
 * src/include/tsearch/ts_public.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PG_TS_PUBLIC_H_
#define _PG_TS_PUBLIC_H_

#include "tsearch/ts_type.h"

/*
 * Parser's framework
 */

/*
 * returning type for prslextype method of parser
 */
typedef struct
{
	int			lexid;
	char	   *alias;
	char	   *descr;
} LexDescr;

/*
 * Interface to headline generator (tsparser's prsheadline function)
 *
 * HeadlineParsedText describes the text that is to be highlighted.
 * Some fields are passed from the core code to the prsheadline function,
 * while others are output from the prsheadline function.
 *
 * The principal data is words[], an array of HeadlineWordEntry,
 * one entry per token, of length curwords.
 * The fields of HeadlineWordEntry are:
 *
 * in, selected, replace, skip: these flags are initially zero
 * and may be set by the prsheadline function.  A consecutive group
 * of tokens marked "in" form a "fragment" to be output.
 * Such tokens may additionally be marked selected, replace, or skip
 * to modify how they are shown.  (If you set more than one of those
 * bits, you get an unspecified one of those behaviors.)
 *
 * type, len, pos, word: filled by core code to describe the token.
 *
 * item: if the token matches any operand of the tsquery of interest,
 * a pointer to such an operand.  (If there are multiple matching
 * operands, we generate extra copies of the HeadlineWordEntry to hold
 * all the pointers.  The extras are marked with repeated = 1 and should
 * be ignored except for checking the item pointer.)
 */
typedef struct
{
	uint32		selected:1,		/* token is to be highlighted */
				in:1,			/* token is part of headline */
				replace:1,		/* token is to be replaced with a space */
				repeated:1,		/* duplicate entry to hold item pointer */
				skip:1,			/* token is to be skipped (not output) */
				unused:3,		/* available bits */
				type:8,			/* parser's token category */
				len:16;			/* length of token */
	WordEntryPos pos;			/* position of token */
	char	   *word;			/* text of token (not null-terminated) */
	QueryOperand *item;			/* a matching query operand, or NULL if none */
} HeadlineWordEntry;

typedef struct
{
	/* Fields filled by core code before calling prsheadline function: */
	HeadlineWordEntry *words;
	int32		lenwords;		/* allocated length of words[] */
	int32		curwords;		/* current number of valid entries */
	int32		vectorpos;		/* used by ts_parse.c in filling pos fields */

	/* The prsheadline function must fill these fields: */
	/* Strings for marking selected tokens and separating fragments: */
	char	   *startsel;		/* palloc'd strings */
	char	   *stopsel;
	char	   *fragdelim;
	int16		startsellen;	/* lengths of strings */
	int16		stopsellen;
	int16		fragdelimlen;
} HeadlineParsedText;

/*
 * Common useful things for tsearch subsystem
 */
extern char *get_tsearch_config_filename(const char *basename,
										 const char *extension);

/*
 * Often useful stopword list management
 */
typedef struct
{
	int			len;
	char	  **stop;
} StopList;

extern void readstoplist(const char *fname, StopList *s,
						 char *(*wordop) (const char *, size_t, Oid));
extern bool searchstoplist(StopList *s, char *key);

/*
 * Interface with dictionaries
 */

/* return struct for any lexize function */
typedef struct
{
	/*----------
	 * Number of current variant of split word.  For example the Norwegian
	 * word 'fotballklubber' has two variants to split: ( fotball, klubb )
	 * and ( fot, ball, klubb ). So, dictionary should return:
	 *
	 * nvariant    lexeme
	 *	   1	   fotball
	 *	   1	   klubb
	 *	   2	   fot
	 *	   2	   ball
	 *	   2	   klubb
	 *
	 * In general, a TSLexeme will be considered to belong to the same split
	 * variant as the previous one if they have the same nvariant value.
	 * The exact values don't matter, only changes from one lexeme to next.
	 *----------
	 */
	uint16		nvariant;

	uint16		flags;			/* See flag bits below */

	char	   *lexeme;			/* C string */
} TSLexeme;

/* Flag bits that can appear in TSLexeme.flags */
#define TSL_ADDPOS		0x01
#define TSL_PREFIX		0x02
#define TSL_FILTER		0x04

/*
 * Struct for supporting complex dictionaries like thesaurus.
 * 4th argument for dictlexize method is a pointer to this
 */
typedef struct
{
	bool		isend;			/* in: marks for lexize_info about text end is
								 * reached */
	bool		getnext;		/* out: dict wants next lexeme */
	void	   *private_state;	/* internal dict state between calls with
								 * getnext == true */
} DictSubState;

#endif							/* _PG_TS_PUBLIC_H_ */
