/*-----------------------------------------------------------------------
 * ascii.h
 *
 *	 Portions Copyright (c) 1999-2023, PostgreSQL Global Development Group
 *
 * src/include/utils/ascii.h
 *
 *-----------------------------------------------------------------------
 */

#ifndef _ASCII_H_
#define _ASCII_H_

#include "port/simd.h"

extern void ascii_safe_strlcpy(char *dest, const char *src, size_t destsiz);

/*
 * Verify a chunk of bytes for valid ASCII.
 *
 * Returns false if the input contains any zero bytes or bytes with the
 * high-bit set. Input len must be a multiple of the chunk size (8 or 16).
 */
static inline bool
is_valid_ascii(const unsigned char *s, int len)
{
	const unsigned char *const s_end = s + len;
	Vector8		chunk;
	Vector8		highbit_cum = vector8_broadcast(0);
#ifdef USE_NO_SIMD
	Vector8		zero_cum = vector8_broadcast(0x80);
#endif

	Assert(len % sizeof(chunk) == 0);

	while (s < s_end)
	{
		vector8_load(&chunk, s);

		/* Capture any zero bytes in this chunk. */
#ifdef USE_NO_SIMD

		/*
		 * First, add 0x7f to each byte. This sets the high bit in each byte,
		 * unless it was a zero. If any resulting high bits are zero, the
		 * corresponding high bits in the zero accumulator will be cleared.
		 *
		 * If none of the bytes in the chunk had the high bit set, the max
		 * value each byte can have after the addition is 0x7f + 0x7f = 0xfe,
		 * and we don't need to worry about carrying over to the next byte. If
		 * any input bytes did have the high bit set, it doesn't matter
		 * because we check for those separately.
		 */
		zero_cum &= (chunk + vector8_broadcast(0x7F));
#else

		/*
		 * Set all bits in each lane of the highbit accumulator where input
		 * bytes are zero.
		 */
		highbit_cum = vector8_or(highbit_cum,
								 vector8_eq(chunk, vector8_broadcast(0)));
#endif

		/* Capture all set bits in this chunk. */
		highbit_cum = vector8_or(highbit_cum, chunk);

		s += sizeof(chunk);
	}

	/* Check if any high bits in the high bit accumulator got set. */
	if (vector8_is_highbit_set(highbit_cum))
		return false;

#ifdef USE_NO_SIMD
	/* Check if any high bits in the zero accumulator got cleared. */
	if (zero_cum != vector8_broadcast(0x80))
		return false;
#endif

	return true;
}

#endif							/* _ASCII_H_ */
