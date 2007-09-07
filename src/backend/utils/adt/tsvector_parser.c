/*-------------------------------------------------------------------------
 *
 * tsvector_parser.c
 *	  Parser for tsvector
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/tsvector_parser.c,v 1.1 2007/09/07 15:09:56 teodor Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqformat.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_utils.h"
#include "utils/memutils.h"

struct TSVectorParseStateData
{
	char   *prsbuf;
	char   *word;		/* buffer to hold the current word */
	int		len;		/* size in bytes allocated for 'word' */
	bool	oprisdelim;
};

/*
 * Initializes parser for the input string. If oprisdelim is set, the
 * following characters are treated as delimiters in addition to whitespace:
 * ! | & ( )
 */
TSVectorParseState
init_tsvector_parser(char *input, bool oprisdelim)
{
	TSVectorParseState state;

	state = (TSVectorParseState) palloc(sizeof(struct TSVectorParseStateData));
	state->prsbuf = input;
	state->len = 32;
	state->word = (char *) palloc(state->len);
	state->oprisdelim = oprisdelim;

	return state;
}

/*
 * Reinitializes parser for parsing 'input', instead of previous input.
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

#define RESIZEPRSBUF \
do { \
	if ( curpos - state->word + pg_database_encoding_max_length() >= state->len ) \
	{ \
		int clen = curpos - state->word; \
		state->len *= 2; \
		state->word = (char*)repalloc( (void*)state->word, state->len ); \
		curpos = state->word + clen; \
	} \
} while (0)


#define ISOPERATOR(x)	( pg_mblen(x)==1 && ( *(x)=='!' || *(x)=='&' || *(x)=='|' || *(x)=='(' || *(x)==')' ) )

/* Fills the output parameters, and returns true */
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

/*
 * Get next token from string being parsed. Returns false if
 * end of input string is reached, otherwise strval, lenval, pos_ptr
 * and poslen output parameters are filled in:
 * 
 * *strval 		token
 * *lenval 		length of*strval
 * *pos_ptr		pointer to a palloc'd array of positions and weights
 * 				associated with the token. If the caller is not interested
 *				in the information, NULL can be supplied. Otherwise
 *				the caller is responsible for pfreeing the array.
 * *poslen		number of elements in *pos_ptr
 */
bool
gettoken_tsvector(TSVectorParseState state, 
				  char **strval, int *lenval,
				  WordEntryPos **pos_ptr, int *poslen,
				  char **endptr)
{
	int	oldstate	= 0;
	char *curpos	= state->word;
	int	statecode	= WAITWORD;

	/* pos is for collecting the comma delimited list of positions followed
	 * by the actual token. 
	 */
	WordEntryPos *pos = NULL;
	int npos		= 0; /* elements of pos used */
	int posalen		= 0; /* allocated size of pos */

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
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error in tsvector")));
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
						 errmsg("there is no escaped character")));
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
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsvector")));
				*(curpos) = '\0';
				RETURN_TOKEN;
			}
			else if (t_iseq(state->prsbuf, ':'))
			{
				if (curpos == state->word)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsvector")));
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
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error in tsvector")));
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
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsvector")));
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
				if (WEP_GETPOS(pos[npos - 1]) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("wrong position info in tsvector")));
				WEP_SETWEIGHT(pos[npos - 1], 0);
				statecode = WAITPOSDELIM;
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error in tsvector")));
		}
		else if (statecode == WAITPOSDELIM)
		{
			if (t_iseq(state->prsbuf, ','))
				statecode = INPOSINFO;
			else if (t_iseq(state->prsbuf, 'a') || t_iseq(state->prsbuf, 'A') || t_iseq(state->prsbuf, '*'))
			{
				if (WEP_GETWEIGHT(pos[npos - 1]))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsvector")));
				WEP_SETWEIGHT(pos[npos - 1], 3);
			}
			else if (t_iseq(state->prsbuf, 'b') || t_iseq(state->prsbuf, 'B'))
			{
				if (WEP_GETWEIGHT(pos[npos - 1]))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsvector")));
				WEP_SETWEIGHT(pos[npos - 1], 2);
			}
			else if (t_iseq(state->prsbuf, 'c') || t_iseq(state->prsbuf, 'C'))
			{
				if (WEP_GETWEIGHT(pos[npos - 1]))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsvector")));
				WEP_SETWEIGHT(pos[npos - 1], 1);
			}
			else if (t_iseq(state->prsbuf, 'd') || t_iseq(state->prsbuf, 'D'))
			{
				if (WEP_GETWEIGHT(pos[npos - 1]))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsvector")));
				WEP_SETWEIGHT(pos[npos - 1], 0);
			}
			else if (t_isspace(state->prsbuf) ||
					 *(state->prsbuf) == '\0')
				RETURN_TOKEN;
			else if (!t_isdigit(state->prsbuf))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error in tsvector")));
		}
		else					/* internal error */
			elog(ERROR, "internal error in gettoken_tsvector");

		/* get next char */
		state->prsbuf += pg_mblen(state->prsbuf);
	}

	return false;
}
