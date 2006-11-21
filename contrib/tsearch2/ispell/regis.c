#include "regis.h"
#include "ts_locale.h"
#include "common.h"

bool
RS_isRegis(const char *str)
{
	while (str && *str)
	{
		if (t_isalpha(str) ||
			t_iseq(str, '[') ||
			t_iseq(str, ']') ||
			t_iseq(str, '^'))
			str += pg_mblen(str);
		else
			return false;
	}
	return true;
}

#define RS_IN_ONEOF 1
#define RS_IN_ONEOF_IN	2
#define RS_IN_NONEOF	3
#define RS_IN_WAIT	4

static RegisNode *
newRegisNode(RegisNode * prev, int len)
{
	RegisNode  *ptr;

	ptr = (RegisNode *) malloc(RNHDRSZ + len + 1);
	if (!ptr)
		ts_error(ERROR, "No memory");
	memset(ptr, 0, RNHDRSZ + len + 1);
	if (prev)
		prev->next = ptr;
	return ptr;
}

void
RS_compile(Regis * r, bool issuffix, char *str)
{
	int			len = strlen(str);
	int			state = RS_IN_WAIT;
	char	   *c = (char *) str;
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
			else
				ts_error(ERROR, "Error in regis: %s", str);
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
			else
				ts_error(ERROR, "Error in regis: %s", str);
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
			else
				ts_error(ERROR, "Error in regis: %s", str);
		}
		else
			ts_error(ERROR, "Internal error in RS_compile: %d", state);
		c += pg_mblen(c);
	}

	ptr = r->node;
	while (ptr)
	{
		r->nchar++;
		ptr = ptr->next;
	}
}

void
RS_free(Regis * r)
{
	RegisNode  *ptr = r->node,
			   *tmp;

	while (ptr)
	{
		tmp = ptr->next;
		free(ptr);
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
RS_execute(Regis * r, char *str)
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
		return false;

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
				ts_error(ERROR, "RS_execute: Unknown type node: %d\n", ptr->type);
		}
		ptr = ptr->next;
		c += pg_mblen(c);
	}

	return true;
}
