/*-------------------------------------------------------------------------
 *
 * date.h
 *	  Definitions for the SQL92 "date" and "time" types.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: date.h,v 1.1 2000/02/16 17:26:26 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DATE_H
#define DATE_H

typedef int32 DateADT;

typedef float8 TimeADT;

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
extern TimeADT *timestamp_time(Timestamp *timestamp);

#endif	 /* DATE_H */
