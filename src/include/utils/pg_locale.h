/* -----------------------------------------------------------------------
 * pg_locale.h
 *
 *	 The PostgreSQL locale utils.
 *
 *
 * $Id: pg_locale.h,v 1.6 2001/01/24 19:43:28 momjian Exp $
 *
 *	 Portions Copyright (c) 1999-2000, PostgreSQL Global Development Group
 *
 *	Karel Zak - Zakkr
 *
 * -----------------------------------------------------------------------
 */

#ifndef _PG_LOCALE_
#define _PG_LOCALE_

#ifdef USE_LOCALE

/*------
 * POSIX locale categories and environment variable LANG
 *------
 */
typedef struct PG_LocaleCategories
{
	char	   *lang,
			   *lc_ctype,
			   *lc_numeric,
			   *lc_time,
			   *lc_collate,
			   *lc_monetary,
			   *lc_messages;
}			PG_LocaleCategories;


extern void PGLC_current(PG_LocaleCategories * lc);

/*------
 * Return the POSIX lconv struct (contains number/money formatting information)
 * with locale information for all categories.  Note that returned lconv
 * does not depend on currently active category settings, but on external
 * environment variables for locale.
 *------
 */
extern struct lconv *PGLC_localeconv(void);


#endif	 /* USE_LOCALE */

#endif	 /* _PG_LOCALE_ */
