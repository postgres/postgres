/*-------------------------------------------------------------------------
 *
 * pg_crc32c_loongarch.c
 *	  Compute CRC-32C checksum using LoongArch CRCC instructions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_crc32c_loongarch.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include "port/pg_crc32c.h"

pg_crc32c
pg_comp_crc32c_loongarch(pg_crc32c crc, const void *data, size_t len)
{
	const unsigned char *p = data;
	const unsigned char *pend = p + len;

	/*
	 * LoongArch doesn't require alignment, but aligned memory access is
	 * significantly faster. Process leading bytes so that the loop below
	 * starts with a pointer aligned to eight bytes.
	 */
	if (!PointerIsAligned(p, uint16) &&
		p + 1 <= pend)
	{
		crc = __builtin_loongarch_crcc_w_b_w(*p, crc);
		p += 1;
	}
	if (!PointerIsAligned(p, uint32) &&
		p + 2 <= pend)
	{
		crc = __builtin_loongarch_crcc_w_h_w(*(uint16 *) p, crc);
		p += 2;
	}
	if (!PointerIsAligned(p, uint64) &&
		p + 4 <= pend)
	{
		crc = __builtin_loongarch_crcc_w_w_w(*(uint32 *) p, crc);
		p += 4;
	}

	/* Process eight bytes at a time, as far as we can. */
	while (p + 8 <= pend)
	{
		crc = __builtin_loongarch_crcc_w_d_w(*(uint64 *) p, crc);
		p += 8;
	}

	/* Process remaining 0-7 bytes. */
	if (p + 4 <= pend)
	{
		crc = __builtin_loongarch_crcc_w_w_w(*(uint32 *) p, crc);
		p += 4;
	}
	if (p + 2 <= pend)
	{
		crc = __builtin_loongarch_crcc_w_h_w(*(uint16 *) p, crc);
		p += 2;
	}
	if (p < pend)
	{
		crc = __builtin_loongarch_crcc_w_b_w(*p, crc);
	}

	return crc;
}
