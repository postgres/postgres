/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/pg_locale.c,v 1.17 2002/05/17 01:19:18 tgl Exp $
 *
 * Portions Copyright (c) 2002, PostgreSQL Global Development Group
 *
 *-----------------------------------------------------------------------
 */

#include "postgres.h"

#include <locale.h>

#include "utils/pg_locale.h"


/* GUC storage area */

char *locale_messages;
char *locale_monetary;
char *locale_numeric;
char *locale_time;


/* GUC assign hooks */

static const char *
locale_xxx_assign(int category, const char *value, bool doit, bool interactive)
{
	if (doit)
	{
		if (!setlocale(category, value))
			return NULL;
	}
	else
	{
		char *save;

		save = setlocale(category, NULL);
		if (!save)
			return NULL;

		if (!setlocale(category, value))
			return NULL;

		setlocale(category, save);
	}
	return value;
}

const char *
locale_messages_assign(const char *value, bool doit, bool interactive)
{
	/* LC_MESSAGES category does not exist everywhere, but accept it anyway */
#ifdef LC_MESSAGES
	return locale_xxx_assign(LC_MESSAGES, value, doit, interactive);
#else
	return value;
#endif
}

const char *
locale_monetary_assign(const char *value, bool doit, bool interactive)
{
	return locale_xxx_assign(LC_MONETARY, value, doit, interactive);
}

const char *
locale_numeric_assign(const char *value, bool doit, bool interactive)
{
	return locale_xxx_assign(LC_NUMERIC, value, doit, interactive);
}

const char *
locale_time_assign(const char *value, bool doit, bool interactive)
{
	return locale_xxx_assign(LC_TIME, value, doit, interactive);
}


/*
 * We'd like to cache whether LC_COLLATE is C (or POSIX), so we can
 * optimize a few code paths in various places.
 */
bool
lc_collate_is_c(void)
{
	/* Cache result so we only have to compute it once */
	static int	result = -1;
	char	   *localeptr;

	if (result >= 0)
		return (bool) result;
	localeptr = setlocale(LC_COLLATE, NULL);
	if (!localeptr)
		elog(PANIC, "Invalid LC_COLLATE setting");

	if (strcmp(localeptr, "C") == 0)
		result = true;
	else if (strcmp(localeptr, "POSIX") == 0)
		result = true;
	else
		result = false;
	return (bool) result;
}


/*
 * Return the POSIX lconv struct (contains number/money formatting
 * information) with locale information for all categories.
 */
struct lconv *
PGLC_localeconv(void)
{
	static bool CurrentLocaleConvValid = false;
	static struct lconv CurrentLocaleConv;

	struct lconv *extlconv;

	/* Did we do it already? */
	if (CurrentLocaleConvValid)
		return &CurrentLocaleConv;

	/* Get formatting information for the external environment */
	extlconv = localeconv();

	/*
	 * Must copy all values since restoring internal settings may
	 * overwrite
	 */
	CurrentLocaleConv = *extlconv;
	CurrentLocaleConv.currency_symbol = strdup(extlconv->currency_symbol);
	CurrentLocaleConv.decimal_point = strdup(extlconv->decimal_point);
	CurrentLocaleConv.grouping = strdup(extlconv->grouping);
	CurrentLocaleConv.thousands_sep = strdup(extlconv->thousands_sep);
	CurrentLocaleConv.int_curr_symbol = strdup(extlconv->int_curr_symbol);
	CurrentLocaleConv.mon_decimal_point = strdup(extlconv->mon_decimal_point);
	CurrentLocaleConv.mon_grouping = strdup(extlconv->mon_grouping);
	CurrentLocaleConv.mon_thousands_sep = strdup(extlconv->mon_thousands_sep);
	CurrentLocaleConv.negative_sign = strdup(extlconv->negative_sign);
	CurrentLocaleConv.positive_sign = strdup(extlconv->positive_sign);

	CurrentLocaleConvValid = true;
	return &CurrentLocaleConv;
}
