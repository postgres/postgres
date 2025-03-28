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
 * pg_popcount_optimized
 *		Returns number of 1 bits in buf
 */
uint64
pg_popcount_optimized(const char *buf, int bytes)
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
 * pg_popcount_masked_optimized
 *		Returns number of 1 bits in buf after applying the mask to each byte
 */
uint64
pg_popcount_masked_optimized(const char *buf, int bytes, bits8 mask)
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
	 * Process remining 8-byte blocks.
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
