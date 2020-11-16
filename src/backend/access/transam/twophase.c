/*-------------------------------------------------------------------------
 *
 * twophase.c
 *		Two-phase commit support functions.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/backend/access/transam/twophase.c
 *
 * NOTES
 *		Each global transaction is associated with a global transaction
 *		identifier (GID). The client assigns a GID to a postgres
 *		transaction with the PREPARE TRANSACTION command.
 *
 *		We keep all active global transactions in a shared memory array.
 *		When the PREPARE TRANSACTION command is issued, the GID is
 *		reserved for the transaction in the array. This is done before
 *		a WAL entry is made, because the reservation checks for duplicate
 *		GIDs and aborts the transaction if there already is a global
 *		transaction in prepared state with the same GID.
 *
 *		A global transaction (gxact) also has dummy PGPROC; this is what keeps
 *		the XID considered running by TransactionIdIsInProgress.  It is also
 *		convenient as a PGPROC to hook the gxact's locks to.
 *
 *		Information to recover prepared transactions in case of crash is
 *		now stored in WAL for the common case. In some cases there will be
 *		an extended period between preparing a GXACT and commit/abort, in
 *		which case we need to separately record prepared transaction data
 *		in permanent storage. This includes locking information, pending
 *		notifications etc. All that state information is written to the
 *		per-transaction state file in the pg_twophase directory.
 *		All prepared transactions will be written prior to shutdown.
 *
 *		Life track of state data is following:
 *
 *		* On PREPARE TRANSACTION backend writes state data only to the WAL and
 *		  stores pointer to the start of the WAL record in
 *		  gxact->prepare_start_lsn.
 *		* If COMMIT occurs before checkpoint then backend reads data from WAL
 *		  using prepare_start_lsn.
 *		* On checkpoint state data copied to files in pg_twophase directory and
 *		  fsynced
 *		* If COMMIT happens after checkpoint then backend reads state data from
 *		  files
 *
 *		During replay and replication, TwoPhaseState also holds information
 *		about active prepared transactions that haven't been moved to disk yet.
 *
 *		Replay of twophase records happens by the following rules:
 *
 *		* At the beginning of recovery, pg_twophase is scanned once, filling
 *		  TwoPhaseState with entries marked with gxact->inredo and
 *		  gxact->ondisk.  Two-phase file data older than the XID horizon of
 *		  the redo position are discarded.
 *		* On PREPARE redo, the transaction is added to TwoPhaseState->prepXacts.
 *		  gxact->inredo is set to true for such entries.
 *		* On Checkpoint we iterate through TwoPhaseState->prepXacts entries
 *		  that have gxact->inredo set and are behind the redo_horizon. We
 *		  save them to disk and then switch gxact->ondisk to true.
 *		* On COMMIT/ABORT we delete the entry from TwoPhaseState->prepXacts.
 *		  If gxact->ondisk is true, the corresponding entry from the disk
 *		  is additionally deleted.
 *		* RecoverPreparedTransactions(), StandbyRecoverPreparedTransactions()
 *		  and PrescanPreparedTransactions() have been modified to go through
 *		  gxact->inredo entries that have not made it to disk.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "access/commit_ts.h"
#include "access/htup_details.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "access/xlogutils.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "replication/origin.h"
#include "replication/syncrep.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/md.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinvaladt.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

/*
 * Directory where Two-phase commit files reside within PGDATA
 */
#define TWOPHASE_DIR "pg_twophase"

/* GUC variable, can't be changed after startup */
int			max_prepared_xacts = 0;

/*
 * This struct describes one global transaction that is in prepared state
 * or attempting to become prepared.
 *
 * The lifecycle of a global transaction is:
 *
 * 1. After checking that the requested GID is not in use, set up an entry in
 * the TwoPhaseState->prepXacts array with the correct GID and valid = false,
 * and mark it as locked by my backend.
 *
 * 2. After successfully completing prepare, set valid = true and enter the
 * referenced PGPROC into the global ProcArray.
 *
 * 3. To begin COMMIT PREPARED or ROLLBACK PREPARED, check that the entry is
 * valid and not locked, then mark the entry as locked by storing my current
 * backend ID into locking_backend.  This prevents concurrent attempts to
 * commit or rollback the same prepared xact.
 *
 * 4. On completion of COMMIT PREPARED or ROLLBACK PREPARED, remove the entry
 * from the ProcArray and the TwoPhaseState->prepXacts array and return it to
 * the freelist.
 *
 * Note that if the preparing transaction fails between steps 1 and 2, the
 * entry must be removed so that the GID and the GlobalTransaction struct
 * can be reused.  See AtAbort_Twophase().
 *
 * typedef struct GlobalTransactionData *GlobalTransaction appears in
 * twophase.h
 */

typedef struct GlobalTransactionData
{
	GlobalTransaction next;		/* list link for free list */
	int			pgprocno;		/* ID of associated dummy PGPROC */
	BackendId	dummyBackendId; /* similar to backend id for backends */
	TimestampTz prepared_at;	/* time of preparation */

	/*
	 * Note that we need to keep track of two LSNs for each GXACT. We keep
	 * track of the start LSN because this is the address we must use to read
	 * state data back from WAL when committing a prepared GXACT. We keep
	 * track of the end LSN because that is the LSN we need to wait for prior
	 * to commit.
	 */
	XLogRecPtr	prepare_start_lsn;	/* XLOG offset of prepare record start */
	XLogRecPtr	prepare_end_lsn;	/* XLOG offset of prepare record end */
	TransactionId xid;			/* The GXACT id */

	Oid			owner;			/* ID of user that executed the xact */
	BackendId	locking_backend;	/* backend currently working on the xact */
	bool		valid;			/* true if PGPROC entry is in proc array */
	bool		ondisk;			/* true if prepare state file is on disk */
	bool		inredo;			/* true if entry was added via xlog_redo */
	char		gid[GIDSIZE];	/* The GID assigned to the prepared xact */
}			GlobalTransactionData;

/*
 * Two Phase Commit shared state.  Access to this struct is protected
 * by TwoPhaseStateLock.
 */
typedef struct TwoPhaseStateData
{
	/* Head of linked list of free GlobalTransactionData structs */
	GlobalTransaction freeGXacts;

	/* Number of valid prepXacts entries. */
	int			numPrepXacts;

	/* There are max_prepared_xacts items in this array */
	GlobalTransaction prepXacts[FLEXIBLE_ARRAY_MEMBER];
} TwoPhaseStateData;

static TwoPhaseStateData *TwoPhaseState;

/*
 * Global transaction entry currently locked by us, if any.  Note that any
 * access to the entry pointed to by this variable must be protected by
 * TwoPhaseStateLock, though obviously the pointer itself doesn't need to be
 * (since it's just local memory).
 */
static GlobalTransaction MyLockedGxact = NULL;

static bool twophaseExitRegistered = false;

static void RecordTransactionCommitPrepared(TransactionId xid,
											int nchildren,
											TransactionId *children,
											int nrels,
											RelFileNode *rels,
											int ninvalmsgs,
											SharedInvalidationMessage *invalmsgs,
											bool initfileinval,
											const char *gid);
static void RecordTransactionAbortPrepared(TransactionId xid,
										   int nchildren,
										   TransactionId *children,
										   int nrels,
										   RelFileNode *rels,
										   const char *gid);
static void ProcessRecords(char *bufptr, TransactionId xid,
						   const TwoPhaseCallback callbacks[]);
static void RemoveGXact(GlobalTransaction gxact);

static void XlogReadTwoPhaseData(XLogRecPtr lsn, char **buf, int *len);
static char *ProcessTwoPhaseBuffer(TransactionId xid,
								   XLogRecPtr prepare_start_lsn,
								   bool fromdisk, bool setParent, bool setNextXid);
static void MarkAsPreparingGuts(GlobalTransaction gxact, TransactionId xid,
								const char *gid, TimestampTz prepared_at, Oid owner,
								Oid databaseid);
static void RemoveTwoPhaseFile(TransactionId xid, bool giveWarning);
static void RecreateTwoPhaseFile(TransactionId xid, void *content, int len);

/*
 * Initialization of shared memory
 */
Size
TwoPhaseShmemSize(void)
{
	Size		size;

	/* Need the fixed struct, the array of pointers, and the GTD structs */
	size = offsetof(TwoPhaseStateData, prepXacts);
	size = add_size(size, mul_size(max_prepared_xacts,
								   sizeof(GlobalTransaction)));
	size = MAXALIGN(size);
	size = add_size(size, mul_size(max_prepared_xacts,
								   sizeof(GlobalTransactionData)));

	return size;
}

void
TwoPhaseShmemInit(void)
{
	bool		found;

	TwoPhaseState = ShmemInitStruct("Prepared Transaction Table",
									TwoPhaseShmemSize(),
									&found);
	if (!IsUnderPostmaster)
	{
		GlobalTransaction gxacts;
		int			i;

		Assert(!found);
		TwoPhaseState->freeGXacts = NULL;
		TwoPhaseState->numPrepXacts = 0;

		/*
		 * Initialize the linked list of free GlobalTransactionData structs
		 */
		gxacts = (GlobalTransaction)
			((char *) TwoPhaseState +
			 MAXALIGN(offsetof(TwoPhaseStateData, prepXacts) +
					  sizeof(GlobalTransaction) * max_prepared_xacts));
		for (i = 0; i < max_prepared_xacts; i++)
		{
			/* insert into linked list */
			gxacts[i].next = TwoPhaseState->freeGXacts;
			TwoPhaseState->freeGXacts = &gxacts[i];

			/* associate it with a PGPROC assigned by InitProcGlobal */
			gxacts[i].pgprocno = PreparedXactProcs[i].pgprocno;

			/*
			 * Assign a unique ID for each dummy proc, so that the range of
			 * dummy backend IDs immediately follows the range of normal
			 * backend IDs. We don't dare to assign a real backend ID to dummy
			 * procs, because prepared transactions don't take part in cache
			 * invalidation like a real backend ID would imply, but having a
			 * unique ID for them is nevertheless handy. This arrangement
			 * allows you to allocate an array of size (MaxBackends +
			 * max_prepared_xacts + 1), and have a slot for every backend and
			 * prepared transaction. Currently multixact.c uses that
			 * technique.
			 */
			gxacts[i].dummyBackendId = MaxBackends + 1 + i;
		}
	}
	else
		Assert(found);
}

/*
 * Exit hook to unlock the global transaction entry we're working on.
 */
static void
AtProcExit_Twophase(int code, Datum arg)
{
	/* same logic as abort */
	AtAbort_Twophase();
}

/*
 * Abort hook to unlock the global transaction entry we're working on.
 */
void
AtAbort_Twophase(void)
{
	if (MyLockedGxact == NULL)
		return;

	/*
	 * What to do with the locked global transaction entry?  If we were in the
	 * process of preparing the transaction, but haven't written the WAL
	 * record and state file yet, the transaction must not be considered as
	 * prepared.  Likewise, if we are in the process of finishing an
	 * already-prepared transaction, and fail after having already written the
	 * 2nd phase commit or rollback record to the WAL, the transaction should
	 * not be considered as prepared anymore.  In those cases, just remove the
	 * entry from shared memory.
	 *
	 * Otherwise, the entry must be left in place so that the transaction can
	 * be finished later, so just unlock it.
	 *
	 * If we abort during prepare, after having written the WAL record, we
	 * might not have transferred all locks and other state to the prepared
	 * transaction yet.  Likewise, if we abort during commit or rollback,
	 * after having written the WAL record, we might not have released all the
	 * resources held by the transaction yet.  In those cases, the in-memory
	 * state can be wrong, but it's too late to back out.
	 */
	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	if (!MyLockedGxact->valid)
		RemoveGXact(MyLockedGxact);
	else
		MyLockedGxact->locking_backend = InvalidBackendId;
	LWLockRelease(TwoPhaseStateLock);

	MyLockedGxact = NULL;
}

/*
 * This is called after we have finished transferring state to the prepared
 * PGPROC entry.
 */
void
PostPrepare_Twophase(void)
{
	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	MyLockedGxact->locking_backend = InvalidBackendId;
	LWLockRelease(TwoPhaseStateLock);

	MyLockedGxact = NULL;
}


/*
 * MarkAsPreparing
 *		Reserve the GID for the given transaction.
 */
GlobalTransaction
MarkAsPreparing(TransactionId xid, const char *gid,
				TimestampTz prepared_at, Oid owner, Oid databaseid)
{
	GlobalTransaction gxact;
	int			i;

	if (strlen(gid) >= GIDSIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("transaction identifier \"%s\" is too long",
						gid)));

	/* fail immediately if feature is disabled */
	if (max_prepared_xacts == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("prepared transactions are disabled"),
				 errhint("Set max_prepared_transactions to a nonzero value.")));

	/* on first call, register the exit hook */
	if (!twophaseExitRegistered)
	{
		before_shmem_exit(AtProcExit_Twophase, 0);
		twophaseExitRegistered = true;
	}

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);

	/* Check for conflicting GID */
	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		gxact = TwoPhaseState->prepXacts[i];
		if (strcmp(gxact->gid, gid) == 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("transaction identifier \"%s\" is already in use",
							gid)));
		}
	}

	/* Get a free gxact from the freelist */
	if (TwoPhaseState->freeGXacts == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("maximum number of prepared transactions reached"),
				 errhint("Increase max_prepared_transactions (currently %d).",
						 max_prepared_xacts)));
	gxact = TwoPhaseState->freeGXacts;
	TwoPhaseState->freeGXacts = gxact->next;

	MarkAsPreparingGuts(gxact, xid, gid, prepared_at, owner, databaseid);

	gxact->ondisk = false;

	/* And insert it into the active array */
	Assert(TwoPhaseState->numPrepXacts < max_prepared_xacts);
	TwoPhaseState->prepXacts[TwoPhaseState->numPrepXacts++] = gxact;

	LWLockRelease(TwoPhaseStateLock);

	return gxact;
}

/*
 * MarkAsPreparingGuts
 *
 * This uses a gxact struct and puts it into the active array.
 * NOTE: this is also used when reloading a gxact after a crash; so avoid
 * assuming that we can use very much backend context.
 *
 * Note: This function should be called with appropriate locks held.
 */
static void
MarkAsPreparingGuts(GlobalTransaction gxact, TransactionId xid, const char *gid,
					TimestampTz prepared_at, Oid owner, Oid databaseid)
{
	PGPROC	   *proc;
	int			i;

	Assert(LWLockHeldByMeInMode(TwoPhaseStateLock, LW_EXCLUSIVE));

	Assert(gxact != NULL);
	proc = &ProcGlobal->allProcs[gxact->pgprocno];

	/* Initialize the PGPROC entry */
	MemSet(proc, 0, sizeof(PGPROC));
	proc->pgprocno = gxact->pgprocno;
	SHMQueueElemInit(&(proc->links));
	proc->waitStatus = PROC_WAIT_STATUS_OK;
	/* We set up the gxact's VXID as InvalidBackendId/XID */
	proc->lxid = (LocalTransactionId) xid;
	proc->xid = xid;
	Assert(proc->xmin == InvalidTransactionId);
	proc->delayChkpt = false;
	proc->statusFlags = 0;
	proc->pid = 0;
	proc->backendId = InvalidBackendId;
	proc->databaseId = databaseid;
	proc->roleId = owner;
	proc->tempNamespaceId = InvalidOid;
	proc->isBackgroundWorker = false;
	proc->lwWaiting = false;
	proc->lwWaitMode = 0;
	proc->waitLock = NULL;
	proc->waitProcLock = NULL;
	for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
		SHMQueueInit(&(proc->myProcLocks[i]));
	/* subxid data must be filled later by GXactLoadSubxactData */
	proc->subxidStatus.overflowed = false;
	proc->subxidStatus.count = 0;

	gxact->prepared_at = prepared_at;
	gxact->xid = xid;
	gxact->owner = owner;
	gxact->locking_backend = MyBackendId;
	gxact->valid = false;
	gxact->inredo = false;
	strcpy(gxact->gid, gid);

	/*
	 * Remember that we have this GlobalTransaction entry locked for us. If we
	 * abort after this, we must release it.
	 */
	MyLockedGxact = gxact;
}

/*
 * GXactLoadSubxactData
 *
 * If the transaction being persisted had any subtransactions, this must
 * be called before MarkAsPrepared() to load information into the dummy
 * PGPROC.
 */
static void
GXactLoadSubxactData(GlobalTransaction gxact, int nsubxacts,
					 TransactionId *children)
{
	PGPROC	   *proc = &ProcGlobal->allProcs[gxact->pgprocno];

	/* We need no extra lock since the GXACT isn't valid yet */
	if (nsubxacts > PGPROC_MAX_CACHED_SUBXIDS)
	{
		proc->subxidStatus.overflowed = true;
		nsubxacts = PGPROC_MAX_CACHED_SUBXIDS;
	}
	if (nsubxacts > 0)
	{
		memcpy(proc->subxids.xids, children,
			   nsubxacts * sizeof(TransactionId));
		proc->subxidStatus.count = nsubxacts;
	}
}

/*
 * MarkAsPrepared
 *		Mark the GXACT as fully valid, and enter it into the global ProcArray.
 *
 * lock_held indicates whether caller already holds TwoPhaseStateLock.
 */
static void
MarkAsPrepared(GlobalTransaction gxact, bool lock_held)
{
	/* Lock here may be overkill, but I'm not convinced of that ... */
	if (!lock_held)
		LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	Assert(!gxact->valid);
	gxact->valid = true;
	if (!lock_held)
		LWLockRelease(TwoPhaseStateLock);

	/*
	 * Put it into the global ProcArray so TransactionIdIsInProgress considers
	 * the XID as still running.
	 */
	ProcArrayAdd(&ProcGlobal->allProcs[gxact->pgprocno]);
}

/*
 * LockGXact
 *		Locate the prepared transaction and mark it busy for COMMIT or PREPARE.
 */
static GlobalTransaction
LockGXact(const char *gid, Oid user)
{
	int			i;

	/* on first call, register the exit hook */
	if (!twophaseExitRegistered)
	{
		before_shmem_exit(AtProcExit_Twophase, 0);
		twophaseExitRegistered = true;
	}

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);

	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];
		PGPROC	   *proc = &ProcGlobal->allProcs[gxact->pgprocno];

		/* Ignore not-yet-valid GIDs */
		if (!gxact->valid)
			continue;
		if (strcmp(gxact->gid, gid) != 0)
			continue;

		/* Found it, but has someone else got it locked? */
		if (gxact->locking_backend != InvalidBackendId)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("prepared transaction with identifier \"%s\" is busy",
							gid)));

		if (user != gxact->owner && !superuser_arg(user))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to finish prepared transaction"),
					 errhint("Must be superuser or the user that prepared the transaction.")));

		/*
		 * Note: it probably would be possible to allow committing from
		 * another database; but at the moment NOTIFY is known not to work and
		 * there may be some other issues as well.  Hence disallow until
		 * someone gets motivated to make it work.
		 */
		if (MyDatabaseId != proc->databaseId)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("prepared transaction belongs to another database"),
					 errhint("Connect to the database where the transaction was prepared to finish it.")));

		/* OK for me to lock it */
		gxact->locking_backend = MyBackendId;
		MyLockedGxact = gxact;

		LWLockRelease(TwoPhaseStateLock);

		return gxact;
	}

	LWLockRelease(TwoPhaseStateLock);

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("prepared transaction with identifier \"%s\" does not exist",
					gid)));

	/* NOTREACHED */
	return NULL;
}

/*
 * RemoveGXact
 *		Remove the prepared transaction from the shared memory array.
 *
 * NB: caller should have already removed it from ProcArray
 */
static void
RemoveGXact(GlobalTransaction gxact)
{
	int			i;

	Assert(LWLockHeldByMeInMode(TwoPhaseStateLock, LW_EXCLUSIVE));

	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		if (gxact == TwoPhaseState->prepXacts[i])
		{
			/* remove from the active array */
			TwoPhaseState->numPrepXacts--;
			TwoPhaseState->prepXacts[i] = TwoPhaseState->prepXacts[TwoPhaseState->numPrepXacts];

			/* and put it back in the freelist */
			gxact->next = TwoPhaseState->freeGXacts;
			TwoPhaseState->freeGXacts = gxact;

			return;
		}
	}

	elog(ERROR, "failed to find %p in GlobalTransaction array", gxact);
}

/*
 * Returns an array of all prepared transactions for the user-level
 * function pg_prepared_xact.
 *
 * The returned array and all its elements are copies of internal data
 * structures, to minimize the time we need to hold the TwoPhaseStateLock.
 *
 * WARNING -- we return even those transactions that are not fully prepared
 * yet.  The caller should filter them out if he doesn't want them.
 *
 * The returned array is palloc'd.
 */
static int
GetPreparedTransactionList(GlobalTransaction *gxacts)
{
	GlobalTransaction array;
	int			num;
	int			i;

	LWLockAcquire(TwoPhaseStateLock, LW_SHARED);

	if (TwoPhaseState->numPrepXacts == 0)
	{
		LWLockRelease(TwoPhaseStateLock);

		*gxacts = NULL;
		return 0;
	}

	num = TwoPhaseState->numPrepXacts;
	array = (GlobalTransaction) palloc(sizeof(GlobalTransactionData) * num);
	*gxacts = array;
	for (i = 0; i < num; i++)
		memcpy(array + i, TwoPhaseState->prepXacts[i],
			   sizeof(GlobalTransactionData));

	LWLockRelease(TwoPhaseStateLock);

	return num;
}


/* Working status for pg_prepared_xact */
typedef struct
{
	GlobalTransaction array;
	int			ngxacts;
	int			currIdx;
} Working_State;

/*
 * pg_prepared_xact
 *		Produce a view with one row per prepared transaction.
 *
 * This function is here so we don't have to export the
 * GlobalTransactionData struct definition.
 */
Datum
pg_prepared_xact(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Working_State *status;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * Switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* build tupdesc for result tuples */
		/* this had better match pg_prepared_xacts view in system_views.sql */
		tupdesc = CreateTemplateTupleDesc(5);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "transaction",
						   XIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "gid",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "prepared",
						   TIMESTAMPTZOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "ownerid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "dbid",
						   OIDOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/*
		 * Collect all the 2PC status information that we will format and send
		 * out as a result set.
		 */
		status = (Working_State *) palloc(sizeof(Working_State));
		funcctx->user_fctx = (void *) status;

		status->ngxacts = GetPreparedTransactionList(&status->array);
		status->currIdx = 0;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	status = (Working_State *) funcctx->user_fctx;

	while (status->array != NULL && status->currIdx < status->ngxacts)
	{
		GlobalTransaction gxact = &status->array[status->currIdx++];
		PGPROC	   *proc = &ProcGlobal->allProcs[gxact->pgprocno];
		Datum		values[5];
		bool		nulls[5];
		HeapTuple	tuple;
		Datum		result;

		if (!gxact->valid)
			continue;

		/*
		 * Form tuple with appropriate data.
		 */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		values[0] = TransactionIdGetDatum(proc->xid);
		values[1] = CStringGetTextDatum(gxact->gid);
		values[2] = TimestampTzGetDatum(gxact->prepared_at);
		values[3] = ObjectIdGetDatum(gxact->owner);
		values[4] = ObjectIdGetDatum(proc->databaseId);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * TwoPhaseGetGXact
 *		Get the GlobalTransaction struct for a prepared transaction
 *		specified by XID
 *
 * If lock_held is set to true, TwoPhaseStateLock will not be taken, so the
 * caller had better hold it.
 */
static GlobalTransaction
TwoPhaseGetGXact(TransactionId xid, bool lock_held)
{
	GlobalTransaction result = NULL;
	int			i;

	static TransactionId cached_xid = InvalidTransactionId;
	static GlobalTransaction cached_gxact = NULL;

	Assert(!lock_held || LWLockHeldByMe(TwoPhaseStateLock));

	/*
	 * During a recovery, COMMIT PREPARED, or ABORT PREPARED, we'll be called
	 * repeatedly for the same XID.  We can save work with a simple cache.
	 */
	if (xid == cached_xid)
		return cached_gxact;

	if (!lock_held)
		LWLockAcquire(TwoPhaseStateLock, LW_SHARED);

	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];

		if (gxact->xid == xid)
		{
			result = gxact;
			break;
		}
	}

	if (!lock_held)
		LWLockRelease(TwoPhaseStateLock);

	if (result == NULL)			/* should not happen */
		elog(ERROR, "failed to find GlobalTransaction for xid %u", xid);

	cached_xid = xid;
	cached_gxact = result;

	return result;
}

/*
 * TwoPhaseGetDummyBackendId
 *		Get the dummy backend ID for prepared transaction specified by XID
 *
 * Dummy backend IDs are similar to real backend IDs of real backends.
 * They start at MaxBackends + 1, and are unique across all currently active
 * real backends and prepared transactions.  If lock_held is set to true,
 * TwoPhaseStateLock will not be taken, so the caller had better hold it.
 */
BackendId
TwoPhaseGetDummyBackendId(TransactionId xid, bool lock_held)
{
	GlobalTransaction gxact = TwoPhaseGetGXact(xid, lock_held);

	return gxact->dummyBackendId;
}

/*
 * TwoPhaseGetDummyProc
 *		Get the PGPROC that represents a prepared transaction specified by XID
 *
 * If lock_held is set to true, TwoPhaseStateLock will not be taken, so the
 * caller had better hold it.
 */
PGPROC *
TwoPhaseGetDummyProc(TransactionId xid, bool lock_held)
{
	GlobalTransaction gxact = TwoPhaseGetGXact(xid, lock_held);

	return &ProcGlobal->allProcs[gxact->pgprocno];
}

/************************************************************************/
/* State file support													*/
/************************************************************************/

#define TwoPhaseFilePath(path, xid) \
	snprintf(path, MAXPGPATH, TWOPHASE_DIR "/%08X", xid)

/*
 * 2PC state file format:
 *
 *	1. TwoPhaseFileHeader
 *	2. TransactionId[] (subtransactions)
 *	3. RelFileNode[] (files to be deleted at commit)
 *	4. RelFileNode[] (files to be deleted at abort)
 *	5. SharedInvalidationMessage[] (inval messages to be sent at commit)
 *	6. TwoPhaseRecordOnDisk
 *	7. ...
 *	8. TwoPhaseRecordOnDisk (end sentinel, rmid == TWOPHASE_RM_END_ID)
 *	9. checksum (CRC-32C)
 *
 * Each segment except the final checksum is MAXALIGN'd.
 */

/*
 * Header for a 2PC state file
 */
#define TWOPHASE_MAGIC	0x57F94534	/* format identifier */

typedef xl_xact_prepare TwoPhaseFileHeader;

/*
 * Header for each record in a state file
 *
 * NOTE: len counts only the rmgr data, not the TwoPhaseRecordOnDisk header.
 * The rmgr data will be stored starting on a MAXALIGN boundary.
 */
typedef struct TwoPhaseRecordOnDisk
{
	uint32		len;			/* length of rmgr data */
	TwoPhaseRmgrId rmid;		/* resource manager for this record */
	uint16		info;			/* flag bits for use by rmgr */
} TwoPhaseRecordOnDisk;

/*
 * During prepare, the state file is assembled in memory before writing it
 * to WAL and the actual state file.  We use a chain of StateFileChunk blocks
 * for that.
 */
typedef struct StateFileChunk
{
	char	   *data;
	uint32		len;
	struct StateFileChunk *next;
} StateFileChunk;

static struct xllist
{
	StateFileChunk *head;		/* first data block in the chain */
	StateFileChunk *tail;		/* last block in chain */
	uint32		num_chunks;
	uint32		bytes_free;		/* free bytes left in tail block */
	uint32		total_len;		/* total data bytes in chain */
}			records;


/*
 * Append a block of data to records data structure.
 *
 * NB: each block is padded to a MAXALIGN multiple.  This must be
 * accounted for when the file is later read!
 *
 * The data is copied, so the caller is free to modify it afterwards.
 */
static void
save_state_data(const void *data, uint32 len)
{
	uint32		padlen = MAXALIGN(len);

	if (padlen > records.bytes_free)
	{
		records.tail->next = palloc0(sizeof(StateFileChunk));
		records.tail = records.tail->next;
		records.tail->len = 0;
		records.tail->next = NULL;
		records.num_chunks++;

		records.bytes_free = Max(padlen, 512);
		records.tail->data = palloc(records.bytes_free);
	}

	memcpy(((char *) records.tail->data) + records.tail->len, data, len);
	records.tail->len += padlen;
	records.bytes_free -= padlen;
	records.total_len += padlen;
}

/*
 * Start preparing a state file.
 *
 * Initializes data structure and inserts the 2PC file header record.
 */
void
StartPrepare(GlobalTransaction gxact)
{
	PGPROC	   *proc = &ProcGlobal->allProcs[gxact->pgprocno];
	TransactionId xid = gxact->xid;
	TwoPhaseFileHeader hdr;
	TransactionId *children;
	RelFileNode *commitrels;
	RelFileNode *abortrels;
	SharedInvalidationMessage *invalmsgs;

	/* Initialize linked list */
	records.head = palloc0(sizeof(StateFileChunk));
	records.head->len = 0;
	records.head->next = NULL;

	records.bytes_free = Max(sizeof(TwoPhaseFileHeader), 512);
	records.head->data = palloc(records.bytes_free);

	records.tail = records.head;
	records.num_chunks = 1;

	records.total_len = 0;

	/* Create header */
	hdr.magic = TWOPHASE_MAGIC;
	hdr.total_len = 0;			/* EndPrepare will fill this in */
	hdr.xid = xid;
	hdr.database = proc->databaseId;
	hdr.prepared_at = gxact->prepared_at;
	hdr.owner = gxact->owner;
	hdr.nsubxacts = xactGetCommittedChildren(&children);
	hdr.ncommitrels = smgrGetPendingDeletes(true, &commitrels);
	hdr.nabortrels = smgrGetPendingDeletes(false, &abortrels);
	hdr.ninvalmsgs = xactGetCommittedInvalidationMessages(&invalmsgs,
														  &hdr.initfileinval);
	hdr.gidlen = strlen(gxact->gid) + 1;	/* Include '\0' */

	save_state_data(&hdr, sizeof(TwoPhaseFileHeader));
	save_state_data(gxact->gid, hdr.gidlen);

	/*
	 * Add the additional info about subxacts, deletable files and cache
	 * invalidation messages.
	 */
	if (hdr.nsubxacts > 0)
	{
		save_state_data(children, hdr.nsubxacts * sizeof(TransactionId));
		/* While we have the child-xact data, stuff it in the gxact too */
		GXactLoadSubxactData(gxact, hdr.nsubxacts, children);
	}
	if (hdr.ncommitrels > 0)
	{
		save_state_data(commitrels, hdr.ncommitrels * sizeof(RelFileNode));
		pfree(commitrels);
	}
	if (hdr.nabortrels > 0)
	{
		save_state_data(abortrels, hdr.nabortrels * sizeof(RelFileNode));
		pfree(abortrels);
	}
	if (hdr.ninvalmsgs > 0)
	{
		save_state_data(invalmsgs,
						hdr.ninvalmsgs * sizeof(SharedInvalidationMessage));
		pfree(invalmsgs);
	}
}

/*
 * Finish preparing state data and writing it to WAL.
 */
void
EndPrepare(GlobalTransaction gxact)
{
	TwoPhaseFileHeader *hdr;
	StateFileChunk *record;
	bool		replorigin;

	/* Add the end sentinel to the list of 2PC records */
	RegisterTwoPhaseRecord(TWOPHASE_RM_END_ID, 0,
						   NULL, 0);

	/* Go back and fill in total_len in the file header record */
	hdr = (TwoPhaseFileHeader *) records.head->data;
	Assert(hdr->magic == TWOPHASE_MAGIC);
	hdr->total_len = records.total_len + sizeof(pg_crc32c);

	replorigin = (replorigin_session_origin != InvalidRepOriginId &&
				  replorigin_session_origin != DoNotReplicateId);

	if (replorigin)
	{
		Assert(replorigin_session_origin_lsn != InvalidXLogRecPtr);
		hdr->origin_lsn = replorigin_session_origin_lsn;
		hdr->origin_timestamp = replorigin_session_origin_timestamp;
	}
	else
	{
		hdr->origin_lsn = InvalidXLogRecPtr;
		hdr->origin_timestamp = 0;
	}

	/*
	 * If the data size exceeds MaxAllocSize, we won't be able to read it in
	 * ReadTwoPhaseFile. Check for that now, rather than fail in the case
	 * where we write data to file and then re-read at commit time.
	 */
	if (hdr->total_len > MaxAllocSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("two-phase state file maximum length exceeded")));

	/*
	 * Now writing 2PC state data to WAL. We let the WAL's CRC protection
	 * cover us, so no need to calculate a separate CRC.
	 *
	 * We have to set delayChkpt here, too; otherwise a checkpoint starting
	 * immediately after the WAL record is inserted could complete without
	 * fsync'ing our state file.  (This is essentially the same kind of race
	 * condition as the COMMIT-to-clog-write case that RecordTransactionCommit
	 * uses delayChkpt for; see notes there.)
	 *
	 * We save the PREPARE record's location in the gxact for later use by
	 * CheckPointTwoPhase.
	 */
	XLogEnsureRecordSpace(0, records.num_chunks);

	START_CRIT_SECTION();

	MyProc->delayChkpt = true;

	XLogBeginInsert();
	for (record = records.head; record != NULL; record = record->next)
		XLogRegisterData(record->data, record->len);

	XLogSetRecordFlags(XLOG_INCLUDE_ORIGIN);

	gxact->prepare_end_lsn = XLogInsert(RM_XACT_ID, XLOG_XACT_PREPARE);

	if (replorigin)
	{
		/* Move LSNs forward for this replication origin */
		replorigin_session_advance(replorigin_session_origin_lsn,
								   gxact->prepare_end_lsn);
	}

	XLogFlush(gxact->prepare_end_lsn);

	/* If we crash now, we have prepared: WAL replay will fix things */

	/* Store record's start location to read that later on Commit */
	gxact->prepare_start_lsn = ProcLastRecPtr;

	/*
	 * Mark the prepared transaction as valid.  As soon as xact.c marks
	 * MyProc as not running our XID (which it will do immediately after
	 * this function returns), others can commit/rollback the xact.
	 *
	 * NB: a side effect of this is to make a dummy ProcArray entry for the
	 * prepared XID.  This must happen before we clear the XID from MyProc /
	 * ProcGlobal->xids[], else there is a window where the XID is not running
	 * according to TransactionIdIsInProgress, and onlookers would be entitled
	 * to assume the xact crashed.  Instead we have a window where the same
	 * XID appears twice in ProcArray, which is OK.
	 */
	MarkAsPrepared(gxact, false);

	/*
	 * Now we can mark ourselves as out of the commit critical section: a
	 * checkpoint starting after this will certainly see the gxact as a
	 * candidate for fsyncing.
	 */
	MyProc->delayChkpt = false;

	/*
	 * Remember that we have this GlobalTransaction entry locked for us.  If
	 * we crash after this point, it's too late to abort, but we must unlock
	 * it so that the prepared transaction can be committed or rolled back.
	 */
	MyLockedGxact = gxact;

	END_CRIT_SECTION();

	/*
	 * Wait for synchronous replication, if required.
	 *
	 * Note that at this stage we have marked the prepare, but still show as
	 * running in the procarray (twice!) and continue to hold locks.
	 */
	SyncRepWaitForLSN(gxact->prepare_end_lsn, false);

	records.tail = records.head = NULL;
	records.num_chunks = 0;
}

/*
 * Register a 2PC record to be written to state file.
 */
void
RegisterTwoPhaseRecord(TwoPhaseRmgrId rmid, uint16 info,
					   const void *data, uint32 len)
{
	TwoPhaseRecordOnDisk record;

	record.rmid = rmid;
	record.info = info;
	record.len = len;
	save_state_data(&record, sizeof(TwoPhaseRecordOnDisk));
	if (len > 0)
		save_state_data(data, len);
}


/*
 * Read and validate the state file for xid.
 *
 * If it looks OK (has a valid magic number and CRC), return the palloc'd
 * contents of the file, issuing an error when finding corrupted data.  If
 * missing_ok is true, which indicates that missing files can be safely
 * ignored, then return NULL.  This state can be reached when doing recovery.
 */
static char *
ReadTwoPhaseFile(TransactionId xid, bool missing_ok)
{
	char		path[MAXPGPATH];
	char	   *buf;
	TwoPhaseFileHeader *hdr;
	int			fd;
	struct stat stat;
	uint32		crc_offset;
	pg_crc32c	calc_crc,
				file_crc;
	int			r;

	TwoPhaseFilePath(path, xid);

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (missing_ok && errno == ENOENT)
			return NULL;

		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	}

	/*
	 * Check file length.  We can determine a lower bound pretty easily. We
	 * set an upper bound to avoid palloc() failure on a corrupt file, though
	 * we can't guarantee that we won't get an out of memory error anyway,
	 * even on a valid file.
	 */
	if (fstat(fd, &stat))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));

	if (stat.st_size < (MAXALIGN(sizeof(TwoPhaseFileHeader)) +
						MAXALIGN(sizeof(TwoPhaseRecordOnDisk)) +
						sizeof(pg_crc32c)) ||
		stat.st_size > MaxAllocSize)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_plural("incorrect size of file \"%s\": %lld byte",
							   "incorrect size of file \"%s\": %lld bytes",
							   (long long int) stat.st_size, path,
							   (long long int) stat.st_size)));

	crc_offset = stat.st_size - sizeof(pg_crc32c);
	if (crc_offset != MAXALIGN(crc_offset))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("incorrect alignment of CRC offset for file \"%s\"",
						path)));

	/*
	 * OK, slurp in the file.
	 */
	buf = (char *) palloc(stat.st_size);

	pgstat_report_wait_start(WAIT_EVENT_TWOPHASE_FILE_READ);
	r = read(fd, buf, stat.st_size);
	if (r != stat.st_size)
	{
		if (r < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", path)));
		else
			ereport(ERROR,
					(errmsg("could not read file \"%s\": read %d of %lld",
							path, r, (long long int) stat.st_size)));
	}

	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	hdr = (TwoPhaseFileHeader *) buf;
	if (hdr->magic != TWOPHASE_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid magic number stored in file \"%s\"",
						path)));

	if (hdr->total_len != stat.st_size)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid size stored in file \"%s\"",
						path)));

	INIT_CRC32C(calc_crc);
	COMP_CRC32C(calc_crc, buf, crc_offset);
	FIN_CRC32C(calc_crc);

	file_crc = *((pg_crc32c *) (buf + crc_offset));

	if (!EQ_CRC32C(calc_crc, file_crc))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("calculated CRC checksum does not match value stored in file \"%s\"",
						path)));

	return buf;
}


/*
 * Reads 2PC data from xlog. During checkpoint this data will be moved to
 * twophase files and ReadTwoPhaseFile should be used instead.
 *
 * Note clearly that this function can access WAL during normal operation,
 * similarly to the way WALSender or Logical Decoding would do.
 */
static void
XlogReadTwoPhaseData(XLogRecPtr lsn, char **buf, int *len)
{
	XLogRecord *record;
	XLogReaderState *xlogreader;
	char	   *errormsg;

	xlogreader = XLogReaderAllocate(wal_segment_size, NULL,
									XL_ROUTINE(.page_read = &read_local_xlog_page,
											   .segment_open = &wal_segment_open,
											   .segment_close = &wal_segment_close),
									NULL);
	if (!xlogreader)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));

	XLogBeginRead(xlogreader, lsn);
	record = XLogReadRecord(xlogreader, &errormsg);
	if (record == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read two-phase state from WAL at %X/%X",
						(uint32) (lsn >> 32),
						(uint32) lsn)));

	if (XLogRecGetRmid(xlogreader) != RM_XACT_ID ||
		(XLogRecGetInfo(xlogreader) & XLOG_XACT_OPMASK) != XLOG_XACT_PREPARE)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("expected two-phase state data is not present in WAL at %X/%X",
						(uint32) (lsn >> 32),
						(uint32) lsn)));

	if (len != NULL)
		*len = XLogRecGetDataLen(xlogreader);

	*buf = palloc(sizeof(char) * XLogRecGetDataLen(xlogreader));
	memcpy(*buf, XLogRecGetData(xlogreader), sizeof(char) * XLogRecGetDataLen(xlogreader));

	XLogReaderFree(xlogreader);
}


/*
 * Confirms an xid is prepared, during recovery
 */
bool
StandbyTransactionIdIsPrepared(TransactionId xid)
{
	char	   *buf;
	TwoPhaseFileHeader *hdr;
	bool		result;

	Assert(TransactionIdIsValid(xid));

	if (max_prepared_xacts <= 0)
		return false;			/* nothing to do */

	/* Read and validate file */
	buf = ReadTwoPhaseFile(xid, true);
	if (buf == NULL)
		return false;

	/* Check header also */
	hdr = (TwoPhaseFileHeader *) buf;
	result = TransactionIdEquals(hdr->xid, xid);
	pfree(buf);

	return result;
}

/*
 * FinishPreparedTransaction: execute COMMIT PREPARED or ROLLBACK PREPARED
 */
void
FinishPreparedTransaction(const char *gid, bool isCommit)
{
	GlobalTransaction gxact;
	PGPROC	   *proc;
	TransactionId xid;
	char	   *buf;
	char	   *bufptr;
	TwoPhaseFileHeader *hdr;
	TransactionId latestXid;
	TransactionId *children;
	RelFileNode *commitrels;
	RelFileNode *abortrels;
	RelFileNode *delrels;
	int			ndelrels;
	SharedInvalidationMessage *invalmsgs;

	/*
	 * Validate the GID, and lock the GXACT to ensure that two backends do not
	 * try to commit the same GID at once.
	 */
	gxact = LockGXact(gid, GetUserId());
	proc = &ProcGlobal->allProcs[gxact->pgprocno];
	xid = gxact->xid;

	/*
	 * Read and validate 2PC state data. State data will typically be stored
	 * in WAL files if the LSN is after the last checkpoint record, or moved
	 * to disk if for some reason they have lived for a long time.
	 */
	if (gxact->ondisk)
		buf = ReadTwoPhaseFile(xid, false);
	else
		XlogReadTwoPhaseData(gxact->prepare_start_lsn, &buf, NULL);


	/*
	 * Disassemble the header area
	 */
	hdr = (TwoPhaseFileHeader *) buf;
	Assert(TransactionIdEquals(hdr->xid, xid));
	bufptr = buf + MAXALIGN(sizeof(TwoPhaseFileHeader));
	bufptr += MAXALIGN(hdr->gidlen);
	children = (TransactionId *) bufptr;
	bufptr += MAXALIGN(hdr->nsubxacts * sizeof(TransactionId));
	commitrels = (RelFileNode *) bufptr;
	bufptr += MAXALIGN(hdr->ncommitrels * sizeof(RelFileNode));
	abortrels = (RelFileNode *) bufptr;
	bufptr += MAXALIGN(hdr->nabortrels * sizeof(RelFileNode));
	invalmsgs = (SharedInvalidationMessage *) bufptr;
	bufptr += MAXALIGN(hdr->ninvalmsgs * sizeof(SharedInvalidationMessage));

	/* compute latestXid among all children */
	latestXid = TransactionIdLatest(xid, hdr->nsubxacts, children);

	/* Prevent cancel/die interrupt while cleaning up */
	HOLD_INTERRUPTS();

	/*
	 * The order of operations here is critical: make the XLOG entry for
	 * commit or abort, then mark the transaction committed or aborted in
	 * pg_xact, then remove its PGPROC from the global ProcArray (which means
	 * TransactionIdIsInProgress will stop saying the prepared xact is in
	 * progress), then run the post-commit or post-abort callbacks. The
	 * callbacks will release the locks the transaction held.
	 */
	if (isCommit)
		RecordTransactionCommitPrepared(xid,
										hdr->nsubxacts, children,
										hdr->ncommitrels, commitrels,
										hdr->ninvalmsgs, invalmsgs,
										hdr->initfileinval, gid);
	else
		RecordTransactionAbortPrepared(xid,
									   hdr->nsubxacts, children,
									   hdr->nabortrels, abortrels,
									   gid);

	ProcArrayRemove(proc, latestXid);

	/*
	 * In case we fail while running the callbacks, mark the gxact invalid so
	 * no one else will try to commit/rollback, and so it will be recycled if
	 * we fail after this point.  It is still locked by our backend so it
	 * won't go away yet.
	 *
	 * (We assume it's safe to do this without taking TwoPhaseStateLock.)
	 */
	gxact->valid = false;

	/*
	 * We have to remove any files that were supposed to be dropped. For
	 * consistency with the regular xact.c code paths, must do this before
	 * releasing locks, so do it before running the callbacks.
	 *
	 * NB: this code knows that we couldn't be dropping any temp rels ...
	 */
	if (isCommit)
	{
		delrels = commitrels;
		ndelrels = hdr->ncommitrels;
	}
	else
	{
		delrels = abortrels;
		ndelrels = hdr->nabortrels;
	}

	/* Make sure files supposed to be dropped are dropped */
	DropRelationFiles(delrels, ndelrels, false);

	/*
	 * Handle cache invalidation messages.
	 *
	 * Relcache init file invalidation requires processing both before and
	 * after we send the SI messages. See AtEOXact_Inval()
	 */
	if (hdr->initfileinval)
		RelationCacheInitFilePreInvalidate();
	SendSharedInvalidMessages(invalmsgs, hdr->ninvalmsgs);
	if (hdr->initfileinval)
		RelationCacheInitFilePostInvalidate();

	/*
	 * Acquire the two-phase lock.  We want to work on the two-phase callbacks
	 * while holding it to avoid potential conflicts with other transactions
	 * attempting to use the same GID, so the lock is released once the shared
	 * memory state is cleared.
	 */
	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);

	/* And now do the callbacks */
	if (isCommit)
		ProcessRecords(bufptr, xid, twophase_postcommit_callbacks);
	else
		ProcessRecords(bufptr, xid, twophase_postabort_callbacks);

	PredicateLockTwoPhaseFinish(xid, isCommit);

	/* Clear shared memory state */
	RemoveGXact(gxact);

	/*
	 * Release the lock as all callbacks are called and shared memory cleanup
	 * is done.
	 */
	LWLockRelease(TwoPhaseStateLock);

	/* Count the prepared xact as committed or aborted */
	AtEOXact_PgStat(isCommit, false);

	/*
	 * And now we can clean up any files we may have left.
	 */
	if (gxact->ondisk)
		RemoveTwoPhaseFile(xid, true);

	MyLockedGxact = NULL;

	RESUME_INTERRUPTS();

	pfree(buf);
}

/*
 * Scan 2PC state data in memory and call the indicated callbacks for each 2PC record.
 */
static void
ProcessRecords(char *bufptr, TransactionId xid,
			   const TwoPhaseCallback callbacks[])
{
	for (;;)
	{
		TwoPhaseRecordOnDisk *record = (TwoPhaseRecordOnDisk *) bufptr;

		Assert(record->rmid <= TWOPHASE_RM_MAX_ID);
		if (record->rmid == TWOPHASE_RM_END_ID)
			break;

		bufptr += MAXALIGN(sizeof(TwoPhaseRecordOnDisk));

		if (callbacks[record->rmid] != NULL)
			callbacks[record->rmid] (xid, record->info,
									 (void *) bufptr, record->len);

		bufptr += MAXALIGN(record->len);
	}
}

/*
 * Remove the 2PC file for the specified XID.
 *
 * If giveWarning is false, do not complain about file-not-present;
 * this is an expected case during WAL replay.
 */
static void
RemoveTwoPhaseFile(TransactionId xid, bool giveWarning)
{
	char		path[MAXPGPATH];

	TwoPhaseFilePath(path, xid);
	if (unlink(path))
		if (errno != ENOENT || giveWarning)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", path)));
}

/*
 * Recreates a state file. This is used in WAL replay and during
 * checkpoint creation.
 *
 * Note: content and len don't include CRC.
 */
static void
RecreateTwoPhaseFile(TransactionId xid, void *content, int len)
{
	char		path[MAXPGPATH];
	pg_crc32c	statefile_crc;
	int			fd;

	/* Recompute CRC */
	INIT_CRC32C(statefile_crc);
	COMP_CRC32C(statefile_crc, content, len);
	FIN_CRC32C(statefile_crc);

	TwoPhaseFilePath(path, xid);

	fd = OpenTransientFile(path,
						   O_CREAT | O_TRUNC | O_WRONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not recreate file \"%s\": %m", path)));

	/* Write content and CRC */
	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_TWOPHASE_FILE_WRITE);
	if (write(fd, content, len) != len)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));
	}
	if (write(fd, &statefile_crc, sizeof(pg_crc32c)) != sizeof(pg_crc32c))
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));
	}
	pgstat_report_wait_end();

	/*
	 * We must fsync the file because the end-of-replay checkpoint will not do
	 * so, there being no GXACT in shared memory yet to tell it to.
	 */
	pgstat_report_wait_start(WAIT_EVENT_TWOPHASE_FILE_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
}

/*
 * CheckPointTwoPhase -- handle 2PC component of checkpointing.
 *
 * We must fsync the state file of any GXACT that is valid or has been
 * generated during redo and has a PREPARE LSN <= the checkpoint's redo
 * horizon.  (If the gxact isn't valid yet, has not been generated in
 * redo, or has a later LSN, this checkpoint is not responsible for
 * fsyncing it.)
 *
 * This is deliberately run as late as possible in the checkpoint sequence,
 * because GXACTs ordinarily have short lifespans, and so it is quite
 * possible that GXACTs that were valid at checkpoint start will no longer
 * exist if we wait a little bit. With typical checkpoint settings this
 * will be about 3 minutes for an online checkpoint, so as a result we
 * expect that there will be no GXACTs that need to be copied to disk.
 *
 * If a GXACT remains valid across multiple checkpoints, it will already
 * be on disk so we don't bother to repeat that write.
 */
void
CheckPointTwoPhase(XLogRecPtr redo_horizon)
{
	int			i;
	int			serialized_xacts = 0;

	if (max_prepared_xacts <= 0)
		return;					/* nothing to do */

	TRACE_POSTGRESQL_TWOPHASE_CHECKPOINT_START();

	/*
	 * We are expecting there to be zero GXACTs that need to be copied to
	 * disk, so we perform all I/O while holding TwoPhaseStateLock for
	 * simplicity. This prevents any new xacts from preparing while this
	 * occurs, which shouldn't be a problem since the presence of long-lived
	 * prepared xacts indicates the transaction manager isn't active.
	 *
	 * It's also possible to move I/O out of the lock, but on every error we
	 * should check whether somebody committed our transaction in different
	 * backend. Let's leave this optimization for future, if somebody will
	 * spot that this place cause bottleneck.
	 *
	 * Note that it isn't possible for there to be a GXACT with a
	 * prepare_end_lsn set prior to the last checkpoint yet is marked invalid,
	 * because of the efforts with delayChkpt.
	 */
	LWLockAcquire(TwoPhaseStateLock, LW_SHARED);
	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		/*
		 * Note that we are using gxact not PGPROC so this works in recovery
		 * also
		 */
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];

		if ((gxact->valid || gxact->inredo) &&
			!gxact->ondisk &&
			gxact->prepare_end_lsn <= redo_horizon)
		{
			char	   *buf;
			int			len;

			XlogReadTwoPhaseData(gxact->prepare_start_lsn, &buf, &len);
			RecreateTwoPhaseFile(gxact->xid, buf, len);
			gxact->ondisk = true;
			gxact->prepare_start_lsn = InvalidXLogRecPtr;
			gxact->prepare_end_lsn = InvalidXLogRecPtr;
			pfree(buf);
			serialized_xacts++;
		}
	}
	LWLockRelease(TwoPhaseStateLock);

	/*
	 * Flush unconditionally the parent directory to make any information
	 * durable on disk.  Two-phase files could have been removed and those
	 * removals need to be made persistent as well as any files newly created
	 * previously since the last checkpoint.
	 */
	fsync_fname(TWOPHASE_DIR, true);

	TRACE_POSTGRESQL_TWOPHASE_CHECKPOINT_DONE();

	if (log_checkpoints && serialized_xacts > 0)
		ereport(LOG,
				(errmsg_plural("%u two-phase state file was written "
							   "for a long-running prepared transaction",
							   "%u two-phase state files were written "
							   "for long-running prepared transactions",
							   serialized_xacts,
							   serialized_xacts)));
}

/*
 * restoreTwoPhaseData
 *
 * Scan pg_twophase and fill TwoPhaseState depending on the on-disk data.
 * This is called once at the beginning of recovery, saving any extra
 * lookups in the future.  Two-phase files that are newer than the
 * minimum XID horizon are discarded on the way.
 */
void
restoreTwoPhaseData(void)
{
	DIR		   *cldir;
	struct dirent *clde;

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	cldir = AllocateDir(TWOPHASE_DIR);
	while ((clde = ReadDir(cldir, TWOPHASE_DIR)) != NULL)
	{
		if (strlen(clde->d_name) == 8 &&
			strspn(clde->d_name, "0123456789ABCDEF") == 8)
		{
			TransactionId xid;
			char	   *buf;

			xid = (TransactionId) strtoul(clde->d_name, NULL, 16);

			buf = ProcessTwoPhaseBuffer(xid, InvalidXLogRecPtr,
										true, false, false);
			if (buf == NULL)
				continue;

			PrepareRedoAdd(buf, InvalidXLogRecPtr,
						   InvalidXLogRecPtr, InvalidRepOriginId);
		}
	}
	LWLockRelease(TwoPhaseStateLock);
	FreeDir(cldir);
}

/*
 * PrescanPreparedTransactions
 *
 * Scan the shared memory entries of TwoPhaseState and determine the range
 * of valid XIDs present.  This is run during database startup, after we
 * have completed reading WAL.  ShmemVariableCache->nextXid has been set to
 * one more than the highest XID for which evidence exists in WAL.
 *
 * We throw away any prepared xacts with main XID beyond nextXid --- if any
 * are present, it suggests that the DBA has done a PITR recovery to an
 * earlier point in time without cleaning out pg_twophase.  We dare not
 * try to recover such prepared xacts since they likely depend on database
 * state that doesn't exist now.
 *
 * However, we will advance nextXid beyond any subxact XIDs belonging to
 * valid prepared xacts.  We need to do this since subxact commit doesn't
 * write a WAL entry, and so there might be no evidence in WAL of those
 * subxact XIDs.
 *
 * On corrupted two-phase files, fail immediately.  Keeping around broken
 * entries and let replay continue causes harm on the system, and a new
 * backup should be rolled in.
 *
 * Our other responsibility is to determine and return the oldest valid XID
 * among the prepared xacts (if none, return ShmemVariableCache->nextXid).
 * This is needed to synchronize pg_subtrans startup properly.
 *
 * If xids_p and nxids_p are not NULL, pointer to a palloc'd array of all
 * top-level xids is stored in *xids_p. The number of entries in the array
 * is returned in *nxids_p.
 */
TransactionId
PrescanPreparedTransactions(TransactionId **xids_p, int *nxids_p)
{
	FullTransactionId nextXid = ShmemVariableCache->nextXid;
	TransactionId origNextXid = XidFromFullTransactionId(nextXid);
	TransactionId result = origNextXid;
	TransactionId *xids = NULL;
	int			nxids = 0;
	int			allocsize = 0;
	int			i;

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		TransactionId xid;
		char	   *buf;
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];

		Assert(gxact->inredo);

		xid = gxact->xid;

		buf = ProcessTwoPhaseBuffer(xid,
									gxact->prepare_start_lsn,
									gxact->ondisk, false, true);

		if (buf == NULL)
			continue;

		/*
		 * OK, we think this file is valid.  Incorporate xid into the
		 * running-minimum result.
		 */
		if (TransactionIdPrecedes(xid, result))
			result = xid;

		if (xids_p)
		{
			if (nxids == allocsize)
			{
				if (nxids == 0)
				{
					allocsize = 10;
					xids = palloc(allocsize * sizeof(TransactionId));
				}
				else
				{
					allocsize = allocsize * 2;
					xids = repalloc(xids, allocsize * sizeof(TransactionId));
				}
			}
			xids[nxids++] = xid;
		}

		pfree(buf);
	}
	LWLockRelease(TwoPhaseStateLock);

	if (xids_p)
	{
		*xids_p = xids;
		*nxids_p = nxids;
	}

	return result;
}

/*
 * StandbyRecoverPreparedTransactions
 *
 * Scan the shared memory entries of TwoPhaseState and setup all the required
 * information to allow standby queries to treat prepared transactions as still
 * active.
 *
 * This is never called at the end of recovery - we use
 * RecoverPreparedTransactions() at that point.
 *
 * The lack of calls to SubTransSetParent() calls here is by design;
 * those calls are made by RecoverPreparedTransactions() at the end of recovery
 * for those xacts that need this.
 */
void
StandbyRecoverPreparedTransactions(void)
{
	int			i;

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		TransactionId xid;
		char	   *buf;
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];

		Assert(gxact->inredo);

		xid = gxact->xid;

		buf = ProcessTwoPhaseBuffer(xid,
									gxact->prepare_start_lsn,
									gxact->ondisk, false, false);
		if (buf != NULL)
			pfree(buf);
	}
	LWLockRelease(TwoPhaseStateLock);
}

/*
 * RecoverPreparedTransactions
 *
 * Scan the shared memory entries of TwoPhaseState and reload the state for
 * each prepared transaction (reacquire locks, etc).
 *
 * This is run at the end of recovery, but before we allow backends to write
 * WAL.
 *
 * At the end of recovery the way we take snapshots will change. We now need
 * to mark all running transactions with their full SubTransSetParent() info
 * to allow normal snapshots to work correctly if snapshots overflow.
 * We do this here because by definition prepared transactions are the only
 * type of write transaction still running, so this is necessary and
 * complete.
 */
void
RecoverPreparedTransactions(void)
{
	int			i;

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		TransactionId xid;
		char	   *buf;
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];
		char	   *bufptr;
		TwoPhaseFileHeader *hdr;
		TransactionId *subxids;
		const char *gid;

		xid = gxact->xid;

		/*
		 * Reconstruct subtrans state for the transaction --- needed because
		 * pg_subtrans is not preserved over a restart.  Note that we are
		 * linking all the subtransactions directly to the top-level XID;
		 * there may originally have been a more complex hierarchy, but
		 * there's no need to restore that exactly. It's possible that
		 * SubTransSetParent has been set before, if the prepared transaction
		 * generated xid assignment records.
		 */
		buf = ProcessTwoPhaseBuffer(xid,
									gxact->prepare_start_lsn,
									gxact->ondisk, true, false);
		if (buf == NULL)
			continue;

		ereport(LOG,
				(errmsg("recovering prepared transaction %u from shared memory", xid)));

		hdr = (TwoPhaseFileHeader *) buf;
		Assert(TransactionIdEquals(hdr->xid, xid));
		bufptr = buf + MAXALIGN(sizeof(TwoPhaseFileHeader));
		gid = (const char *) bufptr;
		bufptr += MAXALIGN(hdr->gidlen);
		subxids = (TransactionId *) bufptr;
		bufptr += MAXALIGN(hdr->nsubxacts * sizeof(TransactionId));
		bufptr += MAXALIGN(hdr->ncommitrels * sizeof(RelFileNode));
		bufptr += MAXALIGN(hdr->nabortrels * sizeof(RelFileNode));
		bufptr += MAXALIGN(hdr->ninvalmsgs * sizeof(SharedInvalidationMessage));

		/*
		 * Recreate its GXACT and dummy PGPROC. But, check whether it was
		 * added in redo and already has a shmem entry for it.
		 */
		MarkAsPreparingGuts(gxact, xid, gid,
							hdr->prepared_at,
							hdr->owner, hdr->database);

		/* recovered, so reset the flag for entries generated by redo */
		gxact->inredo = false;

		GXactLoadSubxactData(gxact, hdr->nsubxacts, subxids);
		MarkAsPrepared(gxact, true);

		LWLockRelease(TwoPhaseStateLock);

		/*
		 * Recover other state (notably locks) using resource managers.
		 */
		ProcessRecords(bufptr, xid, twophase_recover_callbacks);

		/*
		 * Release locks held by the standby process after we process each
		 * prepared transaction. As a result, we don't need too many
		 * additional locks at any one time.
		 */
		if (InHotStandby)
			StandbyReleaseLockTree(xid, hdr->nsubxacts, subxids);

		/*
		 * We're done with recovering this transaction. Clear MyLockedGxact,
		 * like we do in PrepareTransaction() during normal operation.
		 */
		PostPrepare_Twophase();

		pfree(buf);

		LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	}

	LWLockRelease(TwoPhaseStateLock);
}

/*
 * ProcessTwoPhaseBuffer
 *
 * Given a transaction id, read it either from disk or read it directly
 * via shmem xlog record pointer using the provided "prepare_start_lsn".
 *
 * If setParent is true, set up subtransaction parent linkages.
 *
 * If setNextXid is true, set ShmemVariableCache->nextXid to the newest
 * value scanned.
 */
static char *
ProcessTwoPhaseBuffer(TransactionId xid,
					  XLogRecPtr prepare_start_lsn,
					  bool fromdisk,
					  bool setParent, bool setNextXid)
{
	FullTransactionId nextXid = ShmemVariableCache->nextXid;
	TransactionId origNextXid = XidFromFullTransactionId(nextXid);
	TransactionId *subxids;
	char	   *buf;
	TwoPhaseFileHeader *hdr;
	int			i;

	Assert(LWLockHeldByMeInMode(TwoPhaseStateLock, LW_EXCLUSIVE));

	if (!fromdisk)
		Assert(prepare_start_lsn != InvalidXLogRecPtr);

	/* Already processed? */
	if (TransactionIdDidCommit(xid) || TransactionIdDidAbort(xid))
	{
		if (fromdisk)
		{
			ereport(WARNING,
					(errmsg("removing stale two-phase state file for transaction %u",
							xid)));
			RemoveTwoPhaseFile(xid, true);
		}
		else
		{
			ereport(WARNING,
					(errmsg("removing stale two-phase state from memory for transaction %u",
							xid)));
			PrepareRedoRemove(xid, true);
		}
		return NULL;
	}

	/* Reject XID if too new */
	if (TransactionIdFollowsOrEquals(xid, origNextXid))
	{
		if (fromdisk)
		{
			ereport(WARNING,
					(errmsg("removing future two-phase state file for transaction %u",
							xid)));
			RemoveTwoPhaseFile(xid, true);
		}
		else
		{
			ereport(WARNING,
					(errmsg("removing future two-phase state from memory for transaction %u",
							xid)));
			PrepareRedoRemove(xid, true);
		}
		return NULL;
	}

	if (fromdisk)
	{
		/* Read and validate file */
		buf = ReadTwoPhaseFile(xid, false);
	}
	else
	{
		/* Read xlog data */
		XlogReadTwoPhaseData(prepare_start_lsn, &buf, NULL);
	}

	/* Deconstruct header */
	hdr = (TwoPhaseFileHeader *) buf;
	if (!TransactionIdEquals(hdr->xid, xid))
	{
		if (fromdisk)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupted two-phase state file for transaction %u",
							xid)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupted two-phase state in memory for transaction %u",
							xid)));
	}

	/*
	 * Examine subtransaction XIDs ... they should all follow main XID, and
	 * they may force us to advance nextXid.
	 */
	subxids = (TransactionId *) (buf +
								 MAXALIGN(sizeof(TwoPhaseFileHeader)) +
								 MAXALIGN(hdr->gidlen));
	for (i = 0; i < hdr->nsubxacts; i++)
	{
		TransactionId subxid = subxids[i];

		Assert(TransactionIdFollows(subxid, xid));

		/* update nextXid if needed */
		if (setNextXid)
			AdvanceNextFullTransactionIdPastXid(subxid);

		if (setParent)
			SubTransSetParent(subxid, xid);
	}

	return buf;
}


/*
 *	RecordTransactionCommitPrepared
 *
 * This is basically the same as RecordTransactionCommit (q.v. if you change
 * this function): in particular, we must set the delayChkpt flag to avoid a
 * race condition.
 *
 * We know the transaction made at least one XLOG entry (its PREPARE),
 * so it is never possible to optimize out the commit record.
 */
static void
RecordTransactionCommitPrepared(TransactionId xid,
								int nchildren,
								TransactionId *children,
								int nrels,
								RelFileNode *rels,
								int ninvalmsgs,
								SharedInvalidationMessage *invalmsgs,
								bool initfileinval,
								const char *gid)
{
	XLogRecPtr	recptr;
	TimestampTz committs = GetCurrentTimestamp();
	bool		replorigin;

	/*
	 * Are we using the replication origins feature?  Or, in other words, are
	 * we replaying remote actions?
	 */
	replorigin = (replorigin_session_origin != InvalidRepOriginId &&
				  replorigin_session_origin != DoNotReplicateId);

	START_CRIT_SECTION();

	/* See notes in RecordTransactionCommit */
	MyProc->delayChkpt = true;

	/*
	 * Emit the XLOG commit record. Note that we mark 2PC commits as
	 * potentially having AccessExclusiveLocks since we don't know whether or
	 * not they do.
	 */
	recptr = XactLogCommitRecord(committs,
								 nchildren, children, nrels, rels,
								 ninvalmsgs, invalmsgs,
								 initfileinval,
								 MyXactFlags | XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK,
								 xid, gid);


	if (replorigin)
		/* Move LSNs forward for this replication origin */
		replorigin_session_advance(replorigin_session_origin_lsn,
								   XactLastRecEnd);

	/*
	 * Record commit timestamp.  The value comes from plain commit timestamp
	 * if replorigin is not enabled, or replorigin already set a value for us
	 * in replorigin_session_origin_timestamp otherwise.
	 *
	 * We don't need to WAL-log anything here, as the commit record written
	 * above already contains the data.
	 */
	if (!replorigin || replorigin_session_origin_timestamp == 0)
		replorigin_session_origin_timestamp = committs;

	TransactionTreeSetCommitTsData(xid, nchildren, children,
								   replorigin_session_origin_timestamp,
								   replorigin_session_origin, false);

	/*
	 * We don't currently try to sleep before flush here ... nor is there any
	 * support for async commit of a prepared xact (the very idea is probably
	 * a contradiction)
	 */

	/* Flush XLOG to disk */
	XLogFlush(recptr);

	/* Mark the transaction committed in pg_xact */
	TransactionIdCommitTree(xid, nchildren, children);

	/* Checkpoint can proceed now */
	MyProc->delayChkpt = false;

	END_CRIT_SECTION();

	/*
	 * Wait for synchronous replication, if required.
	 *
	 * Note that at this stage we have marked clog, but still show as running
	 * in the procarray and continue to hold locks.
	 */
	SyncRepWaitForLSN(recptr, true);
}

/*
 *	RecordTransactionAbortPrepared
 *
 * This is basically the same as RecordTransactionAbort.
 *
 * We know the transaction made at least one XLOG entry (its PREPARE),
 * so it is never possible to optimize out the abort record.
 */
static void
RecordTransactionAbortPrepared(TransactionId xid,
							   int nchildren,
							   TransactionId *children,
							   int nrels,
							   RelFileNode *rels,
							   const char *gid)
{
	XLogRecPtr	recptr;

	/*
	 * Catch the scenario where we aborted partway through
	 * RecordTransactionCommitPrepared ...
	 */
	if (TransactionIdDidCommit(xid))
		elog(PANIC, "cannot abort transaction %u, it was already committed",
			 xid);

	START_CRIT_SECTION();

	/*
	 * Emit the XLOG commit record. Note that we mark 2PC aborts as
	 * potentially having AccessExclusiveLocks since we don't know whether or
	 * not they do.
	 */
	recptr = XactLogAbortRecord(GetCurrentTimestamp(),
								nchildren, children,
								nrels, rels,
								MyXactFlags | XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK,
								xid, gid);

	/* Always flush, since we're about to remove the 2PC state file */
	XLogFlush(recptr);

	/*
	 * Mark the transaction aborted in clog.  This is not absolutely necessary
	 * but we may as well do it while we are here.
	 */
	TransactionIdAbortTree(xid, nchildren, children);

	END_CRIT_SECTION();

	/*
	 * Wait for synchronous replication, if required.
	 *
	 * Note that at this stage we have marked clog, but still show as running
	 * in the procarray and continue to hold locks.
	 */
	SyncRepWaitForLSN(recptr, false);
}

/*
 * PrepareRedoAdd
 *
 * Store pointers to the start/end of the WAL record along with the xid in
 * a gxact entry in shared memory TwoPhaseState structure.  If caller
 * specifies InvalidXLogRecPtr as WAL location to fetch the two-phase
 * data, the entry is marked as located on disk.
 */
void
PrepareRedoAdd(char *buf, XLogRecPtr start_lsn,
			   XLogRecPtr end_lsn, RepOriginId origin_id)
{
	TwoPhaseFileHeader *hdr = (TwoPhaseFileHeader *) buf;
	char	   *bufptr;
	const char *gid;
	GlobalTransaction gxact;

	Assert(LWLockHeldByMeInMode(TwoPhaseStateLock, LW_EXCLUSIVE));
	Assert(RecoveryInProgress());

	bufptr = buf + MAXALIGN(sizeof(TwoPhaseFileHeader));
	gid = (const char *) bufptr;

	/*
	 * Reserve the GID for the given transaction in the redo code path.
	 *
	 * This creates a gxact struct and puts it into the active array.
	 *
	 * In redo, this struct is mainly used to track PREPARE/COMMIT entries in
	 * shared memory. Hence, we only fill up the bare minimum contents here.
	 * The gxact also gets marked with gxact->inredo set to true to indicate
	 * that it got added in the redo phase
	 */

	/* Get a free gxact from the freelist */
	if (TwoPhaseState->freeGXacts == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("maximum number of prepared transactions reached"),
				 errhint("Increase max_prepared_transactions (currently %d).",
						 max_prepared_xacts)));
	gxact = TwoPhaseState->freeGXacts;
	TwoPhaseState->freeGXacts = gxact->next;

	gxact->prepared_at = hdr->prepared_at;
	gxact->prepare_start_lsn = start_lsn;
	gxact->prepare_end_lsn = end_lsn;
	gxact->xid = hdr->xid;
	gxact->owner = hdr->owner;
	gxact->locking_backend = InvalidBackendId;
	gxact->valid = false;
	gxact->ondisk = XLogRecPtrIsInvalid(start_lsn);
	gxact->inredo = true;		/* yes, added in redo */
	strcpy(gxact->gid, gid);

	/* And insert it into the active array */
	Assert(TwoPhaseState->numPrepXacts < max_prepared_xacts);
	TwoPhaseState->prepXacts[TwoPhaseState->numPrepXacts++] = gxact;

	if (origin_id != InvalidRepOriginId)
	{
		/* recover apply progress */
		replorigin_advance(origin_id, hdr->origin_lsn, end_lsn,
						   false /* backward */ , false /* WAL */ );
	}

	elog(DEBUG2, "added 2PC data in shared memory for transaction %u", gxact->xid);
}

/*
 * PrepareRedoRemove
 *
 * Remove the corresponding gxact entry from TwoPhaseState. Also remove
 * the 2PC file if a prepared transaction was saved via an earlier checkpoint.
 *
 * Caller must hold TwoPhaseStateLock in exclusive mode, because TwoPhaseState
 * is updated.
 */
void
PrepareRedoRemove(TransactionId xid, bool giveWarning)
{
	GlobalTransaction gxact = NULL;
	int			i;
	bool		found = false;

	Assert(LWLockHeldByMeInMode(TwoPhaseStateLock, LW_EXCLUSIVE));
	Assert(RecoveryInProgress());

	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		gxact = TwoPhaseState->prepXacts[i];

		if (gxact->xid == xid)
		{
			Assert(gxact->inredo);
			found = true;
			break;
		}
	}

	/*
	 * Just leave if there is nothing, this is expected during WAL replay.
	 */
	if (!found)
		return;

	/*
	 * And now we can clean up any files we may have left.
	 */
	elog(DEBUG2, "removing 2PC data for transaction %u", xid);
	if (gxact->ondisk)
		RemoveTwoPhaseFile(xid, giveWarning);
	RemoveGXact(gxact);
}
