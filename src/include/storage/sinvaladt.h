/*-------------------------------------------------------------------------
 *
 * sinvaladt.h--
 *	  POSTGRES shared cache invalidation segment definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: sinvaladt.h,v 1.5 1997/09/08 02:39:13 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SINVALADT_H
#define SINVALADT_H

#include <storage/itemptr.h>
#include <storage/ipc.h>

/*
 * The structure of the shared cache invaidation segment
 *
 */
/*
A------------- Header info --------------
	criticalSectionSemaphoreId
	generalSemaphoreId
	startEntrySection	(offset a)
	endEntrySection		(offset a + b)
	startFreeSpace		(offset relative to B)
	startEntryChain		(offset relatiev to B)
	endEntryChain		(offset relative to B)
	numEntries
	maxNumEntries
	procState[MaxBackendId] --> limit
								resetState (bool)
a								tag (POSTID)
B------------- Start entry section -------
	SISegEntry	--> entryData --> ... (see	SharedInvalidData!)
					isfree	(bool)
					next  (offset to next entry in chain )
b	  .... (dynamically growing down)
C----------------End shared segment -------

*/

/* Parameters (configurable)  *******************************************/
#define MaxBackendId 32			/* maximum number of backends		*/
#define MAXNUMMESSAGES 1000		/* maximum number of messages in seg */


#define InvalidOffset	1000000000		/* a invalid offset  (End of
										 * chain) */

typedef struct ProcState
{
	int			limit;			/* the number of read messages			*/
	bool		resetState;		/* true, if backend has to reset its state */
	int			tag;			/* special tag, recieved from the
								 * postmaster */
}			ProcState;


typedef struct SISeg
{
	IpcSemaphoreId criticalSectionSemaphoreId;	/* semaphore id		*/
	IpcSemaphoreId generalSemaphoreId;	/* semaphore id		*/
	Offset		startEntrySection;		/* (offset a)					*/
	Offset		endEntrySection;/* (offset a + b)				*/
	Offset		startFreeSpace; /* (offset relative to B)		*/
	Offset		startEntryChain;/* (offset relative to B)		*/
	Offset		endEntryChain;	/* (offset relative to B)		*/
	int			numEntries;
	int			maxNumEntries;
	ProcState	procState[MaxBackendId];		/* reflects the
												 * invalidation state */
	/* here starts the entry section, controlled by offsets */
}			SISeg;

#define SizeSISeg	  sizeof(SISeg)

typedef struct SharedInvalidData
{
	int			cacheId;		/* XXX */
	Index		hashIndex;
	ItemPointerData pointerData;
}			SharedInvalidData;

typedef SharedInvalidData *SharedInvalid;


typedef struct SISegEntry
{
	SharedInvalidData entryData;/* the message data */
	bool		isfree;			/* entry free? */
	Offset		next;			/* offset to next entry */
}			SISegEntry;

#define SizeOfOneSISegEntry   sizeof(SISegEntry)

typedef struct SISegOffsets
{
	Offset		startSegment;	/* always 0 (for now) */
	Offset		offsetToFirstEntry;		/* A + a = B */
	Offset		offsetToEndOfSegemnt;	/* A + a + b */
}			SISegOffsets;


/****************************************************************************/
/* synchronization of the shared buffer access								*/
/*	  access to the buffer is synchronized by the lock manager !!			*/
/****************************************************************************/

#define SI_LockStartValue  255
#define SI_SharedLock	  (-1)
#define SI_ExclusiveLock  (-255)

extern SISeg *shmInvalBuffer;

/*
 * prototypes for functions in sinvaladt.c
 */
extern int	SIBackendInit(SISeg * segInOutP);
extern int	SISegmentInit(bool killExistingSegment, IPCKey key);

extern bool SISetDataEntry(SISeg * segP, SharedInvalidData * data);
extern void SISetProcStateInvalid(SISeg * segP);
extern bool SIDelDataEntry(SISeg * segP);
extern void
SIReadEntryData(SISeg * segP, int backendId,
				void (*invalFunction) (), void (*resetFunction) ());
extern void SIDelExpiredDataEntries(SISeg * segP);

#endif							/* SINVALADT_H */
