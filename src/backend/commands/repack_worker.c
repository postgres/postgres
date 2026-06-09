/*-------------------------------------------------------------------------
 *
 * repack_worker.c
 *    Implementation of the background worker for ad-hoc logical decoding
 *    during REPACK (CONCURRENTLY).
 *
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/repack_worker.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xlog_internal.h"
#include "access/xlogutils.h"
#include "access/xlogwait.h"
#include "commands/repack.h"
#include "commands/repack_internal.h"
#include "libpq/pqmq.h"
#include "replication/snapbuild.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/memutils.h"

#define PGREPACK_PLUGIN   "pgrepack"

static void RepackWorkerShutdown(int code, Datum arg);
static LogicalDecodingContext *repack_setup_logical_decoding(Oid relid);
static void repack_cleanup_logical_decoding(LogicalDecodingContext *ctx);
static void export_initial_snapshot(Snapshot snapshot,
									DecodingWorkerShared *shared);
static bool decode_concurrent_changes(LogicalDecodingContext *ctx,
									  DecodingWorkerShared *shared);

/* Is this process a REPACK worker? */
static bool am_repack_worker = false;

/* The WAL segment being decoded. */
static XLogSegNo repack_current_segment = 0;

/* Our DSM segment, for shutting down */
static dsm_segment *worker_dsm_segment = NULL;

/*
 * Keep track of the table we're processing, to skip logical decoding of data
 * from other relations.
 */
static RelFileLocator repacked_rel_locator = {.relNumber = InvalidOid};
static RelFileLocator repacked_rel_toast_locator = {.relNumber = InvalidOid};


/* REPACK decoding worker entry point */
void
RepackWorkerMain(Datum main_arg)
{
	dsm_segment *seg;
	DecodingWorkerShared *shared;
	shm_mq	   *mq;
	shm_mq_handle *mqh;
	LogicalDecodingContext *decoding_ctx;
	SharedFileSet *sfs;
	Snapshot	snapshot;

	am_repack_worker = true;

	BackgroundWorkerUnblockSignals();

	seg = dsm_attach(DatumGetUInt32(main_arg));
	if (seg == NULL)
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("could not map dynamic shared memory segment"));
	worker_dsm_segment = seg;

	shared = (DecodingWorkerShared *) dsm_segment_address(seg);

	/* Arrange to signal the leader if we exit. */
	before_shmem_exit(RepackWorkerShutdown, PointerGetDatum(shared));

	/*
	 * Join locking group - see the comments around the call of
	 * start_repack_decoding_worker().
	 */
	if (!BecomeLockGroupMember(shared->backend_proc, shared->backend_pid))
		return;					/* The leader is not running anymore. */

	/*
	 * Setup a queue to send error messages to the backend that launched this
	 * worker.
	 */
	mq = (shm_mq *) (char *) BUFFERALIGN(shared->error_queue);
	shm_mq_set_sender(mq, MyProc);
	mqh = shm_mq_attach(mq, seg, NULL);
	pq_redirect_to_shm_mq(seg, mqh);
	pq_set_parallel_leader(shared->backend_pid,
						   shared->backend_proc_number);

	/* Connect to the database. LOGIN is not required. */
	BackgroundWorkerInitializeConnectionByOid(shared->dbid, shared->roleid,
											  BGWORKER_BYPASS_ROLELOGINCHECK);

	/*
	 * Transaction is needed to open relation, and it also provides us with a
	 * resource owner.
	 */
	StartTransactionCommand();

	shared = (DecodingWorkerShared *) dsm_segment_address(seg);

	/*
	 * Not sure the spinlock is needed here - the backend should not change
	 * anything in the shared memory until we have serialized the snapshot.
	 */
	SpinLockAcquire(&shared->mutex);
	Assert(!XLogRecPtrIsValid(shared->lsn_upto));
	sfs = &shared->sfs;
	SpinLockRelease(&shared->mutex);

	SharedFileSetAttach(sfs, seg);

	/*
	 * Prepare to capture the concurrent data changes ourselves.
	 */
	decoding_ctx = repack_setup_logical_decoding(shared->relid);

	/* Announce that we're ready. */
	SpinLockAcquire(&shared->mutex);
	shared->initialized = true;
	SpinLockRelease(&shared->mutex);
	ConditionVariableSignal(&shared->cv);

	/* There doesn't seem to a nice API to set these */
	XactIsoLevel = XACT_REPEATABLE_READ;
	XactReadOnly = true;

	/* Build the initial snapshot and export it. */
	snapshot = SnapBuildInitialSnapshot(decoding_ctx->snapshot_builder);
	export_initial_snapshot(snapshot, shared);

	/*
	 * Only historic snapshots should be used now. Do not let us restrict the
	 * progress of xmin horizon.
	 */
	InvalidateCatalogSnapshot();

	for (;;)
	{
		bool		stop = decode_concurrent_changes(decoding_ctx, shared);

		if (stop)
			break;

	}

	/* Cleanup. */
	repack_cleanup_logical_decoding(decoding_ctx);
	CommitTransactionCommand();
}

/*
 * See ParallelWorkerShutdown for details.
 */
static void
RepackWorkerShutdown(int code, Datum arg)
{
	DecodingWorkerShared *shared = (DecodingWorkerShared *) DatumGetPointer(arg);

	SendProcSignal(shared->backend_pid,
				   PROCSIG_REPACK_MESSAGE,
				   shared->backend_proc_number);

	dsm_detach(worker_dsm_segment);
}

bool
AmRepackWorker(void)
{
	return am_repack_worker;
}

/*
 * This function is much like pg_create_logical_replication_slot() except that
 * the new slot is neither released (if anyone else could read changes from
 * our slot, we could miss changes other backends do while we copy the
 * existing data into temporary table), nor persisted (it's easier to handle
 * crash by restarting all the work from scratch).
 */
static LogicalDecodingContext *
repack_setup_logical_decoding(Oid relid)
{
	Relation	rel;
	Oid			toastrelid;
	LogicalDecodingContext *ctx;
	char		slotname[NAMEDATALEN];
	RepackDecodingState *dstate;
	MemoryContext oldcxt;

	/*
	 * REPACK CONCURRENTLY is not allowed in a transaction block, so this
	 * should never fire.
	 */
	Assert(!TransactionIdIsValid(GetTopTransactionIdIfAny()));

	/* Make sure we can use logical decoding */
	CheckLogicalDecodingRequirements(true);

	/*
	 * Create the replication slot we'll use, and enable logical decoding in
	 * case it isn't already on.
	 *
	 * Make the slot RS_TEMPORARY so that it's removed on ERROR.  A backend
	 * cannot execute multiple REPACK commands at a time, so the PID is enough
	 * to make the slot name unique.
	 */
	snprintf(slotname, NAMEDATALEN, "pg_repack_%d", MyProcPid);
	ReplicationSlotCreate(slotname, true, RS_TEMPORARY, false, true,
						  false, false);
	EnsureLogicalDecodingEnabled();

	/*
	 * Set up repacked_rel_locator and repacked_rel_toast_locator, which we
	 * use to skip decoding of unrelated relations.
	 */
	rel = table_open(relid, AccessShareLock);
	repacked_rel_locator = rel->rd_locator;
	toastrelid = rel->rd_rel->reltoastrelid;
	if (OidIsValid(toastrelid))
	{
		Relation	toastrel;

		/* Avoid logical decoding of other TOAST relations. */
		toastrel = table_open(toastrelid, AccessShareLock);
		repacked_rel_toast_locator = toastrel->rd_locator;
		table_close(toastrel, AccessShareLock);
	}
	table_close(rel, AccessShareLock);

	/*
	 * Set up our logical decoding context.  We initially use the blocking
	 * read_local_xlog_page until we find the start point, and switch to the
	 * non-blocking interface afterwards.
	 */
	ctx = CreateInitDecodingContext(PGREPACK_PLUGIN,
									NIL,
									true,
									true,
									InvalidXLogRecPtr,
									XL_ROUTINE(.page_read = read_local_xlog_page,
											   .segment_open = wal_segment_open,
											   .segment_close = wal_segment_close),
									NULL, NULL, NULL);

	/* Complete setup of output_writer_private */
	dstate = (RepackDecodingState *) ctx->output_writer_private;
	dstate->relid = relid;
	dstate->worker_cxt = CurrentMemoryContext;
	dstate->worker_resowner = CurrentResourceOwner;

	/* We don't have control on fast_forward, but verify it's sane */
	Assert(!ctx->fast_forward);

	/* Find our decoding starting point. */
	DecodingContextFindStartpoint(ctx);

	/* From this point on, we need non-blocking WAL reads */
	ctx->reader->routine.page_read = read_local_xlog_page_no_wait;

	/*
	 * Initialize repack_current_segment so that we can notice WAL segment
	 * boundaries.
	 */
	XLByteToSeg(ctx->reader->EndRecPtr, repack_current_segment,
				wal_segment_size);

	/*
	 * Set up our reader private state to let the page-read callback notify
	 * when end-of-WAL has been reached.  This lives in the same context as
	 * the logical decoding itself.
	 */
	oldcxt = MemoryContextSwitchTo(ctx->context);
	ctx->reader->private_data = palloc0_object(ReadLocalXLogPageNoWaitPrivate);
	MemoryContextSwitchTo(oldcxt);

	return ctx;
}

static void
repack_cleanup_logical_decoding(LogicalDecodingContext *ctx)
{
	RepackDecodingState *dstate;

	dstate = (RepackDecodingState *) ctx->output_writer_private;
	if (dstate->slot)
		ExecDropSingleTupleTableSlot(dstate->slot);

	FreeDecodingContext(ctx);
	ReplicationSlotDropAcquired(true);
}

/*
 * Make snapshot available to the backend that launched the decoding worker.
 */
static void
export_initial_snapshot(Snapshot snapshot, DecodingWorkerShared *shared)
{
	char		fname[MAXPGPATH];
	BufFile    *file;
	Size		snap_size;
	char	   *snap_space;

	snap_size = EstimateSnapshotSpace(snapshot);
	snap_space = (char *) palloc(snap_size);
	SerializeSnapshot(snapshot, snap_space);

	DecodingWorkerFileName(fname, shared->relid, shared->last_exported + 1);
	file = BufFileCreateFileSet(&shared->sfs.fs, fname);
	/* To make restoration easier, write the snapshot size first. */
	BufFileWrite(file, &snap_size, sizeof(snap_size));
	BufFileWrite(file, snap_space, snap_size);
	BufFileClose(file);
	pfree(snap_space);

	/* Increase the counter to tell the backend that the file is available. */
	SpinLockAcquire(&shared->mutex);
	shared->last_exported++;
	SpinLockRelease(&shared->mutex);
	ConditionVariableSignal(&shared->cv);
}

/*
 * Decode logical changes from the WAL sequence and store them to a file.
 *
 * If true is returned, there is no more work for the worker.
 */
static bool
decode_concurrent_changes(LogicalDecodingContext *ctx,
						  DecodingWorkerShared *shared)
{
	RepackDecodingState *dstate;
	XLogRecPtr	lsn_upto;
	bool		done;
	char		fname[MAXPGPATH];

	dstate = (RepackDecodingState *) ctx->output_writer_private;

	/* Open the output file. */
	DecodingWorkerFileName(fname, shared->relid, shared->last_exported + 1);
	dstate->file = BufFileCreateFileSet(&shared->sfs.fs, fname);

	SpinLockAcquire(&shared->mutex);
	lsn_upto = shared->lsn_upto;
	done = shared->done;
	SpinLockRelease(&shared->mutex);

	while (true)
	{
		XLogRecord *record;
		XLogSegNo	segno_new;
		char	   *errm = NULL;
		XLogRecPtr	end_lsn;

		CHECK_FOR_INTERRUPTS();

		record = XLogReadRecord(ctx->reader, &errm);
		if (record)
		{
			LogicalDecodingProcessRecord(ctx, ctx->reader);

			/*
			 * We want to allow WAL to be recycled while REPACK is running.
			 *
			 * In normal usage of a replication slot, we need to be very
			 * careful not to advance the LSN until it's been confirmed as
			 * received by the remote.  In REPACK's case, this is not needed:
			 * REPACK will never try to replay the same WAL after a crash, and
			 * if there _is_ a crash, the whole REPACK has to be started from
			 * scratch anyway.
			 *
			 * So here we disregard the careful LSN tracking and just move the
			 * LSN locations forward to what we've processed.  Note that it
			 * would be bogus to move the xmin forward, though, so we don't
			 * touch that.
			 *
			 * This can be done on whatever schedule is convenient, but in
			 * order not to cause unnecessary load, we only do it as we cross
			 * each WAL segment boundary.
			 */
			end_lsn = ctx->reader->EndRecPtr;
			XLByteToSeg(end_lsn, segno_new, wal_segment_size);
			if (segno_new != repack_current_segment)
			{
				LogicalIncreaseRestartDecodingForSlot(end_lsn, end_lsn);
				LogicalConfirmReceivedLocation(end_lsn);
				elog(DEBUG1, "REPACK: confirmed receive location %X/%X",
					 (uint32) (end_lsn >> 32), (uint32) end_lsn);
				repack_current_segment = segno_new;
			}
		}
		else
		{
			ReadLocalXLogPageNoWaitPrivate *priv;

			if (errm)
				ereport(ERROR,
						errcode_for_file_access(),
						errmsg("could not read WAL from timeline %u at %X/%08X: %s",
							   ctx->reader->currTLI,
							   LSN_FORMAT_ARGS(ctx->reader->EndRecPtr),
							   errm));

			/*
			 * In the decoding loop we do not want to get blocked when there
			 * is no more WAL available, otherwise the loop would become
			 * uninterruptible.
			 */
			priv = (ReadLocalXLogPageNoWaitPrivate *) ctx->reader->private_data;
			if (priv->end_of_wal)
				/* Do not miss the end of WAL condition next time. */
				priv->end_of_wal = false;
			else
				ereport(ERROR,
						errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("could not read WAL record"));
		}

		/*
		 * Whether we could read new record or not, keep checking if
		 * 'lsn_upto' was specified.
		 */
		if (!XLogRecPtrIsValid(lsn_upto))
		{
			SpinLockAcquire(&shared->mutex);
			lsn_upto = shared->lsn_upto;
			/* 'done' should be set at the same time as 'lsn_upto' */
			done = shared->done;
			SpinLockRelease(&shared->mutex);
		}
		if (XLogRecPtrIsValid(lsn_upto) &&
			ctx->reader->EndRecPtr >= lsn_upto)
			break;

		if (record == NULL)
		{
			int64		timeout = 0;
			WaitLSNResult res;

			/*
			 * Before we retry reading, wait until new WAL is flushed.
			 *
			 * There is a race condition such that the backend executing
			 * REPACK determines 'lsn_upto', but before it sets the shared
			 * variable, we reach the end of WAL. In that case we'd need to
			 * wait until the next WAL flush (unrelated to REPACK). Although
			 * that should not be a problem in a busy system, it might be
			 * noticeable in other cases, including regression tests (which
			 * are not necessarily executed in parallel). Therefore it makes
			 * sense to use timeout.
			 *
			 * If lsn_upto is valid, WAL records having LSN lower than that
			 * should already have been flushed to disk.
			 */
			if (!XLogRecPtrIsValid(lsn_upto))
				timeout = 100L;
			res = WaitForLSN(WAIT_LSN_TYPE_PRIMARY_FLUSH,
							 ctx->reader->EndRecPtr + 1,
							 timeout);
			if (res != WAIT_LSN_RESULT_SUCCESS &&
				res != WAIT_LSN_RESULT_TIMEOUT)
				ereport(ERROR,
						errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("waiting for WAL failed"));
		}
	}

	/*
	 * Close the file so we can make it available to the backend.
	 */
	BufFileClose(dstate->file);
	dstate->file = NULL;
	SpinLockAcquire(&shared->mutex);
	shared->lsn_upto = InvalidXLogRecPtr;
	shared->last_exported++;
	SpinLockRelease(&shared->mutex);
	ConditionVariableSignal(&shared->cv);

	return done;
}

/*
 * Does the WAL record contain a data change that this backend does not need
 * to decode on behalf of REPACK (CONCURRENTLY)?
 */
bool
change_useless_for_repack(XLogRecordBuffer *buf)
{
	XLogReaderState *r = buf->record;
	RelFileLocator locator;

	/* TOAST locator should not be set unless the main is. */
	Assert(!OidIsValid(repacked_rel_toast_locator.relNumber) ||
		   OidIsValid(repacked_rel_locator.relNumber));

	/*
	 * Backends not involved in REPACK (CONCURRENTLY) should not do the
	 * filtering.
	 */
	if (!OidIsValid(repacked_rel_locator.relNumber))
		return false;

	/*
	 * If the record does not contain the block 0, it's probably not INSERT /
	 * UPDATE / DELETE. In any case, we do not have enough information to
	 * filter the change out.
	 */
	if (!XLogRecGetBlockTagExtended(r, 0, &locator, NULL, NULL, NULL))
		return false;

	/*
	 * Decode the change if it belongs to the table we are repacking, or if it
	 * belongs to its TOAST relation.
	 */
	if (RelFileLocatorEquals(locator, repacked_rel_locator))
		return false;
	if (OidIsValid(repacked_rel_toast_locator.relNumber) &&
		RelFileLocatorEquals(locator, repacked_rel_toast_locator))
		return false;

	/* Filter out changes of other tables. */
	return true;
}
