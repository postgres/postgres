/*-------------------------------------------------------------------------
 *
 * autovacuum.h
 *	  header file for integrated autovacuum daemon
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/autovacuum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTOVACUUM_H
#define AUTOVACUUM_H

#include "storage/block.h"

/*
 * Other processes can request specific work from autovacuum, identified by
 * AutoVacuumWorkItem elements.
 */
typedef enum
{
	AVW_BRINSummarizeRange,
} AutoVacuumWorkItemType;


/* GUC variables */
extern PGDLLIMPORT bool autovacuum_start_daemon;
extern PGDLLIMPORT int autovacuum_worker_slots;
extern PGDLLIMPORT int autovacuum_max_workers;
extern PGDLLIMPORT int autovacuum_work_mem;
extern PGDLLIMPORT int autovacuum_naptime;
extern PGDLLIMPORT int autovacuum_vac_thresh;
extern PGDLLIMPORT int autovacuum_vac_max_thresh;
extern PGDLLIMPORT double autovacuum_vac_scale;
extern PGDLLIMPORT int autovacuum_vac_ins_thresh;
extern PGDLLIMPORT double autovacuum_vac_ins_scale;
extern PGDLLIMPORT int autovacuum_anl_thresh;
extern PGDLLIMPORT double autovacuum_anl_scale;
extern PGDLLIMPORT int autovacuum_freeze_max_age;
extern PGDLLIMPORT int autovacuum_multixact_freeze_max_age;
extern PGDLLIMPORT double autovacuum_vac_cost_delay;
extern PGDLLIMPORT int autovacuum_vac_cost_limit;

/* autovacuum launcher PID, only valid when worker is shutting down */
extern PGDLLIMPORT int AutovacuumLauncherPid;

extern PGDLLIMPORT int Log_autovacuum_min_duration;

/* Status inquiry functions */
extern bool AutoVacuumingActive(void);

/* called from postmaster at server startup */
extern void autovac_init(void);

/* called from postmaster when a worker could not be forked */
extern void AutoVacWorkerFailed(void);

extern void AutoVacLauncherMain(const void *startup_data, size_t startup_data_len) pg_attribute_noreturn();
extern void AutoVacWorkerMain(const void *startup_data, size_t startup_data_len) pg_attribute_noreturn();

extern bool AutoVacuumRequestWork(AutoVacuumWorkItemType type,
								  Oid relationId, BlockNumber blkno);

/* shared memory stuff */
extern Size AutoVacuumShmemSize(void);
extern void AutoVacuumShmemInit(void);

#endif							/* AUTOVACUUM_H */
