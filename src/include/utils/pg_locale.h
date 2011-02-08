/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * src/include/utils/pg_locale.h
 *
 * Copyright (c) 2002-2011, PostgreSQL Global Development Group
 *
 *-----------------------------------------------------------------------
 */

#ifndef _PG_LOCALE_
#define _PG_LOCALE_

#include <locale.h>
#ifdef LOCALE_T_IN_XLOCALE
#include <xlocale.h>
#endif

#include "utils/guc.h"


/* GUC settings */
extern char *locale_messages;
extern char *locale_monetary;
extern char *locale_numeric;
extern char *locale_time;

/* lc_time localization cache */
extern char *localized_abbrev_days[];
extern char *localized_full_days[];
extern char *localized_abbrev_months[];
extern char *localized_full_months[];


extern const char *locale_messages_assign(const char *value,
					   bool doit, GucSource source);
extern const char *locale_monetary_assign(const char *value,
					   bool doit, GucSource source);
extern const char *locale_numeric_assign(const char *value,
					  bool doit, GucSource source);
extern const char *locale_time_assign(const char *value,
				   bool doit, GucSource source);

extern bool check_locale(int category, const char *locale);
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
 * We define our own wrapper around locale_t so we can keep the same
 * function signatures for all builds, while not having to create a
 * fake version of the standard type locale_t in the global namespace.
 * The fake version of pg_locale_t can be checked for truth; that's
 * about all it will be needed for.
 */
#ifdef HAVE_LOCALE_T
typedef locale_t pg_locale_t;
#else
typedef int pg_locale_t;
#endif

extern pg_locale_t pg_newlocale_from_collation(Oid collid);

#endif   /* _PG_LOCALE_ */
