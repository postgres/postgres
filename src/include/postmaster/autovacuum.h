/*-------------------------------------------------------------------------
 *
 * autovacuum.h
 *	  header file for integrated autovacuum daemon
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/postmaster/autovacuum.h,v 1.5 2006/11/05 22:42:10 tgl Exp $
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
extern bool IsAutoVacuumProcess(void);

/* Functions to start autovacuum process, called from postmaster */
extern void autovac_init(void);
extern int	autovac_start(void);
extern void autovac_stopped(void);

#ifdef EXEC_BACKEND
extern void AutoVacMain(int argc, char *argv[]);
#endif

#endif   /* AUTOVACUUM_H */
