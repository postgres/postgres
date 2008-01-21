/*-------------------------------------------------------------------------
 *
 * regis.c
 *		Fast regex subset
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/tsearch/regis.c,v 1.4 2008/01/21 02:46:10 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "tsearch/dicts/regis.h"
#include "tsearch/ts_locale.h"

#define RS_IN_ONEOF 1
#define RS_IN_ONEOF_IN	2
#define RS_IN_NONEOF	3
#define RS_IN_WAIT	4


/*
 * Test whether a regex is of the subset supported here.
 * Keep this in sync with RS_compile!
 */
bool
RS_isRegis(const char *str)
{
	int			state = RS_IN_WAIT;
	const char *c = str;

	while (*c)
	{
		if (state == RS_IN_WAIT)
		{
			if (t_isalpha(c))
				/* okay */ ;
			else if (t_iseq(c, '['))
				state = RS_IN_ONEOF;
			else
				return false;
		}
		else if (state == RS_IN_ONEOF)
		{
			if (t_iseq(c, '^'))
				state = RS_IN_NONEOF;
			else if (t_isalpha(c))
				state = RS_IN_ONEOF_IN;
			else
				return false;
		}
		else if (state == RS_IN_ONEOF_IN || state == RS_IN_NONEOF)
		{
			if (t_isalpha(c))
				/* okay */ ;
			else if (t_iseq(c, ']'))
				state = RS_IN_WAIT;
			else
				return false;
		}
		else
			elog(ERROR, "internal error in RS_isRegis: state %d", state);
		c += pg_mblen(c);
	}

	return (state == RS_IN_WAIT);
}

static RegisNode *
newRegisNode(RegisNode *prev, int len)
{
	RegisNode  *ptr;

	ptr = (RegisNode *) palloc0(RNHDRSZ + len + 1);
	if (prev)
		prev->next = ptr;
	return ptr;
}

void
RS_compile(Regis *r, bool issuffix, const char *str)
{
	int			len = strlen(str);
	int			state = RS_IN_WAIT;
	const char *c = str;
	RegisNode  *ptr = NULL;

	memset(r, 0, sizeof(Regis));
	r->issuffix = (issuffix) ? 1 : 0;

	while (*c)
	{
		if (state == RS_IN_WAIT)
		{
			if (t_isalpha(c))
			{
				if (ptr)
					ptr = newRegisNode(ptr, len);
				else
					ptr = r->node = newRegisNode(NULL, len);
				COPYCHAR(ptr->data, c);
				ptr->type = RSF_ONEOF;
				ptr->len = pg_mblen(c);
			}
			else if (t_iseq(c, '['))
			{
				if (ptr)
					ptr = newRegisNode(ptr, len);
				else
					ptr = r->node = newRegisNode(NULL, len);
				ptr->type = RSF_ONEOF;
				state = RS_IN_ONEOF;
			}
			else				/* shouldn't get here */
				elog(ERROR, "invalid regis pattern: \"%s\"", str);
		}
		else if (state == RS_IN_ONEOF)
		{
			if (t_iseq(c, '^'))
			{
				ptr->type = RSF_NONEOF;
				state = RS_IN_NONEOF;
			}
			else if (t_isalpha(c))
			{
				COPYCHAR(ptr->data, c);
				ptr->len = pg_mblen(c);
				state = RS_IN_ONEOF_IN;
			}
			else				/* shouldn't get here */
				elog(ERROR, "invalid regis pattern: \"%s\"", str);
		}
		else if (state == RS_IN_ONEOF_IN || state == RS_IN_NONEOF)
		{
			if (t_isalpha(c))
			{
				COPYCHAR(ptr->data + ptr->len, c);
				ptr->len += pg_mblen(c);
			}
			else if (t_iseq(c, ']'))
				state = RS_IN_WAIT;
			else				/* shouldn't get here */
				elog(ERROR, "invalid regis pattern: \"%s\"", str);
		}
		else
			elog(ERROR, "internal error in RS_compile: state %d", state);
		c += pg_mblen(c);
	}

	if (state != RS_IN_WAIT)		/* shouldn't get here */
		elog(ERROR, "invalid regis pattern: \"%s\"", str);

	ptr = r->node;
	while (ptr)
	{
		r->nchar++;
		ptr = ptr->next;
	}
}

void
RS_free(Regis *r)
{
	RegisNode  *ptr = r->node,
			   *tmp;

	while (ptr)
	{
		tmp = ptr->next;
		pfree(ptr);
		ptr = tmp;
	}

	r->node = NULL;
}

#ifdef TS_USE_WIDE
static bool
mb_strchr(char *str, char *c)
{
	int			clen = pg_mblen(c),
				plen,
				i;
	char	   *ptr = str;
	bool		res = false;

	clen = pg_mblen(c);
	while (*ptr && !res)
	{
		plen = pg_mblen(ptr);
		if (plen == clen)
		{
			i = plen;
			res = true;
			while (i--)
				if (*(ptr + i) != *(c + i))
				{
					res = false;
					break;
				}
		}

		ptr += plen;
	}

	return res;
}
#else
#define mb_strchr(s,c)	( (strchr((s),*(c)) == NULL) ? false : true )
#endif


bool
RS_execute(Regis *r, char *str)
{
	RegisNode  *ptr = r->node;
	char	   *c = str;
	int			len = 0;

	while (*c)
	{
		len++;
		c += pg_mblen(c);
	}

	if (len < r->nchar)
		return 0;

	c = str;
	if (r->issuffix)
	{
		len -= r->nchar;
		while (len-- > 0)
			c += pg_mblen(c);
	}


	while (ptr)
	{
		switch (ptr->type)
		{
			case RSF_ONEOF:
				if (mb_strchr((char *) ptr->data, c) != true)
					return false;
				break;
			case RSF_NONEOF:
				if (mb_strchr((char *) ptr->data, c) == true)
					return false;
				break;
			default:
				elog(ERROR, "unrecognized regis node type: %d", ptr->type);
		}
		ptr = ptr->next;
		c += pg_mblen(c);
	}

	return true;
}
