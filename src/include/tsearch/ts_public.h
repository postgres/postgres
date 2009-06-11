/*-------------------------------------------------------------------------
 *
 * ts_public.h
 *	  Public interface to various tsearch modules, such as
 *	  parsers and dictionaries.
 *
 * Copyright (c) 1998-2009, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/tsearch/ts_public.h,v 1.14 2009/06/11 14:49:12 momjian Exp $
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
	char	   *word;
	QueryOperand *item;
} HeadlineWordEntry;

typedef struct
{
	HeadlineWordEntry *words;
	int4		lenwords;
	int4		curwords;
	char	   *startsel;
	char	   *stopsel;
	char	   *fragdelim;
	int2		startsellen;
	int2		stopsellen;
	int2		fragdelimlen;
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
	/*
	 * number of variant of split word , for example Word 'fotballklubber'
	 * (norwegian) has two varian to split: ( fotball, klubb ) and ( fot,
	 * ball, klubb ). So, dictionary should return: nvariant lexeme 1 fotball
	 * 1	  klubb 2	   fot 2	  ball 2	  klubb
	 */
	uint16		nvariant;

	uint16		flags;

	/* C-string */
	char	   *lexeme;
} TSLexeme;

#define TSL_ADDPOS		0x01
#define TSL_PREFIX		0x02

/*
 * Struct for supporting complex dictionaries like thesaurus.
 * 4th argument for dictlexize method is a pointer to this
 */
typedef struct
{
	bool		isend;			/* in: marks for lexize_info about text end is
								 * reached */
	bool		getnext;		/* out: dict wants next lexeme */
	void	   *private;		/* internal dict state between calls with
								 * getnext == true */
} DictSubState;

#endif   /* _PG_TS_PUBLIC_H_ */
