/*-------------------------------------------------------------------------
 *
 * walsender.h
 *	  Exports from replication/walsender.c.
 *
 * Portions Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *
 * src/include/replication/walsender.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _WALSENDER_H
#define _WALSENDER_H

#include "access/xlog.h"
#include "nodes/nodes.h"
#include "storage/latch.h"
#include "replication/syncrep.h"
#include "storage/spin.h"


typedef enum WalSndState
{
	WALSNDSTATE_STARTUP = 0,
	WALSNDSTATE_BACKUP,
	WALSNDSTATE_CATCHUP,
	WALSNDSTATE_STREAMING
} WalSndState;

/*
 * Each walsender has a WalSnd struct in shared memory.
 */
typedef struct WalSnd
{
	pid_t		pid;			/* this walsender's process id, or 0 */
	WalSndState state;			/* this walsender's state */
	XLogRecPtr	sentPtr;		/* WAL has been sent up to this point */

	/*
	 * The xlog locations that have been written, flushed, and applied by
	 * standby-side. These may be invalid if the standby-side has not offered
	 * values yet.
	 */
	XLogRecPtr	write;
	XLogRecPtr	flush;
	XLogRecPtr	apply;

	/* Protects shared variables shown above. */
	slock_t		mutex;

	/*
	 * Latch used by backends to wake up this walsender when it has work to
	 * do.
	 */
	Latch		latch;

	/*
	 * The priority order of the standby managed by this WALSender, as listed
	 * in synchronous_standby_names, or 0 if not-listed. Protected by
	 * SyncRepLock.
	 */
	int			sync_standby_priority;
} WalSnd;

extern WalSnd *MyWalSnd;

/* There is one WalSndCtl struct for the whole database cluster */
typedef struct
{
	/*
	 * Synchronous replication queue. Protected by SyncRepLock.
	 */
	SHM_QUEUE	SyncRepQueue;

	/*
	 * Current location of the head of the queue. All waiters should have a
	 * waitLSN that follows this value. Protected by SyncRepLock.
	 */
	XLogRecPtr	lsn;

	/*
	 * Are any sync standbys defined?  Waiting backends can't reload the
	 * config file safely, so WAL writer updates this value as needed.
	 * Protected by SyncRepLock.
	 */
	bool		sync_standbys_defined;

	WalSnd		walsnds[1];		/* VARIABLE LENGTH ARRAY */
} WalSndCtlData;

extern WalSndCtlData *WalSndCtl;

/* global state */
extern bool am_walsender;
extern volatile sig_atomic_t walsender_shutdown_requested;
extern volatile sig_atomic_t walsender_ready_to_stop;

/* user-settable parameters */
extern int	WalSndDelay;
extern int	max_wal_senders;
extern int	replication_timeout;

extern int	WalSenderMain(void);
extern void WalSndSignals(void);
extern Size WalSndShmemSize(void);
extern void WalSndShmemInit(void);
extern void WalSndWakeup(void);
extern void WalSndSetState(WalSndState state);
extern void XLogRead(char *buf, XLogRecPtr recptr, Size nbytes);

extern Datum pg_stat_get_wal_senders(PG_FUNCTION_ARGS);

/*
 * Internal functions for parsing the replication grammar, in repl_gram.y and
 * repl_scanner.l
 */
extern int	replication_yyparse(void);
extern int	replication_yylex(void);
extern void replication_yyerror(const char *str);
extern void replication_scanner_init(const char *query_string);
extern void replication_scanner_finish(void);

extern Node *replication_parse_result;

#endif   /* _WALSENDER_H */
