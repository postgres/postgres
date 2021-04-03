/* ----------
 *	pgstat.h
 *
 *	Definitions for the PostgreSQL statistics collector daemon.
 *
 *	Copyright (c) 2001-2021, PostgreSQL Global Development Group
 *
 *	src/include/pgstat.h
 * ----------
 */
#ifndef PGSTAT_H
#define PGSTAT_H

#include "datatype/timestamp.h"
#include "portability/instr_time.h"
#include "postmaster/pgarch.h" /* for MAX_XFN_CHARS */
#include "utils/backend_progress.h" /* for backward compatibility */
#include "utils/backend_status.h" /* for backward compatibility */
#include "utils/hsearch.h"
#include "utils/relcache.h"
#include "utils/wait_event.h" /* for backward compatibility */


/* ----------
 * Paths for the statistics files (relative to installation's $PGDATA).
 * ----------
 */
#define PGSTAT_STAT_PERMANENT_DIRECTORY		"pg_stat"
#define PGSTAT_STAT_PERMANENT_FILENAME		"pg_stat/global.stat"
#define PGSTAT_STAT_PERMANENT_TMPFILE		"pg_stat/global.tmp"

/* Default directory to store temporary statistics data in */
#define PG_STAT_TMP_DIR		"pg_stat_tmp"

/* Values for track_functions GUC variable --- order is significant! */
typedef enum TrackFunctionsLevel
{
	TRACK_FUNC_OFF,
	TRACK_FUNC_PL,
	TRACK_FUNC_ALL
}			TrackFunctionsLevel;

/* Values to track the cause of session termination */
typedef enum SessionEndType
{
	DISCONNECT_NOT_YET,			/* still active */
	DISCONNECT_NORMAL,
	DISCONNECT_CLIENT_EOF,
	DISCONNECT_FATAL,
	DISCONNECT_KILLED
} SessionEndType;

/* ----------
 * The types of backend -> collector messages
 * ----------
 */
typedef enum StatMsgType
{
	PGSTAT_MTYPE_DUMMY,
	PGSTAT_MTYPE_INQUIRY,
	PGSTAT_MTYPE_TABSTAT,
	PGSTAT_MTYPE_TABPURGE,
	PGSTAT_MTYPE_DROPDB,
	PGSTAT_MTYPE_RESETCOUNTER,
	PGSTAT_MTYPE_RESETSHAREDCOUNTER,
	PGSTAT_MTYPE_RESETSINGLECOUNTER,
	PGSTAT_MTYPE_RESETSLRUCOUNTER,
	PGSTAT_MTYPE_RESETREPLSLOTCOUNTER,
	PGSTAT_MTYPE_AUTOVAC_START,
	PGSTAT_MTYPE_VACUUM,
	PGSTAT_MTYPE_ANALYZE,
	PGSTAT_MTYPE_ARCHIVER,
	PGSTAT_MTYPE_BGWRITER,
	PGSTAT_MTYPE_WAL,
	PGSTAT_MTYPE_SLRU,
	PGSTAT_MTYPE_FUNCSTAT,
	PGSTAT_MTYPE_FUNCPURGE,
	PGSTAT_MTYPE_RECOVERYCONFLICT,
	PGSTAT_MTYPE_TEMPFILE,
	PGSTAT_MTYPE_DEADLOCK,
	PGSTAT_MTYPE_CHECKSUMFAILURE,
	PGSTAT_MTYPE_REPLSLOT,
	PGSTAT_MTYPE_CONNECTION,
} StatMsgType;

/* ----------
 * The data type used for counters.
 * ----------
 */
typedef int64 PgStat_Counter;

/* ----------
 * PgStat_TableCounts			The actual per-table counts kept by a backend
 *
 * This struct should contain only actual event counters, because we memcmp
 * it against zeroes to detect whether there are any counts to transmit.
 * It is a component of PgStat_TableStatus (within-backend state) and
 * PgStat_TableEntry (the transmitted message format).
 *
 * Note: for a table, tuples_returned is the number of tuples successfully
 * fetched by heap_getnext, while tuples_fetched is the number of tuples
 * successfully fetched by heap_fetch under the control of bitmap indexscans.
 * For an index, tuples_returned is the number of index entries returned by
 * the index AM, while tuples_fetched is the number of tuples successfully
 * fetched by heap_fetch under the control of simple indexscans for this index.
 *
 * tuples_inserted/updated/deleted/hot_updated count attempted actions,
 * regardless of whether the transaction committed.  delta_live_tuples,
 * delta_dead_tuples, and changed_tuples are set depending on commit or abort.
 * Note that delta_live_tuples and delta_dead_tuples can be negative!
 * ----------
 */
typedef struct PgStat_TableCounts
{
	PgStat_Counter t_numscans;

	PgStat_Counter t_tuples_returned;
	PgStat_Counter t_tuples_fetched;

	PgStat_Counter t_tuples_inserted;
	PgStat_Counter t_tuples_updated;
	PgStat_Counter t_tuples_deleted;
	PgStat_Counter t_tuples_hot_updated;
	bool		t_truncated;

	PgStat_Counter t_delta_live_tuples;
	PgStat_Counter t_delta_dead_tuples;
	PgStat_Counter t_changed_tuples;

	PgStat_Counter t_blocks_fetched;
	PgStat_Counter t_blocks_hit;
} PgStat_TableCounts;

/* Possible targets for resetting cluster-wide shared values */
typedef enum PgStat_Shared_Reset_Target
{
	RESET_ARCHIVER,
	RESET_BGWRITER,
	RESET_WAL
} PgStat_Shared_Reset_Target;

/* Possible object types for resetting single counters */
typedef enum PgStat_Single_Reset_Type
{
	RESET_TABLE,
	RESET_FUNCTION
} PgStat_Single_Reset_Type;

/* ------------------------------------------------------------
 * Structures kept in backend local memory while accumulating counts
 * ------------------------------------------------------------
 */


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
	Oid			t_id;			/* table's OID */
	bool		t_shared;		/* is it a shared catalog? */
	struct PgStat_TableXactStatus *trans;	/* lowest subxact's counts */
	PgStat_TableCounts t_counts;	/* event counts to be sent */
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
	bool		truncated;		/* relation truncated in this (sub)xact */
	PgStat_Counter inserted_pre_trunc;	/* tuples inserted prior to truncate */
	PgStat_Counter updated_pre_trunc;	/* tuples updated prior to truncate */
	PgStat_Counter deleted_pre_trunc;	/* tuples deleted prior to truncate */
	int			nest_level;		/* subtransaction nest level */
	/* links to other structs for same relation: */
	struct PgStat_TableXactStatus *upper;	/* next higher subxact if any */
	PgStat_TableStatus *parent; /* per-table status */
	/* structs of same subxact level are linked here: */
	struct PgStat_TableXactStatus *next;	/* next of same subxact */
} PgStat_TableXactStatus;


/* ------------------------------------------------------------
 * Message formats follow
 * ------------------------------------------------------------
 */


/* ----------
 * PgStat_MsgHdr				The common message header
 * ----------
 */
typedef struct PgStat_MsgHdr
{
	StatMsgType m_type;
	int			m_size;
} PgStat_MsgHdr;

/* ----------
 * Space available in a message.  This will keep the UDP packets below 1K,
 * which should fit unfragmented into the MTU of the loopback interface.
 * (Larger values of PGSTAT_MAX_MSG_SIZE would work for that on most
 * platforms, but we're being conservative here.)
 * ----------
 */
#define PGSTAT_MAX_MSG_SIZE 1000
#define PGSTAT_MSG_PAYLOAD	(PGSTAT_MAX_MSG_SIZE - sizeof(PgStat_MsgHdr))


/* ----------
 * PgStat_MsgDummy				A dummy message, ignored by the collector
 * ----------
 */
typedef struct PgStat_MsgDummy
{
	PgStat_MsgHdr m_hdr;
} PgStat_MsgDummy;


/* ----------
 * PgStat_MsgInquiry			Sent by a backend to ask the collector
 *								to write the stats file(s).
 *
 * Ordinarily, an inquiry message prompts writing of the global stats file,
 * the stats file for shared catalogs, and the stats file for the specified
 * database.  If databaseid is InvalidOid, only the first two are written.
 *
 * New file(s) will be written only if the existing file has a timestamp
 * older than the specified cutoff_time; this prevents duplicated effort
 * when multiple requests arrive at nearly the same time, assuming that
 * backends send requests with cutoff_times a little bit in the past.
 *
 * clock_time should be the requestor's current local time; the collector
 * uses this to check for the system clock going backward, but it has no
 * effect unless that occurs.  We assume clock_time >= cutoff_time, though.
 * ----------
 */

typedef struct PgStat_MsgInquiry
{
	PgStat_MsgHdr m_hdr;
	TimestampTz clock_time;		/* observed local clock time */
	TimestampTz cutoff_time;	/* minimum acceptable file timestamp */
	Oid			databaseid;		/* requested DB (InvalidOid => shared only) */
} PgStat_MsgInquiry;


/* ----------
 * PgStat_TableEntry			Per-table info in a MsgTabstat
 * ----------
 */
typedef struct PgStat_TableEntry
{
	Oid			t_id;
	PgStat_TableCounts t_counts;
} PgStat_TableEntry;

/* ----------
 * PgStat_MsgTabstat			Sent by the backend to report table
 *								and buffer access statistics.
 * ----------
 */
#define PGSTAT_NUM_TABENTRIES  \
	((PGSTAT_MSG_PAYLOAD - sizeof(Oid) - 3 * sizeof(int) - 2 * sizeof(PgStat_Counter))	\
	 / sizeof(PgStat_TableEntry))

typedef struct PgStat_MsgTabstat
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	int			m_nentries;
	int			m_xact_commit;
	int			m_xact_rollback;
	PgStat_Counter m_block_read_time;	/* times in microseconds */
	PgStat_Counter m_block_write_time;
	PgStat_TableEntry m_entry[PGSTAT_NUM_TABENTRIES];
} PgStat_MsgTabstat;


/* ----------
 * PgStat_MsgTabpurge			Sent by the backend to tell the collector
 *								about dead tables.
 * ----------
 */
#define PGSTAT_NUM_TABPURGE  \
	((PGSTAT_MSG_PAYLOAD - sizeof(Oid) - sizeof(int))  \
	 / sizeof(Oid))

typedef struct PgStat_MsgTabpurge
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	int			m_nentries;
	Oid			m_tableid[PGSTAT_NUM_TABPURGE];
} PgStat_MsgTabpurge;


/* ----------
 * PgStat_MsgDropdb				Sent by the backend to tell the collector
 *								about a dropped database
 * ----------
 */
typedef struct PgStat_MsgDropdb
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
} PgStat_MsgDropdb;


/* ----------
 * PgStat_MsgResetcounter		Sent by the backend to tell the collector
 *								to reset counters
 * ----------
 */
typedef struct PgStat_MsgResetcounter
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
} PgStat_MsgResetcounter;

/* ----------
 * PgStat_MsgResetsharedcounter Sent by the backend to tell the collector
 *								to reset a shared counter
 * ----------
 */
typedef struct PgStat_MsgResetsharedcounter
{
	PgStat_MsgHdr m_hdr;
	PgStat_Shared_Reset_Target m_resettarget;
} PgStat_MsgResetsharedcounter;

/* ----------
 * PgStat_MsgResetsinglecounter Sent by the backend to tell the collector
 *								to reset a single counter
 * ----------
 */
typedef struct PgStat_MsgResetsinglecounter
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	PgStat_Single_Reset_Type m_resettype;
	Oid			m_objectid;
} PgStat_MsgResetsinglecounter;

/* ----------
 * PgStat_MsgResetslrucounter Sent by the backend to tell the collector
 *								to reset a SLRU counter
 * ----------
 */
typedef struct PgStat_MsgResetslrucounter
{
	PgStat_MsgHdr m_hdr;
	int			m_index;
} PgStat_MsgResetslrucounter;

/* ----------
 * PgStat_MsgResetreplslotcounter Sent by the backend to tell the collector
 *								to reset replication slot counter(s)
 * ----------
 */
typedef struct PgStat_MsgResetreplslotcounter
{
	PgStat_MsgHdr m_hdr;
	char		m_slotname[NAMEDATALEN];
	bool		clearall;
} PgStat_MsgResetreplslotcounter;

/* ----------
 * PgStat_MsgAutovacStart		Sent by the autovacuum daemon to signal
 *								that a database is going to be processed
 * ----------
 */
typedef struct PgStat_MsgAutovacStart
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	TimestampTz m_start_time;
} PgStat_MsgAutovacStart;


/* ----------
 * PgStat_MsgVacuum				Sent by the backend or autovacuum daemon
 *								after VACUUM
 * ----------
 */
typedef struct PgStat_MsgVacuum
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	Oid			m_tableoid;
	bool		m_autovacuum;
	TimestampTz m_vacuumtime;
	PgStat_Counter m_live_tuples;
	PgStat_Counter m_dead_tuples;
} PgStat_MsgVacuum;


/* ----------
 * PgStat_MsgAnalyze			Sent by the backend or autovacuum daemon
 *								after ANALYZE
 * ----------
 */
typedef struct PgStat_MsgAnalyze
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	Oid			m_tableoid;
	bool		m_autovacuum;
	bool		m_resetcounter;
	TimestampTz m_analyzetime;
	PgStat_Counter m_live_tuples;
	PgStat_Counter m_dead_tuples;
} PgStat_MsgAnalyze;


/* ----------
 * PgStat_MsgArchiver			Sent by the archiver to update statistics.
 * ----------
 */
typedef struct PgStat_MsgArchiver
{
	PgStat_MsgHdr m_hdr;
	bool		m_failed;		/* Failed attempt */
	char		m_xlog[MAX_XFN_CHARS + 1];
	TimestampTz m_timestamp;
} PgStat_MsgArchiver;

/* ----------
 * PgStat_MsgBgWriter			Sent by the bgwriter to update statistics.
 * ----------
 */
typedef struct PgStat_MsgBgWriter
{
	PgStat_MsgHdr m_hdr;

	PgStat_Counter m_timed_checkpoints;
	PgStat_Counter m_requested_checkpoints;
	PgStat_Counter m_buf_written_checkpoints;
	PgStat_Counter m_buf_written_clean;
	PgStat_Counter m_maxwritten_clean;
	PgStat_Counter m_buf_written_backend;
	PgStat_Counter m_buf_fsync_backend;
	PgStat_Counter m_buf_alloc;
	PgStat_Counter m_checkpoint_write_time; /* times in milliseconds */
	PgStat_Counter m_checkpoint_sync_time;
} PgStat_MsgBgWriter;

/* ----------
 * PgStat_MsgWal			Sent by backends and background processes to update WAL statistics.
 * ----------
 */
typedef struct PgStat_MsgWal
{
	PgStat_MsgHdr m_hdr;
	PgStat_Counter m_wal_records;
	PgStat_Counter m_wal_fpi;
	uint64		m_wal_bytes;
	PgStat_Counter m_wal_buffers_full;
	PgStat_Counter m_wal_write;
	PgStat_Counter m_wal_sync;
	PgStat_Counter m_wal_write_time;	/* time spent writing wal records in
										 * microseconds */
	PgStat_Counter m_wal_sync_time; /* time spent syncing wal records in
									 * microseconds */
} PgStat_MsgWal;

/* ----------
 * PgStat_MsgSLRU			Sent by a backend to update SLRU statistics.
 * ----------
 */
typedef struct PgStat_MsgSLRU
{
	PgStat_MsgHdr m_hdr;
	PgStat_Counter m_index;
	PgStat_Counter m_blocks_zeroed;
	PgStat_Counter m_blocks_hit;
	PgStat_Counter m_blocks_read;
	PgStat_Counter m_blocks_written;
	PgStat_Counter m_blocks_exists;
	PgStat_Counter m_flush;
	PgStat_Counter m_truncate;
} PgStat_MsgSLRU;

/* ----------
 * PgStat_MsgReplSlot	Sent by a backend or a wal sender to update replication
 *						slot statistics.
 * ----------
 */
typedef struct PgStat_MsgReplSlot
{
	PgStat_MsgHdr m_hdr;
	char		m_slotname[NAMEDATALEN];
	bool		m_drop;
	PgStat_Counter m_spill_txns;
	PgStat_Counter m_spill_count;
	PgStat_Counter m_spill_bytes;
	PgStat_Counter m_stream_txns;
	PgStat_Counter m_stream_count;
	PgStat_Counter m_stream_bytes;
} PgStat_MsgReplSlot;


/* ----------
 * PgStat_MsgRecoveryConflict	Sent by the backend upon recovery conflict
 * ----------
 */
typedef struct PgStat_MsgRecoveryConflict
{
	PgStat_MsgHdr m_hdr;

	Oid			m_databaseid;
	int			m_reason;
} PgStat_MsgRecoveryConflict;

/* ----------
 * PgStat_MsgTempFile	Sent by the backend upon creating a temp file
 * ----------
 */
typedef struct PgStat_MsgTempFile
{
	PgStat_MsgHdr m_hdr;

	Oid			m_databaseid;
	size_t		m_filesize;
} PgStat_MsgTempFile;

/* ----------
 * PgStat_FunctionCounts	The actual per-function counts kept by a backend
 *
 * This struct should contain only actual event counters, because we memcmp
 * it against zeroes to detect whether there are any counts to transmit.
 *
 * Note that the time counters are in instr_time format here.  We convert to
 * microseconds in PgStat_Counter format when transmitting to the collector.
 * ----------
 */
typedef struct PgStat_FunctionCounts
{
	PgStat_Counter f_numcalls;
	instr_time	f_total_time;
	instr_time	f_self_time;
} PgStat_FunctionCounts;

/* ----------
 * PgStat_BackendFunctionEntry	Entry in backend's per-function hash table
 * ----------
 */
typedef struct PgStat_BackendFunctionEntry
{
	Oid			f_id;
	PgStat_FunctionCounts f_counts;
} PgStat_BackendFunctionEntry;

/* ----------
 * PgStat_FunctionEntry			Per-function info in a MsgFuncstat
 * ----------
 */
typedef struct PgStat_FunctionEntry
{
	Oid			f_id;
	PgStat_Counter f_numcalls;
	PgStat_Counter f_total_time;	/* times in microseconds */
	PgStat_Counter f_self_time;
} PgStat_FunctionEntry;

/* ----------
 * PgStat_MsgFuncstat			Sent by the backend to report function
 *								usage statistics.
 * ----------
 */
#define PGSTAT_NUM_FUNCENTRIES	\
	((PGSTAT_MSG_PAYLOAD - sizeof(Oid) - sizeof(int))  \
	 / sizeof(PgStat_FunctionEntry))

typedef struct PgStat_MsgFuncstat
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	int			m_nentries;
	PgStat_FunctionEntry m_entry[PGSTAT_NUM_FUNCENTRIES];
} PgStat_MsgFuncstat;

/* ----------
 * PgStat_MsgFuncpurge			Sent by the backend to tell the collector
 *								about dead functions.
 * ----------
 */
#define PGSTAT_NUM_FUNCPURGE  \
	((PGSTAT_MSG_PAYLOAD - sizeof(Oid) - sizeof(int))  \
	 / sizeof(Oid))

typedef struct PgStat_MsgFuncpurge
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	int			m_nentries;
	Oid			m_functionid[PGSTAT_NUM_FUNCPURGE];
} PgStat_MsgFuncpurge;

/* ----------
 * PgStat_MsgDeadlock			Sent by the backend to tell the collector
 *								about a deadlock that occurred.
 * ----------
 */
typedef struct PgStat_MsgDeadlock
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
} PgStat_MsgDeadlock;

/* ----------
 * PgStat_MsgChecksumFailure	Sent by the backend to tell the collector
 *								about checksum failures noticed.
 * ----------
 */
typedef struct PgStat_MsgChecksumFailure
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	int			m_failurecount;
	TimestampTz m_failure_time;
} PgStat_MsgChecksumFailure;

/* ----------
 * PgStat_MsgConn			Sent by the backend to update connection statistics.
 * ----------
 */
typedef struct PgStat_MsgConn
{
	PgStat_MsgHdr m_hdr;
	Oid			m_databaseid;
	PgStat_Counter m_count;
	PgStat_Counter m_session_time;
	PgStat_Counter m_active_time;
	PgStat_Counter m_idle_in_xact_time;
	SessionEndType m_disconnect;
} PgStat_MsgConn;


/* ----------
 * PgStat_Msg					Union over all possible messages.
 * ----------
 */
typedef union PgStat_Msg
{
	PgStat_MsgHdr msg_hdr;
	PgStat_MsgDummy msg_dummy;
	PgStat_MsgInquiry msg_inquiry;
	PgStat_MsgTabstat msg_tabstat;
	PgStat_MsgTabpurge msg_tabpurge;
	PgStat_MsgDropdb msg_dropdb;
	PgStat_MsgResetcounter msg_resetcounter;
	PgStat_MsgResetsharedcounter msg_resetsharedcounter;
	PgStat_MsgResetsinglecounter msg_resetsinglecounter;
	PgStat_MsgResetslrucounter msg_resetslrucounter;
	PgStat_MsgResetreplslotcounter msg_resetreplslotcounter;
	PgStat_MsgAutovacStart msg_autovacuum_start;
	PgStat_MsgVacuum msg_vacuum;
	PgStat_MsgAnalyze msg_analyze;
	PgStat_MsgArchiver msg_archiver;
	PgStat_MsgBgWriter msg_bgwriter;
	PgStat_MsgWal msg_wal;
	PgStat_MsgSLRU msg_slru;
	PgStat_MsgFuncstat msg_funcstat;
	PgStat_MsgFuncpurge msg_funcpurge;
	PgStat_MsgRecoveryConflict msg_recoveryconflict;
	PgStat_MsgDeadlock msg_deadlock;
	PgStat_MsgTempFile msg_tempfile;
	PgStat_MsgChecksumFailure msg_checksumfailure;
	PgStat_MsgReplSlot msg_replslot;
	PgStat_MsgConn msg_conn;
} PgStat_Msg;


/* ------------------------------------------------------------
 * Statistic collector data structures follow
 *
 * PGSTAT_FILE_FORMAT_ID should be changed whenever any of these
 * data structures change.
 * ------------------------------------------------------------
 */

#define PGSTAT_FILE_FORMAT_ID	0x01A5BCA1

/* ----------
 * PgStat_StatDBEntry			The collector's data per database
 * ----------
 */
typedef struct PgStat_StatDBEntry
{
	Oid			databaseid;
	PgStat_Counter n_xact_commit;
	PgStat_Counter n_xact_rollback;
	PgStat_Counter n_blocks_fetched;
	PgStat_Counter n_blocks_hit;
	PgStat_Counter n_tuples_returned;
	PgStat_Counter n_tuples_fetched;
	PgStat_Counter n_tuples_inserted;
	PgStat_Counter n_tuples_updated;
	PgStat_Counter n_tuples_deleted;
	TimestampTz last_autovac_time;
	PgStat_Counter n_conflict_tablespace;
	PgStat_Counter n_conflict_lock;
	PgStat_Counter n_conflict_snapshot;
	PgStat_Counter n_conflict_bufferpin;
	PgStat_Counter n_conflict_startup_deadlock;
	PgStat_Counter n_temp_files;
	PgStat_Counter n_temp_bytes;
	PgStat_Counter n_deadlocks;
	PgStat_Counter n_checksum_failures;
	TimestampTz last_checksum_failure;
	PgStat_Counter n_block_read_time;	/* times in microseconds */
	PgStat_Counter n_block_write_time;
	PgStat_Counter n_sessions;
	PgStat_Counter total_session_time;
	PgStat_Counter total_active_time;
	PgStat_Counter total_idle_in_xact_time;
	PgStat_Counter n_sessions_abandoned;
	PgStat_Counter n_sessions_fatal;
	PgStat_Counter n_sessions_killed;

	TimestampTz stat_reset_timestamp;
	TimestampTz stats_timestamp;	/* time of db stats file update */

	/*
	 * tables and functions must be last in the struct, because we don't write
	 * the pointers out to the stats file.
	 */
	HTAB	   *tables;
	HTAB	   *functions;
} PgStat_StatDBEntry;


/* ----------
 * PgStat_StatTabEntry			The collector's data per table (or index)
 * ----------
 */
typedef struct PgStat_StatTabEntry
{
	Oid			tableid;

	PgStat_Counter numscans;

	PgStat_Counter tuples_returned;
	PgStat_Counter tuples_fetched;

	PgStat_Counter tuples_inserted;
	PgStat_Counter tuples_updated;
	PgStat_Counter tuples_deleted;
	PgStat_Counter tuples_hot_updated;

	PgStat_Counter n_live_tuples;
	PgStat_Counter n_dead_tuples;
	PgStat_Counter changes_since_analyze;
	PgStat_Counter inserts_since_vacuum;

	PgStat_Counter blocks_fetched;
	PgStat_Counter blocks_hit;

	TimestampTz vacuum_timestamp;	/* user initiated vacuum */
	PgStat_Counter vacuum_count;
	TimestampTz autovac_vacuum_timestamp;	/* autovacuum initiated */
	PgStat_Counter autovac_vacuum_count;
	TimestampTz analyze_timestamp;	/* user initiated */
	PgStat_Counter analyze_count;
	TimestampTz autovac_analyze_timestamp;	/* autovacuum initiated */
	PgStat_Counter autovac_analyze_count;
} PgStat_StatTabEntry;


/* ----------
 * PgStat_StatFuncEntry			The collector's data per function
 * ----------
 */
typedef struct PgStat_StatFuncEntry
{
	Oid			functionid;

	PgStat_Counter f_numcalls;

	PgStat_Counter f_total_time;	/* times in microseconds */
	PgStat_Counter f_self_time;
} PgStat_StatFuncEntry;


/*
 * Archiver statistics kept in the stats collector
 */
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

/*
 * Global statistics kept in the stats collector
 */
typedef struct PgStat_GlobalStats
{
	TimestampTz stats_timestamp;	/* time of stats file update */
	PgStat_Counter timed_checkpoints;
	PgStat_Counter requested_checkpoints;
	PgStat_Counter checkpoint_write_time;	/* times in milliseconds */
	PgStat_Counter checkpoint_sync_time;
	PgStat_Counter buf_written_checkpoints;
	PgStat_Counter buf_written_clean;
	PgStat_Counter maxwritten_clean;
	PgStat_Counter buf_written_backend;
	PgStat_Counter buf_fsync_backend;
	PgStat_Counter buf_alloc;
	TimestampTz stat_reset_timestamp;
} PgStat_GlobalStats;

/*
 * WAL statistics kept in the stats collector
 */
typedef struct PgStat_WalStats
{
	PgStat_Counter wal_records;
	PgStat_Counter wal_fpi;
	uint64		wal_bytes;
	PgStat_Counter wal_buffers_full;
	PgStat_Counter wal_write;
	PgStat_Counter wal_sync;
	PgStat_Counter wal_write_time;
	PgStat_Counter wal_sync_time;
	TimestampTz stat_reset_timestamp;
} PgStat_WalStats;

/*
 * SLRU statistics kept in the stats collector
 */
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

/*
 * Replication slot statistics kept in the stats collector
 */
typedef struct PgStat_ReplSlotStats
{
	char		slotname[NAMEDATALEN];
	PgStat_Counter spill_txns;
	PgStat_Counter spill_count;
	PgStat_Counter spill_bytes;
	PgStat_Counter stream_txns;
	PgStat_Counter stream_count;
	PgStat_Counter stream_bytes;
	TimestampTz stat_reset_timestamp;
} PgStat_ReplSlotStats;


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
	instr_time	f_start;
} PgStat_FunctionCallUsage;


/* ----------
 * GUC parameters
 * ----------
 */
extern PGDLLIMPORT bool pgstat_track_counts;
extern PGDLLIMPORT int pgstat_track_functions;
extern char *pgstat_stat_directory;
extern char *pgstat_stat_tmpname;
extern char *pgstat_stat_filename;

/*
 * BgWriter statistics counters are updated directly by bgwriter and bufmgr
 */
extern PgStat_MsgBgWriter BgWriterStats;

/*
 * WAL statistics counter is updated by backends and background processes
 */
extern PgStat_MsgWal WalStats;

/*
 * Updated by pgstat_count_buffer_*_time macros
 */
extern PgStat_Counter pgStatBlockReadTime;
extern PgStat_Counter pgStatBlockWriteTime;

/*
 * Updated by pgstat_count_conn_*_time macros, called by
 * pgstat_report_activity().
 */
extern PgStat_Counter pgStatActiveTime;
extern PgStat_Counter pgStatTransactionIdleTime;


/*
 * Updated by the traffic cop and in errfinish()
 */
extern SessionEndType pgStatSessionEndCause;

/* ----------
 * Functions called from postmaster
 * ----------
 */
extern void pgstat_init(void);
extern int	pgstat_start(void);
extern void pgstat_reset_all(void);
extern void allow_immediate_pgstat_restart(void);

#ifdef EXEC_BACKEND
extern void PgstatCollectorMain(int argc, char *argv[]) pg_attribute_noreturn();
#endif


/* ----------
 * Functions called from backends
 * ----------
 */
extern void pgstat_ping(void);

extern void pgstat_report_stat(bool force);
extern void pgstat_vacuum_stat(void);
extern void pgstat_drop_database(Oid databaseid);

extern void pgstat_clear_snapshot(void);
extern void pgstat_reset_counters(void);
extern void pgstat_reset_shared_counters(const char *);
extern void pgstat_reset_single_counter(Oid objectid, PgStat_Single_Reset_Type type);
extern void pgstat_reset_slru_counter(const char *);
extern void pgstat_reset_replslot_counter(const char *name);

extern void pgstat_report_autovac(Oid dboid);
extern void pgstat_report_vacuum(Oid tableoid, bool shared,
								 PgStat_Counter livetuples, PgStat_Counter deadtuples);
extern void pgstat_report_analyze(Relation rel,
								  PgStat_Counter livetuples, PgStat_Counter deadtuples,
								  bool resetcounter);

extern void pgstat_report_recovery_conflict(int reason);
extern void pgstat_report_deadlock(void);
extern void pgstat_report_checksum_failures_in_db(Oid dboid, int failurecount);
extern void pgstat_report_checksum_failure(void);
extern void pgstat_report_replslot(const char *slotname, PgStat_Counter spilltxns,
								   PgStat_Counter spillcount, PgStat_Counter spillbytes,
								   PgStat_Counter streamtxns, PgStat_Counter streamcount,
								   PgStat_Counter streambytes);
extern void pgstat_report_replslot_drop(const char *slotname);

extern void pgstat_initialize(void);


extern PgStat_TableStatus *find_tabstat_entry(Oid rel_id);
extern PgStat_BackendFunctionEntry *find_funcstat_entry(Oid func_id);

extern void pgstat_initstats(Relation rel);

/* nontransactional event counts are simple enough to inline */

#define pgstat_count_heap_scan(rel)									\
	do {															\
		if ((rel)->pgstat_info != NULL)								\
			(rel)->pgstat_info->t_counts.t_numscans++;				\
	} while (0)
#define pgstat_count_heap_getnext(rel)								\
	do {															\
		if ((rel)->pgstat_info != NULL)								\
			(rel)->pgstat_info->t_counts.t_tuples_returned++;		\
	} while (0)
#define pgstat_count_heap_fetch(rel)								\
	do {															\
		if ((rel)->pgstat_info != NULL)								\
			(rel)->pgstat_info->t_counts.t_tuples_fetched++;		\
	} while (0)
#define pgstat_count_index_scan(rel)								\
	do {															\
		if ((rel)->pgstat_info != NULL)								\
			(rel)->pgstat_info->t_counts.t_numscans++;				\
	} while (0)
#define pgstat_count_index_tuples(rel, n)							\
	do {															\
		if ((rel)->pgstat_info != NULL)								\
			(rel)->pgstat_info->t_counts.t_tuples_returned += (n);	\
	} while (0)
#define pgstat_count_buffer_read(rel)								\
	do {															\
		if ((rel)->pgstat_info != NULL)								\
			(rel)->pgstat_info->t_counts.t_blocks_fetched++;		\
	} while (0)
#define pgstat_count_buffer_hit(rel)								\
	do {															\
		if ((rel)->pgstat_info != NULL)								\
			(rel)->pgstat_info->t_counts.t_blocks_hit++;			\
	} while (0)
#define pgstat_count_buffer_read_time(n)							\
	(pgStatBlockReadTime += (n))
#define pgstat_count_buffer_write_time(n)							\
	(pgStatBlockWriteTime += (n))
#define pgstat_count_conn_active_time(n)							\
	(pgStatActiveTime += (n))
#define pgstat_count_conn_txn_idle_time(n)							\
	(pgStatTransactionIdleTime += (n))

extern void pgstat_count_heap_insert(Relation rel, PgStat_Counter n);
extern void pgstat_count_heap_update(Relation rel, bool hot);
extern void pgstat_count_heap_delete(Relation rel);
extern void pgstat_count_truncate(Relation rel);
extern void pgstat_update_heap_dead_tuples(Relation rel, int delta);

struct FunctionCallInfoBaseData;
extern void pgstat_init_function_usage(struct FunctionCallInfoBaseData *fcinfo,
									   PgStat_FunctionCallUsage *fcu);
extern void pgstat_end_function_usage(PgStat_FunctionCallUsage *fcu,
									  bool finalize);

extern void AtEOXact_PgStat(bool isCommit, bool parallel);
extern void AtEOSubXact_PgStat(bool isCommit, int nestDepth);

extern void AtPrepare_PgStat(void);
extern void PostPrepare_PgStat(void);

extern void pgstat_twophase_postcommit(TransactionId xid, uint16 info,
									   void *recdata, uint32 len);
extern void pgstat_twophase_postabort(TransactionId xid, uint16 info,
									  void *recdata, uint32 len);

extern void pgstat_send_archiver(const char *xlog, bool failed);
extern void pgstat_send_bgwriter(void);
extern void pgstat_report_wal(void);
extern bool pgstat_send_wal(bool force);

/* ----------
 * Support functions for the SQL-callable functions to
 * generate the pgstat* views.
 * ----------
 */
extern PgStat_StatDBEntry *pgstat_fetch_stat_dbentry(Oid dbid);
extern PgStat_StatTabEntry *pgstat_fetch_stat_tabentry(Oid relid);
extern PgStat_StatFuncEntry *pgstat_fetch_stat_funcentry(Oid funcid);
extern PgStat_ArchiverStats *pgstat_fetch_stat_archiver(void);
extern PgStat_GlobalStats *pgstat_fetch_global(void);
extern PgStat_WalStats *pgstat_fetch_stat_wal(void);
extern PgStat_SLRUStats *pgstat_fetch_slru(void);
extern PgStat_ReplSlotStats *pgstat_fetch_replslot(int *nslots_p);

extern void pgstat_count_slru_page_zeroed(int slru_idx);
extern void pgstat_count_slru_page_hit(int slru_idx);
extern void pgstat_count_slru_page_read(int slru_idx);
extern void pgstat_count_slru_page_written(int slru_idx);
extern void pgstat_count_slru_page_exists(int slru_idx);
extern void pgstat_count_slru_flush(int slru_idx);
extern void pgstat_count_slru_truncate(int slru_idx);
extern const char *pgstat_slru_name(int slru_idx);
extern int	pgstat_slru_index(const char *name);

#endif							/* PGSTAT_H */
