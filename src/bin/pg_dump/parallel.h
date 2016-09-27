/*-------------------------------------------------------------------------
 *
 * parallel.h
 *
 *	Parallel support for pg_dump and pg_restore
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/parallel.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_DUMP_PARALLEL_H
#define PG_DUMP_PARALLEL_H

#include "pg_backup_archiver.h"

/* Function to call in master process on completion of a worker task */
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

/* Worker process statuses */
typedef enum
{
	WRKR_IDLE,
	WRKR_WORKING,
	WRKR_TERMINATED
} T_WorkerStatus;

/*
 * Per-parallel-worker state of parallel.c.
 *
 * Much of this is valid only in the master process (or, on Windows, should
 * be touched only by the master thread).  But the AH field should be touched
 * only by workers.  The pipe descriptors are valid everywhere.
 */
typedef struct ParallelSlot
{
	T_WorkerStatus workerStatus;	/* see enum above */

	/* These fields are valid if workerStatus == WRKR_WORKING: */
	TocEntry   *te;				/* item being worked on */
	ParallelCompletionPtr callback;		/* function to call on completion */
	void	   *callback_data;	/* passthru data for it */

	ArchiveHandle *AH;			/* Archive data worker is using */

	int			pipeRead;		/* master's end of the pipes */
	int			pipeWrite;
	int			pipeRevRead;	/* child's end of the pipes */
	int			pipeRevWrite;

	/* Child process/thread identity info: */
#ifdef WIN32
	uintptr_t	hThread;
	unsigned int threadId;
#else
	pid_t		pid;
#endif
} ParallelSlot;

/* Overall state for parallel.c */
typedef struct ParallelState
{
	int			numWorkers;		/* allowed number of workers */
	ParallelSlot *parallelSlot; /* array of numWorkers slots */
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

#endif   /* PG_DUMP_PARALLEL_H */
