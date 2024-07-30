/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * src/include/utils/pg_locale.h
 *
 * Copyright (c) 2002-2024, PostgreSQL Global Development Group
 *
 *-----------------------------------------------------------------------
 */

#ifndef _PG_LOCALE_
#define _PG_LOCALE_

#if defined(LOCALE_T_IN_XLOCALE) || defined(WCSTOMBS_L_IN_XLOCALE)
#include <xlocale.h>
#endif
#ifdef USE_ICU
#include <unicode/ucol.h>
#endif

#ifdef USE_ICU
/*
 * ucol_strcollUTF8() was introduced in ICU 50, but it is buggy before ICU 53.
 * (see
 * <https://www.postgresql.org/message-id/flat/f1438ec6-22aa-4029-9a3b-26f79d330e72%40manitou-mail.org>)
 */
#if U_ICU_VERSION_MAJOR_NUM >= 53
#define HAVE_UCOL_STRCOLLUTF8 1
#else
#undef HAVE_UCOL_STRCOLLUTF8
#endif
#endif

/* use for libc locale names */
#define LOCALE_NAME_BUFLEN 128

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

/* is the databases's LC_CTYPE the C locale? */
extern PGDLLIMPORT bool database_ctype_is_c;

extern bool check_locale(int category, const char *locale, char **canonname);
extern char *pg_perm_setlocale(int category, const char *locale);

extern bool lc_collate_is_c(Oid collation);
extern bool lc_ctype_is_c(Oid collation);

/*
 * Return the POSIX lconv struct (contains number/money formatting
 * information) with locale information for all categories.
 */
extern struct lconv *PGLC_localeconv(void);

extern void cache_locale_time(void);


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
 * Note that some code relies on the flags not reporting false negatives
 * (that is, saying it's not C when it is).  For example, char2wchar()
 * could fail if the locale is C, so str_tolower() shouldn't call it
 * in that case.
 */
struct pg_locale_struct
{
	char		provider;
	bool		deterministic;
	bool		collate_is_c;
	bool		ctype_is_c;
	union
	{
		struct
		{
			const char *locale;
		}			builtin;
		locale_t	lt;
#ifdef USE_ICU
		struct
		{
			const char *locale;
			UCollator  *ucol;
		}			icu;
#endif
	}			info;
};

typedef struct pg_locale_struct *pg_locale_t;

extern void make_icu_collator(const char *iculocstr,
							  const char *icurules,
							  struct pg_locale_struct *resultp);

extern bool pg_locale_deterministic(pg_locale_t locale);
extern void init_database_collation(void);
extern pg_locale_t pg_newlocale_from_collation(Oid collid);

extern char *get_collation_actual_version(char collprovider, const char *collcollate);
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

extern int	builtin_locale_encoding(const char *locale);
extern const char *builtin_validate_locale(int encoding, const char *locale);
extern void icu_validate_locale(const char *loc_str);
extern char *icu_language_tag(const char *loc_str, int elevel);

#ifdef USE_ICU
extern int32_t icu_to_uchar(UChar **buff_uchar, const char *buff, size_t nbytes);
extern int32_t icu_from_uchar(char **result, const UChar *buff_uchar, int32_t len_uchar);
#endif

/* These functions convert from/to libc's wchar_t, *not* pg_wchar_t */
extern size_t wchar2char(char *to, const wchar_t *from, size_t tolen,
						 pg_locale_t locale);
extern size_t char2wchar(wchar_t *to, size_t tolen,
						 const char *from, size_t fromlen, pg_locale_t locale);

#endif							/* _PG_LOCALE_ */
