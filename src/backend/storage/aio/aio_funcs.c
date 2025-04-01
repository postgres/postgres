/*-------------------------------------------------------------------------
 *
 * aio_funcs.c
 *    AIO - SQL interface for AIO
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *    src/backend/storage/aio/aio_funcs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "nodes/execnodes.h"
#include "port/atomics.h"
#include "storage/aio_internal.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "storage/procnumber.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/tuplestore.h"


/*
 * Byte length of an iovec.
 */
static size_t
iov_byte_length(const struct iovec *iov, int cnt)
{
	size_t		len = 0;

	for (int i = 0; i < cnt; i++)
	{
		len += iov[i].iov_len;
	}

	return len;
}

Datum
pg_get_aios(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);

#define PG_GET_AIOS_COLS	15

	for (uint64 i = 0; i < pgaio_ctl->io_handle_count; i++)
	{
		PgAioHandle *live_ioh = &pgaio_ctl->io_handles[i];
		uint32		ioh_id = pgaio_io_get_id(live_ioh);
		Datum		values[PG_GET_AIOS_COLS] = {0};
		bool		nulls[PG_GET_AIOS_COLS] = {0};
		ProcNumber	owner;
		PGPROC	   *owner_proc;
		int32		owner_pid;
		PgAioHandleState start_state;
		uint64		start_generation;
		PgAioHandle ioh_copy;
		struct iovec iov_copy[PG_IOV_MAX];


		/*
		 * There is no lock that could prevent the state of the IO to advance
		 * concurrently - and we don't want to introduce one, as that would
		 * introduce atomics into a very common path. Instead we
		 *
		 * 1) Determine the state + generation of the IO.
		 *
		 * 2) Copy the IO to local memory.
		 *
		 * 3) Check if state or generation of the IO changed. If the state
		 * changed, retry, if the generation changed don't display the IO.
		 */

		/* 1) from above */
		start_generation = live_ioh->generation;

		/*
		 * Retry at this point, so we can accept changing states, but not
		 * changing generations.
		 */
retry:
		pg_read_barrier();
		start_state = live_ioh->state;

		if (start_state == PGAIO_HS_IDLE)
			continue;

		/* 2) from above */
		memcpy(&ioh_copy, live_ioh, sizeof(PgAioHandle));

		/*
		 * Safe to copy even if no iovec is used - we always reserve the
		 * required space.
		 */
		memcpy(&iov_copy, &pgaio_ctl->iovecs[ioh_copy.iovec_off],
			   PG_IOV_MAX * sizeof(struct iovec));

		/*
		 * Copy information about owner before 3) below, if the process exited
		 * it'd have to wait for the IO to finish first, which we would detect
		 * in 3).
		 */
		owner = ioh_copy.owner_procno;
		owner_proc = GetPGProcByNumber(owner);
		owner_pid = owner_proc->pid;

		/* 3) from above */
		pg_read_barrier();

		/*
		 * The IO completed and a new one was started with the same ID. Don't
		 * display it - it really started after this function was called.
		 * There be a risk of a livelock if we just retried endlessly, if IOs
		 * complete very quickly.
		 */
		if (live_ioh->generation != start_generation)
			continue;

		/*
		 * The IO's state changed while we were "rendering" it. Just start
		 * from scratch. There's no risk of a livelock here, as an IO has a
		 * limited sets of states it can be in, and state changes go only in a
		 * single direction.
		 */
		if (live_ioh->state != start_state)
			goto retry;

		/*
		 * Now that we have copied the IO into local memory and checked that
		 * it's still in the same state, we are not allowed to access "live"
		 * memory anymore. To make it slightly easier to catch such cases, set
		 * the "live" pointers to NULL.
		 */
		live_ioh = NULL;
		owner_proc = NULL;


		/* column: owning pid */
		if (owner_pid != 0)
			values[0] = Int32GetDatum(owner_pid);
		else
			nulls[0] = false;

		/* column: IO's id */
		values[1] = ioh_id;

		/* column: IO's generation */
		values[2] = Int64GetDatum(start_generation);

		/* column: IO's state */
		values[3] = CStringGetTextDatum(pgaio_io_get_state_name(&ioh_copy));

		/*
		 * If the IO is in PGAIO_HS_HANDED_OUT state, none of the following
		 * fields are valid yet (or are in the process of being set).
		 * Therefore we don't want to display any other columns.
		 */
		if (start_state == PGAIO_HS_HANDED_OUT)
		{
			memset(nulls + 4, 1, (lengthof(nulls) - 4) * sizeof(bool));
			goto display;
		}

		/* column: IO's operation */
		values[4] = CStringGetTextDatum(pgaio_io_get_op_name(&ioh_copy));

		/* columns: details about the IO's operation (offset, length) */
		switch (ioh_copy.op)
		{
			case PGAIO_OP_INVALID:
				nulls[5] = true;
				nulls[6] = true;
				break;
			case PGAIO_OP_READV:
				values[5] = Int64GetDatum(ioh_copy.op_data.read.offset);
				values[6] =
					Int64GetDatum(iov_byte_length(iov_copy, ioh_copy.op_data.read.iov_length));
				break;
			case PGAIO_OP_WRITEV:
				values[5] = Int64GetDatum(ioh_copy.op_data.write.offset);
				values[6] =
					Int64GetDatum(iov_byte_length(iov_copy, ioh_copy.op_data.write.iov_length));
				break;
		}

		/* column: IO's target */
		values[7] = CStringGetTextDatum(pgaio_io_get_target_name(&ioh_copy));

		/* column: length of IO's data array */
		values[8] = Int16GetDatum(ioh_copy.handle_data_len);

		/* column: raw result (i.e. some form of syscall return value) */
		if (start_state == PGAIO_HS_COMPLETED_IO
			|| start_state == PGAIO_HS_COMPLETED_SHARED
			|| start_state == PGAIO_HS_COMPLETED_LOCAL)
			values[9] = Int32GetDatum(ioh_copy.result);
		else
			nulls[9] = true;

		/*
		 * column: result in the higher level representation (unknown if not
		 * finished)
		 */
		values[10] =
			CStringGetTextDatum(pgaio_result_status_string(ioh_copy.distilled_result.status));

		/* column: target description */
		values[11] = CStringGetTextDatum(pgaio_io_get_target_description(&ioh_copy));

		/* columns: one for each flag */
		values[12] = BoolGetDatum(ioh_copy.flags & PGAIO_HF_SYNCHRONOUS);
		values[13] = BoolGetDatum(ioh_copy.flags & PGAIO_HF_REFERENCES_LOCAL);
		values[14] = BoolGetDatum(ioh_copy.flags & PGAIO_HF_BUFFERED);

display:
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}
