/*-------------------------------------------------------------------------
 *
 * like.c--
 *    like expression handling code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    /usr/local/devel/pglite/cvs/src/backend/utils/adt/like.c,v 1.1 1995/07/30 23:55:36 emkxp01 Exp
 *
 *
 *   NOTES
 *	A big hack of the regexp.c code!! Contributed by
 *	Keith Parks <emkxp01@mtcc.demon.co.uk> (7/95).
 *
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"		/* postgres system include file */
#include "utils/palloc.h"
#include "utils/builtins.h"	/* where the function declarations go */

int like(char *text, char *p);

/*
 *  interface routines called by the function manager
 */

/*
   fixedlen_like:

   a generic fixed length like routine
         s      - the string to match against  (not necessarily null-terminated)
	 p         - the pattern
	 charlen   - the length of the string
*/
static bool 
fixedlen_like(char *s, struct varlena* p, int charlen)
{
    char *sterm, *pterm;
    int result;

    if (!s || !p)
	return FALSE;
    
    /* be sure sterm is null-terminated */
    sterm = (char *) palloc(charlen + 1);
    memset(sterm, 0, charlen + 1);
    strncpy(sterm, s, charlen);
    
    /* p is a text = varlena, not a string so we have to make 
     * a string from the vl_data field of the struct. */
    
    /* palloc the length of the text + the null character */
    pterm = (char *) palloc(VARSIZE(p) - VARHDRSZ + 1);
    memmove(pterm, VARDATA(p), VARSIZE(p) - VARHDRSZ);
    *(pterm + VARSIZE(p) - VARHDRSZ) = (char)NULL;
    
    /* do the regexp matching */
    result = like(sterm, pterm);
    
    pfree(sterm);
    pfree(pterm);
    
    return ((bool) result);
}

bool 
char2like(uint16 arg1, struct varlena *p)
{
    char *s = (char *) &arg1;
    return (fixedlen_like(s, p, 2));
}    

bool 
char2nlike(uint16 arg1, struct varlena *p)
{
    return (!char2like(arg1, p));
}

bool 
char4like(uint32 arg1, struct varlena *p)
{
    char *s = (char *) &arg1;
    return (fixedlen_like(s, p, 4));
}

bool 
char4nlike(uint32 arg1, struct varlena *p)
{
    return (!char4like(arg1, p));
}

bool 
char8like(char *s, struct varlena *p)
{
    return (fixedlen_like(s, p, 8));
}

bool 
char8nlike(char *s, struct varlena *p)
{
    return (!char8like(s, p));
}

bool 
char16like(char *s, struct varlena *p)
{
    return (fixedlen_like(s, p, 16));
}
bool 
char16nlike(char *s, struct varlena *p)
{
    return (!char16like(s, p));
}

bool 
namelike(NameData *n, struct varlena *p)
{
    if (!n) return FALSE;
    return (fixedlen_like(n->data, p, NAMEDATALEN));
}

bool 
namenlike(NameData *s, struct varlena *p)
{
    return (!namelike(s, p));
}

bool 
textlike(struct varlena *s, struct varlena *p)
{
    if (!s) return FALSE;
    return (fixedlen_like(VARDATA(s), p, VARSIZE(s) - VARHDRSZ));
}

bool textnlike(struct varlena *s, struct varlena *p)
{
    return (!textlike(s, p));
}


/*  $Revision: 1.3 $
**  "like.c" A first attempt at a LIKE operator for Postgres95.
**
**  Originally written by Rich $alz, mirror!rs, Wed Nov 26 19:03:17 EST 1986.
**  Rich $alz is now <rsalz@bbn.com>.
**  Special thanks to Lars Mathiesen <thorinn@diku.dk> for the LABORT code.
** 
**  This code was shamelessly stolen from the "pql" code by myself and
**  slightly modified :)
** 
**  All references to the word "star" were replaced by "percent"
**  All references to the word "wild" were replaced by "like"
** 
**  All the nice shell RE matching stuff was replaced by just "_" and "%"
** 
**  As I don't have a copy of the SQL standard handy I wasn't sure whether
**  to leave in the '\' escape character handling. (I suspect the standard
**  handles "%%" as a single literal percent)
**
**  Keith Parks. <keith@mtcc.demon.co.uk>
**
**  [SQL92 lets you specify the escape character by saying
**   LIKE <pattern> ESCAPE <escape character>. We are a small operation
**   so we force you to use '\'. - ay 7/95]
**
*/

#define LIKE_TRUE			1
#define LIKE_FALSE			0
#define LIKE_ABORT			-1

/*
**  Match text and p, return LIKE_TRUE, LIKE_FALSE, or LIKE_ABORT.
*/
static int
DoMatch(register char *text, register char *p)
{
    register int	matched;

    for ( ; *p; text++, p++) {
	if (*text == '\0' && *p != '%')
	    return LIKE_ABORT;
	switch (*p) {
	case '\\':
	    /* Literal match with following character. */
	    p++;
	    /* FALLTHROUGH */
	default:
	    if (*text != *p)
		return LIKE_FALSE;
	    continue;
	case '_':
	    /* Match anything. */
	    continue;
	case '%':
	    while (*++p == '%')
		/* Consecutive percents act just like one. */
		continue;
	    if (*p == '\0')
		/* Trailing percent matches everything. */
		return LIKE_TRUE;
	    while (*text)
		if ((matched = DoMatch(text++, p)) != LIKE_FALSE)
		    return matched;
	    return LIKE_ABORT;
	}
    }

    return *text == '\0';
}


/*
**  User-level routine.  Returns TRUE or FALSE.
*/
int
like(char *text, char *p)
{
    if (p[0] == '%' && p[1] == '\0')
	return TRUE;
    return (DoMatch(text, p) == LIKE_TRUE);
}
