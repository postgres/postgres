/*-------------------------------------------------------------------------
 *
 * parallel.h
 *
 *	Parallel support header file for the pg_dump archiver
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	The author is not responsible for loss or damages that may
 *	result from its use.
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/parallel.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_DUMP_PARALLEL_H
#define PG_DUMP_PARALLEL_H

#include "pg_backup_archiver.h"

typedef enum
{
	WRKR_TERMINATED = 0,
	WRKR_IDLE,
	WRKR_WORKING,
	WRKR_FINISHED
} T_WorkerStatus;

/* Arguments needed for a worker process */
typedef struct ParallelArgs
{
	ArchiveHandle *AH;
	TocEntry   *te;
} ParallelArgs;

/* State for each parallel activity slot */
typedef struct ParallelSlot
{
	ParallelArgs *args;
	T_WorkerStatus workerStatus;
	int			status;
	int			pipeRead;		/* master's end of the pipes */
	int			pipeWrite;
	int			pipeRevRead;	/* child's end of the pipes */
	int			pipeRevWrite;
#ifdef WIN32
	uintptr_t	hThread;
	unsigned int threadId;
#else
	pid_t		pid;
#endif
} ParallelSlot;

#define NO_SLOT (-1)

typedef struct ParallelState
{
	int			numWorkers;
	ParallelSlot *parallelSlot;
} ParallelState;

#ifdef WIN32
extern bool parallel_init_done;
extern DWORD mainThreadId;
#endif

extern void init_parallel_dump_utils(void);

extern int	GetIdleWorker(ParallelState *pstate);
extern bool IsEveryWorkerIdle(ParallelState *pstate);
extern void ListenToWorkers(ArchiveHandle *AH, ParallelState *pstate, bool do_wait);
extern int	ReapWorkerStatus(ParallelState *pstate, int *status);
extern void EnsureIdleWorker(ArchiveHandle *AH, ParallelState *pstate);
extern void EnsureWorkersFinished(ArchiveHandle *AH, ParallelState *pstate);

extern ParallelState *ParallelBackupStart(ArchiveHandle *AH);
extern void DispatchJobForTocEntry(ArchiveHandle *AH,
					   ParallelState *pstate,
					   TocEntry *te, T_Action act);
extern void ParallelBackupEnd(ArchiveHandle *AH, ParallelState *pstate);

extern void set_archive_cancel_info(ArchiveHandle *AH, PGconn *conn);

#endif   /* PG_DUMP_PARALLEL_H */
