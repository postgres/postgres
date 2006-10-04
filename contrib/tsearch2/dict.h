/* $PostgreSQL: pgsql/contrib/tsearch2/dict.h,v 1.8 2006/10/04 00:29:46 momjian Exp $ */

#ifndef __DICT_H__
#define __DICT_H__
#include "postgres.h"
#include "fmgr.h"
#include "ts_cfg.h"

typedef struct
{
	int			len;
	char	  **stop;
	char	   *(*wordop) (char *);
}	StopList;

void		sortstoplist(StopList * s);
void		freestoplist(StopList * s);
void		readstoplist(text *in, StopList * s);
bool		searchstoplist(StopList * s, char *key);

typedef struct
{
	Oid			dict_id;
	FmgrInfo	lexize_info;
	void	   *dictionary;
}	DictInfo;

void		init_dict(Oid id, DictInfo * dict);
DictInfo   *finddict(Oid id);
Oid			name2id_dict(text *name);
void		reset_dict(void);

typedef struct
{
	bool		isend;			/* in: marks for lexize_info about text end is
								 * reached */
	bool		getnext;		/* out: dict wants next lexeme */
	void	   *private;		/* internal dict state between calls with
								 * getnext == true */
}	DictSubState;

/* simple parser of cfg string */
typedef struct
{
	char	   *key;
	char	   *value;
}	Map;

void		parse_cfgdict(text *in, Map ** m);

/* return struct for any lexize function */
typedef struct
{
	/*
	 * number of variant of split word , for example Word 'fotballklubber'
	 * (norwegian) has two varian to split: ( fotball, klubb ) and ( fot,
	 * ball, klubb ). So, dictionary should return: nvariant lexeme 1
	 * fotball 1	  klubb 2	   fot 2	  ball 2	  klubb
	 */
	uint16		nvariant;

	uint16		flags;

	/* C-string */
	char	   *lexeme;
}	TSLexeme;

#define TSL_ADDPOS		0x01


/*
 * Lexize subsystem
 */

typedef struct ParsedLex
{
	int			type;
	char	   *lemm;
	int			lenlemm;
	bool		resfollow;
	struct ParsedLex *next;
}	ParsedLex;

typedef struct ListParsedLex
{
	ParsedLex  *head;
	ParsedLex  *tail;
}	ListParsedLex;

typedef struct
{
	TSCfgInfo  *cfg;
	Oid			curDictId;
	int			posDict;
	DictSubState dictState;
	ParsedLex  *curSub;
	ListParsedLex towork;		/* current list to work */
	ListParsedLex waste;		/* list of lexemes that already lexized */

	/*
	 * fields to store last variant to lexize (basically, thesaurus or similar
	 * to, which wants	several lexemes
	 */

	ParsedLex  *lastRes;
	TSLexeme   *tmpRes;
}	LexizeData;


void		LexizeInit(LexizeData * ld, TSCfgInfo * cfg);
void		LexizeAddLemm(LexizeData * ld, int type, char *lemm, int lenlemm);
TSLexeme   *LexizeExec(LexizeData * ld, ParsedLex ** correspondLexem);

#endif
