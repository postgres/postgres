/*-------------------------------------------------------------------------
 *
 * pg_popcount_aarch64.c
 *	  Holds the AArch64 popcount implementations.
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/pg_popcount_aarch64.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include "port/pg_bitutils.h"

#ifdef POPCNT_AARCH64

#include <arm_neon.h>

#ifdef USE_SVE_POPCNT_WITH_RUNTIME_CHECK
#include <arm_sve.h>

#if defined(HAVE_ELF_AUX_INFO) || defined(HAVE_GETAUXVAL)
#include <sys/auxv.h>
#endif
#endif

/*
 * The Neon versions are built regardless of whether we are building the SVE
 * versions.
 */
static uint64 pg_popcount_neon(const char *buf, int bytes);
static uint64 pg_popcount_masked_neon(const char *buf, int bytes, bits8 mask);

#ifdef USE_SVE_POPCNT_WITH_RUNTIME_CHECK

/*
 * These are the SVE implementations of the popcount functions.
 */
static uint64 pg_popcount_sve(const char *buf, int bytes);
static uint64 pg_popcount_masked_sve(const char *buf, int bytes, bits8 mask);

/*
 * The function pointers are initially set to "choose" functions.  These
 * functions will first set the pointers to the right implementations (based on
 * what the current CPU supports) and then will call the pointer to fulfill the
 * caller's request.
 */
static uint64 pg_popcount_choose(const char *buf, int bytes);
static uint64 pg_popcount_masked_choose(const char *buf, int bytes, bits8 mask);
uint64		(*pg_popcount_optimized) (const char *buf, int bytes) = pg_popcount_choose;
uint64		(*pg_popcount_masked_optimized) (const char *buf, int bytes, bits8 mask) = pg_popcount_masked_choose;

static inline bool
pg_popcount_sve_available(void)
{
#ifdef HAVE_ELF_AUX_INFO
	unsigned long value;

	return elf_aux_info(AT_HWCAP, &value, sizeof(value)) == 0 &&
		(value & HWCAP_SVE) != 0;
#elif defined(HAVE_GETAUXVAL)
	return (getauxval(AT_HWCAP) & HWCAP_SVE) != 0;
#else
	return false;
#endif
}

static inline void
choose_popcount_functions(void)
{
	if (pg_popcount_sve_available())
	{
		pg_popcount_optimized = pg_popcount_sve;
		pg_popcount_masked_optimized = pg_popcount_masked_sve;
	}
	else
	{
		pg_popcount_optimized = pg_popcount_neon;
		pg_popcount_masked_optimized = pg_popcount_masked_neon;
	}
}

static uint64
pg_popcount_choose(const char *buf, int bytes)
{
	choose_popcount_functions();
	return pg_popcount_optimized(buf, bytes);
}

static uint64
pg_popcount_masked_choose(const char *buf, int bytes, bits8 mask)
{
	choose_popcount_functions();
	return pg_popcount_masked_optimized(buf, bytes, mask);
}

/*
 * pg_popcount_sve
 *		Returns number of 1 bits in buf
 */
pg_attribute_target("arch=armv8-a+sve")
static uint64
pg_popcount_sve(const char *buf, int bytes)
{
	svbool_t	pred = svptrue_b64();
	svuint64_t	accum1 = svdup_u64(0),
				accum2 = svdup_u64(0),
				accum3 = svdup_u64(0),
				accum4 = svdup_u64(0);
	uint32		vec_len = svcntb(),
				bytes_per_iteration = 4 * vec_len;
	uint64		popcnt = 0;

	/*
	 * For better instruction-level parallelism, each loop iteration operates
	 * on a block of four registers.
	 */
	for (; bytes >= bytes_per_iteration; bytes -= bytes_per_iteration)
	{
		svuint64_t	vec;

		vec = svld1_u64(pred, (const uint64 *) buf);
		accum1 = svadd_u64_x(pred, accum1, svcnt_u64_x(pred, vec));
		buf += vec_len;

		vec = svld1_u64(pred, (const uint64 *) buf);
		accum2 = svadd_u64_x(pred, accum2, svcnt_u64_x(pred, vec));
		buf += vec_len;

		vec = svld1_u64(pred, (const uint64 *) buf);
		accum3 = svadd_u64_x(pred, accum3, svcnt_u64_x(pred, vec));
		buf += vec_len;

		vec = svld1_u64(pred, (const uint64 *) buf);
		accum4 = svadd_u64_x(pred, accum4, svcnt_u64_x(pred, vec));
		buf += vec_len;
	}

	/*
	 * If enough data remains, do another iteration on a block of two
	 * registers.
	 */
	bytes_per_iteration = 2 * vec_len;
	if (bytes >= bytes_per_iteration)
	{
		svuint64_t	vec;

		vec = svld1_u64(pred, (const uint64 *) buf);
		accum1 = svadd_u64_x(pred, accum1, svcnt_u64_x(pred, vec));
		buf += vec_len;

		vec = svld1_u64(pred, (const uint64 *) buf);
		accum2 = svadd_u64_x(pred, accum2, svcnt_u64_x(pred, vec));
		buf += vec_len;

		bytes -= bytes_per_iteration;
	}

	/*
	 * Add the accumulators.
	 */
	popcnt += svaddv_u64(pred, svadd_u64_x(pred, accum1, accum2));
	popcnt += svaddv_u64(pred, svadd_u64_x(pred, accum3, accum4));

	/*
	 * Process any remaining data.
	 */
	for (; bytes > 0; bytes -= vec_len)
	{
		svuint8_t	vec;

		pred = svwhilelt_b8_s32(0, bytes);
		vec = svld1_u8(pred, (const uint8 *) buf);
		popcnt += svaddv_u8(pred, svcnt_u8_x(pred, vec));
		buf += vec_len;
	}

	return popcnt;
}

/*
 * pg_popcount_masked_sve
 *		Returns number of 1 bits in buf after applying the mask to each byte
 */
pg_attribute_target("arch=armv8-a+sve")
static uint64
pg_popcount_masked_sve(const char *buf, int bytes, bits8 mask)
{
	svbool_t	pred = svptrue_b64();
	svuint64_t	accum1 = svdup_u64(0),
				accum2 = svdup_u64(0),
				accum3 = svdup_u64(0),
				accum4 = svdup_u64(0);
	uint32		vec_len = svcntb(),
				bytes_per_iteration = 4 * vec_len;
	uint64		popcnt = 0,
				mask64 = ~UINT64CONST(0) / 0xFF * mask;

	/*
	 * For better instruction-level parallelism, each loop iteration operates
	 * on a block of four registers.
	 */
	for (; bytes >= bytes_per_iteration; bytes -= bytes_per_iteration)
	{
		svuint64_t	vec;

		vec = svand_n_u64_x(pred, svld1_u64(pred, (const uint64 *) buf), mask64);
		accum1 = svadd_u64_x(pred, accum1, svcnt_u64_x(pred, vec));
		buf += vec_len;

		vec = svand_n_u64_x(pred, svld1_u64(pred, (const uint64 *) buf), mask64);
		accum2 = svadd_u64_x(pred, accum2, svcnt_u64_x(pred, vec));
		buf += vec_len;

		vec = svand_n_u64_x(pred, svld1_u64(pred, (const uint64 *) buf), mask64);
		accum3 = svadd_u64_x(pred, accum3, svcnt_u64_x(pred, vec));
		buf += vec_len;

		vec = svand_n_u64_x(pred, svld1_u64(pred, (const uint64 *) buf), mask64);
		accum4 = svadd_u64_x(pred, accum4, svcnt_u64_x(pred, vec));
		buf += vec_len;
	}

	/*
	 * If enough data remains, do another iteration on a block of two
	 * registers.
	 */
	bytes_per_iteration = 2 * vec_len;
	if (bytes >= bytes_per_iteration)
	{
		svuint64_t	vec;

		vec = svand_n_u64_x(pred, svld1_u64(pred, (const uint64 *) buf), mask64);
		accum1 = svadd_u64_x(pred, accum1, svcnt_u64_x(pred, vec));
		buf += vec_len;

		vec = svand_n_u64_x(pred, svld1_u64(pred, (const uint64 *) buf), mask64);
		accum2 = svadd_u64_x(pred, accum2, svcnt_u64_x(pred, vec));
		buf += vec_len;

		bytes -= bytes_per_iteration;
	}

	/*
	 * Add the accumulators.
	 */
	popcnt += svaddv_u64(pred, svadd_u64_x(pred, accum1, accum2));
	popcnt += svaddv_u64(pred, svadd_u64_x(pred, accum3, accum4));

	/*
	 * Process any remaining data.
	 */
	for (; bytes > 0; bytes -= vec_len)
	{
		svuint8_t	vec;

		pred = svwhilelt_b8_s32(0, bytes);
		vec = svand_n_u8_x(pred, svld1_u8(pred, (const uint8 *) buf), mask);
		popcnt += svaddv_u8(pred, svcnt_u8_x(pred, vec));
		buf += vec_len;
	}

	return popcnt;
}

#else							/* USE_SVE_POPCNT_WITH_RUNTIME_CHECK */

/*
 * When the SVE version isn't available, there's no point in using function
 * pointers to vary the implementation.  We instead just make these actual
 * external functions when USE_SVE_POPCNT_WITH_RUNTIME_CHECK is not defined.
 * The compiler should be able to inline the Neon versions here.
 */
uint64
pg_popcount_optimized(const char *buf, int bytes)
{
	return pg_popcount_neon(buf, bytes);
}

uint64
pg_popcount_masked_optimized(const char *buf, int bytes, bits8 mask)
{
	return pg_popcount_masked_neon(buf, bytes, mask);
}

#endif							/* ! USE_SVE_POPCNT_WITH_RUNTIME_CHECK */

/*
 * pg_popcount32
 *		Return number of 1 bits in word
 */
int
pg_popcount32(uint32 word)
{
	return pg_popcount64((uint64) word);
}

/*
 * pg_popcount64
 *		Return number of 1 bits in word
 */
int
pg_popcount64(uint64 word)
{
	/*
	 * For some compilers, __builtin_popcountl() already emits Neon
	 * instructions.  The line below should compile to the same code on those
	 * systems.
	 */
	return vaddv_u8(vcnt_u8(vld1_u8((const uint8 *) &word)));
}

/*
 * pg_popcount_neon
 *		Returns number of 1 bits in buf
 */
static uint64
pg_popcount_neon(const char *buf, int bytes)
{
	uint8x16_t	vec;
	uint64x2_t	accum1 = vdupq_n_u64(0),
				accum2 = vdupq_n_u64(0),
				accum3 = vdupq_n_u64(0),
				accum4 = vdupq_n_u64(0);
	uint32		bytes_per_iteration = 4 * sizeof(uint8x16_t);
	uint64		popcnt = 0;

	/*
	 * For better instruction-level parallelism, each loop iteration operates
	 * on a block of four registers.
	 */
	for (; bytes >= bytes_per_iteration; bytes -= bytes_per_iteration)
	{
		vec = vld1q_u8((const uint8 *) buf);
		accum1 = vpadalq_u32(accum1, vpaddlq_u16(vpaddlq_u8(vcntq_u8(vec))));
		buf += sizeof(uint8x16_t);

		vec = vld1q_u8((const uint8 *) buf);
		accum2 = vpadalq_u32(accum2, vpaddlq_u16(vpaddlq_u8(vcntq_u8(vec))));
		buf += sizeof(uint8x16_t);

		vec = vld1q_u8((const uint8 *) buf);
		accum3 = vpadalq_u32(accum3, vpaddlq_u16(vpaddlq_u8(vcntq_u8(vec))));
		buf += sizeof(uint8x16_t);

		vec = vld1q_u8((const uint8 *) buf);
		accum4 = vpadalq_u32(accum4, vpaddlq_u16(vpaddlq_u8(vcntq_u8(vec))));
		buf += sizeof(uint8x16_t);
	}

	/*
	 * If enough data remains, do another iteration on a block of two
	 * registers.
	 */
	bytes_per_iteration = 2 * sizeof(uint8x16_t);
	if (bytes >= bytes_per_iteration)
	{
		vec = vld1q_u8((const uint8 *) buf);
		accum1 = vpadalq_u32(accum1, vpaddlq_u16(vpaddlq_u8(vcntq_u8(vec))));
		buf += sizeof(uint8x16_t);

		vec = vld1q_u8((const uint8 *) buf);
		accum2 = vpadalq_u32(accum2, vpaddlq_u16(vpaddlq_u8(vcntq_u8(vec))));
		buf += sizeof(uint8x16_t);

		bytes -= bytes_per_iteration;
	}

	/*
	 * Add the accumulators.
	 */
	popcnt += vaddvq_u64(vaddq_u64(accum1, accum2));
	popcnt += vaddvq_u64(vaddq_u64(accum3, accum4));

	/*
	 * Process remaining 8-byte blocks.
	 */
	for (; bytes >= sizeof(uint64); bytes -= sizeof(uint64))
	{
		popcnt += pg_popcount64(*((uint64 *) buf));
		buf += sizeof(uint64);
	}

	/*
	 * Process any remaining data byte-by-byte.
	 */
	while (bytes--)
		popcnt += pg_number_of_ones[(unsigned char) *buf++];

	return popcnt;
}

/*
 * pg_popcount_masked_neon
 *		Returns number of 1 bits in buf after applying the mask to each byte
 */
static uint64
pg_popcount_masked_neon(const char *buf, int bytes, bits8 mask)
{
	uint8x16_t	vec,
				maskv = vdupq_n_u8(mask);
	uint64x2_t	accum1 = vdupq_n_u64(0),
				accum2 = vdupq_n_u64(0),
				accum3 = vdupq_n_u64(0),
				accum4 = vdupq_n_u64(0);
	uint32		bytes_per_iteration = 4 * sizeof(uint8x16_t);
	uint64		popcnt = 0,
				mask64 = ~UINT64CONST(0) / 0xFF * mask;

	/*
	 * For better instruction-level parallelism, each loop iteration operates
	 * on a block of four registers.
	 */
	for (; bytes >= bytes_per_iteration; bytes -= bytes_per_iteration)
	{
		vec = vandq_u8(vld1q_u8((const uint8 *) buf), maskv);
		accum1 = vpadalq_u32(accum1, vpaddlq_u16(vpaddlq_u8(vcntq_u8(vec))));
		buf += sizeof(uint8x16_t);

		vec = vandq_u8(vld1q_u8((const uint8 *) buf), maskv);
		accum2 = vpadalq_u32(accum2, vpaddlq_u16(vpaddlq_u8(vcntq_u8(vec))));
		buf += sizeof(uint8x16_t);

		vec = vandq_u8(vld1q_u8((const uint8 *) buf), maskv);
		accum3 = vpadalq_u32(accum3, vpaddlq_u16(vpaddlq_u8(vcntq_u8(vec))));
		buf += sizeof(uint8x16_t);

		vec = vandq_u8(vld1q_u8((const uint8 *) buf), maskv);
		accum4 = vpadalq_u32(accum4, vpaddlq_u16(vpaddlq_u8(vcntq_u8(vec))));
		buf += sizeof(uint8x16_t);
	}

	/*
	 * If enough data remains, do another iteration on a block of two
	 * registers.
	 */
	bytes_per_iteration = 2 * sizeof(uint8x16_t);
	if (bytes >= bytes_per_iteration)
	{
		vec = vandq_u8(vld1q_u8((const uint8 *) buf), maskv);
		accum1 = vpadalq_u32(accum1, vpaddlq_u16(vpaddlq_u8(vcntq_u8(vec))));
		buf += sizeof(uint8x16_t);

		vec = vandq_u8(vld1q_u8((const uint8 *) buf), maskv);
		accum2 = vpadalq_u32(accum2, vpaddlq_u16(vpaddlq_u8(vcntq_u8(vec))));
		buf += sizeof(uint8x16_t);

		bytes -= bytes_per_iteration;
	}

	/*
	 * Add the accumulators.
	 */
	popcnt += vaddvq_u64(vaddq_u64(accum1, accum2));
	popcnt += vaddvq_u64(vaddq_u64(accum3, accum4));

	/*
	 * Process remaining 8-byte blocks.
	 */
	for (; bytes >= sizeof(uint64); bytes -= sizeof(uint64))
	{
		popcnt += pg_popcount64(*((uint64 *) buf) & mask64);
		buf += sizeof(uint64);
	}

	/*
	 * Process any remaining data byte-by-byte.
	 */
	while (bytes--)
		popcnt += pg_number_of_ones[(unsigned char) *buf++ & mask];

	return popcnt;
}

#endif							/* POPCNT_AARCH64 */
