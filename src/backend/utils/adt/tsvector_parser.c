/*-------------------------------------------------------------------------
 *
 * tsvector_parser.c
 *	  Parser for tsvector
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/tsvector_parser.c,v 1.6 2008/01/01 19:45:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqformat.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_utils.h"
#include "utils/memutils.h"


/*
 * Private state of tsvector parser.  Note that tsquery also uses this code to
 * parse its input, hence the boolean flags.  The two flags are both true or
 * both false in current usage, but we keep them separate for clarity.
 * is_tsquery affects *only* the content of error messages.
 */
struct TSVectorParseStateData
{
	char	   *prsbuf;			/* next input character */
	char	   *bufstart;		/* whole string (used only for errors) */
	char	   *word;			/* buffer to hold the current word */
	int			len;			/* size in bytes allocated for 'word' */
	int			eml;			/* max bytes per character */
	bool		oprisdelim;		/* treat ! | * ( ) as delimiters? */
	bool		is_tsquery;		/* say "tsquery" not "tsvector" in errors? */
};


/*
 * Initializes parser for the input string. If oprisdelim is set, the
 * following characters are treated as delimiters in addition to whitespace:
 * ! | & ( )
 */
TSVectorParseState
init_tsvector_parser(char *input, bool oprisdelim, bool is_tsquery)
{
	TSVectorParseState state;

	state = (TSVectorParseState) palloc(sizeof(struct TSVectorParseStateData));
	state->prsbuf = input;
	state->bufstart = input;
	state->len = 32;
	state->word = (char *) palloc(state->len);
	state->eml = pg_database_encoding_max_length();
	state->oprisdelim = oprisdelim;
	state->is_tsquery = is_tsquery;

	return state;
}

/*
 * Reinitializes parser to parse 'input', instead of previous input.
 */
void
reset_tsvector_parser(TSVectorParseState state, char *input)
{
	state->prsbuf = input;
}

/*
 * Shuts down a tsvector parser.
 */
void
close_tsvector_parser(TSVectorParseState state)
{
	pfree(state->word);
	pfree(state);
}

/* increase the size of 'word' if needed to hold one more character */
#define RESIZEPRSBUF \
do { \
	int clen = curpos - state->word; \
	if ( clen + state->eml >= state->len ) \
	{ \
		state->len *= 2; \
		state->word = (char *) repalloc(state->word, state->len); \
		curpos = state->word + clen; \
	} \
} while (0)

#define ISOPERATOR(x)	( pg_mblen(x)==1 && ( *(x)=='!' || *(x)=='&' || *(x)=='|' || *(x)=='(' || *(x)==')' ) )

/* Fills gettoken_tsvector's output parameters, and returns true */
#define RETURN_TOKEN \
do { \
	if (pos_ptr != NULL) \
	{ \
		*pos_ptr = pos; \
		*poslen = npos; \
	} \
	else if (pos != NULL) \
		pfree(pos); \
	\
	if (strval != NULL) \
		*strval = state->word; \
	if (lenval != NULL) \
		*lenval = curpos - state->word; \
	if (endptr != NULL) \
		*endptr = state->prsbuf; \
	return true; \
} while(0)


/* State codes used in gettoken_tsvector */
#define WAITWORD		1
#define WAITENDWORD		2
#define WAITNEXTCHAR	3
#define WAITENDCMPLX	4
#define WAITPOSINFO		5
#define INPOSINFO		6
#define WAITPOSDELIM	7
#define WAITCHARCMPLX	8

#define PRSSYNTAXERROR prssyntaxerror(state)

static void
prssyntaxerror(TSVectorParseState state)
{
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 state->is_tsquery ?
			 errmsg("syntax error in tsquery: \"%s\"", state->bufstart) :
			 errmsg("syntax error in tsvector: \"%s\"", state->bufstart)));
}


/*
 * Get next token from string being parsed. Returns true if successful,
 * false if end of input string is reached.  On success, these output
 * parameters are filled in:
 *
 * *strval		pointer to token
 * *lenval		length of *strval
 * *pos_ptr		pointer to a palloc'd array of positions and weights
 *				associated with the token. If the caller is not interested
 *				in the information, NULL can be supplied. Otherwise
 *				the caller is responsible for pfreeing the array.
 * *poslen		number of elements in *pos_ptr
 * *endptr		scan resumption point
 *
 * Pass NULL for unwanted output parameters.
 */
bool
gettoken_tsvector(TSVectorParseState state,
				  char **strval, int *lenval,
				  WordEntryPos **pos_ptr, int *poslen,
				  char **endptr)
{
	int			oldstate = 0;
	char	   *curpos = state->word;
	int			statecode = WAITWORD;

	/*
	 * pos is for collecting the comma delimited list of positions followed by
	 * the actual token.
	 */
	WordEntryPos *pos = NULL;
	int			npos = 0;		/* elements of pos used */
	int			posalen = 0;	/* allocated size of pos */

	while (1)
	{
		if (statecode == WAITWORD)
		{
			if (*(state->prsbuf) == '\0')
				return false;
			else if (t_iseq(state->prsbuf, '\''))
				statecode = WAITENDCMPLX;
			else if (t_iseq(state->prsbuf, '\\'))
			{
				statecode = WAITNEXTCHAR;
				oldstate = WAITENDWORD;
			}
			else if (state->oprisdelim && ISOPERATOR(state->prsbuf))
				PRSSYNTAXERROR;
			else if (!t_isspace(state->prsbuf))
			{
				COPYCHAR(curpos, state->prsbuf);
				curpos += pg_mblen(state->prsbuf);
				statecode = WAITENDWORD;
			}
		}
		else if (statecode == WAITNEXTCHAR)
		{
			if (*(state->prsbuf) == '\0')
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("there is no escaped character: \"%s\"",
								state->bufstart)));
			else
			{
				RESIZEPRSBUF;
				COPYCHAR(curpos, state->prsbuf);
				curpos += pg_mblen(state->prsbuf);
				Assert(oldstate != 0);
				statecode = oldstate;
			}
		}
		else if (statecode == WAITENDWORD)
		{
			if (t_iseq(state->prsbuf, '\\'))
			{
				statecode = WAITNEXTCHAR;
				oldstate = WAITENDWORD;
			}
			else if (t_isspace(state->prsbuf) || *(state->prsbuf) == '\0' ||
					 (state->oprisdelim && ISOPERATOR(state->prsbuf)))
			{
				RESIZEPRSBUF;
				if (curpos == state->word)
					PRSSYNTAXERROR;
				*(curpos) = '\0';
				RETURN_TOKEN;
			}
			else if (t_iseq(state->prsbuf, ':'))
			{
				if (curpos == state->word)
					PRSSYNTAXERROR;
				*(curpos) = '\0';
				if (state->oprisdelim)
					RETURN_TOKEN;
				else
					statecode = INPOSINFO;
			}
			else
			{
				RESIZEPRSBUF;
				COPYCHAR(curpos, state->prsbuf);
				curpos += pg_mblen(state->prsbuf);
			}
		}
		else if (statecode == WAITENDCMPLX)
		{
			if (t_iseq(state->prsbuf, '\''))
			{
				statecode = WAITCHARCMPLX;
			}
			else if (t_iseq(state->prsbuf, '\\'))
			{
				statecode = WAITNEXTCHAR;
				oldstate = WAITENDCMPLX;
			}
			else if (*(state->prsbuf) == '\0')
				PRSSYNTAXERROR;
			else
			{
				RESIZEPRSBUF;
				COPYCHAR(curpos, state->prsbuf);
				curpos += pg_mblen(state->prsbuf);
			}
		}
		else if (statecode == WAITCHARCMPLX)
		{
			if (t_iseq(state->prsbuf, '\''))
			{
				RESIZEPRSBUF;
				COPYCHAR(curpos, state->prsbuf);
				curpos += pg_mblen(state->prsbuf);
				statecode = WAITENDCMPLX;
			}
			else
			{
				RESIZEPRSBUF;
				*(curpos) = '\0';
				if (curpos == state->word)
					PRSSYNTAXERROR;
				if (state->oprisdelim)
				{
					/* state->prsbuf+=pg_mblen(state->prsbuf); */
					RETURN_TOKEN;
				}
				else
					statecode = WAITPOSINFO;
				continue;		/* recheck current character */
			}
		}
		else if (statecode == WAITPOSINFO)
		{
			if (t_iseq(state->prsbuf, ':'))
				statecode = INPOSINFO;
			else
				RETURN_TOKEN;
		}
		else if (statecode == INPOSINFO)
		{
			if (t_isdigit(state->prsbuf))
			{
				if (posalen == 0)
				{
					posalen = 4;
					pos = (WordEntryPos *) palloc(sizeof(WordEntryPos) * posalen);
					npos = 0;
				}
				else if (npos + 1 >= posalen)
				{
					posalen *= 2;
					pos = (WordEntryPos *) repalloc(pos, sizeof(WordEntryPos) * posalen);
				}
				npos++;
				WEP_SETPOS(pos[npos - 1], LIMITPOS(atoi(state->prsbuf)));
				/* we cannot get here in tsquery, so no need for 2 errmsgs */
				if (WEP_GETPOS(pos[npos - 1]) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("wrong position info in tsvector: \"%s\"",
									state->bufstart)));
				WEP_SETWEIGHT(pos[npos - 1], 0);
				statecode = WAITPOSDELIM;
			}
			else
				PRSSYNTAXERROR;
		}
		else if (statecode == WAITPOSDELIM)
		{
			if (t_iseq(state->prsbuf, ','))
				statecode = INPOSINFO;
			else if (t_iseq(state->prsbuf, 'a') || t_iseq(state->prsbuf, 'A') || t_iseq(state->prsbuf, '*'))
			{
				if (WEP_GETWEIGHT(pos[npos - 1]))
					PRSSYNTAXERROR;
				WEP_SETWEIGHT(pos[npos - 1], 3);
			}
			else if (t_iseq(state->prsbuf, 'b') || t_iseq(state->prsbuf, 'B'))
			{
				if (WEP_GETWEIGHT(pos[npos - 1]))
					PRSSYNTAXERROR;
				WEP_SETWEIGHT(pos[npos - 1], 2);
			}
			else if (t_iseq(state->prsbuf, 'c') || t_iseq(state->prsbuf, 'C'))
			{
				if (WEP_GETWEIGHT(pos[npos - 1]))
					PRSSYNTAXERROR;
				WEP_SETWEIGHT(pos[npos - 1], 1);
			}
			else if (t_iseq(state->prsbuf, 'd') || t_iseq(state->prsbuf, 'D'))
			{
				if (WEP_GETWEIGHT(pos[npos - 1]))
					PRSSYNTAXERROR;
				WEP_SETWEIGHT(pos[npos - 1], 0);
			}
			else if (t_isspace(state->prsbuf) ||
					 *(state->prsbuf) == '\0')
				RETURN_TOKEN;
			else if (!t_isdigit(state->prsbuf))
				PRSSYNTAXERROR;
		}
		else	/* internal error */
			elog(ERROR, "unrecognized state in gettoken_tsvector: %d",
				 statecode);

		/* get next char */
		state->prsbuf += pg_mblen(state->prsbuf);
	}

	return false;
}
