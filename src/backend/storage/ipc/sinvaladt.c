/*-------------------------------------------------------------------------
 *
 * sinvaladt.c
 *	  POSTGRES shared cache invalidation segment definitions.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/ipc/sinvaladt.c,v 1.17 1999/02/19 06:06:03 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

#include "postgres.h"

#include "storage/ipc.h"
#include "storage/backendid.h"
#include "storage/sinvaladt.h"
#include "storage/lmgr.h"
#include "utils/palloc.h"
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
	ProcState  *stateP;

	stateP = NULL;

	for (index = 0; index < MAXBACKENDS; index++)
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

	for (index++; index < MAXBACKENDS; index++)
	{
		if (segInOutP->procState[index].tag == backendTag)
		{
			elog(FATAL, "SIAssignBackendId: tag %d found twice",
				 backendTag);
		}
	}

	Assert(stateP);

	if (stateP->tag != InvalidBackendTag)
	{
		if (stateP->tag == backendTag)
		{
			elog(NOTICE, "SIAssignBackendId: reusing tag %d",
				 backendTag);
		}
		else
		{
			elog(NOTICE,
				 "SIAssignBackendId: discarding tag %d",
				 stateP->tag);
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
/* SIComputeSize()	- retuns the size of a buffer segment				*/
/************************************************************************/
static SISegOffsets *
SIComputeSize(int *segSize)
{
	int			A,
				B,
				a,
				b,
				totalSize;
	SISegOffsets *oP;

	A = 0;
	a = SizeSISeg;				/* offset to first data entry */
	b = SizeOfOneSISegEntry * MAXNUMMESSAGES;
	B = A + a + b;
	totalSize = B - A;
	*segSize = totalSize;

	oP = (SISegOffsets *) palloc(sizeof(SISegOffsets));
	oP->startSegment = A;
	oP->offsetToFirstEntry = a; /* relatiove to A */
	oP->offsetToEndOfSegemnt = totalSize;		/* relative to A */
	return oP;
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
static int
SIGetProcStateLimit(SISeg *segP, int i)
{
	return segP->procState[i].limit;
}

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
static SISegEntry *
SIGetNextDataEntry(SISeg *segP, Offset offset)
{
	SISegEntry *eP;

	if (offset == InvalidOffset)
		return NULL;

	eP = (SISegEntry *) ((Pointer) segP +
						 SIGetStartEntrySection(segP) +
						 offset);
	return eP;
}


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

	for (i = 0; i < MAXBACKENDS; i++)
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
/* SIDelDataEntry(segP)		- free the FIRST entry						*/
/************************************************************************/
bool
SIDelDataEntry(SISeg *segP)
{
	SISegEntry *e1P;

	if (!SIDecNumEntries(segP, 1))
	{
		/* no entries in buffer */
		return false;
	}

	e1P = SIGetFirstDataEntry(segP);
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
	SIDecProcLimit(segP, 1);
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

	for (i = 0; i < MAXBACKENDS; i++)
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
/* SIReadEntryData(segP, backendId, function)							*/
/*						- marks messages to be read by id				*/
/*						  and executes function							*/
/************************************************************************/
void
SIReadEntryData(SISeg *segP,
				int backendId,
				void (*invalFunction) (),
				void (*resetFunction) ())
{
	int			i = 0;
	SISegEntry *data;

	Assert(segP->procState[backendId - 1].tag == MyBackendTag);

	if (!segP->procState[backendId - 1].resetState)
	{
		/* invalidate data, but only those, you have not seen yet !! */
		/* therefore skip read messages */
		data = SIGetNthDataEntry(segP,
						   SIGetProcStateLimit(segP, backendId - 1) + 1);
		while (data != NULL)
		{
			i++;
			segP->procState[backendId - 1].limit++;		/* one more message read */
			invalFunction(data->entryData.cacheId,
						  data->entryData.hashIndex,
						  &data->entryData.pointerData);
			data = SIGetNextDataEntry(segP, data->next);
		}
		/* SIDelExpiredDataEntries(segP); */
	}
	else
	{
		/* backend must not read messages, its own state has to be reset	 */
		elog(NOTICE, "SIReadEntryData: cache state reset");
		resetFunction();		/* XXXX call it here, parameters? */

		/* new valid state--mark all messages "read" */
		segP->procState[backendId - 1].resetState = false;
		segP->procState[backendId - 1].limit = SIGetNumEntries(segP);
	}
	/* check whether we can remove dead messages							*/
	if (i > MAXNUMMESSAGES)
		elog(FATAL, "SIReadEntryData: Invalid segment state");
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
	for (i = 0; i < MAXBACKENDS; i++)
	{
		h = SIGetProcStateLimit(segP, i);
		if (h >= 0)
		{						/* backend active */
			if (h < min)
				min = h;
		}
	}
	if (min != 9999999)
	{
		/* we can remove min messages */
		for (i = 1; i <= min; i++)
		{
			/* this  adjusts also the state limits! */
			if (!SIDelDataEntry(segP))
				elog(FATAL, "SIDelExpiredDataEntries: Invalid segment state");
		}
	}
}



/************************************************************************/
/* SISegInit(segP)	- initializes the segment							*/
/************************************************************************/
static void
SISegInit(SISeg *segP)
{
	SISegOffsets *oP;
	int			segSize,
				i;
	SISegEntry *eP;

	oP = SIComputeSize(&segSize);
	/* set sempahore ids in the segment */
	/* XXX */
	SISetStartEntrySection(segP, oP->offsetToFirstEntry);
	SISetEndEntrySection(segP, oP->offsetToEndOfSegemnt);
	SISetStartFreeSpace(segP, 0);
	SISetStartEntryChain(segP, InvalidOffset);
	SISetEndEntryChain(segP, InvalidOffset);
	SISetNumEntries(segP, 0);
	SISetMaxNumEntries(segP, MAXNUMMESSAGES);
	for (i = 0; i < MAXBACKENDS; i++)
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

	/*
	 * Be tidy
	 */
	pfree(oP);

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
		elog(NOTICE, "SISegmentAttach: Could not attach segment");
		elog(FATAL, "SISegmentAttach: %m");
	}
}


/************************************************************************/
/* SISegmentInit(killExistingSegment, key)	initialize segment			*/
/************************************************************************/
int
SISegmentInit(bool killExistingSegment, IPCKey key)
{
	SISegOffsets *oP;
	int			segSize;
	IpcMemoryId shmId;
	bool		create;

	if (killExistingSegment)
	{
		/* Kill existing segment */
		/* set semaphore */
		SISegmentKill(key);

		/* Get a shared segment */

		oP = SIComputeSize(&segSize);

		/*
		 * Be tidy
		 */
		pfree(oP);

		create = true;
		shmId = SISegmentGet(key, segSize, create);
		if (shmId < 0)
		{
			perror("SISegmentGet: failed");
			return -1;			/* an error */
		}

		/* Attach the shared cache invalidation  segment */
		/* sets the global variable shmInvalBuffer */
		SISegmentAttach(shmId);

		/* Init shared memory table */
		SISegInit(shmInvalBuffer);
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
