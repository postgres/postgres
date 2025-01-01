/*--------------------------------------------------------------------
 * bgworker_internals.h
 *		POSTGRES pluggable background workers internals
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/postmaster/bgworker_internals.h
 *--------------------------------------------------------------------
 */
#ifndef BGWORKER_INTERNALS_H
#define BGWORKER_INTERNALS_H

#include "datatype/timestamp.h"
#include "lib/ilist.h"
#include "postmaster/bgworker.h"

/* GUC options */

/*
 * Maximum possible value of parallel workers.
 */
#define MAX_PARALLEL_WORKER_LIMIT 1024

/*
 * List of background workers, private to postmaster.
 *
 * All workers that are currently running will also have an entry in
 * ActiveChildList.
 */
typedef struct RegisteredBgWorker
{
	BackgroundWorker rw_worker; /* its registry entry */
	pid_t		rw_pid;			/* 0 if not running */
	TimestampTz rw_crashed_at;	/* if not 0, time it last crashed */
	int			rw_shmem_slot;
	bool		rw_terminate;
	dlist_node	rw_lnode;		/* list link */
} RegisteredBgWorker;

extern PGDLLIMPORT dlist_head BackgroundWorkerList;

extern Size BackgroundWorkerShmemSize(void);
extern void BackgroundWorkerShmemInit(void);
extern void BackgroundWorkerStateChange(bool allow_new_workers);
extern void ForgetBackgroundWorker(RegisteredBgWorker *rw);
extern void ReportBackgroundWorkerPID(RegisteredBgWorker *rw);
extern void ReportBackgroundWorkerExit(RegisteredBgWorker *rw);
extern void BackgroundWorkerStopNotifications(pid_t pid);
extern void ForgetUnstartedBackgroundWorkers(void);
extern void ResetBackgroundWorkerCrashTimes(void);

/* Entry point for background worker processes */
extern void BackgroundWorkerMain(char *startup_data, size_t startup_data_len) pg_attribute_noreturn();

#endif							/* BGWORKER_INTERNALS_H */
