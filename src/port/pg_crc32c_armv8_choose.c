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

#if defined(__NetBSD__)
#include <sys/sysctl.h>
#if defined(__aarch64__)
#include <aarch64/armreg.h>
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
#elif defined(__NetBSD__)
	/*
	 * On NetBSD we can read the Instruction Set Attribute Registers via
	 * sysctl.  For doubtless-historical reasons the sysctl interface is
	 * completely different on 64-bit than 32-bit, but the underlying
	 * registers contain the same fields.
	 */
#define ISAR0_CRC32_BITPOS 16
#define ISAR0_CRC32_BITWIDTH 4
#define WIDTHMASK(w)	((1 << (w)) - 1)
#define SYSCTL_CPU_ID_MAXSIZE 64

	size_t		len;
	uint64		sysctlbuf[SYSCTL_CPU_ID_MAXSIZE];
#if defined(__aarch64__)
	/* We assume cpu0 is representative of all the machine's CPUs. */
	const char *path = "machdep.cpu0.cpu_id";
	size_t		expected_len = sizeof(struct aarch64_sysctl_cpu_id);
#define ISAR0 ((struct aarch64_sysctl_cpu_id *) sysctlbuf)->ac_aa64isar0
#else
	const char *path = "machdep.id_isar";
	size_t		expected_len = 6 * sizeof(int);
#define ISAR0 ((int *) sysctlbuf)[5]
#endif
	uint64		fld;

	/* Fetch the appropriate set of register values. */
	len = sizeof(sysctlbuf);
	memset(sysctlbuf, 0, len);
	if (sysctlbyname(path, sysctlbuf, &len, NULL, 0) != 0)
		return false;			/* perhaps kernel is 64-bit and we aren't? */
	if (len != expected_len)
		return false;			/* kernel API change? */

	/* Fetch the CRC32 field from ISAR0. */
	fld = (ISAR0 >> ISAR0_CRC32_BITPOS) & WIDTHMASK(ISAR0_CRC32_BITWIDTH);

	/*
	 * Current documentation defines only the field values 0 (No CRC32) and 1
	 * (CRC32B/CRC32H/CRC32W/CRC32X/CRC32CB/CRC32CH/CRC32CW/CRC32CX).  Assume
	 * that any future nonzero value will be a superset of 1.
	 */
	return (fld != 0);
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
