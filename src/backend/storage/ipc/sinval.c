/*-------------------------------------------------------------------------
 *
 * sinval.c
 *	  POSTGRES shared cache invalidation communication code.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/ipc/sinval.c,v 1.25 2001/01/24 19:43:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/* #define INVALIDDEBUG 1 */

#include <sys/types.h>

#include "postgres.h"

#include "storage/backendid.h"
#include "storage/proc.h"
#include "storage/sinval.h"
#include "storage/sinvaladt.h"
#include "utils/tqual.h"

SPINLOCK	SInvalLock = (SPINLOCK) NULL;

/****************************************************************************/
/*	CreateSharedInvalidationState()		 Initialize SI buffer				*/
/*																			*/
/*	should be called only by the POSTMASTER									*/
/****************************************************************************/
void
CreateSharedInvalidationState(int maxBackends)
{
	/* SInvalLock must be initialized already, during spinlock init */
	SIBufferInit(maxBackends);
}

/*
 * InitBackendSharedInvalidationState
 *		Initialize new backend's state info in buffer segment.
 */
void
InitBackendSharedInvalidationState(void)
{
	SpinAcquire(SInvalLock);
	if (!SIBackendInit(shmInvalBuffer))
	{
		SpinRelease(SInvalLock);
		elog(FATAL, "Backend cache invalidation initialization failed");
	}
	SpinRelease(SInvalLock);
}

/*
 * RegisterSharedInvalid
 *	Add a shared-cache-invalidation message to the global SI message queue.
 *
 * Note:
 *	Assumes hash index is valid.
 *	Assumes item pointer is valid.
 */
void
RegisterSharedInvalid(int cacheId,		/* XXX */
					  Index hashIndex,
					  ItemPointer pointer)
{
	SharedInvalidData newInvalid;
	bool		insertOK;

	/*
	 * This code has been hacked to accept two types of messages.  This
	 * might be treated more generally in the future.
	 *
	 * (1) cacheId= system cache id hashIndex= system cache hash index for a
	 * (possibly) cached tuple pointer= pointer of (possibly) cached tuple
	 *
	 * (2) cacheId= special non-syscache id hashIndex= object id contained in
	 * (possibly) cached relation descriptor pointer= null
	 */

	newInvalid.cacheId = cacheId;
	newInvalid.hashIndex = hashIndex;

	if (ItemPointerIsValid(pointer))
		ItemPointerCopy(pointer, &newInvalid.pointerData);
	else
		ItemPointerSetInvalid(&newInvalid.pointerData);

	SpinAcquire(SInvalLock);
	insertOK = SIInsertDataEntry(shmInvalBuffer, &newInvalid);
	SpinRelease(SInvalLock);
	if (!insertOK)
		elog(NOTICE, "RegisterSharedInvalid: SI buffer overflow");
}

/*
 * InvalidateSharedInvalid
 *		Process shared-cache-invalidation messages waiting for this backend
 */
void
			InvalidateSharedInvalid(void (*invalFunction) (),
									void (*resetFunction) ())
{
	SharedInvalidData data;
	int			getResult;
	bool		gotMessage = false;

	for (;;)
	{
		SpinAcquire(SInvalLock);
		getResult = SIGetDataEntry(shmInvalBuffer, MyBackendId, &data);
		SpinRelease(SInvalLock);
		if (getResult == 0)
			break;				/* nothing more to do */
		if (getResult < 0)
		{
			/* got a reset message */
			elog(NOTICE, "InvalidateSharedInvalid: cache state reset");
			resetFunction();
		}
		else
		{
			/* got a normal data message */
			invalFunction(data.cacheId,
						  data.hashIndex,
						  &data.pointerData);
		}
		gotMessage = true;
	}

	/* If we got any messages, try to release dead messages */
	if (gotMessage)
	{
		SpinAcquire(SInvalLock);
		SIDelExpiredDataEntries(shmInvalBuffer);
		SpinRelease(SInvalLock);
	}
}


/****************************************************************************/
/* Functions that need to scan the PROC structures of all running backends. */
/* It's a bit strange to keep these in sinval.c, since they don't have any	*/
/* direct relationship to shared-cache invalidation.  But the procState		*/
/* array in the SI segment is the only place in the system where we have	*/
/* an array of per-backend data, so it is the most convenient place to keep */
/* pointers to the backends' PROC structures.  We used to implement these	*/
/* functions with a slow, ugly search through the ShmemIndex hash table --- */
/* now they are simple loops over the SI ProcState array.					*/
/****************************************************************************/


/*
 * DatabaseHasActiveBackends -- are there any backends running in the given DB
 *
 * If 'ignoreMyself' is TRUE, ignore this particular backend while checking
 * for backends in the target database.
 *
 * This function is used to interlock DROP DATABASE against there being
 * any active backends in the target DB --- dropping the DB while active
 * backends remain would be a Bad Thing.  Note that we cannot detect here
 * the possibility of a newly-started backend that is trying to connect
 * to the doomed database, so additional interlocking is needed during
 * backend startup.
 */

bool
DatabaseHasActiveBackends(Oid databaseId, bool ignoreMyself)
{
	bool		result = false;
	SISeg	   *segP = shmInvalBuffer;
	ProcState  *stateP = segP->procState;
	int			index;

	SpinAcquire(SInvalLock);

	for (index = 0; index < segP->lastBackend; index++)
	{
		SHMEM_OFFSET pOffset = stateP[index].procStruct;

		if (pOffset != INVALID_OFFSET)
		{
			PROC	   *proc = (PROC *) MAKE_PTR(pOffset);

			if (proc->databaseId == databaseId)
			{
				if (ignoreMyself && proc == MyProc)
					continue;

				result = true;
				break;
			}
		}
	}

	SpinRelease(SInvalLock);

	return result;
}

/*
 * TransactionIdIsInProgress -- is given transaction running by some backend
 */
bool
TransactionIdIsInProgress(TransactionId xid)
{
	bool		result = false;
	SISeg	   *segP = shmInvalBuffer;
	ProcState  *stateP = segP->procState;
	int			index;

	SpinAcquire(SInvalLock);

	for (index = 0; index < segP->lastBackend; index++)
	{
		SHMEM_OFFSET pOffset = stateP[index].procStruct;

		if (pOffset != INVALID_OFFSET)
		{
			PROC	   *proc = (PROC *) MAKE_PTR(pOffset);

			if (proc->xid == xid)
			{
				result = true;
				break;
			}
		}
	}

	SpinRelease(SInvalLock);

	return result;
}

/*
 * GetXmaxRecent -- returns oldest transaction that was running
 *					when all current transaction was started.
 *					It's used by vacuum to decide what deleted
 *					tuples must be preserved in a table.
 */
void
GetXmaxRecent(TransactionId *XmaxRecent)
{
	SISeg	   *segP = shmInvalBuffer;
	ProcState  *stateP = segP->procState;
	int			index;

	*XmaxRecent = GetCurrentTransactionId();

	SpinAcquire(SInvalLock);

	for (index = 0; index < segP->lastBackend; index++)
	{
		SHMEM_OFFSET pOffset = stateP[index].procStruct;

		if (pOffset != INVALID_OFFSET)
		{
			PROC	   *proc = (PROC *) MAKE_PTR(pOffset);
			TransactionId xmin;

			xmin = proc->xmin;	/* we don't use spin-locking in
								 * AbortTransaction() ! */
			if (proc == MyProc || xmin < FirstTransactionId)
				continue;
			if (xmin < *XmaxRecent)
				*XmaxRecent = xmin;
		}
	}

	SpinRelease(SInvalLock);
}

/*
 * GetSnapshotData -- returns information about running transactions.
 */
Snapshot
GetSnapshotData(bool serializable)
{
	Snapshot	snapshot = (Snapshot) malloc(sizeof(SnapshotData));
	SISeg	   *segP = shmInvalBuffer;
	ProcState  *stateP = segP->procState;
	int			index;
	int			count = 0;

	/*
	 * There can be no more than lastBackend active transactions, so this
	 * is enough space:
	 */
	snapshot->xip = (TransactionId *)
		malloc(segP->lastBackend * sizeof(TransactionId));
	snapshot->xmin = GetCurrentTransactionId();

	SpinAcquire(SInvalLock);

	/*
	 * Unfortunately, we have to call ReadNewTransactionId() after
	 * acquiring SInvalLock above. It's not good because
	 * ReadNewTransactionId() does SpinAcquire(OidGenLockId) but
	 * _necessary_.
	 */
	ReadNewTransactionId(&(snapshot->xmax));

	for (index = 0; index < segP->lastBackend; index++)
	{
		SHMEM_OFFSET pOffset = stateP[index].procStruct;

		if (pOffset != INVALID_OFFSET)
		{
			PROC	   *proc = (PROC *) MAKE_PTR(pOffset);
			TransactionId xid;

			/*
			 * We don't use spin-locking when changing proc->xid in
			 * GetNewTransactionId() and in AbortTransaction() !..
			 */
			xid = proc->xid;
			if (proc == MyProc ||
				xid < FirstTransactionId || xid >= snapshot->xmax)
			{

				/*
				 * Seems that there is no sense to store xid >=
				 * snapshot->xmax (what we got from ReadNewTransactionId
				 * above) in snapshot->xip - we just assume that all xacts
				 * with such xid-s are running and may be ignored.
				 */
				continue;
			}
			if (xid < snapshot->xmin)
				snapshot->xmin = xid;
			snapshot->xip[count] = xid;
			count++;
		}
	}

	if (serializable)
		MyProc->xmin = snapshot->xmin;
	/* Serializable snapshot must be computed before any other... */
	Assert(MyProc->xmin != InvalidTransactionId);

	SpinRelease(SInvalLock);

	snapshot->xcnt = count;
	return snapshot;
}

/*
 * GetUndoRecPtr -- returns oldest PROC->logRec.
 */
XLogRecPtr	GetUndoRecPtr(void);

XLogRecPtr
GetUndoRecPtr(void)
{
	SISeg	   *segP = shmInvalBuffer;
	ProcState  *stateP = segP->procState;
	XLogRecPtr	urec = {0, 0};
	XLogRecPtr	tempr;
	int			index;

	SpinAcquire(SInvalLock);

	for (index = 0; index < segP->lastBackend; index++)
	{
		SHMEM_OFFSET pOffset = stateP[index].procStruct;

		if (pOffset != INVALID_OFFSET)
		{
			PROC	   *proc = (PROC *) MAKE_PTR(pOffset);
			tempr = proc->logRec;
			if (tempr.xrecoff == 0)
				continue;
			if (urec.xrecoff != 0 && XLByteLT(urec, tempr))
				continue;
			urec = tempr;
		}
	}

	SpinRelease(SInvalLock);

	return(urec);
}
