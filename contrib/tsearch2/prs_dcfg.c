/*
 * Simple config parser
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include "postgres.h"

#include <ctype.h>

#include "dict.h"
#include "common.h"
#include "ts_locale.h"

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
		if (t_iseq(ptr, '\\'))
			ptr++;
		COPYCHAR(cptr, ptr);
		cptr += pg_mblen(ptr);
		ptr += pg_mblen(ptr);
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
		if (t_iseq(ptr, ','))
			num++;
		ptr += pg_mblen(ptr);
	}

	*m = mptr = (Map *) palloc(sizeof(Map) * (num + 2));
	memset(mptr, 0, sizeof(Map) * (num + 2));
	ptr = VARDATA(in);
	while (ptr - VARDATA(in) < VARSIZE(in) - VARHDRSZ)
	{
		if (state == CS_WAITKEY)
		{
			if (t_isalpha(ptr))
			{
				begin = ptr;
				state = CS_INKEY;
			}
			else if (!t_isspace(ptr))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error"),
						 errdetail("Syntax error in position %d.",
								   (int) (ptr - VARDATA(in)))));
		}
		else if (state == CS_INKEY)
		{
			if (t_isspace(ptr))
			{
				mptr->key = nstrdup(begin, ptr - begin);
				state = CS_WAITEQ;
			}
			else if (t_iseq(ptr, '='))
			{
				mptr->key = nstrdup(begin, ptr - begin);
				state = CS_WAITVALUE;
			}
			else if (!t_isalpha(ptr))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error"),
						 errdetail("Syntax error in position %d.",
								   (int) (ptr - VARDATA(in)))));
		}
		else if (state == CS_WAITEQ)
		{
			if (t_iseq(ptr, '='))
				state = CS_WAITVALUE;
			else if (!t_isspace(ptr))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error"),
						 errdetail("Syntax error in position %d.",
								   (int) (ptr - VARDATA(in)))));
		}
		else if (state == CS_WAITVALUE)
		{
			if (t_iseq(ptr, '"'))
			{
				begin = ptr + 1;
				state = CS_INVALUE;
			}
			else if (!t_isspace(ptr))
			{
				begin = ptr;
				state = CS_IN2VALUE;
			}
		}
		else if (state == CS_INVALUE)
		{
			if (t_iseq(ptr, '"'))
			{
				mptr->value = nstrdup(begin, ptr - begin);
				mptr++;
				state = CS_WAITDELIM;
			}
			else if (t_iseq(ptr, '\\'))
				state = CS_INESC;
		}
		else if (state == CS_IN2VALUE)
		{
			if (t_isspace(ptr) || t_iseq(ptr, ','))
			{
				mptr->value = nstrdup(begin, ptr - begin);
				mptr++;
				state = (t_iseq(ptr, ',')) ? CS_WAITKEY : CS_WAITDELIM;
			}
			else if (t_iseq(ptr, '\\'))
				state = CS_INESC;
		}
		else if (state == CS_WAITDELIM)
		{
			if (t_iseq(ptr, ','))
				state = CS_WAITKEY;
			else if (!t_isspace(ptr))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("syntax error"),
						 errdetail("Syntax error in position %d.",
								   (int) (ptr - VARDATA(in)))));
		}
		else if (state == CS_INESC)
			state = CS_INVALUE;
		else if (state == CS_IN2ESC)
			state = CS_IN2VALUE;
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("bad parser state"),
					 errdetail("%d at position %d.",
							   state, (int) (ptr - VARDATA(in)))));
		ptr += pg_mblen(ptr);
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
