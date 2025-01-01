/*-------------------------------------------------------------------------
 *
 * autoprewarm.c
 *		Periodically dump information about the blocks present in
 *		shared_buffers, and reload them on server restart.
 *
 *		Due to locking considerations, we can't actually begin prewarming
 *		until the server reaches a consistent state.  We need the catalogs
 *		to be consistent so that we can figure out which relation to lock,
 *		and we need to lock the relations so that we don't try to prewarm
 *		pages from a relation that is in the process of being dropped.
 *
 *		While prewarming, autoprewarm will use two workers.  There's a
 *		leader worker that reads and sorts the list of blocks to be
 *		prewarmed and then launches a per-database worker for each
 *		relevant database in turn.  The former keeps running after the
 *		initial prewarm is complete to update the dump file periodically.
 *
 *	Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 *	IDENTIFICATION
 *		contrib/pg_prewarm/autoprewarm.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "access/relation.h"
#include "access/xact.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/buf_internals.h"
#include "storage/dsm.h"
#include "storage/dsm_registry.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/procsignal.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/relfilenumbermap.h"
#include "utils/timestamp.h"

#define AUTOPREWARM_FILE "autoprewarm.blocks"

/* Metadata for each block we dump. */
typedef struct BlockInfoRecord
{
	Oid			database;
	Oid			tablespace;
	RelFileNumber filenumber;
	ForkNumber	forknum;
	BlockNumber blocknum;
} BlockInfoRecord;

/* Shared state information for autoprewarm bgworker. */
typedef struct AutoPrewarmSharedState
{
	LWLock		lock;			/* mutual exclusion */
	pid_t		bgworker_pid;	/* for main bgworker */
	pid_t		pid_using_dumpfile; /* for autoprewarm or block dump */

	/* Following items are for communication with per-database worker */
	dsm_handle	block_info_handle;
	Oid			database;
	int			prewarm_start_idx;
	int			prewarm_stop_idx;
	int			prewarmed_blocks;
} AutoPrewarmSharedState;

PGDLLEXPORT void autoprewarm_main(Datum main_arg);
PGDLLEXPORT void autoprewarm_database_main(Datum main_arg);

PG_FUNCTION_INFO_V1(autoprewarm_start_worker);
PG_FUNCTION_INFO_V1(autoprewarm_dump_now);

static void apw_load_buffers(void);
static int	apw_dump_now(bool is_bgworker, bool dump_unlogged);
static void apw_start_leader_worker(void);
static void apw_start_database_worker(void);
static bool apw_init_shmem(void);
static void apw_detach_shmem(int code, Datum arg);
static int	apw_compare_blockinfo(const void *p, const void *q);

/* Pointer to shared-memory state. */
static AutoPrewarmSharedState *apw_state = NULL;

/* GUC variables. */
static bool autoprewarm = true; /* start worker? */
static int	autoprewarm_interval = 300; /* dump interval */

/*
 * Module load callback.
 */
void
_PG_init(void)
{
	DefineCustomIntVariable("pg_prewarm.autoprewarm_interval",
							"Sets the interval between dumps of shared buffers",
							"If set to zero, time-based dumping is disabled.",
							&autoprewarm_interval,
							300,
							0, INT_MAX / 1000,
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL,
							NULL,
							NULL);

	if (!process_shared_preload_libraries_in_progress)
		return;

	/* can't define PGC_POSTMASTER variable after startup */
	DefineCustomBoolVariable("pg_prewarm.autoprewarm",
							 "Starts the autoprewarm worker.",
							 NULL,
							 &autoprewarm,
							 true,
							 PGC_POSTMASTER,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("pg_prewarm");

	/* Register autoprewarm worker, if enabled. */
	if (autoprewarm)
		apw_start_leader_worker();
}

/*
 * Main entry point for the leader autoprewarm process.  Per-database workers
 * have a separate entry point.
 */
void
autoprewarm_main(Datum main_arg)
{
	bool		first_time = true;
	bool		final_dump_allowed = true;
	TimestampTz last_dump_time = 0;

	/* Establish signal handlers; once that's done, unblock signals. */
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	BackgroundWorkerUnblockSignals();

	/* Create (if necessary) and attach to our shared memory area. */
	if (apw_init_shmem())
		first_time = false;

	/*
	 * Set on-detach hook so that our PID will be cleared on exit.
	 *
	 * NB: Autoprewarm's state is stored in a DSM segment, and DSM segments
	 * are detached before calling the on_shmem_exit callbacks, so we must put
	 * apw_detach_shmem in the before_shmem_exit callback list.
	 */
	before_shmem_exit(apw_detach_shmem, 0);

	/*
	 * Store our PID in the shared memory area --- unless there's already
	 * another worker running, in which case just exit.
	 */
	LWLockAcquire(&apw_state->lock, LW_EXCLUSIVE);
	if (apw_state->bgworker_pid != InvalidPid)
	{
		LWLockRelease(&apw_state->lock);
		ereport(LOG,
				(errmsg("autoprewarm worker is already running under PID %d",
						(int) apw_state->bgworker_pid)));
		return;
	}
	apw_state->bgworker_pid = MyProcPid;
	LWLockRelease(&apw_state->lock);

	/*
	 * Preload buffers from the dump file only if we just created the shared
	 * memory region.  Otherwise, it's either already been done or shouldn't
	 * be done - e.g. because the old dump file has been overwritten since the
	 * server was started.
	 *
	 * There's not much point in performing a dump immediately after we finish
	 * preloading; so, if we do end up preloading, consider the last dump time
	 * to be equal to the current time.
	 *
	 * If apw_load_buffers() is terminated early by a shutdown request,
	 * prevent dumping out our state below the loop, because we'd effectively
	 * just truncate the saved state to however much we'd managed to preload.
	 */
	if (first_time)
	{
		apw_load_buffers();
		final_dump_allowed = !ShutdownRequestPending;
		last_dump_time = GetCurrentTimestamp();
	}

	/* Periodically dump buffers until terminated. */
	while (!ShutdownRequestPending)
	{
		/* In case of a SIGHUP, just reload the configuration. */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (autoprewarm_interval <= 0)
		{
			/* We're only dumping at shutdown, so just wait forever. */
			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_EXIT_ON_PM_DEATH,
							 -1L,
							 PG_WAIT_EXTENSION);
		}
		else
		{
			TimestampTz next_dump_time;
			long		delay_in_ms;

			/* Compute the next dump time. */
			next_dump_time =
				TimestampTzPlusMilliseconds(last_dump_time,
											autoprewarm_interval * 1000);
			delay_in_ms =
				TimestampDifferenceMilliseconds(GetCurrentTimestamp(),
												next_dump_time);

			/* Perform a dump if it's time. */
			if (delay_in_ms <= 0)
			{
				last_dump_time = GetCurrentTimestamp();
				apw_dump_now(true, false);
				continue;
			}

			/* Sleep until the next dump time. */
			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 delay_in_ms,
							 PG_WAIT_EXTENSION);
		}

		/* Reset the latch, loop. */
		ResetLatch(MyLatch);
	}

	/*
	 * Dump one last time.  We assume this is probably the result of a system
	 * shutdown, although it's possible that we've merely been terminated.
	 */
	if (final_dump_allowed)
		apw_dump_now(true, true);
}

/*
 * Read the dump file and launch per-database workers one at a time to
 * prewarm the buffers found there.
 */
static void
apw_load_buffers(void)
{
	FILE	   *file = NULL;
	int			num_elements,
				i;
	BlockInfoRecord *blkinfo;
	dsm_segment *seg;

	/*
	 * Skip the prewarm if the dump file is in use; otherwise, prevent any
	 * other process from writing it while we're using it.
	 */
	LWLockAcquire(&apw_state->lock, LW_EXCLUSIVE);
	if (apw_state->pid_using_dumpfile == InvalidPid)
		apw_state->pid_using_dumpfile = MyProcPid;
	else
	{
		LWLockRelease(&apw_state->lock);
		ereport(LOG,
				(errmsg("skipping prewarm because block dump file is being written by PID %d",
						(int) apw_state->pid_using_dumpfile)));
		return;
	}
	LWLockRelease(&apw_state->lock);

	/*
	 * Open the block dump file.  Exit quietly if it doesn't exist, but report
	 * any other error.
	 */
	file = AllocateFile(AUTOPREWARM_FILE, "r");
	if (!file)
	{
		if (errno == ENOENT)
		{
			LWLockAcquire(&apw_state->lock, LW_EXCLUSIVE);
			apw_state->pid_using_dumpfile = InvalidPid;
			LWLockRelease(&apw_state->lock);
			return;				/* No file to load. */
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m",
						AUTOPREWARM_FILE)));
	}

	/* First line of the file is a record count. */
	if (fscanf(file, "<<%d>>\n", &num_elements) != 1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from file \"%s\": %m",
						AUTOPREWARM_FILE)));

	/* Allocate a dynamic shared memory segment to store the record data. */
	seg = dsm_create(sizeof(BlockInfoRecord) * num_elements, 0);
	blkinfo = (BlockInfoRecord *) dsm_segment_address(seg);

	/* Read records, one per line. */
	for (i = 0; i < num_elements; i++)
	{
		unsigned	forknum;

		if (fscanf(file, "%u,%u,%u,%u,%u\n", &blkinfo[i].database,
				   &blkinfo[i].tablespace, &blkinfo[i].filenumber,
				   &forknum, &blkinfo[i].blocknum) != 5)
			ereport(ERROR,
					(errmsg("autoprewarm block dump file is corrupted at line %d",
							i + 1)));
		blkinfo[i].forknum = forknum;
	}

	FreeFile(file);

	/* Sort the blocks to be loaded. */
	qsort(blkinfo, num_elements, sizeof(BlockInfoRecord),
		  apw_compare_blockinfo);

	/* Populate shared memory state. */
	apw_state->block_info_handle = dsm_segment_handle(seg);
	apw_state->prewarm_start_idx = apw_state->prewarm_stop_idx = 0;
	apw_state->prewarmed_blocks = 0;

	/* Get the info position of the first block of the next database. */
	while (apw_state->prewarm_start_idx < num_elements)
	{
		int			j = apw_state->prewarm_start_idx;
		Oid			current_db = blkinfo[j].database;

		/*
		 * Advance the prewarm_stop_idx to the first BlockInfoRecord that does
		 * not belong to this database.
		 */
		j++;
		while (j < num_elements)
		{
			if (current_db != blkinfo[j].database)
			{
				/*
				 * Combine BlockInfoRecords for global objects with those of
				 * the database.
				 */
				if (current_db != InvalidOid)
					break;
				current_db = blkinfo[j].database;
			}

			j++;
		}

		/*
		 * If we reach this point with current_db == InvalidOid, then only
		 * BlockInfoRecords belonging to global objects exist.  We can't
		 * prewarm without a database connection, so just bail out.
		 */
		if (current_db == InvalidOid)
			break;

		/* Configure stop point and database for next per-database worker. */
		apw_state->prewarm_stop_idx = j;
		apw_state->database = current_db;
		Assert(apw_state->prewarm_start_idx < apw_state->prewarm_stop_idx);

		/* If we've run out of free buffers, don't launch another worker. */
		if (!have_free_buffer())
			break;

		/*
		 * Likewise, don't launch if we've already been told to shut down.
		 * (The launch would fail anyway, but we might as well skip it.)
		 */
		if (ShutdownRequestPending)
			break;

		/*
		 * Start a per-database worker to load blocks for this database; this
		 * function will return once the per-database worker exits.
		 */
		apw_start_database_worker();

		/* Prepare for next database. */
		apw_state->prewarm_start_idx = apw_state->prewarm_stop_idx;
	}

	/* Clean up. */
	dsm_detach(seg);
	LWLockAcquire(&apw_state->lock, LW_EXCLUSIVE);
	apw_state->block_info_handle = DSM_HANDLE_INVALID;
	apw_state->pid_using_dumpfile = InvalidPid;
	LWLockRelease(&apw_state->lock);

	/* Report our success, if we were able to finish. */
	if (!ShutdownRequestPending)
		ereport(LOG,
				(errmsg("autoprewarm successfully prewarmed %d of %d previously-loaded blocks",
						apw_state->prewarmed_blocks, num_elements)));
}

/*
 * Prewarm all blocks for one database (and possibly also global objects, if
 * those got grouped with this database).
 */
void
autoprewarm_database_main(Datum main_arg)
{
	int			pos;
	BlockInfoRecord *block_info;
	Relation	rel = NULL;
	BlockNumber nblocks = 0;
	BlockInfoRecord *old_blk = NULL;
	dsm_segment *seg;

	/* Establish signal handlers; once that's done, unblock signals. */
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/* Connect to correct database and get block information. */
	apw_init_shmem();
	seg = dsm_attach(apw_state->block_info_handle);
	if (seg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not map dynamic shared memory segment")));
	BackgroundWorkerInitializeConnectionByOid(apw_state->database, InvalidOid, 0);
	block_info = (BlockInfoRecord *) dsm_segment_address(seg);
	pos = apw_state->prewarm_start_idx;

	/*
	 * Loop until we run out of blocks to prewarm or until we run out of free
	 * buffers.
	 */
	while (pos < apw_state->prewarm_stop_idx && have_free_buffer())
	{
		BlockInfoRecord *blk = &block_info[pos++];
		Buffer		buf;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Quit if we've reached records for another database. If previous
		 * blocks are of some global objects, then continue pre-warming.
		 */
		if (old_blk != NULL && old_blk->database != blk->database &&
			old_blk->database != 0)
			break;

		/*
		 * As soon as we encounter a block of a new relation, close the old
		 * relation. Note that rel will be NULL if try_relation_open failed
		 * previously; in that case, there is nothing to close.
		 */
		if (old_blk != NULL && old_blk->filenumber != blk->filenumber &&
			rel != NULL)
		{
			relation_close(rel, AccessShareLock);
			rel = NULL;
			CommitTransactionCommand();
		}

		/*
		 * Try to open each new relation, but only once, when we first
		 * encounter it. If it's been dropped, skip the associated blocks.
		 */
		if (old_blk == NULL || old_blk->filenumber != blk->filenumber)
		{
			Oid			reloid;

			Assert(rel == NULL);
			StartTransactionCommand();
			reloid = RelidByRelfilenumber(blk->tablespace, blk->filenumber);
			if (OidIsValid(reloid))
				rel = try_relation_open(reloid, AccessShareLock);

			if (!rel)
				CommitTransactionCommand();
		}
		if (!rel)
		{
			old_blk = blk;
			continue;
		}

		/* Once per fork, check for fork existence and size. */
		if (old_blk == NULL ||
			old_blk->filenumber != blk->filenumber ||
			old_blk->forknum != blk->forknum)
		{
			/*
			 * smgrexists is not safe for illegal forknum, hence check whether
			 * the passed forknum is valid before using it in smgrexists.
			 */
			if (blk->forknum > InvalidForkNumber &&
				blk->forknum <= MAX_FORKNUM &&
				smgrexists(RelationGetSmgr(rel), blk->forknum))
				nblocks = RelationGetNumberOfBlocksInFork(rel, blk->forknum);
			else
				nblocks = 0;
		}

		/* Check whether blocknum is valid and within fork file size. */
		if (blk->blocknum >= nblocks)
		{
			/* Move to next forknum. */
			old_blk = blk;
			continue;
		}

		/* Prewarm buffer. */
		buf = ReadBufferExtended(rel, blk->forknum, blk->blocknum, RBM_NORMAL,
								 NULL);
		if (BufferIsValid(buf))
		{
			apw_state->prewarmed_blocks++;
			ReleaseBuffer(buf);
		}

		old_blk = blk;
	}

	dsm_detach(seg);

	/* Release lock on previous relation. */
	if (rel)
	{
		relation_close(rel, AccessShareLock);
		CommitTransactionCommand();
	}
}

/*
 * Dump information on blocks in shared buffers.  We use a text format here
 * so that it's easy to understand and even change the file contents if
 * necessary.
 * Returns the number of blocks dumped.
 */
static int
apw_dump_now(bool is_bgworker, bool dump_unlogged)
{
	int			num_blocks;
	int			i;
	int			ret;
	BlockInfoRecord *block_info_array;
	BufferDesc *bufHdr;
	FILE	   *file;
	char		transient_dump_file_path[MAXPGPATH];
	pid_t		pid;

	LWLockAcquire(&apw_state->lock, LW_EXCLUSIVE);
	pid = apw_state->pid_using_dumpfile;
	if (apw_state->pid_using_dumpfile == InvalidPid)
		apw_state->pid_using_dumpfile = MyProcPid;
	LWLockRelease(&apw_state->lock);

	if (pid != InvalidPid)
	{
		if (!is_bgworker)
			ereport(ERROR,
					(errmsg("could not perform block dump because dump file is being used by PID %d",
							(int) apw_state->pid_using_dumpfile)));

		ereport(LOG,
				(errmsg("skipping block dump because it is already being performed by PID %d",
						(int) apw_state->pid_using_dumpfile)));
		return 0;
	}

	block_info_array =
		(BlockInfoRecord *) palloc(sizeof(BlockInfoRecord) * NBuffers);

	for (num_blocks = 0, i = 0; i < NBuffers; i++)
	{
		uint32		buf_state;

		CHECK_FOR_INTERRUPTS();

		bufHdr = GetBufferDescriptor(i);

		/* Lock each buffer header before inspecting. */
		buf_state = LockBufHdr(bufHdr);

		/*
		 * Unlogged tables will be automatically truncated after a crash or
		 * unclean shutdown. In such cases we need not prewarm them. Dump them
		 * only if requested by caller.
		 */
		if (buf_state & BM_TAG_VALID &&
			((buf_state & BM_PERMANENT) || dump_unlogged))
		{
			block_info_array[num_blocks].database = bufHdr->tag.dbOid;
			block_info_array[num_blocks].tablespace = bufHdr->tag.spcOid;
			block_info_array[num_blocks].filenumber =
				BufTagGetRelNumber(&bufHdr->tag);
			block_info_array[num_blocks].forknum =
				BufTagGetForkNum(&bufHdr->tag);
			block_info_array[num_blocks].blocknum = bufHdr->tag.blockNum;
			++num_blocks;
		}

		UnlockBufHdr(bufHdr, buf_state);
	}

	snprintf(transient_dump_file_path, MAXPGPATH, "%s.tmp", AUTOPREWARM_FILE);
	file = AllocateFile(transient_dump_file_path, "w");
	if (!file)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						transient_dump_file_path)));

	ret = fprintf(file, "<<%d>>\n", num_blocks);
	if (ret < 0)
	{
		int			save_errno = errno;

		FreeFile(file);
		unlink(transient_dump_file_path);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						transient_dump_file_path)));
	}

	for (i = 0; i < num_blocks; i++)
	{
		CHECK_FOR_INTERRUPTS();

		ret = fprintf(file, "%u,%u,%u,%u,%u\n",
					  block_info_array[i].database,
					  block_info_array[i].tablespace,
					  block_info_array[i].filenumber,
					  (uint32) block_info_array[i].forknum,
					  block_info_array[i].blocknum);
		if (ret < 0)
		{
			int			save_errno = errno;

			FreeFile(file);
			unlink(transient_dump_file_path);
			errno = save_errno;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m",
							transient_dump_file_path)));
		}
	}

	pfree(block_info_array);

	/*
	 * Rename transient_dump_file_path to AUTOPREWARM_FILE to make things
	 * permanent.
	 */
	ret = FreeFile(file);
	if (ret != 0)
	{
		int			save_errno = errno;

		unlink(transient_dump_file_path);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						transient_dump_file_path)));
	}

	(void) durable_rename(transient_dump_file_path, AUTOPREWARM_FILE, ERROR);
	apw_state->pid_using_dumpfile = InvalidPid;

	ereport(DEBUG1,
			(errmsg_internal("wrote block details for %d blocks", num_blocks)));
	return num_blocks;
}

/*
 * SQL-callable function to launch autoprewarm.
 */
Datum
autoprewarm_start_worker(PG_FUNCTION_ARGS)
{
	pid_t		pid;

	if (!autoprewarm)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("autoprewarm is disabled")));

	apw_init_shmem();
	LWLockAcquire(&apw_state->lock, LW_EXCLUSIVE);
	pid = apw_state->bgworker_pid;
	LWLockRelease(&apw_state->lock);

	if (pid != InvalidPid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("autoprewarm worker is already running under PID %d",
						(int) pid)));

	apw_start_leader_worker();

	PG_RETURN_VOID();
}

/*
 * SQL-callable function to perform an immediate block dump.
 *
 * Note: this is declared to return int8, as insurance against some
 * very distant day when we might make NBuffers wider than int.
 */
Datum
autoprewarm_dump_now(PG_FUNCTION_ARGS)
{
	int			num_blocks;

	apw_init_shmem();

	PG_ENSURE_ERROR_CLEANUP(apw_detach_shmem, 0);
	{
		num_blocks = apw_dump_now(false, true);
	}
	PG_END_ENSURE_ERROR_CLEANUP(apw_detach_shmem, 0);

	PG_RETURN_INT64((int64) num_blocks);
}

static void
apw_init_state(void *ptr)
{
	AutoPrewarmSharedState *state = (AutoPrewarmSharedState *) ptr;

	LWLockInitialize(&state->lock, LWLockNewTrancheId());
	state->bgworker_pid = InvalidPid;
	state->pid_using_dumpfile = InvalidPid;
}

/*
 * Allocate and initialize autoprewarm related shared memory, if not already
 * done, and set up backend-local pointer to that state.  Returns true if an
 * existing shared memory segment was found.
 */
static bool
apw_init_shmem(void)
{
	bool		found;

	apw_state = GetNamedDSMSegment("autoprewarm",
								   sizeof(AutoPrewarmSharedState),
								   apw_init_state,
								   &found);
	LWLockRegisterTranche(apw_state->lock.tranche, "autoprewarm");

	return found;
}

/*
 * Clear our PID from autoprewarm shared state.
 */
static void
apw_detach_shmem(int code, Datum arg)
{
	LWLockAcquire(&apw_state->lock, LW_EXCLUSIVE);
	if (apw_state->pid_using_dumpfile == MyProcPid)
		apw_state->pid_using_dumpfile = InvalidPid;
	if (apw_state->bgworker_pid == MyProcPid)
		apw_state->bgworker_pid = InvalidPid;
	LWLockRelease(&apw_state->lock);
}

/*
 * Start autoprewarm leader worker process.
 */
static void
apw_start_leader_worker(void)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	BgwHandleStatus status;
	pid_t		pid;

	MemSet(&worker, 0, sizeof(BackgroundWorker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	strcpy(worker.bgw_library_name, "pg_prewarm");
	strcpy(worker.bgw_function_name, "autoprewarm_main");
	strcpy(worker.bgw_name, "autoprewarm leader");
	strcpy(worker.bgw_type, "autoprewarm leader");

	if (process_shared_preload_libraries_in_progress)
	{
		RegisterBackgroundWorker(&worker);
		return;
	}

	/* must set notify PID to wait for startup */
	worker.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not register background process"),
				 errhint("You may need to increase \"max_worker_processes\".")));

	status = WaitForBackgroundWorkerStartup(handle, &pid);
	if (status != BGWH_STARTED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not start background process"),
				 errhint("More details may be available in the server log.")));
}

/*
 * Start autoprewarm per-database worker process.
 */
static void
apw_start_database_worker(void)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;

	MemSet(&worker, 0, sizeof(BackgroundWorker));
	worker.bgw_flags =
		BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	strcpy(worker.bgw_library_name, "pg_prewarm");
	strcpy(worker.bgw_function_name, "autoprewarm_database_main");
	strcpy(worker.bgw_name, "autoprewarm worker");
	strcpy(worker.bgw_type, "autoprewarm worker");

	/* must set notify PID to wait for shutdown */
	worker.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("registering dynamic bgworker autoprewarm failed"),
				 errhint("Consider increasing the configuration parameter \"%s\".", "max_worker_processes")));

	/*
	 * Ignore return value; if it fails, postmaster has died, but we have
	 * checks for that elsewhere.
	 */
	WaitForBackgroundWorkerShutdown(handle);
}

/* Compare member elements to check whether they are not equal. */
#define cmp_member_elem(fld)	\
do { \
	if (a->fld < b->fld)		\
		return -1;				\
	else if (a->fld > b->fld)	\
		return 1;				\
} while(0)

/*
 * apw_compare_blockinfo
 *
 * We depend on all records for a particular database being consecutive
 * in the dump file; each per-database worker will preload blocks until
 * it sees a block for some other database.  Sorting by tablespace,
 * filenumber, forknum, and blocknum isn't critical for correctness, but
 * helps us get a sequential I/O pattern.
 */
static int
apw_compare_blockinfo(const void *p, const void *q)
{
	const BlockInfoRecord *a = (const BlockInfoRecord *) p;
	const BlockInfoRecord *b = (const BlockInfoRecord *) q;

	cmp_member_elem(database);
	cmp_member_elem(tablespace);
	cmp_member_elem(filenumber);
	cmp_member_elem(forknum);
	cmp_member_elem(blocknum);

	return 0;
}
