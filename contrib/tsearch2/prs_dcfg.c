/*
 * Simple config parser
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "postgres.h"

#include "dict.h"
#include "common.h"

#define CS_WAITKEY	0
#define CS_INKEY	1
#define CS_WAITEQ	2
#define CS_WAITVALUE	3
#define CS_INVALUE	4
#define CS_IN2VALUE 5
#define CS_WAITDELIM	6
#define CS_INESC	7
#define CS_IN2ESC	8

static char *
nstrdup(char *ptr, int len)
{
	char	   *res = palloc(len + 1),
			   *cptr;

	memcpy(res, ptr, len);
	res[len] = '\0';
	cptr = ptr = res;
	while (*ptr)
	{
		if (*ptr == '\\')
			ptr++;
		*cptr = *ptr;
		ptr++;
		cptr++;
	}
	*cptr = '\0';

	return res;
}

void
parse_cfgdict(text *in, Map ** m)
{
	Map		   *mptr;
	char	   *ptr = VARDATA(in),
			   *begin = NULL;
	char		num = 0;
	int			state = CS_WAITKEY;

	while (ptr - VARDATA(in) < VARSIZE(in) - VARHDRSZ)
	{
		if (*ptr == ',')
			num++;
		ptr++;
	}

	*m = mptr = (Map *) palloc(sizeof(Map) * (num + 2));
	memset(mptr, 0, sizeof(Map) * (num + 2));
	ptr = VARDATA(in);
	while (ptr - VARDATA(in) < VARSIZE(in) - VARHDRSZ)
	{
		if (state == CS_WAITKEY)
		{
			if (isalpha(*ptr))
			{
				begin = ptr;
				state = CS_INKEY;
			}
			else if (!isspace(*ptr))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error"),
					 errdetail("Syntax error in position %d near \"%c\"",
							   (int) (ptr - VARDATA(in)), *ptr)));
		}
		else if (state == CS_INKEY)
		{
			if (isspace(*ptr))
			{
				mptr->key = nstrdup(begin, ptr - begin);
				state = CS_WAITEQ;
			}
			else if (*ptr == '=')
			{
				mptr->key = nstrdup(begin, ptr - begin);
				state = CS_WAITVALUE;
			}
			else if (!isalpha(*ptr))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error"),
					 errdetail("Syntax error in position %d near \"%c\"",
							   (int) (ptr - VARDATA(in)), *ptr)));
		}
		else if (state == CS_WAITEQ)
		{
			if (*ptr == '=')
				state = CS_WAITVALUE;
			else if (!isspace(*ptr))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error"),
					 errdetail("Syntax error in position %d near \"%c\"",
							   (int) (ptr - VARDATA(in)), *ptr)));
		}
		else if (state == CS_WAITVALUE)
		{
			if (*ptr == '"')
			{
				begin = ptr + 1;
				state = CS_INVALUE;
			}
			else if (!isspace(*ptr))
			{
				begin = ptr;
				state = CS_IN2VALUE;
			}
		}
		else if (state == CS_INVALUE)
		{
			if (*ptr == '"')
			{
				mptr->value = nstrdup(begin, ptr - begin);
				mptr++;
				state = CS_WAITDELIM;
			}
			else if (*ptr == '\\')
				state = CS_INESC;
		}
		else if (state == CS_IN2VALUE)
		{
			if (isspace(*ptr) || *ptr == ',')
			{
				mptr->value = nstrdup(begin, ptr - begin);
				mptr++;
				state = (*ptr == ',') ? CS_WAITKEY : CS_WAITDELIM;
			}
			else if (*ptr == '\\')
				state = CS_INESC;
		}
		else if (state == CS_WAITDELIM)
		{
			if (*ptr == ',')
				state = CS_WAITKEY;
			else if (!isspace(*ptr))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error"),
					 errdetail("Syntax error in position %d near \"%c\"",
							   (int) (ptr - VARDATA(in)), *ptr)));
		}
		else if (state == CS_INESC)
			state = CS_INVALUE;
		else if (state == CS_IN2ESC)
			state = CS_IN2VALUE;
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("bad parser state"),
					 errdetail("%d at position %d near \"%c\"",
							   state, (int) (ptr - VARDATA(in)), *ptr)));
		ptr++;
	}

	if (state == CS_IN2VALUE)
	{
		mptr->value = nstrdup(begin, ptr - begin);
		mptr++;
	}
	else if (!(state == CS_WAITDELIM || state == CS_WAITKEY))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("unexpected end of line")));
}
