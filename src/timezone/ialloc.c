/*
 * This file is in the public domain, so clarified as of
 * 1996-06-05 by Arthur David Olson (arthur_david_olson@nih.gov).
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/timezone/ialloc.c,v 1.7 2005/10/15 02:49:51 momjian Exp $
 */

#include "postgres.h"

#include "private.h"


#define nonzero(n)	(((n) == 0) ? 1 : (n))

char *
imalloc(const int n)
{
	return malloc((size_t) nonzero(n));
}

char *
icalloc(int nelem, int elsize)
{
	if (nelem == 0 || elsize == 0)
		nelem = elsize = 1;
	return calloc((size_t) nelem, (size_t) elsize);
}

void *
irealloc(void *pointer, const int size)
{
	if (pointer == NULL)
		return imalloc(size);
	return realloc((void *) pointer, (size_t) nonzero(size));
}

char *
icatalloc(char *old, const char *new)
{
	char	   *result;
	int			oldsize,
				newsize;

	newsize = (new == NULL) ? 0 : strlen(new);
	if (old == NULL)
		oldsize = 0;
	else if (newsize == 0)
		return old;
	else
		oldsize = strlen(old);
	if ((result = irealloc(old, oldsize + newsize + 1)) != NULL)
		if (new != NULL)
			(void) strcpy(result + oldsize, new);
	return result;
}

char *
icpyalloc(const char *string)
{
	return icatalloc((char *) NULL, string);
}

void
ifree(char *p)
{
	if (p != NULL)
		(void) free(p);
}

void
icfree(char *p)
{
	if (p != NULL)
		(void) free(p);
}
