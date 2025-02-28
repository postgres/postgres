/* ----------
 *	pgstat.h
 *
 *	Definitions for the PostgreSQL cumulative statistics system.
 *
 *	Copyright (c) 2001-2025, PostgreSQL Global Development Group
 *
 *	src/include/pgstat.h
 * ----------
 */
#ifndef PGSTAT_H
#define PGSTAT_H

#include "access/xlogdefs.h"
#include "datatype/timestamp.h"
#include "portability/instr_time.h"
#include "postmaster/pgarch.h"	/* for MAX_XFN_CHARS */
#include "replication/conflict.h"
#include "utils/backend_progress.h" /* for backward compatibility */	/* IWYU pragma: export */
#include "utils/backend_status.h"	/* for backward compatibility */	/* IWYU pragma: export */
#include "utils/pgstat_kind.h"
#include "utils/relcache.h"
#include "utils/wait_event.h"	/* for backward compatibility */	/* IWYU pragma: export */


/* ----------
 * Paths for the statistics files (relative to installation's $PGDATA).
 * ----------
 */
#define PGSTAT_STAT_PERMANENT_DIRECTORY		"pg_stat"
#define PGSTAT_STAT_PERMANENT_FILENAME		"pg_stat/pgstat.stat"
#define PGSTAT_STAT_PERMANENT_TMPFILE		"pg_stat/pgstat.tmp"

/* Default directory to store temporary statistics data in */
#define PG_STAT_TMP_DIR		"pg_stat_tmp"

/* Values for track_functions GUC variable --- order is significant! */
typedef enum TrackFunctionsLevel
{
	TRACK_FUNC_OFF,
	TRACK_FUNC_PL,
	TRACK_FUNC_ALL,
}			TrackFunctionsLevel;

typedef enum PgStat_FetchConsistency
{
	PGSTAT_FETCH_CONSISTENCY_NONE,
	PGSTAT_FETCH_CONSISTENCY_CACHE,
	PGSTAT_FETCH_CONSISTENCY_SNAPSHOT,
} PgStat_FetchConsistency;

/* Values to track the cause of session termination */
typedef enum SessionEndType
{
	DISCONNECT_NOT_YET,			/* still active */
	DISCONNECT_NORMAL,
	DISCONNECT_CLIENT_EOF,
	DISCONNECT_FATAL,
	DISCONNECT_KILLED,
} SessionEndType;

/* ----------
 * The data type used for counters.
 * ----------
 */
typedef int64 PgStat_Counter;


/* ------------------------------------------------------------
 * Structures kept in backend local memory while accumulating counts
 * ------------------------------------------------------------
 */

/* ----------
 * PgStat_FunctionCounts	The actual per-function counts kept by a backend
 *
 * Note that the time counters are in instr_time format here.  We convert to
 * microseconds in PgStat_Counter format when flushing out pending statistics.
 * ----------
 */
typedef struct PgStat_FunctionCounts
{
	PgStat_Counter numcalls;
	instr_time	total_time;
	instr_time	self_time;
} PgStat_FunctionCounts;

/*
 * Working state needed to accumulate per-function-call timing statistics.
 */
typedef struct PgStat_FunctionCallUsage
{
	/* Link to function's hashtable entry (must still be there at exit!) */
	/* NULL means we are not tracking the current function call */
	PgStat_FunctionCounts *fs;
	/* Total time previously charged to function, as of function start */
	instr_time	save_f_total_time;
	/* Backend-wide total time as of function start */
	instr_time	save_total;
	/* system clock as of function start */
	instr_time	start;
} PgStat_FunctionCallUsage;

/* ----------
 * PgStat_BackendSubEntry	Non-flushed subscription stats.
 * ----------
 */
typedef struct PgStat_BackendSubEntry
{
	PgStat_Counter apply_error_count;
	PgStat_Counter sync_error_count;
	PgStat_Counter conflict_count[CONFLICT_NUM_TYPES];
} PgStat_BackendSubEntry;

/* ----------
 * PgStat_TableCounts			The actual per-table counts kept by a backend
 *
 * This struct should contain only actual event counters, because we make use
 * of pg_memory_is_all_zeros() to detect whether there are any stats updates
 * to apply.
 *
 * It is a component of PgStat_TableStatus (within-backend state).
 *
 * Note: for a table, tuples_returned is the number of tuples successfully
 * fetched by heap_getnext, while tuples_fetched is the number of tuples
 * successfully fetched by heap_fetch under the control of bitmap indexscans.
 * For an index, tuples_returned is the number of index entries returned by
 * the index AM, while tuples_fetched is the number of tuples successfully
 * fetched by heap_fetch under the control of simple indexscans for this index.
 *
 * tuples_inserted/updated/deleted/hot_updated/newpage_updated count attempted
 * actions, regardless of whether the transaction committed.  delta_live_tuples,
 * delta_dead_tuples, and changed_tuples are set depending on commit or abort.
 * Note that delta_live_tuples and delta_dead_tuples can be negative!
 * ----------
 */
typedef struct PgStat_TableCounts
{
	PgStat_Counter numscans;

	PgStat_Counter tuples_returned;
	PgStat_Counter tuples_fetched;

	PgStat_Counter tuples_inserted;
	PgStat_Counter tuples_updated;
	PgStat_Counter tuples_deleted;
	PgStat_Counter tuples_hot_updated;
	PgStat_Counter tuples_newpage_updated;
	bool		truncdropped;

	PgStat_Counter delta_live_tuples;
	PgStat_Counter delta_dead_tuples;
	PgStat_Counter changed_tuples;

	PgStat_Counter blocks_fetched;
	PgStat_Counter blocks_hit;
} PgStat_TableCounts;

/* ----------
 * PgStat_TableStatus			Per-table status within a backend
 *
 * Many of the event counters are nontransactional, ie, we count events
 * in committed and aborted transactions alike.  For these, we just count
 * directly in the PgStat_TableStatus.  However, delta_live_tuples,
 * delta_dead_tuples, and changed_tuples must be derived from event counts
 * with awareness of whether the transaction or subtransaction committed or
 * aborted.  Hence, we also keep a stack of per-(sub)transaction status
 * records for every table modified in the current transaction.  At commit
 * or abort, we propagate tuples_inserted/updated/deleted up to the
 * parent subtransaction level, or out to the parent PgStat_TableStatus,
 * as appropriate.
 * ----------
 */
typedef struct PgStat_TableStatus
{
	Oid			id;				/* table's OID */
	bool		shared;			/* is it a shared catalog? */
	struct PgStat_TableXactStatus *trans;	/* lowest subxact's counts */
	PgStat_TableCounts counts;	/* event counts to be sent */
	Relation	relation;		/* rel that is using this entry */
} PgStat_TableStatus;

/* ----------
 * PgStat_TableXactStatus		Per-table, per-subtransaction status
 * ----------
 */
typedef struct PgStat_TableXactStatus
{
	PgStat_Counter tuples_inserted; /* tuples inserted in (sub)xact */
	PgStat_Counter tuples_updated;	/* tuples updated in (sub)xact */
	PgStat_Counter tuples_deleted;	/* tuples deleted in (sub)xact */
	bool		truncdropped;	/* relation truncated/dropped in this
								 * (sub)xact */
	/* tuples i/u/d prior to truncate/drop */
	PgStat_Counter inserted_pre_truncdrop;
	PgStat_Counter updated_pre_truncdrop;
	PgStat_Counter deleted_pre_truncdrop;
	int			nest_level;		/* subtransaction nest level */
	/* links to other structs for same relation: */
	struct PgStat_TableXactStatus *upper;	/* next higher subxact if any */
	PgStat_TableStatus *parent; /* per-table status */
	/* structs of same subxact level are linked here: */
	struct PgStat_TableXactStatus *next;	/* next of same subxact */
} PgStat_TableXactStatus;


/* ------------------------------------------------------------
 * Data structures on disk and in shared memory follow
 *
 * PGSTAT_FILE_FORMAT_ID should be changed whenever any of these
 * data structures change.
 * ------------------------------------------------------------
 */

#define PGSTAT_FILE_FORMAT_ID	0x01A5BCB5

typedef struct PgStat_ArchiverStats
{
	PgStat_Counter archived_count;	/* archival successes */
	char		last_archived_wal[MAX_XFN_CHARS + 1];	/* last WAL file
														 * archived */
	TimestampTz last_archived_timestamp;	/* last archival success time */
	PgStat_Counter failed_count;	/* failed archival attempts */
	char		last_failed_wal[MAX_XFN_CHARS + 1]; /* WAL file involved in
													 * last failure */
	TimestampTz last_failed_timestamp;	/* last archival failure time */
	TimestampTz stat_reset_timestamp;
} PgStat_ArchiverStats;

/* ---------
 * PgStat_BgWriterStats			Background Writer statistics
 *
 * This struct should contain only actual event counters, because we make use
 * of pg_memory_is_all_zeros() to detect whether there are any stats updates
 * to apply.
 * ---------
 */
typedef struct PgStat_BgWriterStats
{
	PgStat_Counter buf_written_clean;
	PgStat_Counter maxwritten_clean;
	PgStat_Counter buf_alloc;
	TimestampTz stat_reset_timestamp;
} PgStat_BgWriterStats;

/* --------
 * PgStat_CheckpointerStats		Checkpoint statistics
 *
 * This struct should contain only actual event counters, because we make use
 * of pg_memory_is_all_zeros() to detect whether there are any stats updates to
 * apply.
 * ---------
 */
typedef struct PgStat_CheckpointerStats
{
	PgStat_Counter num_timed;
	PgStat_Counter num_requested;
	PgStat_Counter num_performed;
	PgStat_Counter restartpoints_timed;
	PgStat_Counter restartpoints_requested;
	PgStat_Counter restartpoints_performed;
	PgStat_Counter write_time;	/* times in milliseconds */
	PgStat_Counter sync_time;
	PgStat_Counter buffers_written;
	PgStat_Counter slru_written;
	TimestampTz stat_reset_timestamp;
} PgStat_CheckpointerStats;


/*
 * Types related to counting IO operations
 */
typedef enum IOObject
{
	IOOBJECT_RELATION,
	IOOBJECT_TEMP_RELATION,
	IOOBJECT_WAL,
} IOObject;

#define IOOBJECT_NUM_TYPES (IOOBJECT_WAL + 1)

typedef enum IOContext
{
	IOCONTEXT_BULKREAD,
	IOCONTEXT_BULKWRITE,
	IOCONTEXT_INIT,
	IOCONTEXT_NORMAL,
	IOCONTEXT_VACUUM,
} IOContext;

#define IOCONTEXT_NUM_TYPES (IOCONTEXT_VACUUM + 1)

/*
 * Enumeration of IO operations.
 *
 * This enum categorizes IO operations into two groups, depending on if
 * byte operations are supported.
 *
 * Ensure IOOP_EXTEND is the first and IOOP_WRITE is the last ones in the
 * tracked in bytes group and that the groups stay in that order.
 */
typedef enum IOOp
{
	/* IOs not tracked in bytes */
	IOOP_EVICT,
	IOOP_FSYNC,
	IOOP_HIT,
	IOOP_REUSE,
	IOOP_WRITEBACK,

	/* IOs tracked in bytes */
	IOOP_EXTEND,
	IOOP_READ,
	IOOP_WRITE,
} IOOp;

#define IOOP_NUM_TYPES (IOOP_WRITE + 1)

#define pgstat_is_ioop_tracked_in_bytes(io_op) \
	(((unsigned int) (io_op)) < IOOP_NUM_TYPES && \
	 ((unsigned int) (io_op)) >= IOOP_EXTEND)

typedef struct PgStat_BktypeIO
{
	uint64		bytes[IOOBJECT_NUM_TYPES][IOCONTEXT_NUM_TYPES][IOOP_NUM_TYPES];
	PgStat_Counter counts[IOOBJECT_NUM_TYPES][IOCONTEXT_NUM_TYPES][IOOP_NUM_TYPES];
	PgStat_Counter times[IOOBJECT_NUM_TYPES][IOCONTEXT_NUM_TYPES][IOOP_NUM_TYPES];
} PgStat_BktypeIO;

typedef struct PgStat_PendingIO
{
	uint64		bytes[IOOBJECT_NUM_TYPES][IOCONTEXT_NUM_TYPES][IOOP_NUM_TYPES];
	PgStat_Counter counts[IOOBJECT_NUM_TYPES][IOCONTEXT_NUM_TYPES][IOOP_NUM_TYPES];
	instr_time	pending_times[IOOBJECT_NUM_TYPES][IOCONTEXT_NUM_TYPES][IOOP_NUM_TYPES];
} PgStat_PendingIO;

typedef struct PgStat_IO
{
	TimestampTz stat_reset_timestamp;
	PgStat_BktypeIO stats[BACKEND_NUM_TYPES];
} PgStat_IO;

typedef struct PgStat_Backend
{
	TimestampTz stat_reset_timestamp;
	PgStat_BktypeIO io_stats;
} PgStat_Backend;

/* ---------
 * PgStat_BackendPending	Non-flushed backend stats.
 * ---------
 */
typedef struct PgStat_BackendPending
{
	/*
	 * Backend statistics store the same amount of IO data as PGSTAT_KIND_IO.
	 */
	PgStat_PendingIO pending_io;
} PgStat_BackendPending;

typedef struct PgStat_StatDBEntry
{
	PgStat_Counter xact_commit;
	PgStat_Counter xact_rollback;
	PgStat_Counter blocks_fetched;
	PgStat_Counter blocks_hit;
	PgStat_Counter tuples_returned;
	PgStat_Counter tuples_fetched;
	PgStat_Counter tuples_inserted;
	PgStat_Counter tuples_updated;
	PgStat_Counter tuples_deleted;
	TimestampTz last_autovac_time;
	PgStat_Counter conflict_tablespace;
	PgStat_Counter conflict_lock;
	PgStat_Counter conflict_snapshot;
	PgStat_Counter conflict_logicalslot;
	PgStat_Counter conflict_bufferpin;
	PgStat_Counter conflict_startup_deadlock;
	PgStat_Counter temp_files;
	PgStat_Counter temp_bytes;
	PgStat_Counter deadlocks;
	PgStat_Counter checksum_failures;
	TimestampTz last_checksum_failure;
	PgStat_Counter blk_read_time;	/* times in microseconds */
	PgStat_Counter blk_write_time;
	PgStat_Counter sessions;
	PgStat_Counter session_time;
	PgStat_Counter active_time;
	PgStat_Counter idle_in_transaction_time;
	PgStat_Counter sessions_abandoned;
	PgStat_Counter sessions_fatal;
	PgStat_Counter sessions_killed;
	PgStat_Counter parallel_workers_to_launch;
	PgStat_Counter parallel_workers_launched;

	TimestampTz stat_reset_timestamp;
} PgStat_StatDBEntry;

typedef struct PgStat_StatFuncEntry
{
	PgStat_Counter numcalls;

	PgStat_Counter total_time;	/* times in microseconds */
	PgStat_Counter self_time;
} PgStat_StatFuncEntry;

typedef struct PgStat_StatReplSlotEntry
{
	PgStat_Counter spill_txns;
	PgStat_Counter spill_count;
	PgStat_Counter spill_bytes;
	PgStat_Counter stream_txns;
	PgStat_Counter stream_count;
	PgStat_Counter stream_bytes;
	PgStat_Counter total_txns;
	PgStat_Counter total_bytes;
	TimestampTz stat_reset_timestamp;
} PgStat_StatReplSlotEntry;

typedef struct PgStat_SLRUStats
{
	PgStat_Counter blocks_zeroed;
	PgStat_Counter blocks_hit;
	PgStat_Counter blocks_read;
	PgStat_Counter blocks_written;
	PgStat_Counter blocks_exists;
	PgStat_Counter flush;
	PgStat_Counter truncate;
	TimestampTz stat_reset_timestamp;
} PgStat_SLRUStats;

typedef struct PgStat_StatSubEntry
{
	PgStat_Counter apply_error_count;
	PgStat_Counter sync_error_count;
	PgStat_Counter conflict_count[CONFLICT_NUM_TYPES];
	TimestampTz stat_reset_timestamp;
} PgStat_StatSubEntry;

typedef struct PgStat_StatTabEntry
{
	PgStat_Counter numscans;
	TimestampTz lastscan;

	PgStat_Counter tuples_returned;
	PgStat_Counter tuples_fetched;

	PgStat_Counter tuples_inserted;
	PgStat_Counter tuples_updated;
	PgStat_Counter tuples_deleted;
	PgStat_Counter tuples_hot_updated;
	PgStat_Counter tuples_newpage_updated;

	PgStat_Counter live_tuples;
	PgStat_Counter dead_tuples;
	PgStat_Counter mod_since_analyze;
	PgStat_Counter ins_since_vacuum;

	PgStat_Counter blocks_fetched;
	PgStat_Counter blocks_hit;

	TimestampTz last_vacuum_time;	/* user initiated vacuum */
	PgStat_Counter vacuum_count;
	TimestampTz last_autovacuum_time;	/* autovacuum initiated */
	PgStat_Counter autovacuum_count;
	TimestampTz last_analyze_time;	/* user initiated */
	PgStat_Counter analyze_count;
	TimestampTz last_autoanalyze_time;	/* autovacuum initiated */
	PgStat_Counter autoanalyze_count;

	PgStat_Counter total_vacuum_time;	/* times in milliseconds */
	PgStat_Counter total_autovacuum_time;
	PgStat_Counter total_analyze_time;
	PgStat_Counter total_autoanalyze_time;
} PgStat_StatTabEntry;

/* ------
 * PgStat_WalCounters	WAL activity data gathered from WalUsage
 *
 * This stores all the counters and data gathered from WalUsage for WAL
 * activity statistics, separated into its own structure so as this can be
 * shared across multiple Stats structures.
 * ------
 */
typedef struct PgStat_WalCounters
{
	PgStat_Counter wal_records;
	PgStat_Counter wal_fpi;
	uint64		wal_bytes;
	PgStat_Counter wal_buffers_full;
} PgStat_WalCounters;

/* -------
 * PgStat_WalStats		WAL statistics
 * -------
 */
typedef struct PgStat_WalStats
{
	PgStat_WalCounters wal_counters;
	TimestampTz stat_reset_timestamp;
} PgStat_WalStats;

/*
 * Functions in pgstat.c
 */

/* functions called from postmaster */
extern Size StatsShmemSize(void);
extern void StatsShmemInit(void);

/* Functions called during server startup / shutdown */
extern void pgstat_restore_stats(XLogRecPtr redo);
extern void pgstat_discard_stats(void);
extern void pgstat_before_server_shutdown(int code, Datum arg);

/* Functions for backend initialization */
extern void pgstat_initialize(void);

/* Functions called from backends */
extern long pgstat_report_stat(bool force);
extern void pgstat_force_next_flush(void);

extern void pgstat_reset_counters(void);
extern void pgstat_reset(PgStat_Kind kind, Oid dboid, uint64 objid);
extern void pgstat_reset_of_kind(PgStat_Kind kind);

/* stats accessors */
extern void pgstat_clear_snapshot(void);
extern TimestampTz pgstat_get_stat_snapshot_timestamp(bool *have_snapshot);

/* helpers */
extern PgStat_Kind pgstat_get_kind_from_str(char *kind_str);
extern bool pgstat_have_entry(PgStat_Kind kind, Oid dboid, uint64 objid);


/*
 * Functions in pgstat_archiver.c
 */

extern void pgstat_report_archiver(const char *xlog, bool failed);
extern PgStat_ArchiverStats *pgstat_fetch_stat_archiver(void);

/*
 * Functions in pgstat_backend.c
 */

/* used by pgstat_io.c for I/O stats tracked in backends */
extern void pgstat_count_backend_io_op_time(IOObject io_object,
											IOContext io_context,
											IOOp io_op,
											instr_time io_time);
extern void pgstat_count_backend_io_op(IOObject io_object,
									   IOContext io_context,
									   IOOp io_op, uint32 cnt,
									   uint64 bytes);
extern PgStat_Backend *pgstat_fetch_stat_backend(ProcNumber procNumber);
extern PgStat_Backend *pgstat_fetch_stat_backend_by_pid(int pid,
														BackendType *bktype);
extern bool pgstat_tracks_backend_bktype(BackendType bktype);
extern void pgstat_create_backend(ProcNumber procnum);

/*
 * Functions in pgstat_bgwriter.c
 */

extern void pgstat_report_bgwriter(void);
extern PgStat_BgWriterStats *pgstat_fetch_stat_bgwriter(void);


/*
 * Functions in pgstat_checkpointer.c
 */

extern void pgstat_report_checkpointer(void);
extern PgStat_CheckpointerStats *pgstat_fetch_stat_checkpointer(void);


/*
 * Functions in pgstat_io.c
 */

extern bool pgstat_bktype_io_stats_valid(PgStat_BktypeIO *backend_io,
										 BackendType bktype);
extern void pgstat_count_io_op(IOObject io_object, IOContext io_context,
							   IOOp io_op, uint32 cnt, uint64 bytes);
extern instr_time pgstat_prepare_io_time(bool track_io_guc);
extern void pgstat_count_io_op_time(IOObject io_object, IOContext io_context,
									IOOp io_op, instr_time start_time,
									uint32 cnt, uint64 bytes);

extern PgStat_IO *pgstat_fetch_stat_io(void);
extern const char *pgstat_get_io_context_name(IOContext io_context);
extern const char *pgstat_get_io_object_name(IOObject io_object);

extern bool pgstat_tracks_io_bktype(BackendType bktype);
extern bool pgstat_tracks_io_object(BackendType bktype,
									IOObject io_object, IOContext io_context);
extern bool pgstat_tracks_io_op(BackendType bktype, IOObject io_object,
								IOContext io_context, IOOp io_op);


/*
 * Functions in pgstat_database.c
 */

extern void pgstat_drop_database(Oid databaseid);
extern void pgstat_report_autovac(Oid dboid);
extern void pgstat_report_recovery_conflict(int reason);
extern void pgstat_report_deadlock(void);
extern void pgstat_report_checksum_failures_in_db(Oid dboid, int failurecount);
extern void pgstat_report_checksum_failure(void);
extern void pgstat_report_connect(Oid dboid);
extern void pgstat_update_parallel_workers_stats(PgStat_Counter workers_to_launch,
												 PgStat_Counter workers_launched);

#define pgstat_count_buffer_read_time(n)							\
	(pgStatBlockReadTime += (n))
#define pgstat_count_buffer_write_time(n)							\
	(pgStatBlockWriteTime += (n))
#define pgstat_count_conn_active_time(n)							\
	(pgStatActiveTime += (n))
#define pgstat_count_conn_txn_idle_time(n)							\
	(pgStatTransactionIdleTime += (n))

extern PgStat_StatDBEntry *pgstat_fetch_stat_dbentry(Oid dboid);


/*
 * Functions in pgstat_function.c
 */

extern void pgstat_create_function(Oid proid);
extern void pgstat_drop_function(Oid proid);

struct FunctionCallInfoBaseData;
extern void pgstat_init_function_usage(struct FunctionCallInfoBaseData *fcinfo,
									   PgStat_FunctionCallUsage *fcu);
extern void pgstat_end_function_usage(PgStat_FunctionCallUsage *fcu,
									  bool finalize);

extern PgStat_StatFuncEntry *pgstat_fetch_stat_funcentry(Oid func_id);
extern PgStat_FunctionCounts *find_funcstat_entry(Oid func_id);


/*
 * Functions in pgstat_relation.c
 */

extern void pgstat_create_relation(Relation rel);
extern void pgstat_drop_relation(Relation rel);
extern void pgstat_copy_relation_stats(Relation dst, Relation src);

extern void pgstat_init_relation(Relation rel);
extern void pgstat_assoc_relation(Relation rel);
extern void pgstat_unlink_relation(Relation rel);

extern void pgstat_report_vacuum(Oid tableoid, bool shared,
								 PgStat_Counter livetuples, PgStat_Counter deadtuples,
								 TimestampTz starttime);
extern void pgstat_report_analyze(Relation rel,
								  PgStat_Counter livetuples, PgStat_Counter deadtuples,
								  bool resetcounter, TimestampTz starttime);

/*
 * If stats are enabled, but pending data hasn't been prepared yet, call
 * pgstat_assoc_relation() to do so. See its comment for why this is done
 * separately from pgstat_init_relation().
 */
#define pgstat_should_count_relation(rel)                           \
	(likely((rel)->pgstat_info != NULL) ? true :                    \
	 ((rel)->pgstat_enabled ? pgstat_assoc_relation(rel), true : false))

/* nontransactional event counts are simple enough to inline */

#define pgstat_count_heap_scan(rel)									\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.numscans++;					\
	} while (0)
#define pgstat_count_heap_getnext(rel)								\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.tuples_returned++;			\
	} while (0)
#define pgstat_count_heap_fetch(rel)								\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.tuples_fetched++;			\
	} while (0)
#define pgstat_count_index_scan(rel)								\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.numscans++;					\
	} while (0)
#define pgstat_count_index_tuples(rel, n)							\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.tuples_returned += (n);		\
	} while (0)
#define pgstat_count_buffer_read(rel)								\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.blocks_fetched++;			\
	} while (0)
#define pgstat_count_buffer_hit(rel)								\
	do {															\
		if (pgstat_should_count_relation(rel))						\
			(rel)->pgstat_info->counts.blocks_hit++;				\
	} while (0)

extern void pgstat_count_heap_insert(Relation rel, PgStat_Counter n);
extern void pgstat_count_heap_update(Relation rel, bool hot, bool newpage);
extern void pgstat_count_heap_delete(Relation rel);
extern void pgstat_count_truncate(Relation rel);
extern void pgstat_update_heap_dead_tuples(Relation rel, int delta);

extern void pgstat_twophase_postcommit(TransactionId xid, uint16 info,
									   void *recdata, uint32 len);
extern void pgstat_twophase_postabort(TransactionId xid, uint16 info,
									  void *recdata, uint32 len);

extern PgStat_StatTabEntry *pgstat_fetch_stat_tabentry(Oid relid);
extern PgStat_StatTabEntry *pgstat_fetch_stat_tabentry_ext(bool shared,
														   Oid reloid);
extern PgStat_TableStatus *find_tabstat_entry(Oid rel_id);


/*
 * Functions in pgstat_replslot.c
 */

extern void pgstat_reset_replslot(const char *name);
struct ReplicationSlot;
extern void pgstat_report_replslot(struct ReplicationSlot *slot, const PgStat_StatReplSlotEntry *repSlotStat);
extern void pgstat_create_replslot(struct ReplicationSlot *slot);
extern void pgstat_acquire_replslot(struct ReplicationSlot *slot);
extern void pgstat_drop_replslot(struct ReplicationSlot *slot);
extern PgStat_StatReplSlotEntry *pgstat_fetch_replslot(NameData slotname);


/*
 * Functions in pgstat_slru.c
 */

extern void pgstat_reset_slru(const char *);
extern void pgstat_count_slru_page_zeroed(int slru_idx);
extern void pgstat_count_slru_page_hit(int slru_idx);
extern void pgstat_count_slru_page_read(int slru_idx);
extern void pgstat_count_slru_page_written(int slru_idx);
extern void pgstat_count_slru_page_exists(int slru_idx);
extern void pgstat_count_slru_flush(int slru_idx);
extern void pgstat_count_slru_truncate(int slru_idx);
extern const char *pgstat_get_slru_name(int slru_idx);
extern int	pgstat_get_slru_index(const char *name);
extern PgStat_SLRUStats *pgstat_fetch_slru(void);


/*
 * Functions in pgstat_subscription.c
 */

extern void pgstat_report_subscription_error(Oid subid, bool is_apply_error);
extern void pgstat_report_subscription_conflict(Oid subid, ConflictType type);
extern void pgstat_create_subscription(Oid subid);
extern void pgstat_drop_subscription(Oid subid);
extern PgStat_StatSubEntry *pgstat_fetch_stat_subscription(Oid subid);


/*
 * Functions in pgstat_xact.c
 */

extern void AtEOXact_PgStat(bool isCommit, bool parallel);
extern void AtEOSubXact_PgStat(bool isCommit, int nestDepth);
extern void AtPrepare_PgStat(void);
extern void PostPrepare_PgStat(void);
struct xl_xact_stats_item;
extern int	pgstat_get_transactional_drops(bool isCommit, struct xl_xact_stats_item **items);
extern void pgstat_execute_transactional_drops(int ndrops, struct xl_xact_stats_item *items, bool is_redo);


/*
 * Functions in pgstat_wal.c
 */

extern void pgstat_report_wal(bool force);
extern PgStat_WalStats *pgstat_fetch_stat_wal(void);


/*
 * Variables in pgstat.c
 */

/* GUC parameters */
extern PGDLLIMPORT bool pgstat_track_counts;
extern PGDLLIMPORT int pgstat_track_functions;
extern PGDLLIMPORT int pgstat_fetch_consistency;


/*
 * Variables in pgstat_bgwriter.c
 */

/* updated directly by bgwriter and bufmgr */
extern PGDLLIMPORT PgStat_BgWriterStats PendingBgWriterStats;


/*
 * Variables in pgstat_checkpointer.c
 */

/*
 * Checkpointer statistics counters are updated directly by checkpointer and
 * bufmgr.
 */
extern PGDLLIMPORT PgStat_CheckpointerStats PendingCheckpointerStats;


/*
 * Variables in pgstat_database.c
 */

/* Updated by pgstat_count_buffer_*_time macros */
extern PGDLLIMPORT PgStat_Counter pgStatBlockReadTime;
extern PGDLLIMPORT PgStat_Counter pgStatBlockWriteTime;

/*
 * Updated by pgstat_count_conn_*_time macros, called by
 * pgstat_report_activity().
 */
extern PGDLLIMPORT PgStat_Counter pgStatActiveTime;
extern PGDLLIMPORT PgStat_Counter pgStatTransactionIdleTime;

/* updated by the traffic cop and in errfinish() */
extern PGDLLIMPORT SessionEndType pgStatSessionEndCause;

#endif							/* PGSTAT_H */
