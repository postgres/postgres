/*-------------------------------------------------------------------------
 *
 * sinvaladt.h
 *	  POSTGRES shared cache invalidation segment definitions.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: sinvaladt.h,v 1.34 2003/08/04 02:40:15 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SINVALADT_H
#define SINVALADT_H

#include "storage/shmem.h"
#include "storage/sinval.h"

/*
 * The shared cache invalidation manager is responsible for transmitting
 * invalidation messages between backends.	Any message sent by any backend
 * must be delivered to all already-running backends before it can be
 * forgotten.
 *
 * Conceptually, the messages are stored in an infinite array, where
 * maxMsgNum is the next array subscript to store a submitted message in,
 * minMsgNum is the smallest array subscript containing a message not yet
 * read by all backends, and we always have maxMsgNum >= minMsgNum.  (They
 * are equal when there are no messages pending.)  For each active backend,
 * there is a nextMsgNum pointer indicating the next message it needs to read;
 * we have maxMsgNum >= nextMsgNum >= minMsgNum for every backend.
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
 *
 * The struct type SharedInvalidationMessage, defining the contents of
 * a single message, is defined in sinval.h.
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
	SHMEM_OFFSET procStruct;	/* location of backend's PGPROC struct */
} ProcState;

/* Shared cache invalidation memory segment */
typedef struct SISeg
{
	/*
	 * General state information
	 */
	int			minMsgNum;		/* oldest message still needed */
	int			maxMsgNum;		/* next message number to be assigned */
	int			lastBackend;	/* index of last active procState entry,
								 * +1 */
	int			maxBackends;	/* size of procState array */
	int			freeBackends;	/* number of empty procState slots */

	/*
	 * Circular buffer holding shared-inval messages
	 */
	SharedInvalidationMessage buffer[MAXNUMMESSAGES];

	/*
	 * Per-backend state info.
	 *
	 * We declare procState as 1 entry because C wants a fixed-size array,
	 * but actually it is maxBackends entries long.
	 */
	ProcState	procState[1];	/* reflects the invalidation state */
} SISeg;


extern SISeg *shmInvalBuffer;	/* pointer to the shared inval buffer */


/*
 * prototypes for functions in sinvaladt.c
 */
extern void SIBufferInit(int maxBackends);
extern int	SIBackendInit(SISeg *segP);

extern bool SIInsertDataEntry(SISeg *segP, SharedInvalidationMessage *data);
extern int SIGetDataEntry(SISeg *segP, int backendId,
			   SharedInvalidationMessage *data);
extern void SIDelExpiredDataEntries(SISeg *segP);

#endif   /* SINVALADT_H */
