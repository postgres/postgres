/*-------------------------------------------------------------------------
 *
 * pmsignal.h
 *	  routines for signaling the postmaster from its child processes
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/pmsignal.h,v 1.32.2.1 2010/08/23 17:20:08 tgl Exp $
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
	PMSIGNAL_RECOVERY_STARTED,	/* recovery has started */
	PMSIGNAL_BEGIN_HOT_STANDBY, /* begin Hot Standby */
	PMSIGNAL_WAKEN_ARCHIVER,	/* send a NOTIFY signal to xlog archiver */
	PMSIGNAL_ROTATE_LOGFILE,	/* send SIGUSR1 to syslogger to rotate logfile */
	PMSIGNAL_START_AUTOVAC_LAUNCHER,	/* start an autovacuum launcher */
	PMSIGNAL_START_AUTOVAC_WORKER,		/* start an autovacuum worker */
	PMSIGNAL_START_WALRECEIVER, /* start a walreceiver */

	NUM_PMSIGNALS				/* Must be last value of enum! */
} PMSignalReason;

/* PMSignalData is an opaque struct, details known only within pmsignal.c */
typedef struct PMSignalData PMSignalData;

/*
 * prototypes for functions in pmsignal.c
 */
extern Size PMSignalShmemSize(void);
extern void PMSignalShmemInit(void);
extern void SendPostmasterSignal(PMSignalReason reason);
extern bool CheckPostmasterSignal(PMSignalReason reason);
extern int	AssignPostmasterChildSlot(void);
extern bool ReleasePostmasterChildSlot(int slot);
extern bool IsPostmasterChildWalSender(int slot);
extern void MarkPostmasterChildActive(void);
extern void MarkPostmasterChildInactive(void);
extern bool PostmasterIsAlive(bool amDirectChild);

#endif   /* PMSIGNAL_H */
