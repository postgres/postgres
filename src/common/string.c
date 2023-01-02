/*-------------------------------------------------------------------------
 *
 * string.c
 *		string handling helpers
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/string.c
 *
 *-------------------------------------------------------------------------
 */


#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "common/string.h"
#include "lib/stringinfo.h"


/*
 * Returns whether the string `str' has the postfix `end'.
 */
bool
pg_str_endswith(const char *str, const char *end)
{
	size_t		slen = strlen(str);
	size_t		elen = strlen(end);

	/* can't be a postfix if longer */
	if (elen > slen)
		return false;

	/* compare the end of the strings */
	str += slen - elen;
	return strcmp(str, end) == 0;
}


/*
 * strtoint --- just like strtol, but returns int not long
 */
int
strtoint(const char *pg_restrict str, char **pg_restrict endptr, int base)
{
	long		val;

	val = strtol(str, endptr, base);
	if (val != (int) val)
		errno = ERANGE;
	return (int) val;
}


/*
 * pg_clean_ascii -- Replace any non-ASCII chars with a "\xXX" string
 *
 * Makes a newly allocated copy of the string passed in, which must be
 * '\0'-terminated. In the backend, additional alloc_flags may be provided and
 * will be passed as-is to palloc_extended(); in the frontend, alloc_flags is
 * ignored and the copy is malloc'd.
 *
 * This function exists specifically to deal with filtering out
 * non-ASCII characters in a few places where the client can provide an almost
 * arbitrary string (and it isn't checked to ensure it's a valid username or
 * database name or similar) and we don't want to have control characters or other
 * things ending up in the log file where server admins might end up with a
 * messed up terminal when looking at them.
 *
 * In general, this function should NOT be used- instead, consider how to handle
 * the string without needing to filter out the non-ASCII characters.
 *
 * Ultimately, we'd like to improve the situation to not require replacing all
 * non-ASCII but perform more intelligent filtering which would allow UTF or
 * similar, but it's unclear exactly what we should allow, so stick to ASCII only
 * for now.
 */
char *
pg_clean_ascii(const char *str, int alloc_flags)
{
	size_t		dstlen;
	char	   *dst;
	const char *p;
	size_t		i = 0;

	/* Worst case, each byte can become four bytes, plus a null terminator. */
	dstlen = strlen(str) * 4 + 1;

#ifdef FRONTEND
	dst = malloc(dstlen);
#else
	dst = palloc_extended(dstlen, alloc_flags);
#endif

	if (!dst)
		return NULL;

	for (p = str; *p != '\0'; p++)
	{

		/* Only allow clean ASCII chars in the string */
		if (*p < 32 || *p > 126)
		{
			Assert(i < (dstlen - 3));
			snprintf(&dst[i], dstlen - i, "\\x%02x", (unsigned char) *p);
			i += 4;
		}
		else
		{
			Assert(i < dstlen);
			dst[i] = *p;
			i++;
		}
	}

	Assert(i < dstlen);
	dst[i] = '\0';
	return dst;
}


/*
 * pg_is_ascii -- Check if string is made only of ASCII characters
 */
bool
pg_is_ascii(const char *str)
{
	while (*str)
	{
		if (IS_HIGHBIT_SET(*str))
			return false;
		str++;
	}
	return true;
}


/*
 * pg_strip_crlf -- Remove any trailing newline and carriage return
 *
 * Removes any trailing newline and carriage return characters (\r on
 * Windows) in the input string, zero-terminating it.
 *
 * The passed in string must be zero-terminated.  This function returns
 * the new length of the string.
 */
int
pg_strip_crlf(char *str)
{
	int			len = strlen(str);

	while (len > 0 && (str[len - 1] == '\n' ||
					   str[len - 1] == '\r'))
		str[--len] = '\0';

	return len;
}
