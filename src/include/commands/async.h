/*-------------------------------------------------------------------------
 *
 * async.h
 *	  Asynchronous notification: NOTIFY, LISTEN, UNLISTEN
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/async.h,v 1.27 2004/12/31 22:03:28 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ASYNC_H
#define ASYNC_H

extern bool Trace_notify;

/* notify-related SQL statements */
extern void Async_Notify(char *relname);
extern void Async_Listen(char *relname, int pid);
extern void Async_Unlisten(char *relname, int pid);

/* perform (or cancel) outbound notify processing at transaction commit */
extern void AtCommit_Notify(void);
extern void AtAbort_Notify(void);
extern void AtSubStart_Notify(void);
extern void AtSubCommit_Notify(void);
extern void AtSubAbort_Notify(void);

/* signal handler for inbound notifies (SIGUSR2) */
extern void NotifyInterruptHandler(SIGNAL_ARGS);

/*
 * enable/disable processing of inbound notifies directly from signal handler.
 * The enable routine first performs processing of any inbound notifies that
 * have occurred since the last disable.
 */
extern void EnableNotifyInterrupt(void);
extern bool DisableNotifyInterrupt(void);

#endif   /* ASYNC_H */
