/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * $Header: /cvsroot/pgsql/src/include/utils/pg_locale.h,v 1.12 2002/04/03 05:39:33 petere Exp $
 *
 * Copyright (c) 2002, PostgreSQL Global Development Group
 *
 *-----------------------------------------------------------------------
 */

#ifndef _PG_LOCALE_
#define _PG_LOCALE_

#include "postgres.h"
#include <locale.h>

extern char * locale_messages;
extern char * locale_monetary;
extern char * locale_numeric;
extern char * locale_time;

bool locale_messages_check(const char *proposed);
bool locale_monetary_check(const char *proposed);
bool locale_numeric_check(const char *proposed);
bool locale_time_check(const char *proposed);

void locale_messages_assign(const char *value);
void locale_monetary_assign(const char *value);
void locale_numeric_assign(const char *value);
void locale_time_assign(const char *value);

bool chklocale(int category, const char *proposed);
bool lc_collate_is_c(void);

/*
 * Return the POSIX lconv struct (contains number/money formatting
 * information) with locale information for all categories.
 */
extern struct lconv *PGLC_localeconv(void);

#endif   /* _PG_LOCALE_ */
