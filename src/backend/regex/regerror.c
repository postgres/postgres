/*-
 * Copyright (c) 1992, 1993, 1994 Henry Spencer.
 * Copyright (c) 1992, 1993, 1994
 *		The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Henry Spencer.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *	  must display the following acknowledgement:
 *		This product includes software developed by the University of
 *		California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *	  may be used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *		@(#)regerror.c	8.4 (Berkeley) 3/20/94
 */

#include "postgres.h"

#include <sys/types.h>
#include <ctype.h>
#include <limits.h>
#include <assert.h>

#include "regex/regex.h"
#include "regex/utils.h"
#include "regex/regex2.h"

static char *regatoi(const regex_t *preg, char *localbuf);

static struct rerr
{
	int			code;
	char	   *name;
	char	   *explain;
}			rerrs[] =

{
	{
		REG_NOMATCH, "REG_NOMATCH", "regexec() failed to match"
	},
	{
		REG_BADPAT, "REG_BADPAT", "invalid regular expression"
	},
	{
		REG_ECOLLATE, "REG_ECOLLATE", "invalid collating element"
	},
	{
		REG_ECTYPE, "REG_ECTYPE", "invalid character class"
	},
	{
		REG_EESCAPE, "REG_EESCAPE", "trailing backslash (\\)"
	},
	{
		REG_ESUBREG, "REG_ESUBREG", "invalid backreference number"
	},
	{
		REG_EBRACK, "REG_EBRACK", "brackets ([ ]) not balanced"
	},
	{
		REG_EPAREN, "REG_EPAREN", "parentheses not balanced"
	},
	{
		REG_EBRACE, "REG_EBRACE", "braces not balanced"
	},
	{
		REG_BADBR, "REG_BADBR", "invalid repetition count(s)"
	},
	{
		REG_ERANGE, "REG_ERANGE", "invalid character range"
	},
	{
		REG_ESPACE, "REG_ESPACE", "out of memory"
	},
	{
		REG_BADRPT, "REG_BADRPT", "repetition-operator operand invalid"
	},
	{
		REG_EMPTY, "REG_EMPTY", "empty (sub)expression"
	},
	{
		REG_ASSERT, "REG_ASSERT", "\"can't happen\" -- you found a bug"
	},
	{
		REG_INVARG, "REG_INVARG", "invalid argument to regex routine"
	},
	{
		0, "", "*** unknown regexp error code ***"
	}
};

/*
 * regerror - the interface to error numbers
 */
/* ARGSUSED */
size_t
pg95_regerror(int errcode, const regex_t *preg,
			  char *errbuf, size_t errbuf_size)
{
	struct rerr *r;
	size_t		len;
	int			target = errcode & ~REG_ITOA;
	char	   *s;
	char		convbuf[50];

	if (errcode == REG_ATOI)
		s = regatoi(preg, convbuf);
	else
	{
		for (r = rerrs; r->code != 0; r++)
			if (r->code == target)
				break;

		if (errcode & REG_ITOA)
		{
			if (r->code != 0)
				strcpy(convbuf, r->name);
			else
				sprintf(convbuf, "REG_0x%x", target);
			assert(strlen(convbuf) < sizeof(convbuf));
			s = convbuf;
		}
		else
			s = r->explain;
	}

	len = strlen(s) + 1;
	if (errbuf_size > 0)
	{
		if (errbuf_size > len)
			strcpy(errbuf, s);
		else
		{
			strncpy(errbuf, s, errbuf_size - 1);
			errbuf[errbuf_size - 1] = '\0';
		}
	}

	return len;
}

/*
 * regatoi - internal routine to implement REG_ATOI
 */
static char *
regatoi(const regex_t *preg, char *localbuf)
{
	struct rerr *r;

	for (r = rerrs; r->code != 0; r++)
#ifdef MULTIBYTE
		if (pg_char_and_wchar_strcmp(r->name, preg->re_endp) == 0)
#else
		if (strcmp(r->name, preg->re_endp) == 0)
#endif
			break;
	if (r->code == 0)
		return "0";

	sprintf(localbuf, "%d", r->code);
	return localbuf;
}
