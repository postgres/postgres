/*-------------------------------------------------------------------------
 *
 * walsender.h
 *	  Exports from replication/walsender.c.
 *
 * Portions Copyright (c) 2010-2012, PostgreSQL Global Development Group
 *
 * src/include/replication/walsender.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _WALSENDER_H
#define _WALSENDER_H

#include <signal.h>

#include "fmgr.h"

/* global state */
extern bool am_walsender;
extern bool am_cascading_walsender;
extern volatile sig_atomic_t walsender_shutdown_requested;
extern volatile sig_atomic_t walsender_ready_to_stop;
extern bool wake_wal_senders;

/* user-settable parameters */
extern int	max_wal_senders;
extern int	replication_timeout;

extern void WalSenderMain(void) __attribute__((noreturn));
extern void WalSndSignals(void);
extern Size WalSndShmemSize(void);
extern void WalSndShmemInit(void);
extern void WalSndWakeup(void);
extern void WalSndRqstFileReload(void);

extern Datum pg_stat_get_wal_senders(PG_FUNCTION_ARGS);

/*
 * Remember that we want to wakeup walsenders later
 *
 * This is separated from doing the actual wakeup because the writeout is done
 * while holding contended locks.
 */
#define WalSndWakeupRequest() \
	do { wake_wal_senders = true; } while (0)

/*
 * wakeup walsenders if there is work to be done
 */
#define WalSndWakeupProcessRequests()		\
	do										\
	{										\
		if (wake_wal_senders)				\
		{									\
			wake_wal_senders = false;		\
			if (max_wal_senders > 0)		\
				WalSndWakeup();				\
		}									\
	} while (0)

#endif   /* _WALSENDER_H */
