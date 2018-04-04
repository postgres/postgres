/*-------------------------------------------------------------------------
 *
 * pg_crc32c_armv8_choose.c
 *	  Choose between ARMv8 and software CRC-32C implementation.
 *
 * On first call, checks if the CPU we're running on supports the ARMv8
 * CRC Extension. If it does, use the special instructions for CRC-32C
 * computation. Otherwise, fall back to the pure software implementation
 * (slicing-by-8).
 *
 * XXX: The glibc-specific getauxval() function, with the HWCAP_CRC32
 * flag, is used to determine if the CRC Extension is available on the
 * current platform. Is there a more portable way to determine that?
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_crc32c_armv8_choose.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <sys/auxv.h>
#include <asm/hwcap.h>

#include "port/pg_crc32c.h"

static bool
pg_crc32c_armv8_available(void)
{
	unsigned long auxv = getauxval(AT_HWCAP);

	return (auxv & HWCAP_CRC32) != 0;
}

/*
 * This gets called on the first call. It replaces the function pointer
 * so that subsequent calls are routed directly to the chosen implementation.
 */
static pg_crc32c
pg_comp_crc32c_choose(pg_crc32c crc, const void *data, size_t len)
{
	if (pg_crc32c_armv8_available())
		pg_comp_crc32c = pg_comp_crc32c_armv8;
	else
		pg_comp_crc32c = pg_comp_crc32c_sb8;

	return pg_comp_crc32c(crc, data, len);
}

pg_crc32c	(*pg_comp_crc32c) (pg_crc32c crc, const void *data, size_t len) = pg_comp_crc32c_choose;
