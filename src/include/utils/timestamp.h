/*-------------------------------------------------------------------------
 *
 * timestamp.h
 *	  Definitions for the SQL92 "timestamp" and "interval" types.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: timestamp.h,v 1.3 2000/04/07 13:40:12 thomas Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TIMESTAMP_H
#define TIMESTAMP_H

#include <time.h>
#include <math.h>
#include <limits.h>

/*
 * Timestamp represents absolute time.
 * Interval represents delta time. Keep track of months (and years)
 *	separately since the elapsed time spanned is unknown until instantiated
 *	relative to an absolute time.
 *
 * Note that Postgres uses "time interval" to mean a bounded interval,
 *	consisting of a beginning and ending time, not a time span - thomas 97/03/20
 */

typedef double Timestamp;

typedef struct
{
	double		time;	/* all time units other than months and years */
	int4		month;	/* months and years, after time for alignment */
} Interval;


#ifdef NAN
#define DT_INVALID		(NAN)
#else
#define DT_INVALID		(DBL_MIN+DBL_MIN)
#endif
#ifdef HUGE_VAL
#define DT_NOBEGIN		(-HUGE_VAL)
#define DT_NOEND		(HUGE_VAL)
#else
#define DT_NOBEGIN		(-DBL_MAX)
#define DT_NOEND		(DBL_MAX)
#endif
#define DT_CURRENT		(DBL_MIN)
#define DT_EPOCH		(-DBL_MIN)

#define TIMESTAMP_INVALID(j)		{j = DT_INVALID;}
#ifdef NAN
#define TIMESTAMP_IS_INVALID(j)	(isnan(j))
#else
#define TIMESTAMP_IS_INVALID(j)	(j == DT_INVALID)
#endif

#define TIMESTAMP_NOBEGIN(j)		{j = DT_NOBEGIN;}
#define TIMESTAMP_IS_NOBEGIN(j)	(j == DT_NOBEGIN)

#define TIMESTAMP_NOEND(j)		{j = DT_NOEND;}
#define TIMESTAMP_IS_NOEND(j)	(j == DT_NOEND)

#define TIMESTAMP_CURRENT(j)		{j = DT_CURRENT;}
#if defined(linux) && defined(__powerpc__)
extern int	timestamp_is_current(double j);

#define TIMESTAMP_IS_CURRENT(j)	timestamp_is_current(j)
#else
#define TIMESTAMP_IS_CURRENT(j)	(j == DT_CURRENT)
#endif

#define TIMESTAMP_EPOCH(j)		{j = DT_EPOCH;}
#if defined(linux) && defined(__powerpc__)
extern int	timestamp_is_epoch(double j);

#define TIMESTAMP_IS_EPOCH(j)	timestamp_is_epoch(j)
#else
#define TIMESTAMP_IS_EPOCH(j)	(j == DT_EPOCH)
#endif

#define TIMESTAMP_IS_RELATIVE(j) (TIMESTAMP_IS_CURRENT(j) || TIMESTAMP_IS_EPOCH(j))
#define TIMESTAMP_NOT_FINITE(j)	(TIMESTAMP_IS_INVALID(j) \
								|| TIMESTAMP_IS_NOBEGIN(j) || TIMESTAMP_IS_NOEND(j))
#define TIMESTAMP_IS_RESERVED(j) (TIMESTAMP_IS_RELATIVE(j) || TIMESTAMP_NOT_FINITE(j))

#define INTERVAL_INVALID(j)		{(j).time = DT_INVALID;}
#ifdef NAN
#define INTERVAL_IS_INVALID(j)	(isnan((j).time))
#else
#define INTERVAL_IS_INVALID(j)	((j).time == DT_INVALID)
#endif
#define INTERVAL_NOT_FINITE(j)	INTERVAL_IS_INVALID(j)

#define TIME_PREC_INV 1000000.0
#define JROUND(j) (rint(((double) (j))*TIME_PREC_INV)/TIME_PREC_INV)


/*
 * timestamp.c prototypes
 */

extern Timestamp *timestamp_in(char *str);
extern char *timestamp_out(Timestamp *dt);
extern bool timestamp_eq(Timestamp *dt1, Timestamp *dt2);
extern bool timestamp_ne(Timestamp *dt1, Timestamp *dt2);
extern bool timestamp_lt(Timestamp *dt1, Timestamp *dt2);
extern bool timestamp_le(Timestamp *dt1, Timestamp *dt2);
extern bool timestamp_ge(Timestamp *dt1, Timestamp *dt2);
extern bool timestamp_gt(Timestamp *dt1, Timestamp *dt2);
extern bool timestamp_finite(Timestamp *timestamp);
extern int	timestamp_cmp(Timestamp *dt1, Timestamp *dt2);
extern Timestamp *timestamp_smaller(Timestamp *dt1, Timestamp *dt2);
extern Timestamp *timestamp_larger(Timestamp *dt1, Timestamp *dt2);

extern Interval *interval_in(char *str);
extern char *interval_out(Interval *span);
extern bool interval_eq(Interval *span1, Interval *span2);
extern bool interval_ne(Interval *span1, Interval *span2);
extern bool interval_lt(Interval *span1, Interval *span2);
extern bool interval_le(Interval *span1, Interval *span2);
extern bool interval_ge(Interval *span1, Interval *span2);
extern bool interval_gt(Interval *span1, Interval *span2);
extern bool interval_finite(Interval *span);
extern int	interval_cmp(Interval *span1, Interval *span2);
extern Interval *interval_smaller(Interval *span1, Interval *span2);
extern Interval *interval_larger(Interval *span1, Interval *span2);

extern text *timestamp_text(Timestamp *timestamp);
extern Timestamp *text_timestamp(text *str);
extern text *interval_text(Interval *interval);
extern Interval *text_interval(text *str);
extern Timestamp *timestamp_trunc(text *units, Timestamp *timestamp);
extern Interval *interval_trunc(text *units, Interval *interval);
extern float64 timestamp_part(text *units, Timestamp *timestamp);
extern float64 interval_part(text *units, Interval *interval);
extern text *timestamp_zone(text *zone, Timestamp *timestamp);

extern Interval *interval_um(Interval *span);
extern Interval *interval_pl(Interval *span1, Interval *span2);
extern Interval *interval_mi(Interval *span1, Interval *span2);
extern Interval *interval_mul(Interval *span1, float8 *factor);
extern Interval *mul_d_interval(float8 *factor, Interval *span1);
extern Interval *interval_div(Interval *span1, float8 *factor);

extern Interval *timestamp_mi(Timestamp *dt1, Timestamp *dt2);
extern Timestamp *timestamp_pl_span(Timestamp *dt, Interval *span);
extern Timestamp *timestamp_mi_span(Timestamp *dt, Interval *span);
extern Interval *timestamp_age(Timestamp *dt1, Timestamp *dt2);
extern bool overlaps_timestamp(Timestamp *dt1, Timestamp *dt2, Timestamp *dt3, Timestamp *dt4);

extern int tm2timestamp(struct tm * tm, double fsec, int *tzp, Timestamp *dt);
extern int timestamp2tm(Timestamp dt, int *tzp, struct tm * tm, double *fsec, char **tzn);

extern Timestamp SetTimestamp(Timestamp timestamp);
extern Timestamp dt2local(Timestamp dt, int timezone);
extern void dt2time(Timestamp dt, int *hour, int *min, double *sec);
extern int EncodeSpecialTimestamp(Timestamp dt, char *str);
extern int interval2tm(Interval span, struct tm * tm, float8 *fsec);
extern int tm2interval(struct tm * tm, double fsec, Interval *span);
extern Timestamp *now(void);

#endif	 /* TIMESTAMP_H */
