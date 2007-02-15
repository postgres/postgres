/*-------------------------------------------------------------------------
 *
 * autovacuum.h
 *	  header file for integrated autovacuum daemon
 *
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/postmaster/autovacuum.h,v 1.8 2007/02/15 23:23:23 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef AUTOVACUUM_H
#define AUTOVACUUM_H

/* GUC variables */
extern bool autovacuum_start_daemon;
extern int	autovacuum_naptime;
extern int	autovacuum_vac_thresh;
extern double autovacuum_vac_scale;
extern int	autovacuum_anl_thresh;
extern double autovacuum_anl_scale;
extern int	autovacuum_freeze_max_age;
extern int	autovacuum_vac_cost_delay;
extern int	autovacuum_vac_cost_limit;

/* Status inquiry functions */
extern bool AutoVacuumingActive(void);
extern bool IsAutoVacuumLauncherProcess(void);
extern bool IsAutoVacuumWorkerProcess(void);

/* Functions to start autovacuum process, called from postmaster */
extern void autovac_init(void);
extern int	StartAutoVacLauncher(void);
extern int	StartAutoVacWorker(void);

#ifdef EXEC_BACKEND
extern void AutoVacLauncherMain(int argc, char *argv[]);
extern void AutoVacWorkerMain(int argc, char *argv[]);
extern void AutovacuumWorkerIAm(void);
extern void AutovacuumLauncherIAm(void);
#endif

/* shared memory stuff */
extern Size AutoVacuumShmemSize(void);
extern void AutoVacuumShmemInit(void);

#endif   /* AUTOVACUUM_H */
