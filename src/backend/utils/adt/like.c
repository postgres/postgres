/*-------------------------------------------------------------------------
 *
 * like.c
 *	  like expression handling code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  /usr/local/devel/pglite/cvs/src/backend/utils/adt/like.c,v 1.1 1995/07/30 23:55:36 emkxp01 Exp
 *
 *
 *	 NOTES
 *		A big hack of the regexp.c code!! Contributed by
 *		Keith Parks <emkxp01@mtcc.demon.co.uk> (7/95).
 *
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"			/* postgres system include file */
#include "utils/palloc.h"
#include "utils/builtins.h"		/* where the function declarations go */
#include "mb/pg_wchar.h"
#include "utils/mcxt.h"

static int	like(pg_wchar * text, pg_wchar * p);

/*
 *	interface routines called by the function manager
 */

/*
   fixedlen_like:

   a generic fixed length like routine
		 s		- the string to match against  (not necessarily null-terminated)
		 p		   - the pattern
		 charlen   - the length of the string
*/
static bool
fixedlen_like(char *s, struct varlena * p, int charlen)
{
	pg_wchar   *sterm,
			   *pterm;
	int			result;
	int			len;

	if (!s || !p)
		return FALSE;

	/* be sure sterm is null-terminated */
#ifdef MULTIBYTE
	sterm = (pg_wchar *) palloc((charlen + 1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) s, sterm, charlen);
#else
	sterm = (char *) palloc(charlen + 1);
	StrNCpy(sterm, s, charlen + 1);
#endif

	/*
	 * p is a text = varlena, not a string so we have to make a string
	 * from the vl_data field of the struct.
	 */

	/* palloc the length of the text + the null character */
	len = VARSIZE(p) - VARHDRSZ;
#ifdef MULTIBYTE
	pterm = (pg_wchar *) palloc((len + 1) * sizeof(pg_wchar));
	(void) pg_mb2wchar_with_len((unsigned char *) VARDATA(p), pterm, len);
#else
	pterm = (char *) palloc(len + 1);
	memmove(pterm, VARDATA(p), len);
	*(pterm + len) = (char) NULL;
#endif

	/* do the regexp matching */
	result = like(sterm, pterm);

	pfree(sterm);
	pfree(pterm);

	return (bool) result;
}

bool
namelike(NameData *n, struct varlena * p)
{
	if (!n)
		return FALSE;
	return fixedlen_like(n->data, p, NAMEDATALEN);
}

bool
namenlike(NameData *s, struct varlena * p)
{
	return !namelike(s, p);
}

bool
textlike(struct varlena * s, struct varlena * p)
{
	if (!s)
		return FALSE;
	return fixedlen_like(VARDATA(s), p, VARSIZE(s) - VARHDRSZ);
}

bool
textnlike(struct varlena * s, struct varlena * p)
{
	return !textlike(s, p);
}


/*	$Revision: 1.26 $
**	"like.c" A first attempt at a LIKE operator for Postgres95.
**
**	Originally written by Rich $alz, mirror!rs, Wed Nov 26 19:03:17 EST 1986.
**	Rich $alz is now <rsalz@bbn.com>.
**	Special thanks to Lars Mathiesen <thorinn@diku.dk> for the LABORT code.
**
**	This code was shamelessly stolen from the "pql" code by myself and
**	slightly modified :)
**
**	All references to the word "star" were replaced by "percent"
**	All references to the word "wild" were replaced by "like"
**
**	All the nice shell RE matching stuff was replaced by just "_" and "%"
**
**	As I don't have a copy of the SQL standard handy I wasn't sure whether
**	to leave in the '\' escape character handling. (I suspect the standard
**	handles "%%" as a single literal percent)
**
**	Keith Parks. <keith@mtcc.demon.co.uk>
**
**	[SQL92 lets you specify the escape character by saying
**	 LIKE <pattern> ESCAPE <escape character>. We are a small operation
**	 so we force you to use '\'. - ay 7/95]
**
*/

#define LIKE_TRUE						1
#define LIKE_FALSE						0
#define LIKE_ABORT						-1

/*
**	Match text and p, return LIKE_TRUE, LIKE_FALSE, or LIKE_ABORT.
*/
static int
DoMatch(pg_wchar * text, pg_wchar * p)
{
	int			matched;

	for (; *p && *text; text ++, p++)
	{
		switch (*p)
		{
			case '\\':
				/* Literal match with following character. */
				p++;
				/* FALLTHROUGH */
			default:
				if (*text !=*p)
					return LIKE_FALSE;
				break;
			case '_':
				/* Match anything. */
				break;
			case '%':
				/* %% is the same as % according to the SQL standard */
				/* Advance past all %'s */
				while (*p == '%')
					p++;
				if (*p == '\0')
					/* Trailing percent matches everything. */
					return LIKE_TRUE;
				while (*text)
				{
					/* Optimization to prevent most recursion */
					if ((*text == *p ||
						 *p == '\\' || *p == '%' || *p == '_') &&
						(matched = DoMatch(text, p)) != LIKE_FALSE)
						return matched;
					text	  ++;
				}
				return LIKE_ABORT;
		}
	}

	if (*text !='\0')
		return LIKE_ABORT;
	else
	{
		/* End of input string.  Do we have matching string remaining? */
		while (*p == '%')		/* allow multiple %'s at end of pattern */
			p++;
		if (*p == '\0')
			return LIKE_TRUE;
		else
			return LIKE_ABORT;
	}
}


/*
**	User-level routine.  Returns TRUE or FALSE.
*/
static int
like(pg_wchar * text, pg_wchar * p)
{
	if (p[0] == '%' && p[1] == '\0')
		return TRUE;
	return DoMatch(text, p) == LIKE_TRUE;
}
