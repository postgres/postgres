/*-------------------------------------------------------------------------
 *
 * ts_public.h
 *	  Public interface to various tsearch modules, such as
 *	  parsers and dictionaries.
 *
 * Copyright (c) 1998-2016, PostgreSQL Global Development Group
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
 * Interface to headline generator
 */
typedef struct
{
	uint32		selected:1,
				in:1,
				replace:1,
				repeated:1,
				skip:1,
				unused:3,
				type:8,
				len:16;
	WordEntryPos pos;
	char	   *word;
	QueryOperand *item;
} HeadlineWordEntry;

typedef struct
{
	HeadlineWordEntry *words;
	int32		lenwords;
	int32		curwords;
	int32		vectorpos;		/* positions a-la tsvector */
	char	   *startsel;
	char	   *stopsel;
	char	   *fragdelim;
	int16		startsellen;
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
			 char *(*wordop) (const char *));
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

#endif   /* _PG_TS_PUBLIC_H_ */
