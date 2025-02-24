/*-------------------------------------------------------------------------
 *
 * instr_time.c
 *	   Non-inline parts of the portable high-precision interval timing
 *	 implementation
 *
 * Portions Copyright (c) 2022, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/port/instr_time.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "portability/instr_time.h"

#ifndef WIN32
/*
 * Stores what the number of cycles needs to be multiplied with to end up
 * with nanoseconds using integer math. See comment in pg_initialize_rdtsc()
 * for more details.
 *
 * By default assume we are using clock_gettime() as a fallback which uses
 * nanoseconds as ticks. Hence, we set the multiplier to the precision scalar
 * so that the division in INSTR_TIME_GET_NANOSEC() won't change the nanoseconds.
 *
 * When using the RDTSC instruction directly this is filled in during initialization
 * based on the relevant CPUID fields.
 */
int64		ticks_per_ns_scaled = TICKS_TO_NS_PRECISION;
int64		ticks_per_sec = NS_PER_S;
int64		max_ticks_no_overflow = PG_INT64_MAX / TICKS_TO_NS_PRECISION;

#if defined(__x86_64__) && defined(__linux__)
/*
 * Indicates if RDTSC can be used (Linux/x86 only, when OS uses TSC clocksource)
 */
bool		has_rdtsc = false;

/*
 * Indicates if RDTSCP can be used. True if RDTSC can be used and RDTSCP is available.
 */
bool		has_rdtscp = false;

#define CPUID_HYPERVISOR_VMWARE(words) (words[1] == 0x61774d56 && words[2] == 0x4d566572 && words[3] == 0x65726177) /* VMwareVMware */
#define CPUID_HYPERVISOR_KVM(words) (words[1] == 0x4b4d564b && words[2] == 0x564b4d56 && words[3] == 0x0000004d)	/* KVMKVMKVM */

static bool
get_tsc_frequency_khz(uint32 *tsc_freq)
{
	uint32		r[4];

	if (__get_cpuid(0x15, &r[0] /* denominator */ , &r[1] /* numerator */ , &r[2] /* hz */ , &r[3]) && r[2] > 0)
	{
		if (r[0] == 0 || r[1] == 0)
			return false;

		*tsc_freq = r[2] / 1000 * r[1] / r[0];
		return true;
	}

	/* Some CPUs only report frequency in 16H */
	if (__get_cpuid(0x16, &r[0] /* base_mhz */ , &r[1], &r[2], &r[3]))
	{
		*tsc_freq = r[0] * 1000;
		return true;
	}

	/*
	 * Check if we have a KVM or VMware Hypervisor passing down TSC frequency
	 * to us in a guest VM
	 *
	 * Note that accessing the 0x40000000 leaf for Hypervisor info requires
	 * use of __cpuidex to set ECX to 0.
	 *
	 * TODO: We need to check whether our compiler is new enough
	 * (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=95973)
	 */
	__cpuidex((int32 *) r, 0x40000000, 0);
	if (r[0] >= 0x40000010 && (CPUID_HYPERVISOR_VMWARE(r) || CPUID_HYPERVISOR_KVM(r)))
	{
		__cpuidex((int32 *) r, 0x40000010, 0);
		if (r[0] > 0)
		{
			*tsc_freq = r[0];
			return true;
		}
	}

	return false;
}

static bool
is_rdtscp_available()
{
	uint32		r[4];

	return __get_cpuid(0x80000001, &r[0], &r[1], &r[2], &r[3]) > 0 && (r[3] & (1 << 27)) != 0;
}

/*
 * Decide whether we use the RDTSC instruction at runtime, for Linux/x86,
 * instead of incurring the overhead of a full clock_gettime() call.
 *
 * This can't be reliably determined at compile time, since the
 * availability of an "invariant" TSC (that is not affected by CPU
 * frequency changes) is dependent on the CPU architecture. Additionally,
 * there are cases where TSC availability is impacted by virtualization,
 * where a simple cpuid feature check would not be enough.
 *
 * Since Linux already does a significant amount of work to determine
 * whether TSC is a viable clock source, decide based on that.
 */
void
pg_initialize_rdtsc(void)
{
	FILE	   *fp = fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "r");

	if (fp)
	{
		char		buf[128];

		if (fgets(buf, sizeof(buf), fp) != NULL && strcmp(buf, "tsc\n") == 0)
		{
			/*
			 * Compute baseline CPU peformance, determines speed at which
			 * RDTSC advances.
			 */
			uint32		tsc_freq;

			if (get_tsc_frequency_khz(&tsc_freq))
			{
				/*
				 * Ticks to nanoseconds conversion requires floating point
				 * math because because:
				 *
				 * sec = ticks / frequency_hz ns  = ticks / frequency_hz *
				 * 1,000,000,000 ns  = ticks * (1,000,000,000 / frequency_hz)
				 * ns  = ticks * (1,000,000 / frequency_khz) <-- now in
				 * kilohertz
				 *
				 * Here, 'ns' is usually a floating number. For example for a
				 * 2.5 GHz CPU the scaling factor becomes 1,000,000 /
				 * 2,500,000 = 1.2.
				 *
				 * To be able to use integer math we work around the lack of
				 * precision. We first scale the integer up and after the
				 * multiplication by the number of ticks in
				 * INSTR_TIME_GET_NANOSEC() we divide again by the same value.
				 * We picked the scaler such that it provides enough precision
				 * and is a power-of-two which allows for shifting instead of
				 * doing an integer division.
				 */
				ticks_per_ns_scaled = INT64CONST(1000000) * TICKS_TO_NS_PRECISION / tsc_freq;
				ticks_per_sec = tsc_freq * 1000;	/* KHz->Hz */
				max_ticks_no_overflow = PG_INT64_MAX / ticks_per_ns_scaled;

				has_rdtsc = true;
				has_rdtscp = is_rdtscp_available();
			}
		}

		fclose(fp);
	}
}
#endif							/* defined(__x86_64__) && defined(__linux__) */

#endif							/* WIN32 */
