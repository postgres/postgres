/*-------------------------------------------------------------------------
 *
 * syncrep.h
 *	  Exports from replication/syncrep.c.
 *
 * Portions Copyright (c) 2010-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/replication/syncrep.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _SYNCREP_H
#define _SYNCREP_H

#include "access/xlogdefs.h"
#include "utils/guc.h"

#define SyncRepRequested() \
	(max_wal_senders > 0 && synchronous_commit > SYNCHRONOUS_COMMIT_LOCAL_FLUSH)

/* SyncRepWaitMode */
#define SYNC_REP_NO_WAIT		(-1)
#define SYNC_REP_WAIT_WRITE		0
#define SYNC_REP_WAIT_FLUSH		1
#define SYNC_REP_WAIT_APPLY		2

#define NUM_SYNC_REP_WAIT_MODE	3

/* syncRepState */
#define SYNC_REP_NOT_WAITING		0
#define SYNC_REP_WAITING			1
#define SYNC_REP_WAIT_COMPLETE		2

/*
 * Struct for the configuration of synchronous replication.
 *
 * Note: this must be a flat representation that can be held in a single
 * chunk of malloc'd memory, so that it can be stored as the "extra" data
 * for the synchronous_standby_names GUC.
 */
typedef struct SyncRepConfigData
{
	int			config_size;	/* total size of this struct, in bytes */
	int			num_sync;		/* number of sync standbys that we need to
								 * wait for */
	int			nmembers;		/* number of members in the following list */
	/* member_names contains nmembers consecutive nul-terminated C strings */
	char		member_names[FLEXIBLE_ARRAY_MEMBER];
} SyncRepConfigData;

/* communication variables for parsing synchronous_standby_names GUC */
extern SyncRepConfigData *syncrep_parse_result;
extern char *syncrep_parse_error_msg;

/* user-settable parameters for synchronous replication */
extern char *SyncRepStandbyNames;

/* called by user backend */
extern void SyncRepWaitForLSN(XLogRecPtr lsn, bool commit);

/* called at backend exit */
extern void SyncRepCleanupAtProcExit(void);

/* called by wal sender */
extern void SyncRepInitConfig(void);
extern void SyncRepReleaseWaiters(void);

/* called by wal sender and user backend */
extern List *SyncRepGetSyncStandbys(bool *am_sync);

/* called by checkpointer */
extern void SyncRepUpdateSyncStandbysDefined(void);

/* GUC infrastructure */
extern bool check_synchronous_standby_names(char **newval, void **extra, GucSource source);
extern void assign_synchronous_standby_names(const char *newval, void *extra);
extern void assign_synchronous_commit(int newval, void *extra);

/*
 * Internal functions for parsing synchronous_standby_names grammar,
 * in syncrep_gram.y and syncrep_scanner.l
 */
extern int	syncrep_yyparse(void);
extern int	syncrep_yylex(void);
extern void syncrep_yyerror(const char *str);
extern void syncrep_scanner_init(const char *query_string);
extern void syncrep_scanner_finish(void);

#endif   /* _SYNCREP_H */
