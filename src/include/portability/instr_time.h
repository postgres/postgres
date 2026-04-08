/*-------------------------------------------------------------------------
 *
 * instr_time.h
 *	  portable high-precision interval timing
 *
 * This file provides an abstraction layer to hide portability issues in
 * interval timing. On x86 we use the RDTSC/RDTSCP instruction directly in
 * certain cases, or alternatively clock_gettime() on Unix-like systems and
 * QueryPerformanceCounter() on Windows. These macros also give some breathing
 * room to use other high-precision-timing APIs.
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
 * INSTR_TIME_SET_CURRENT_FAST(t)	set t to current time without waiting
 * 									for instructions in out-of-order window
 *
 * INSTR_TIME_SET_CURRENT(t)		set t to current time while waiting for
 * 									instructions in OOO to retire
 *
 *
 * INSTR_TIME_ADD(x, y)				x += y
 *
 * INSTR_TIME_ADD_NANOSEC(t, n)		t += n in nanoseconds (converts to ticks)
 *
 * INSTR_TIME_SUBTRACT(x, y)		x -= y
 *
 * INSTR_TIME_ACCUM_DIFF(x, y, z)	x += (y - z)
 *
 * INSTR_TIME_GT(x, y)				x > y
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
 * Copyright (c) 2001-2026, PostgreSQL Global Development Group
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

/* Shift amount for fixed-point ticks-to-nanoseconds conversion. */
#define TICKS_TO_NS_SHIFT 14

/*
 * PG_INSTR_TICKS_TO_NS controls whether pg_ticks_to_ns/pg_ns_to_ticks needs to
 * check ticks_per_ns_scaled and potentially convert ticks <=> nanoseconds.
 *
 * PG_INSTR_TSC_CLOCK controls whether the TSC clock source is compiled in, and
 * potentially used based on timing_tsc_enabled.
 */
#if defined(__x86_64__) || defined(_M_X64)
#define PG_INSTR_TICKS_TO_NS 1
#define PG_INSTR_TSC_CLOCK 1
#elif defined(WIN32)
#define PG_INSTR_TICKS_TO_NS 1
#define PG_INSTR_TSC_CLOCK 0
#else
#define PG_INSTR_TICKS_TO_NS 0
#define PG_INSTR_TSC_CLOCK 0
#endif

/*
 * Variables used to translate ticks to nanoseconds, initialized by
 * pg_initialize_timing and adjusted by pg_set_timing_clock_source calls or
 * changes of the "timing_clock_source" GUC.
 *
 * Note that changing these values after setting an instr_time and before
 * reading/converting it will lead to incorrect results. This is technically
 * possible because the GUC can be changed at runtime, but unlikely, and we
 * allow changing this at runtime to simplify testing of different sources.
 */
extern PGDLLIMPORT uint64 ticks_per_ns_scaled;
extern PGDLLIMPORT uint64 max_ticks_no_overflow;
extern PGDLLIMPORT bool timing_initialized;

typedef enum
{
	TIMING_CLOCK_SOURCE_AUTO,
	TIMING_CLOCK_SOURCE_SYSTEM,
#if PG_INSTR_TSC_CLOCK
	TIMING_CLOCK_SOURCE_TSC
#endif
} TimingClockSourceType;

extern PGDLLIMPORT int timing_clock_source;

/*
 * Initialize timing infrastructure
 *
 * This must be called at least once before using INSTR_TIME_SET_CURRENT*
 * macros.
 *
 * If you want to use the TSC clock source in a client program,
 * pg_set_timing_clock_source() needs to also be called.
 */
extern void pg_initialize_timing(void);

/*
 * Sets the time source to be used. Mainly intended for frontend programs,
 * the backend should set it via the timing_clock_source GUC instead.
 *
 * Returns false if the clock source could not be set, for example when TSC
 * is not available despite being explicitly set.
 */
extern bool pg_set_timing_clock_source(TimingClockSourceType source);

/* Whether to actually use TSC based on availability and GUC settings. */
extern PGDLLIMPORT bool timing_tsc_enabled;

/*
 * TSC frequency in kHz, set during initialization.
 *
 * -1 = not yet initialized, 0 = TSC not usable, >0 = frequency in kHz.
 */
extern PGDLLIMPORT int32 timing_tsc_frequency_khz;

#if PG_INSTR_TSC_CLOCK

extern void pg_initialize_timing_tsc(void);

extern uint32 pg_tsc_calibrate_frequency(void);

#endif							/* PG_INSTR_TSC_CLOCK */

/*
 * Returns the current timing clock source effectively in use, resolving
 * TIMING_CLOCK_SOURCE_AUTO to either TIMING_CLOCK_SOURCE_SYSTEM or
 * TIMING_CLOCK_SOURCE_TSC.
 */
static inline TimingClockSourceType
pg_current_timing_clock_source(void)
{
#if PG_INSTR_TSC_CLOCK
	if (timing_tsc_enabled)
		return TIMING_CLOCK_SOURCE_TSC;
#endif
	return TIMING_CLOCK_SOURCE_SYSTEM;
}

#ifndef WIN32

/* On POSIX, use clock_gettime() for system clock source */

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
 *
 * Note this does not get used in case the TSC clock source logic is used,
 * which directly calls architecture specific timing instructions (e.g. RDTSC).
 */
#if defined(__darwin__) && defined(CLOCK_MONOTONIC_RAW)
#define PG_INSTR_SYSTEM_CLOCK	CLOCK_MONOTONIC_RAW
#define PG_INSTR_SYSTEM_CLOCK_NAME	"clock_gettime (CLOCK_MONOTONIC_RAW)"
#elif defined(CLOCK_MONOTONIC)
#define PG_INSTR_SYSTEM_CLOCK	CLOCK_MONOTONIC
#define PG_INSTR_SYSTEM_CLOCK_NAME	"clock_gettime (CLOCK_MONOTONIC)"
#else
#define PG_INSTR_SYSTEM_CLOCK	CLOCK_REALTIME
#define PG_INSTR_SYSTEM_CLOCK_NAME	"clock_gettime (CLOCK_REALTIME)"
#endif

static inline instr_time
pg_get_ticks_system(void)
{
	instr_time	now;
	struct timespec tmp;

	Assert(timing_initialized);

	clock_gettime(PG_INSTR_SYSTEM_CLOCK, &tmp);
	now.ticks = tmp.tv_sec * NS_PER_S + tmp.tv_nsec;

	return now;
}

#else							/* WIN32 */

/* On Windows, use QueryPerformanceCounter() for system clock source */

#define PG_INSTR_SYSTEM_CLOCK_NAME	"QueryPerformanceCounter"
static inline instr_time
pg_get_ticks_system(void)
{
	instr_time	now;
	LARGE_INTEGER tmp;

	Assert(timing_initialized);

	QueryPerformanceCounter(&tmp);
	now.ticks = tmp.QuadPart;

	return now;
}

#endif							/* WIN32 */

static inline int64
pg_ticks_to_ns(int64 ticks)
{
#if PG_INSTR_TICKS_TO_NS
	int64		ns = 0;

	Assert(timing_initialized);

	/*
	 * Avoid doing work if we don't use scaled ticks, e.g. system clock on
	 * Unix (in that case ticks is counted in nanoseconds)
	 */
	if (ticks_per_ns_scaled == 0)
		return ticks;

	/*
	 * Would multiplication overflow? If so perform computation in two parts.
	 */
	if (unlikely(ticks > (int64) max_ticks_no_overflow))
	{
		/*
		 * To avoid overflow, first scale total ticks down by the fixed
		 * factor, and *afterwards* multiply them by the frequency-based scale
		 * factor.
		 *
		 * The remaining ticks can follow the regular formula, since they
		 * won't overflow.
		 */
		int64		count = ticks >> TICKS_TO_NS_SHIFT;

		ns = count * ticks_per_ns_scaled;
		ticks -= (count << TICKS_TO_NS_SHIFT);
	}

	ns += (ticks * ticks_per_ns_scaled) >> TICKS_TO_NS_SHIFT;

	return ns;
#else
	Assert(timing_initialized);

	return ticks;
#endif							/* PG_INSTR_TICKS_TO_NS */
}

static inline int64
pg_ns_to_ticks(int64 ns)
{
#if PG_INSTR_TICKS_TO_NS
	int64		ticks = 0;

	Assert(timing_initialized);

	/*
	 * If ticks_per_ns_scaled is zero, ticks are already in nanoseconds (e.g.
	 * system clock on Unix).
	 */
	if (ticks_per_ns_scaled == 0)
		return ns;

	/*
	 * The reverse of pg_ticks_to_ns to avoid a similar overflow problem.
	 */
	if (unlikely(ns > (INT64_MAX >> TICKS_TO_NS_SHIFT)))
	{
		int64		count = ns / ticks_per_ns_scaled;

		ticks = count << TICKS_TO_NS_SHIFT;
		ns -= count * ticks_per_ns_scaled;
	}

	ticks += (ns << TICKS_TO_NS_SHIFT) / ticks_per_ns_scaled;

	return ticks;
#else
	Assert(timing_initialized);

	return ns;
#endif							/* PG_INSTR_TICKS_TO_NS */
}

#if PG_INSTR_TSC_CLOCK

#define PG_INSTR_TSC_CLOCK_NAME_FAST  "RDTSC"
#define PG_INSTR_TSC_CLOCK_NAME "RDTSCP"

#ifdef _MSC_VER
#include <intrin.h>
#endif							/* defined(_MSC_VER) */

/* Helpers to abstract compiler differences for reading the x86 TSC. */
static inline int64
pg_rdtsc(void)
{
#ifdef _MSC_VER
	return __rdtsc();
#else
	return __builtin_ia32_rdtsc();
#endif							/* defined(_MSC_VER) */
}

static inline int64
pg_rdtscp(void)
{
	uint32		unused;

#ifdef _MSC_VER
	return __rdtscp(&unused);
#else
	return __builtin_ia32_rdtscp(&unused);
#endif							/* defined(_MSC_VER) */
}

/*
 * Marked always_inline due to a shortcoming in gcc's heuristics leading to
 * only inlining the function partially.
 * See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=124795
 */
static pg_attribute_always_inline instr_time
pg_get_ticks(void)
{
	if (likely(timing_tsc_enabled))
	{
		instr_time	now;

		now.ticks = pg_rdtscp();
		return now;
	}

	return pg_get_ticks_system();
}

static pg_attribute_always_inline instr_time
pg_get_ticks_fast(void)
{
	if (likely(timing_tsc_enabled))
	{
		instr_time	now;

		now.ticks = pg_rdtsc();
		return now;
	}

	return pg_get_ticks_system();
}

#else

static pg_attribute_always_inline instr_time
pg_get_ticks(void)
{
	return pg_get_ticks_system();
}

static pg_attribute_always_inline instr_time
pg_get_ticks_fast(void)
{
	return pg_get_ticks_system();
}

#endif							/* PG_INSTR_TSC_CLOCK */

/*
 * Common macros
 */

#define INSTR_TIME_IS_ZERO(t)	((t).ticks == 0)

#define INSTR_TIME_SET_ZERO(t)	((t).ticks = 0)

#define INSTR_TIME_SET_CURRENT_FAST(t) \
	((t) = pg_get_ticks_fast())

#define INSTR_TIME_SET_CURRENT(t) \
	((t) = pg_get_ticks())


#define INSTR_TIME_ADD(x,y) \
	((x).ticks += (y).ticks)

#define INSTR_TIME_ADD_NANOSEC(t, n) \
	((t).ticks += pg_ns_to_ticks(n))

#define INSTR_TIME_SUBTRACT(x,y) \
	((x).ticks -= (y).ticks)

#define INSTR_TIME_ACCUM_DIFF(x,y,z) \
	((x).ticks += (y).ticks - (z).ticks)

#define INSTR_TIME_GT(x,y) \
	((x).ticks > (y).ticks)

#define INSTR_TIME_GET_NANOSEC(t) \
	(pg_ticks_to_ns((t).ticks))

#define INSTR_TIME_GET_DOUBLE(t) \
	((double) INSTR_TIME_GET_NANOSEC(t) / NS_PER_S)

#define INSTR_TIME_GET_MILLISEC(t) \
	((double) INSTR_TIME_GET_NANOSEC(t) / NS_PER_MS)

#define INSTR_TIME_GET_MICROSEC(t) \
	(INSTR_TIME_GET_NANOSEC(t) / NS_PER_US)

#endif							/* INSTR_TIME_H */
