
/* -----------------------------------------------------------------------
 * pg_locale.h
 *
 * $Header: /cvsroot/pgsql/src/include/utils/pg_locale.h,v 1.4 2000/04/12 17:16:55 momjian Exp $
 *
 *
 *	 Portions Copyright (c) 1999-2000, PostgreSQL, Inc
 *
 *	 The PostgreSQL locale utils.
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


extern PG_LocaleCategories *PGLC_current(PG_LocaleCategories * lc);
extern PG_LocaleCategories *PGLC_setlocale(PG_LocaleCategories * lc);

/*------
 * Return the POSIX lconv struct (contains number/money formatting information)
 * with locale information for *all* categories. Returned lconv is *independent*
 * on current locale catogories setting - in contrast to standard localeconv().
 *------
 */
extern struct lconv *PGLC_localeconv(void);


#endif	 /* USE_LOCALE */

#endif	 /* _PG_LOCALE_ */
