/*-------------------------------------------------------------------------
 *
 * aio_target.c
 *	  AIO - Functionality related to executing IO for different targets
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/storage/aio/aio_target.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/smgr.h"


/*
 * Registry for entities that can be the target of AIO.
 */
static const PgAioTargetInfo *pgaio_target_info[] = {
	[PGAIO_TID_INVALID] = &(PgAioTargetInfo) {
		.name = "invalid",
	},
	[PGAIO_TID_SMGR] = &aio_smgr_target_info,
};



/* --------------------------------------------------------------------------------
 * Public target related functions operating on IO Handles
 * --------------------------------------------------------------------------------
 */

bool
pgaio_io_has_target(PgAioHandle *ioh)
{
	return ioh->target != PGAIO_TID_INVALID;
}

/*
 * Return the name for the target associated with the IO. Mostly useful for
 * debugging/logging.
 */
const char *
pgaio_io_get_target_name(PgAioHandle *ioh)
{
	/* explicitly allow INVALID here, function used by debug messages */
	Assert(ioh->target >= PGAIO_TID_INVALID && ioh->target < PGAIO_TID_COUNT);

	return pgaio_target_info[ioh->target]->name;
}

/*
 * Assign a target to the IO.
 *
 * This has to be called exactly once before pgaio_io_start_*() is called.
 */
void
pgaio_io_set_target(PgAioHandle *ioh, PgAioTargetID targetid)
{
	Assert(ioh->state == PGAIO_HS_HANDED_OUT);
	Assert(ioh->target == PGAIO_TID_INVALID);

	ioh->target = targetid;
}

PgAioTargetData *
pgaio_io_get_target_data(PgAioHandle *ioh)
{
	return &ioh->target_data;
}

/*
 * Return a stringified description of the IO's target.
 *
 * The string is localized and allocated in the current memory context.
 */
char *
pgaio_io_get_target_description(PgAioHandle *ioh)
{
	/* disallow INVALID, there wouldn't be a description */
	Assert(ioh->target > PGAIO_TID_INVALID && ioh->target < PGAIO_TID_COUNT);

	return pgaio_target_info[ioh->target]->describe_identity(&ioh->target_data);
}



/* --------------------------------------------------------------------------------
 * Internal target related functions operating on IO Handles
 * --------------------------------------------------------------------------------
 */

/*
 * Internal: Check if pgaio_io_reopen() is available for the IO.
 */
bool
pgaio_io_can_reopen(PgAioHandle *ioh)
{
	Assert(ioh->target > PGAIO_TID_INVALID && ioh->target < PGAIO_TID_COUNT);

	return pgaio_target_info[ioh->target]->reopen != NULL;
}

/*
 * Internal: Before executing an IO outside of the context of the process the
 * IO has been staged in, the file descriptor has to be reopened - any FD
 * referenced in the IO itself, won't be valid in the separate process.
 */
void
pgaio_io_reopen(PgAioHandle *ioh)
{
	Assert(ioh->target > PGAIO_TID_INVALID && ioh->target < PGAIO_TID_COUNT);
	Assert(ioh->op > PGAIO_OP_INVALID && ioh->op < PGAIO_OP_COUNT);

	pgaio_target_info[ioh->target]->reopen(ioh);
}
