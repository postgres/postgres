/*-----------------------------------------------------------------------
 * ascii.h
 *
 *	 Portions Copyright (c) 1999-2022, PostgreSQL Global Development Group
 *
 * src/include/utils/ascii.h
 *
 *-----------------------------------------------------------------------
 */

#ifndef _ASCII_H_
#define _ASCII_H_

extern void ascii_safe_strlcpy(char *dest, const char *src, size_t destsiz);

/*
 * Verify a chunk of bytes for valid ASCII.
 *
 * Returns false if the input contains any zero bytes or bytes with the
 * high-bit set. Input len must be a multiple of 8.
 */
static inline bool
is_valid_ascii(const unsigned char *s, int len)
{
	uint64		chunk,
				highbit_cum = UINT64CONST(0),
				zero_cum = UINT64CONST(0x8080808080808080);

	Assert(len % sizeof(chunk) == 0);

	while (len > 0)
	{
		memcpy(&chunk, s, sizeof(chunk));

		/*
		 * Capture any zero bytes in this chunk.
		 *
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
		zero_cum &= (chunk + UINT64CONST(0x7f7f7f7f7f7f7f7f));

		/* Capture any set bits in this chunk. */
		highbit_cum |= chunk;

		s += sizeof(chunk);
		len -= sizeof(chunk);
	}

	/* Check if any high bits in the high bit accumulator got set. */
	if (highbit_cum & UINT64CONST(0x8080808080808080))
		return false;

	/* Check if any high bits in the zero accumulator got cleared. */
	if (zero_cum != UINT64CONST(0x8080808080808080))
		return false;

	return true;
}

#endif							/* _ASCII_H_ */
