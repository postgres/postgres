/*-------------------------------------------------------------------------
 *
 * pgstrcasecmp.c
 *	   Portable SQL-like case-independent comparisons and conversions.
 *
 * SQL99 specifies Unicode-aware case normalization, which we don't yet
 * have the infrastructure for.  Instead we use tolower() to provide a
 * locale-aware translation.  However, there are some locales where this
 * is not right either (eg, Turkish may do strange things with 'i' and
 * 'I').  Our current compromise is to use tolower() for characters with
 * the high bit set, and use an ASCII-only downcasing for 7-bit
 * characters.
 *
 * NB: this code should match downcase_truncate_identifier() in scansup.c.
 *
 * We also provide strict ASCII-only case conversion functions, which can
 * be used to implement C/POSIX case folding semantics no matter what the
 * C library thinks the locale is.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * src/port/pgstrcasecmp.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <ctype.h>


/*
 * Case-independent comparison of two null-terminated strings.
 */
int
pg_strcasecmp(const char *s1, const char *s2)
{
	for (;;)
	{
		unsigned char ch1 = (unsigned char) *s1++;
		unsigned char ch2 = (unsigned char) *s2++;

		if (ch1 != ch2)
		{
			if (ch1 >= 'A' && ch1 <= 'Z')
				ch1 += 'a' - 'A';
			else if (IS_HIGHBIT_SET(ch1) && isupper(ch1))
				ch1 = tolower(ch1);

			if (ch2 >= 'A' && ch2 <= 'Z')
				ch2 += 'a' - 'A';
			else if (IS_HIGHBIT_SET(ch2) && isupper(ch2))
				ch2 = tolower(ch2);

			if (ch1 != ch2)
				return (int) ch1 - (int) ch2;
		}
		if (ch1 == 0)
			break;
	}
	return 0;
}

/*
 * Case-independent comparison of two not-necessarily-null-terminated strings.
 * At most n bytes will be examined from each string.
 */
int
pg_strncasecmp(const char *s1, const char *s2, size_t n)
{
	while (n-- > 0)
	{
		unsigned char ch1 = (unsigned char) *s1++;
		unsigned char ch2 = (unsigned char) *s2++;

		if (ch1 != ch2)
		{
			if (ch1 >= 'A' && ch1 <= 'Z')
				ch1 += 'a' - 'A';
			else if (IS_HIGHBIT_SET(ch1) && isupper(ch1))
				ch1 = tolower(ch1);

			if (ch2 >= 'A' && ch2 <= 'Z')
				ch2 += 'a' - 'A';
			else if (IS_HIGHBIT_SET(ch2) && isupper(ch2))
				ch2 = tolower(ch2);

			if (ch1 != ch2)
				return (int) ch1 - (int) ch2;
		}
		if (ch1 == 0)
			break;
	}
	return 0;
}

/*
 * Fold a character to upper case.
 *
 * Unlike some versions of toupper(), this is safe to apply to characters
 * that aren't lower case letters.  Note however that the whole thing is
 * a bit bogus for multibyte character sets.
 */
unsigned char
pg_toupper(unsigned char ch)
{
	if (ch >= 'a' && ch <= 'z')
		ch += 'A' - 'a';
	else if (IS_HIGHBIT_SET(ch) && islower(ch))
		ch = toupper(ch);
	return ch;
}

/*
 * Fold a character to lower case.
 *
 * Unlike some versions of tolower(), this is safe to apply to characters
 * that aren't upper case letters.  Note however that the whole thing is
 * a bit bogus for multibyte character sets.
 */
unsigned char
pg_tolower(unsigned char ch)
{
	if (ch >= 'A' && ch <= 'Z')
		ch += 'a' - 'A';
	else if (IS_HIGHBIT_SET(ch) && isupper(ch))
		ch = tolower(ch);
	return ch;
}

/*
 * Fold a character to upper case, following C/POSIX locale rules.
 */
unsigned char
pg_ascii_toupper(unsigned char ch)
{
	if (ch >= 'a' && ch <= 'z')
		ch += 'A' - 'a';
	return ch;
}

/*
 * Fold a character to lower case, following C/POSIX locale rules.
 */
unsigned char
pg_ascii_tolower(unsigned char ch)
{
	if (ch >= 'A' && ch <= 'Z')
		ch += 'a' - 'A';
	return ch;
}
