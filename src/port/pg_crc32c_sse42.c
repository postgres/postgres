/*-------------------------------------------------------------------------
 *
 * pg_crc32c_sse42.c
 *	  Compute CRC-32C checksum using Intel SSE 4.2 instructions.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_crc32c_sse42.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include <nmmintrin.h>

#include "port/pg_crc32c.h"

pg_attribute_no_sanitize_alignment()
pg_attribute_target("sse4.2")
pg_crc32c
pg_comp_crc32c_sse42(pg_crc32c crc, const void *data, size_t len)
{
	const unsigned char *p = data;
	const unsigned char *pend = p + len;

	/*
	 * Process eight bytes of data at a time.
	 *
	 * NB: We do unaligned accesses here. The Intel architecture allows that,
	 * and performance testing didn't show any performance gain from aligning
	 * the begin address.
	 */
#ifdef __x86_64__
	while (p + 8 <= pend)
	{
		crc = (uint32) _mm_crc32_u64(crc, *((const uint64 *) p));
		p += 8;
	}

	/* Process remaining full four bytes if any */
	if (p + 4 <= pend)
	{
		crc = _mm_crc32_u32(crc, *((const unsigned int *) p));
		p += 4;
	}
#else

	/*
	 * Process four bytes at a time. (The eight byte instruction is not
	 * available on the 32-bit x86 architecture).
	 */
	while (p + 4 <= pend)
	{
		crc = _mm_crc32_u32(crc, *((const unsigned int *) p));
		p += 4;
	}
#endif							/* __x86_64__ */

	/* Process any remaining bytes one at a time. */
	while (p < pend)
	{
		crc = _mm_crc32_u8(crc, *p);
		p++;
	}

	return crc;
}
