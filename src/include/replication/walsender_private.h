/*-------------------------------------------------------------------------
 *
 * walsender_private.h
 *	  Private definitions from replication/walsender.c.
 *
 * Portions Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * src/include/replication/walsender_private.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _WALSENDER_PRIVATE_H
#define _WALSENDER_PRIVATE_H

#include "access/xlog.h"
#include "lib/ilist.h"
#include "nodes/nodes.h"
#include "nodes/replnodes.h"
#include "replication/syncrep.h"
#include "storage/condition_variable.h"
#include "storage/shmem.h"
#include "storage/spin.h"

typedef enum WalSndState
{
	WALSNDSTATE_STARTUP = 0,
	WALSNDSTATE_BACKUP,
	WALSNDSTATE_CATCHUP,
	WALSNDSTATE_STREAMING,
	WALSNDSTATE_STOPPING,
} WalSndState;

/*
 * Each walsender has a WalSnd struct in shared memory.
 *
 * This struct is protected by its 'mutex' spinlock field, except that some
 * members are only written by the walsender process itself, and thus that
 * process is free to read those members without holding spinlock.  pid and
 * needreload always require the spinlock to be held for all accesses.
 */
typedef struct WalSnd
{
	pid_t		pid;			/* this walsender's PID, or 0 if not active */

	WalSndState state;			/* this walsender's state */
	XLogRecPtr	sentPtr;		/* WAL has been sent up to this point */
	bool		needreload;		/* does currently-open file need to be
								 * reloaded? */

	/*
	 * The xlog locations that have been written, flushed, and applied by
	 * standby-side. These may be invalid if the standby-side has not offered
	 * values yet.
	 */
	XLogRecPtr	write;
	XLogRecPtr	flush;
	XLogRecPtr	apply;

	/* Measured lag times, or -1 for unknown/none. */
	TimeOffset	writeLag;
	TimeOffset	flushLag;
	TimeOffset	applyLag;

	/*
	 * The priority order of the standby managed by this WALSender, as listed
	 * in synchronous_standby_names, or 0 if not-listed.
	 */
	int			sync_standby_priority;

	/* Protects shared variables in this structure. */
	slock_t		mutex;

	/*
	 * Timestamp of the last message received from standby.
	 */
	TimestampTz replyTime;

	ReplicationKind kind;
} WalSnd;

extern PGDLLIMPORT WalSnd *MyWalSnd;

/* There is one WalSndCtl struct for the whole database cluster */
typedef struct
{
	/*
	 * Synchronous replication queue with one queue per request type.
	 * Protected by SyncRepLock.
	 */
	dlist_head	SyncRepQueue[NUM_SYNC_REP_WAIT_MODE];

	/*
	 * Current location of the head of the queue. All waiters should have a
	 * waitLSN that follows this value. Protected by SyncRepLock.
	 */
	XLogRecPtr	lsn[NUM_SYNC_REP_WAIT_MODE];

	/*
	 * Are any sync standbys defined?  Waiting backends can't reload the
	 * config file safely, so checkpointer updates this value as needed.
	 * Protected by SyncRepLock.
	 */
	bool		sync_standbys_defined;

	/* used as a registry of physical / logical walsenders to wake */
	ConditionVariable wal_flush_cv;
	ConditionVariable wal_replay_cv;

	/*
	 * Used by physical walsenders holding slots specified in
	 * synchronized_standby_slots to wake up logical walsenders holding
	 * logical failover slots when a walreceiver confirms the receipt of LSN.
	 */
	ConditionVariable wal_confirm_rcv_cv;

	WalSnd		walsnds[FLEXIBLE_ARRAY_MEMBER];
} WalSndCtlData;

extern PGDLLIMPORT WalSndCtlData *WalSndCtl;


extern void WalSndSetState(WalSndState state);

/*
 * Internal functions for parsing the replication grammar, in repl_gram.y and
 * repl_scanner.l
 */
union YYSTYPE;
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void *yyscan_t;
#endif
extern int	replication_yyparse(Node **replication_parse_result_p, yyscan_t yyscanner);
extern int	replication_yylex(union YYSTYPE *yylval_param, yyscan_t yyscanner);
extern void replication_yyerror(Node **replication_parse_result_p, yyscan_t yyscanner, const char *message) pg_attribute_noreturn();
extern void replication_scanner_init(const char *str, yyscan_t *yyscannerp);
extern void replication_scanner_finish(yyscan_t yyscanner);
extern bool replication_scanner_is_replication_command(yyscan_t yyscanner);

#endif							/* _WALSENDER_PRIVATE_H */
