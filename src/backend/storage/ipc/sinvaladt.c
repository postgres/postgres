/*-------------------------------------------------------------------------
 *
 * sinvaladt.c
 *	  POSTGRES shared cache invalidation segment definitions.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/ipc/sinvaladt.c,v 1.69 2008/03/18 12:36:43 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/backendid.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/sinvaladt.h"


/*
 * Conceptually, the shared cache invalidation messages are stored in an
 * infinite array, where maxMsgNum is the next array subscript to store a
 * submitted message in, minMsgNum is the smallest array subscript containing a
 * message not yet read by all backends, and we always have maxMsgNum >=
 * minMsgNum.  (They are equal when there are no messages pending.)  For each
 * active backend, there is a nextMsgNum pointer indicating the next message it
 * needs to read; we have maxMsgNum >= nextMsgNum >= minMsgNum for every
 * backend.
 *
 * In reality, the messages are stored in a circular buffer of MAXNUMMESSAGES
 * entries.  We translate MsgNum values into circular-buffer indexes by
 * computing MsgNum % MAXNUMMESSAGES (this should be fast as long as
 * MAXNUMMESSAGES is a constant and a power of 2).	As long as maxMsgNum
 * doesn't exceed minMsgNum by more than MAXNUMMESSAGES, we have enough space
 * in the buffer.  If the buffer does overflow, we reset it to empty and
 * force each backend to "reset", ie, discard all its invalidatable state.
 *
 * We would have problems if the MsgNum values overflow an integer, so
 * whenever minMsgNum exceeds MSGNUMWRAPAROUND, we subtract MSGNUMWRAPAROUND
 * from all the MsgNum variables simultaneously.  MSGNUMWRAPAROUND can be
 * large so that we don't need to do this often.  It must be a multiple of
 * MAXNUMMESSAGES so that the existing circular-buffer entries don't need
 * to be moved when we do it.
 */


/*
 * Configurable parameters.
 *
 * MAXNUMMESSAGES: max number of shared-inval messages we can buffer.
 * Must be a power of 2 for speed.
 *
 * MSGNUMWRAPAROUND: how often to reduce MsgNum variables to avoid overflow.
 * Must be a multiple of MAXNUMMESSAGES.  Should be large.
 */

#define MAXNUMMESSAGES 4096
#define MSGNUMWRAPAROUND (MAXNUMMESSAGES * 4096)

/* Per-backend state in shared invalidation structure */
typedef struct ProcState
{
	/* nextMsgNum is -1 in an inactive ProcState array entry. */
	int			nextMsgNum;		/* next message number to read, or -1 */
	bool		resetState;		/* true, if backend has to reset its state */
} ProcState;

/* Shared cache invalidation memory segment */
typedef struct SISeg
{
	/*
	 * General state information
	 */
	int			minMsgNum;		/* oldest message still needed */
	int			maxMsgNum;		/* next message number to be assigned */
	int			lastBackend;	/* index of last active procState entry, +1 */
	int			maxBackends;	/* size of procState array */
	int			freeBackends;	/* number of empty procState slots */

	/*
	 * Next LocalTransactionId to use for each idle backend slot.  We keep
	 * this here because it is indexed by BackendId and it is convenient to
	 * copy the value to and from local memory when MyBackendId is set.
	 */
	LocalTransactionId *nextLXID;		/* array of maxBackends entries */

	/*
	 * Circular buffer holding shared-inval messages
	 */
	SharedInvalidationMessage buffer[MAXNUMMESSAGES];

	/*
	 * Per-backend state info.
	 *
	 * We declare procState as 1 entry because C wants a fixed-size array, but
	 * actually it is maxBackends entries long.
	 */
	ProcState	procState[1];	/* reflects the invalidation state */
} SISeg;

static SISeg *shmInvalBuffer;	/* pointer to the shared inval buffer */


static LocalTransactionId nextLocalTransactionId;

static void CleanupInvalidationState(int status, Datum arg);
static void SISetProcStateInvalid(SISeg *segP);


/*
 * SInvalShmemSize --- return shared-memory space needed
 */
Size
SInvalShmemSize(void)
{
	Size		size;

	size = offsetof(SISeg, procState);
	size = add_size(size, mul_size(sizeof(ProcState), MaxBackends));

	size = add_size(size, mul_size(sizeof(LocalTransactionId), MaxBackends));

	return size;
}

/*
 * SharedInvalBufferInit
 *		Create and initialize the SI message buffer
 */
void
CreateSharedInvalidationState(void)
{
	Size		size;
	int			i;
	bool		found;

	/* Allocate space in shared memory */
	size = offsetof(SISeg, procState);
	size = add_size(size, mul_size(sizeof(ProcState), MaxBackends));

	shmInvalBuffer = (SISeg *)
		ShmemInitStruct("shmInvalBuffer", size, &found);
	if (found)
		return;

	shmInvalBuffer->nextLXID = ShmemAlloc(sizeof(LocalTransactionId) * MaxBackends);

	/* Clear message counters, save size of procState array */
	shmInvalBuffer->minMsgNum = 0;
	shmInvalBuffer->maxMsgNum = 0;
	shmInvalBuffer->lastBackend = 0;
	shmInvalBuffer->maxBackends = MaxBackends;
	shmInvalBuffer->freeBackends = MaxBackends;

	/* The buffer[] array is initially all unused, so we need not fill it */

	/* Mark all backends inactive, and initialize nextLXID */
	for (i = 0; i < shmInvalBuffer->maxBackends; i++)
	{
		shmInvalBuffer->procState[i].nextMsgNum = -1;		/* inactive */
		shmInvalBuffer->procState[i].resetState = false;
		shmInvalBuffer->nextLXID[i] = InvalidLocalTransactionId;
	}
}

/*
 * SharedInvalBackendInit
 *		Initialize a new backend to operate on the sinval buffer
 */
void
SharedInvalBackendInit(void)
{
	int			index;
	ProcState  *stateP = NULL;
	SISeg	   *segP = shmInvalBuffer;

	LWLockAcquire(SInvalLock, LW_EXCLUSIVE);

	/* Look for a free entry in the procState array */
	for (index = 0; index < segP->lastBackend; index++)
	{
		if (segP->procState[index].nextMsgNum < 0)		/* inactive slot? */
		{
			stateP = &segP->procState[index];
			break;
		}
	}

	if (stateP == NULL)
	{
		if (segP->lastBackend < segP->maxBackends)
		{
			stateP = &segP->procState[segP->lastBackend];
			Assert(stateP->nextMsgNum < 0);
			segP->lastBackend++;
		}
		else
		{
			/*
			 * out of procState slots: MaxBackends exceeded -- report normally
			 */
			MyBackendId = InvalidBackendId;
			LWLockRelease(SInvalLock);
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("sorry, too many clients already")));
		}
	}

	MyBackendId = (stateP - &segP->procState[0]) + 1;

#ifdef	INVALIDDEBUG
	elog(DEBUG2, "my backend id is %d", MyBackendId);
#endif   /* INVALIDDEBUG */

	/* Advertise assigned backend ID in MyProc */
	MyProc->backendId = MyBackendId;

	/* Reduce free slot count */
	segP->freeBackends--;

	/* Fetch next local transaction ID into local memory */
	nextLocalTransactionId = segP->nextLXID[MyBackendId - 1];

	/* mark myself active, with all extant messages already read */
	stateP->nextMsgNum = segP->maxMsgNum;
	stateP->resetState = false;

	LWLockRelease(SInvalLock);

	/* register exit routine to mark my entry inactive at exit */
	on_shmem_exit(CleanupInvalidationState, PointerGetDatum(segP));
}

/*
 * CleanupInvalidationState
 *		Mark the current backend as no longer active.
 *
 * This function is called via on_shmem_exit() during backend shutdown,
 * so the caller has NOT acquired the lock for us.
 *
 * arg is really of type "SISeg*".
 */
static void
CleanupInvalidationState(int status, Datum arg)
{
	SISeg	   *segP = (SISeg *) DatumGetPointer(arg);
	int			i;

	Assert(PointerIsValid(segP));

	LWLockAcquire(SInvalLock, LW_EXCLUSIVE);

	/* Update next local transaction ID for next holder of this backendID */
	segP->nextLXID[MyBackendId - 1] = nextLocalTransactionId;

	/* Mark myself inactive */
	segP->procState[MyBackendId - 1].nextMsgNum = -1;
	segP->procState[MyBackendId - 1].resetState = false;

	/* Recompute index of last active backend */
	for (i = segP->lastBackend; i > 0; i--)
	{
		if (segP->procState[i - 1].nextMsgNum >= 0)
			break;
	}
	segP->lastBackend = i;

	/* Adjust free slot count */
	segP->freeBackends++;

	LWLockRelease(SInvalLock);
}

/*
 * SIInsertDataEntry
 *		Add a new invalidation message to the buffer.
 *
 * If we are unable to insert the message because the buffer is full,
 * then clear the buffer and assert the "reset" flag to each backend.
 * This will cause all the backends to discard *all* invalidatable state.
 *
 * Returns true for normal successful insertion, false if had to reset.
 */
bool
SIInsertDataEntry(SharedInvalidationMessage *data)
{
	int			numMsgs;
	bool		signal_postmaster = false;
	SISeg	   *segP;

	LWLockAcquire(SInvalLock, LW_EXCLUSIVE);

	segP = shmInvalBuffer;
	numMsgs = segP->maxMsgNum - segP->minMsgNum;

	/* Is the buffer full? */
	if (numMsgs >= MAXNUMMESSAGES)
	{
		/*
		 * Don't panic just yet: slowest backend might have consumed some
		 * messages but not yet have done SIDelExpiredDataEntries() to advance
		 * minMsgNum.  So, make sure minMsgNum is up-to-date.
		 */
		SIDelExpiredDataEntries(true);
		numMsgs = segP->maxMsgNum - segP->minMsgNum;
		if (numMsgs >= MAXNUMMESSAGES)
		{
			/* Yup, it's definitely full, no choice but to reset */
			SISetProcStateInvalid(segP);
			LWLockRelease(SInvalLock);
			return false;
		}
	}

	/*
	 * Try to prevent table overflow.  When the table is 70% full send a
	 * WAKEN_CHILDREN request to the postmaster.  The postmaster will send a
	 * SIGUSR1 signal to all the backends, which will cause sinval.c to read
	 * any pending SI entries.
	 *
	 * This should never happen if all the backends are actively executing
	 * queries, but if a backend is sitting idle then it won't be starting
	 * transactions and so won't be reading SI entries.
	 */
	if (numMsgs == (MAXNUMMESSAGES * 70 / 100) && IsUnderPostmaster)
		signal_postmaster = true;

	/*
	 * Insert new message into proper slot of circular buffer
	 */
	segP->buffer[segP->maxMsgNum % MAXNUMMESSAGES] = *data;
	segP->maxMsgNum++;

	LWLockRelease(SInvalLock);

	if (signal_postmaster)
	{
		elog(DEBUG4, "SI table is 70%% full, signaling postmaster");
		SendPostmasterSignal(PMSIGNAL_WAKEN_CHILDREN);
	}

	return true;
}

/*
 * SISetProcStateInvalid
 *		Flush pending messages from buffer, assert reset flag for each backend
 *
 * This is used only to recover from SI buffer overflow.
 */
static void
SISetProcStateInvalid(SISeg *segP)
{
	int			i;

	segP->minMsgNum = 0;
	segP->maxMsgNum = 0;

	for (i = 0; i < segP->lastBackend; i++)
	{
		if (segP->procState[i].nextMsgNum >= 0) /* active backend? */
		{
			segP->procState[i].resetState = true;
			segP->procState[i].nextMsgNum = 0;
		}
	}
}

/*
 * SIGetDataEntry
 *		get next SI message for specified backend, if there is one
 *
 * Possible return values:
 *	0: no SI message available
 *	1: next SI message has been extracted into *data
 *		(there may be more messages available after this one!)
 * -1: SI reset message extracted
 *
 * NB: this can run in parallel with other instances of SIGetDataEntry
 * executing on behalf of other backends, since each instance will modify only
 * fields of its own backend's ProcState, and no instance will look at fields
 * of other backends' ProcStates.  We express this by grabbing SInvalLock in
 * shared mode.  Note that this is not exactly the normal (read-only)
 * interpretation of a shared lock! Look closely at the interactions before
 * allowing SInvalLock to be grabbed in shared mode for any other reason!
 */
int
SIGetDataEntry(int backendId, SharedInvalidationMessage *data)
{
	ProcState  *stateP;
	SISeg	   *segP;
	
	LWLockAcquire(SInvalLock, LW_SHARED);

	segP = shmInvalBuffer;
	stateP = &segP->procState[backendId - 1];

	if (stateP->resetState)
	{
		/*
		 * Force reset.  We can say we have dealt with any messages added
		 * since the reset, as well...
		 */
		stateP->resetState = false;
		stateP->nextMsgNum = segP->maxMsgNum;
		LWLockRelease(SInvalLock);
		return -1;
	}

	if (stateP->nextMsgNum >= segP->maxMsgNum)
	{
		LWLockRelease(SInvalLock);
		return 0;				/* nothing to read */
	}

	/*
	 * Retrieve message and advance my counter.
	 */
	*data = segP->buffer[stateP->nextMsgNum % MAXNUMMESSAGES];
	stateP->nextMsgNum++;

	/*
	 * There may be other backends that haven't read the message, so we cannot
	 * delete it here. SIDelExpiredDataEntries() should be called to remove
	 * dead messages.
	 */

	LWLockRelease(SInvalLock);
	return 1;					/* got a message */
}

/*
 * SIDelExpiredDataEntries
 *		Remove messages that have been consumed by all active backends
 */
void
SIDelExpiredDataEntries(bool locked)
{
	SISeg	   *segP = shmInvalBuffer;
	int			min,
				i,
				h;

	if (!locked)
		LWLockAcquire(SInvalLock, LW_EXCLUSIVE);

	min = segP->maxMsgNum;
	if (min == segP->minMsgNum)
	{
		if (!locked)
			LWLockRelease(SInvalLock);
		return;					/* fast path if no messages exist */
	}

	/* Recompute minMsgNum = minimum of all backends' nextMsgNum */

	for (i = 0; i < segP->lastBackend; i++)
	{
		h = segP->procState[i].nextMsgNum;
		if (h >= 0)
		{						/* backend active */
			if (h < min)
				min = h;
		}
	}
	segP->minMsgNum = min;

	/*
	 * When minMsgNum gets really large, decrement all message counters so as
	 * to forestall overflow of the counters.
	 */
	if (min >= MSGNUMWRAPAROUND)
	{
		segP->minMsgNum -= MSGNUMWRAPAROUND;
		segP->maxMsgNum -= MSGNUMWRAPAROUND;
		for (i = 0; i < segP->lastBackend; i++)
		{
			if (segP->procState[i].nextMsgNum >= 0)
				segP->procState[i].nextMsgNum -= MSGNUMWRAPAROUND;
		}
	}

	if (!locked)
		LWLockRelease(SInvalLock);
}


/*
 * GetNextLocalTransactionId --- allocate a new LocalTransactionId
 *
 * We split VirtualTransactionIds into two parts so that it is possible
 * to allocate a new one without any contention for shared memory, except
 * for a bit of additional overhead during backend startup/shutdown.
 * The high-order part of a VirtualTransactionId is a BackendId, and the
 * low-order part is a LocalTransactionId, which we assign from a local
 * counter.  To avoid the risk of a VirtualTransactionId being reused
 * within a short interval, successive procs occupying the same backend ID
 * slot should use a consecutive sequence of local IDs, which is implemented
 * by copying nextLocalTransactionId as seen above.
 */
LocalTransactionId
GetNextLocalTransactionId(void)
{
	LocalTransactionId result;

	/* loop to avoid returning InvalidLocalTransactionId at wraparound */
	do
	{
		result = nextLocalTransactionId++;
	} while (!LocalTransactionIdIsValid(result));

	return result;
}
