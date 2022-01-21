/*-------------------------------------------------------------------------
 *
 * parallel.h
 *
 *	Parallel support for pg_dump and pg_restore
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/parallel.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_DUMP_PARALLEL_H
#define PG_DUMP_PARALLEL_H

#include <limits.h>

#include "pg_backup_archiver.h"

/* Function to call in leader process on completion of a worker task */
typedef void (*ParallelCompletionPtr) (ArchiveHandle *AH,
									   TocEntry *te,
									   int status,
									   void *callback_data);

/* Wait options for WaitForWorkers */
typedef enum
{
	WFW_NO_WAIT,
	WFW_GOT_STATUS,
	WFW_ONE_IDLE,
	WFW_ALL_IDLE
} WFW_WaitOption;

/*
 * Maximum number of parallel jobs allowed.
 *
 * On Windows we can only have at most MAXIMUM_WAIT_OBJECTS (= 64 usually)
 * parallel jobs because that's the maximum limit for the
 * WaitForMultipleObjects() call.
 */
#ifdef WIN32
#define PG_MAX_JOBS MAXIMUM_WAIT_OBJECTS
#else
#define PG_MAX_JOBS INT_MAX
#endif

/* ParallelSlot is an opaque struct known only within parallel.c */
typedef struct ParallelSlot ParallelSlot;

/* Overall state for parallel.c */
typedef struct ParallelState
{
	int			numWorkers;		/* allowed number of workers */
	/* these arrays have numWorkers entries, one per worker: */
	TocEntry  **te;				/* item being worked on, or NULL */
	ParallelSlot *parallelSlot; /* private info about each worker */
} ParallelState;

#ifdef WIN32
extern bool parallel_init_done;
extern DWORD mainThreadId;
#endif

extern void init_parallel_dump_utils(void);

extern bool IsEveryWorkerIdle(ParallelState *pstate);
extern void WaitForWorkers(ArchiveHandle *AH, ParallelState *pstate,
						   WFW_WaitOption mode);

extern ParallelState *ParallelBackupStart(ArchiveHandle *AH);
extern void DispatchJobForTocEntry(ArchiveHandle *AH,
								   ParallelState *pstate,
								   TocEntry *te,
								   T_Action act,
								   ParallelCompletionPtr callback,
								   void *callback_data);
extern void ParallelBackupEnd(ArchiveHandle *AH, ParallelState *pstate);

extern void set_archive_cancel_info(ArchiveHandle *AH, PGconn *conn);

#endif							/* PG_DUMP_PARALLEL_H */
