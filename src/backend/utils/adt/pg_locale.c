/* -----------------------------------------------------------------------
 * pg_locale.c
 *
 *	 The PostgreSQL locale utils.
 *
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/pg_locale.c,v 1.13 2001/11/05 17:46:29 momjian Exp $
 *
 *	 Portions Copyright (c) 1999-2000, PostgreSQL Global Development Group
 *
 * Karel Zak
 *
 * -----------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef USE_LOCALE

#include <locale.h>

#include "utils/pg_locale.h"

/* #define DEBUG_LOCALE_UTILS  */


static bool CurrentLocaleConvValid = false;
static struct lconv CurrentLocaleConv;


static void PGLC_setlocale(PG_LocaleCategories *lc);

/*------
 * Frees memory used in PG_LocaleCategories -- this memory is
 * allocated in PGLC_current().
 *------
 */
void
PGLC_free_categories(PG_LocaleCategories *lc)
{
	if (lc->lc_ctype)
		pfree(lc->lc_ctype);
	if (lc->lc_numeric)
		pfree(lc->lc_numeric);
	if (lc->lc_time)
		pfree(lc->lc_time);
	if (lc->lc_collate)
		pfree(lc->lc_collate);
	if (lc->lc_monetary);
	pfree(lc->lc_monetary);
#ifdef LC_MESSAGES
	if (lc->lc_messages)
		pfree(lc->lc_messages);
#endif
}

/*------
 * Return in PG_LocaleCategories the current locale settings.
 *
 * NB: strings are allocated in the current memory context!
 *------
 */
void
PGLC_current(PG_LocaleCategories *lc)
{
	lc->lang = getenv("LANG");

	lc->lc_ctype = pstrdup(setlocale(LC_CTYPE, NULL));
	lc->lc_numeric = pstrdup(setlocale(LC_NUMERIC, NULL));
	lc->lc_time = pstrdup(setlocale(LC_TIME, NULL));
	lc->lc_collate = pstrdup(setlocale(LC_COLLATE, NULL));
	lc->lc_monetary = pstrdup(setlocale(LC_MONETARY, NULL));
#ifdef LC_MESSAGES
	lc->lc_messages = pstrdup(setlocale(LC_MESSAGES, NULL));
#endif
}


#ifdef DEBUG_LOCALE_UTILS

/*------
 * Print a PG_LocaleCategories struct as DEBUG
 *------
 */
static void
PGLC_debug_lc(PG_LocaleCategories *lc)
{
#ifdef LC_MESSAGES
	elog(DEBUG, "CURRENT LOCALE ENVIRONMENT:\n\nLANG:   \t%s\nLC_CTYPE:\t%s\nLC_NUMERIC:\t%s\nLC_TIME:\t%s\nLC_COLLATE:\t%s\nLC_MONETARY:\t%s\nLC_MESSAGES:\t%s\n",
		 lc->lang,
		 lc->lc_ctype,
		 lc->lc_numeric,
		 lc->lc_time,
		 lc->lc_collate,
		 lc->lc_monetary,
		 lc->lc_messages);
#else
	elog(DEBUG, "CURRENT LOCALE ENVIRONMENT:\n\nLANG:   \t%s\nLC_CTYPE:\t%s\nLC_NUMERIC:\t%s\nLC_TIME:\t%s\nLC_COLLATE:\t%s\nLC_MONETARY:\t%s\n",
		 lc->lang,
		 lc->lc_ctype,
		 lc->lc_numeric,
		 lc->lc_time,
		 lc->lc_collate,
		 lc->lc_monetary);
#endif
}
#endif

/*------
 * Set locales via a PG_LocaleCategories struct
 *
 * NB: it would be very dangerous to set the locale values to any random
 * choice of locale, since that could cause indexes to become corrupt, etc.
 * Therefore this routine is NOT exported from this module.  It should be
 * used only to restore previous locale settings during PGLC_localeconv.
 *------
 */
static void
PGLC_setlocale(PG_LocaleCategories *lc)
{
	if (!setlocale(LC_COLLATE, lc->lc_collate))
		elog(NOTICE, "pg_setlocale(): 'LC_COLLATE=%s' cannot be honored.",
			 lc->lc_collate);

	if (!setlocale(LC_CTYPE, lc->lc_ctype))
		elog(NOTICE, "pg_setlocale(): 'LC_CTYPE=%s' cannot be honored.",
			 lc->lc_ctype);

	if (!setlocale(LC_NUMERIC, lc->lc_numeric))
		elog(NOTICE, "pg_setlocale(): 'LC_NUMERIC=%s' cannot be honored.",
			 lc->lc_numeric);

	if (!setlocale(LC_TIME, lc->lc_time))
		elog(NOTICE, "pg_setlocale(): 'LC_TIME=%s' cannot be honored.",
			 lc->lc_time);

	if (!setlocale(LC_MONETARY, lc->lc_monetary))
		elog(NOTICE, "pg_setlocale(): 'LC_MONETARY=%s' cannot be honored.",
			 lc->lc_monetary);

#ifdef LC_MESSAGES
	if (!setlocale(LC_MESSAGES, lc->lc_messages))
		elog(NOTICE, "pg_setlocale(): 'LC_MESSAGES=%s' cannot be honored.",
			 lc->lc_messages);
#endif
}

/*------
 * Return the POSIX lconv struct (contains number/money formatting information)
 * with locale information for all categories.	Note that returned lconv
 * does not depend on currently active category settings, but on external
 * environment variables for locale.
 *------
 */
struct lconv *
PGLC_localeconv(void)
{
	PG_LocaleCategories lc;
	struct lconv *extlconv;

	/* Did we do it already? */
	if (CurrentLocaleConvValid)
		return &CurrentLocaleConv;

	/* Save current locale setting to lc */
	PGLC_current(&lc);

	/* Set all locale categories based on postmaster's environment vars */
	setlocale(LC_ALL, "");

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

	/* Restore Postgres' internal locale settings */
	PGLC_setlocale(&lc);

	/* Deallocate category settings allocated in PGLC_current() */
	PGLC_free_categories(&lc);

	CurrentLocaleConvValid = true;
	return &CurrentLocaleConv;
}

#endif   /* USE_LOCALE */
