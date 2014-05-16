/*-------------------------------------------------------------------------
 *
 * twophase.c
 *		Two-phase commit support functions.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
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
 *		A global transaction (gxact) also has dummy PGXACT and PGPROC; this is
 *		what keeps the XID considered running by TransactionIdIsInProgress.
 *		It is also convenient as a PGPROC to hook the gxact's locks to.
 *
 *		In order to survive crashes and shutdowns, all prepared
 *		transactions must be stored in permanent storage. This includes
 *		locking information, pending notifications etc. All that state
 *		information is written to the per-transaction state file in
 *		the pg_twophase directory.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "replication/walsender.h"
#include "replication/syncrep.h"
#include "storage/fd.h"
#include "storage/ipc.h"
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
#define GIDSIZE 200

typedef struct GlobalTransactionData
{
	GlobalTransaction next;		/* list link for free list */
	int			pgprocno;		/* ID of associated dummy PGPROC */
	BackendId	dummyBackendId; /* similar to backend id for backends */
	TimestampTz prepared_at;	/* time of preparation */
	XLogRecPtr	prepare_lsn;	/* XLOG offset of prepare record */
	Oid			owner;			/* ID of user that executed the xact */
	BackendId	locking_backend; /* backend currently working on the xact */
	bool		valid;			/* TRUE if PGPROC entry is in proc array */
	char		gid[GIDSIZE];	/* The GID assigned to the prepared xact */
}	GlobalTransactionData;

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

	/*
	 * There are max_prepared_xacts items in this array, but C wants a
	 * fixed-size array.
	 */
	GlobalTransaction prepXacts[1];		/* VARIABLE LENGTH ARRAY */
} TwoPhaseStateData;			/* VARIABLE LENGTH STRUCT */

static TwoPhaseStateData *TwoPhaseState;

/*
 * Global transaction entry currently locked by us, if any.
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
								bool initfileinval);
static void RecordTransactionAbortPrepared(TransactionId xid,
							   int nchildren,
							   TransactionId *children,
							   int nrels,
							   RelFileNode *rels);
static void ProcessRecords(char *bufptr, TransactionId xid,
			   const TwoPhaseCallback callbacks[]);
static void RemoveGXact(GlobalTransaction gxact);


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
	 * What to do with the locked global transaction entry?  If we were in
	 * the process of preparing the transaction, but haven't written the WAL
	 * record and state file yet, the transaction must not be considered as
	 * prepared.  Likewise, if we are in the process of finishing an
	 * already-prepared transaction, and fail after having already written
	 * the 2nd phase commit or rollback record to the WAL, the transaction
	 * should not be considered as prepared anymore.  In those cases, just
	 * remove the entry from shared memory.
	 *
	 * Otherwise, the entry must be left in place so that the transaction
	 * can be finished later, so just unlock it.
	 *
	 * If we abort during prepare, after having written the WAL record, we
	 * might not have transfered all locks and other state to the prepared
	 * transaction yet.  Likewise, if we abort during commit or rollback,
	 * after having written the WAL record, we might not have released
	 * all the resources held by the transaction yet.  In those cases, the
	 * in-memory state can be wrong, but it's too late to back out.
	 */
	if (!MyLockedGxact->valid)
	{
		RemoveGXact(MyLockedGxact);
	}
	else
	{
		LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);

		MyLockedGxact->locking_backend = InvalidBackendId;

		LWLockRelease(TwoPhaseStateLock);
	}
	MyLockedGxact = NULL;
}

/*
 * This is called after we have finished transfering state to the prepared
 * PGXACT entry.
 */
void
PostPrepare_Twophase()
{
	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	MyLockedGxact->locking_backend = InvalidBackendId;
	LWLockRelease(TwoPhaseStateLock);

	MyLockedGxact = NULL;
}


/*
 * MarkAsPreparing
 *		Reserve the GID for the given transaction.
 *
 * Internally, this creates a gxact struct and puts it into the active array.
 * NOTE: this is also used when reloading a gxact after a crash; so avoid
 * assuming that we can use very much backend context.
 */
GlobalTransaction
MarkAsPreparing(TransactionId xid, const char *gid,
				TimestampTz prepared_at, Oid owner, Oid databaseid)
{
	GlobalTransaction gxact;
	PGPROC	   *proc;
	PGXACT	   *pgxact;
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

	proc = &ProcGlobal->allProcs[gxact->pgprocno];
	pgxact = &ProcGlobal->allPgXact[gxact->pgprocno];

	/* Initialize the PGPROC entry */
	MemSet(proc, 0, sizeof(PGPROC));
	proc->pgprocno = gxact->pgprocno;
	SHMQueueElemInit(&(proc->links));
	proc->waitStatus = STATUS_OK;
	/* We set up the gxact's VXID as InvalidBackendId/XID */
	proc->lxid = (LocalTransactionId) xid;
	pgxact->xid = xid;
	pgxact->xmin = InvalidTransactionId;
	pgxact->delayChkpt = false;
	pgxact->vacuumFlags = 0;
	proc->pid = 0;
	proc->backendId = InvalidBackendId;
	proc->databaseId = databaseid;
	proc->roleId = owner;
	proc->lwWaiting = false;
	proc->lwWaitMode = 0;
	proc->lwWaitLink = NULL;
	proc->waitLock = NULL;
	proc->waitProcLock = NULL;
	for (i = 0; i < NUM_LOCK_PARTITIONS; i++)
		SHMQueueInit(&(proc->myProcLocks[i]));
	/* subxid data must be filled later by GXactLoadSubxactData */
	pgxact->overflowed = false;
	pgxact->nxids = 0;

	gxact->prepared_at = prepared_at;
	/* initialize LSN to 0 (start of WAL) */
	gxact->prepare_lsn = 0;
	gxact->owner = owner;
	gxact->locking_backend = MyBackendId;
	gxact->valid = false;
	strcpy(gxact->gid, gid);

	/* And insert it into the active array */
	Assert(TwoPhaseState->numPrepXacts < max_prepared_xacts);
	TwoPhaseState->prepXacts[TwoPhaseState->numPrepXacts++] = gxact;

	/*
	 * Remember that we have this GlobalTransaction entry locked for us.
	 * If we abort after this, we must release it.
	 */
	MyLockedGxact = gxact;

	LWLockRelease(TwoPhaseStateLock);

	return gxact;
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
	PGXACT	   *pgxact = &ProcGlobal->allPgXact[gxact->pgprocno];

	/* We need no extra lock since the GXACT isn't valid yet */
	if (nsubxacts > PGPROC_MAX_CACHED_SUBXIDS)
	{
		pgxact->overflowed = true;
		nsubxacts = PGPROC_MAX_CACHED_SUBXIDS;
	}
	if (nsubxacts > 0)
	{
		memcpy(proc->subxids.xids, children,
			   nsubxacts * sizeof(TransactionId));
		pgxact->nxids = nsubxacts;
	}
}

/*
 * MarkAsPrepared
 *		Mark the GXACT as fully valid, and enter it into the global ProcArray.
 */
static void
MarkAsPrepared(GlobalTransaction gxact)
{
	/* Lock here may be overkill, but I'm not convinced of that ... */
	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);
	Assert(!gxact->valid);
	gxact->valid = true;
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

	LWLockAcquire(TwoPhaseStateLock, LW_EXCLUSIVE);

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

			LWLockRelease(TwoPhaseStateLock);

			return;
		}
	}

	LWLockRelease(TwoPhaseStateLock);

	elog(ERROR, "failed to find %p in GlobalTransaction array", gxact);
}

/*
 * TransactionIdIsPrepared
 *		True iff transaction associated with the identifier is prepared
 *		for two-phase commit
 *
 * Note: only gxacts marked "valid" are considered; but notice we do not
 * check the locking status.
 *
 * This is not currently exported, because it is only needed internally.
 */
static bool
TransactionIdIsPrepared(TransactionId xid)
{
	bool		result = false;
	int			i;

	LWLockAcquire(TwoPhaseStateLock, LW_SHARED);

	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];
		PGXACT	   *pgxact = &ProcGlobal->allPgXact[gxact->pgprocno];

		if (gxact->valid && pgxact->xid == xid)
		{
			result = true;
			break;
		}
	}

	LWLockRelease(TwoPhaseStateLock);

	return result;
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
		tupdesc = CreateTemplateTupleDesc(5, false);
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
		PGXACT	   *pgxact = &ProcGlobal->allPgXact[gxact->pgprocno];
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

		values[0] = TransactionIdGetDatum(pgxact->xid);
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
 */
static GlobalTransaction
TwoPhaseGetGXact(TransactionId xid)
{
	GlobalTransaction result = NULL;
	int			i;

	static TransactionId cached_xid = InvalidTransactionId;
	static GlobalTransaction cached_gxact = NULL;

	/*
	 * During a recovery, COMMIT PREPARED, or ABORT PREPARED, we'll be called
	 * repeatedly for the same XID.  We can save work with a simple cache.
	 */
	if (xid == cached_xid)
		return cached_gxact;

	LWLockAcquire(TwoPhaseStateLock, LW_SHARED);

	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];
		PGXACT	   *pgxact = &ProcGlobal->allPgXact[gxact->pgprocno];

		if (pgxact->xid == xid)
		{
			result = gxact;
			break;
		}
	}

	LWLockRelease(TwoPhaseStateLock);

	if (result == NULL)			/* should not happen */
		elog(ERROR, "failed to find GlobalTransaction for xid %u", xid);

	cached_xid = xid;
	cached_gxact = result;

	return result;
}

/*
 * TwoPhaseGetDummyProc
 *		Get the dummy backend ID for prepared transaction specified by XID
 *
 * Dummy backend IDs are similar to real backend IDs of real backends.
 * They start at MaxBackends + 1, and are unique across all currently active
 * real backends and prepared transactions.
 */
BackendId
TwoPhaseGetDummyBackendId(TransactionId xid)
{
	GlobalTransaction gxact = TwoPhaseGetGXact(xid);

	return gxact->dummyBackendId;
}

/*
 * TwoPhaseGetDummyProc
 *		Get the PGPROC that represents a prepared transaction specified by XID
 */
PGPROC *
TwoPhaseGetDummyProc(TransactionId xid)
{
	GlobalTransaction gxact = TwoPhaseGetGXact(xid);

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
 *	9. CRC32
 *
 * Each segment except the final CRC32 is MAXALIGN'd.
 */

/*
 * Header for a 2PC state file
 */
#define TWOPHASE_MAGIC	0x57F94532		/* format identifier */

typedef struct TwoPhaseFileHeader
{
	uint32		magic;			/* format identifier */
	uint32		total_len;		/* actual file length */
	TransactionId xid;			/* original transaction XID */
	Oid			database;		/* OID of database it was in */
	TimestampTz prepared_at;	/* time of preparation */
	Oid			owner;			/* user running the transaction */
	int32		nsubxacts;		/* number of following subxact XIDs */
	int32		ncommitrels;	/* number of delete-on-commit rels */
	int32		nabortrels;		/* number of delete-on-abort rels */
	int32		ninvalmsgs;		/* number of cache invalidation messages */
	bool		initfileinval;	/* does relcache init file need invalidation? */
	char		gid[GIDSIZE];	/* GID for transaction */
} TwoPhaseFileHeader;

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
 * to WAL and the actual state file.  We use a chain of XLogRecData blocks
 * so that we will be able to pass the state file contents directly to
 * XLogInsert.
 */
static struct xllist
{
	XLogRecData *head;			/* first data block in the chain */
	XLogRecData *tail;			/* last block in chain */
	uint32		bytes_free;		/* free bytes left in tail block */
	uint32		total_len;		/* total data bytes in chain */
}	records;


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
		records.tail->next = palloc0(sizeof(XLogRecData));
		records.tail = records.tail->next;
		records.tail->buffer = InvalidBuffer;
		records.tail->len = 0;
		records.tail->next = NULL;

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
	PGXACT	   *pgxact = &ProcGlobal->allPgXact[gxact->pgprocno];
	TransactionId xid = pgxact->xid;
	TwoPhaseFileHeader hdr;
	TransactionId *children;
	RelFileNode *commitrels;
	RelFileNode *abortrels;
	SharedInvalidationMessage *invalmsgs;

	/* Initialize linked list */
	records.head = palloc0(sizeof(XLogRecData));
	records.head->buffer = InvalidBuffer;
	records.head->len = 0;
	records.head->next = NULL;

	records.bytes_free = Max(sizeof(TwoPhaseFileHeader), 512);
	records.head->data = palloc(records.bytes_free);

	records.tail = records.head;

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
	StrNCpy(hdr.gid, gxact->gid, GIDSIZE);

	save_state_data(&hdr, sizeof(TwoPhaseFileHeader));

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
 * Finish preparing state file.
 *
 * Calculates CRC and writes state file to WAL and in pg_twophase directory.
 */
void
EndPrepare(GlobalTransaction gxact)
{
	PGXACT	   *pgxact = &ProcGlobal->allPgXact[gxact->pgprocno];
	TransactionId xid = pgxact->xid;
	TwoPhaseFileHeader *hdr;
	char		path[MAXPGPATH];
	XLogRecData *record;
	pg_crc32	statefile_crc;
	pg_crc32	bogus_crc;
	int			fd;

	/* Add the end sentinel to the list of 2PC records */
	RegisterTwoPhaseRecord(TWOPHASE_RM_END_ID, 0,
						   NULL, 0);

	/* Go back and fill in total_len in the file header record */
	hdr = (TwoPhaseFileHeader *) records.head->data;
	Assert(hdr->magic == TWOPHASE_MAGIC);
	hdr->total_len = records.total_len + sizeof(pg_crc32);

	/*
	 * If the file size exceeds MaxAllocSize, we won't be able to read it in
	 * ReadTwoPhaseFile. Check for that now, rather than fail at commit time.
	 */
	if (hdr->total_len > MaxAllocSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("two-phase state file maximum length exceeded")));

	/*
	 * Create the 2PC state file.
	 */
	TwoPhaseFilePath(path, xid);

	fd = OpenTransientFile(path,
						   O_CREAT | O_EXCL | O_WRONLY | PG_BINARY,
						   S_IRUSR | S_IWUSR);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create two-phase state file \"%s\": %m",
						path)));

	/* Write data to file, and calculate CRC as we pass over it */
	INIT_CRC32(statefile_crc);

	for (record = records.head; record != NULL; record = record->next)
	{
		COMP_CRC32(statefile_crc, record->data, record->len);
		if ((write(fd, record->data, record->len)) != record->len)
		{
			CloseTransientFile(fd);
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write two-phase state file: %m")));
		}
	}

	FIN_CRC32(statefile_crc);

	/*
	 * Write a deliberately bogus CRC to the state file; this is just paranoia
	 * to catch the case where four more bytes will run us out of disk space.
	 */
	bogus_crc = ~statefile_crc;

	if ((write(fd, &bogus_crc, sizeof(pg_crc32))) != sizeof(pg_crc32))
	{
		CloseTransientFile(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write two-phase state file: %m")));
	}

	/* Back up to prepare for rewriting the CRC */
	if (lseek(fd, -((off_t) sizeof(pg_crc32)), SEEK_CUR) < 0)
	{
		CloseTransientFile(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in two-phase state file: %m")));
	}

	/*
	 * The state file isn't valid yet, because we haven't written the correct
	 * CRC yet.  Before we do that, insert entry in WAL and flush it to disk.
	 *
	 * Between the time we have written the WAL entry and the time we write
	 * out the correct state file CRC, we have an inconsistency: the xact is
	 * prepared according to WAL but not according to our on-disk state. We
	 * use a critical section to force a PANIC if we are unable to complete
	 * the write --- then, WAL replay should repair the inconsistency.  The
	 * odds of a PANIC actually occurring should be very tiny given that we
	 * were able to write the bogus CRC above.
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
	START_CRIT_SECTION();

	MyPgXact->delayChkpt = true;

	gxact->prepare_lsn = XLogInsert(RM_XACT_ID, XLOG_XACT_PREPARE,
									records.head);
	XLogFlush(gxact->prepare_lsn);

	/* If we crash now, we have prepared: WAL replay will fix things */

	/* write correct CRC and close file */
	if ((write(fd, &statefile_crc, sizeof(pg_crc32))) != sizeof(pg_crc32))
	{
		CloseTransientFile(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write two-phase state file: %m")));
	}

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close two-phase state file: %m")));

	/*
	 * Mark the prepared transaction as valid.  As soon as xact.c marks
	 * MyPgXact as not running our XID (which it will do immediately after
	 * this function returns), others can commit/rollback the xact.
	 *
	 * NB: a side effect of this is to make a dummy ProcArray entry for the
	 * prepared XID.  This must happen before we clear the XID from MyPgXact,
	 * else there is a window where the XID is not running according to
	 * TransactionIdIsInProgress, and onlookers would be entitled to assume
	 * the xact crashed.  Instead we have a window where the same XID appears
	 * twice in ProcArray, which is OK.
	 */
	MarkAsPrepared(gxact);

	/*
	 * Now we can mark ourselves as out of the commit critical section: a
	 * checkpoint starting after this will certainly see the gxact as a
	 * candidate for fsyncing.
	 */
	MyPgXact->delayChkpt = false;

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
	SyncRepWaitForLSN(gxact->prepare_lsn);

	records.tail = records.head = NULL;
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
 * contents of the file.  Otherwise return NULL.
 */
static char *
ReadTwoPhaseFile(TransactionId xid, bool give_warnings)
{
	char		path[MAXPGPATH];
	char	   *buf;
	TwoPhaseFileHeader *hdr;
	int			fd;
	struct stat stat;
	uint32		crc_offset;
	pg_crc32	calc_crc,
				file_crc;

	TwoPhaseFilePath(path, xid);

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY, 0);
	if (fd < 0)
	{
		if (give_warnings)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not open two-phase state file \"%s\": %m",
							path)));
		return NULL;
	}

	/*
	 * Check file length.  We can determine a lower bound pretty easily. We
	 * set an upper bound to avoid palloc() failure on a corrupt file, though
	 * we can't guarantee that we won't get an out of memory error anyway,
	 * even on a valid file.
	 */
	if (fstat(fd, &stat))
	{
		CloseTransientFile(fd);
		if (give_warnings)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not stat two-phase state file \"%s\": %m",
							path)));
		return NULL;
	}

	if (stat.st_size < (MAXALIGN(sizeof(TwoPhaseFileHeader)) +
						MAXALIGN(sizeof(TwoPhaseRecordOnDisk)) +
						sizeof(pg_crc32)) ||
		stat.st_size > MaxAllocSize)
	{
		CloseTransientFile(fd);
		return NULL;
	}

	crc_offset = stat.st_size - sizeof(pg_crc32);
	if (crc_offset != MAXALIGN(crc_offset))
	{
		CloseTransientFile(fd);
		return NULL;
	}

	/*
	 * OK, slurp in the file.
	 */
	buf = (char *) palloc(stat.st_size);

	if (read(fd, buf, stat.st_size) != stat.st_size)
	{
		CloseTransientFile(fd);
		if (give_warnings)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not read two-phase state file \"%s\": %m",
							path)));
		pfree(buf);
		return NULL;
	}

	CloseTransientFile(fd);

	hdr = (TwoPhaseFileHeader *) buf;
	if (hdr->magic != TWOPHASE_MAGIC || hdr->total_len != stat.st_size)
	{
		pfree(buf);
		return NULL;
	}

	INIT_CRC32(calc_crc);
	COMP_CRC32(calc_crc, buf, crc_offset);
	FIN_CRC32(calc_crc);

	file_crc = *((pg_crc32 *) (buf + crc_offset));

	if (!EQ_CRC32(calc_crc, file_crc))
	{
		pfree(buf);
		return NULL;
	}

	return buf;
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
	buf = ReadTwoPhaseFile(xid, false);
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
	PGXACT	   *pgxact;
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
	int			i;

	/*
	 * Validate the GID, and lock the GXACT to ensure that two backends do not
	 * try to commit the same GID at once.
	 */
	gxact = LockGXact(gid, GetUserId());
	proc = &ProcGlobal->allProcs[gxact->pgprocno];
	pgxact = &ProcGlobal->allPgXact[gxact->pgprocno];
	xid = pgxact->xid;

	/*
	 * Read and validate the state file
	 */
	buf = ReadTwoPhaseFile(xid, true);
	if (buf == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("two-phase state file for transaction %u is corrupt",
						xid)));

	/*
	 * Disassemble the header area
	 */
	hdr = (TwoPhaseFileHeader *) buf;
	Assert(TransactionIdEquals(hdr->xid, xid));
	bufptr = buf + MAXALIGN(sizeof(TwoPhaseFileHeader));
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

	/*
	 * The order of operations here is critical: make the XLOG entry for
	 * commit or abort, then mark the transaction committed or aborted in
	 * pg_clog, then remove its PGPROC from the global ProcArray (which means
	 * TransactionIdIsInProgress will stop saying the prepared xact is in
	 * progress), then run the post-commit or post-abort callbacks. The
	 * callbacks will release the locks the transaction held.
	 */
	if (isCommit)
		RecordTransactionCommitPrepared(xid,
										hdr->nsubxacts, children,
										hdr->ncommitrels, commitrels,
										hdr->ninvalmsgs, invalmsgs,
										hdr->initfileinval);
	else
		RecordTransactionAbortPrepared(xid,
									   hdr->nsubxacts, children,
									   hdr->nabortrels, abortrels);

	ProcArrayRemove(proc, latestXid);

	/*
	 * In case we fail while running the callbacks, mark the gxact invalid so
	 * no one else will try to commit/rollback, and so it will be recycled
	 * if we fail after this point.  It is still locked by our backend so it
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
	for (i = 0; i < ndelrels; i++)
	{
		SMgrRelation srel = smgropen(delrels[i], InvalidBackendId);

		smgrdounlink(srel, false);
		smgrclose(srel);
	}

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

	/* And now do the callbacks */
	if (isCommit)
		ProcessRecords(bufptr, xid, twophase_postcommit_callbacks);
	else
		ProcessRecords(bufptr, xid, twophase_postabort_callbacks);

	PredicateLockTwoPhaseFinish(xid, isCommit);

	/* Count the prepared xact as committed or aborted */
	AtEOXact_PgStat(isCommit);

	/*
	 * And now we can clean up our mess.
	 */
	RemoveTwoPhaseFile(xid, true);

	RemoveGXact(gxact);
	MyLockedGxact = NULL;

	pfree(buf);
}

/*
 * Scan a 2PC state file (already read into memory by ReadTwoPhaseFile)
 * and call the indicated callbacks for each 2PC record.
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
void
RemoveTwoPhaseFile(TransactionId xid, bool giveWarning)
{
	char		path[MAXPGPATH];

	TwoPhaseFilePath(path, xid);
	if (unlink(path))
		if (errno != ENOENT || giveWarning)
			ereport(WARNING,
					(errcode_for_file_access(),
				   errmsg("could not remove two-phase state file \"%s\": %m",
						  path)));
}

/*
 * Recreates a state file. This is used in WAL replay.
 *
 * Note: content and len don't include CRC.
 */
void
RecreateTwoPhaseFile(TransactionId xid, void *content, int len)
{
	char		path[MAXPGPATH];
	pg_crc32	statefile_crc;
	int			fd;

	/* Recompute CRC */
	INIT_CRC32(statefile_crc);
	COMP_CRC32(statefile_crc, content, len);
	FIN_CRC32(statefile_crc);

	TwoPhaseFilePath(path, xid);

	fd = OpenTransientFile(path,
						   O_CREAT | O_TRUNC | O_WRONLY | PG_BINARY,
						   S_IRUSR | S_IWUSR);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not recreate two-phase state file \"%s\": %m",
						path)));

	/* Write content and CRC */
	if (write(fd, content, len) != len)
	{
		CloseTransientFile(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write two-phase state file: %m")));
	}
	if (write(fd, &statefile_crc, sizeof(pg_crc32)) != sizeof(pg_crc32))
	{
		CloseTransientFile(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write two-phase state file: %m")));
	}

	/*
	 * We must fsync the file because the end-of-replay checkpoint will not do
	 * so, there being no GXACT in shared memory yet to tell it to.
	 */
	if (pg_fsync(fd) != 0)
	{
		CloseTransientFile(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync two-phase state file: %m")));
	}

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close two-phase state file: %m")));
}

/*
 * CheckPointTwoPhase -- handle 2PC component of checkpointing.
 *
 * We must fsync the state file of any GXACT that is valid and has a PREPARE
 * LSN <= the checkpoint's redo horizon.  (If the gxact isn't valid yet or
 * has a later LSN, this checkpoint is not responsible for fsyncing it.)
 *
 * This is deliberately run as late as possible in the checkpoint sequence,
 * because GXACTs ordinarily have short lifespans, and so it is quite
 * possible that GXACTs that were valid at checkpoint start will no longer
 * exist if we wait a little bit.
 *
 * If a GXACT remains valid across multiple checkpoints, it'll be fsynced
 * each time.  This is considered unusual enough that we don't bother to
 * expend any extra code to avoid the redundant fsyncs.  (They should be
 * reasonably cheap anyway, since they won't cause I/O.)
 */
void
CheckPointTwoPhase(XLogRecPtr redo_horizon)
{
	TransactionId *xids;
	int			nxids;
	char		path[MAXPGPATH];
	int			i;

	/*
	 * We don't want to hold the TwoPhaseStateLock while doing I/O, so we grab
	 * it just long enough to make a list of the XIDs that require fsyncing,
	 * and then do the I/O afterwards.
	 *
	 * This approach creates a race condition: someone else could delete a
	 * GXACT between the time we release TwoPhaseStateLock and the time we try
	 * to open its state file.  We handle this by special-casing ENOENT
	 * failures: if we see that, we verify that the GXACT is no longer valid,
	 * and if so ignore the failure.
	 */
	if (max_prepared_xacts <= 0)
		return;					/* nothing to do */

	TRACE_POSTGRESQL_TWOPHASE_CHECKPOINT_START();

	xids = (TransactionId *) palloc(max_prepared_xacts * sizeof(TransactionId));
	nxids = 0;

	LWLockAcquire(TwoPhaseStateLock, LW_SHARED);

	for (i = 0; i < TwoPhaseState->numPrepXacts; i++)
	{
		GlobalTransaction gxact = TwoPhaseState->prepXacts[i];
		PGXACT	   *pgxact = &ProcGlobal->allPgXact[gxact->pgprocno];

		if (gxact->valid &&
			gxact->prepare_lsn <= redo_horizon)
			xids[nxids++] = pgxact->xid;
	}

	LWLockRelease(TwoPhaseStateLock);

	for (i = 0; i < nxids; i++)
	{
		TransactionId xid = xids[i];
		int			fd;

		TwoPhaseFilePath(path, xid);

		fd = OpenTransientFile(path, O_RDWR | PG_BINARY, 0);
		if (fd < 0)
		{
			if (errno == ENOENT)
			{
				/* OK if gxact is no longer valid */
				if (!TransactionIdIsPrepared(xid))
					continue;
				/* Restore errno in case it was changed */
				errno = ENOENT;
			}
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open two-phase state file \"%s\": %m",
							path)));
		}

		if (pg_fsync(fd) != 0)
		{
			CloseTransientFile(fd);
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not fsync two-phase state file \"%s\": %m",
							path)));
		}

		if (CloseTransientFile(fd) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close two-phase state file \"%s\": %m",
							path)));
	}

	pfree(xids);

	TRACE_POSTGRESQL_TWOPHASE_CHECKPOINT_DONE();
}

/*
 * PrescanPreparedTransactions
 *
 * Scan the pg_twophase directory and determine the range of valid XIDs
 * present.  This is run during database startup, after we have completed
 * reading WAL.  ShmemVariableCache->nextXid has been set to one more than
 * the highest XID for which evidence exists in WAL.
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
	TransactionId origNextXid = ShmemVariableCache->nextXid;
	TransactionId result = origNextXid;
	DIR		   *cldir;
	struct dirent *clde;
	TransactionId *xids = NULL;
	int			nxids = 0;
	int			allocsize = 0;

	cldir = AllocateDir(TWOPHASE_DIR);
	while ((clde = ReadDir(cldir, TWOPHASE_DIR)) != NULL)
	{
		if (strlen(clde->d_name) == 8 &&
			strspn(clde->d_name, "0123456789ABCDEF") == 8)
		{
			TransactionId xid;
			char	   *buf;
			TwoPhaseFileHeader *hdr;
			TransactionId *subxids;
			int			i;

			xid = (TransactionId) strtoul(clde->d_name, NULL, 16);

			/* Reject XID if too new */
			if (TransactionIdFollowsOrEquals(xid, origNextXid))
			{
				ereport(WARNING,
						(errmsg("removing future two-phase state file \"%s\"",
								clde->d_name)));
				RemoveTwoPhaseFile(xid, true);
				continue;
			}

			/*
			 * Note: we can't check if already processed because clog
			 * subsystem isn't up yet.
			 */

			/* Read and validate file */
			buf = ReadTwoPhaseFile(xid, true);
			if (buf == NULL)
			{
				ereport(WARNING,
					  (errmsg("removing corrupt two-phase state file \"%s\"",
							  clde->d_name)));
				RemoveTwoPhaseFile(xid, true);
				continue;
			}

			/* Deconstruct header */
			hdr = (TwoPhaseFileHeader *) buf;
			if (!TransactionIdEquals(hdr->xid, xid))
			{
				ereport(WARNING,
					  (errmsg("removing corrupt two-phase state file \"%s\"",
							  clde->d_name)));
				RemoveTwoPhaseFile(xid, true);
				pfree(buf);
				continue;
			}

			/*
			 * OK, we think this file is valid.  Incorporate xid into the
			 * running-minimum result.
			 */
			if (TransactionIdPrecedes(xid, result))
				result = xid;

			/*
			 * Examine subtransaction XIDs ... they should all follow main
			 * XID, and they may force us to advance nextXid.
			 *
			 * We don't expect anyone else to modify nextXid, hence we don't
			 * need to hold a lock while examining it.  We still acquire the
			 * lock to modify it, though.
			 */
			subxids = (TransactionId *)
				(buf + MAXALIGN(sizeof(TwoPhaseFileHeader)));
			for (i = 0; i < hdr->nsubxacts; i++)
			{
				TransactionId subxid = subxids[i];

				Assert(TransactionIdFollows(subxid, xid));
				if (TransactionIdFollowsOrEquals(subxid,
												 ShmemVariableCache->nextXid))
				{
					LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
					ShmemVariableCache->nextXid = subxid;
					TransactionIdAdvance(ShmemVariableCache->nextXid);
					LWLockRelease(XidGenLock);
				}
			}


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
	}
	FreeDir(cldir);

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
 * Scan the pg_twophase directory and setup all the required information to
 * allow standby queries to treat prepared transactions as still active.
 * This is never called at the end of recovery - we use
 * RecoverPreparedTransactions() at that point.
 *
 * Currently we simply call SubTransSetParent() for any subxids of prepared
 * transactions. If overwriteOK is true, it's OK if some XIDs have already
 * been marked in pg_subtrans.
 */
void
StandbyRecoverPreparedTransactions(bool overwriteOK)
{
	DIR		   *cldir;
	struct dirent *clde;

	cldir = AllocateDir(TWOPHASE_DIR);
	while ((clde = ReadDir(cldir, TWOPHASE_DIR)) != NULL)
	{
		if (strlen(clde->d_name) == 8 &&
			strspn(clde->d_name, "0123456789ABCDEF") == 8)
		{
			TransactionId xid;
			char	   *buf;
			TwoPhaseFileHeader *hdr;
			TransactionId *subxids;
			int			i;

			xid = (TransactionId) strtoul(clde->d_name, NULL, 16);

			/* Already processed? */
			if (TransactionIdDidCommit(xid) || TransactionIdDidAbort(xid))
			{
				ereport(WARNING,
						(errmsg("removing stale two-phase state file \"%s\"",
								clde->d_name)));
				RemoveTwoPhaseFile(xid, true);
				continue;
			}

			/* Read and validate file */
			buf = ReadTwoPhaseFile(xid, true);
			if (buf == NULL)
			{
				ereport(WARNING,
					  (errmsg("removing corrupt two-phase state file \"%s\"",
							  clde->d_name)));
				RemoveTwoPhaseFile(xid, true);
				continue;
			}

			/* Deconstruct header */
			hdr = (TwoPhaseFileHeader *) buf;
			if (!TransactionIdEquals(hdr->xid, xid))
			{
				ereport(WARNING,
					  (errmsg("removing corrupt two-phase state file \"%s\"",
							  clde->d_name)));
				RemoveTwoPhaseFile(xid, true);
				pfree(buf);
				continue;
			}

			/*
			 * Examine subtransaction XIDs ... they should all follow main
			 * XID.
			 */
			subxids = (TransactionId *)
				(buf + MAXALIGN(sizeof(TwoPhaseFileHeader)));
			for (i = 0; i < hdr->nsubxacts; i++)
			{
				TransactionId subxid = subxids[i];

				Assert(TransactionIdFollows(subxid, xid));
				SubTransSetParent(xid, subxid, overwriteOK);
			}
		}
	}
	FreeDir(cldir);
}

/*
 * RecoverPreparedTransactions
 *
 * Scan the pg_twophase directory and reload shared-memory state for each
 * prepared transaction (reacquire locks, etc).  This is run during database
 * startup.
 */
void
RecoverPreparedTransactions(void)
{
	char		dir[MAXPGPATH];
	DIR		   *cldir;
	struct dirent *clde;
	bool		overwriteOK = false;

	snprintf(dir, MAXPGPATH, "%s", TWOPHASE_DIR);

	cldir = AllocateDir(dir);
	while ((clde = ReadDir(cldir, dir)) != NULL)
	{
		if (strlen(clde->d_name) == 8 &&
			strspn(clde->d_name, "0123456789ABCDEF") == 8)
		{
			TransactionId xid;
			char	   *buf;
			char	   *bufptr;
			TwoPhaseFileHeader *hdr;
			TransactionId *subxids;
			GlobalTransaction gxact;
			int			i;

			xid = (TransactionId) strtoul(clde->d_name, NULL, 16);

			/* Already processed? */
			if (TransactionIdDidCommit(xid) || TransactionIdDidAbort(xid))
			{
				ereport(WARNING,
						(errmsg("removing stale two-phase state file \"%s\"",
								clde->d_name)));
				RemoveTwoPhaseFile(xid, true);
				continue;
			}

			/* Read and validate file */
			buf = ReadTwoPhaseFile(xid, true);
			if (buf == NULL)
			{
				ereport(WARNING,
					  (errmsg("removing corrupt two-phase state file \"%s\"",
							  clde->d_name)));
				RemoveTwoPhaseFile(xid, true);
				continue;
			}

			ereport(LOG,
					(errmsg("recovering prepared transaction %u", xid)));

			/* Deconstruct header */
			hdr = (TwoPhaseFileHeader *) buf;
			Assert(TransactionIdEquals(hdr->xid, xid));
			bufptr = buf + MAXALIGN(sizeof(TwoPhaseFileHeader));
			subxids = (TransactionId *) bufptr;
			bufptr += MAXALIGN(hdr->nsubxacts * sizeof(TransactionId));
			bufptr += MAXALIGN(hdr->ncommitrels * sizeof(RelFileNode));
			bufptr += MAXALIGN(hdr->nabortrels * sizeof(RelFileNode));
			bufptr += MAXALIGN(hdr->ninvalmsgs * sizeof(SharedInvalidationMessage));

			/*
			 * It's possible that SubTransSetParent has been set before, if
			 * the prepared transaction generated xid assignment records. Test
			 * here must match one used in AssignTransactionId().
			 */
			if (InHotStandby && (hdr->nsubxacts >= PGPROC_MAX_CACHED_SUBXIDS ||
								 XLogLogicalInfoActive()))
				overwriteOK = true;

			/*
			 * Reconstruct subtrans state for the transaction --- needed
			 * because pg_subtrans is not preserved over a restart.  Note that
			 * we are linking all the subtransactions directly to the
			 * top-level XID; there may originally have been a more complex
			 * hierarchy, but there's no need to restore that exactly.
			 */
			for (i = 0; i < hdr->nsubxacts; i++)
				SubTransSetParent(subxids[i], xid, overwriteOK);

			/*
			 * Recreate its GXACT and dummy PGPROC
			 *
			 * Note: since we don't have the PREPARE record's WAL location at
			 * hand, we leave prepare_lsn zeroes.  This means the GXACT will
			 * be fsync'd on every future checkpoint.  We assume this
			 * situation is infrequent enough that the performance cost is
			 * negligible (especially since we know the state file has already
			 * been fsynced).
			 */
			gxact = MarkAsPreparing(xid, hdr->gid,
									hdr->prepared_at,
									hdr->owner, hdr->database);
			GXactLoadSubxactData(gxact, hdr->nsubxacts, subxids);
			MarkAsPrepared(gxact);

			/*
			 * Recover other state (notably locks) using resource managers
			 */
			ProcessRecords(bufptr, xid, twophase_recover_callbacks);

			/*
			 * Release locks held by the standby process after we process each
			 * prepared transaction. As a result, we don't need too many
			 * additional locks at any one time.
			 */
			if (InHotStandby)
				StandbyReleaseLockTree(xid, hdr->nsubxacts, subxids);

			pfree(buf);
		}
	}
	FreeDir(cldir);
}

/*
 *	RecordTransactionCommitPrepared
 *
 * This is basically the same as RecordTransactionCommit: in particular,
 * we must set the delayChkpt flag to avoid a race condition.
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
								bool initfileinval)
{
	XLogRecData rdata[4];
	int			lastrdata = 0;
	xl_xact_commit_prepared xlrec;
	XLogRecPtr	recptr;

	START_CRIT_SECTION();

	/* See notes in RecordTransactionCommit */
	MyPgXact->delayChkpt = true;

	/* Emit the XLOG commit record */
	xlrec.xid = xid;

	xlrec.crec.xinfo = initfileinval ? XACT_COMPLETION_UPDATE_RELCACHE_FILE : 0;

	xlrec.crec.dbId = MyDatabaseId;
	xlrec.crec.tsId = MyDatabaseTableSpace;

	xlrec.crec.xact_time = GetCurrentTimestamp();
	xlrec.crec.nrels = nrels;
	xlrec.crec.nsubxacts = nchildren;
	xlrec.crec.nmsgs = ninvalmsgs;

	rdata[0].data = (char *) (&xlrec);
	rdata[0].len = MinSizeOfXactCommitPrepared;
	rdata[0].buffer = InvalidBuffer;
	/* dump rels to delete */
	if (nrels > 0)
	{
		rdata[0].next = &(rdata[1]);
		rdata[1].data = (char *) rels;
		rdata[1].len = nrels * sizeof(RelFileNode);
		rdata[1].buffer = InvalidBuffer;
		lastrdata = 1;
	}
	/* dump committed child Xids */
	if (nchildren > 0)
	{
		rdata[lastrdata].next = &(rdata[2]);
		rdata[2].data = (char *) children;
		rdata[2].len = nchildren * sizeof(TransactionId);
		rdata[2].buffer = InvalidBuffer;
		lastrdata = 2;
	}
	/* dump cache invalidation messages */
	if (ninvalmsgs > 0)
	{
		rdata[lastrdata].next = &(rdata[3]);
		rdata[3].data = (char *) invalmsgs;
		rdata[3].len = ninvalmsgs * sizeof(SharedInvalidationMessage);
		rdata[3].buffer = InvalidBuffer;
		lastrdata = 3;
	}
	rdata[lastrdata].next = NULL;

	recptr = XLogInsert(RM_XACT_ID, XLOG_XACT_COMMIT_PREPARED, rdata);

	/*
	 * We don't currently try to sleep before flush here ... nor is there any
	 * support for async commit of a prepared xact (the very idea is probably
	 * a contradiction)
	 */

	/* Flush XLOG to disk */
	XLogFlush(recptr);

	/* Mark the transaction committed in pg_clog */
	TransactionIdCommitTree(xid, nchildren, children);

	/* Checkpoint can proceed now */
	MyPgXact->delayChkpt = false;

	END_CRIT_SECTION();

	/*
	 * Wait for synchronous replication, if required.
	 *
	 * Note that at this stage we have marked clog, but still show as running
	 * in the procarray and continue to hold locks.
	 */
	SyncRepWaitForLSN(recptr);
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
							   RelFileNode *rels)
{
	XLogRecData rdata[3];
	int			lastrdata = 0;
	xl_xact_abort_prepared xlrec;
	XLogRecPtr	recptr;

	/*
	 * Catch the scenario where we aborted partway through
	 * RecordTransactionCommitPrepared ...
	 */
	if (TransactionIdDidCommit(xid))
		elog(PANIC, "cannot abort transaction %u, it was already committed",
			 xid);

	START_CRIT_SECTION();

	/* Emit the XLOG abort record */
	xlrec.xid = xid;
	xlrec.arec.xact_time = GetCurrentTimestamp();
	xlrec.arec.nrels = nrels;
	xlrec.arec.nsubxacts = nchildren;
	rdata[0].data = (char *) (&xlrec);
	rdata[0].len = MinSizeOfXactAbortPrepared;
	rdata[0].buffer = InvalidBuffer;
	/* dump rels to delete */
	if (nrels > 0)
	{
		rdata[0].next = &(rdata[1]);
		rdata[1].data = (char *) rels;
		rdata[1].len = nrels * sizeof(RelFileNode);
		rdata[1].buffer = InvalidBuffer;
		lastrdata = 1;
	}
	/* dump committed child Xids */
	if (nchildren > 0)
	{
		rdata[lastrdata].next = &(rdata[2]);
		rdata[2].data = (char *) children;
		rdata[2].len = nchildren * sizeof(TransactionId);
		rdata[2].buffer = InvalidBuffer;
		lastrdata = 2;
	}
	rdata[lastrdata].next = NULL;

	recptr = XLogInsert(RM_XACT_ID, XLOG_XACT_ABORT_PREPARED, rdata);

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
	SyncRepWaitForLSN(recptr);
}
