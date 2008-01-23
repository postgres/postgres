/*-------------------------------------------------------------------------
 *
 * timestamp.h
 *	  Definitions for the SQL92 "timestamp" and "interval" types.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/timestamp.h,v 1.64.2.1 2008/01/23 21:26:20 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TIMESTAMP_H
#define TIMESTAMP_H

#include <math.h>
#include <limits.h>
#include <float.h>

#include "fmgr.h"
#include "pgtime.h"
#ifdef HAVE_INT64_TIMESTAMP
#include "utils/int8.h"
#endif

/*
 * Timestamp represents absolute time.
 * Interval represents delta time. Keep track of months (and years), days,
 *	and time separately since the elapsed time spanned is unknown until
 *	instantiated relative to an absolute time.
 *
 * Note that Postgres uses "time interval" to mean a bounded interval,
 * consisting of a beginning and ending time, not a time span - thomas 97/03/20
 */

#ifdef HAVE_INT64_TIMESTAMP
typedef int64 Timestamp;
typedef int64 TimestampTz;
#else
typedef double Timestamp;
typedef double TimestampTz;
#endif

typedef struct
{
#ifdef HAVE_INT64_TIMESTAMP
	int64		time;			/* all time units other than days, months and
								 * years */
#else
	double		time;			/* all time units other than days, months and
								 * years */
#endif
	int32		day;			/* days, after time for alignment */
	int32		month;			/* months and years, after time for alignment */
} Interval;


#define MAX_TIMESTAMP_PRECISION 6
#define MAX_INTERVAL_PRECISION 6

/* in both timestamp.h and ecpg/dt.h */
#define DAYS_PER_YEAR	365.25	/* assumes leap year every four years */
#define MONTHS_PER_YEAR 12
/*
 *	DAYS_PER_MONTH is very imprecise.  The more accurate value is
 *	365.2425/12 = 30.436875, or '30 days 10:29:06'.  Right now we only
 *	return an integral number of days, but someday perhaps we should
 *	also return a 'time' value to be used as well.	ISO 8601 suggests
 *	30 days.
 */
#define DAYS_PER_MONTH	30		/* assumes exactly 30 days per month */
#define HOURS_PER_DAY	24		/* assume no daylight savings time changes */

/*
 *	This doesn't adjust for uneven daylight savings time intervals or leap
 *	seconds, and it crudely estimates leap years.  A more accurate value
 *	for days per years is 365.2422.
 */
#define SECS_PER_YEAR	(36525 * 864)	/* avoid floating-point computation */
#define SECS_PER_DAY	86400
#define SECS_PER_HOUR	3600
#define SECS_PER_MINUTE 60
#define MINS_PER_HOUR	60

#ifdef HAVE_INT64_TIMESTAMP
#define USECS_PER_DAY	INT64CONST(86400000000)
#define USECS_PER_HOUR	INT64CONST(3600000000)
#define USECS_PER_MINUTE INT64CONST(60000000)
#define USECS_PER_SEC	INT64CONST(1000000)
#endif

/*
 * Macros for fmgr-callable functions.
 *
 * For Timestamp, we make use of the same support routines as for int64
 * or float8.  Therefore Timestamp is pass-by-reference if and only if
 * int64 or float8 is!
 */
#ifdef HAVE_INT64_TIMESTAMP

#define DatumGetTimestamp(X)  ((Timestamp) DatumGetInt64(X))
#define DatumGetTimestampTz(X)	((TimestampTz) DatumGetInt64(X))
#define DatumGetIntervalP(X)  ((Interval *) DatumGetPointer(X))

#define TimestampGetDatum(X) Int64GetDatum(X)
#define TimestampTzGetDatum(X) Int64GetDatum(X)
#define IntervalPGetDatum(X) PointerGetDatum(X)

#define PG_GETARG_TIMESTAMP(n) PG_GETARG_INT64(n)
#define PG_GETARG_TIMESTAMPTZ(n) PG_GETARG_INT64(n)
#define PG_GETARG_INTERVAL_P(n) DatumGetIntervalP(PG_GETARG_DATUM(n))

#define PG_RETURN_TIMESTAMP(x) PG_RETURN_INT64(x)
#define PG_RETURN_TIMESTAMPTZ(x) PG_RETURN_INT64(x)
#define PG_RETURN_INTERVAL_P(x) return IntervalPGetDatum(x)

#define DT_NOBEGIN		(-INT64CONST(0x7fffffffffffffff) - 1)
#define DT_NOEND		(INT64CONST(0x7fffffffffffffff))
#else

#define DatumGetTimestamp(X)  ((Timestamp) DatumGetFloat8(X))
#define DatumGetTimestampTz(X)	((TimestampTz) DatumGetFloat8(X))
#define DatumGetIntervalP(X)  ((Interval *) DatumGetPointer(X))

#define TimestampGetDatum(X) Float8GetDatum(X)
#define TimestampTzGetDatum(X) Float8GetDatum(X)
#define IntervalPGetDatum(X) PointerGetDatum(X)

#define PG_GETARG_TIMESTAMP(n) DatumGetTimestamp(PG_GETARG_DATUM(n))
#define PG_GETARG_TIMESTAMPTZ(n) DatumGetTimestampTz(PG_GETARG_DATUM(n))
#define PG_GETARG_INTERVAL_P(n) DatumGetIntervalP(PG_GETARG_DATUM(n))

#define PG_RETURN_TIMESTAMP(x) return TimestampGetDatum(x)
#define PG_RETURN_TIMESTAMPTZ(x) return TimestampTzGetDatum(x)
#define PG_RETURN_INTERVAL_P(x) return IntervalPGetDatum(x)

#ifdef HUGE_VAL
#define DT_NOBEGIN		(-HUGE_VAL)
#define DT_NOEND		(HUGE_VAL)
#else
#define DT_NOBEGIN		(-DBL_MAX)
#define DT_NOEND		(DBL_MAX)
#endif
#endif   /* HAVE_INT64_TIMESTAMP */


#define TIMESTAMP_NOBEGIN(j)	\
	do {(j) = DT_NOBEGIN;} while (0)
#define TIMESTAMP_IS_NOBEGIN(j) ((j) == DT_NOBEGIN)

#define TIMESTAMP_NOEND(j)		\
	do {(j) = DT_NOEND;} while (0)
#define TIMESTAMP_IS_NOEND(j)	((j) == DT_NOEND)

#define TIMESTAMP_NOT_FINITE(j) (TIMESTAMP_IS_NOBEGIN(j) || TIMESTAMP_IS_NOEND(j))

#ifdef HAVE_INT64_TIMESTAMP

typedef int32 fsec_t;
#else

typedef double fsec_t;
#endif

/*
 *	Round off to MAX_TIMESTAMP_PRECISION decimal places.
 *	Note: this is also used for rounding off intervals.
 */
#define TS_PREC_INV 1000000.0
#define TSROUND(j) (rint(((double) (j)) * TS_PREC_INV) / TS_PREC_INV)

#define TIMESTAMP_MASK(b) (1 << (b))
#define INTERVAL_MASK(b) (1 << (b))

/* Macros to handle packing and unpacking the typmod field for intervals */
#define INTERVAL_FULL_RANGE (0x7FFF)
#define INTERVAL_RANGE_MASK (0x7FFF)
#define INTERVAL_FULL_PRECISION (0xFFFF)
#define INTERVAL_PRECISION_MASK (0xFFFF)
#define INTERVAL_TYPMOD(p,r) ((((r) & INTERVAL_RANGE_MASK) << 16) | ((p) & INTERVAL_PRECISION_MASK))
#define INTERVAL_PRECISION(t) ((t) & INTERVAL_PRECISION_MASK)
#define INTERVAL_RANGE(t) (((t) >> 16) & INTERVAL_RANGE_MASK)

#ifdef HAVE_INT64_TIMESTAMP
#define TimestampTzPlusMilliseconds(tz,ms) ((tz) + ((ms) * (int64) 1000))
#else
#define TimestampTzPlusMilliseconds(tz,ms) ((tz) + ((ms) / 1000.0))
#endif


/* Set at postmaster start */
extern TimestampTz PgStartTime;


/*
 * timestamp.c prototypes
 */

extern Datum timestamp_in(PG_FUNCTION_ARGS);
extern Datum timestamp_out(PG_FUNCTION_ARGS);
extern Datum timestamp_recv(PG_FUNCTION_ARGS);
extern Datum timestamp_send(PG_FUNCTION_ARGS);
extern Datum timestamp_scale(PG_FUNCTION_ARGS);
extern Datum timestamp_eq(PG_FUNCTION_ARGS);
extern Datum timestamp_ne(PG_FUNCTION_ARGS);
extern Datum timestamp_lt(PG_FUNCTION_ARGS);
extern Datum timestamp_le(PG_FUNCTION_ARGS);
extern Datum timestamp_ge(PG_FUNCTION_ARGS);
extern Datum timestamp_gt(PG_FUNCTION_ARGS);
extern Datum timestamp_finite(PG_FUNCTION_ARGS);
extern Datum timestamp_cmp(PG_FUNCTION_ARGS);
extern Datum timestamp_smaller(PG_FUNCTION_ARGS);
extern Datum timestamp_larger(PG_FUNCTION_ARGS);

extern Datum timestamp_eq_timestamptz(PG_FUNCTION_ARGS);
extern Datum timestamp_ne_timestamptz(PG_FUNCTION_ARGS);
extern Datum timestamp_lt_timestamptz(PG_FUNCTION_ARGS);
extern Datum timestamp_le_timestamptz(PG_FUNCTION_ARGS);
extern Datum timestamp_gt_timestamptz(PG_FUNCTION_ARGS);
extern Datum timestamp_ge_timestamptz(PG_FUNCTION_ARGS);
extern Datum timestamp_cmp_timestamptz(PG_FUNCTION_ARGS);

extern Datum timestamptz_eq_timestamp(PG_FUNCTION_ARGS);
extern Datum timestamptz_ne_timestamp(PG_FUNCTION_ARGS);
extern Datum timestamptz_lt_timestamp(PG_FUNCTION_ARGS);
extern Datum timestamptz_le_timestamp(PG_FUNCTION_ARGS);
extern Datum timestamptz_gt_timestamp(PG_FUNCTION_ARGS);
extern Datum timestamptz_ge_timestamp(PG_FUNCTION_ARGS);
extern Datum timestamptz_cmp_timestamp(PG_FUNCTION_ARGS);

extern Datum interval_in(PG_FUNCTION_ARGS);
extern Datum interval_out(PG_FUNCTION_ARGS);
extern Datum interval_recv(PG_FUNCTION_ARGS);
extern Datum interval_send(PG_FUNCTION_ARGS);
extern Datum interval_scale(PG_FUNCTION_ARGS);
extern Datum interval_eq(PG_FUNCTION_ARGS);
extern Datum interval_ne(PG_FUNCTION_ARGS);
extern Datum interval_lt(PG_FUNCTION_ARGS);
extern Datum interval_le(PG_FUNCTION_ARGS);
extern Datum interval_ge(PG_FUNCTION_ARGS);
extern Datum interval_gt(PG_FUNCTION_ARGS);
extern Datum interval_finite(PG_FUNCTION_ARGS);
extern Datum interval_cmp(PG_FUNCTION_ARGS);
extern Datum interval_hash(PG_FUNCTION_ARGS);
extern Datum interval_smaller(PG_FUNCTION_ARGS);
extern Datum interval_larger(PG_FUNCTION_ARGS);
extern Datum interval_justify_interval(PG_FUNCTION_ARGS);
extern Datum interval_justify_hours(PG_FUNCTION_ARGS);
extern Datum interval_justify_days(PG_FUNCTION_ARGS);

extern Datum timestamp_text(PG_FUNCTION_ARGS);
extern Datum text_timestamp(PG_FUNCTION_ARGS);
extern Datum interval_text(PG_FUNCTION_ARGS);
extern Datum text_interval(PG_FUNCTION_ARGS);
extern Datum timestamp_trunc(PG_FUNCTION_ARGS);
extern Datum interval_trunc(PG_FUNCTION_ARGS);
extern Datum timestamp_part(PG_FUNCTION_ARGS);
extern Datum interval_part(PG_FUNCTION_ARGS);
extern Datum timestamp_zone(PG_FUNCTION_ARGS);
extern Datum timestamp_izone(PG_FUNCTION_ARGS);
extern Datum timestamp_timestamptz(PG_FUNCTION_ARGS);

extern Datum timestamptz_in(PG_FUNCTION_ARGS);
extern Datum timestamptz_out(PG_FUNCTION_ARGS);
extern Datum timestamptz_recv(PG_FUNCTION_ARGS);
extern Datum timestamptz_send(PG_FUNCTION_ARGS);
extern Datum timestamptz_scale(PG_FUNCTION_ARGS);
extern Datum timestamptz_timestamp(PG_FUNCTION_ARGS);
extern Datum timestamptz_zone(PG_FUNCTION_ARGS);
extern Datum timestamptz_izone(PG_FUNCTION_ARGS);
extern Datum timestamptz_timestamptz(PG_FUNCTION_ARGS);

extern Datum interval_um(PG_FUNCTION_ARGS);
extern Datum interval_pl(PG_FUNCTION_ARGS);
extern Datum interval_mi(PG_FUNCTION_ARGS);
extern Datum interval_mul(PG_FUNCTION_ARGS);
extern Datum mul_d_interval(PG_FUNCTION_ARGS);
extern Datum interval_div(PG_FUNCTION_ARGS);
extern Datum interval_accum(PG_FUNCTION_ARGS);
extern Datum interval_avg(PG_FUNCTION_ARGS);

extern Datum timestamp_mi(PG_FUNCTION_ARGS);
extern Datum timestamp_pl_interval(PG_FUNCTION_ARGS);
extern Datum timestamp_mi_interval(PG_FUNCTION_ARGS);
extern Datum timestamp_age(PG_FUNCTION_ARGS);
extern Datum overlaps_timestamp(PG_FUNCTION_ARGS);

extern Datum timestamptz_text(PG_FUNCTION_ARGS);
extern Datum text_timestamptz(PG_FUNCTION_ARGS);
extern Datum timestamptz_pl_interval(PG_FUNCTION_ARGS);
extern Datum timestamptz_mi_interval(PG_FUNCTION_ARGS);
extern Datum timestamptz_age(PG_FUNCTION_ARGS);
extern Datum timestamptz_trunc(PG_FUNCTION_ARGS);
extern Datum timestamptz_part(PG_FUNCTION_ARGS);

extern Datum now(PG_FUNCTION_ARGS);
extern Datum statement_timestamp(PG_FUNCTION_ARGS);
extern Datum clock_timestamp(PG_FUNCTION_ARGS);

extern Datum pgsql_postmaster_start_time(PG_FUNCTION_ARGS);

/* Internal routines (not fmgr-callable) */

extern TimestampTz GetCurrentTimestamp(void);

extern void TimestampDifference(TimestampTz start_time, TimestampTz stop_time,
					long *secs, int *microsecs);

extern TimestampTz time_t_to_timestamptz(time_t tm);
extern time_t timestamptz_to_time_t(TimestampTz t);

extern int	tm2timestamp(struct pg_tm * tm, fsec_t fsec, int *tzp, Timestamp *dt);
extern int timestamp2tm(Timestamp dt, int *tzp, struct pg_tm * tm,
			 fsec_t *fsec, char **tzn, pg_tz *attimezone);
extern void dt2time(Timestamp dt, int *hour, int *min, int *sec, fsec_t *fsec);

extern int	interval2tm(Interval span, struct pg_tm * tm, fsec_t *fsec);
extern int	tm2interval(struct pg_tm * tm, fsec_t fsec, Interval *span);

extern Timestamp SetEpochTimestamp(void);
extern void GetEpochTime(struct pg_tm * tm);

extern int	timestamp_cmp_internal(Timestamp dt1, Timestamp dt2);

/* timestamp comparison works for timestamptz also */
#define timestamptz_cmp_internal(dt1,dt2)	timestamp_cmp_internal(dt1, dt2)

extern void isoweek2date(int woy, int *year, int *mon, int *mday);
extern int	date2isoweek(int year, int mon, int mday);
extern int	date2isoyear(int year, int mon, int mday);

#endif   /* TIMESTAMP_H */
