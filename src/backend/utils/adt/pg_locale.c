/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/pg_locale.c,v 1.16 2002/04/03 05:39:31 petere Exp $
 *
 * Portions Copyright (c) 2002, PostgreSQL Global Development Group
 *
 *-----------------------------------------------------------------------
 */

#include "postgres.h"
#include "utils/pg_locale.h"
#include <locale.h>


/* GUC storage area */

char * locale_messages;
char * locale_monetary;
char * locale_numeric;
char * locale_time;

/* GUC parse hooks */

bool locale_messages_check(const char *proposed)
{
#ifdef LC_MESSAGES
	return chklocale(LC_MESSAGES, proposed);
#else
	/* We return true here so LC_MESSAGES can be set in the
       configuration file on every system. */
	return true;
#endif
}

bool locale_monetary_check(const char *proposed)
{
	return chklocale(LC_MONETARY, proposed);
}

bool locale_numeric_check(const char *proposed)
{
	return chklocale(LC_NUMERIC, proposed);
}

bool locale_time_check(const char *proposed)
{
	return chklocale(LC_TIME, proposed);
}

/* GUC assign hooks */

void locale_messages_assign(const char *value)
{
#ifdef LC_MESSAGES
	setlocale(LC_MESSAGES, value);
#endif
}

void locale_monetary_assign(const char *value)
{
	setlocale(LC_MONETARY, value);
}

void locale_numeric_assign(const char *value)
{
	setlocale(LC_NUMERIC, value);
}

void locale_time_assign(const char *value)
{
	setlocale(LC_TIME, value);
}


/*
 * Returns true if the proposed string represents a valid locale of
 * the given category.  This is probably pretty slow, but it's not
 * called in critical places.
 */
bool
chklocale(int category, const char *proposed)
{
	char *save;

	save = setlocale(category, NULL);
	if (!save)
		return false;

	if (!setlocale(category, proposed))
		return false;

	setlocale(category, save);
	return true;
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
	struct lconv *extlconv;
	static bool CurrentLocaleConvValid = false;
	static struct lconv CurrentLocaleConv;

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
