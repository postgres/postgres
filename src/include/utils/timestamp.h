/*-------------------------------------------------------------------------
 *
 * timestamp.h
 *	  Definitions for the SQL92 "timestamp" and "interval" types.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: timestamp.h,v 1.13 2001/01/23 03:10:25 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TIMESTAMP_H
#define TIMESTAMP_H

#include <time.h>
#include <math.h>
#include <limits.h>
#include <float.h>

#include "fmgr.h"


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
	int32		month;	/* months and years, after time for alignment */
} Interval;


/*
 * Macros for fmgr-callable functions.
 *
 * For Timestamp, we make use of the same support routines as for float8.
 * Therefore Timestamp is pass-by-reference if and only if float8 is!
 */
#define DatumGetTimestamp(X)  ((Timestamp) DatumGetFloat8(X))
#define DatumGetIntervalP(X)  ((Interval *) DatumGetPointer(X))

#define TimestampGetDatum(X)  Float8GetDatum(X)
#define IntervalPGetDatum(X)  PointerGetDatum(X)

#define PG_GETARG_TIMESTAMP(n)  DatumGetTimestamp(PG_GETARG_DATUM(n))
#define PG_GETARG_INTERVAL_P(n) DatumGetIntervalP(PG_GETARG_DATUM(n))

#define PG_RETURN_TIMESTAMP(x)  return TimestampGetDatum(x)
#define PG_RETURN_INTERVAL_P(x) return IntervalPGetDatum(x)


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

#define TIMESTAMP_INVALID(j)		do {j = DT_INVALID;} while (0)
#ifdef NAN
#define TIMESTAMP_IS_INVALID(j) (isnan(j))
#else
#define TIMESTAMP_IS_INVALID(j) (j == DT_INVALID)
#endif

#define TIMESTAMP_NOBEGIN(j)		do {j = DT_NOBEGIN;} while (0)
#define TIMESTAMP_IS_NOBEGIN(j) (j == DT_NOBEGIN)

#define TIMESTAMP_NOEND(j)		do {j = DT_NOEND;} while (0)
#define TIMESTAMP_IS_NOEND(j)	(j == DT_NOEND)

#define TIMESTAMP_CURRENT(j)		do {j = DT_CURRENT;} while (0)
#if defined(linux) && defined(__powerpc__)
extern int	timestamp_is_current(double j);

#define TIMESTAMP_IS_CURRENT(j) timestamp_is_current(j)
#else
#define TIMESTAMP_IS_CURRENT(j) (j == DT_CURRENT)
#endif

#define TIMESTAMP_EPOCH(j)		do {j = DT_EPOCH;} while (0)
#if defined(linux) && defined(__powerpc__)
extern int	timestamp_is_epoch(double j);

#define TIMESTAMP_IS_EPOCH(j)	timestamp_is_epoch(j)
#else
#define TIMESTAMP_IS_EPOCH(j)	(j == DT_EPOCH)
#endif

#define TIMESTAMP_IS_RELATIVE(j) (TIMESTAMP_IS_CURRENT(j) || TIMESTAMP_IS_EPOCH(j))
#define TIMESTAMP_NOT_FINITE(j) (TIMESTAMP_IS_INVALID(j) \
								|| TIMESTAMP_IS_NOBEGIN(j) || TIMESTAMP_IS_NOEND(j))
#define TIMESTAMP_IS_RESERVED(j) (TIMESTAMP_IS_RELATIVE(j) || TIMESTAMP_NOT_FINITE(j))

#define INTERVAL_INVALID(j)		do {(j).time = DT_INVALID;} while (0)
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

extern Datum timestamp_in(PG_FUNCTION_ARGS);
extern Datum timestamp_out(PG_FUNCTION_ARGS);
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

extern Datum interval_in(PG_FUNCTION_ARGS);
extern Datum interval_out(PG_FUNCTION_ARGS);
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

extern Datum interval_um(PG_FUNCTION_ARGS);
extern Datum interval_pl(PG_FUNCTION_ARGS);
extern Datum interval_mi(PG_FUNCTION_ARGS);
extern Datum interval_mul(PG_FUNCTION_ARGS);
extern Datum mul_d_interval(PG_FUNCTION_ARGS);
extern Datum interval_div(PG_FUNCTION_ARGS);
extern Datum interval_accum(PG_FUNCTION_ARGS);
extern Datum interval_avg(PG_FUNCTION_ARGS);

extern Datum timestamp_mi(PG_FUNCTION_ARGS);
extern Datum timestamp_pl_span(PG_FUNCTION_ARGS);
extern Datum timestamp_mi_span(PG_FUNCTION_ARGS);
extern Datum timestamp_age(PG_FUNCTION_ARGS);
extern Datum overlaps_timestamp(PG_FUNCTION_ARGS);

extern Datum now(PG_FUNCTION_ARGS);

/* Internal routines (not fmgr-callable) */

extern int	tm2timestamp(struct tm * tm, double fsec, int *tzp, Timestamp *dt);
extern int	timestamp2tm(Timestamp dt, int *tzp, struct tm * tm,
						 double *fsec, char **tzn);

extern Timestamp SetTimestamp(Timestamp timestamp);

extern void	isoweek2date( int woy, int *year, int *mon, int *mday);
extern int	date2isoweek(int year, int mon, int mday);

#endif	 /* TIMESTAMP_H */
