/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities for builtin provider
 *
 * Portions Copyright (c) 2002-2025, PostgreSQL Global Development Group
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
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/pg_locale.h"
#include "utils/syscache.h"

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
 * Simple word boundary iterator that draws boundaries each time the result of
 * pg_u_isalnum() changes.
 */
static size_t
initcap_wbnext(void *state)
{
	struct WordBoundaryState *wbstate = (struct WordBoundaryState *) state;

	while (wbstate->offset < wbstate->len &&
		   wbstate->str[wbstate->offset] != '\0')
	{
		pg_wchar	u = utf8_to_unicode((unsigned char *) wbstate->str +
										wbstate->offset);
		bool		curr_alnum = pg_u_isalnum(u, wbstate->posix);

		if (!wbstate->init || curr_alnum != wbstate->prev_alnum)
		{
			size_t		prev_offset = wbstate->offset;

			wbstate->init = true;
			wbstate->offset += unicode_utf8len(u);
			wbstate->prev_alnum = curr_alnum;
			return prev_offset;
		}

		wbstate->offset += unicode_utf8len(u);
	}

	return wbstate->len;
}

static size_t
strlower_builtin(char *dest, size_t destsize, const char *src, ssize_t srclen,
				 pg_locale_t locale)
{
	return unicode_strlower(dest, destsize, src, srclen,
							locale->info.builtin.casemap_full);
}

static size_t
strtitle_builtin(char *dest, size_t destsize, const char *src, ssize_t srclen,
				 pg_locale_t locale)
{
	struct WordBoundaryState wbstate = {
		.str = src,
		.len = srclen,
		.offset = 0,
		.posix = !locale->info.builtin.casemap_full,
		.init = false,
		.prev_alnum = false,
	};

	return unicode_strtitle(dest, destsize, src, srclen,
							locale->info.builtin.casemap_full,
							initcap_wbnext, &wbstate);
}

static size_t
strupper_builtin(char *dest, size_t destsize, const char *src, ssize_t srclen,
				 pg_locale_t locale)
{
	return unicode_strupper(dest, destsize, src, srclen,
							locale->info.builtin.casemap_full);
}

static size_t
strfold_builtin(char *dest, size_t destsize, const char *src, ssize_t srclen,
				pg_locale_t locale)
{
	return unicode_strfold(dest, destsize, src, srclen,
						   locale->info.builtin.casemap_full);
}

static bool
wc_isdigit_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isdigit(wc, !locale->info.builtin.casemap_full);
}

static bool
wc_isalpha_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isalpha(wc);
}

static bool
wc_isalnum_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isalnum(wc, !locale->info.builtin.casemap_full);
}

static bool
wc_isupper_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isupper(wc);
}

static bool
wc_islower_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_islower(wc);
}

static bool
wc_isgraph_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isgraph(wc);
}

static bool
wc_isprint_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isprint(wc);
}

static bool
wc_ispunct_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_ispunct(wc, !locale->info.builtin.casemap_full);
}

static bool
wc_isspace_builtin(pg_wchar wc, pg_locale_t locale)
{
	return pg_u_isspace(wc);
}

static bool
char_is_cased_builtin(char ch, pg_locale_t locale)
{
	return IS_HIGHBIT_SET(ch) ||
		(ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

static pg_wchar
wc_toupper_builtin(pg_wchar wc, pg_locale_t locale)
{
	return unicode_uppercase_simple(wc);
}

static pg_wchar
wc_tolower_builtin(pg_wchar wc, pg_locale_t locale)
{
	return unicode_lowercase_simple(wc);
}

static const struct ctype_methods ctype_methods_builtin = {
	.strlower = strlower_builtin,
	.strtitle = strtitle_builtin,
	.strupper = strupper_builtin,
	.strfold = strfold_builtin,
	.wc_isdigit = wc_isdigit_builtin,
	.wc_isalpha = wc_isalpha_builtin,
	.wc_isalnum = wc_isalnum_builtin,
	.wc_isupper = wc_isupper_builtin,
	.wc_islower = wc_islower_builtin,
	.wc_isgraph = wc_isgraph_builtin,
	.wc_isprint = wc_isprint_builtin,
	.wc_ispunct = wc_ispunct_builtin,
	.wc_isspace = wc_isspace_builtin,
	.char_is_cased = char_is_cased_builtin,
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

	result->info.builtin.locale = MemoryContextStrdup(context, locstr);
	result->info.builtin.casemap_full = (strcmp(locstr, "PG_UNICODE_FAST") == 0);
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
	 * The only two supported locales (C and C.UTF-8) are both based on memcmp
	 * and are not expected to change, but track the version anyway.
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
