/*-------------------------------------------------------------------------
 *
 * memcmp.c
 *	  compares memory bytes
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/memcmp.c,v 1.4 2003/11/29 19:52:13 pgsql Exp $
 *
 * This file was taken from NetBSD and is used by SunOS because memcmp
 * on that platform does not properly compare negative bytes.
 *
 *-------------------------------------------------------------------------
 */

#include <string.h>

/*
 * Compare memory regions.
 */
int
memcmp(const void *s1, const void *s2, size_t n)
{
	if (n != 0)
	{
		const unsigned char *p1 = s1,
				   *p2 = s2;

		do
		{
			if (*p1++ != *p2++)
				return (*--p1 - *--p2);
		} while (--n != 0);
	}
	return 0;
}
