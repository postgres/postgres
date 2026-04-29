/*-------------------------------------------------------------------------
 *
 * pg_cpu_x86.c
 *	  Runtime CPU feature detection for x86
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_cpu_x86.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#if defined(USE_SSE2) || defined(__i386__)

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif

#ifdef HAVE_XSAVE_INTRINSICS
#include <immintrin.h>
#endif

#include "port/pg_cpu.h"

/*
 * XSAVE state component bits that we need
 *
 * https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-1-manual.pdf
 * Chapter "MANAGING STATE USING THE XSAVE FEATURE SET"
 */
#define XMM			(1<<1)
#define YMM			(1<<2)
#define OPMASK		(1<<5)
#define ZMM0_15		(1<<6)
#define ZMM16_31	(1<<7)


/* array indexed by enum X86FeatureId */
bool		X86Features[X86FeaturesSize] = {0};

static bool
mask_available(uint32 value, uint32 mask)
{
	return (value & mask) == mask;
}

/* Named indexes for CPUID register array */
#define EAX 0
#define EBX 1
#define ECX 2
#define EDX 3

/*
 * Request CPUID information for the specified leaf.
 */
static inline void
pg_cpuid(int leaf, unsigned int *reg)
{
#if defined(HAVE__GET_CPUID)
	__get_cpuid(leaf, &reg[EAX], &reg[EBX], &reg[ECX], &reg[EDX]);
#elif defined(HAVE__CPUID)
	__cpuid((int *) reg, leaf);
#else
#error cpuid instruction not available
#endif
}

/*
 * Request CPUID information for the specified leaf and subleaf.
 *
 * Returns true if the CPUID leaf/subleaf is supported, false otherwise.
 */
static inline bool
pg_cpuid_subleaf(int leaf, int subleaf, unsigned int *reg)
{
	memset(reg, 0, 4 * sizeof(unsigned int));
#if defined(HAVE__GET_CPUID_COUNT)
	return __get_cpuid_count(leaf, subleaf, &reg[EAX], &reg[EBX], &reg[ECX], &reg[EDX]) == 1;
#elif defined(HAVE__CPUIDEX)
	__cpuidex((int *) reg, leaf, subleaf);
	return true;
#else
	return false;
#endif
}

/*
 * Parse the CPU ID info for runtime checks.
 */
#ifdef HAVE_XSAVE_INTRINSICS
pg_attribute_target("xsave")
#endif
void
set_x86_features(void)
{
	unsigned int reg[4] = {0};
	bool		have_osxsave;

	pg_cpuid(0x01, reg);

	X86Features[PG_SSE4_2] = reg[ECX] >> 20 & 1;
	X86Features[PG_POPCNT] = reg[ECX] >> 23 & 1;
	X86Features[PG_HYPERVISOR] = reg[ECX] >> 31 & 1;
	have_osxsave = reg[ECX] >> 27 & 1;

	pg_cpuid_subleaf(0x07, 0, reg);

	X86Features[PG_TSC_ADJUST] = reg[EBX] >> 1 & 1;

	/* leaf 7 features that depend on OSXSAVE */
	if (have_osxsave)
	{
		uint32		xcr0_val = 0;

#ifdef HAVE_XSAVE_INTRINSICS
		/* get value of Extended Control Register */
		xcr0_val = _xgetbv(0);
#endif

		/* Are YMM registers enabled? */
		if (mask_available(xcr0_val, XMM | YMM))
			X86Features[PG_AVX2] = reg[EBX] >> 5 & 1;

		/* Are ZMM registers enabled? */
		if (mask_available(xcr0_val, XMM | YMM |
						   OPMASK | ZMM0_15 | ZMM16_31))
		{
			X86Features[PG_AVX512_BW] = reg[EBX] >> 30 & 1;
			X86Features[PG_AVX512_VL] = reg[EBX] >> 31 & 1;

			X86Features[PG_AVX512_VPCLMULQDQ] = reg[ECX] >> 10 & 1;
			X86Features[PG_AVX512_VPOPCNTDQ] = reg[ECX] >> 14 & 1;
		}
	}

	/* Check for other TSC related flags */
	pg_cpuid(0x80000001, reg);
	X86Features[PG_RDTSCP] = reg[EDX] >> 27 & 1;

	pg_cpuid(0x80000007, reg);
	X86Features[PG_TSC_INVARIANT] = reg[EDX] >> 8 & 1;

	X86Features[INIT_PG_X86] = true;
}

/* TSC (Time-stamp Counter) handling code */

static uint32 x86_hypervisor_tsc_frequency_khz(void);

/*
 * Determine the TSC frequency of the CPU through CPUID, where supported.
 *
 * Needed to interpret the tick value returned by RDTSC/RDTSCP. Return value of
 * 0 indicates the frequency information was not accessible via CPUID.
 */
uint32
x86_tsc_frequency_khz(void)
{
	unsigned int reg[4] = {0};

	/*
	 * If we're inside a virtual machine, try to fetch the TSC frequency from
	 * the hypervisor, using a hypervisor specific method.
	 *
	 * Note it is not safe to utilize the regular 0x15/0x16 CPUID registers
	 * (i.e. the logic below) in virtual machines, as they have been observed
	 * to be wildly incorrect when virtualized.
	 */
	if (x86_feature_available(PG_HYPERVISOR))
		return x86_hypervisor_tsc_frequency_khz();

	/*
	 * On modern Intel CPUs, the TSC is implemented by invariant timekeeping
	 * hardware, also called "Always Running Timer", or ART. The ART stays
	 * consistent even if the CPU changes frequency due to changing power
	 * levels.
	 *
	 * As documented in "Determining the Processor Base Frequency" in the
	 * "Intel® 64 and IA-32 Architectures Software Developer's Manual",
	 * February 2026 Edition, we can get the TSC frequency as follows:
	 *
	 * Nominal TSC frequency = ( CPUID.15H:ECX[31:0] * CPUID.15H:EBX[31:0] ) /
	 * CPUID.15H:EAX[31:0]
	 *
	 * With CPUID.15H:ECX representing the nominal core crystal clock
	 * frequency, and EAX/EBX representing values used to translate the TSC
	 * value to that frequency, see "Chapter 20.17 "Time-Stamp Counter" of
	 * that manual.
	 *
	 * Older Intel CPUs, and other vendors do not set CPUID.15H:ECX, and as
	 * such we fall back to alternate approaches.
	 */
	pg_cpuid(0x15, reg);
	if (reg[ECX] > 0)
	{
		/*
		 * EBX not being set indicates invariant TSC is not available. Require
		 * EAX being non-zero too, to avoid a theoretical divide by zero.
		 */
		if (reg[EAX] == 0 || reg[EBX] == 0)
			return 0;

		return reg[ECX] / 1000 * reg[EBX] / reg[EAX];
	}

	/*
	 * When CPUID.15H is not available/incomplete, we can instead try to get
	 * the processor base frequency in MHz from CPUID.16H:EAX, the "Processor
	 * Frequency Information Leaf".
	 */
	pg_cpuid(0x16, reg);
	if (reg[EAX] > 0)
		return reg[EAX] * 1000;

	return 0;
}

/*
 * Support for reading TSC frequency for hypervisors passing it to a guest VM.
 *
 * Two Hypervisors (VMware and KVM) are known to make TSC frequency in KHz
 * available at the vendor-specific 0x40000010 leaf in the EAX register.
 *
 * For some other Hypervisors that have an invariant TSC, e.g. HyperV, we would
 * need to access a model-specific register (MSR) to get the frequency. MSRs are
 * separate from CPUID and typically not available for unprivileged processes,
 * so we can't get the frequency this way.
 */
#define CPUID_HYPERVISOR_VMWARE(r) (r[EBX] == 0x61774d56 && r[ECX] == 0x4d566572 && r[EDX] == 0x65726177)	/* VMwareVMware */
#define CPUID_HYPERVISOR_KVM(r) (r[EBX] == 0x4b4d564b && r[ECX] == 0x564b4d56 && r[EDX] == 0x0000004d)	/* KVMKVMKVM */
static uint32
x86_hypervisor_tsc_frequency_khz(void)
{
#if defined(HAVE__CPUIDEX)
	unsigned int reg[4] = {0};

	/*
	 * The hypervisor is determined using the 0x40000000 Hypervisor
	 * information leaf, which requires use of __cpuidex to set ECX to 0 to
	 * access it.
	 *
	 * The similar __get_cpuid_count function does not work as expected since
	 * it contains a check for __get_cpuid_max, which has been observed to be
	 * lower than the special Hypervisor leaf, despite it being available.
	 */
	__cpuidex((int *) reg, 0x40000000, 0);

	if (reg[EAX] >= 0x40000010 && (CPUID_HYPERVISOR_VMWARE(reg) || CPUID_HYPERVISOR_KVM(reg)))
	{
		__cpuidex((int *) reg, 0x40000010, 0);
		if (reg[EAX] > 0)
			return reg[EAX];
	}
#endif							/* HAVE__CPUIDEX */

	return 0;
}

#else							/* defined(USE_SSE2) || defined(__i386__) */

/* prevent linker complaints about empty module */
extern int	pg_cpu_x86_dummy_variable;
int			pg_cpu_x86_dummy_variable = 0;

#endif							/* ! (USE_SSE2 || __i386__) */
