/*-------------------------------------------------------------------------
 *
 * pgtime.h
 *	  PostgreSQL internal timezone library
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/include/pgtime.h,v 1.1 2004/05/21 05:08:03 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PGTIME_H
#define _PGTIME_H

#ifdef FRONTEND

/* Don't mess with anything for the frontends */
#include <time.h>

#else

/* 
 * Redefine functions and defines we implement, so we cause an
 * error if someone tries to use the "base functions" 
 */
#ifndef NO_REDEFINE_TIMEFUNCS
#define localtime DONOTUSETHIS_localtime
#define gmtime DONOTUSETHIS_gmtime
#define asctime DONOTUSETHIS_asctime
#define ctime DONOTUSETHIS_ctime
#define tzset DONOTUSETHIS_tzset
#define mktime DONOTUSETHIS_mktime
#define tzname DONOTUSETHIS_tzname
#define daylight DONOTUSETHIS_daylight
#define strftime DONOTUSETHIS_strftime
#endif

/* Then pull in default declarations, particularly time_t */
#include <time.h>

/*
 * Now define prototype for our own timezone implementation
 * structs and functions.
 */
struct pg_tm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
	long int tm_gmtoff;
	const char *tm_zone;
};

extern struct pg_tm *pg_localtime(const time_t *);
extern struct pg_tm *pg_gmtime(const time_t *);
extern time_t pg_mktime(struct pg_tm *);
extern bool pg_tzset(const char *tzname);
extern size_t pg_strftime(char *s, size_t max, const char *format,
						  const struct pg_tm *tm);
extern void pg_timezone_initialize(void);
extern bool tz_acceptable(void);
extern const char *select_default_timezone(void);
extern const char *pg_get_current_timezone(void);

#endif /* FRONTEND */

#endif /* _PGTIME_H */
