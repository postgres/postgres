/*-------------------------------------------------------------------------
 *
 * method_sync.c
 *    AIO - perform "AIO" by executing it synchronously
 *
 * This method is mainly to check if AIO use causes regressions. Other IO
 * methods might also fall back to the synchronous method for functionality
 * they cannot provide.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/aio/method_sync.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/aio.h"
#include "storage/aio_internal.h"

static bool pgaio_sync_needs_synchronous_execution(PgAioHandle *ioh);
static int	pgaio_sync_submit(uint16 num_staged_ios, PgAioHandle **staged_ios);


const IoMethodOps pgaio_sync_ops = {
	.needs_synchronous_execution = pgaio_sync_needs_synchronous_execution,
	.submit = pgaio_sync_submit,
};



static bool
pgaio_sync_needs_synchronous_execution(PgAioHandle *ioh)
{
	return true;
}

static int
pgaio_sync_submit(uint16 num_staged_ios, PgAioHandle **staged_ios)
{
	elog(ERROR, "IO should have been executed synchronously");

	return 0;
}
