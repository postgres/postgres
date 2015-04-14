/*-------------------------------------------------------------------------
 *
 * pg_crc32c_sse42.c
 *	  Compute CRC-32C checksum using Intel SSE 4.2 instructions.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_crc32c_sse42.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include "port/pg_crc32c.h"

#include <nmmintrin.h>

pg_crc32c
pg_comp_crc32c_sse42(pg_crc32c crc, const void *data, size_t len)
{
	const unsigned char *p = data;
	const uint64 *p8;

	/*
	 * Process eight bytes of data at a time.
	 *
	 * NB: We do unaligned 8-byte accesses here. The Intel architecture
	 * allows that, and performance testing didn't show any performance
	 * gain from aligning the beginning address.
	 */
	p8 = (const uint64 *) p;
	while (len >= 8)
	{
		crc = (uint32) _mm_crc32_u64(crc, *p8++);
		len -= 8;
	}

	/*
	 * Handle any remaining bytes one at a time.
	 */
	p = (const unsigned char *) p8;
	while (len > 0)
	{
		crc = _mm_crc32_u8(crc, *p++);
		len--;
	}

	return crc;
}
