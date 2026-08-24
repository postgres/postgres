/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * src/include/utils/pg_locale.h
 *
 * Copyright (c) 2002-2026, PostgreSQL Global Development Group
 *
 *-----------------------------------------------------------------------
 */

#ifndef _PG_LOCALE_
#define _PG_LOCALE_

#include "common/unicode_limits.h"
#include "mb/pg_wchar.h"

/* use for libc locale names */
#define LOCALE_NAME_BUFLEN 128

/*
 * Expansion factor of string length, not including terminating NUL.  That is,
 * the upper bound of the number of multibyte characters in the result string
 * per multibyte character in the input string.
 *
 * NB: assumes no provider exceeds the Unicode-defined maximum.
 */
#define PG_MAX_CASEMAP_MBCHARS		UNICODE_MAX_CASEMAP_CODEPOINTS

/*
 * Expansion factor of a string in bytes, not including terminating NUL.
 *
 * This is a conservative upper bound, assuming that each character in the
 * input string is a 1-byte character that maps to PG_MAX_CASEMAP_MBCHARS
 * other characters, all requiring MAX_MULTIBYTE_CHAR_LEN bytes.
 */
#define PG_MAX_CASEMAP_EXPANSION	(PG_MAX_CASEMAP_MBCHARS * \
									 MAX_MULTIBYTE_CHAR_LEN)

/*
 * The maximum number of bytes needed to store the result of case mapping a
 * single multibyte character, including terminating NUL.
 *
 * This is a conservative upper bound, assuming that a single multibyte
 * character can expand into PG_MAX_CASEMAP_MBCHARS other multibyte
 * characters, each requiring MAX_MULTIBYTE_CHAR_LEN bytes.
 */
#define PG_CASEMAP_BUFSZ			((PG_MAX_CASEMAP_MBCHARS * \
									  MAX_MULTIBYTE_CHAR_LEN) + 1)

/* GUC settings */
extern PGDLLIMPORT char *locale_messages;
extern PGDLLIMPORT char *locale_monetary;
extern PGDLLIMPORT char *locale_numeric;
extern PGDLLIMPORT char *locale_time;
extern PGDLLIMPORT int icu_validation_level;

/* lc_time localization cache */
extern PGDLLIMPORT char *localized_abbrev_days[];
extern PGDLLIMPORT char *localized_full_days[];
extern PGDLLIMPORT char *localized_abbrev_months[];
extern PGDLLIMPORT char *localized_full_months[];

extern bool check_locale(int category, const char *locale, char **canonname);
extern char *pg_perm_setlocale(int category, const char *locale);

/*
 * Return the POSIX lconv struct (contains number/money formatting
 * information) with locale information for all categories.
 */
extern struct lconv *PGLC_localeconv(void);

extern void cache_locale_time(void);


struct pg_locale_struct;
typedef struct pg_locale_struct *pg_locale_t;

/*
 * Collation behavior: string ordering.
 */
struct collate_methods
{
	/* required */
	int			(*strncoll) (const char *arg1, size_t len1,
							 const char *arg2, size_t len2,
							 pg_locale_t locale);

	int			(*strcoll) (const char *arg1, const char *arg2,
							pg_locale_t locale);

	/* required */
	size_t		(*strnxfrm) (char *dest, size_t destsize,
							 const char *src, size_t srclen,
							 pg_locale_t locale);

	size_t		(*strxfrm) (char *dest, size_t destsize,
							const char *src, pg_locale_t locale);

	/* optional */
	size_t		(*strnxfrm_prefix) (char *dest, size_t destsize,
									const char *src, size_t srclen,
									pg_locale_t locale);

	size_t		(*strxfrm_prefix) (char *dest, size_t destsize,
								   const char *src, pg_locale_t locale);

	/*
	 * If the strnxfrm method is not trusted to return the correct results,
	 * set strxfrm_is_safe to false. If set to false, the method will not be
	 * used in most cases, but the planner still expects it to be there for
	 * estimation purposes (where incorrect results are acceptable).
	 */
	bool		strxfrm_is_safe;
};

/*
 * Character behavior: casing semantics and pattern matching.
 */
struct ctype_methods
{
	/* required */
	size_t		(*strlower) (char *dest, size_t destsize,
							 const char *src, size_t srclen,
							 pg_locale_t locale);
	size_t		(*strtitle) (char *dest, size_t destsize,
							 const char *src, size_t srclen,
							 pg_locale_t locale);
	size_t		(*strupper) (char *dest, size_t destsize,
							 const char *src, size_t srclen,
							 pg_locale_t locale);
	size_t		(*strfold) (char *dest, size_t destsize,
							const char *src, size_t srclen,
							pg_locale_t locale);

	/* optional */
	size_t		(*downcase_ident) (char *dest, size_t destsize,
								   const char *src, size_t srclen,
								   pg_locale_t locale);

	/* required */
	bool		(*wc_isdigit) (pg_wchar wc, pg_locale_t locale);
	bool		(*wc_isalpha) (pg_wchar wc, pg_locale_t locale);
	bool		(*wc_isalnum) (pg_wchar wc, pg_locale_t locale);
	bool		(*wc_isupper) (pg_wchar wc, pg_locale_t locale);
	bool		(*wc_islower) (pg_wchar wc, pg_locale_t locale);
	bool		(*wc_isgraph) (pg_wchar wc, pg_locale_t locale);
	bool		(*wc_isprint) (pg_wchar wc, pg_locale_t locale);
	bool		(*wc_ispunct) (pg_wchar wc, pg_locale_t locale);
	bool		(*wc_isspace) (pg_wchar wc, pg_locale_t locale);
	bool		(*wc_isxdigit) (pg_wchar wc, pg_locale_t locale);
	bool		(*wc_iscased) (pg_wchar wc, pg_locale_t locale);
	pg_wchar	(*wc_toupper) (pg_wchar wc, pg_locale_t locale);
	pg_wchar	(*wc_tolower) (pg_wchar wc, pg_locale_t locale);
};

/*
 * We use a discriminated union to hold either a locale_t or an ICU collator.
 * pg_locale_t is occasionally checked for truth, so make it a pointer.
 *
 * Also, hold two flags: whether the collation's LC_COLLATE or LC_CTYPE is C
 * (or POSIX), so we can optimize a few code paths in various places.  For the
 * built-in C and POSIX collations, we can know that without even doing a
 * cache lookup, but we want to support aliases for C/POSIX too.  For the
 * "default" collation, there are separate static cache variables, since
 * consulting the pg_collation catalog doesn't tell us what we need.
 *
 * Note that some code, such as wchar2char(), relies on the flags not
 * reporting false negatives (that is, saying it's not C when it is).
 */
struct pg_locale_struct
{
	bool		deterministic;
	bool		collate_is_c;
	bool		ctype_is_c;
	bool		is_default;

	const struct collate_methods *collate;	/* NULL if collate_is_c */
	const struct ctype_methods *ctype;	/* NULL if ctype_is_c */

	union
	{
		struct
		{
			const char *locale;
			bool		casemap_full;
		}			builtin;
		locale_t	lt;
#ifdef USE_ICU
		struct
		{
			const char *locale;
			struct UCollator *ucol;
			struct UCaseMap *ucasemap;
			locale_t	lt;
		}			icu;
#endif
	};
};

extern void init_database_collation(void);
extern pg_locale_t pg_database_locale(void);
extern pg_locale_t pg_newlocale_from_collation(Oid collid);

extern char *get_collation_actual_version(char collprovider, const char *collcollate);

extern size_t pg_strlower(char *dst, size_t dstsize,
						  const char *src, size_t srclen,
						  pg_locale_t locale);
extern size_t pg_strtitle(char *dst, size_t dstsize,
						  const char *src, size_t srclen,
						  pg_locale_t locale);
extern size_t pg_strupper(char *dst, size_t dstsize,
						  const char *src, size_t srclen,
						  pg_locale_t locale);
extern size_t pg_strfold(char *dst, size_t dstsize,
						 const char *src, size_t srclen,
						 pg_locale_t locale);
extern size_t pg_downcase_ident(char *dst, size_t dstsize,
								const char *src, size_t srclen);
extern int	pg_strcoll(const char *arg1, const char *arg2, pg_locale_t locale);
extern int	pg_strncoll(const char *arg1, size_t len1,
						const char *arg2, size_t len2, pg_locale_t locale);
extern bool pg_strxfrm_enabled(pg_locale_t locale);
extern size_t pg_strxfrm(char *dest, const char *src, size_t destsize,
						 pg_locale_t locale);
extern size_t pg_strnxfrm(char *dest, size_t destsize, const char *src,
						  size_t srclen, pg_locale_t locale);
extern bool pg_strxfrm_prefix_enabled(pg_locale_t locale);
extern size_t pg_strxfrm_prefix(char *dest, const char *src, size_t destsize,
								pg_locale_t locale);
extern size_t pg_strnxfrm_prefix(char *dest, size_t destsize, const char *src,
								 size_t srclen, pg_locale_t locale);

extern bool pg_iswdigit(pg_wchar wc, pg_locale_t locale);
extern bool pg_iswalpha(pg_wchar wc, pg_locale_t locale);
extern bool pg_iswalnum(pg_wchar wc, pg_locale_t locale);
extern bool pg_iswupper(pg_wchar wc, pg_locale_t locale);
extern bool pg_iswlower(pg_wchar wc, pg_locale_t locale);
extern bool pg_iswgraph(pg_wchar wc, pg_locale_t locale);
extern bool pg_iswprint(pg_wchar wc, pg_locale_t locale);
extern bool pg_iswpunct(pg_wchar wc, pg_locale_t locale);
extern bool pg_iswspace(pg_wchar wc, pg_locale_t locale);
extern bool pg_iswxdigit(pg_wchar wc, pg_locale_t locale);
extern bool pg_iswcased(pg_wchar wc, pg_locale_t locale);
extern pg_wchar pg_towupper(pg_wchar wc, pg_locale_t locale);
extern pg_wchar pg_towlower(pg_wchar wc, pg_locale_t locale);

extern const char *pg_icu_unicode_version(void);

extern int	builtin_locale_encoding(const char *locale);
extern const char *builtin_validate_locale(int encoding, const char *locale);
extern void icu_validate_locale(const char *loc_str);
extern char *icu_language_tag(const char *loc_str, int elevel);
extern void report_newlocale_failure(const char *localename);

/* This function converts from libc's wchar_t, *not* pg_wchar */
extern size_t wchar2char(char *to, const wchar_t *from, size_t tolen,
						 locale_t loc);

#endif							/* _PG_LOCALE_ */
