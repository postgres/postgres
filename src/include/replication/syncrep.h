/*-------------------------------------------------------------------------
 *
 * syncrep.h
 *	  Exports from replication/syncrep.c.
 *
 * Portions Copyright (c) 2010-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/replication/syncrep.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _SYNCREP_H
#define _SYNCREP_H

#include "access/xlogdefs.h"

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

/* syncrep_method of SyncRepConfigData */
#define SYNC_REP_PRIORITY		0
#define SYNC_REP_QUORUM		1

/*
 * SyncRepGetCandidateStandbys returns an array of these structs,
 * one per candidate synchronous walsender.
 */
typedef struct SyncRepStandbyData
{
	/* Copies of relevant fields from WalSnd shared-memory struct */
	pid_t		pid;
	XLogRecPtr	write;
	XLogRecPtr	flush;
	XLogRecPtr	apply;
	int			sync_standby_priority;
	/* Index of this walsender in the WalSnd shared-memory array */
	int			walsnd_index;
	/* This flag indicates whether this struct is about our own process */
	bool		is_me;
} SyncRepStandbyData;

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
	uint8		syncrep_method; /* method to choose sync standbys */
	int			nmembers;		/* number of members in the following list */
	/* member_names contains nmembers consecutive nul-terminated C strings */
	char		member_names[FLEXIBLE_ARRAY_MEMBER];
} SyncRepConfigData;

extern PGDLLIMPORT SyncRepConfigData *SyncRepConfig;

/* user-settable parameters for synchronous replication */
extern PGDLLIMPORT char *SyncRepStandbyNames;

/* called by user backend */
extern void SyncRepWaitForLSN(XLogRecPtr lsn, bool commit);

/* called at backend exit */
extern void SyncRepCleanupAtProcExit(void);

/* called by wal sender */
extern void SyncRepInitConfig(void);
extern void SyncRepReleaseWaiters(void);

/* called by wal sender and user backend */
extern int	SyncRepGetCandidateStandbys(SyncRepStandbyData **standbys);

/* called by checkpointer */
extern void SyncRepUpdateSyncStandbysDefined(void);

/*
 * Internal functions for parsing synchronous_standby_names grammar,
 * in syncrep_gram.y and syncrep_scanner.l
 */
union YYSTYPE;
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void *yyscan_t;
#endif
extern int	syncrep_yyparse(SyncRepConfigData **syncrep_parse_result_p, char **syncrep_parse_error_msg_p, yyscan_t yyscanner);
extern int	syncrep_yylex(union YYSTYPE *yylval_param, char **syncrep_parse_error_msg_p, yyscan_t yyscanner);
extern void syncrep_yyerror(SyncRepConfigData **syncrep_parse_result_p, char **syncrep_parse_error_msg_p, yyscan_t yyscanner, const char *str);
extern void syncrep_scanner_init(const char *str, yyscan_t *yyscannerp);
extern void syncrep_scanner_finish(yyscan_t yyscanner);

#endif							/* _SYNCREP_H */
