/*
 * regerror - error-code expansion
 *
 * Copyright (c) 1998, 1999 Henry Spencer.  All rights reserved.
 *
 * Development of this software was funded, in part, by Cray Research Inc.,
 * UUNET Communications Services Inc., Sun Microsystems Inc., and Scriptics
 * Corporation, none of whom are responsible for the results.  The author
 * thanks all of them.
 *
 * Redistribution and use in source and binary forms -- with or without
 * modification -- are permitted for any purpose, provided that
 * redistributions in source form retain this entire copyright notice and
 * indicate the origin and nature of any modifications.
 *
 * I'd appreciate being given credit for this package in the documentation
 * of software which uses it, but that is not a requirement.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * HENRY SPENCER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * src/backend/regex/regerror.c
 *
 */

#include "regex/regguts.h"

/* unknown-error explanation */
static const char unk[] = "*** unknown regex error code 0x%x ***";

/* struct to map among codes, code names, and explanations */
static const struct rerr
{
	int			code;
	const char *name;
	const char *explain;
}	rerrs[] =

{
	/* the actual table is built from regex.h */
#include "regex/regerrs.h"		/* pgrminclude ignore */
	{
		-1, "", "oops"
	},							/* explanation special-cased in code */
};

/*
 * pg_regerror - the interface to error numbers
 */
/* ARGSUSED */
size_t							/* actual space needed (including NUL) */
pg_regerror(int errcode,		/* error code, or REG_ATOI or REG_ITOA */
			const regex_t *preg,	/* associated regex_t (unused at present) */
			char *errbuf,		/* result buffer (unless errbuf_size==0) */
			size_t errbuf_size) /* available space in errbuf, can be 0 */
{
	const struct rerr *r;
	const char *msg;
	char		convbuf[sizeof(unk) + 50];		/* 50 = plenty for int */
	size_t		len;
	int			icode;

	switch (errcode)
	{
		case REG_ATOI:			/* convert name to number */
			for (r = rerrs; r->code >= 0; r++)
				if (strcmp(r->name, errbuf) == 0)
					break;
			sprintf(convbuf, "%d", r->code);	/* -1 for unknown */
			msg = convbuf;
			break;
		case REG_ITOA:			/* convert number to name */
			icode = atoi(errbuf);		/* not our problem if this fails */
			for (r = rerrs; r->code >= 0; r++)
				if (r->code == icode)
					break;
			if (r->code >= 0)
				msg = r->name;
			else
			{					/* unknown; tell him the number */
				sprintf(convbuf, "REG_%u", (unsigned) icode);
				msg = convbuf;
			}
			break;
		default:				/* a real, normal error code */
			for (r = rerrs; r->code >= 0; r++)
				if (r->code == errcode)
					break;
			if (r->code >= 0)
				msg = r->explain;
			else
			{					/* unknown; say so */
				sprintf(convbuf, unk, errcode);
				msg = convbuf;
			}
			break;
	}

	len = strlen(msg) + 1;		/* space needed, including NUL */
	if (errbuf_size > 0)
	{
		if (errbuf_size > len)
			strcpy(errbuf, msg);
		else
		{						/* truncate to fit */
			strncpy(errbuf, msg, errbuf_size - 1);
			errbuf[errbuf_size - 1] = '\0';
		}
	}

	return len;
}
