/*-------------------------------------------------------------------------
 *
 * sinvaladt.c
 *	  POSTGRES shared cache invalidation segment definitions.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/ipc/sinvaladt.c,v 1.24 1999/09/04 18:36:45 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <signal.h>
#include <unistd.h>

#include "postgres.h"

#include "storage/backendid.h"
#include "storage/lmgr.h"
#include "utils/trace.h"

/* ----------------
 *		global variable notes
 *
 *		SharedInvalidationSemaphore
 *
 *		shmInvalBuffer
 *				the shared buffer segment, set by SISegmentAttach()
 *
 *		MyBackendId
 *				might be removed later, used only for
 *				debugging in debug routines (end of file)
 *
 *		SIDbId
 *				identification of buffer (disappears)
 *
 *		SIRelId			\
 *		SIDummyOid		 \	identification of buffer
 *		SIXidData		 /
 *		SIXid			/
 *
 *	XXX This file really needs to be cleaned up.  We switched to using
 *		spinlocks to protect critical sections (as opposed to using fake
 *		relations and going through the lock manager) and some of the old
 *		cruft was 'ifdef'ed out, while other parts (now unused) are still
 *		compiled into the system. -mer 5/24/92
 * ----------------
 */
#ifdef HAS_TEST_AND_SET
int			SharedInvalidationLockId;

#else
IpcSemaphoreId SharedInvalidationSemaphore;

#endif

SISeg	   *shmInvalBuffer;
extern BackendId MyBackendId;

static void CleanupInvalidationState(int status, SISeg *segInOutP);
static BackendId SIAssignBackendId(SISeg *segInOutP, BackendTag backendTag);
static int	SIGetNumEntries(SISeg *segP);

/************************************************************************/
/* SISetActiveProcess(segP, backendId)	set the backend status active	*/
/*		should be called only by the postmaster when creating a backend */
/************************************************************************/
/* XXX I suspect that the segP parameter is extraneous. -hirohama */
static void
SISetActiveProcess(SISeg *segInOutP, BackendId backendId)
{
	/* mark all messages as read */

	/* Assert(segP->procState[backendId - 1].tag == MyBackendTag); */

	segInOutP->procState[backendId - 1].resetState = false;
	segInOutP->procState[backendId - 1].limit = SIGetNumEntries(segInOutP);
}

/****************************************************************************/
/* SIBackendInit()	initializes a backend to operate on the buffer			*/
/****************************************************************************/
int
SIBackendInit(SISeg *segInOutP)
{
	LockRelId	LtCreateRelId();
	TransactionId LMITransactionIdCopy();

	Assert(MyBackendTag > 0);

	MyBackendId = SIAssignBackendId(segInOutP, MyBackendTag);
	if (MyBackendId == InvalidBackendTag)
		return 0;

#ifdef	INVALIDDEBUG
	elog(DEBUG, "SIBackendInit: backend tag %d; backend id %d.",
		 MyBackendTag, MyBackendId);
#endif	 /* INVALIDDEBUG */

	SISetActiveProcess(segInOutP, MyBackendId);
	on_shmem_exit(CleanupInvalidationState, (caddr_t) segInOutP);
	return 1;
}

/* ----------------
 *		SIAssignBackendId
 * ----------------
 */
static BackendId
SIAssignBackendId(SISeg *segInOutP, BackendTag backendTag)
{
	Index		index;
	ProcState  *stateP = NULL;

	for (index = 0; index < segInOutP->maxBackends; index++)
	{
		if (segInOutP->procState[index].tag == InvalidBackendTag ||
			segInOutP->procState[index].tag == backendTag)
		{
			stateP = &segInOutP->procState[index];
			break;
		}

		if (!PointerIsValid(stateP) ||
			(segInOutP->procState[index].resetState &&
			 (!stateP->resetState ||
			  stateP->tag < backendTag)) ||
			(!stateP->resetState &&
			 (segInOutP->procState[index].limit <
			  stateP->limit ||
			  stateP->tag < backendTag)))
			stateP = &segInOutP->procState[index];
	}

	/* verify that all "procState" entries checked for matching tags */

	for (index++; index < segInOutP->maxBackends; index++)
	{
		if (segInOutP->procState[index].tag == backendTag)
			elog(FATAL, "SIAssignBackendId: tag %d found twice", backendTag);
	}

	Assert(stateP);

	if (stateP->tag != InvalidBackendTag)
	{
		if (stateP->tag == backendTag)
			elog(NOTICE, "SIAssignBackendId: reusing tag %d", backendTag);
		else
		{
			elog(NOTICE, "SIAssignBackendId: discarding tag %d", stateP->tag);
			return InvalidBackendTag;
		}
	}

	stateP->tag = backendTag;

	return 1 + stateP - &segInOutP->procState[0];
}


/************************************************************************/
/* The following function should be called only by the postmaster !!	*/
/************************************************************************/

/************************************************************************/
/* SISetDeadProcess(segP, backendId)  set the backend status DEAD		*/
/*		should be called only by the postmaster when a backend died		*/
/************************************************************************/
static void
SISetDeadProcess(SISeg *segP, int backendId)
{
	/* XXX call me.... */

	segP->procState[backendId - 1].resetState = false;
	segP->procState[backendId - 1].limit = -1;
	segP->procState[backendId - 1].tag = InvalidBackendTag;
}

/*
 * CleanupInvalidationState
 * Note:
 *		This is a temporary hack.  ExitBackend should call this instead
 *		of exit (via on_shmem_exit).
 */
static void
CleanupInvalidationState(int status,	/* XXX */
						 SISeg *segInOutP)		/* XXX style */
{
	Assert(PointerIsValid(segInOutP));

	SISetDeadProcess(segInOutP, MyBackendId);
}


/************************************************************************/
/* SIComputeSize()	- compute size and offsets for SI segment			*/
/************************************************************************/
static void
SIComputeSize(SISegOffsets *oP, int maxBackends)
{
	int			A,
				B,
				a,
				b,
				totalSize;

	A = 0;
	/* sizeof(SISeg) includes the first ProcState entry */
	a = sizeof(SISeg) + sizeof(ProcState) * (maxBackends - 1);
	a = MAXALIGN(a);			/* offset to first data entry */
	b = sizeof(SISegEntry) * MAXNUMMESSAGES;
	B = A + a + b;
	B = MAXALIGN(B);
	totalSize = B - A;

	oP->startSegment = A;
	oP->offsetToFirstEntry = a; /* relative to A */
	oP->offsetToEndOfSegment = totalSize;		/* relative to A */
}


/************************************************************************/
/* SISetStartEntrySection(segP, offset)		- sets the offset			*/
/************************************************************************/
static void
SISetStartEntrySection(SISeg *segP, Offset offset)
{
	segP->startEntrySection = offset;
}

/************************************************************************/
/* SIGetStartEntrySection(segP)		- returnss the offset				*/
/************************************************************************/
static Offset
SIGetStartEntrySection(SISeg *segP)
{
	return segP->startEntrySection;
}


/************************************************************************/
/* SISetEndEntrySection(segP, offset)	- sets the offset				*/
/************************************************************************/
static void
SISetEndEntrySection(SISeg *segP, Offset offset)
{
	segP->endEntrySection = offset;
}

/************************************************************************/
/* SISetEndEntryChain(segP, offset)		- sets the offset				*/
/************************************************************************/
static void
SISetEndEntryChain(SISeg *segP, Offset offset)
{
	segP->endEntryChain = offset;
}

/************************************************************************/
/* SIGetEndEntryChain(segP)		- returnss the offset					*/
/************************************************************************/
static Offset
SIGetEndEntryChain(SISeg *segP)
{
	return segP->endEntryChain;
}

/************************************************************************/
/* SISetStartEntryChain(segP, offset)	- sets the offset				*/
/************************************************************************/
static void
SISetStartEntryChain(SISeg *segP, Offset offset)
{
	segP->startEntryChain = offset;
}

/************************************************************************/
/* SIGetStartEntryChain(segP)	- returns  the offset					*/
/************************************************************************/
static Offset
SIGetStartEntryChain(SISeg *segP)
{
	return segP->startEntryChain;
}

/************************************************************************/
/* SISetNumEntries(segP, num)	sets the current nuber of entries		*/
/************************************************************************/
static bool
SISetNumEntries(SISeg *segP, int num)
{
	if (num <= MAXNUMMESSAGES)
	{
		segP->numEntries = num;
		return true;
	}
	else
	{
		return false;			/* table full */
	}
}

/************************************************************************/
/* SIGetNumEntries(segP)	- returns the current nuber of entries		*/
/************************************************************************/
static int
SIGetNumEntries(SISeg *segP)
{
	return segP->numEntries;
}


/************************************************************************/
/* SISetMaxNumEntries(segP, num)	sets the maximal number of entries	*/
/************************************************************************/
static bool
SISetMaxNumEntries(SISeg *segP, int num)
{
	if (num <= MAXNUMMESSAGES)
	{
		segP->maxNumEntries = num;
		return true;
	}
	else
	{
		return false;			/* wrong number */
	}
}


/************************************************************************/
/* SIGetProcStateLimit(segP, i) returns the limit of read messages		*/
/************************************************************************/

#define SIGetProcStateLimit(segP,i) \
		((segP)->procState[i].limit)

/************************************************************************/
/* SIIncNumEntries(segP, num)	increments the current nuber of entries */
/************************************************************************/
static bool
SIIncNumEntries(SISeg *segP, int num)
{

	/*
	 * Try to prevent table overflow. When the table is 70% full send a
	 * SIGUSR2 to the postmaster which will send it back to all the
	 * backends. This will be handled by Async_NotifyHandler() with a
	 * StartTransactionCommand() which will flush unread SI entries for
	 * each backend.									dz - 27 Jan 1998
	 */
	if (segP->numEntries == (MAXNUMMESSAGES * 70 / 100))
	{
		TPRINTF(TRACE_VERBOSE,
			"SIIncNumEntries: table is 70%% full, signaling postmaster");
		kill(getppid(), SIGUSR2);
	}

	if ((segP->numEntries + num) <= MAXNUMMESSAGES)
	{
		segP->numEntries = segP->numEntries + num;
		return true;
	}
	else
	{
		return false;			/* table full */
	}
}

/************************************************************************/
/* SIDecNumEntries(segP, num)	decrements the current nuber of entries */
/************************************************************************/
static bool
SIDecNumEntries(SISeg *segP, int num)
{
	if ((segP->numEntries - num) >= 0)
	{
		segP->numEntries = segP->numEntries - num;
		return true;
	}
	else
	{
		return false;			/* not enough entries in table */
	}
}

/************************************************************************/
/* SISetStartFreeSpace(segP, offset)  - sets the offset					*/
/************************************************************************/
static void
SISetStartFreeSpace(SISeg *segP, Offset offset)
{
	segP->startFreeSpace = offset;
}

/************************************************************************/
/* SIGetStartFreeSpace(segP)  - returns the offset						*/
/************************************************************************/
static Offset
SIGetStartFreeSpace(SISeg *segP)
{
	return segP->startFreeSpace;
}



/************************************************************************/
/* SIGetFirstDataEntry(segP)  returns first data entry					*/
/************************************************************************/
static SISegEntry *
SIGetFirstDataEntry(SISeg *segP)
{
	SISegEntry *eP;
	Offset		startChain;

	startChain = SIGetStartEntryChain(segP);

	if (startChain == InvalidOffset)
		return NULL;

	eP = (SISegEntry *) ((Pointer) segP +
						 SIGetStartEntrySection(segP) +
						 startChain);
	return eP;
}


/************************************************************************/
/* SIGetLastDataEntry(segP)  returns last data entry in the chain		*/
/************************************************************************/
static SISegEntry *
SIGetLastDataEntry(SISeg *segP)
{
	SISegEntry *eP;
	Offset		endChain;

	endChain = SIGetEndEntryChain(segP);

	if (endChain == InvalidOffset)
		return NULL;

	eP = (SISegEntry *) ((Pointer) segP +
						 SIGetStartEntrySection(segP) +
						 endChain);
	return eP;
}

/************************************************************************/
/* SIGetNextDataEntry(segP, offset)  returns next data entry			*/
/************************************************************************/
#define SIGetNextDataEntry(segP,offset) \
	(((offset) == InvalidOffset) ? (SISegEntry *) NULL : \
	 (SISegEntry *) ((Pointer) (segP) + \
					 (segP)->startEntrySection + \
					 (Offset) (offset)))

/************************************************************************/
/* SIGetNthDataEntry(segP, n)	returns the n-th data entry in chain	*/
/************************************************************************/
static SISegEntry *
SIGetNthDataEntry(SISeg *segP,
				  int n)		/* must range from 1 to MaxMessages */
{
	SISegEntry *eP;
	int			i;

	if (n <= 0)
		return NULL;

	eP = SIGetFirstDataEntry(segP);
	for (i = 1; i < n; i++)
	{
		/* skip one and get the next	*/
		eP = SIGetNextDataEntry(segP, eP->next);
	}

	return eP;
}

/************************************************************************/
/* SIEntryOffset(segP, entryP)	 returns the offset for an pointer		*/
/************************************************************************/
static Offset
SIEntryOffset(SISeg *segP, SISegEntry *entryP)
{
	/* relative to B !! */
	return ((Offset) ((Pointer) entryP -
					  (Pointer) segP -
					  SIGetStartEntrySection(segP)));
}


/************************************************************************/
/* SISetDataEntry(segP, data)  - sets a message in the segemnt			*/
/************************************************************************/
bool
SISetDataEntry(SISeg *segP, SharedInvalidData *data)
{
	Offset		offsetToNewData;
	SISegEntry *eP,
			   *lastP;

	if (!SIIncNumEntries(segP, 1))
		return false;			/* no space */

	/* get a free entry */
	offsetToNewData = SIGetStartFreeSpace(segP);
	eP = SIGetNextDataEntry(segP, offsetToNewData);		/* it's a free one */
	SISetStartFreeSpace(segP, eP->next);
	/* fill it up */
	eP->entryData = *data;
	eP->isfree = false;
	eP->next = InvalidOffset;

	/* handle insertion point at the end of the chain !! */
	lastP = SIGetLastDataEntry(segP);
	if (lastP == NULL)
	{
		/* there is no chain, insert the first entry */
		SISetStartEntryChain(segP, SIEntryOffset(segP, eP));
	}
	else
	{
		/* there is a last entry in the chain */
		lastP->next = SIEntryOffset(segP, eP);
	}
	SISetEndEntryChain(segP, SIEntryOffset(segP, eP));
	return true;
}


/************************************************************************/
/* SIDecProcLimit(segP, num)  decrements all process limits				*/
/************************************************************************/
static void
SIDecProcLimit(SISeg *segP, int num)
{
	int			i;

	for (i = 0; i < segP->maxBackends; i++)
	{
		/* decrement only, if there is a limit > 0	*/
		if (segP->procState[i].limit > 0)
		{
			segP->procState[i].limit = segP->procState[i].limit - num;
			if (segP->procState[i].limit < 0)
			{
				/* limit was not high enough, reset to zero */
				/* negative means it's a dead backend	    */
				segP->procState[i].limit = 0;
			}
		}
	}
}


/************************************************************************/
/* SIDelDataEntries(segP, n)		- free the FIRST n entries			*/
/************************************************************************/
bool
SIDelDataEntries(SISeg *segP, int n)
{
	int			i;

	if (n <= 0)
		return false;

	if (!SIDecNumEntries(segP, n))
	{
		/* not that many entries in buffer */
		return false;
	}

	for (i = 1; i <= n; i++)
	{
		SISegEntry *e1P = SIGetFirstDataEntry(segP);
		SISetStartEntryChain(segP, e1P->next);
		if (SIGetStartEntryChain(segP) == InvalidOffset)
		{
			/* it was the last entry */
			SISetEndEntryChain(segP, InvalidOffset);
		}
		/* free the entry */
		e1P->isfree = true;
		e1P->next = SIGetStartFreeSpace(segP);
		SISetStartFreeSpace(segP, SIEntryOffset(segP, e1P));
	}

	SIDecProcLimit(segP, n);
	return true;
}



/************************************************************************/
/* SISetProcStateInvalid(segP)	checks and marks a backends state as	*/
/*									invalid								*/
/************************************************************************/
void
SISetProcStateInvalid(SISeg *segP)
{
	int			i;

	for (i = 0; i < segP->maxBackends; i++)
	{
		if (segP->procState[i].limit == 0)
		{
			/* backend i didn't read any message    	    	    	 */
			segP->procState[i].resetState = true;

			/*
			 * XXX signal backend that it has to reset its internal cache
			 * ?
			 */
		}
	}
}

/************************************************************************/
/* SIGetDataEntry(segP, backendId, data)								*/
/*		get next SI message for specified backend, if there is one		*/
/*																		*/
/*		Possible return values:											*/
/*			0: no SI message available									*/
/*			1: next SI message has been extracted into *data			*/
/*				(there may be more messages available after this one!)	*/
/*		   -1: SI reset message extracted								*/
/************************************************************************/
int
SIGetDataEntry(SISeg *segP, int backendId,
			   SharedInvalidData *data)
{
	SISegEntry *msg;

	Assert(segP->procState[backendId - 1].tag == MyBackendTag);

	if (segP->procState[backendId - 1].resetState)
	{
		/* new valid state--mark all messages "read" */
		segP->procState[backendId - 1].resetState = false;
		segP->procState[backendId - 1].limit = SIGetNumEntries(segP);
		return -1;
	}

	/* Get next message for this backend, if any */

	/* This is fairly inefficient if there are many messages,
	 * but normally there should not be...
	 */
	msg = SIGetNthDataEntry(segP,
							SIGetProcStateLimit(segP, backendId - 1) + 1);

	if (msg == NULL)
		return 0;				/* nothing to read */

	*data = msg->entryData;		/* return contents of message */

	segP->procState[backendId - 1].limit++;		/* one more message read */

	/* There may be other backends that haven't read the message,
	 * so we cannot delete it here.
	 * SIDelExpiredDataEntries() should be called to remove dead messages.
	 */
	return 1;					/* got a message */
}

/************************************************************************/
/* SIDelExpiredDataEntries	(segP)	- removes irrelevant messages		*/
/************************************************************************/
void
SIDelExpiredDataEntries(SISeg *segP)
{
	int			min,
				i,
				h;

	min = 9999999;
	for (i = 0; i < segP->maxBackends; i++)
	{
		h = SIGetProcStateLimit(segP, i);
		if (h >= 0)
		{						/* backend active */
			if (h < min)
				min = h;
		}
	}
	if (min < 9999999 && min > 0)
	{
		/* we can remove min messages */
		/* this adjusts also the state limits! */
		if (!SIDelDataEntries(segP, min))
			elog(FATAL, "SIDelExpiredDataEntries: Invalid segment state");
	}
}



/************************************************************************/
/* SISegInit(segP)	- initializes the segment							*/
/************************************************************************/
static void
SISegInit(SISeg *segP, SISegOffsets *oP, int maxBackends)
{
	int			i;
	SISegEntry *eP;

	/* set semaphore ids in the segment */
	/* XXX */
	SISetStartEntrySection(segP, oP->offsetToFirstEntry);
	SISetEndEntrySection(segP, oP->offsetToEndOfSegment);
	SISetStartFreeSpace(segP, 0);
	SISetStartEntryChain(segP, InvalidOffset);
	SISetEndEntryChain(segP, InvalidOffset);
	SISetNumEntries(segP, 0);
	SISetMaxNumEntries(segP, MAXNUMMESSAGES);
	segP->maxBackends = maxBackends;
	for (i = 0; i < segP->maxBackends; i++)
	{
		segP->procState[i].limit = -1;	/* no backend active  !! */
		segP->procState[i].resetState = false;
		segP->procState[i].tag = InvalidBackendTag;
	}
	/* construct a chain of free entries							*/
	for (i = 1; i < MAXNUMMESSAGES; i++)
	{
		eP = (SISegEntry *) ((Pointer) segP +
							 SIGetStartEntrySection(segP) +
							 (i - 1) * sizeof(SISegEntry));
		eP->isfree = true;
		eP->next = i * sizeof(SISegEntry);		/* relative to B */
	}
	/* handle the last free entry separate							*/
	eP = (SISegEntry *) ((Pointer) segP +
						 SIGetStartEntrySection(segP) +
						 (MAXNUMMESSAGES - 1) * sizeof(SISegEntry));
	eP->isfree = true;
	eP->next = InvalidOffset;	/* it's the end of the chain !! */
}



/************************************************************************/
/* SISegmentKill(key)	- kill any segment								*/
/************************************************************************/
static void
SISegmentKill(int key)			/* the corresponding key for the segment */
{
	IpcMemoryKill(key);
}


/************************************************************************/
/* SISegmentGet(key, size)	- get a shared segment of size <size>		*/
/*				  returns a segment id									*/
/************************************************************************/
static IpcMemoryId
SISegmentGet(int key,			/* the corresponding key for the segment */
			 int size,			/* size of segment in bytes				 */
			 bool create)
{
	IpcMemoryId shmid;

	if (create)
		shmid = IpcMemoryCreate(key, size, IPCProtection);
	else
		shmid = IpcMemoryIdGet(key, size);
	return shmid;
}

/************************************************************************/
/* SISegmentAttach(shmid)	- attach a shared segment with id shmid		*/
/************************************************************************/
static void
SISegmentAttach(IpcMemoryId shmid)
{
	shmInvalBuffer = (struct SISeg *) IpcMemoryAttach(shmid);
	if (shmInvalBuffer == IpcMemAttachFailed)
	{
		/* XXX use validity function */
		elog(FATAL, "SISegmentAttach: Could not attach segment: %m");
	}
}


/************************************************************************/
/* SISegmentInit()			initialize SI segment						*/
/*																		*/
/* NB: maxBackends param is only valid when killExistingSegment is true	*/
/************************************************************************/
int
SISegmentInit(bool killExistingSegment, IPCKey key, int maxBackends)
{
	SISegOffsets offsets;
	IpcMemoryId shmId;
	bool		create;

	if (killExistingSegment)
	{
		/* Kill existing segment */
		/* set semaphore */
		SISegmentKill(key);

		/* Get a shared segment */
		SIComputeSize(&offsets, maxBackends);
		create = true;
		shmId = SISegmentGet(key, offsets.offsetToEndOfSegment, create);
		if (shmId < 0)
		{
			perror("SISegmentGet: failed");
			return -1;			/* an error */
		}

		/* Attach the shared cache invalidation  segment */
		/* sets the global variable shmInvalBuffer */
		SISegmentAttach(shmId);

		/* Init shared memory table */
		SISegInit(shmInvalBuffer, &offsets, maxBackends);
	}
	else
	{
		/* use an existing segment */
		create = false;
		shmId = SISegmentGet(key, 0, create);
		if (shmId < 0)
		{
			perror("SISegmentGet: getting an existent segment failed");
			return -1;			/* an error */
		}
		/* Attach the shared cache invalidation segment */
		SISegmentAttach(shmId);
	}
	return 1;
}
