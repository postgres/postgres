/*-------------------------------------------------------------------------
 *
 * pgtime.h
 *	  PostgreSQL internal timezone library
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/include/pgtime.h,v 1.4 2004/08/29 05:06:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PGTIME_H
#define _PGTIME_H


/*
 * The API of this library is generally similar to the corresponding
 * C library functions, except that we use pg_time_t which (we hope) is
 * 64 bits wide, and which is most definitely signed not unsigned.
 */

typedef int64 pg_time_t;

struct pg_tm
{
	int			tm_sec;
	int			tm_min;
	int			tm_hour;
	int			tm_mday;
	int			tm_mon;			/* origin 0, not 1 */
	int			tm_year;		/* relative to 1900 */
	int			tm_wday;
	int			tm_yday;
	int			tm_isdst;
	long int	tm_gmtoff;
	const char *tm_zone;
};

extern struct pg_tm *pg_localtime(const pg_time_t *);
extern struct pg_tm *pg_gmtime(const pg_time_t *);
extern bool pg_tzset(const char *tzname);
extern size_t pg_strftime(char *s, size_t max, const char *format,
			const struct pg_tm * tm);
extern void pg_timezone_initialize(void);
extern bool tz_acceptable(void);
extern const char *select_default_timezone(void);
extern const char *pg_get_current_timezone(void);

#endif   /* _PGTIME_H */
