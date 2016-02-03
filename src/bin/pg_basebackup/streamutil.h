/*-------------------------------------------------------------------------
 *
 * streamutil.h
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/streamutil.h
 *-------------------------------------------------------------------------
 */

#ifndef STREAMUTIL_H
#define STREAMUTIL_H

#include "libpq-fe.h"

#include "access/xlogdefs.h"

extern const char *progname;
extern char *connection_string;
extern char *dbhost;
extern char *dbuser;
extern char *dbport;
extern char *dbname;
extern int	dbgetpassword;
extern char *replication_slot;

/* Connection kept global so we can disconnect easily */
extern PGconn *conn;

extern PGconn *GetConnection(void);

/* Replication commands */
extern bool CreateReplicationSlot(PGconn *conn, const char *slot_name,
					  const char *plugin, bool is_physical,
					  bool slot_exists_ok);
extern bool DropReplicationSlot(PGconn *conn, const char *slot_name);
extern bool RunIdentifySystem(PGconn *conn, char **sysid,
				  TimeLineID *starttli,
				  XLogRecPtr *startpos,
				  char **db_name);
extern int64 feGetCurrentTimestamp(void);
extern void feTimestampDifference(int64 start_time, int64 stop_time,
					  long *secs, int *microsecs);

extern bool feTimestampDifferenceExceeds(int64 start_time, int64 stop_time,
							 int msec);
extern void fe_sendint64(int64 i, char *buf);
extern int64 fe_recvint64(char *buf);

#endif   /* STREAMUTIL_H */
