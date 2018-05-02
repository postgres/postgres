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

#include <setjmp.h>

#include "libpq/pqsignal.h"
#include "port/pg_crc32c.h"


static sigjmp_buf illegal_instruction_jump;

/*
 * Probe by trying to execute pg_comp_crc32c_armv8().  If the instruction
 * isn't available, we expect to get SIGILL, which we can trap.
 */
static void
illegal_instruction_handler(int signo)
{
	siglongjmp(illegal_instruction_jump, 1);
}

static bool
pg_crc32c_armv8_available(void)
{
	uint64		data = 42;
	bool		result;

	pqsignal(SIGILL, illegal_instruction_handler);
	if (sigsetjmp(illegal_instruction_jump, 1) == 0)
		result = (pg_comp_crc32c_armv8(0, &data, sizeof(data)) == 0xdd439b0d);
	else
		result = false;
	pqsignal(SIGILL, SIG_DFL);

	return result;
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
