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
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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

#include <setjmp.h>
#include <signal.h>

#include "port/pg_crc32c.h"


static sigjmp_buf illegal_instruction_jump;

/*
 * Probe by trying to execute pg_comp_crc32c_armv8().  If the instruction
 * isn't available, we expect to get SIGILL, which we can trap.
 */
static void
illegal_instruction_handler(SIGNAL_ARGS)
{
	siglongjmp(illegal_instruction_jump, 1);
}

static bool
pg_crc32c_armv8_available(void)
{
	uint64		data = 42;
	int			result;

	/*
	 * Be careful not to do anything that might throw an error while we have
	 * the SIGILL handler set to a nonstandard value.
	 */
	pqsignal(SIGILL, illegal_instruction_handler);
	if (sigsetjmp(illegal_instruction_jump, 1) == 0)
	{
		/* Rather than hard-wiring an expected result, compare to SB8 code */
		result = (pg_comp_crc32c_armv8(0, &data, sizeof(data)) ==
				  pg_comp_crc32c_sb8(0, &data, sizeof(data)));
	}
	else
	{
		/* We got the SIGILL trap */
		result = -1;
	}
	pqsignal(SIGILL, SIG_DFL);

#ifndef FRONTEND
	/* We don't expect this case, so complain loudly */
	if (result == 0)
		elog(ERROR, "crc32 hardware and software results disagree");

	elog(DEBUG1, "using armv8 crc32 hardware = %d", (result > 0));
#endif

	return (result > 0);
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
