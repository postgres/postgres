/*-------------------------------------------------------------------------
 *
 * checksumhelper.c
 *	  Background worker to walk the database and write checksums to pages
 *
 * When enabling data checksums on a database at initdb time, no extra process
 * is required as each page is checksummed, and verified, at accesses.  When
 * enabling checksums on an already running cluster, which was not initialized
 * with checksums, this helper worker will ensure that all pages are
 * checksummed before verification of the checksums is turned on.
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/checksumhelper.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_database.h"
#include "commands/vacuum.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/bgwriter.h"
#include "postmaster/checksumhelper.h"
#include "storage/bufmgr.h"
#include "storage/checksum.h"
#include "storage/lmgr.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/ps_status.h"


typedef enum
{
	SUCCESSFUL = 0,
	ABORTED,
	FAILED
}			ChecksumHelperResult;

typedef struct ChecksumHelperShmemStruct
{
	pg_atomic_flag launcher_started;
	ChecksumHelperResult success;
	bool		process_shared_catalogs;
	bool		abort;
	/* Parameter values set on start */
	int			cost_delay;
	int			cost_limit;
}			ChecksumHelperShmemStruct;

/* Shared memory segment for checksumhelper */
static ChecksumHelperShmemStruct * ChecksumHelperShmem;

/* Bookkeeping for work to do */
typedef struct ChecksumHelperDatabase
{
	Oid			dboid;
	char	   *dbname;
}			ChecksumHelperDatabase;

typedef struct ChecksumHelperRelation
{
	Oid			reloid;
	char		relkind;
}			ChecksumHelperRelation;

/* Prototypes */
static List *BuildDatabaseList(void);
static List *BuildRelationList(bool include_shared);
static List *BuildTempTableList(void);
static ChecksumHelperResult ProcessDatabase(ChecksumHelperDatabase * db);
static void launcher_cancel_handler(SIGNAL_ARGS);

/*
 * Main entry point for checksumhelper launcher process.
 */
bool
StartChecksumHelperLauncher(int cost_delay, int cost_limit)
{
	BackgroundWorker bgw;
	BackgroundWorkerHandle *bgw_handle;

	if (ChecksumHelperShmem->abort)
	{
		ereport(ERROR,
				(errmsg("could not start checksumhelper: has been cancelled")));
	}

	ChecksumHelperShmem->cost_delay = cost_delay;
	ChecksumHelperShmem->cost_limit = cost_limit;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ChecksumHelperLauncherMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "checksumhelper launcher");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "checksumhelper launcher");
	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	bgw.bgw_notify_pid = MyProcPid;
	bgw.bgw_main_arg = (Datum) 0;

	if (!pg_atomic_test_set_flag(&ChecksumHelperShmem->launcher_started))
	{
		/* Failed to set means somebody else started */
		ereport(ERROR,
				(errmsg("could not start checksumhelper: already running")));
	}

	if (!RegisterDynamicBackgroundWorker(&bgw, &bgw_handle))
	{
		pg_atomic_clear_flag(&ChecksumHelperShmem->launcher_started);
		return false;
	}

	return true;
}

/*
 * ShutdownChecksumHelperIfRunning
 *		Request shutdown of the checksumhelper
 *
 * This does not turn off processing immediately, it signals the checksum
 * process to end when done with the current block.
 */
void
ShutdownChecksumHelperIfRunning(void)
{
	/* If the launcher isn't started, there is nothing to shut down */
	if (pg_atomic_unlocked_test_flag(&ChecksumHelperShmem->launcher_started))
		return;

	/*
	 * We don't need an atomic variable for aborting, setting it multiple
	 * times will not change the handling.
	 */
	ChecksumHelperShmem->abort = true;
}

/*
 * ProcessSingleRelationFork
 *		Enable checksums in a single relation/fork.
 *
 * Returns true if successful, and false if *aborted*. On error, an actual
 * error is raised in the lower levels.
 */
static bool
ProcessSingleRelationFork(Relation reln, ForkNumber forkNum, BufferAccessStrategy strategy)
{
	BlockNumber numblocks = RelationGetNumberOfBlocksInFork(reln, forkNum);
	BlockNumber b;
	char		activity[NAMEDATALEN * 2 + 128];

	for (b = 0; b < numblocks; b++)
	{
		Buffer		buf = ReadBufferExtended(reln, forkNum, b, RBM_NORMAL, strategy);

		/*
		 * Report to pgstat every 100 blocks (so as not to "spam")
		 */
		if ((b % 100) == 0)
		{
			snprintf(activity, sizeof(activity) - 1, "processing: %s.%s (%s block %d/%d)",
					 get_namespace_name(RelationGetNamespace(reln)), RelationGetRelationName(reln),
					 forkNames[forkNum], b, numblocks);
			pgstat_report_activity(STATE_RUNNING, activity);
		}

		/* Need to get an exclusive lock before we can flag as dirty */
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		/*
		 * Mark the buffer as dirty and force a full page write.  We have to
		 * re-write the page to WAL even if the checksum hasn't changed,
		 * because if there is a replica it might have a slightly different
		 * version of the page with an invalid checksum, caused by unlogged
		 * changes (e.g. hintbits) on the master happening while checksums
		 * were off. This can happen if there was a valid checksum on the page
		 * at one point in the past, so only when checksums are first on, then
		 * off, and then turned on again.
		 */
		START_CRIT_SECTION();
		MarkBufferDirty(buf);
		log_newpage_buffer(buf, false);
		END_CRIT_SECTION();

		UnlockReleaseBuffer(buf);

		/*
		 * This is the only place where we check if we are asked to abort, the
		 * abortion will bubble up from here.
		 */
		if (ChecksumHelperShmem->abort)
			return false;

		vacuum_delay_point();
	}

	return true;
}

/*
 * ProcessSingleRelationByOid
 *		Process a single relation based on oid.
 *
 * Returns true if successful, and false if *aborted*. On error, an actual error
 * is raised in the lower levels.
 */
static bool
ProcessSingleRelationByOid(Oid relationId, BufferAccessStrategy strategy)
{
	Relation	rel;
	ForkNumber	fnum;
	bool		aborted = false;

	StartTransactionCommand();

	elog(DEBUG2, "Checksumhelper starting to process relation %d", relationId);
	rel = try_relation_open(relationId, AccessShareLock);
	if (rel == NULL)
	{
		/*
		 * Relation no longer exist. We consider this a success, since there
		 * are no pages in it that need checksums, and thus return true.
		 */
		elog(DEBUG1, "Checksumhelper skipping relation %d as it no longer exists", relationId);
		CommitTransactionCommand();
		pgstat_report_activity(STATE_IDLE, NULL);
		return true;
	}
	RelationOpenSmgr(rel);

	for (fnum = 0; fnum <= MAX_FORKNUM; fnum++)
	{
		if (smgrexists(rel->rd_smgr, fnum))
		{
			if (!ProcessSingleRelationFork(rel, fnum, strategy))
			{
				aborted = true;
				break;
			}
		}
	}
	relation_close(rel, AccessShareLock);
	elog(DEBUG2, "Checksumhelper done with relation %d: %s",
		 relationId, (aborted ? "aborted" : "finished"));

	CommitTransactionCommand();

	pgstat_report_activity(STATE_IDLE, NULL);

	return !aborted;
}

/*
 * ProcessDatabase
 *		Enable checksums in a single database.
 *
 * We do this by launching a dynamic background worker into this database, and
 * waiting for it to finish.  We have to do this in a separate worker, since
 * each process can only be connected to one database during its lifetime.
 */
static ChecksumHelperResult
ProcessDatabase(ChecksumHelperDatabase * db)
{
	BackgroundWorker bgw;
	BackgroundWorkerHandle *bgw_handle;
	BgwHandleStatus status;
	pid_t		pid;
	char		activity[NAMEDATALEN + 64];

	ChecksumHelperShmem->success = FAILED;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "postgres");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ChecksumHelperWorkerMain");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "checksumhelper worker");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "checksumhelper worker");
	bgw.bgw_restart_time = BGW_NEVER_RESTART;
	bgw.bgw_notify_pid = MyProcPid;
	bgw.bgw_main_arg = ObjectIdGetDatum(db->dboid);

	if (!RegisterDynamicBackgroundWorker(&bgw, &bgw_handle))
	{
		ereport(LOG,
				(errmsg("failed to start worker for checksumhelper in \"%s\"",
						db->dbname)));
		return FAILED;
	}

	status = WaitForBackgroundWorkerStartup(bgw_handle, &pid);
	if (status != BGWH_STARTED)
	{
		ereport(LOG,
				(errmsg("failed to wait for worker startup for checksumhelper in \"%s\"",
						db->dbname)));
		return FAILED;
	}

	ereport(DEBUG1,
			(errmsg("started background worker for checksums in \"%s\"",
					db->dbname)));

	snprintf(activity, sizeof(activity) - 1,
			 "Waiting for worker in database %s (pid %d)", db->dbname, pid);
	pgstat_report_activity(STATE_RUNNING, activity);


	status = WaitForBackgroundWorkerShutdown(bgw_handle);
	if (status != BGWH_STOPPED)
	{
		ereport(LOG,
				(errmsg("failed to wait for worker shutdown for checksumhelper in \"%s\"",
						db->dbname)));
		return FAILED;
	}

	if (ChecksumHelperShmem->success == ABORTED)
		ereport(LOG,
				(errmsg("checksumhelper was aborted during processing in \"%s\"",
						db->dbname)));

	ereport(DEBUG1,
			(errmsg("background worker for checksums in \"%s\" completed",
					db->dbname)));

	pgstat_report_activity(STATE_IDLE, NULL);

	return ChecksumHelperShmem->success;
}

static void
launcher_exit(int code, Datum arg)
{
	ChecksumHelperShmem->abort = false;
	pg_atomic_clear_flag(&ChecksumHelperShmem->launcher_started);
}

static void
launcher_cancel_handler(SIGNAL_ARGS)
{
	ChecksumHelperShmem->abort = true;
}

static void
WaitForAllTransactionsToFinish(void)
{
	TransactionId waitforxid;

	LWLockAcquire(XidGenLock, LW_SHARED);
	waitforxid = ShmemVariableCache->nextXid;
	LWLockRelease(XidGenLock);

	while (true)
	{
		TransactionId oldestxid = GetOldestActiveTransactionId();

		elog(DEBUG1, "Checking old transactions");
		if (TransactionIdPrecedes(oldestxid, waitforxid))
		{
			char		activity[64];

			/* Oldest running xid is older than us, so wait */
			snprintf(activity, sizeof(activity), "Waiting for current transactions to finish (waiting for %d)", waitforxid);
			pgstat_report_activity(STATE_RUNNING, activity);

			/* Retry every 5 seconds */
			ResetLatch(MyLatch);
			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT,
							 5000,
							 WAIT_EVENT_PG_SLEEP);
		}
		else
		{
			pgstat_report_activity(STATE_IDLE, NULL);
			return;
		}
	}
}

void
ChecksumHelperLauncherMain(Datum arg)
{
	List	   *DatabaseList;
	List	   *remaining = NIL;
	ListCell   *lc,
			   *lc2;
	List	   *CurrentDatabases = NIL;
	bool		found_failed = false;

	on_shmem_exit(launcher_exit, 0);

	ereport(DEBUG1,
			(errmsg("checksumhelper launcher started")));

	pqsignal(SIGTERM, die);
	pqsignal(SIGINT, launcher_cancel_handler);

	BackgroundWorkerUnblockSignals();

	init_ps_display(pgstat_get_backend_desc(B_CHECKSUMHELPER_LAUNCHER), "", "", "");

	/*
	 * Initialize a connection to shared catalogs only.
	 */
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	/*
	 * Set up so first run processes shared catalogs, but not once in every
	 * db.
	 */
	ChecksumHelperShmem->process_shared_catalogs = true;

	/*
	 * Wait for all existing transactions to finish. This will make sure that
	 * we can see all tables all databases, so we don't miss any. Anything
	 * created after this point is known to have checksums on all pages
	 * already, so we don't have to care about those.
	 */
	WaitForAllTransactionsToFinish();

	/*
	 * Create a database list.  We don't need to concern ourselves with
	 * rebuilding this list during runtime since any database created after
	 * this process started will be running with checksums turned on from the
	 * start.
	 */
	DatabaseList = BuildDatabaseList();

	/*
	 * If there are no databases at all to checksum, we can exit immediately
	 * as there is no work to do.
	 */
	if (DatabaseList == NIL || list_length(DatabaseList) == 0)
		return;

	foreach(lc, DatabaseList)
	{
		ChecksumHelperDatabase *db = (ChecksumHelperDatabase *) lfirst(lc);
		ChecksumHelperResult processing;

		processing = ProcessDatabase(db);

		if (processing == SUCCESSFUL)
		{
			pfree(db->dbname);
			pfree(db);

			if (ChecksumHelperShmem->process_shared_catalogs)

				/*
				 * Now that one database has completed shared catalogs, we
				 * don't have to process them again.
				 */
				ChecksumHelperShmem->process_shared_catalogs = false;
		}
		else if (processing == FAILED)
		{
			/*
			 * Put failed databases on the remaining list.
			 */
			remaining = lappend(remaining, db);
		}
		else
			/* aborted */
			return;
	}
	list_free(DatabaseList);

	/*
	 * remaining now has all databases not yet processed. This can be because
	 * they failed for some reason, or because the database was dropped
	 * between us getting the database list and trying to process it. Get a
	 * fresh list of databases to detect the second case where the database
	 * was dropped before we had started processing it. If a database still
	 * exists, but enabling checksums failed then we fail the entire
	 * checksumming process and exit with an error.
	 */
	CurrentDatabases = BuildDatabaseList();

	foreach(lc, remaining)
	{
		ChecksumHelperDatabase *db = (ChecksumHelperDatabase *) lfirst(lc);
		bool		found = false;

		foreach(lc2, CurrentDatabases)
		{
			ChecksumHelperDatabase *db2 = (ChecksumHelperDatabase *) lfirst(lc2);

			if (db->dboid == db2->dboid)
			{
				found = true;
				ereport(WARNING,
						(errmsg("failed to enable checksums in \"%s\"",
								db->dbname)));
				break;
			}
		}

		if (found)
			found_failed = true;
		else
		{
			ereport(LOG,
					(errmsg("database \"%s\" has been dropped, skipping",
							db->dbname)));
		}

		pfree(db->dbname);
		pfree(db);
	}
	list_free(remaining);

	/* Free the extra list of databases */
	foreach(lc, CurrentDatabases)
	{
		ChecksumHelperDatabase *db = (ChecksumHelperDatabase *) lfirst(lc);

		pfree(db->dbname);
		pfree(db);
	}
	list_free(CurrentDatabases);

	if (found_failed)
	{
		/* Disable checksums on cluster, because we failed */
		SetDataChecksumsOff();
		ereport(ERROR,
				(errmsg("checksumhelper failed to enable checksums in all databases, aborting")));
	}

	/*
	 * Force a checkpoint to get everything out to disk.
	 */
	RequestCheckpoint(CHECKPOINT_FORCE | CHECKPOINT_WAIT | CHECKPOINT_IMMEDIATE);

	/*
	 * Everything has been processed, so flag checksums enabled.
	 */
	SetDataChecksumsOn();

	ereport(LOG,
			(errmsg("checksums enabled, checksumhelper launcher shutting down")));
}

/*
 * ChecksumHelperShmemSize
 *		Compute required space for checksumhelper-related shared memory
 */
Size
ChecksumHelperShmemSize(void)
{
	Size		size;

	size = sizeof(ChecksumHelperShmemStruct);
	size = MAXALIGN(size);

	return size;
}

/*
 * ChecksumHelperShmemInit
 *		Allocate and initialize checksumhelper-related shared memory
 */
void
ChecksumHelperShmemInit(void)
{
	bool		found;

	ChecksumHelperShmem = (ChecksumHelperShmemStruct *)
		ShmemInitStruct("ChecksumHelper Data",
						ChecksumHelperShmemSize(),
						&found);

	if (!found)
	{
		MemSet(ChecksumHelperShmem, 0, ChecksumHelperShmemSize());
		pg_atomic_init_flag(&ChecksumHelperShmem->launcher_started);
	}
}

/*
 * BuildDatabaseList
 *		Compile a list of all currently available databases in the cluster
 *
 * This creates the list of databases for the checksumhelper workers to add
 * checksums to.
 */
static List *
BuildDatabaseList(void)
{
	List	   *DatabaseList = NIL;
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tup;
	MemoryContext ctx = CurrentMemoryContext;
	MemoryContext oldctx;

	StartTransactionCommand();

	rel = heap_open(DatabaseRelationId, AccessShareLock);
	scan = heap_beginscan_catalog(rel, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_database pgdb = (Form_pg_database) GETSTRUCT(tup);
		ChecksumHelperDatabase *db;

		oldctx = MemoryContextSwitchTo(ctx);

		db = (ChecksumHelperDatabase *) palloc(sizeof(ChecksumHelperDatabase));

		db->dboid = HeapTupleGetOid(tup);
		db->dbname = pstrdup(NameStr(pgdb->datname));

		DatabaseList = lappend(DatabaseList, db);

		MemoryContextSwitchTo(oldctx);
	}

	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	CommitTransactionCommand();

	return DatabaseList;
}

/*
 * BuildRelationList
 *		Compile a list of all relations in the database
 *
 * If shared is true, both shared relations and local ones are returned, else
 * all non-shared relations are returned.
 * Temp tables are not included.
 */
static List *
BuildRelationList(bool include_shared)
{
	List	   *RelationList = NIL;
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tup;
	MemoryContext ctx = CurrentMemoryContext;
	MemoryContext oldctx;

	StartTransactionCommand();

	rel = heap_open(RelationRelationId, AccessShareLock);
	scan = heap_beginscan_catalog(rel, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_class pgc = (Form_pg_class) GETSTRUCT(tup);
		ChecksumHelperRelation *relentry;

		if (pgc->relpersistence == 't')
			continue;

		if (pgc->relisshared && !include_shared)
			continue;

		/*
		 * Foreign tables have by definition no local storage that can be
		 * checksummed, so skip.
		 */
		if (pgc->relkind == RELKIND_FOREIGN_TABLE)
			continue;

		oldctx = MemoryContextSwitchTo(ctx);
		relentry = (ChecksumHelperRelation *) palloc(sizeof(ChecksumHelperRelation));

		relentry->reloid = HeapTupleGetOid(tup);
		relentry->relkind = pgc->relkind;

		RelationList = lappend(RelationList, relentry);

		MemoryContextSwitchTo(oldctx);
	}

	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	CommitTransactionCommand();

	return RelationList;
}

/*
 * BuildTempTableList
 *		Compile a list of all temporary tables in database
 *
 * Returns a List of oids.
 */
static List *
BuildTempTableList(void)
{
	List	   *RelationList = NIL;
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tup;
	MemoryContext ctx = CurrentMemoryContext;
	MemoryContext oldctx;

	StartTransactionCommand();

	rel = heap_open(RelationRelationId, AccessShareLock);
	scan = heap_beginscan_catalog(rel, 0, NULL);

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_class pgc = (Form_pg_class) GETSTRUCT(tup);

		if (pgc->relpersistence != 't')
			continue;

		oldctx = MemoryContextSwitchTo(ctx);
		RelationList = lappend_oid(RelationList, HeapTupleGetOid(tup));
		MemoryContextSwitchTo(oldctx);
	}

	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	CommitTransactionCommand();

	return RelationList;
}

/*
 * Main function for enabling checksums in a single database
 */
void
ChecksumHelperWorkerMain(Datum arg)
{
	Oid			dboid = DatumGetObjectId(arg);
	List	   *RelationList = NIL;
	List	   *InitialTempTableList = NIL;
	ListCell   *lc;
	BufferAccessStrategy strategy;
	bool		aborted = false;

	pqsignal(SIGTERM, die);

	BackgroundWorkerUnblockSignals();

	init_ps_display(pgstat_get_backend_desc(B_CHECKSUMHELPER_WORKER), "", "", "");

	ereport(DEBUG1,
			(errmsg("checksum worker starting for database oid %d", dboid)));

	BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid, BGWORKER_BYPASS_ALLOWCONN);

	/*
	 * Get a list of all temp tables present as we start in this database. We
	 * need to wait until they are all gone until we are done, since we cannot
	 * access those files and modify them.
	 */
	InitialTempTableList = BuildTempTableList();

	/*
	 * Enable vacuum cost delay, if any.
	 */
	VacuumCostDelay = ChecksumHelperShmem->cost_delay;
	VacuumCostLimit = ChecksumHelperShmem->cost_limit;
	VacuumCostActive = (VacuumCostDelay > 0);
	VacuumCostBalance = 0;
	VacuumPageHit = 0;
	VacuumPageMiss = 0;
	VacuumPageDirty = 0;

	/*
	 * Create and set the vacuum strategy as our buffer strategy.
	 */
	strategy = GetAccessStrategy(BAS_VACUUM);

	RelationList = BuildRelationList(ChecksumHelperShmem->process_shared_catalogs);
	foreach(lc, RelationList)
	{
		ChecksumHelperRelation *rel = (ChecksumHelperRelation *) lfirst(lc);

		if (!ProcessSingleRelationByOid(rel->reloid, strategy))
		{
			aborted = true;
			break;
		}
	}
	list_free_deep(RelationList);

	if (aborted)
	{
		ChecksumHelperShmem->success = ABORTED;
		ereport(DEBUG1,
				(errmsg("checksum worker aborted in database oid %d", dboid)));
		return;
	}

	/*
	 * Wait for all temp tables that existed when we started to go away. This
	 * is necessary since we cannot "reach" them to enable checksums. Any temp
	 * tables created after we started will already have checksums in them
	 * (due to the inprogress state), so those are safe.
	 */
	while (true)
	{
		List	   *CurrentTempTables;
		ListCell   *lc;
		int			numleft;
		char		activity[64];

		CurrentTempTables = BuildTempTableList();
		numleft = 0;
		foreach(lc, InitialTempTableList)
		{
			if (list_member_oid(CurrentTempTables, lfirst_oid(lc)))
				numleft++;
		}
		list_free(CurrentTempTables);

		if (numleft == 0)
			break;

		/* At least one temp table left to wait for */
		snprintf(activity, sizeof(activity), "Waiting for %d temp tables to be removed", numleft);
		pgstat_report_activity(STATE_RUNNING, activity);

		/* Retry every 5 seconds */
		ResetLatch(MyLatch);
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT,
						 5000,
						 WAIT_EVENT_PG_SLEEP);
	}

	list_free(InitialTempTableList);

	ChecksumHelperShmem->success = SUCCESSFUL;
	ereport(DEBUG1,
			(errmsg("checksum worker completed in database oid %d", dboid)));
}
