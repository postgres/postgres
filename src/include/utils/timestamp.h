/*-------------------------------------------------------------------------
 *
 * timestamp.h
 *	  Definitions for the SQL "timestamp" and "interval" types.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/timestamp.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TIMESTAMP_H
#define TIMESTAMP_H

#include "datatype/timestamp.h"
#include "fmgr.h"
#include "pgtime.h"


/*
 * Functions for fmgr-callable functions.
 *
 * For Timestamp, we make use of the same support routines as for int64.
 * Therefore Timestamp is pass-by-reference if and only if int64 is!
 */
static inline Timestamp
DatumGetTimestamp(Datum X)
{
	return (Timestamp) DatumGetInt64(X);
}

static inline TimestampTz
DatumGetTimestampTz(Datum X)
{
	return (TimestampTz) DatumGetInt64(X);
}

static inline Interval *
DatumGetIntervalP(Datum X)
{
	return (Interval *) DatumGetPointer(X);
}

static inline Datum
TimestampGetDatum(Timestamp X)
{
	return Int64GetDatum(X);
}

static inline Datum
TimestampTzGetDatum(TimestampTz X)
{
	return Int64GetDatum(X);
}

static inline Datum
IntervalPGetDatum(const Interval *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_TIMESTAMP(n) DatumGetTimestamp(PG_GETARG_DATUM(n))
#define PG_GETARG_TIMESTAMPTZ(n) DatumGetTimestampTz(PG_GETARG_DATUM(n))
#define PG_GETARG_INTERVAL_P(n) DatumGetIntervalP(PG_GETARG_DATUM(n))

#define PG_RETURN_TIMESTAMP(x) return TimestampGetDatum(x)
#define PG_RETURN_TIMESTAMPTZ(x) return TimestampTzGetDatum(x)
#define PG_RETURN_INTERVAL_P(x) return IntervalPGetDatum(x)


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

/* Macros for doing timestamp arithmetic without assuming timestamp's units */
#define TimestampTzPlusMilliseconds(tz,ms) ((tz) + ((ms) * (int64) 1000))
#define TimestampTzPlusSeconds(tz,s) ((tz) + ((s) * (int64) 1000000))

/* Helper for simple subtraction between two timestamps */
static inline uint64
TimestampDifferenceMicroseconds(TimestampTz start_time,
								TimestampTz stop_time)
{
	if (start_time >= stop_time)
		return 0;
	return (uint64) stop_time - start_time;
}

/* Set at postmaster start */
extern PGDLLIMPORT TimestampTz PgStartTime;

/* Set at configuration reload */
extern PGDLLIMPORT TimestampTz PgReloadTime;


/* Internal routines (not fmgr-callable) */

extern int32 anytimestamp_typmod_check(bool istz, int32 typmod);

extern TimestampTz GetCurrentTimestamp(void);
extern TimestampTz GetSQLCurrentTimestamp(int32 typmod);
extern Timestamp GetSQLLocalTimestamp(int32 typmod);
extern void TimestampDifference(TimestampTz start_time, TimestampTz stop_time,
								long *secs, int *microsecs);
extern long TimestampDifferenceMilliseconds(TimestampTz start_time,
											TimestampTz stop_time);
extern bool TimestampDifferenceExceeds(TimestampTz start_time,
									   TimestampTz stop_time,
									   int msec);
extern bool TimestampDifferenceExceedsSeconds(TimestampTz start_time,
											  TimestampTz stop_time,
											  int threshold_sec);

extern TimestampTz time_t_to_timestamptz(pg_time_t tm);
extern pg_time_t timestamptz_to_time_t(TimestampTz t);

extern const char *timestamptz_to_str(TimestampTz t);

extern int	tm2timestamp(struct pg_tm *tm, fsec_t fsec, int *tzp, Timestamp *result);
extern int	timestamp2tm(Timestamp dt, int *tzp, struct pg_tm *tm,
						 fsec_t *fsec, const char **tzn, pg_tz *attimezone);
extern void dt2time(Timestamp jd, int *hour, int *min, int *sec, fsec_t *fsec);

extern void interval2itm(Interval span, struct pg_itm *itm);
extern int	itm2interval(struct pg_itm *itm, Interval *span);
extern int	itmin2interval(struct pg_itm_in *itm_in, Interval *span);

extern Timestamp SetEpochTimestamp(void);
extern void GetEpochTime(struct pg_tm *tm);

extern int	timestamp_cmp_internal(Timestamp dt1, Timestamp dt2);

/* timestamp comparison works for timestamptz also */
#define timestamptz_cmp_internal(dt1,dt2)	timestamp_cmp_internal(dt1, dt2)

extern TimestampTz timestamp2timestamptz_opt_overflow(Timestamp timestamp,
													  int *overflow);
extern int32 timestamp_cmp_timestamptz_internal(Timestamp timestampVal,
												TimestampTz dt2);

extern int	isoweek2j(int year, int week);
extern void isoweek2date(int woy, int *year, int *mon, int *mday);
extern void isoweekdate2date(int isoweek, int wday, int *year, int *mon, int *mday);
extern int	date2isoweek(int year, int mon, int mday);
extern int	date2isoyear(int year, int mon, int mday);
extern int	date2isoyearday(int year, int mon, int mday);

extern bool TimestampTimestampTzRequiresRewrite(void);

#endif							/* TIMESTAMP_H */
