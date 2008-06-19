/*-------------------------------------------------------------------------
 *
 * pmsignal.h
 *	  routines for signaling the postmaster from its child processes
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/pmsignal.h,v 1.20 2008/06/19 21:32:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PMSIGNAL_H
#define PMSIGNAL_H

/*
 * Reasons for signaling the postmaster.  We can cope with simultaneous
 * signals for different reasons.  If the same reason is signaled multiple
 * times in quick succession, however, the postmaster is likely to observe
 * only one notification of it.  This is okay for the present uses.
 */
typedef enum
{
	PMSIGNAL_PASSWORD_CHANGE,	/* pg_auth file has changed */
	PMSIGNAL_WAKEN_ARCHIVER,	/* send a NOTIFY signal to xlog archiver */
	PMSIGNAL_ROTATE_LOGFILE,	/* send SIGUSR1 to syslogger to rotate logfile */
	PMSIGNAL_START_AUTOVAC_LAUNCHER,	/* start an autovacuum launcher */
	PMSIGNAL_START_AUTOVAC_WORKER,		/* start an autovacuum worker */

	NUM_PMSIGNALS				/* Must be last value of enum! */
} PMSignalReason;

/*
 * prototypes for functions in pmsignal.c
 */
extern void PMSignalInit(void);
extern void SendPostmasterSignal(PMSignalReason reason);
extern bool CheckPostmasterSignal(PMSignalReason reason);
extern bool PostmasterIsAlive(bool amDirectChild);

#endif   /* PMSIGNAL_H */
