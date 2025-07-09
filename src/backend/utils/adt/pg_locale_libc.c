/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities for libc
 *
 * Portions Copyright (c) 2002-2025, PostgreSQL Global Development Group
 *
 * src/backend/utils/adt/pg_locale_libc.c
 *
 *-----------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>
#include <wctype.h>

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

#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif

#ifdef WIN32
#include <shlwapi.h>
#endif

/*
 * For the libc provider, to provide as much functionality as possible on a
 * variety of platforms without going so far as to implement everything from
 * scratch, we use several implementation strategies depending on the
 * situation:
 *
 * 1. In C/POSIX collations, we use hard-wired code.  We can't depend on
 * the <ctype.h> functions since those will obey LC_CTYPE.  Note that these
 * collations don't give a fig about multibyte characters.
 *
 * 2. When working in UTF8 encoding, we use the <wctype.h> functions.
 * This assumes that every platform uses Unicode codepoints directly
 * as the wchar_t representation of Unicode.  (XXX: ICU makes this assumption
 * even for non-UTF8 encodings, which may be a problem.)  On some platforms
 * wchar_t is only 16 bits wide, so we have to punt for codepoints > 0xFFFF.
 *
 * 3. In all other encodings, we use the <ctype.h> functions for pg_wchar
 * values up to 255, and punt for values above that.  This is 100% correct
 * only in single-byte encodings such as LATINn.  However, non-Unicode
 * multibyte encodings are mostly Far Eastern character sets for which the
 * properties being tested here aren't very relevant for higher code values
 * anyway.  The difficulty with using the <wctype.h> functions with
 * non-Unicode multibyte encodings is that we can have no certainty that
 * the platform's wchar_t representation matches what we do in pg_wchar
 * conversions.
 *
 * As a special case, in the "default" collation, (2) and (3) force ASCII
 * letters to follow ASCII upcase/downcase rules, while in a non-default
 * collation we just let the library functions do what they will.  The case
 * where this matters is treatment of I/i in Turkish, and the behavior is
 * meant to match the upper()/lower() SQL functions.
 *
 * We store the active collation setting in static variables.  In principle
 * it could be passed down to here via the regex library's "struct vars" data
 * structure; but that would require somewhat invasive changes in the regex
 * library, and right now there's no real benefit to be gained from that.
 *
 * NB: the coding here assumes pg_wchar is an unsigned type.
 */

/*
 * Size of stack buffer to use for string transformations, used to avoid heap
 * allocations in typical cases. This should be large enough that most strings
 * will fit, but small enough that we feel comfortable putting it on the
 * stack.
 */
#define		TEXTBUFLEN			1024

extern pg_locale_t create_pg_locale_libc(Oid collid, MemoryContext context);

static int	strncoll_libc(const char *arg1, ssize_t len1,
						  const char *arg2, ssize_t len2,
						  pg_locale_t locale);
static size_t strnxfrm_libc(char *dest, size_t destsize,
							const char *src, ssize_t srclen,
							pg_locale_t locale);
extern char *get_collation_actual_version_libc(const char *collcollate);
static locale_t make_libc_collator(const char *collate,
								   const char *ctype);

#ifdef WIN32
static int	strncoll_libc_win32_utf8(const char *arg1, ssize_t len1,
									 const char *arg2, ssize_t len2,
									 pg_locale_t locale);
#endif

static size_t strlower_libc_sb(char *dest, size_t destsize,
							   const char *src, ssize_t srclen,
							   pg_locale_t locale);
static size_t strlower_libc_mb(char *dest, size_t destsize,
							   const char *src, ssize_t srclen,
							   pg_locale_t locale);
static size_t strtitle_libc_sb(char *dest, size_t destsize,
							   const char *src, ssize_t srclen,
							   pg_locale_t locale);
static size_t strtitle_libc_mb(char *dest, size_t destsize,
							   const char *src, ssize_t srclen,
							   pg_locale_t locale);
static size_t strupper_libc_sb(char *dest, size_t destsize,
							   const char *src, ssize_t srclen,
							   pg_locale_t locale);
static size_t strupper_libc_mb(char *dest, size_t destsize,
							   const char *src, ssize_t srclen,
							   pg_locale_t locale);

static bool
wc_isdigit_libc_sb(pg_wchar wc, pg_locale_t locale)
{
	return isdigit_l((unsigned char) wc, locale->info.lt);
}

static bool
wc_isalpha_libc_sb(pg_wchar wc, pg_locale_t locale)
{
	return isalpha_l((unsigned char) wc, locale->info.lt);
}

static bool
wc_isalnum_libc_sb(pg_wchar wc, pg_locale_t locale)
{
	return isalnum_l((unsigned char) wc, locale->info.lt);
}

static bool
wc_isupper_libc_sb(pg_wchar wc, pg_locale_t locale)
{
	return isupper_l((unsigned char) wc, locale->info.lt);
}

static bool
wc_islower_libc_sb(pg_wchar wc, pg_locale_t locale)
{
	return islower_l((unsigned char) wc, locale->info.lt);
}

static bool
wc_isgraph_libc_sb(pg_wchar wc, pg_locale_t locale)
{
	return isgraph_l((unsigned char) wc, locale->info.lt);
}

static bool
wc_isprint_libc_sb(pg_wchar wc, pg_locale_t locale)
{
	return isprint_l((unsigned char) wc, locale->info.lt);
}

static bool
wc_ispunct_libc_sb(pg_wchar wc, pg_locale_t locale)
{
	return ispunct_l((unsigned char) wc, locale->info.lt);
}

static bool
wc_isspace_libc_sb(pg_wchar wc, pg_locale_t locale)
{
	return isspace_l((unsigned char) wc, locale->info.lt);
}

static bool
wc_isdigit_libc_mb(pg_wchar wc, pg_locale_t locale)
{
	return iswdigit_l((wint_t) wc, locale->info.lt);
}

static bool
wc_isalpha_libc_mb(pg_wchar wc, pg_locale_t locale)
{
	return iswalpha_l((wint_t) wc, locale->info.lt);
}

static bool
wc_isalnum_libc_mb(pg_wchar wc, pg_locale_t locale)
{
	return iswalnum_l((wint_t) wc, locale->info.lt);
}

static bool
wc_isupper_libc_mb(pg_wchar wc, pg_locale_t locale)
{
	return iswupper_l((wint_t) wc, locale->info.lt);
}

static bool
wc_islower_libc_mb(pg_wchar wc, pg_locale_t locale)
{
	return iswlower_l((wint_t) wc, locale->info.lt);
}

static bool
wc_isgraph_libc_mb(pg_wchar wc, pg_locale_t locale)
{
	return iswgraph_l((wint_t) wc, locale->info.lt);
}

static bool
wc_isprint_libc_mb(pg_wchar wc, pg_locale_t locale)
{
	return iswprint_l((wint_t) wc, locale->info.lt);
}

static bool
wc_ispunct_libc_mb(pg_wchar wc, pg_locale_t locale)
{
	return iswpunct_l((wint_t) wc, locale->info.lt);
}

static bool
wc_isspace_libc_mb(pg_wchar wc, pg_locale_t locale)
{
	return iswspace_l((wint_t) wc, locale->info.lt);
}

static char
char_tolower_libc(unsigned char ch, pg_locale_t locale)
{
	Assert(pg_database_encoding_max_length() == 1);
	return tolower_l(ch, locale->info.lt);
}

static bool
char_is_cased_libc(char ch, pg_locale_t locale)
{
	bool		is_multibyte = pg_database_encoding_max_length() > 1;

	if (is_multibyte && IS_HIGHBIT_SET(ch))
		return true;
	else
		return isalpha_l((unsigned char) ch, locale->info.lt);
}

static pg_wchar
toupper_libc_sb(pg_wchar wc, pg_locale_t locale)
{
	Assert(GetDatabaseEncoding() != PG_UTF8);

	/* force C behavior for ASCII characters, per comments above */
	if (locale->is_default && wc <= (pg_wchar) 127)
		return pg_ascii_toupper((unsigned char) wc);
	if (wc <= (pg_wchar) UCHAR_MAX)
		return toupper_l((unsigned char) wc, locale->info.lt);
	else
		return wc;
}

static pg_wchar
toupper_libc_mb(pg_wchar wc, pg_locale_t locale)
{
	Assert(GetDatabaseEncoding() == PG_UTF8);

	/* force C behavior for ASCII characters, per comments above */
	if (locale->is_default && wc <= (pg_wchar) 127)
		return pg_ascii_toupper((unsigned char) wc);
	if (sizeof(wchar_t) >= 4 || wc <= (pg_wchar) 0xFFFF)
		return towupper_l((wint_t) wc, locale->info.lt);
	else
		return wc;
}

static pg_wchar
tolower_libc_sb(pg_wchar wc, pg_locale_t locale)
{
	Assert(GetDatabaseEncoding() != PG_UTF8);

	/* force C behavior for ASCII characters, per comments above */
	if (locale->is_default && wc <= (pg_wchar) 127)
		return pg_ascii_tolower((unsigned char) wc);
	if (wc <= (pg_wchar) UCHAR_MAX)
		return tolower_l((unsigned char) wc, locale->info.lt);
	else
		return wc;
}

static pg_wchar
tolower_libc_mb(pg_wchar wc, pg_locale_t locale)
{
	Assert(GetDatabaseEncoding() == PG_UTF8);

	/* force C behavior for ASCII characters, per comments above */
	if (locale->is_default && wc <= (pg_wchar) 127)
		return pg_ascii_tolower((unsigned char) wc);
	if (sizeof(wchar_t) >= 4 || wc <= (pg_wchar) 0xFFFF)
		return towlower_l((wint_t) wc, locale->info.lt);
	else
		return wc;
}

static const struct ctype_methods ctype_methods_libc_sb = {
	.strlower = strlower_libc_sb,
	.strtitle = strtitle_libc_sb,
	.strupper = strupper_libc_sb,
	.wc_isdigit = wc_isdigit_libc_sb,
	.wc_isalpha = wc_isalpha_libc_sb,
	.wc_isalnum = wc_isalnum_libc_sb,
	.wc_isupper = wc_isupper_libc_sb,
	.wc_islower = wc_islower_libc_sb,
	.wc_isgraph = wc_isgraph_libc_sb,
	.wc_isprint = wc_isprint_libc_sb,
	.wc_ispunct = wc_ispunct_libc_sb,
	.wc_isspace = wc_isspace_libc_sb,
	.char_is_cased = char_is_cased_libc,
	.char_tolower = char_tolower_libc,
	.wc_toupper = toupper_libc_sb,
	.wc_tolower = tolower_libc_sb,
	.max_chr = UCHAR_MAX,
};

/*
 * Non-UTF8 multibyte encodings use multibyte semantics for case mapping, but
 * single-byte semantics for pattern matching.
 */
static const struct ctype_methods ctype_methods_libc_other_mb = {
	.strlower = strlower_libc_mb,
	.strtitle = strtitle_libc_mb,
	.strupper = strupper_libc_mb,
	.wc_isdigit = wc_isdigit_libc_sb,
	.wc_isalpha = wc_isalpha_libc_sb,
	.wc_isalnum = wc_isalnum_libc_sb,
	.wc_isupper = wc_isupper_libc_sb,
	.wc_islower = wc_islower_libc_sb,
	.wc_isgraph = wc_isgraph_libc_sb,
	.wc_isprint = wc_isprint_libc_sb,
	.wc_ispunct = wc_ispunct_libc_sb,
	.wc_isspace = wc_isspace_libc_sb,
	.char_is_cased = char_is_cased_libc,
	.char_tolower = char_tolower_libc,
	.wc_toupper = toupper_libc_sb,
	.wc_tolower = tolower_libc_sb,
	.max_chr = UCHAR_MAX,
};

static const struct ctype_methods ctype_methods_libc_utf8 = {
	.strlower = strlower_libc_mb,
	.strtitle = strtitle_libc_mb,
	.strupper = strupper_libc_mb,
	.wc_isdigit = wc_isdigit_libc_mb,
	.wc_isalpha = wc_isalpha_libc_mb,
	.wc_isalnum = wc_isalnum_libc_mb,
	.wc_isupper = wc_isupper_libc_mb,
	.wc_islower = wc_islower_libc_mb,
	.wc_isgraph = wc_isgraph_libc_mb,
	.wc_isprint = wc_isprint_libc_mb,
	.wc_ispunct = wc_ispunct_libc_mb,
	.wc_isspace = wc_isspace_libc_mb,
	.char_is_cased = char_is_cased_libc,
	.char_tolower = char_tolower_libc,
	.wc_toupper = toupper_libc_mb,
	.wc_tolower = tolower_libc_mb,
};

static const struct collate_methods collate_methods_libc = {
	.strncoll = strncoll_libc,
	.strnxfrm = strnxfrm_libc,
	.strnxfrm_prefix = NULL,

	/*
	 * Unfortunately, it seems that strxfrm() for non-C collations is broken
	 * on many common platforms; testing of multiple versions of glibc reveals
	 * that, for many locales, strcoll() and strxfrm() do not return
	 * consistent results. While no other libc other than Cygwin has so far
	 * been shown to have a problem, we take the conservative course of action
	 * for right now and disable this categorically.  (Users who are certain
	 * this isn't a problem on their system can define TRUST_STRXFRM.)
	 */
#ifdef TRUST_STRXFRM
	.strxfrm_is_safe = true,
#else
	.strxfrm_is_safe = false,
#endif
};

#ifdef WIN32
static const struct collate_methods collate_methods_libc_win32_utf8 = {
	.strncoll = strncoll_libc_win32_utf8,
	.strnxfrm = strnxfrm_libc,
	.strnxfrm_prefix = NULL,
#ifdef TRUST_STRXFRM
	.strxfrm_is_safe = true,
#else
	.strxfrm_is_safe = false,
#endif
};
#endif

static size_t
strlower_libc_sb(char *dest, size_t destsize, const char *src, ssize_t srclen,
				 pg_locale_t locale)
{
	if (srclen < 0)
		srclen = strlen(src);

	if (srclen + 1 <= destsize)
	{
		locale_t	loc = locale->info.lt;
		char	   *p;

		if (srclen + 1 > destsize)
			return srclen;

		memcpy(dest, src, srclen);
		dest[srclen] = '\0';

		/*
		 * Note: we assume that tolower_l() will not be so broken as to need
		 * an isupper_l() guard test.  When using the default collation, we
		 * apply the traditional Postgres behavior that forces ASCII-style
		 * treatment of I/i, but in non-default collations you get exactly
		 * what the collation says.
		 */
		for (p = dest; *p; p++)
		{
			if (locale->is_default)
				*p = pg_tolower((unsigned char) *p);
			else
				*p = tolower_l((unsigned char) *p, loc);
		}
	}

	return srclen;
}

static size_t
strlower_libc_mb(char *dest, size_t destsize, const char *src, ssize_t srclen,
				 pg_locale_t locale)
{
	locale_t	loc = locale->info.lt;
	size_t		result_size;
	wchar_t    *workspace;
	char	   *result;
	size_t		curr_char;
	size_t		max_size;

	if (srclen < 0)
		srclen = strlen(src);

	/* Overflow paranoia */
	if ((srclen + 1) > (INT_MAX / sizeof(wchar_t)))
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/* Output workspace cannot have more codes than input bytes */
	workspace = (wchar_t *) palloc((srclen + 1) * sizeof(wchar_t));

	char2wchar(workspace, srclen + 1, src, srclen, loc);

	for (curr_char = 0; workspace[curr_char] != 0; curr_char++)
		workspace[curr_char] = towlower_l(workspace[curr_char], loc);

	/*
	 * Make result large enough; case change might change number of bytes
	 */
	max_size = curr_char * pg_database_encoding_max_length();
	result = palloc(max_size + 1);

	result_size = wchar2char(result, workspace, max_size + 1, loc);

	if (result_size + 1 > destsize)
		return result_size;

	memcpy(dest, result, result_size);
	dest[result_size] = '\0';

	pfree(workspace);
	pfree(result);

	return result_size;
}

static size_t
strtitle_libc_sb(char *dest, size_t destsize, const char *src, ssize_t srclen,
				 pg_locale_t locale)
{
	if (srclen < 0)
		srclen = strlen(src);

	if (srclen + 1 <= destsize)
	{
		locale_t	loc = locale->info.lt;
		int			wasalnum = false;
		char	   *p;

		memcpy(dest, src, srclen);
		dest[srclen] = '\0';

		/*
		 * Note: we assume that toupper_l()/tolower_l() will not be so broken
		 * as to need guard tests.  When using the default collation, we apply
		 * the traditional Postgres behavior that forces ASCII-style treatment
		 * of I/i, but in non-default collations you get exactly what the
		 * collation says.
		 */
		for (p = dest; *p; p++)
		{
			if (locale->is_default)
			{
				if (wasalnum)
					*p = pg_tolower((unsigned char) *p);
				else
					*p = pg_toupper((unsigned char) *p);
			}
			else
			{
				if (wasalnum)
					*p = tolower_l((unsigned char) *p, loc);
				else
					*p = toupper_l((unsigned char) *p, loc);
			}
			wasalnum = isalnum_l((unsigned char) *p, loc);
		}
	}

	return srclen;
}

static size_t
strtitle_libc_mb(char *dest, size_t destsize, const char *src, ssize_t srclen,
				 pg_locale_t locale)
{
	locale_t	loc = locale->info.lt;
	int			wasalnum = false;
	size_t		result_size;
	wchar_t    *workspace;
	char	   *result;
	size_t		curr_char;
	size_t		max_size;

	if (srclen < 0)
		srclen = strlen(src);

	/* Overflow paranoia */
	if ((srclen + 1) > (INT_MAX / sizeof(wchar_t)))
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/* Output workspace cannot have more codes than input bytes */
	workspace = (wchar_t *) palloc((srclen + 1) * sizeof(wchar_t));

	char2wchar(workspace, srclen + 1, src, srclen, loc);

	for (curr_char = 0; workspace[curr_char] != 0; curr_char++)
	{
		if (wasalnum)
			workspace[curr_char] = towlower_l(workspace[curr_char], loc);
		else
			workspace[curr_char] = towupper_l(workspace[curr_char], loc);
		wasalnum = iswalnum_l(workspace[curr_char], loc);
	}

	/*
	 * Make result large enough; case change might change number of bytes
	 */
	max_size = curr_char * pg_database_encoding_max_length();
	result = palloc(max_size + 1);

	result_size = wchar2char(result, workspace, max_size + 1, loc);

	if (result_size + 1 > destsize)
		return result_size;

	memcpy(dest, result, result_size);
	dest[result_size] = '\0';

	pfree(workspace);
	pfree(result);

	return result_size;
}

static size_t
strupper_libc_sb(char *dest, size_t destsize, const char *src, ssize_t srclen,
				 pg_locale_t locale)
{
	if (srclen < 0)
		srclen = strlen(src);

	if (srclen + 1 <= destsize)
	{
		locale_t	loc = locale->info.lt;
		char	   *p;

		memcpy(dest, src, srclen);
		dest[srclen] = '\0';

		/*
		 * Note: we assume that toupper_l() will not be so broken as to need
		 * an islower_l() guard test.  When using the default collation, we
		 * apply the traditional Postgres behavior that forces ASCII-style
		 * treatment of I/i, but in non-default collations you get exactly
		 * what the collation says.
		 */
		for (p = dest; *p; p++)
		{
			if (locale->is_default)
				*p = pg_toupper((unsigned char) *p);
			else
				*p = toupper_l((unsigned char) *p, loc);
		}
	}

	return srclen;
}

static size_t
strupper_libc_mb(char *dest, size_t destsize, const char *src, ssize_t srclen,
				 pg_locale_t locale)
{
	locale_t	loc = locale->info.lt;
	size_t		result_size;
	wchar_t    *workspace;
	char	   *result;
	size_t		curr_char;
	size_t		max_size;

	if (srclen < 0)
		srclen = strlen(src);

	/* Overflow paranoia */
	if ((srclen + 1) > (INT_MAX / sizeof(wchar_t)))
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/* Output workspace cannot have more codes than input bytes */
	workspace = (wchar_t *) palloc((srclen + 1) * sizeof(wchar_t));

	char2wchar(workspace, srclen + 1, src, srclen, loc);

	for (curr_char = 0; workspace[curr_char] != 0; curr_char++)
		workspace[curr_char] = towupper_l(workspace[curr_char], loc);

	/*
	 * Make result large enough; case change might change number of bytes
	 */
	max_size = curr_char * pg_database_encoding_max_length();
	result = palloc(max_size + 1);

	result_size = wchar2char(result, workspace, max_size + 1, loc);

	if (result_size + 1 > destsize)
		return result_size;

	memcpy(dest, result, result_size);
	dest[result_size] = '\0';

	pfree(workspace);
	pfree(result);

	return result_size;
}

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
	result->deterministic = true;
	result->collate_is_c = (strcmp(collate, "C") == 0) ||
		(strcmp(collate, "POSIX") == 0);
	result->ctype_is_c = (strcmp(ctype, "C") == 0) ||
		(strcmp(ctype, "POSIX") == 0);
	result->info.lt = loc;
	if (!result->collate_is_c)
	{
#ifdef WIN32
		if (GetDatabaseEncoding() == PG_UTF8)
			result->collate = &collate_methods_libc_win32_utf8;
		else
#endif
			result->collate = &collate_methods_libc;
	}
	if (!result->ctype_is_c)
	{
		if (GetDatabaseEncoding() == PG_UTF8)
			result->ctype = &ctype_methods_libc_utf8;
		else if (pg_database_encoding_max_length() > 1)
			result->ctype = &ctype_methods_libc_other_mb;
		else
			result->ctype = &ctype_methods_libc_sb;
	}

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

char *
get_collation_actual_version_libc(const char *collcollate)
{
	char	   *collversion = NULL;

	if (pg_strcasecmp("C", collcollate) != 0 &&
		pg_strncasecmp("C.", collcollate, 2) != 0 &&
		pg_strcasecmp("POSIX", collcollate) != 0)
	{
#if defined(__GLIBC__)
		/* Use the glibc version because we don't have anything better. */
		collversion = pstrdup(gnu_get_libc_version());
#elif defined(LC_VERSION_MASK)
		locale_t	loc;

		/* Look up FreeBSD collation version. */
		loc = newlocale(LC_COLLATE_MASK, collcollate, NULL);
		if (loc)
		{
			collversion =
				pstrdup(querylocale(LC_COLLATE_MASK | LC_VERSION_MASK, loc));
			freelocale(loc);
		}
		else
			ereport(ERROR,
					(errmsg("could not load locale \"%s\"", collcollate)));
#elif defined(WIN32)
		/*
		 * If we are targeting Windows Vista and above, we can ask for a name
		 * given a collation name (earlier versions required a location code
		 * that we don't have).
		 */
		NLSVERSIONINFOEX version = {sizeof(NLSVERSIONINFOEX)};
		WCHAR		wide_collcollate[LOCALE_NAME_MAX_LENGTH];

		MultiByteToWideChar(CP_ACP, 0, collcollate, -1, wide_collcollate,
							LOCALE_NAME_MAX_LENGTH);
		if (!GetNLSVersionEx(COMPARE_STRING, wide_collcollate, &version))
		{
			/*
			 * GetNLSVersionEx() wants a language tag such as "en-US", not a
			 * locale name like "English_United States.1252".  Until those
			 * values can be prevented from entering the system, or 100%
			 * reliably converted to the more useful tag format, tolerate the
			 * resulting error and report that we have no version data.
			 */
			if (GetLastError() == ERROR_INVALID_PARAMETER)
				return NULL;

			ereport(ERROR,
					(errmsg("could not get collation version for locale \"%s\": error code %lu",
							collcollate,
							GetLastError())));
		}
		collversion = psprintf("%lu.%lu,%lu.%lu",
							   (version.dwNLSVersion >> 8) & 0xFFFF,
							   version.dwNLSVersion & 0xFF,
							   (version.dwDefinedVersion >> 8) & 0xFFFF,
							   version.dwDefinedVersion & 0xFF);
#endif
	}

	return collversion;
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
void
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
wchar2char(char *to, const wchar_t *from, size_t tolen, locale_t loc)
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
	if (loc == (locale_t) 0)
	{
		/* Use wcstombs directly for the default locale */
		result = wcstombs(to, from, tolen);
	}
	else
	{
		/* Use wcstombs_l for nondefault locales */
		result = wcstombs_l(to, from, tolen, loc);
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
		   locale_t loc)
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

		if (loc == (locale_t) 0)
		{
			/* Use mbstowcs directly for the default locale */
			result = mbstowcs(to, str, tolen);
		}
		else
		{
			/* Use mbstowcs_l for nondefault locales */
			result = mbstowcs_l(to, str, tolen, loc);
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
