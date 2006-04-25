/*-------------------------------------------------------------------------
 *
 * async.h
 *	  Asynchronous notification: NOTIFY, LISTEN, UNLISTEN
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/async.h,v 1.33 2006/04/25 14:11:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ASYNC_H
#define ASYNC_H

extern bool Trace_notify;

/* notify-related SQL statements */
extern void Async_Notify(const char *relname);
extern void Async_Listen(const char *relname);
extern void Async_Unlisten(const char *relname);

/* perform (or cancel) outbound notify processing at transaction commit */
extern void AtCommit_Notify(void);
extern void AtAbort_Notify(void);
extern void AtSubStart_Notify(void);
extern void AtSubCommit_Notify(void);
extern void AtSubAbort_Notify(void);
extern void AtPrepare_Notify(void);

/* signal handler for inbound notifies (SIGUSR2) */
extern void NotifyInterruptHandler(SIGNAL_ARGS);

/*
 * enable/disable processing of inbound notifies directly from signal handler.
 * The enable routine first performs processing of any inbound notifies that
 * have occurred since the last disable.
 */
extern void EnableNotifyInterrupt(void);
extern bool DisableNotifyInterrupt(void);

extern void notify_twophase_postcommit(TransactionId xid, uint16 info,
						   void *recdata, uint32 len);

#endif   /* ASYNC_H */
