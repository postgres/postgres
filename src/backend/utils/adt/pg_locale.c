/* -----------------------------------------------------------------------
 * pg_locale.c
 *
 *	 The PostgreSQL locale utils.
 *
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/pg_locale.c,v 1.8 2001/01/24 19:43:14 momjian Exp $
 *
 *	 Portions Copyright (c) 1999-2000, PostgreSQL Global Development Group
 *
 *	Karel Zak - Zakkr
 *
 * -----------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef USE_LOCALE

#include <locale.h>
#include "utils/pg_locale.h"

/* #define DEBUG_LOCALE_UTILS  */


static struct lconv *CurrentLocaleConv = NULL;

static void PGLC_setlocale(PG_LocaleCategories * lc);

/*------
 * Return in PG_LocaleCategories the current locale settings
 *------
 */
void
PGLC_current(PG_LocaleCategories * lc)
{
	lc->lang = getenv("LANG");

	lc->lc_ctype = setlocale(LC_CTYPE, NULL);
	lc->lc_numeric = setlocale(LC_NUMERIC, NULL);
	lc->lc_time = setlocale(LC_TIME, NULL);
	lc->lc_collate = setlocale(LC_COLLATE, NULL);
	lc->lc_monetary = setlocale(LC_MONETARY, NULL);
#ifdef LC_MESSAGES
	lc->lc_messages = setlocale(LC_MESSAGES, NULL);
#endif
}


#ifdef DEBUG_LOCALE_UTILS

/*------
 * Print a PG_LocaleCategories struct as DEBUG
 *------
 */
static void
PGLC_debug_lc(PG_LocaleCategories * lc)
{
#ifdef LC_MESSAGES
	elog(DEBUG, "CURRENT LOCALE ENVIRONMENT:\n\nLANG:   \t%s\nLC_CTYPE:\t%s\nLC_NUMERIC:\t%s\nLC_TIME:\t%s\nLC_COLLATE:\t%s\nLC_MONETARY:\t%s\nLC_MESSAGES:\t%s\n",
#else
	elog(DEBUG, "CURRENT LOCALE ENVIRONMENT:\n\nLANG:   \t%s\nLC_CTYPE:\t%s\nLC_NUMERIC:\t%s\nLC_TIME:\t%s\nLC_COLLATE:\t%s\nLC_MONETARY:\t%s\n",
#endif
		 lc->lang,
		 lc->lc_ctype,
		 lc->lc_numeric,
		 lc->lc_time,
		 lc->lc_collate,
		 lc->lc_monetary
#ifdef LC_MESSAGES
		 , lc->lc_messages
#endif
	);
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
PGLC_setlocale(PG_LocaleCategories * lc)
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
		elog(NOTICE, "pg_setlocale(): 'LC_MESSAGE=%s' cannot be honored.",
			 lc->lc_messages);
#endif
}

/*------
 * Return the POSIX lconv struct (contains number/money formatting information)
 * with locale information for all categories.  Note that returned lconv
 * does not depend on currently active category settings, but on external
 * environment variables for locale.
 *
 * XXX we assume that restoring old category settings via setlocale() will
 * not immediately corrupt the static data returned by localeconv().
 * How portable is this?
 *
 * XXX in any case, there certainly must not be any other calls to
 * localeconv() anywhere in the backend, else the values reported here
 * will be overwritten with the Postgres-internal locale settings.
 *------
 */
struct lconv *
PGLC_localeconv(void)
{
	PG_LocaleCategories lc;

	/* Did we do it already? */
	if (CurrentLocaleConv)
		return CurrentLocaleConv;

	/* Save current locale setting to lc */
	PGLC_current(&lc);

	/* Set all locale categories based on postmaster's environment vars */
	setlocale(LC_ALL, "");

	/* Get formatting information for the external environment */
	CurrentLocaleConv = localeconv();

	/* Restore Postgres' internal locale settings */
	PGLC_setlocale(&lc);

	return CurrentLocaleConv;
}

#endif	 /* USE_LOCALE */
