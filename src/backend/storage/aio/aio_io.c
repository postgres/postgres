/*-------------------------------------------------------------------------
 *
 * aio_io.c
 *    AIO - Low Level IO Handling
 *
 * Functions related to associating IO operations to IO Handles and IO-method
 * independent support functions for actually performing IO.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/storage/aio/aio_io.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/fd.h"
#include "utils/wait_event.h"


static void pgaio_io_before_start(PgAioHandle *ioh);



/* --------------------------------------------------------------------------------
 * Public IO related functions operating on IO Handles
 * --------------------------------------------------------------------------------
 */

/*
 * Scatter/gather IO needs to associate an iovec with the Handle. To support
 * worker mode this data needs to be in shared memory.
 */
int
pgaio_io_get_iovec(PgAioHandle *ioh, struct iovec **iov)
{
	Assert(ioh->state == PGAIO_HS_HANDED_OUT);

	*iov = &pgaio_ctl->iovecs[ioh->iovec_off];

	return PG_IOV_MAX;
}

PgAioOp
pgaio_io_get_op(PgAioHandle *ioh)
{
	return ioh->op;
}

PgAioOpData *
pgaio_io_get_op_data(PgAioHandle *ioh)
{
	return &ioh->op_data;
}



/* --------------------------------------------------------------------------------
 * "Start" routines for individual IO operations
 *
 * These are called by the code actually initiating an IO, to associate the IO
 * specific data with an AIO handle.
 *
 * Each of the "start" routines first needs to call pgaio_io_before_start(),
 * then fill IO specific fields in the handle and then finally call
 * pgaio_io_stage().
 * --------------------------------------------------------------------------------
 */

void
pgaio_io_start_readv(PgAioHandle *ioh,
					 int fd, int iovcnt, uint64 offset)
{
	pgaio_io_before_start(ioh);

	ioh->op_data.read.fd = fd;
	ioh->op_data.read.offset = offset;
	ioh->op_data.read.iov_length = iovcnt;

	pgaio_io_stage(ioh, PGAIO_OP_READV);
}

void
pgaio_io_start_writev(PgAioHandle *ioh,
					  int fd, int iovcnt, uint64 offset)
{
	pgaio_io_before_start(ioh);

	ioh->op_data.write.fd = fd;
	ioh->op_data.write.offset = offset;
	ioh->op_data.write.iov_length = iovcnt;

	pgaio_io_stage(ioh, PGAIO_OP_WRITEV);
}



/* --------------------------------------------------------------------------------
 * Internal IO related functions operating on IO Handles
 * --------------------------------------------------------------------------------
 */

/*
 * Execute IO operation synchronously. This is implemented here, not in
 * method_sync.c, because other IO methods also might use it / fall back to
 * it.
 */
void
pgaio_io_perform_synchronously(PgAioHandle *ioh)
{
	ssize_t		result = 0;
	struct iovec *iov = &pgaio_ctl->iovecs[ioh->iovec_off];

	START_CRIT_SECTION();

	/* Perform IO. */
	switch (ioh->op)
	{
		case PGAIO_OP_READV:
			pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_READ);
			result = pg_preadv(ioh->op_data.read.fd, iov,
							   ioh->op_data.read.iov_length,
							   ioh->op_data.read.offset);
			pgstat_report_wait_end();
			break;
		case PGAIO_OP_WRITEV:
			pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_WRITE);
			result = pg_pwritev(ioh->op_data.write.fd, iov,
								ioh->op_data.write.iov_length,
								ioh->op_data.write.offset);
			pgstat_report_wait_end();
			break;
		case PGAIO_OP_INVALID:
			elog(ERROR, "trying to execute invalid IO operation");
	}

	ioh->result = result < 0 ? -errno : result;

	pgaio_io_process_completion(ioh, ioh->result);

	END_CRIT_SECTION();
}

/*
 * Helper function to be called by IO operation preparation functions, before
 * any data in the handle is set.  Mostly to centralize assertions.
 */
static void
pgaio_io_before_start(PgAioHandle *ioh)
{
	Assert(ioh->state == PGAIO_HS_HANDED_OUT);
	Assert(pgaio_my_backend->handed_out_io == ioh);
	Assert(pgaio_io_has_target(ioh));
	Assert(ioh->op == PGAIO_OP_INVALID);

	/*
	 * Otherwise the FDs referenced by the IO could be closed due to interrupt
	 * processing.
	 */
	Assert(!INTERRUPTS_CAN_BE_PROCESSED());
}

/*
 * Could be made part of the public interface, but it's not clear there's
 * really a use case for that.
 */
const char *
pgaio_io_get_op_name(PgAioHandle *ioh)
{
	Assert(ioh->op >= 0 && ioh->op < PGAIO_OP_COUNT);

	switch (ioh->op)
	{
		case PGAIO_OP_INVALID:
			return "invalid";
		case PGAIO_OP_READV:
			return "readv";
		case PGAIO_OP_WRITEV:
			return "writev";
	}

	return NULL;				/* silence compiler */
}

/*
 * Used to determine if an IO needs to be waited upon before the file
 * descriptor can be closed.
 */
bool
pgaio_io_uses_fd(PgAioHandle *ioh, int fd)
{
	Assert(ioh->state >= PGAIO_HS_DEFINED);

	switch (ioh->op)
	{
		case PGAIO_OP_READV:
			return ioh->op_data.read.fd == fd;
		case PGAIO_OP_WRITEV:
			return ioh->op_data.write.fd == fd;
		case PGAIO_OP_INVALID:
			return false;
	}

	return false;				/* silence compiler */
}

/*
 * Return the iovec and its length. Currently only expected to be used by
 * debugging infrastructure
 */
int
pgaio_io_get_iovec_length(PgAioHandle *ioh, struct iovec **iov)
{
	Assert(ioh->state >= PGAIO_HS_DEFINED);

	*iov = &pgaio_ctl->iovecs[ioh->iovec_off];

	switch (ioh->op)
	{
		case PGAIO_OP_READV:
			return ioh->op_data.read.iov_length;
		case PGAIO_OP_WRITEV:
			return ioh->op_data.write.iov_length;
		default:
			pg_unreachable();
			return 0;
	}
}
