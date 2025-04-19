/*-------------------------------------------------------------------------
 *
 * pg_localeconv_r.c
 *    Thread-safe implementations of localeconv()
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    src/port/pg_localeconv_r.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#if !defined(WIN32)
#include <langinfo.h>
#include <pthread.h>
#endif

#include <limits.h>

#ifdef MON_THOUSANDS_SEP
/*
 * One of glibc's extended langinfo items detected.  Assume that the full set
 * is present, which means we can use nl_langinfo_l() instead of localeconv().
 */
#define TRANSLATE_FROM_LANGINFO
#endif

struct lconv_member_info
{
	bool		is_string;
	int			category;
	size_t		offset;
#ifdef TRANSLATE_FROM_LANGINFO
	nl_item		item;
#endif
};

/* Some macros to declare the lconv members compactly. */
#ifdef TRANSLATE_FROM_LANGINFO
#define LCONV_M(is_string, category, name, item)						\
	{ is_string, category, offsetof(struct lconv, name), item }
#else
#define LCONV_M(is_string, category, name, item)			\
	{ is_string, category, offsetof(struct lconv, name) }
#endif
#define LCONV_S(c, n, i) LCONV_M(true,  c, n, i)
#define LCONV_C(c, n, i) LCONV_M(false, c, n, i)

/*
 * The work of populating lconv objects is driven by this table.  Since we
 * tolerate non-matching encodings in LC_NUMERIC and LC_MONETARY, we have to
 * call the underlying OS routine multiple times, with the correct locales.
 * The first column of this table says which locale category applies to each struct
 * member.  The second column is the name of the struct member.  The third
 * column is the name of the nl_item, if translating from nl_langinfo_l() (it's
 * always the member name, in upper case).
 */
static const struct lconv_member_info table[] = {
	/* String fields. */
	LCONV_S(LC_NUMERIC, decimal_point, DECIMAL_POINT),
	LCONV_S(LC_NUMERIC, thousands_sep, THOUSANDS_SEP),
	LCONV_S(LC_NUMERIC, grouping, GROUPING),
	LCONV_S(LC_MONETARY, int_curr_symbol, INT_CURR_SYMBOL),
	LCONV_S(LC_MONETARY, currency_symbol, CURRENCY_SYMBOL),
	LCONV_S(LC_MONETARY, mon_decimal_point, MON_DECIMAL_POINT),
	LCONV_S(LC_MONETARY, mon_thousands_sep, MON_THOUSANDS_SEP),
	LCONV_S(LC_MONETARY, mon_grouping, MON_GROUPING),
	LCONV_S(LC_MONETARY, positive_sign, POSITIVE_SIGN),
	LCONV_S(LC_MONETARY, negative_sign, NEGATIVE_SIGN),

	/* Character fields. */
	LCONV_C(LC_MONETARY, int_frac_digits, INT_FRAC_DIGITS),
	LCONV_C(LC_MONETARY, frac_digits, FRAC_DIGITS),
	LCONV_C(LC_MONETARY, p_cs_precedes, P_CS_PRECEDES),
	LCONV_C(LC_MONETARY, p_sep_by_space, P_SEP_BY_SPACE),
	LCONV_C(LC_MONETARY, n_cs_precedes, N_CS_PRECEDES),
	LCONV_C(LC_MONETARY, n_sep_by_space, N_SEP_BY_SPACE),
	LCONV_C(LC_MONETARY, p_sign_posn, P_SIGN_POSN),
	LCONV_C(LC_MONETARY, n_sign_posn, N_SIGN_POSN),
};

static inline char **
lconv_string_member(struct lconv *lconv, int i)
{
	return (char **) ((char *) lconv + table[i].offset);
}

static inline char *
lconv_char_member(struct lconv *lconv, int i)
{
	return (char *) lconv + table[i].offset;
}

/*
 * Free the members of a struct lconv populated by pg_localeconv_r().  The
 * struct itself is in storage provided by the caller of pg_localeconv_r().
 */
void
pg_localeconv_free(struct lconv *lconv)
{
	for (int i = 0; i < lengthof(table); ++i)
		if (table[i].is_string)
			free(*lconv_string_member(lconv, i));
}

#ifdef TRANSLATE_FROM_LANGINFO
/*
 * Fill in struct lconv members using the equivalent nl_langinfo_l() items.
 */
static int
pg_localeconv_from_langinfo(struct lconv *dst,
							locale_t monetary_locale,
							locale_t numeric_locale)
{
	for (int i = 0; i < lengthof(table); ++i)
	{
		locale_t	locale;

		locale = table[i].category == LC_NUMERIC ?
			numeric_locale : monetary_locale;

		if (table[i].is_string)
		{
			char	   *string;

			string = nl_langinfo_l(table[i].item, locale);
			if (!(string = strdup(string)))
			{
				pg_localeconv_free(dst);
				errno = ENOMEM;
				return -1;
			}
			*lconv_string_member(dst, i) = string;
		}
		else
		{
			*lconv_char_member(dst, i) =
				*nl_langinfo_l(table[i].item, locale);
		}
	}

	return 0;
}
#else							/* not TRANSLATE_FROM_LANGINFO */
/*
 * Copy members from a given category.  Note that you have to call this twice
 * to copy the LC_MONETARY and then LC_NUMERIC members.
 */
static int
pg_localeconv_copy_members(struct lconv *dst,
						   struct lconv *src,
						   int category)
{
	for (int i = 0; i < lengthof(table); ++i)
	{
		if (table[i].category != category)
			continue;

		if (table[i].is_string)
		{
			char	   *string;

			string = *lconv_string_member(src, i);
			if (!(string = strdup(string)))
			{
				pg_localeconv_free(dst);
				errno = ENOMEM;
				return -1;
			}
			*lconv_string_member(dst, i) = string;
		}
		else
		{
			*lconv_char_member(dst, i) = *lconv_char_member(src, i);
		}
	}

	return 0;
}
#endif							/* not TRANSLATE_FROM_LANGINFO */

/*
 * A thread-safe routine to get a copy of the lconv struct for a given
 * LC_NUMERIC and LC_MONETARY.  Different approaches are used on different
 * OSes, because the standard interface is so multi-threading unfriendly.
 *
 * 1.  On Windows, there is no uselocale(), but there is a way to put
 * setlocale() into a thread-local mode temporarily.  Its localeconv() is
 * documented as returning a pointer to thread-local storage, so we don't have
 * to worry about concurrent callers.
 *
 * 2.  On Glibc, as an extension, all the information required to populate
 * struct lconv is also available via nl_langpath_l(), which is thread-safe.
 *
 * 3.  On macOS and *BSD, there is localeconv_l(), so we can create a temporary
 * locale_t to pass in, and the result is a pointer to storage associated with
 * the locale_t so we control its lifetime and we don't have to worry about
 * concurrent calls clobbering it.
 *
 * 4.  Otherwise, we wrap plain old localeconv() in uselocale() to avoid
 * touching the global locale, but the output buffer is allowed by the standard
 * to be overwritten by concurrent calls to localeconv().  We protect against
 * _this_ function doing that with a Big Lock, but there isn't much we can do
 * about code outside our tree that might call localeconv(), given such a poor
 * interface.
 *
 * The POSIX standard explicitly says that it is undefined what happens if
 * LC_MONETARY or LC_NUMERIC imply an encoding (codeset) different from that
 * implied by LC_CTYPE.  In practice, all Unix-ish platforms seem to believe
 * that localeconv() should return strings that are encoded in the codeset
 * implied by the LC_MONETARY or LC_NUMERIC locale name.  On Windows, LC_CTYPE
 * has to match to get sane results.
 *
 * To get predictable results on all platforms, we'll call the underlying
 * routines with LC_ALL set to the appropriate locale for each set of members,
 * and merge the results.  Three members of the resulting object are therefore
 * guaranteed to be encoded with LC_NUMERIC's codeset: "decimal_point",
 * "thousands_sep" and "grouping".  All other members are encoded with
 * LC_MONETARY's codeset.
 *
 * Returns 0 on success.  Returns non-zero on failure, and sets errno.  On
 * success, the caller is responsible for calling pg_localeconv_free() on the
 * output struct to free the string members it contains.
 */
int
pg_localeconv_r(const char *lc_monetary,
				const char *lc_numeric,
				struct lconv *output)
{
#ifdef WIN32
	wchar_t    *save_lc_ctype = NULL;
	wchar_t    *save_lc_monetary = NULL;
	wchar_t    *save_lc_numeric = NULL;
	int			save_config_thread_locale;
	int			result = -1;

	/* Put setlocale() into thread-local mode. */
	save_config_thread_locale = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);

	/*
	 * Capture the current values as wide strings.  Otherwise, we might not be
	 * able to restore them if their names contain non-ASCII characters and
	 * the intermediate locale changes the expected encoding.  We don't want
	 * to leave the caller in an unexpected state by failing to restore, or
	 * crash the runtime library.
	 */
	save_lc_ctype = _wsetlocale(LC_CTYPE, NULL);
	if (!save_lc_ctype || !(save_lc_ctype = wcsdup(save_lc_ctype)))
		goto exit;
	save_lc_monetary = _wsetlocale(LC_MONETARY, NULL);
	if (!save_lc_monetary || !(save_lc_monetary = wcsdup(save_lc_monetary)))
		goto exit;
	save_lc_numeric = _wsetlocale(LC_NUMERIC, NULL);
	if (!save_lc_numeric || !(save_lc_numeric = wcsdup(save_lc_numeric)))
		goto exit;

	memset(output, 0, sizeof(*output));

	/* Copy the LC_MONETARY members. */
	if (!setlocale(LC_ALL, lc_monetary))
		goto exit;
	result = pg_localeconv_copy_members(output, localeconv(), LC_MONETARY);
	if (result != 0)
		goto exit;

	/* Copy the LC_NUMERIC members. */
	if (!setlocale(LC_ALL, lc_numeric))
		goto exit;
	result = pg_localeconv_copy_members(output, localeconv(), LC_NUMERIC);

exit:
	/* Restore everything we changed. */
	if (save_lc_ctype)
	{
		_wsetlocale(LC_CTYPE, save_lc_ctype);
		free(save_lc_ctype);
	}
	if (save_lc_monetary)
	{
		_wsetlocale(LC_MONETARY, save_lc_monetary);
		free(save_lc_monetary);
	}
	if (save_lc_numeric)
	{
		_wsetlocale(LC_NUMERIC, save_lc_numeric);
		free(save_lc_numeric);
	}
	_configthreadlocale(save_config_thread_locale);

	return result;

#else							/* !WIN32 */
	locale_t	monetary_locale;
	locale_t	numeric_locale;
	int			result;

	/*
	 * All variations on Unix require locale_t objects for LC_MONETARY and
	 * LC_NUMERIC.  We'll set all locale categories, so that we can don't have
	 * to worry about POSIX's undefined behavior if LC_CTYPE's encoding
	 * doesn't match.
	 */
	errno = ENOENT;
	monetary_locale = newlocale(LC_ALL_MASK, lc_monetary, 0);
	if (monetary_locale == 0)
		return -1;
	numeric_locale = newlocale(LC_ALL_MASK, lc_numeric, 0);
	if (numeric_locale == 0)
	{
		freelocale(monetary_locale);
		return -1;
	}

	memset(output, 0, sizeof(*output));
#if defined(TRANSLATE_FROM_LANGINFO)
	/* Copy from non-standard nl_langinfo_l() extended items. */
	result = pg_localeconv_from_langinfo(output,
										 monetary_locale,
										 numeric_locale);
#elif defined(HAVE_LOCALECONV_L)
	/* Copy the LC_MONETARY members from a thread-safe lconv object. */
	result = pg_localeconv_copy_members(output,
										localeconv_l(monetary_locale),
										LC_MONETARY);
	if (result == 0)
	{
		/* Copy the LC_NUMERIC members from a thread-safe lconv object. */
		result = pg_localeconv_copy_members(output,
											localeconv_l(numeric_locale),
											LC_NUMERIC);
	}
#else
	/* We have nothing better than standard POSIX facilities. */
	{
		static pthread_mutex_t big_lock = PTHREAD_MUTEX_INITIALIZER;
		locale_t	save_locale;

		pthread_mutex_lock(&big_lock);
		/* Copy the LC_MONETARY members. */
		save_locale = uselocale(monetary_locale);
		result = pg_localeconv_copy_members(output,
											localeconv(),
											LC_MONETARY);
		if (result == 0)
		{
			/* Copy the LC_NUMERIC members. */
			uselocale(numeric_locale);
			result = pg_localeconv_copy_members(output,
												localeconv(),
												LC_NUMERIC);
		}
		pthread_mutex_unlock(&big_lock);

		uselocale(save_locale);
	}
#endif

	freelocale(monetary_locale);
	freelocale(numeric_locale);

	return result;
#endif							/* !WIN32 */
}
