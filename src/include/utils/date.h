/*-------------------------------------------------------------------------
 *
 * date.h
 *	  Definitions for the SQL92 "date" and "time" types.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: date.h,v 1.3 2000/04/12 17:16:54 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DATE_H
#define DATE_H

typedef int32 DateADT;

typedef float8 TimeADT;

typedef struct
{
	double		time;			/* all time units other than months and
								 * years */
	int4		zone;			/* numeric time zone, in seconds */
} TimeTzADT;

/* date.c */
extern DateADT date_in(char *datestr);
extern char *date_out(DateADT dateVal);
extern bool date_eq(DateADT dateVal1, DateADT dateVal2);
extern bool date_ne(DateADT dateVal1, DateADT dateVal2);
extern bool date_lt(DateADT dateVal1, DateADT dateVal2);
extern bool date_le(DateADT dateVal1, DateADT dateVal2);
extern bool date_gt(DateADT dateVal1, DateADT dateVal2);
extern bool date_ge(DateADT dateVal1, DateADT dateVal2);
extern int	date_cmp(DateADT dateVal1, DateADT dateVal2);
extern DateADT date_larger(DateADT dateVal1, DateADT dateVal2);
extern DateADT date_smaller(DateADT dateVal1, DateADT dateVal2);
extern int32 date_mi(DateADT dateVal1, DateADT dateVal2);
extern DateADT date_pli(DateADT dateVal, int32 days);
extern DateADT date_mii(DateADT dateVal, int32 days);
extern Timestamp *date_timestamp(DateADT date);
extern DateADT timestamp_date(Timestamp *timestamp);
extern Timestamp *datetime_timestamp(DateADT date, TimeADT *time);
extern DateADT abstime_date(AbsoluteTime abstime);

extern TimeADT *time_in(char *timestr);
extern char *time_out(TimeADT *time);
extern bool time_eq(TimeADT *time1, TimeADT *time2);
extern bool time_ne(TimeADT *time1, TimeADT *time2);
extern bool time_lt(TimeADT *time1, TimeADT *time2);
extern bool time_le(TimeADT *time1, TimeADT *time2);
extern bool time_gt(TimeADT *time1, TimeADT *time2);
extern bool time_ge(TimeADT *time1, TimeADT *time2);
extern int	time_cmp(TimeADT *time1, TimeADT *time2);
extern bool overlaps_time(TimeADT *time1, TimeADT *time2,
			  TimeADT *time3, TimeADT *time4);
extern TimeADT *time_larger(TimeADT *time1, TimeADT *time2);
extern TimeADT *time_smaller(TimeADT *time1, TimeADT *time2);
extern TimeADT *timestamp_time(Timestamp *timestamp);
extern Interval *time_interval(TimeADT *time);

extern TimeTzADT *timetz_in(char *timestr);
extern char *timetz_out(TimeTzADT *time);
extern bool timetz_eq(TimeTzADT *time1, TimeTzADT *time2);
extern bool timetz_ne(TimeTzADT *time1, TimeTzADT *time2);
extern bool timetz_lt(TimeTzADT *time1, TimeTzADT *time2);
extern bool timetz_le(TimeTzADT *time1, TimeTzADT *time2);
extern bool timetz_gt(TimeTzADT *time1, TimeTzADT *time2);
extern bool timetz_ge(TimeTzADT *time1, TimeTzADT *time2);
extern int	timetz_cmp(TimeTzADT *time1, TimeTzADT *time2);
extern bool overlaps_timetz(TimeTzADT *time1, TimeTzADT *time2,
				TimeTzADT *time3, TimeTzADT *time4);
extern TimeTzADT *timetz_larger(TimeTzADT *time1, TimeTzADT *time2);
extern TimeTzADT *timetz_smaller(TimeTzADT *time1, TimeTzADT *time2);
extern TimeTzADT *timestamp_timetz(Timestamp *timestamp);
extern Timestamp *datetimetz_timestamp(DateADT date, TimeTzADT *time);

#endif	 /* DATE_H */
