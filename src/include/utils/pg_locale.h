/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * $PostgreSQL: pgsql/src/include/utils/pg_locale.h,v 1.16 2003/11/29 22:41:16 pgsql Exp $
 *
 * Copyright (c) 2002-2003, PostgreSQL Global Development Group
 *
 *-----------------------------------------------------------------------
 */

#ifndef _PG_LOCALE_
#define _PG_LOCALE_

#include <locale.h>

extern char *locale_messages;
extern char *locale_monetary;
extern char *locale_numeric;
extern char *locale_time;

extern const char *locale_messages_assign(const char *value,
					   bool doit, bool interactive);
extern const char *locale_monetary_assign(const char *value,
					   bool doit, bool interactive);
extern const char *locale_numeric_assign(const char *value,
					  bool doit, bool interactive);
extern const char *locale_time_assign(const char *value,
				   bool doit, bool interactive);

extern bool lc_collate_is_c(void);

/*
 * Return the POSIX lconv struct (contains number/money formatting
 * information) with locale information for all categories.
 */
extern struct lconv *PGLC_localeconv(void);

#endif   /* _PG_LOCALE_ */
