/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * $PostgreSQL: pgsql/src/include/utils/pg_locale.h,v 1.25 2008/05/19 18:08:16 tgl Exp $
 *
 * Copyright (c) 2002-2008, PostgreSQL Global Development Group
 *
 *-----------------------------------------------------------------------
 */

#ifndef _PG_LOCALE_
#define _PG_LOCALE_

#include <locale.h>

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

extern char *pg_perm_setlocale(int category, const char *locale);

extern bool lc_collate_is_c(void);
extern bool lc_ctype_is_c(void);

/*
 * Return the POSIX lconv struct (contains number/money formatting
 * information) with locale information for all categories.
 */
extern struct lconv *PGLC_localeconv(void);

extern void cache_locale_time(void);

#endif   /* _PG_LOCALE_ */
