#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "regis.h"
#include "common.h"

int
RS_isRegis(const char *str)
{
	unsigned char *ptr = (unsigned char *) str;

	while (ptr && *ptr)
		if (isalpha(*ptr) || *ptr == '[' || *ptr == ']' || *ptr == '^')
			ptr++;
		else
			return 0;
	return 1;
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

int
RS_compile(Regis * r, int issuffix, const char *str)
{
	int			i,
				len = strlen(str);
	int			state = RS_IN_WAIT;
	RegisNode  *ptr = NULL;

	memset(r, 0, sizeof(Regis));
	r->issuffix = (issuffix) ? 1 : 0;

	for (i = 0; i < len; i++)
	{
		unsigned char c = *(((unsigned char *) str) + i);

		if (state == RS_IN_WAIT)
		{
			if (isalpha(c))
			{
				if (ptr)
					ptr = newRegisNode(ptr, len);
				else
					ptr = r->node = newRegisNode(NULL, len);
				ptr->data[0] = c;
				ptr->type = RSF_ONEOF;
				ptr->len = 1;
			}
			else if (c == '[')
			{
				if (ptr)
					ptr = newRegisNode(ptr, len);
				else
					ptr = r->node = newRegisNode(NULL, len);
				ptr->type = RSF_ONEOF;
				state = RS_IN_ONEOF;
			}
			else
				ts_error(ERROR, "Error in regis: %s at pos %d\n", str, i + 1);
		}
		else if (state == RS_IN_ONEOF)
		{
			if (c == '^')
			{
				ptr->type = RSF_NONEOF;
				state = RS_IN_NONEOF;
			}
			else if (isalpha(c))
			{
				ptr->data[0] = c;
				ptr->len = 1;
				state = RS_IN_ONEOF_IN;
			}
			else
				ts_error(ERROR, "Error in regis: %s at pos %d\n", str, i + 1);
		}
		else if (state == RS_IN_ONEOF_IN || state == RS_IN_NONEOF)
		{
			if (isalpha(c))
			{
				ptr->data[ptr->len] = c;
				ptr->len++;
			}
			else if (c == ']')
				state = RS_IN_WAIT;
			else
				ts_error(ERROR, "Error in regis: %s at pos %d\n", str, i + 1);
		}
		else
			ts_error(ERROR, "Internal error in RS_compile: %d\n", state);
	}

	ptr = r->node;
	while (ptr)
	{
		r->nchar++;
		ptr = ptr->next;
	}

	return 0;
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

int
RS_execute(Regis * r, const char *str, int len)
{
	RegisNode  *ptr = r->node;
	unsigned char *c;

	if (len < 0)
		len = strlen(str);

	if (len < r->nchar)
		return 0;

	if (r->issuffix)
		c = ((unsigned char *) str) + len - r->nchar;
	else
		c = (unsigned char *) str;

	while (ptr)
	{
		switch (ptr->type)
		{
			case RSF_ONEOF:
				if (ptr->len == 0)
				{
					if (*c != *(ptr->data))
						return 0;
				}
				else if (strchr((char *) ptr->data, *c) == NULL)
					return 0;
				break;
			case RSF_NONEOF:
				if (ptr->len == 0)
				{
					if (*c == *(ptr->data))
						return 0;
				}
				else if (strchr((char *) ptr->data, *c) != NULL)
					return 0;
				break;
			default:
				ts_error(ERROR, "RS_execute: Unknown type node: %d\n", ptr->type);
		}
		ptr = ptr->next;
		c++;
	}

	return 1;
}
