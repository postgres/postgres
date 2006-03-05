/*-------------------------------------------------------------------------
 *
 * sinvaladt.c
 *	  POSTGRES shared cache invalidation segment definitions.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/ipc/sinvaladt.c,v 1.62 2006/03/05 15:58:37 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/backendid.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/shmem.h"
#include "storage/sinvaladt.h"


SISeg	   *shmInvalBuffer;

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

	return size;
}

/*
 * SIBufferInit
 *		Create and initialize a new SI message buffer
 */
void
SIBufferInit(void)
{
	SISeg	   *segP;
	int			i;
	bool		found;

	/* Allocate space in shared memory */
	shmInvalBuffer = segP = (SISeg *)
		ShmemInitStruct("shmInvalBuffer", SInvalShmemSize(), &found);
	if (found)
		return;

	/* Clear message counters, save size of procState array */
	segP->minMsgNum = 0;
	segP->maxMsgNum = 0;
	segP->lastBackend = 0;
	segP->maxBackends = MaxBackends;
	segP->freeBackends = MaxBackends;

	/* The buffer[] array is initially all unused, so we need not fill it */

	/* Mark all backends inactive */
	for (i = 0; i < segP->maxBackends; i++)
	{
		segP->procState[i].nextMsgNum = -1;		/* inactive */
		segP->procState[i].resetState = false;
	}
}

/*
 * SIBackendInit
 *		Initialize a new backend to operate on the sinval buffer
 *
 * Returns:
 *	   >0	A-OK
 *		0	Failed to find a free procState slot (ie, MaxBackends exceeded)
 *	   <0	Some other failure (not currently used)
 *
 * NB: this routine, and all following ones, must be executed with the
 * SInvalLock lock held, since there may be multiple backends trying
 * to access the buffer.
 */
int
SIBackendInit(SISeg *segP)
{
	int			index;
	ProcState  *stateP = NULL;

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
			/* out of procState slots */
			MyBackendId = InvalidBackendId;
			return 0;
		}
	}

	MyBackendId = (stateP - &segP->procState[0]) + 1;

#ifdef	INVALIDDEBUG
	elog(DEBUG2, "my backend id is %d", MyBackendId);
#endif   /* INVALIDDEBUG */

	/* Reduce free slot count */
	segP->freeBackends--;

	/* mark myself active, with all extant messages already read */
	stateP->nextMsgNum = segP->maxMsgNum;
	stateP->resetState = false;

	/* register exit routine to mark my entry inactive at exit */
	on_shmem_exit(CleanupInvalidationState, PointerGetDatum(segP));

	return 1;
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
SIInsertDataEntry(SISeg *segP, SharedInvalidationMessage *data)
{
	int			numMsgs = segP->maxMsgNum - segP->minMsgNum;

	/* Is the buffer full? */
	if (numMsgs >= MAXNUMMESSAGES)
	{
		/*
		 * Don't panic just yet: slowest backend might have consumed some
		 * messages but not yet have done SIDelExpiredDataEntries() to advance
		 * minMsgNum.  So, make sure minMsgNum is up-to-date.
		 */
		SIDelExpiredDataEntries(segP);
		numMsgs = segP->maxMsgNum - segP->minMsgNum;
		if (numMsgs >= MAXNUMMESSAGES)
		{
			/* Yup, it's definitely full, no choice but to reset */
			SISetProcStateInvalid(segP);
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
	if (numMsgs == (MAXNUMMESSAGES * 70 / 100) &&
		IsUnderPostmaster)
	{
		elog(DEBUG4, "SI table is 70%% full, signaling postmaster");
		SendPostmasterSignal(PMSIGNAL_WAKEN_CHILDREN);
	}

	/*
	 * Insert new message into proper slot of circular buffer
	 */
	segP->buffer[segP->maxMsgNum % MAXNUMMESSAGES] = *data;
	segP->maxMsgNum++;

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
 * executing on behalf of other backends.  See comments in sinval.c in
 * ReceiveSharedInvalidMessages().
 */
int
SIGetDataEntry(SISeg *segP, int backendId,
			   SharedInvalidationMessage *data)
{
	ProcState  *stateP = &segP->procState[backendId - 1];

	if (stateP->resetState)
	{
		/*
		 * Force reset.  We can say we have dealt with any messages added
		 * since the reset, as well...
		 */
		stateP->resetState = false;
		stateP->nextMsgNum = segP->maxMsgNum;
		return -1;
	}

	if (stateP->nextMsgNum >= segP->maxMsgNum)
		return 0;				/* nothing to read */

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
	return 1;					/* got a message */
}

/*
 * SIDelExpiredDataEntries
 *		Remove messages that have been consumed by all active backends
 */
void
SIDelExpiredDataEntries(SISeg *segP)
{
	int			min,
				i,
				h;

	min = segP->maxMsgNum;
	if (min == segP->minMsgNum)
		return;					/* fast path if no messages exist */

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
}
