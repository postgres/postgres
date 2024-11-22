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
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_crc32c_armv8_choose.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#if defined(HAVE_ELF_AUX_INFO) || defined(HAVE_GETAUXVAL)
#include <sys/auxv.h>
#if defined(__linux__) && !defined(__aarch64__) && !defined(HWCAP2_CRC32)
#include <asm/hwcap.h>
#endif
#endif

#include "port/pg_crc32c.h"

static bool
pg_crc32c_armv8_available(void)
{
#if defined(HAVE_ELF_AUX_INFO)
	unsigned long value;

#ifdef __aarch64__
	return elf_aux_info(AT_HWCAP, &value, sizeof(value)) == 0 &&
		(value & HWCAP_CRC32) != 0;
#else
	return elf_aux_info(AT_HWCAP2, &value, sizeof(value)) == 0 &&
		(value & HWCAP2_CRC32) != 0;
#endif
#elif defined(HAVE_GETAUXVAL)
#ifdef __aarch64__
	return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
#else
	return (getauxval(AT_HWCAP2) & HWCAP2_CRC32) != 0;
#endif
#else
	return false;
#endif
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
