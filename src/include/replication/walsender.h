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
#include "storage/latch.h"
#include "storage/spin.h"


typedef enum WalSndState
{
	WALSNDSTATE_STARTUP = 0,
	WALSNDSTATE_BACKUP,
	WALSNDSTATE_CATCHUP,
	WALSNDSTATE_STREAMING
}	WalSndState;

/*
 * Each walsender has a WalSnd struct in shared memory.
 */
typedef struct WalSnd
{
	pid_t		pid;			/* this walsender's process id, or 0 */
	WalSndState state;			/* this walsender's state */
	XLogRecPtr	sentPtr;		/* WAL has been sent up to this point */

	slock_t		mutex;			/* locks shared variables shown above */

	/*
	 * Latch used by backends to wake up this walsender when it has work
	 * to do.
	 */
	Latch		latch;
} WalSnd;

/* There is one WalSndCtl struct for the whole database cluster */
typedef struct
{
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

extern int	WalSenderMain(void);
extern void WalSndSignals(void);
extern Size WalSndShmemSize(void);
extern void WalSndShmemInit(void);
extern void WalSndWakeup(void);
extern void WalSndSetState(WalSndState state);

extern Datum pg_stat_get_wal_senders(PG_FUNCTION_ARGS);

#endif   /* _WALSENDER_H */
