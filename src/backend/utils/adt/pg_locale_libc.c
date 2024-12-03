/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities for libc
 *
 * Portions Copyright (c) 2002-2024, PostgreSQL Global Development Group
 *
 * src/backend/utils/adt/pg_locale_libc.c
 *
 *-----------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_database.h"
#include "catalog/pg_collation.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/syscache.h"

/*
 * Size of stack buffer to use for string transformations, used to avoid heap
 * allocations in typical cases. This should be large enough that most strings
 * will fit, but small enough that we feel comfortable putting it on the
 * stack.
 */
#define		TEXTBUFLEN			1024

extern pg_locale_t create_pg_locale_libc(Oid collid, MemoryContext context);

extern int	strncoll_libc(const char *arg1, ssize_t len1,
						  const char *arg2, ssize_t len2,
						  pg_locale_t locale);
extern size_t strnxfrm_libc(char *dest, size_t destsize,
							const char *src, ssize_t srclen,
							pg_locale_t locale);
static locale_t make_libc_collator(const char *collate,
								   const char *ctype);
static void report_newlocale_failure(const char *localename);

#ifdef WIN32
static int	strncoll_libc_win32_utf8(const char *arg1, ssize_t len1,
									 const char *arg2, ssize_t len2,
									 pg_locale_t locale);
#endif

pg_locale_t
create_pg_locale_libc(Oid collid, MemoryContext context)
{
	const char *collate;
	const char *ctype;
	locale_t	loc;
	pg_locale_t result;

	if (collid == DEFAULT_COLLATION_OID)
	{
		HeapTuple	tp;
		Datum		datum;

		tp = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
		datum = SysCacheGetAttrNotNull(DATABASEOID, tp,
									   Anum_pg_database_datcollate);
		collate = TextDatumGetCString(datum);
		datum = SysCacheGetAttrNotNull(DATABASEOID, tp,
									   Anum_pg_database_datctype);
		ctype = TextDatumGetCString(datum);

		ReleaseSysCache(tp);
	}
	else
	{
		HeapTuple	tp;
		Datum		datum;

		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", collid);

		datum = SysCacheGetAttrNotNull(COLLOID, tp,
									   Anum_pg_collation_collcollate);
		collate = TextDatumGetCString(datum);
		datum = SysCacheGetAttrNotNull(COLLOID, tp,
									   Anum_pg_collation_collctype);
		ctype = TextDatumGetCString(datum);

		ReleaseSysCache(tp);
	}


	loc = make_libc_collator(collate, ctype);

	result = MemoryContextAllocZero(context, sizeof(struct pg_locale_struct));
	result->provider = COLLPROVIDER_LIBC;
	result->deterministic = true;
	result->collate_is_c = (strcmp(collate, "C") == 0) ||
		(strcmp(collate, "POSIX") == 0);
	result->ctype_is_c = (strcmp(ctype, "C") == 0) ||
		(strcmp(ctype, "POSIX") == 0);
	result->info.lt = loc;

	return result;
}

/*
 * Create a locale_t with the given collation and ctype.
 *
 * The "C" and "POSIX" locales are not actually handled by libc, so return
 * NULL.
 *
 * Ensure that no path leaks a locale_t.
 */
static locale_t
make_libc_collator(const char *collate, const char *ctype)
{
	locale_t	loc = 0;

	if (strcmp(collate, ctype) == 0)
	{
		if (strcmp(ctype, "C") != 0 && strcmp(ctype, "POSIX") != 0)
		{
			/* Normal case where they're the same */
			errno = 0;
#ifndef WIN32
			loc = newlocale(LC_COLLATE_MASK | LC_CTYPE_MASK, collate,
							NULL);
#else
			loc = _create_locale(LC_ALL, collate);
#endif
			if (!loc)
				report_newlocale_failure(collate);
		}
	}
	else
	{
#ifndef WIN32
		/* We need two newlocale() steps */
		locale_t	loc1 = 0;

		if (strcmp(collate, "C") != 0 && strcmp(collate, "POSIX") != 0)
		{
			errno = 0;
			loc1 = newlocale(LC_COLLATE_MASK, collate, NULL);
			if (!loc1)
				report_newlocale_failure(collate);
		}

		if (strcmp(ctype, "C") != 0 && strcmp(ctype, "POSIX") != 0)
		{
			errno = 0;
			loc = newlocale(LC_CTYPE_MASK, ctype, loc1);
			if (!loc)
			{
				if (loc1)
					freelocale(loc1);
				report_newlocale_failure(ctype);
			}
		}
		else
			loc = loc1;
#else

		/*
		 * XXX The _create_locale() API doesn't appear to support this. Could
		 * perhaps be worked around by changing pg_locale_t to contain two
		 * separate fields.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("collations with different collate and ctype values are not supported on this platform")));
#endif
	}

	return loc;
}

/*
 * strncoll_libc
 *
 * NUL-terminate arguments, if necessary, and pass to strcoll_l().
 *
 * An input string length of -1 means that it's already NUL-terminated.
 */
int
strncoll_libc(const char *arg1, ssize_t len1, const char *arg2, ssize_t len2,
			  pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	size_t		bufsize1 = (len1 == -1) ? 0 : len1 + 1;
	size_t		bufsize2 = (len2 == -1) ? 0 : len2 + 1;
	const char *arg1n;
	const char *arg2n;
	int			result;

	Assert(locale->provider == COLLPROVIDER_LIBC);

#ifdef WIN32
	/* check for this case before doing the work for nul-termination */
	if (GetDatabaseEncoding() == PG_UTF8)
		return strncoll_libc_win32_utf8(arg1, len1, arg2, len2, locale);
#endif							/* WIN32 */

	if (bufsize1 + bufsize2 > TEXTBUFLEN)
		buf = palloc(bufsize1 + bufsize2);

	/* nul-terminate arguments if necessary */
	if (len1 == -1)
	{
		arg1n = arg1;
	}
	else
	{
		char	   *buf1 = buf;

		memcpy(buf1, arg1, len1);
		buf1[len1] = '\0';
		arg1n = buf1;
	}

	if (len2 == -1)
	{
		arg2n = arg2;
	}
	else
	{
		char	   *buf2 = buf + bufsize1;

		memcpy(buf2, arg2, len2);
		buf2[len2] = '\0';
		arg2n = buf2;
	}

	result = strcoll_l(arg1n, arg2n, locale->info.lt);

	if (buf != sbuf)
		pfree(buf);

	return result;
}

/*
 * strnxfrm_libc
 *
 * NUL-terminate src, if necessary, and pass to strxfrm_l().
 *
 * A source length of -1 means that it's already NUL-terminated.
 */
size_t
strnxfrm_libc(char *dest, size_t destsize, const char *src, ssize_t srclen,
			  pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	size_t		bufsize = srclen + 1;
	size_t		result;

	Assert(locale->provider == COLLPROVIDER_LIBC);

	if (srclen == -1)
		return strxfrm_l(dest, src, destsize, locale->info.lt);

	if (bufsize > TEXTBUFLEN)
		buf = palloc(bufsize);

	/* nul-terminate argument */
	memcpy(buf, src, srclen);
	buf[srclen] = '\0';

	result = strxfrm_l(dest, buf, destsize, locale->info.lt);

	if (buf != sbuf)
		pfree(buf);

	/* if dest is defined, it should be nul-terminated */
	Assert(result >= destsize || dest[result] == '\0');

	return result;
}

/*
 * strncoll_libc_win32_utf8
 *
 * Win32 does not have UTF-8. Convert UTF8 arguments to wide characters and
 * invoke wcscoll_l().
 *
 * An input string length of -1 means that it's NUL-terminated.
 */
#ifdef WIN32
static int
strncoll_libc_win32_utf8(const char *arg1, ssize_t len1, const char *arg2,
						 ssize_t len2, pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	char	   *a1p,
			   *a2p;
	int			a1len;
	int			a2len;
	int			r;
	int			result;

	Assert(locale->provider == COLLPROVIDER_LIBC);
	Assert(GetDatabaseEncoding() == PG_UTF8);

	if (len1 == -1)
		len1 = strlen(arg1);
	if (len2 == -1)
		len2 = strlen(arg2);

	a1len = len1 * 2 + 2;
	a2len = len2 * 2 + 2;

	if (a1len + a2len > TEXTBUFLEN)
		buf = palloc(a1len + a2len);

	a1p = buf;
	a2p = buf + a1len;

	/* API does not work for zero-length input */
	if (len1 == 0)
		r = 0;
	else
	{
		r = MultiByteToWideChar(CP_UTF8, 0, arg1, len1,
								(LPWSTR) a1p, a1len / 2);
		if (!r)
			ereport(ERROR,
					(errmsg("could not convert string to UTF-16: error code %lu",
							GetLastError())));
	}
	((LPWSTR) a1p)[r] = 0;

	if (len2 == 0)
		r = 0;
	else
	{
		r = MultiByteToWideChar(CP_UTF8, 0, arg2, len2,
								(LPWSTR) a2p, a2len / 2);
		if (!r)
			ereport(ERROR,
					(errmsg("could not convert string to UTF-16: error code %lu",
							GetLastError())));
	}
	((LPWSTR) a2p)[r] = 0;

	errno = 0;
	result = wcscoll_l((LPWSTR) a1p, (LPWSTR) a2p, locale->info.lt);
	if (result == 2147483647)	/* _NLSCMPERROR; missing from mingw headers */
		ereport(ERROR,
				(errmsg("could not compare Unicode strings: %m")));

	if (buf != sbuf)
		pfree(buf);

	return result;
}
#endif							/* WIN32 */

/* simple subroutine for reporting errors from newlocale() */
static void
report_newlocale_failure(const char *localename)
{
	int			save_errno;

	/*
	 * Windows doesn't provide any useful error indication from
	 * _create_locale(), and BSD-derived platforms don't seem to feel they
	 * need to set errno either (even though POSIX is pretty clear that
	 * newlocale should do so).  So, if errno hasn't been set, assume ENOENT
	 * is what to report.
	 */
	if (errno == 0)
		errno = ENOENT;

	/*
	 * ENOENT means "no such locale", not "no such file", so clarify that
	 * errno with an errdetail message.
	 */
	save_errno = errno;			/* auxiliary funcs might change errno */
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("could not create locale \"%s\": %m",
					localename),
			 (save_errno == ENOENT ?
			  errdetail("The operating system could not find any locale data for the locale name \"%s\".",
						localename) : 0)));
}

/*
 * POSIX doesn't define _l-variants of these functions, but several systems
 * have them.  We provide our own replacements here.
 */
#ifndef HAVE_MBSTOWCS_L
static size_t
mbstowcs_l(wchar_t *dest, const char *src, size_t n, locale_t loc)
{
#ifdef WIN32
	return _mbstowcs_l(dest, src, n, loc);
#else
	size_t		result;
	locale_t	save_locale = uselocale(loc);

	result = mbstowcs(dest, src, n);
	uselocale(save_locale);
	return result;
#endif
}
#endif
#ifndef HAVE_WCSTOMBS_L
static size_t
wcstombs_l(char *dest, const wchar_t *src, size_t n, locale_t loc)
{
#ifdef WIN32
	return _wcstombs_l(dest, src, n, loc);
#else
	size_t		result;
	locale_t	save_locale = uselocale(loc);

	result = wcstombs(dest, src, n);
	uselocale(save_locale);
	return result;
#endif
}
#endif

/*
 * These functions convert from/to libc's wchar_t, *not* pg_wchar_t.
 * Therefore we keep them here rather than with the mbutils code.
 */

/*
 * wchar2char --- convert wide characters to multibyte format
 *
 * This has the same API as the standard wcstombs_l() function; in particular,
 * tolen is the maximum number of bytes to store at *to, and *from must be
 * zero-terminated.  The output will be zero-terminated iff there is room.
 */
size_t
wchar2char(char *to, const wchar_t *from, size_t tolen, pg_locale_t locale)
{
	size_t		result;

	if (tolen == 0)
		return 0;

#ifdef WIN32

	/*
	 * On Windows, the "Unicode" locales assume UTF16 not UTF8 encoding, and
	 * for some reason mbstowcs and wcstombs won't do this for us, so we use
	 * MultiByteToWideChar().
	 */
	if (GetDatabaseEncoding() == PG_UTF8)
	{
		result = WideCharToMultiByte(CP_UTF8, 0, from, -1, to, tolen,
									 NULL, NULL);
		/* A zero return is failure */
		if (result <= 0)
			result = -1;
		else
		{
			Assert(result <= tolen);
			/* Microsoft counts the zero terminator in the result */
			result--;
		}
	}
	else
#endif							/* WIN32 */
	if (locale == (pg_locale_t) 0)
	{
		/* Use wcstombs directly for the default locale */
		result = wcstombs(to, from, tolen);
	}
	else
	{
		/* Use wcstombs_l for nondefault locales */
		result = wcstombs_l(to, from, tolen, locale->info.lt);
	}

	return result;
}

/*
 * char2wchar --- convert multibyte characters to wide characters
 *
 * This has almost the API of mbstowcs_l(), except that *from need not be
 * null-terminated; instead, the number of input bytes is specified as
 * fromlen.  Also, we ereport() rather than returning -1 for invalid
 * input encoding.  tolen is the maximum number of wchar_t's to store at *to.
 * The output will be zero-terminated iff there is room.
 */
size_t
char2wchar(wchar_t *to, size_t tolen, const char *from, size_t fromlen,
		   pg_locale_t locale)
{
	size_t		result;

	if (tolen == 0)
		return 0;

#ifdef WIN32
	/* See WIN32 "Unicode" comment above */
	if (GetDatabaseEncoding() == PG_UTF8)
	{
		/* Win32 API does not work for zero-length input */
		if (fromlen == 0)
			result = 0;
		else
		{
			result = MultiByteToWideChar(CP_UTF8, 0, from, fromlen, to, tolen - 1);
			/* A zero return is failure */
			if (result == 0)
				result = -1;
		}

		if (result != -1)
		{
			Assert(result < tolen);
			/* Append trailing null wchar (MultiByteToWideChar() does not) */
			to[result] = 0;
		}
	}
	else
#endif							/* WIN32 */
	{
		/* mbstowcs requires ending '\0' */
		char	   *str = pnstrdup(from, fromlen);

		if (locale == (pg_locale_t) 0)
		{
			/* Use mbstowcs directly for the default locale */
			result = mbstowcs(to, str, tolen);
		}
		else
		{
			/* Use mbstowcs_l for nondefault locales */
			result = mbstowcs_l(to, str, tolen, locale->info.lt);
		}

		pfree(str);
	}

	if (result == -1)
	{
		/*
		 * Invalid multibyte character encountered.  We try to give a useful
		 * error message by letting pg_verifymbstr check the string.  But it's
		 * possible that the string is OK to us, and not OK to mbstowcs ---
		 * this suggests that the LC_CTYPE locale is different from the
		 * database encoding.  Give a generic error message if pg_verifymbstr
		 * can't find anything wrong.
		 */
		pg_verifymbstr(from, fromlen, false);	/* might not return */
		/* but if it does ... */
		ereport(ERROR,
				(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
				 errmsg("invalid multibyte character for locale"),
				 errhint("The server's LC_CTYPE locale is probably incompatible with the database encoding.")));
	}

	return result;
}
