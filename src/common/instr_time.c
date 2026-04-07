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

#include <math.h>

#include "port/pg_cpu.h"
#include "portability/instr_time.h"

/*
 * Stores what the number of ticks needs to be multiplied with to end up
 * with nanoseconds using integer math.
 *
 * In certain cases (TSC on x86-64, and QueryPerformanceCounter on Windows)
 * the ticks to nanoseconds conversion requires floating point math because:
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
 * In all other cases we are using clock_gettime(), which uses nanoseconds
 * as ticks. Hence, we set the multiplier to zero, which causes pg_ticks_to_ns
 * to return the original value.
 */
uint64		ticks_per_ns_scaled = 0;
uint64		max_ticks_no_overflow = 0;
bool		timing_initialized = false;
int			timing_clock_source = TIMING_CLOCK_SOURCE_AUTO;

bool		timing_tsc_enabled = false;
int32		timing_tsc_frequency_khz = -1;

static void set_ticks_per_ns(void);
static void set_ticks_per_ns_system(void);

#if PG_INSTR_TSC_CLOCK
static bool tsc_use_by_default(void);
static void set_ticks_per_ns_for_tsc(void);
#endif

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

bool
pg_set_timing_clock_source(TimingClockSourceType source)
{
	Assert(timing_initialized);

#if PG_INSTR_TSC_CLOCK
	pg_initialize_timing_tsc();

	switch (source)
	{
		case TIMING_CLOCK_SOURCE_AUTO:
			timing_tsc_enabled = (timing_tsc_frequency_khz > 0) && tsc_use_by_default();
			break;
		case TIMING_CLOCK_SOURCE_SYSTEM:
			timing_tsc_enabled = false;
			break;
		case TIMING_CLOCK_SOURCE_TSC:
			/* Tell caller TSC is not usable */
			if (timing_tsc_frequency_khz <= 0)
				return false;
			timing_tsc_enabled = true;
			break;
	}
#endif

	set_ticks_per_ns();
	timing_clock_source = source;
	return true;
}

static void
set_ticks_per_ns(void)
{
#if PG_INSTR_TSC_CLOCK
	if (timing_tsc_enabled)
	{
		set_ticks_per_ns_for_tsc();
		return;
	}
#endif
	set_ticks_per_ns_system();
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

/* TSC specific logic */

#if PG_INSTR_TSC_CLOCK

static void tsc_detect_frequency(void);

/*
 * Initialize the TSC clock source by determining its usability and frequency.
 *
 * This can be called multiple times without causing repeated work, as
 * timing_tsc_frequency_khz will be set to 0 if a prior call determined the
 * TSC is not usable. On EXEC_BACKEND (Windows), the TSC frequency may also be
 * set by restore_backend_variables.
 */
void
pg_initialize_timing_tsc(void)
{
	if (timing_tsc_frequency_khz < 0)
		tsc_detect_frequency();
}

static void
set_ticks_per_ns_for_tsc(void)
{
	ticks_per_ns_scaled = ((NS_PER_S / 1000) << TICKS_TO_NS_SHIFT) / timing_tsc_frequency_khz;
	max_ticks_no_overflow = PG_INT64_MAX / ticks_per_ns_scaled;
}

/*
 * Detect the TSC frequency and whether RDTSCP is available on x86-64.
 *
 * This can't be reliably determined at compile time, since the
 * availability of an "invariant" TSC (that is not affected by CPU
 * frequency changes) is dependent on the CPU architecture. Additionally,
 * there are cases where TSC availability is impacted by virtualization,
 * where a simple cpuid feature check would not be enough.
 */
static void
tsc_detect_frequency(void)
{
	timing_tsc_frequency_khz = 0;

	/* We require RDTSCP support and an invariant TSC, bail if not available */
	if (!x86_feature_available(PG_RDTSCP) || !x86_feature_available(PG_TSC_INVARIANT))
		return;

	/* Determine speed at which the TSC advances */
	timing_tsc_frequency_khz = x86_tsc_frequency_khz();
	if (timing_tsc_frequency_khz > 0)
		return;

	/*
	 * CPUID did not give us the TSC frequency. We can instead measure the
	 * frequency by comparing ticks against walltime in a calibration loop.
	 */
	timing_tsc_frequency_khz = pg_tsc_calibrate_frequency();
}

/*
 * Decides whether to use the TSC clock source if the user did not specify it
 * one way or the other, and it is available (checked separately).
 *
 * Inspired by the Linux kernel's clocksource watchdog disable logic as updated
 * in 2021 to reflect the reliability of the TSC on Intel platforms, see
 * check_system_tsc_reliable() in arch/x86/kernel/tsc.c, as well as discussion
 * in https://lore.kernel.org/lkml/87eekfk8bd.fsf@nanos.tec.linutronix.de/
 * and https://lore.kernel.org/lkml/87a6pimt1f.ffs@nanos.tec.linutronix.de/
 * for reference.
 *
 * When tsc_detect_frequency determines the TSC is viable (invariant, etc.), and
 * we're on an Intel platform (determined via TSC_ADJUST), we consider the TSC
 * trustworthy by default, matching the Linux kernel.
 *
 * On other CPU platforms (e.g. AMD), or in some virtual machines, we don't have
 * an easy way to determine the TSC's reliability. If on Linux, we can check if
 * TSC is the active clocksource, based on it having run the watchdog logic to
 * monitor TSC correctness. For other platforms the user must explicitly enable
 * it via GUC instead.
 */
static bool
tsc_use_by_default(void)
{
	if (x86_feature_available(PG_TSC_ADJUST))
		return true;

#if defined(__linux__)
	{
		FILE	   *fp;
		char		buf[128];

		fp = fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "r");
		if (fp)
		{
			bool		is_tsc = (fgets(buf, sizeof(buf), fp) != NULL &&
								  strcmp(buf, "tsc\n") == 0);

			fclose(fp);
			if (is_tsc)
				return true;
		}
	}
#endif

	return false;
}

/*
 * Calibrate the TSC frequency by comparing TSC ticks against walltime.
 *
 * Takes initial TSC and system clock snapshots, then loops, recomputing the
 * frequency each TSC_CALIBRATION_SKIPS iterations from cumulative TSC
 * ticks divided by elapsed time.
 *
 * Once the frequency estimate stabilizes (consecutive iterations agree), we
 * consider it converged and the frequency in KHz is returned. If either too
 * many iterations or a time limit passes without convergence, 0 is returned.
 */
#define TSC_CALIBRATION_MAX_NS		(50 * NS_PER_MS)
#define TSC_CALIBRATION_ITERATIONS	1000000
#define TSC_CALIBRATION_SKIPS		100
#define TSC_CALIBRATION_STABLE_CYCLES	10
uint32
pg_tsc_calibrate_frequency(void)
{
	instr_time	initial_wall;
	int64		initial_tsc;
	double		freq_khz = 0;
	double		prev_freq_khz = 0;
	int			stable_count = 0;
	int64		prev_tsc;
	int			saved_clock_source = timing_clock_source;

	/*
	 * Frequency must be initialized to avoid recursion via
	 * pg_set_timing_clock_source.
	 */
	Assert(timing_tsc_frequency_khz >= 0);

	/* Ensure INSTR_* calls below work on system time */
	pg_set_timing_clock_source(TIMING_CLOCK_SOURCE_SYSTEM);

	INSTR_TIME_SET_CURRENT(initial_wall);

	initial_tsc = pg_rdtscp();
	prev_tsc = initial_tsc;

	for (int i = 0; i < TSC_CALIBRATION_ITERATIONS; i++)
	{
		instr_time	now_wall;
		int64		now_tsc;
		int64		elapsed_ns;
		int64		elapsed_ticks;

		INSTR_TIME_SET_CURRENT(now_wall);

		now_tsc = pg_rdtscp();

		INSTR_TIME_SUBTRACT(now_wall, initial_wall);
		elapsed_ns = INSTR_TIME_GET_NANOSEC(now_wall);

		/* Safety: bail out if we've taken too long */
		if (elapsed_ns >= TSC_CALIBRATION_MAX_NS)
			break;

		elapsed_ticks = now_tsc - initial_tsc;

		/*
		 * Skip if TSC hasn't advanced, or we walked backwards for some
		 * reason.
		 */
		if (now_tsc == prev_tsc || elapsed_ns <= 0 || elapsed_ticks <= 0)
			continue;

		/*
		 * We only measure frequency every TSC_CALIBRATION_SKIPS to avoid
		 * stabilizing based on just a handful of RDTSC instructions.
		 */
		if (i % TSC_CALIBRATION_SKIPS != 0)
			continue;

		freq_khz = ((double) elapsed_ticks / elapsed_ns) * 1000 * 1000;

		/*
		 * Once freq_khz / prev_freq_khz is small, check if it stays that way.
		 * If it does for long enough, we've got a winner frequency.
		 */
		if (prev_freq_khz != 0 && fabs(1 - freq_khz / prev_freq_khz) < 0.0001)
		{
			stable_count++;
			if (stable_count >= TSC_CALIBRATION_STABLE_CYCLES)
				break;
		}
		else
			stable_count = 0;

		prev_tsc = now_tsc;
		prev_freq_khz = freq_khz;
	}

	/* Restore the previous clock source */
	pg_set_timing_clock_source(saved_clock_source);

	if (stable_count < TSC_CALIBRATION_STABLE_CYCLES)
		return 0;				/* did not converge */

	return (uint32) freq_khz;
}

#endif							/* PG_INSTR_TSC_CLOCK */
