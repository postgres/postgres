/*-------------------------------------------------------------------------
 *
 * instr_time.c
 *	   Non-inline parts of the portable high-precision interval timing
 *	 implementation
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/common/instr_time.c
 *
 *-------------------------------------------------------------------------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "portability/instr_time.h"

/*
 * Stores what the number of ticks needs to be multiplied with to end up
 * with nanoseconds using integer math.
 *
 * On certain platforms (currently Windows) the ticks to nanoseconds conversion
 * requires floating point math because:
 *
 * sec = ticks / frequency_hz
 * ns  = ticks / frequency_hz * 1,000,000,000
 * ns  = ticks * (1,000,000,000 / frequency_hz)
 * ns  = ticks * (1,000,000 / frequency_khz) <-- now in kilohertz
 *
 * Here, 'ns' is usually a floating point number. For example for a 2.5 GHz CPU
 * the scaling factor becomes 1,000,000 / 2,500,000 = 0.4.
 *
 * To be able to use integer math we work around the lack of precision. We
 * first scale the integer up (left shift by TICKS_TO_NS_SHIFT) and after the
 * multiplication by the number of ticks in pg_ticks_to_ns() we shift right by
 * the same amount.
 *
 * We remember the maximum number of ticks that can be multiplied by the scale
 * factor without overflowing so we can check via a * b > max <=> a > max / b.
 *
 * However, as this is meant for interval measurements, it is unlikely that the
 * overflow path is actually taken in typical scenarios, since overflows would
 * only occur for intervals longer than 6.5 days.
 *
 * Note we utilize unsigned integers even though ticks are stored as a signed
 * value to encourage compilers to generate better assembly, since we can be
 * sure these values are not negative.
 *
 * On all other platforms we are using clock_gettime(), which uses nanoseconds
 * as ticks. Hence, we set the multiplier to zero, which causes pg_ticks_to_ns
 * to return the original value.
 */
uint64		ticks_per_ns_scaled = 0;
uint64		max_ticks_no_overflow = 0;
bool		timing_initialized = false;

static void set_ticks_per_ns_system(void);

/*
 * Initializes timing infrastructure. Must be called before making any use
 * of INSTR* macros.
 */
void
pg_initialize_timing(void)
{
	if (timing_initialized)
		return;

	set_ticks_per_ns_system();
	timing_initialized = true;
}

#ifndef WIN32

static void
set_ticks_per_ns_system(void)
{
	ticks_per_ns_scaled = 0;
	max_ticks_no_overflow = 0;
}

#else							/* WIN32 */

/* GetTimerFrequency returns counts per second */
static inline double
GetTimerFrequency(void)
{
	LARGE_INTEGER f;

	QueryPerformanceFrequency(&f);
	return (double) f.QuadPart;
}

static void
set_ticks_per_ns_system(void)
{
	ticks_per_ns_scaled = (NS_PER_S << TICKS_TO_NS_SHIFT) / GetTimerFrequency();
	max_ticks_no_overflow = PG_INT64_MAX / ticks_per_ns_scaled;
}

#endif							/* WIN32 */
