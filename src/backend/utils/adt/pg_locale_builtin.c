/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities for builtin provider
 *
 * Portions Copyright (c) 2002-2026, PostgreSQL Global Development Group
 *
 * src/backend/utils/adt/pg_locale_builtin.c
 *
 *-----------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_database.h"
#include "catalog/pg_collation.h"
#include "common/unicode_case.h"
#include "common/unicode_category.h"
#include "common/unicode_limits.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/syscache.h"

/*
 * The largest text value must fit in MaxAllocSize, but then may grow during
 * case mapping. While the resulting string will not be representable as a new
 * text value, we must at least be sure not to overflow a size_t while
 * processing it.
 */
StaticAssertDecl(SIZE_MAX / UTF8_MAX_CASEMAP_EXPANSION > MaxAllocSize,
				 "case mapping may overflow size_t");

extern pg_locale_t create_pg_locale_builtin(Oid collid,
											MemoryContext context);
extern char *get_collation_actual_version_builtin(const char *collcollate);

struct WordBoundaryState
{
	const char *str;
	size_t		len;
	size_t		offset;
	bool		posix;
	bool		init;
	bool		prev_alnum;
};

/*
 * In UTF-8, pg_wchar is guaranteed to be the code point value.
 */
static inline char32_t
to_char32(pg_wchar wc)
{
	Assert(GetDatabaseEncoding() == PG_UTF8);
	return (char32_t) wc;
}

static inline pg_wchar
to_pg_wchar(char32_t c32)
{
	Assert(GetDatabaseEncoding() == PG_UTF8);
	return (pg_wchar) c32;
}

/*
 * Simple word boundary iterator that draws boundaries each time the result of
 * pg_u_isalnum() changes.
 */
static size_t
initcap_wbnext(void *state)
{
	struct WordBoundaryState *wbstate = (struct WordBoundaryState *) state;

	while (wbstate->offset < wbstate->len)
	{
		int			ulen = pg_utf_mblen((const unsigned char *) wbstate->str +
										wbstate->offset);
		char32_t	u;
		bool		curr_alnum;
		size_t		prev_offset = wbstate->offset;

		/* invalid UTF8 */
		if (wbstate->offset + ulen > wbstate->len)
		{
			wbstate->init = true;
			wbstate->offset = wbstate->len;
			return prev_offset;
		}

		u = utf8_to_unicode((const unsigned char *) wbstate->str +
							wbstate->offset);
		curr_alnum = pg_u_isalnum(u, wbstate->posix);

		if (!wbstate->init || curr_alnum != wbstate->prev_alnum)
		{
			wbstate->init = true;
			wbstate->offset += ulen;
			wbstate->prev_alnum = curr_alnum;
			return prev_offset;
		}

		wbstate->offset += ulen;
	}

	return wbstate->len;
}

static size_t
strlower_builtin(char *dest, size_t destsize, const char *src, size_t srclen,
				 pg_locale_t locale)
{
	size_t		consumed;
	size_t		result;

	result = unicode_strlower(dest, destsize, src, srclen, &consumed,
							  locale->builtin.casemap_full);
	if (consumed < srclen)
		report_invalid_encoding(GetDatabaseEncoding(), src + consumed,
								srclen - consumed);

	return result;
}

static size_t
strtitle_builtin(char *dest, size_t destsize, const char *src, size_t srclen,
				 pg_locale_t locale)
{
	struct WordBoundaryState wbstate = {
		.str = src,
		.len = srclen,
		.offset = 0,
		.posix = !locale->builtin.casemap_full,
		.init = false,
		.prev_alnum = false,
	};
	size_t		consumed;
	size_t		result;

	result = unicode_strtitle(dest, destsize, src, srclen, &consumed,
							  locale->builtin.casemap_full,
							  initcap_wbnext, &wbstate);

	if (consumed < srclen)
		report_invalid_encoding(GetDatabaseEncoding(), src + consumed,
								srclen - consumed);

	return result;
}

static size_t
strupper_builtin(char *dest, size_t destsize, const char *src, size_t srclen,
				 pg_locale_t locale)
{
	size_t		consumed;
	size_t		result;

	result = unicode_strupper(dest, destsize, src, srclen, &consumed,
							  locale->builtin.casemap_full);
	if (consumed < srclen)
		report_invalid_encoding(GetDatabaseEncoding(), src + consumed,
								srclen - consumed);

	return result;
}

static size_t
strfold_builtin(char *dest, size_t destsize, const char *src, size_t srclen,
				pg_locale_t locale)
{
	size_t		consumed;
	size_t		result;

	result = unicode_strfold(dest, destsize, src, srclen, &consumed,
							 locale->builtin.casemap_full);
	if (consumed < srclen)
		report_invalid_encoding(GetDatabaseEncoding(), src + consumed,
								srclen - consumed);

	return result;
}

static bool
wc_isdigit_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isdigit(to_char32(wc), !locale->builtin.casemap_full);
}

static bool
wc_isalpha_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isalpha(to_char32(wc));
}

static bool
wc_isalnum_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isalnum(to_char32(wc), !locale->builtin.casemap_full);
}

static bool
wc_isupper_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isupper(to_char32(wc));
}

static bool
wc_islower_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_islower(to_char32(wc));
}

static bool
wc_isgraph_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isgraph(to_char32(wc));
}

static bool
wc_isprint_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isprint(to_char32(wc));
}

static bool
wc_ispunct_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_ispunct(to_char32(wc), !locale->builtin.casemap_full);
}

static bool
wc_isspace_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isspace(to_char32(wc));
}

static bool
wc_isxdigit_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isxdigit(to_char32(wc), !locale->builtin.casemap_full);
}

static bool
wc_iscased_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_prop_cased(to_char32(wc));
}

static pg_wchar
wc_toupper_builtin(pg_wchar wc, pg_locale_t locale)
{
	return to_pg_wchar(unicode_uppercase_simple(to_char32(wc)));
}

static pg_wchar
wc_tolower_builtin(pg_wchar wc, pg_locale_t locale)
{
	return to_pg_wchar(unicode_lowercase_simple(to_char32(wc)));
}

static const struct ctype_methods ctype_methods_builtin = {
	.strlower = strlower_builtin,
	.strtitle = strtitle_builtin,
	.strupper = strupper_builtin,
	.strfold = strfold_builtin,
	/* uses plain ASCII semantics for historical reasons */
	.downcase_ident = NULL,
	.wc_isdigit = wc_isdigit_builtin,
	.wc_isalpha = wc_isalpha_builtin,
	.wc_isalnum = wc_isalnum_builtin,
	.wc_isupper = wc_isupper_builtin,
	.wc_islower = wc_islower_builtin,
	.wc_isgraph = wc_isgraph_builtin,
	.wc_isprint = wc_isprint_builtin,
	.wc_ispunct = wc_ispunct_builtin,
	.wc_isspace = wc_isspace_builtin,
	.wc_isxdigit = wc_isxdigit_builtin,
	.wc_iscased = wc_iscased_builtin,
	.wc_tolower = wc_tolower_builtin,
	.wc_toupper = wc_toupper_builtin,
};

pg_locale_t
create_pg_locale_builtin(Oid collid, MemoryContext context)
{
	const char *locstr;
	pg_locale_t result;

	if (collid == DEFAULT_COLLATION_OID)
	{
		HeapTuple	tp;
		Datum		datum;

		tp = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
		datum = SysCacheGetAttrNotNull(DATABASEOID, tp,
									   Anum_pg_database_datlocale);
		locstr = TextDatumGetCString(datum);
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
									   Anum_pg_collation_colllocale);
		locstr = TextDatumGetCString(datum);
		ReleaseSysCache(tp);
	}

	builtin_validate_locale(GetDatabaseEncoding(), locstr);

	result = MemoryContextAllocZero(context, sizeof(struct pg_locale_struct));

	result->builtin.locale = MemoryContextStrdup(context, locstr);
	result->builtin.casemap_full = (strcmp(locstr, "PG_UNICODE_FAST") == 0);
	result->deterministic = true;
	result->collate_is_c = true;
	result->ctype_is_c = (strcmp(locstr, "C") == 0);
	if (!result->ctype_is_c)
		result->ctype = &ctype_methods_builtin;

	return result;
}

char *
get_collation_actual_version_builtin(const char *collcollate)
{
	/*
	 * The supported locales (C, C.UTF-8, and PG_UNICODE_FAST) are all based
	 * on memcmp and are not expected to change, but track the version anyway.
	 *
	 * Note that the character semantics may change for some locales, but the
	 * collation version only tracks changes to sort order.
	 */
	if (strcmp(collcollate, "C") == 0)
		return "1";
	else if (strcmp(collcollate, "C.UTF-8") == 0)
		return "1";
	else if (strcmp(collcollate, "PG_UNICODE_FAST") == 0)
		return "1";
	else
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("invalid locale name \"%s\" for builtin provider",
						collcollate)));

	return NULL;				/* keep compiler quiet */
}
