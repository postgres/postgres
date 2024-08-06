/*-------------------------------------------------------------------------
 *
 * instr_time.h
 *	  portable high-precision interval timing
 *
 * This file provides an abstraction layer to hide portability issues in
 * interval timing.  On Unix we use clock_gettime(), and on Windows we use
 * QueryPerformanceCounter().  These macros also give some breathing room to
 * use other high-precision-timing APIs.
 *
 * The basic data type is instr_time, which all callers should treat as an
 * opaque typedef.  instr_time can store either an absolute time (of
 * unspecified reference time) or an interval.  The operations provided
 * for it are:
 *
 * INSTR_TIME_IS_ZERO(t)			is t equal to zero?
 *
 * INSTR_TIME_SET_ZERO(t)			set t to zero (memset is acceptable too)
 *
 * INSTR_TIME_SET_CURRENT(t)		set t to current time
 *
 * INSTR_TIME_SET_CURRENT_LAZY(t)	set t to current time if t is zero,
 *									evaluates to whether t changed
 *
 * INSTR_TIME_ADD(x, y)				x += y
 *
 * INSTR_TIME_SUBTRACT(x, y)		x -= y
 *
 * INSTR_TIME_ACCUM_DIFF(x, y, z)	x += (y - z)
 *
 * INSTR_TIME_GET_DOUBLE(t)			convert t to double (in seconds)
 *
 * INSTR_TIME_GET_MILLISEC(t)		convert t to double (in milliseconds)
 *
 * INSTR_TIME_GET_MICROSEC(t)		convert t to int64 (in microseconds)
 *
 * INSTR_TIME_GET_NANOSEC(t)		convert t to int64 (in nanoseconds)
 *
 * Note that INSTR_TIME_SUBTRACT and INSTR_TIME_ACCUM_DIFF convert
 * absolute times to intervals.  The INSTR_TIME_GET_xxx operations are
 * only useful on intervals.
 *
 * When summing multiple measurements, it's recommended to leave the
 * running sum in instr_time form (ie, use INSTR_TIME_ADD or
 * INSTR_TIME_ACCUM_DIFF) and convert to a result format only at the end.
 *
 * Beware of multiple evaluations of the macro arguments.
 *
 *
 * Copyright (c) 2001-2024, PostgreSQL Global Development Group
 *
 * src/include/portability/instr_time.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INSTR_TIME_H
#define INSTR_TIME_H


/*
 * We store interval times as an int64 integer on all platforms, as int64 is
 * cheap to add/subtract, the most common operation for instr_time. The
 * acquisition of time and converting to specific units of time is platform
 * specific.
 *
 * To avoid users of the API relying on the integer representation, we wrap
 * the 64bit integer in a struct.
 */
typedef struct instr_time
{
	int64		ticks;			/* in platforms specific unit */
} instr_time;


/* helpers macros used in platform specific code below */

#define NS_PER_S	INT64CONST(1000000000)
#define NS_PER_MS	INT64CONST(1000000)
#define NS_PER_US	INT64CONST(1000)


#ifndef WIN32


/* Use clock_gettime() */

#include <time.h>

/*
 * The best clockid to use according to the POSIX spec is CLOCK_MONOTONIC,
 * since that will give reliable interval timing even in the face of changes
 * to the system clock.  However, POSIX doesn't require implementations to
 * provide anything except CLOCK_REALTIME, so fall back to that if we don't
 * find CLOCK_MONOTONIC.
 *
 * Also, some implementations have nonstandard clockids with better properties
 * than CLOCK_MONOTONIC.  In particular, as of macOS 10.12, Apple provides
 * CLOCK_MONOTONIC_RAW which is both faster to read and higher resolution than
 * their version of CLOCK_MONOTONIC.
 */
#if defined(__darwin__) && defined(CLOCK_MONOTONIC_RAW)
#define PG_INSTR_CLOCK	CLOCK_MONOTONIC_RAW
#elif defined(CLOCK_MONOTONIC)
#define PG_INSTR_CLOCK	CLOCK_MONOTONIC
#else
#define PG_INSTR_CLOCK	CLOCK_REALTIME
#endif

/* helper for INSTR_TIME_SET_CURRENT */
static inline instr_time
pg_clock_gettime_ns(void)
{
	instr_time	now;
	struct timespec tmp;

	clock_gettime(PG_INSTR_CLOCK, &tmp);
	now.ticks = tmp.tv_sec * NS_PER_S + tmp.tv_nsec;

	return now;
}

#define INSTR_TIME_SET_CURRENT(t) \
	((t) = pg_clock_gettime_ns())

#define INSTR_TIME_GET_NANOSEC(t) \
	((int64) (t).ticks)


#else							/* WIN32 */


/* Use QueryPerformanceCounter() */

/* helper for INSTR_TIME_SET_CURRENT */
static inline instr_time
pg_query_performance_counter(void)
{
	instr_time	now;
	LARGE_INTEGER tmp;

	QueryPerformanceCounter(&tmp);
	now.ticks = tmp.QuadPart;

	return now;
}

static inline double
GetTimerFrequency(void)
{
	LARGE_INTEGER f;

	QueryPerformanceFrequency(&f);
	return (double) f.QuadPart;
}

#define INSTR_TIME_SET_CURRENT(t) \
	((t) = pg_query_performance_counter())

#define INSTR_TIME_GET_NANOSEC(t) \
	((int64) ((t).ticks * ((double) NS_PER_S / GetTimerFrequency())))

#endif							/* WIN32 */


/*
 * Common macros
 */

#define INSTR_TIME_IS_ZERO(t)	((t).ticks == 0)


#define INSTR_TIME_SET_ZERO(t)	((t).ticks = 0)

#define INSTR_TIME_SET_CURRENT_LAZY(t) \
	(INSTR_TIME_IS_ZERO(t) ? INSTR_TIME_SET_CURRENT(t), true : false)


#define INSTR_TIME_ADD(x,y) \
	((x).ticks += (y).ticks)

#define INSTR_TIME_SUBTRACT(x,y) \
	((x).ticks -= (y).ticks)

#define INSTR_TIME_ACCUM_DIFF(x,y,z) \
	((x).ticks += (y).ticks - (z).ticks)


#define INSTR_TIME_GET_DOUBLE(t) \
	((double) INSTR_TIME_GET_NANOSEC(t) / NS_PER_S)

#define INSTR_TIME_GET_MILLISEC(t) \
	((double) INSTR_TIME_GET_NANOSEC(t) / NS_PER_MS)

#define INSTR_TIME_GET_MICROSEC(t) \
	(INSTR_TIME_GET_NANOSEC(t) / NS_PER_US)

#endif							/* INSTR_TIME_H */
