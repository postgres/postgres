/*-------------------------------------------------------------------------
 *
 * worker_internal.h
 *	  Internal headers shared by logical replication workers.
 *
 * Portions Copyright (c) 2016-2019, PostgreSQL Global Development Group
 *
 * src/include/replication/worker_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WORKER_INTERNAL_H
#define WORKER_INTERNAL_H

#include <signal.h>

#include "access/xlogdefs.h"
#include "catalog/pg_subscription.h"
#include "datatype/timestamp.h"
#include "storage/lock.h"

typedef struct LogicalRepWorker
{
	/* Time at which this worker was launched. */
	TimestampTz launch_time;

	/* Indicates if this slot is used or free. */
	bool		in_use;

	/* Increased every time the slot is taken by new worker. */
	uint16		generation;

	/* Pointer to proc array. NULL if not running. */
	PGPROC	   *proc;

	/* Database id to connect to. */
	Oid			dbid;

	/* User to use for connection (will be same as owner of subscription). */
	Oid			userid;

	/* Subscription id for the worker. */
	Oid			subid;

	/* Used for initial table synchronization. */
	Oid			relid;
	char		relstate;
	XLogRecPtr	relstate_lsn;
	slock_t		relmutex;

	/* Stats. */
	XLogRecPtr	last_lsn;
	TimestampTz last_send_time;
	TimestampTz last_recv_time;
	XLogRecPtr	reply_lsn;
	TimestampTz reply_time;
} LogicalRepWorker;

/* Main memory context for apply worker. Permanent during worker lifetime. */
extern MemoryContext ApplyContext;

/* libpqreceiver connection */
extern struct WalReceiverConn *LogRepWorkerWalRcvConn;

/* Worker and subscription objects. */
extern Subscription *MySubscription;
extern LogicalRepWorker *MyLogicalRepWorker;

extern bool in_remote_transaction;

extern void logicalrep_worker_attach(int slot);
extern LogicalRepWorker *logicalrep_worker_find(Oid subid, Oid relid,
												bool only_running);
extern List *logicalrep_workers_find(Oid subid, bool only_running);
extern void logicalrep_worker_launch(Oid dbid, Oid subid, const char *subname,
									 Oid userid, Oid relid);
extern void logicalrep_worker_stop(Oid subid, Oid relid);
extern void logicalrep_worker_stop_at_commit(Oid subid, Oid relid);
extern void logicalrep_worker_wakeup(Oid subid, Oid relid);
extern void logicalrep_worker_wakeup_ptr(LogicalRepWorker *worker);

extern int	logicalrep_sync_worker_count(Oid subid);

extern char *LogicalRepSyncTableStart(XLogRecPtr *origin_startpos);
void		process_syncing_tables(XLogRecPtr current_lsn);
void		invalidate_syncing_table_states(Datum arg, int cacheid,
											uint32 hashvalue);

static inline bool
am_tablesync_worker(void)
{
	return OidIsValid(MyLogicalRepWorker->relid);
}

#endif							/* WORKER_INTERNAL_H */
