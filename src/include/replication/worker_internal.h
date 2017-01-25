/*-------------------------------------------------------------------------
 *
 * worker_internal.h
 *	  Internal headers shared by logical replication workers.
 *
 * Portions Copyright (c) 2016-2017, PostgreSQL Global Development Group
 *
 * src/include/replication/worker_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WORKER_INTERNAL_H
#define WORKER_INTERNAL_H

#include "access/xlogdefs.h"
#include "catalog/pg_subscription.h"
#include "datatype/timestamp.h"
#include "storage/lock.h"

typedef struct LogicalRepWorker
{
	/* Pointer to proc array. NULL if not running. */
	PGPROC *proc;

	/* Database id to connect to. */
	Oid		dbid;

	/* User to use for connection (will be same as owner of subscription). */
	Oid		userid;

	/* Subscription id for the worker. */
	Oid		subid;

	/* Used for initial table synchronization. */
	Oid		relid;

	/* Stats. */
	XLogRecPtr	last_lsn;
	TimestampTz	last_send_time;
	TimestampTz	last_recv_time;
	XLogRecPtr	reply_lsn;
	TimestampTz	reply_time;
} LogicalRepWorker;

/* libpqreceiver connection */
extern struct WalReceiverConn	   *wrconn;

/* Worker and subscription objects. */
extern Subscription		   *MySubscription;
extern LogicalRepWorker	   *MyLogicalRepWorker;

extern bool	in_remote_transaction;
extern bool	got_SIGTERM;

extern void logicalrep_worker_attach(int slot);
extern LogicalRepWorker *logicalrep_worker_find(Oid subid);
extern int logicalrep_worker_count(Oid subid);
extern void logicalrep_worker_launch(Oid dbid, Oid subid, const char *subname, Oid userid);
extern void logicalrep_worker_stop(Oid subid);
extern void logicalrep_worker_wakeup(Oid subid);

extern void logicalrep_worker_sigterm(SIGNAL_ARGS);

#endif   /* WORKER_INTERNAL_H */
