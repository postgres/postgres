/*--------------------------------------------------------------------
 *
 * guc_tables.c
 *
 * Static tables for the Grand Unified Configuration scheme.
 *
 * Many of these tables are const.  However, ConfigureNamesBool[]
 * and so on are not, because the structs in those arrays are actually
 * the live per-variable state data that guc.c manipulates.  While many of
 * their fields are intended to be constant, some fields change at runtime.
 *
 *
 * Copyright (c) 2000-2024, PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/guc_tables.c
 *
 *--------------------------------------------------------------------
 */
#include "postgres.h"

#include <float.h>
#include <limits.h>
#ifdef HAVE_SYSLOG
#include <syslog.h>
#endif

#include "access/commit_ts.h"
#include "access/gin.h"
#include "access/slru.h"
#include "access/toast_compression.h"
#include "access/twophase.h"
#include "access/xlog_internal.h"
#include "access/xlogprefetcher.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "archive/archive_module.h"
#include "catalog/namespace.h"
#include "catalog/storage.h"
#include "commands/async.h"
#include "commands/event_trigger.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "commands/user.h"
#include "commands/vacuum.h"
#include "common/file_utils.h"
#include "common/scram-common.h"
#include "jit/jit.h"
#include "libpq/auth.h"
#include "libpq/libpq.h"
#include "libpq/scram.h"
#include "nodes/queryjumble.h"
#include "optimizer/cost.h"
#include "optimizer/geqo.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "parser/parse_expr.h"
#include "parser/parser.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/bgwriter.h"
#include "postmaster/postmaster.h"
#include "postmaster/startup.h"
#include "postmaster/syslogger.h"
#include "postmaster/walsummarizer.h"
#include "postmaster/walwriter.h"
#include "replication/logicallauncher.h"
#include "replication/slot.h"
#include "replication/slotsync.h"
#include "replication/syncrep.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/large_object.h"
#include "storage/pg_shmem.h"
#include "storage/predicate.h"
#include "storage/standby.h"
#include "tcop/backend_startup.h"
#include "tcop/tcopprot.h"
#include "tsearch/ts_cache.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/float.h"
#include "utils/guc_hooks.h"
#include "utils/guc_tables.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/plancache.h"
#include "utils/ps_status.h"
#include "utils/rls.h"
#include "utils/xml.h"

#ifdef TRACE_SYNCSCAN
#include "access/syncscan.h"
#endif

/* This value is normally passed in from the Makefile */
#ifndef PG_KRB_SRVTAB
#define PG_KRB_SRVTAB ""
#endif

/*
 * Options for enum values defined in this module.
 *
 * NOTE! Option values may not contain double quotes!
 */

static const struct config_enum_entry bytea_output_options[] = {
	{"escape", BYTEA_OUTPUT_ESCAPE, false},
	{"hex", BYTEA_OUTPUT_HEX, false},
	{NULL, 0, false}
};

StaticAssertDecl(lengthof(bytea_output_options) == (BYTEA_OUTPUT_HEX + 2),
				 "array length mismatch");

/*
 * We have different sets for client and server message level options because
 * they sort slightly different (see "log" level), and because "fatal"/"panic"
 * aren't sensible for client_min_messages.
 */
static const struct config_enum_entry client_message_level_options[] = {
	{"debug5", DEBUG5, false},
	{"debug4", DEBUG4, false},
	{"debug3", DEBUG3, false},
	{"debug2", DEBUG2, false},
	{"debug1", DEBUG1, false},
	{"debug", DEBUG2, true},
	{"log", LOG, false},
	{"info", INFO, true},
	{"notice", NOTICE, false},
	{"warning", WARNING, false},
	{"error", ERROR, false},
	{NULL, 0, false}
};

static const struct config_enum_entry server_message_level_options[] = {
	{"debug5", DEBUG5, false},
	{"debug4", DEBUG4, false},
	{"debug3", DEBUG3, false},
	{"debug2", DEBUG2, false},
	{"debug1", DEBUG1, false},
	{"debug", DEBUG2, true},
	{"info", INFO, false},
	{"notice", NOTICE, false},
	{"warning", WARNING, false},
	{"error", ERROR, false},
	{"log", LOG, false},
	{"fatal", FATAL, false},
	{"panic", PANIC, false},
	{NULL, 0, false}
};

static const struct config_enum_entry intervalstyle_options[] = {
	{"postgres", INTSTYLE_POSTGRES, false},
	{"postgres_verbose", INTSTYLE_POSTGRES_VERBOSE, false},
	{"sql_standard", INTSTYLE_SQL_STANDARD, false},
	{"iso_8601", INTSTYLE_ISO_8601, false},
	{NULL, 0, false}
};

static const struct config_enum_entry icu_validation_level_options[] = {
	{"disabled", -1, false},
	{"debug5", DEBUG5, false},
	{"debug4", DEBUG4, false},
	{"debug3", DEBUG3, false},
	{"debug2", DEBUG2, false},
	{"debug1", DEBUG1, false},
	{"debug", DEBUG2, true},
	{"log", LOG, false},
	{"info", INFO, true},
	{"notice", NOTICE, false},
	{"warning", WARNING, false},
	{"error", ERROR, false},
	{NULL, 0, false}
};

StaticAssertDecl(lengthof(intervalstyle_options) == (INTSTYLE_ISO_8601 + 2),
				 "array length mismatch");

static const struct config_enum_entry log_error_verbosity_options[] = {
	{"terse", PGERROR_TERSE, false},
	{"default", PGERROR_DEFAULT, false},
	{"verbose", PGERROR_VERBOSE, false},
	{NULL, 0, false}
};

StaticAssertDecl(lengthof(log_error_verbosity_options) == (PGERROR_VERBOSE + 2),
				 "array length mismatch");

static const struct config_enum_entry log_statement_options[] = {
	{"none", LOGSTMT_NONE, false},
	{"ddl", LOGSTMT_DDL, false},
	{"mod", LOGSTMT_MOD, false},
	{"all", LOGSTMT_ALL, false},
	{NULL, 0, false}
};

StaticAssertDecl(lengthof(log_statement_options) == (LOGSTMT_ALL + 2),
				 "array length mismatch");

static const struct config_enum_entry isolation_level_options[] = {
	{"serializable", XACT_SERIALIZABLE, false},
	{"repeatable read", XACT_REPEATABLE_READ, false},
	{"read committed", XACT_READ_COMMITTED, false},
	{"read uncommitted", XACT_READ_UNCOMMITTED, false},
	{NULL, 0}
};

static const struct config_enum_entry session_replication_role_options[] = {
	{"origin", SESSION_REPLICATION_ROLE_ORIGIN, false},
	{"replica", SESSION_REPLICATION_ROLE_REPLICA, false},
	{"local", SESSION_REPLICATION_ROLE_LOCAL, false},
	{NULL, 0, false}
};

StaticAssertDecl(lengthof(session_replication_role_options) == (SESSION_REPLICATION_ROLE_LOCAL + 2),
				 "array length mismatch");

static const struct config_enum_entry syslog_facility_options[] = {
#ifdef HAVE_SYSLOG
	{"local0", LOG_LOCAL0, false},
	{"local1", LOG_LOCAL1, false},
	{"local2", LOG_LOCAL2, false},
	{"local3", LOG_LOCAL3, false},
	{"local4", LOG_LOCAL4, false},
	{"local5", LOG_LOCAL5, false},
	{"local6", LOG_LOCAL6, false},
	{"local7", LOG_LOCAL7, false},
#else
	{"none", 0, false},
#endif
	{NULL, 0}
};

static const struct config_enum_entry track_function_options[] = {
	{"none", TRACK_FUNC_OFF, false},
	{"pl", TRACK_FUNC_PL, false},
	{"all", TRACK_FUNC_ALL, false},
	{NULL, 0, false}
};

StaticAssertDecl(lengthof(track_function_options) == (TRACK_FUNC_ALL + 2),
				 "array length mismatch");

static const struct config_enum_entry stats_fetch_consistency[] = {
	{"none", PGSTAT_FETCH_CONSISTENCY_NONE, false},
	{"cache", PGSTAT_FETCH_CONSISTENCY_CACHE, false},
	{"snapshot", PGSTAT_FETCH_CONSISTENCY_SNAPSHOT, false},
	{NULL, 0, false}
};

StaticAssertDecl(lengthof(stats_fetch_consistency) == (PGSTAT_FETCH_CONSISTENCY_SNAPSHOT + 2),
				 "array length mismatch");

static const struct config_enum_entry xmlbinary_options[] = {
	{"base64", XMLBINARY_BASE64, false},
	{"hex", XMLBINARY_HEX, false},
	{NULL, 0, false}
};

StaticAssertDecl(lengthof(xmlbinary_options) == (XMLBINARY_HEX + 2),
				 "array length mismatch");

static const struct config_enum_entry xmloption_options[] = {
	{"content", XMLOPTION_CONTENT, false},
	{"document", XMLOPTION_DOCUMENT, false},
	{NULL, 0, false}
};

StaticAssertDecl(lengthof(xmloption_options) == (XMLOPTION_CONTENT + 2),
				 "array length mismatch");

/*
 * Although only "on", "off", and "safe_encoding" are documented, we
 * accept all the likely variants of "on" and "off".
 */
static const struct config_enum_entry backslash_quote_options[] = {
	{"safe_encoding", BACKSLASH_QUOTE_SAFE_ENCODING, false},
	{"on", BACKSLASH_QUOTE_ON, false},
	{"off", BACKSLASH_QUOTE_OFF, false},
	{"true", BACKSLASH_QUOTE_ON, true},
	{"false", BACKSLASH_QUOTE_OFF, true},
	{"yes", BACKSLASH_QUOTE_ON, true},
	{"no", BACKSLASH_QUOTE_OFF, true},
	{"1", BACKSLASH_QUOTE_ON, true},
	{"0", BACKSLASH_QUOTE_OFF, true},
	{NULL, 0, false}
};

/*
 * Although only "on", "off", and "auto" are documented, we accept
 * all the likely variants of "on" and "off".
 */
static const struct config_enum_entry compute_query_id_options[] = {
	{"auto", COMPUTE_QUERY_ID_AUTO, false},
	{"regress", COMPUTE_QUERY_ID_REGRESS, false},
	{"on", COMPUTE_QUERY_ID_ON, false},
	{"off", COMPUTE_QUERY_ID_OFF, false},
	{"true", COMPUTE_QUERY_ID_ON, true},
	{"false", COMPUTE_QUERY_ID_OFF, true},
	{"yes", COMPUTE_QUERY_ID_ON, true},
	{"no", COMPUTE_QUERY_ID_OFF, true},
	{"1", COMPUTE_QUERY_ID_ON, true},
	{"0", COMPUTE_QUERY_ID_OFF, true},
	{NULL, 0, false}
};

/*
 * Although only "on", "off", and "partition" are documented, we
 * accept all the likely variants of "on" and "off".
 */
static const struct config_enum_entry constraint_exclusion_options[] = {
	{"partition", CONSTRAINT_EXCLUSION_PARTITION, false},
	{"on", CONSTRAINT_EXCLUSION_ON, false},
	{"off", CONSTRAINT_EXCLUSION_OFF, false},
	{"true", CONSTRAINT_EXCLUSION_ON, true},
	{"false", CONSTRAINT_EXCLUSION_OFF, true},
	{"yes", CONSTRAINT_EXCLUSION_ON, true},
	{"no", CONSTRAINT_EXCLUSION_OFF, true},
	{"1", CONSTRAINT_EXCLUSION_ON, true},
	{"0", CONSTRAINT_EXCLUSION_OFF, true},
	{NULL, 0, false}
};

/*
 * Although only "on", "off", "remote_apply", "remote_write", and "local" are
 * documented, we accept all the likely variants of "on" and "off".
 */
static const struct config_enum_entry synchronous_commit_options[] = {
	{"local", SYNCHRONOUS_COMMIT_LOCAL_FLUSH, false},
	{"remote_write", SYNCHRONOUS_COMMIT_REMOTE_WRITE, false},
	{"remote_apply", SYNCHRONOUS_COMMIT_REMOTE_APPLY, false},
	{"on", SYNCHRONOUS_COMMIT_ON, false},
	{"off", SYNCHRONOUS_COMMIT_OFF, false},
	{"true", SYNCHRONOUS_COMMIT_ON, true},
	{"false", SYNCHRONOUS_COMMIT_OFF, true},
	{"yes", SYNCHRONOUS_COMMIT_ON, true},
	{"no", SYNCHRONOUS_COMMIT_OFF, true},
	{"1", SYNCHRONOUS_COMMIT_ON, true},
	{"0", SYNCHRONOUS_COMMIT_OFF, true},
	{NULL, 0, false}
};

/*
 * Although only "on", "off", "try" are documented, we accept all the likely
 * variants of "on" and "off".
 */
static const struct config_enum_entry huge_pages_options[] = {
	{"off", HUGE_PAGES_OFF, false},
	{"on", HUGE_PAGES_ON, false},
	{"try", HUGE_PAGES_TRY, false},
	{"true", HUGE_PAGES_ON, true},
	{"false", HUGE_PAGES_OFF, true},
	{"yes", HUGE_PAGES_ON, true},
	{"no", HUGE_PAGES_OFF, true},
	{"1", HUGE_PAGES_ON, true},
	{"0", HUGE_PAGES_OFF, true},
	{NULL, 0, false}
};

static const struct config_enum_entry huge_pages_status_options[] = {
	{"off", HUGE_PAGES_OFF, false},
	{"on", HUGE_PAGES_ON, false},
	{"unknown", HUGE_PAGES_UNKNOWN, false},
	{NULL, 0, false}
};

static const struct config_enum_entry recovery_prefetch_options[] = {
	{"off", RECOVERY_PREFETCH_OFF, false},
	{"on", RECOVERY_PREFETCH_ON, false},
	{"try", RECOVERY_PREFETCH_TRY, false},
	{"true", RECOVERY_PREFETCH_ON, true},
	{"false", RECOVERY_PREFETCH_OFF, true},
	{"yes", RECOVERY_PREFETCH_ON, true},
	{"no", RECOVERY_PREFETCH_OFF, true},
	{"1", RECOVERY_PREFETCH_ON, true},
	{"0", RECOVERY_PREFETCH_OFF, true},
	{NULL, 0, false}
};

static const struct config_enum_entry debug_parallel_query_options[] = {
	{"off", DEBUG_PARALLEL_OFF, false},
	{"on", DEBUG_PARALLEL_ON, false},
	{"regress", DEBUG_PARALLEL_REGRESS, false},
	{"true", DEBUG_PARALLEL_ON, true},
	{"false", DEBUG_PARALLEL_OFF, true},
	{"yes", DEBUG_PARALLEL_ON, true},
	{"no", DEBUG_PARALLEL_OFF, true},
	{"1", DEBUG_PARALLEL_ON, true},
	{"0", DEBUG_PARALLEL_OFF, true},
	{NULL, 0, false}
};

static const struct config_enum_entry plan_cache_mode_options[] = {
	{"auto", PLAN_CACHE_MODE_AUTO, false},
	{"force_generic_plan", PLAN_CACHE_MODE_FORCE_GENERIC_PLAN, false},
	{"force_custom_plan", PLAN_CACHE_MODE_FORCE_CUSTOM_PLAN, false},
	{NULL, 0, false}
};

static const struct config_enum_entry password_encryption_options[] = {
	{"md5", PASSWORD_TYPE_MD5, false},
	{"scram-sha-256", PASSWORD_TYPE_SCRAM_SHA_256, false},
	{NULL, 0, false}
};

static const struct config_enum_entry ssl_protocol_versions_info[] = {
	{"", PG_TLS_ANY, false},
	{"TLSv1", PG_TLS1_VERSION, false},
	{"TLSv1.1", PG_TLS1_1_VERSION, false},
	{"TLSv1.2", PG_TLS1_2_VERSION, false},
	{"TLSv1.3", PG_TLS1_3_VERSION, false},
	{NULL, 0, false}
};

static const struct config_enum_entry debug_logical_replication_streaming_options[] = {
	{"buffered", DEBUG_LOGICAL_REP_STREAMING_BUFFERED, false},
	{"immediate", DEBUG_LOGICAL_REP_STREAMING_IMMEDIATE, false},
	{NULL, 0, false}
};

StaticAssertDecl(lengthof(ssl_protocol_versions_info) == (PG_TLS1_3_VERSION + 2),
				 "array length mismatch");

static const struct config_enum_entry recovery_init_sync_method_options[] = {
	{"fsync", DATA_DIR_SYNC_METHOD_FSYNC, false},
#ifdef HAVE_SYNCFS
	{"syncfs", DATA_DIR_SYNC_METHOD_SYNCFS, false},
#endif
	{NULL, 0, false}
};

static const struct config_enum_entry shared_memory_options[] = {
#ifndef WIN32
	{"sysv", SHMEM_TYPE_SYSV, false},
#endif
#ifndef EXEC_BACKEND
	{"mmap", SHMEM_TYPE_MMAP, false},
#endif
#ifdef WIN32
	{"windows", SHMEM_TYPE_WINDOWS, false},
#endif
	{NULL, 0, false}
};

static const struct config_enum_entry default_toast_compression_options[] = {
	{"pglz", TOAST_PGLZ_COMPRESSION, false},
#ifdef  USE_LZ4
	{"lz4", TOAST_LZ4_COMPRESSION, false},
#endif
	{NULL, 0, false}
};

static const struct config_enum_entry wal_compression_options[] = {
	{"pglz", WAL_COMPRESSION_PGLZ, false},
#ifdef USE_LZ4
	{"lz4", WAL_COMPRESSION_LZ4, false},
#endif
#ifdef USE_ZSTD
	{"zstd", WAL_COMPRESSION_ZSTD, false},
#endif
	{"on", WAL_COMPRESSION_PGLZ, false},
	{"off", WAL_COMPRESSION_NONE, false},
	{"true", WAL_COMPRESSION_PGLZ, true},
	{"false", WAL_COMPRESSION_NONE, true},
	{"yes", WAL_COMPRESSION_PGLZ, true},
	{"no", WAL_COMPRESSION_NONE, true},
	{"1", WAL_COMPRESSION_PGLZ, true},
	{"0", WAL_COMPRESSION_NONE, true},
	{NULL, 0, false}
};

/*
 * Options for enum values stored in other modules
 */
extern const struct config_enum_entry wal_level_options[];
extern const struct config_enum_entry archive_mode_options[];
extern const struct config_enum_entry recovery_target_action_options[];
extern const struct config_enum_entry wal_sync_method_options[];
extern const struct config_enum_entry dynamic_shared_memory_options[];

/*
 * GUC option variables that are exported from this module
 */
bool		AllowAlterSystem = true;
bool		log_duration = false;
bool		Debug_print_plan = false;
bool		Debug_print_parse = false;
bool		Debug_print_rewritten = false;
bool		Debug_pretty_print = true;

#ifdef DEBUG_NODE_TESTS_ENABLED
bool		Debug_copy_parse_plan_trees;
bool		Debug_write_read_parse_plan_trees;
bool		Debug_raw_expression_coverage_test;
#endif

bool		log_parser_stats = false;
bool		log_planner_stats = false;
bool		log_executor_stats = false;
bool		log_statement_stats = false;	/* this is sort of all three above
											 * together */
bool		log_btree_build_stats = false;
char	   *event_source;

bool		row_security;
bool		check_function_bodies = true;

/*
 * This GUC exists solely for backward compatibility, check its definition for
 * details.
 */
static bool default_with_oids = false;

bool		current_role_is_superuser;

int			log_min_error_statement = ERROR;
int			log_min_messages = WARNING;
int			client_min_messages = NOTICE;
int			log_min_duration_sample = -1;
int			log_min_duration_statement = -1;
int			log_parameter_max_length = -1;
int			log_parameter_max_length_on_error = 0;
int			log_temp_files = -1;
double		log_statement_sample_rate = 1.0;
double		log_xact_sample_rate = 0;
char	   *backtrace_functions;

int			temp_file_limit = -1;

int			num_temp_buffers = 1024;

char	   *cluster_name = "";
char	   *ConfigFileName;
char	   *HbaFileName;
char	   *IdentFileName;
char	   *external_pid_file;

char	   *application_name;

int			tcp_keepalives_idle;
int			tcp_keepalives_interval;
int			tcp_keepalives_count;
int			tcp_user_timeout;

/*
 * SSL renegotiation was been removed in PostgreSQL 9.5, but we tolerate it
 * being set to zero (meaning never renegotiate) for backward compatibility.
 * This avoids breaking compatibility with clients that have never supported
 * renegotiation and therefore always try to zero it.
 */
static int	ssl_renegotiation_limit;

/*
 * This really belongs in pg_shmem.c, but is defined here so that it doesn't
 * need to be duplicated in all the different implementations of pg_shmem.c.
 */
int			huge_pages = HUGE_PAGES_TRY;
int			huge_page_size;
static int	huge_pages_status = HUGE_PAGES_UNKNOWN;

/*
 * These variables are all dummies that don't do anything, except in some
 * cases provide the value for SHOW to display.  The real state is elsewhere
 * and is kept in sync by assign_hooks.
 */
static char *syslog_ident_str;
static double phony_random_seed;
static char *client_encoding_string;
static char *datestyle_string;
static char *server_encoding_string;
static char *server_version_string;
static int	server_version_num;
static char *debug_io_direct_string;
static char *restrict_nonsystem_relation_kind_string;

#ifdef HAVE_SYSLOG
#define	DEFAULT_SYSLOG_FACILITY LOG_LOCAL0
#else
#define	DEFAULT_SYSLOG_FACILITY 0
#endif
static int	syslog_facility = DEFAULT_SYSLOG_FACILITY;

static char *timezone_string;
static char *log_timezone_string;
static char *timezone_abbreviations_string;
static char *data_directory;
static char *session_authorization_string;
static int	max_function_args;
static int	max_index_keys;
static int	max_identifier_length;
static int	block_size;
static int	segment_size;
static int	shared_memory_size_mb;
static int	shared_memory_size_in_huge_pages;
static int	wal_block_size;
static int	num_os_semaphores;
static bool data_checksums;
static bool integer_datetimes;

#ifdef USE_ASSERT_CHECKING
#define DEFAULT_ASSERT_ENABLED true
#else
#define DEFAULT_ASSERT_ENABLED false
#endif
static bool assert_enabled = DEFAULT_ASSERT_ENABLED;

static char *recovery_target_timeline_string;
static char *recovery_target_string;
static char *recovery_target_xid_string;
static char *recovery_target_name_string;
static char *recovery_target_lsn_string;

/* should be static, but commands/variable.c needs to get at this */
char	   *role_string;

/* should be static, but guc.c needs to get at this */
bool		in_hot_standby_guc;


/*
 * Displayable names for context types (enum GucContext)
 *
 * Note: these strings are deliberately not localized.
 */
const char *const GucContext_Names[] =
{
	[PGC_INTERNAL] = "internal",
	[PGC_POSTMASTER] = "postmaster",
	[PGC_SIGHUP] = "sighup",
	[PGC_SU_BACKEND] = "superuser-backend",
	[PGC_BACKEND] = "backend",
	[PGC_SUSET] = "superuser",
	[PGC_USERSET] = "user",
};

StaticAssertDecl(lengthof(GucContext_Names) == (PGC_USERSET + 1),
				 "array length mismatch");

/*
 * Displayable names for source types (enum GucSource)
 *
 * Note: these strings are deliberately not localized.
 */
const char *const GucSource_Names[] =
{
	[PGC_S_DEFAULT] = "default",
	[PGC_S_DYNAMIC_DEFAULT] = "default",
	[PGC_S_ENV_VAR] = "environment variable",
	[PGC_S_FILE] = "configuration file",
	[PGC_S_ARGV] = "command line",
	[PGC_S_GLOBAL] = "global",
	[PGC_S_DATABASE] = "database",
	[PGC_S_USER] = "user",
	[PGC_S_DATABASE_USER] = "database user",
	[PGC_S_CLIENT] = "client",
	[PGC_S_OVERRIDE] = "override",
	[PGC_S_INTERACTIVE] = "interactive",
	[PGC_S_TEST] = "test",
	[PGC_S_SESSION] = "session",
};

StaticAssertDecl(lengthof(GucSource_Names) == (PGC_S_SESSION + 1),
				 "array length mismatch");

/*
 * Displayable names for the groupings defined in enum config_group
 */
const char *const config_group_names[] =
{
	[UNGROUPED] = gettext_noop("Ungrouped"),
	[FILE_LOCATIONS] = gettext_noop("File Locations"),
	[CONN_AUTH_SETTINGS] = gettext_noop("Connections and Authentication / Connection Settings"),
	[CONN_AUTH_TCP] = gettext_noop("Connections and Authentication / TCP Settings"),
	[CONN_AUTH_AUTH] = gettext_noop("Connections and Authentication / Authentication"),
	[CONN_AUTH_SSL] = gettext_noop("Connections and Authentication / SSL"),
	[RESOURCES_MEM] = gettext_noop("Resource Usage / Memory"),
	[RESOURCES_DISK] = gettext_noop("Resource Usage / Disk"),
	[RESOURCES_KERNEL] = gettext_noop("Resource Usage / Kernel Resources"),
	[RESOURCES_VACUUM_DELAY] = gettext_noop("Resource Usage / Cost-Based Vacuum Delay"),
	[RESOURCES_BGWRITER] = gettext_noop("Resource Usage / Background Writer"),
	[RESOURCES_ASYNCHRONOUS] = gettext_noop("Resource Usage / Asynchronous Behavior"),
	[WAL_SETTINGS] = gettext_noop("Write-Ahead Log / Settings"),
	[WAL_CHECKPOINTS] = gettext_noop("Write-Ahead Log / Checkpoints"),
	[WAL_ARCHIVING] = gettext_noop("Write-Ahead Log / Archiving"),
	[WAL_RECOVERY] = gettext_noop("Write-Ahead Log / Recovery"),
	[WAL_ARCHIVE_RECOVERY] = gettext_noop("Write-Ahead Log / Archive Recovery"),
	[WAL_RECOVERY_TARGET] = gettext_noop("Write-Ahead Log / Recovery Target"),
	[WAL_SUMMARIZATION] = gettext_noop("Write-Ahead Log / Summarization"),
	[REPLICATION_SENDING] = gettext_noop("Replication / Sending Servers"),
	[REPLICATION_PRIMARY] = gettext_noop("Replication / Primary Server"),
	[REPLICATION_STANDBY] = gettext_noop("Replication / Standby Servers"),
	[REPLICATION_SUBSCRIBERS] = gettext_noop("Replication / Subscribers"),
	[QUERY_TUNING_METHOD] = gettext_noop("Query Tuning / Planner Method Configuration"),
	[QUERY_TUNING_COST] = gettext_noop("Query Tuning / Planner Cost Constants"),
	[QUERY_TUNING_GEQO] = gettext_noop("Query Tuning / Genetic Query Optimizer"),
	[QUERY_TUNING_OTHER] = gettext_noop("Query Tuning / Other Planner Options"),
	[LOGGING_WHERE] = gettext_noop("Reporting and Logging / Where to Log"),
	[LOGGING_WHEN] = gettext_noop("Reporting and Logging / When to Log"),
	[LOGGING_WHAT] = gettext_noop("Reporting and Logging / What to Log"),
	[PROCESS_TITLE] = gettext_noop("Reporting and Logging / Process Title"),
	[STATS_MONITORING] = gettext_noop("Statistics / Monitoring"),
	[STATS_CUMULATIVE] = gettext_noop("Statistics / Cumulative Query and Index Statistics"),
	[AUTOVACUUM] = gettext_noop("Autovacuum"),
	[CLIENT_CONN_STATEMENT] = gettext_noop("Client Connection Defaults / Statement Behavior"),
	[CLIENT_CONN_LOCALE] = gettext_noop("Client Connection Defaults / Locale and Formatting"),
	[CLIENT_CONN_PRELOAD] = gettext_noop("Client Connection Defaults / Shared Library Preloading"),
	[CLIENT_CONN_OTHER] = gettext_noop("Client Connection Defaults / Other Defaults"),
	[LOCK_MANAGEMENT] = gettext_noop("Lock Management"),
	[COMPAT_OPTIONS_PREVIOUS] = gettext_noop("Version and Platform Compatibility / Previous PostgreSQL Versions"),
	[COMPAT_OPTIONS_OTHER] = gettext_noop("Version and Platform Compatibility / Other Platforms and Clients"),
	[ERROR_HANDLING_OPTIONS] = gettext_noop("Error Handling"),
	[PRESET_OPTIONS] = gettext_noop("Preset Options"),
	[CUSTOM_OPTIONS] = gettext_noop("Customized Options"),
	[DEVELOPER_OPTIONS] = gettext_noop("Developer Options"),
};

StaticAssertDecl(lengthof(config_group_names) == (DEVELOPER_OPTIONS + 1),
				 "array length mismatch");

/*
 * Displayable names for GUC variable types (enum config_type)
 *
 * Note: these strings are deliberately not localized.
 */
const char *const config_type_names[] =
{
	[PGC_BOOL] = "bool",
	[PGC_INT] = "integer",
	[PGC_REAL] = "real",
	[PGC_STRING] = "string",
	[PGC_ENUM] = "enum",
};

StaticAssertDecl(lengthof(config_type_names) == (PGC_ENUM + 1),
				 "array length mismatch");


/*
 * Contents of GUC tables
 *
 * See src/backend/utils/misc/README for design notes.
 *
 * TO ADD AN OPTION:
 *
 * 1. Declare a global variable of type bool, int, double, or char*
 *	  and make use of it.
 *
 * 2. Decide at what times it's safe to set the option. See guc.h for
 *	  details.
 *
 * 3. Decide on a name, a default value, upper and lower bounds (if
 *	  applicable), etc.
 *
 * 4. Add a record below.
 *
 * 5. Add it to src/backend/utils/misc/postgresql.conf.sample, if
 *	  appropriate.
 *
 * 6. Don't forget to document the option (at least in config.sgml).
 *
 * 7. If it's a new GUC_LIST_QUOTE option, you must add it to
 *	  variable_is_guc_list_quote() in src/bin/pg_dump/dumputils.c.
 */

struct config_bool ConfigureNamesBool[] =
{
	{
		{"enable_seqscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of sequential-scan plans."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_seqscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_indexscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of index-scan plans."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_indexscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_indexonlyscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of index-only-scan plans."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_indexonlyscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_bitmapscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of bitmap-scan plans."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_bitmapscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_tidscan", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of TID scan plans."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_tidscan,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_sort", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of explicit sort steps."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_sort,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_incremental_sort", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of incremental sort steps."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_incremental_sort,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_hashagg", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of hashed aggregation plans."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_hashagg,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_material", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of materialization."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_material,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_memoize", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of memoization."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_memoize,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_nestloop", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of nested-loop join plans."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_nestloop,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_mergejoin", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of merge join plans."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_mergejoin,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_hashjoin", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of hash join plans."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_hashjoin,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_gathermerge", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of gather merge plans."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_gathermerge,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_partitionwise_join", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables partitionwise join."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_partitionwise_join,
		false,
		NULL, NULL, NULL
	},
	{
		{"enable_partitionwise_aggregate", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables partitionwise aggregation and grouping."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_partitionwise_aggregate,
		false,
		NULL, NULL, NULL
	},
	{
		{"enable_parallel_append", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of parallel append plans."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_parallel_append,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_parallel_hash", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of parallel hash plans."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_parallel_hash,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_partition_pruning", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables plan-time and execution-time partition pruning."),
			gettext_noop("Allows the query planner and executor to compare partition "
						 "bounds to conditions in the query to determine which "
						 "partitions must be scanned."),
			GUC_EXPLAIN
		},
		&enable_partition_pruning,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_presorted_aggregate", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's ability to produce plans that "
						 "provide presorted input for ORDER BY / DISTINCT aggregate "
						 "functions."),
			gettext_noop("Allows the query planner to build plans that provide "
						 "presorted input for aggregate functions with an ORDER BY / "
						 "DISTINCT clause.  When disabled, implicit sorts are always "
						 "performed during execution."),
			GUC_EXPLAIN
		},
		&enable_presorted_aggregate,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_async_append", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables the planner's use of async append plans."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_async_append,
		true,
		NULL, NULL, NULL
	},
	{
		{"enable_group_by_reordering", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enables reordering of GROUP BY keys."),
			NULL,
			GUC_EXPLAIN
		},
		&enable_group_by_reordering,
		true,
		NULL, NULL, NULL
	},
	{
		{"geqo", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("Enables genetic query optimization."),
			gettext_noop("This algorithm attempts to do planning without "
						 "exhaustive searching."),
			GUC_EXPLAIN
		},
		&enable_geqo,
		true,
		NULL, NULL, NULL
	},
	{
		/*
		 * Not for general use --- used by SET SESSION AUTHORIZATION and SET
		 * ROLE
		 */
		{"is_superuser", PGC_INTERNAL, UNGROUPED,
			gettext_noop("Shows whether the current user is a superuser."),
			NULL,
			GUC_REPORT | GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | GUC_ALLOW_IN_PARALLEL
		},
		&current_role_is_superuser,
		false,
		NULL, NULL, NULL
	},
	{
		/*
		 * This setting itself cannot be set by ALTER SYSTEM to avoid an
		 * operator turning this setting off by using ALTER SYSTEM, without a
		 * way to turn it back on.
		 */
		{"allow_alter_system", PGC_SIGHUP, COMPAT_OPTIONS_OTHER,
			gettext_noop("Allows running the ALTER SYSTEM command."),
			gettext_noop("Can be set to off for environments where global configuration "
						 "changes should be made using a different method."),
			GUC_DISALLOW_IN_AUTO_FILE
		},
		&AllowAlterSystem,
		true,
		NULL, NULL, NULL
	},
	{
		{"bonjour", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Enables advertising the server via Bonjour."),
			NULL
		},
		&enable_bonjour,
		false,
		check_bonjour, NULL, NULL
	},
	{
		{"track_commit_timestamp", PGC_POSTMASTER, REPLICATION_SENDING,
			gettext_noop("Collects transaction commit time."),
			NULL
		},
		&track_commit_timestamp,
		false,
		NULL, NULL, NULL
	},
	{
		{"ssl", PGC_SIGHUP, CONN_AUTH_SSL,
			gettext_noop("Enables SSL connections."),
			NULL
		},
		&EnableSSL,
		false,
		check_ssl, NULL, NULL
	},
	{
		{"ssl_passphrase_command_supports_reload", PGC_SIGHUP, CONN_AUTH_SSL,
			gettext_noop("Controls whether \"ssl_passphrase_command\" is called during server reload."),
			NULL
		},
		&ssl_passphrase_command_supports_reload,
		false,
		NULL, NULL, NULL
	},
	{
		{"ssl_prefer_server_ciphers", PGC_SIGHUP, CONN_AUTH_SSL,
			gettext_noop("Give priority to server ciphersuite order."),
			NULL
		},
		&SSLPreferServerCiphers,
		true,
		NULL, NULL, NULL
	},
	{
		{"fsync", PGC_SIGHUP, WAL_SETTINGS,
			gettext_noop("Forces synchronization of updates to disk."),
			gettext_noop("The server will use the fsync() system call in several places to make "
						 "sure that updates are physically written to disk. This ensures "
						 "that a database cluster will recover to a consistent state after "
						 "an operating system or hardware crash.")
		},
		&enableFsync,
		true,
		NULL, NULL, NULL
	},
	{
		{"ignore_checksum_failure", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Continues processing after a checksum failure."),
			gettext_noop("Detection of a checksum failure normally causes PostgreSQL to "
						 "report an error, aborting the current transaction. Setting "
						 "ignore_checksum_failure to true causes the system to ignore the failure "
						 "(but still report a warning), and continue processing. This "
						 "behavior could cause crashes or other serious problems. Only "
						 "has an effect if checksums are enabled."),
			GUC_NOT_IN_SAMPLE
		},
		&ignore_checksum_failure,
		false,
		NULL, NULL, NULL
	},
	{
		{"zero_damaged_pages", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Continues processing past damaged page headers."),
			gettext_noop("Detection of a damaged page header normally causes PostgreSQL to "
						 "report an error, aborting the current transaction. Setting "
						 "\"zero_damaged_pages\" to true causes the system to instead report a "
						 "warning, zero out the damaged page, and continue processing. This "
						 "behavior will destroy data, namely all the rows on the damaged page."),
			GUC_NOT_IN_SAMPLE
		},
		&zero_damaged_pages,
		false,
		NULL, NULL, NULL
	},
	{
		{"ignore_invalid_pages", PGC_POSTMASTER, DEVELOPER_OPTIONS,
			gettext_noop("Continues recovery after an invalid pages failure."),
			gettext_noop("Detection of WAL records having references to "
						 "invalid pages during recovery causes PostgreSQL to "
						 "raise a PANIC-level error, aborting the recovery. "
						 "Setting \"ignore_invalid_pages\" to true causes "
						 "the system to ignore invalid page references "
						 "in WAL records (but still report a warning), "
						 "and continue recovery. This behavior may cause "
						 "crashes, data loss, propagate or hide corruption, "
						 "or other serious problems. Only has an effect "
						 "during recovery or in standby mode."),
			GUC_NOT_IN_SAMPLE
		},
		&ignore_invalid_pages,
		false,
		NULL, NULL, NULL
	},
	{
		{"full_page_writes", PGC_SIGHUP, WAL_SETTINGS,
			gettext_noop("Writes full pages to WAL when first modified after a checkpoint."),
			gettext_noop("A page write in process during an operating system crash might be "
						 "only partially written to disk.  During recovery, the row changes "
						 "stored in WAL are not enough to recover.  This option writes "
						 "pages when first modified after a checkpoint to WAL so full recovery "
						 "is possible.")
		},
		&fullPageWrites,
		true,
		NULL, NULL, NULL
	},

	{
		{"wal_log_hints", PGC_POSTMASTER, WAL_SETTINGS,
			gettext_noop("Writes full pages to WAL when first modified after a checkpoint, even for a non-critical modification."),
			NULL
		},
		&wal_log_hints,
		false,
		NULL, NULL, NULL
	},

	{
		{"wal_init_zero", PGC_SUSET, WAL_SETTINGS,
			gettext_noop("Writes zeroes to new WAL files before first use."),
			NULL
		},
		&wal_init_zero,
		true,
		NULL, NULL, NULL
	},

	{
		{"wal_recycle", PGC_SUSET, WAL_SETTINGS,
			gettext_noop("Recycles WAL files by renaming them."),
			NULL
		},
		&wal_recycle,
		true,
		NULL, NULL, NULL
	},

	{
		{"log_checkpoints", PGC_SIGHUP, LOGGING_WHAT,
			gettext_noop("Logs each checkpoint."),
			NULL
		},
		&log_checkpoints,
		true,
		NULL, NULL, NULL
	},
	{
		{"log_connections", PGC_SU_BACKEND, LOGGING_WHAT,
			gettext_noop("Logs each successful connection."),
			NULL
		},
		&Log_connections,
		false,
		NULL, NULL, NULL
	},
	{
		{"trace_connection_negotiation", PGC_POSTMASTER, DEVELOPER_OPTIONS,
			gettext_noop("Logs details of pre-authentication connection handshake."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_connection_negotiation,
		false,
		NULL, NULL, NULL
	},
	{
		{"log_disconnections", PGC_SU_BACKEND, LOGGING_WHAT,
			gettext_noop("Logs end of a session, including duration."),
			NULL
		},
		&Log_disconnections,
		false,
		NULL, NULL, NULL
	},
	{
		{"log_replication_commands", PGC_SUSET, LOGGING_WHAT,
			gettext_noop("Logs each replication command."),
			NULL
		},
		&log_replication_commands,
		false,
		NULL, NULL, NULL
	},
	{
		{"debug_assertions", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows whether the running server has assertion checks enabled."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&assert_enabled,
		DEFAULT_ASSERT_ENABLED,
		NULL, NULL, NULL
	},

	{
		{"exit_on_error", PGC_USERSET, ERROR_HANDLING_OPTIONS,
			gettext_noop("Terminate session on any error."),
			NULL
		},
		&ExitOnAnyError,
		false,
		NULL, NULL, NULL
	},
	{
		{"restart_after_crash", PGC_SIGHUP, ERROR_HANDLING_OPTIONS,
			gettext_noop("Reinitialize server after backend crash."),
			NULL
		},
		&restart_after_crash,
		true,
		NULL, NULL, NULL
	},
	{
		{"remove_temp_files_after_crash", PGC_SIGHUP, DEVELOPER_OPTIONS,
			gettext_noop("Remove temporary files after backend crash."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&remove_temp_files_after_crash,
		true,
		NULL, NULL, NULL
	},
	{
		{"send_abort_for_crash", PGC_SIGHUP, DEVELOPER_OPTIONS,
			gettext_noop("Send SIGABRT not SIGQUIT to child processes after backend crash."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&send_abort_for_crash,
		false,
		NULL, NULL, NULL
	},
	{
		{"send_abort_for_kill", PGC_SIGHUP, DEVELOPER_OPTIONS,
			gettext_noop("Send SIGABRT not SIGKILL to stuck child processes."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&send_abort_for_kill,
		false,
		NULL, NULL, NULL
	},

	{
		{"log_duration", PGC_SUSET, LOGGING_WHAT,
			gettext_noop("Logs the duration of each completed SQL statement."),
			NULL
		},
		&log_duration,
		false,
		NULL, NULL, NULL
	},
#ifdef DEBUG_NODE_TESTS_ENABLED
	{
		{"debug_copy_parse_plan_trees", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Set this to force all parse and plan trees to be passed through "
						 "copyObject(), to facilitate catching errors and omissions in "
						 "copyObject()."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Debug_copy_parse_plan_trees,
/* support for legacy compile-time setting */
#ifdef COPY_PARSE_PLAN_TREES
		true,
#else
		false,
#endif
		NULL, NULL, NULL
	},
	{
		{"debug_write_read_parse_plan_trees", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Set this to force all parse and plan trees to be passed through "
						 "outfuncs.c/readfuncs.c, to facilitate catching errors and omissions in "
						 "those modules."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Debug_write_read_parse_plan_trees,
/* support for legacy compile-time setting */
#ifdef WRITE_READ_PARSE_PLAN_TREES
		true,
#else
		false,
#endif
		NULL, NULL, NULL
	},
	{
		{"debug_raw_expression_coverage_test", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Set this to force all raw parse trees for DML statements to be scanned "
						 "by raw_expression_tree_walker(), to facilitate catching errors and "
						 "omissions in that function."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Debug_raw_expression_coverage_test,
/* support for legacy compile-time setting */
#ifdef RAW_EXPRESSION_COVERAGE_TEST
		true,
#else
		false,
#endif
		NULL, NULL, NULL
	},
#endif							/* DEBUG_NODE_TESTS_ENABLED */
	{
		{"debug_print_parse", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Logs each query's parse tree."),
			NULL
		},
		&Debug_print_parse,
		false,
		NULL, NULL, NULL
	},
	{
		{"debug_print_rewritten", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Logs each query's rewritten parse tree."),
			NULL
		},
		&Debug_print_rewritten,
		false,
		NULL, NULL, NULL
	},
	{
		{"debug_print_plan", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Logs each query's execution plan."),
			NULL
		},
		&Debug_print_plan,
		false,
		NULL, NULL, NULL
	},
	{
		{"debug_pretty_print", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Indents parse and plan tree displays."),
			NULL
		},
		&Debug_pretty_print,
		true,
		NULL, NULL, NULL
	},
	{
		{"log_parser_stats", PGC_SUSET, STATS_MONITORING,
			gettext_noop("Writes parser performance statistics to the server log."),
			NULL
		},
		&log_parser_stats,
		false,
		check_stage_log_stats, NULL, NULL
	},
	{
		{"log_planner_stats", PGC_SUSET, STATS_MONITORING,
			gettext_noop("Writes planner performance statistics to the server log."),
			NULL
		},
		&log_planner_stats,
		false,
		check_stage_log_stats, NULL, NULL
	},
	{
		{"log_executor_stats", PGC_SUSET, STATS_MONITORING,
			gettext_noop("Writes executor performance statistics to the server log."),
			NULL
		},
		&log_executor_stats,
		false,
		check_stage_log_stats, NULL, NULL
	},
	{
		{"log_statement_stats", PGC_SUSET, STATS_MONITORING,
			gettext_noop("Writes cumulative performance statistics to the server log."),
			NULL
		},
		&log_statement_stats,
		false,
		check_log_stats, NULL, NULL
	},
#ifdef BTREE_BUILD_STATS
	{
		{"log_btree_build_stats", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Logs system resource usage statistics (memory and CPU) on various B-tree operations."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&log_btree_build_stats,
		false,
		NULL, NULL, NULL
	},
#endif

	{
		{"track_activities", PGC_SUSET, STATS_CUMULATIVE,
			gettext_noop("Collects information about executing commands."),
			gettext_noop("Enables the collection of information on the currently "
						 "executing command of each session, along with "
						 "the time at which that command began execution.")
		},
		&pgstat_track_activities,
		true,
		NULL, NULL, NULL
	},
	{
		{"track_counts", PGC_SUSET, STATS_CUMULATIVE,
			gettext_noop("Collects statistics on database activity."),
			NULL
		},
		&pgstat_track_counts,
		true,
		NULL, NULL, NULL
	},
	{
		{"track_io_timing", PGC_SUSET, STATS_CUMULATIVE,
			gettext_noop("Collects timing statistics for database I/O activity."),
			NULL
		},
		&track_io_timing,
		false,
		NULL, NULL, NULL
	},
	{
		{"track_wal_io_timing", PGC_SUSET, STATS_CUMULATIVE,
			gettext_noop("Collects timing statistics for WAL I/O activity."),
			NULL
		},
		&track_wal_io_timing,
		false,
		NULL, NULL, NULL
	},

	{
		{"update_process_title", PGC_SUSET, PROCESS_TITLE,
			gettext_noop("Updates the process title to show the active SQL command."),
			gettext_noop("Enables updating of the process title every time a new SQL command is received by the server.")
		},
		&update_process_title,
		DEFAULT_UPDATE_PROCESS_TITLE,
		NULL, NULL, NULL
	},

	{
		{"autovacuum", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Starts the autovacuum subprocess."),
			NULL
		},
		&autovacuum_start_daemon,
		true,
		NULL, NULL, NULL
	},

	{
		{"trace_notify", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Generates debugging output for LISTEN and NOTIFY."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_notify,
		false,
		NULL, NULL, NULL
	},

#ifdef LOCK_DEBUG
	{
		{"trace_locks", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Emits information about lock usage."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_locks,
		false,
		NULL, NULL, NULL
	},
	{
		{"trace_userlocks", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Emits information about user lock usage."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_userlocks,
		false,
		NULL, NULL, NULL
	},
	{
		{"trace_lwlocks", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Emits information about lightweight lock usage."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_lwlocks,
		false,
		NULL, NULL, NULL
	},
	{
		{"debug_deadlocks", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Dumps information about all current locks when a deadlock timeout occurs."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Debug_deadlocks,
		false,
		NULL, NULL, NULL
	},
#endif

	{
		{"log_lock_waits", PGC_SUSET, LOGGING_WHAT,
			gettext_noop("Logs long lock waits."),
			NULL
		},
		&log_lock_waits,
		false,
		NULL, NULL, NULL
	},
	{
		{"log_recovery_conflict_waits", PGC_SIGHUP, LOGGING_WHAT,
			gettext_noop("Logs standby recovery conflict waits."),
			NULL
		},
		&log_recovery_conflict_waits,
		false,
		NULL, NULL, NULL
	},
	{
		{"log_hostname", PGC_SIGHUP, LOGGING_WHAT,
			gettext_noop("Logs the host name in the connection logs."),
			gettext_noop("By default, connection logs only show the IP address "
						 "of the connecting host. If you want them to show the host name you "
						 "can turn this on, but depending on your host name resolution "
						 "setup it might impose a non-negligible performance penalty.")
		},
		&log_hostname,
		false,
		NULL, NULL, NULL
	},
	{
		{"transform_null_equals", PGC_USERSET, COMPAT_OPTIONS_OTHER,
			gettext_noop("Treats \"expr=NULL\" as \"expr IS NULL\"."),
			gettext_noop("When turned on, expressions of the form expr = NULL "
						 "(or NULL = expr) are treated as expr IS NULL, that is, they "
						 "return true if expr evaluates to the null value, and false "
						 "otherwise. The correct behavior of expr = NULL is to always "
						 "return null (unknown).")
		},
		&Transform_null_equals,
		false,
		NULL, NULL, NULL
	},
	{
		{"default_transaction_read_only", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the default read-only status of new transactions."),
			NULL,
			GUC_REPORT
		},
		&DefaultXactReadOnly,
		false,
		NULL, NULL, NULL
	},
	{
		{"transaction_read_only", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the current transaction's read-only status."),
			NULL,
			GUC_NO_RESET | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&XactReadOnly,
		false,
		check_transaction_read_only, NULL, NULL
	},
	{
		{"default_transaction_deferrable", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the default deferrable status of new transactions."),
			NULL
		},
		&DefaultXactDeferrable,
		false,
		NULL, NULL, NULL
	},
	{
		{"transaction_deferrable", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Whether to defer a read-only serializable transaction until it can be executed with no possible serialization failures."),
			NULL,
			GUC_NO_RESET | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&XactDeferrable,
		false,
		check_transaction_deferrable, NULL, NULL
	},
	{
		{"row_security", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Enable row security."),
			gettext_noop("When enabled, row security will be applied to all users.")
		},
		&row_security,
		true,
		NULL, NULL, NULL
	},
	{
		{"check_function_bodies", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Check routine bodies during CREATE FUNCTION and CREATE PROCEDURE."),
			NULL
		},
		&check_function_bodies,
		true,
		NULL, NULL, NULL
	},
	{
		{"array_nulls", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Enable input of NULL elements in arrays."),
			gettext_noop("When turned on, unquoted NULL in an array input "
						 "value means a null value; "
						 "otherwise it is taken literally.")
		},
		&Array_nulls,
		true,
		NULL, NULL, NULL
	},

	/*
	 * WITH OIDS support, and consequently default_with_oids, was removed in
	 * PostgreSQL 12, but we tolerate the parameter being set to false to
	 * avoid unnecessarily breaking older dump files.
	 */
	{
		{"default_with_oids", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("WITH OIDS is no longer supported; this can only be false."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE
		},
		&default_with_oids,
		false,
		check_default_with_oids, NULL, NULL
	},
	{
		{"logging_collector", PGC_POSTMASTER, LOGGING_WHERE,
			gettext_noop("Start a subprocess to capture stderr output and/or csvlogs into log files."),
			NULL
		},
		&Logging_collector,
		false,
		NULL, NULL, NULL
	},
	{
		{"log_truncate_on_rotation", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Truncate existing log files of same name during log rotation."),
			NULL
		},
		&Log_truncate_on_rotation,
		false,
		NULL, NULL, NULL
	},

	{
		{"trace_sort", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Emit information about resource usage in sorting."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&trace_sort,
		false,
		NULL, NULL, NULL
	},

#ifdef TRACE_SYNCSCAN
	/* this is undocumented because not exposed in a standard build */
	{
		{"trace_syncscan", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Generate debugging output for synchronized scanning."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&trace_syncscan,
		false,
		NULL, NULL, NULL
	},
#endif

#ifdef DEBUG_BOUNDED_SORT
	/* this is undocumented because not exposed in a standard build */
	{
		{
			"optimize_bounded_sort", PGC_USERSET, QUERY_TUNING_METHOD,
			gettext_noop("Enable bounded sorting using heap sort."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_EXPLAIN
		},
		&optimize_bounded_sort,
		true,
		NULL, NULL, NULL
	},
#endif

#ifdef WAL_DEBUG
	{
		{"wal_debug", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Emit WAL-related debugging output."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&XLOG_DEBUG,
		false,
		NULL, NULL, NULL
	},
#endif

	{
		{"integer_datetimes", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows whether datetimes are integer based."),
			NULL,
			GUC_REPORT | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&integer_datetimes,
		true,
		NULL, NULL, NULL
	},

	{
		{"krb_caseins_users", PGC_SIGHUP, CONN_AUTH_AUTH,
			gettext_noop("Sets whether Kerberos and GSSAPI user names should be treated as case-insensitive."),
			NULL
		},
		&pg_krb_caseins_users,
		false,
		NULL, NULL, NULL
	},

	{
		{"gss_accept_delegation", PGC_SIGHUP, CONN_AUTH_AUTH,
			gettext_noop("Sets whether GSSAPI delegation should be accepted from the client."),
			NULL
		},
		&pg_gss_accept_delegation,
		false,
		NULL, NULL, NULL
	},

	{
		{"escape_string_warning", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Warn about backslash escapes in ordinary string literals."),
			NULL
		},
		&escape_string_warning,
		true,
		NULL, NULL, NULL
	},

	{
		{"standard_conforming_strings", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Causes '...' strings to treat backslashes literally."),
			NULL,
			GUC_REPORT
		},
		&standard_conforming_strings,
		true,
		NULL, NULL, NULL
	},

	{
		{"synchronize_seqscans", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Enable synchronized sequential scans."),
			NULL
		},
		&synchronize_seqscans,
		true,
		NULL, NULL, NULL
	},

	{
		{"recovery_target_inclusive", PGC_POSTMASTER, WAL_RECOVERY_TARGET,
			gettext_noop("Sets whether to include or exclude transaction with recovery target."),
			NULL
		},
		&recoveryTargetInclusive,
		true,
		NULL, NULL, NULL
	},

	{
		{"summarize_wal", PGC_SIGHUP, WAL_SUMMARIZATION,
			gettext_noop("Starts the WAL summarizer process to enable incremental backup."),
			NULL
		},
		&summarize_wal,
		false,
		NULL, NULL, NULL
	},

	{
		{"hot_standby", PGC_POSTMASTER, REPLICATION_STANDBY,
			gettext_noop("Allows connections and queries during recovery."),
			NULL
		},
		&EnableHotStandby,
		true,
		NULL, NULL, NULL
	},

	{
		{"hot_standby_feedback", PGC_SIGHUP, REPLICATION_STANDBY,
			gettext_noop("Allows feedback from a hot standby to the primary that will avoid query conflicts."),
			NULL
		},
		&hot_standby_feedback,
		false,
		NULL, NULL, NULL
	},

	{
		{"in_hot_standby", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows whether hot standby is currently active."),
			NULL,
			GUC_REPORT | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&in_hot_standby_guc,
		false,
		NULL, NULL, show_in_hot_standby
	},

	{
		{"allow_system_table_mods", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Allows modifications of the structure of system tables."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&allowSystemTableMods,
		false,
		NULL, NULL, NULL
	},

	{
		{"ignore_system_indexes", PGC_BACKEND, DEVELOPER_OPTIONS,
			gettext_noop("Disables reading from system indexes."),
			gettext_noop("It does not prevent updating the indexes, so it is safe "
						 "to use.  The worst consequence is slowness."),
			GUC_NOT_IN_SAMPLE
		},
		&IgnoreSystemIndexes,
		false,
		NULL, NULL, NULL
	},

	{
		{"allow_in_place_tablespaces", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Allows tablespaces directly inside pg_tblspc, for testing."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&allow_in_place_tablespaces,
		false,
		NULL, NULL, NULL
	},

	{
		{"lo_compat_privileges", PGC_SUSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Enables backward compatibility mode for privilege checks on large objects."),
			gettext_noop("Skips privilege checks when reading or modifying large objects, "
						 "for compatibility with PostgreSQL releases prior to 9.0.")
		},
		&lo_compat_privileges,
		false,
		NULL, NULL, NULL
	},

	{
		{"quote_all_identifiers", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("When generating SQL fragments, quote all identifiers."),
			NULL,
		},
		&quote_all_identifiers,
		false,
		NULL, NULL, NULL
	},

	{
		{"data_checksums", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows whether data checksums are turned on for this cluster."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | GUC_RUNTIME_COMPUTED
		},
		&data_checksums,
		false,
		NULL, NULL, NULL
	},

	{
		{"syslog_sequence_numbers", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Add sequence number to syslog messages to avoid duplicate suppression."),
			NULL
		},
		&syslog_sequence_numbers,
		true,
		NULL, NULL, NULL
	},

	{
		{"syslog_split_messages", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Split messages sent to syslog by lines and to fit into 1024 bytes."),
			NULL
		},
		&syslog_split_messages,
		true,
		NULL, NULL, NULL
	},

	{
		{"parallel_leader_participation", PGC_USERSET, RESOURCES_ASYNCHRONOUS,
			gettext_noop("Controls whether Gather and Gather Merge also run subplans."),
			gettext_noop("Should gather nodes also run subplans or just gather tuples?"),
			GUC_EXPLAIN
		},
		&parallel_leader_participation,
		true,
		NULL, NULL, NULL
	},

	{
		{"jit", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Allow JIT compilation."),
			NULL,
			GUC_EXPLAIN
		},
		&jit_enabled,
		true,
		NULL, NULL, NULL
	},

	{
		{"jit_debugging_support", PGC_SU_BACKEND, DEVELOPER_OPTIONS,
			gettext_noop("Register JIT-compiled functions with debugger."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&jit_debugging_support,
		false,

		/*
		 * This is not guaranteed to be available, but given it's a developer
		 * oriented option, it doesn't seem worth adding code checking
		 * availability.
		 */
		NULL, NULL, NULL
	},

	{
		{"jit_dump_bitcode", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Write out LLVM bitcode to facilitate JIT debugging."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&jit_dump_bitcode,
		false,
		NULL, NULL, NULL
	},

	{
		{"jit_expressions", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Allow JIT compilation of expressions."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&jit_expressions,
		true,
		NULL, NULL, NULL
	},

	{
		{"jit_profiling_support", PGC_SU_BACKEND, DEVELOPER_OPTIONS,
			gettext_noop("Register JIT-compiled functions with perf profiler."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&jit_profiling_support,
		false,

		/*
		 * This is not guaranteed to be available, but given it's a developer
		 * oriented option, it doesn't seem worth adding code checking
		 * availability.
		 */
		NULL, NULL, NULL
	},

	{
		{"jit_tuple_deforming", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Allow JIT compilation of tuple deforming."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&jit_tuple_deforming,
		true,
		NULL, NULL, NULL
	},

	{
		{"data_sync_retry", PGC_POSTMASTER, ERROR_HANDLING_OPTIONS,
			gettext_noop("Whether to continue running after a failure to sync data files."),
		},
		&data_sync_retry,
		false,
		NULL, NULL, NULL
	},

	{
		{"wal_receiver_create_temp_slot", PGC_SIGHUP, REPLICATION_STANDBY,
			gettext_noop("Sets whether a WAL receiver should create a temporary replication slot if no permanent slot is configured."),
		},
		&wal_receiver_create_temp_slot,
		false,
		NULL, NULL, NULL
	},

	{
		{"event_triggers", PGC_SUSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Enables event triggers."),
			gettext_noop("When enabled, event triggers will fire for all applicable statements."),
		},
		&event_triggers,
		true,
		NULL, NULL, NULL
	},

	{
		{"sync_replication_slots", PGC_SIGHUP, REPLICATION_STANDBY,
			gettext_noop("Enables a physical standby to synchronize logical failover replication slots from the primary server."),
		},
		&sync_replication_slots,
		false,
		NULL, NULL, NULL
	},

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, false, NULL, NULL, NULL
	}
};


struct config_int ConfigureNamesInt[] =
{
	{
		{"archive_timeout", PGC_SIGHUP, WAL_ARCHIVING,
			gettext_noop("Sets the amount of time to wait before forcing a "
						 "switch to the next WAL file."),
			NULL,
			GUC_UNIT_S
		},
		&XLogArchiveTimeout,
		0, 0, INT_MAX / 2,
		NULL, NULL, NULL
	},
	{
		{"post_auth_delay", PGC_BACKEND, DEVELOPER_OPTIONS,
			gettext_noop("Sets the amount of time to wait after "
						 "authentication on connection startup."),
			gettext_noop("This allows attaching a debugger to the process."),
			GUC_NOT_IN_SAMPLE | GUC_UNIT_S
		},
		&PostAuthDelay,
		0, 0, INT_MAX / 1000000,
		NULL, NULL, NULL
	},
	{
		{"default_statistics_target", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Sets the default statistics target."),
			gettext_noop("This applies to table columns that have not had a "
						 "column-specific target set via ALTER TABLE SET STATISTICS.")
		},
		&default_statistics_target,
		100, 1, MAX_STATISTICS_TARGET,
		NULL, NULL, NULL
	},
	{
		{"from_collapse_limit", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Sets the FROM-list size beyond which subqueries "
						 "are not collapsed."),
			gettext_noop("The planner will merge subqueries into upper "
						 "queries if the resulting FROM list would have no more than "
						 "this many items."),
			GUC_EXPLAIN
		},
		&from_collapse_limit,
		8, 1, INT_MAX,
		NULL, NULL, NULL
	},
	{
		{"join_collapse_limit", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Sets the FROM-list size beyond which JOIN "
						 "constructs are not flattened."),
			gettext_noop("The planner will flatten explicit JOIN "
						 "constructs into lists of FROM items whenever a "
						 "list of no more than this many items would result."),
			GUC_EXPLAIN
		},
		&join_collapse_limit,
		8, 1, INT_MAX,
		NULL, NULL, NULL
	},
	{
		{"geqo_threshold", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("Sets the threshold of FROM items beyond which GEQO is used."),
			NULL,
			GUC_EXPLAIN
		},
		&geqo_threshold,
		12, 2, INT_MAX,
		NULL, NULL, NULL
	},
	{
		{"geqo_effort", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("GEQO: effort is used to set the default for other GEQO parameters."),
			NULL,
			GUC_EXPLAIN
		},
		&Geqo_effort,
		DEFAULT_GEQO_EFFORT, MIN_GEQO_EFFORT, MAX_GEQO_EFFORT,
		NULL, NULL, NULL
	},
	{
		{"geqo_pool_size", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("GEQO: number of individuals in the population."),
			gettext_noop("Zero selects a suitable default value."),
			GUC_EXPLAIN
		},
		&Geqo_pool_size,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},
	{
		{"geqo_generations", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("GEQO: number of iterations of the algorithm."),
			gettext_noop("Zero selects a suitable default value."),
			GUC_EXPLAIN
		},
		&Geqo_generations,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		/* This is PGC_SUSET to prevent hiding from log_lock_waits. */
		{"deadlock_timeout", PGC_SUSET, LOCK_MANAGEMENT,
			gettext_noop("Sets the time to wait on a lock before checking for deadlock."),
			NULL,
			GUC_UNIT_MS
		},
		&DeadlockTimeout,
		1000, 1, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"max_standby_archive_delay", PGC_SIGHUP, REPLICATION_STANDBY,
			gettext_noop("Sets the maximum delay before canceling queries when a hot standby server is processing archived WAL data."),
			NULL,
			GUC_UNIT_MS
		},
		&max_standby_archive_delay,
		30 * 1000, -1, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"max_standby_streaming_delay", PGC_SIGHUP, REPLICATION_STANDBY,
			gettext_noop("Sets the maximum delay before canceling queries when a hot standby server is processing streamed WAL data."),
			NULL,
			GUC_UNIT_MS
		},
		&max_standby_streaming_delay,
		30 * 1000, -1, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"recovery_min_apply_delay", PGC_SIGHUP, REPLICATION_STANDBY,
			gettext_noop("Sets the minimum delay for applying changes during recovery."),
			NULL,
			GUC_UNIT_MS
		},
		&recovery_min_apply_delay,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"wal_receiver_status_interval", PGC_SIGHUP, REPLICATION_STANDBY,
			gettext_noop("Sets the maximum interval between WAL receiver status reports to the sending server."),
			NULL,
			GUC_UNIT_S
		},
		&wal_receiver_status_interval,
		10, 0, INT_MAX / 1000,
		NULL, NULL, NULL
	},

	{
		{"wal_receiver_timeout", PGC_SIGHUP, REPLICATION_STANDBY,
			gettext_noop("Sets the maximum wait time to receive data from the sending server."),
			NULL,
			GUC_UNIT_MS
		},
		&wal_receiver_timeout,
		60 * 1000, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"max_connections", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the maximum number of concurrent connections."),
			NULL
		},
		&MaxConnections,
		100, 1, MAX_BACKENDS,
		NULL, NULL, NULL
	},

	{
		/* see max_connections */
		{"superuser_reserved_connections", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the number of connection slots reserved for superusers."),
			NULL
		},
		&SuperuserReservedConnections,
		3, 0, MAX_BACKENDS,
		NULL, NULL, NULL
	},

	{
		{"reserved_connections", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the number of connection slots reserved for roles "
						 "with privileges of pg_use_reserved_connections."),
			NULL
		},
		&ReservedConnections,
		0, 0, MAX_BACKENDS,
		NULL, NULL, NULL
	},

	{
		{"min_dynamic_shared_memory", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Amount of dynamic shared memory reserved at startup."),
			NULL,
			GUC_UNIT_MB
		},
		&min_dynamic_shared_memory,
		0, 0, (int) Min((size_t) INT_MAX, SIZE_MAX / (1024 * 1024)),
		NULL, NULL, NULL
	},

	/*
	 * We sometimes multiply the number of shared buffers by two without
	 * checking for overflow, so we mustn't allow more than INT_MAX / 2.
	 */
	{
		{"shared_buffers", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Sets the number of shared memory buffers used by the server."),
			NULL,
			GUC_UNIT_BLOCKS
		},
		&NBuffers,
		16384, 16, INT_MAX / 2,
		NULL, NULL, NULL
	},

	{
		{"vacuum_buffer_usage_limit", PGC_USERSET, RESOURCES_MEM,
			gettext_noop("Sets the buffer pool size for VACUUM, ANALYZE, and autovacuum."),
			NULL,
			GUC_UNIT_KB
		},
		&VacuumBufferUsageLimit,
		2048, 0, MAX_BAS_VAC_RING_SIZE_KB,
		check_vacuum_buffer_usage_limit, NULL, NULL
	},

	{
		{"shared_memory_size", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the size of the server's main shared memory area (rounded up to the nearest MB)."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | GUC_UNIT_MB | GUC_RUNTIME_COMPUTED
		},
		&shared_memory_size_mb,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"shared_memory_size_in_huge_pages", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the number of huge pages needed for the main shared memory area."),
			gettext_noop("-1 indicates that the value could not be determined."),
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | GUC_RUNTIME_COMPUTED
		},
		&shared_memory_size_in_huge_pages,
		-1, -1, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"num_os_semaphores", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the number of semaphores required for the server."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | GUC_RUNTIME_COMPUTED
		},
		&num_os_semaphores,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"commit_timestamp_buffers", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Sets the size of the dedicated buffer pool used for the commit timestamp cache."),
			gettext_noop("Specify 0 to have this value determined as a fraction of \"shared_buffers\"."),
			GUC_UNIT_BLOCKS
		},
		&commit_timestamp_buffers,
		0, 0, SLRU_MAX_ALLOWED_BUFFERS,
		check_commit_ts_buffers, NULL, NULL
	},

	{
		{"multixact_member_buffers", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Sets the size of the dedicated buffer pool used for the MultiXact member cache."),
			NULL,
			GUC_UNIT_BLOCKS
		},
		&multixact_member_buffers,
		32, 16, SLRU_MAX_ALLOWED_BUFFERS,
		check_multixact_member_buffers, NULL, NULL
	},

	{
		{"multixact_offset_buffers", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Sets the size of the dedicated buffer pool used for the MultiXact offset cache."),
			NULL,
			GUC_UNIT_BLOCKS
		},
		&multixact_offset_buffers,
		16, 16, SLRU_MAX_ALLOWED_BUFFERS,
		check_multixact_offset_buffers, NULL, NULL
	},

	{
		{"notify_buffers", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Sets the size of the dedicated buffer pool used for the LISTEN/NOTIFY message cache."),
			NULL,
			GUC_UNIT_BLOCKS
		},
		&notify_buffers,
		16, 16, SLRU_MAX_ALLOWED_BUFFERS,
		check_notify_buffers, NULL, NULL
	},

	{
		{"serializable_buffers", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Sets the size of the dedicated buffer pool used for the serializable transaction cache."),
			NULL,
			GUC_UNIT_BLOCKS
		},
		&serializable_buffers,
		32, 16, SLRU_MAX_ALLOWED_BUFFERS,
		check_serial_buffers, NULL, NULL
	},

	{
		{"subtransaction_buffers", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Sets the size of the dedicated buffer pool used for the subtransaction cache."),
			gettext_noop("Specify 0 to have this value determined as a fraction of \"shared_buffers\"."),
			GUC_UNIT_BLOCKS
		},
		&subtransaction_buffers,
		0, 0, SLRU_MAX_ALLOWED_BUFFERS,
		check_subtrans_buffers, NULL, NULL
	},

	{
		{"transaction_buffers", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Sets the size of the dedicated buffer pool used for the transaction status cache."),
			gettext_noop("Specify 0 to have this value determined as a fraction of \"shared_buffers\"."),
			GUC_UNIT_BLOCKS
		},
		&transaction_buffers,
		0, 0, SLRU_MAX_ALLOWED_BUFFERS,
		check_transaction_buffers, NULL, NULL
	},

	{
		{"temp_buffers", PGC_USERSET, RESOURCES_MEM,
			gettext_noop("Sets the maximum number of temporary buffers used by each session."),
			NULL,
			GUC_UNIT_BLOCKS | GUC_EXPLAIN
		},
		&num_temp_buffers,
		1024, 100, INT_MAX / 2,
		check_temp_buffers, NULL, NULL
	},

	{
		{"port", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the TCP port the server listens on."),
			NULL
		},
		&PostPortNumber,
		DEF_PGPORT, 1, 65535,
		NULL, NULL, NULL
	},

	{
		{"unix_socket_permissions", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the access permissions of the Unix-domain socket."),
			gettext_noop("Unix-domain sockets use the usual Unix file system "
						 "permission set. The parameter value is expected "
						 "to be a numeric mode specification in the form "
						 "accepted by the chmod and umask system calls. "
						 "(To use the customary octal format the number must "
						 "start with a 0 (zero).)")
		},
		&Unix_socket_permissions,
		0777, 0000, 0777,
		NULL, NULL, show_unix_socket_permissions
	},

	{
		{"log_file_mode", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Sets the file permissions for log files."),
			gettext_noop("The parameter value is expected "
						 "to be a numeric mode specification in the form "
						 "accepted by the chmod and umask system calls. "
						 "(To use the customary octal format the number must "
						 "start with a 0 (zero).)")
		},
		&Log_file_mode,
		0600, 0000, 0777,
		NULL, NULL, show_log_file_mode
	},


	{
		{"data_directory_mode", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the mode of the data directory."),
			gettext_noop("The parameter value is a numeric mode specification "
						 "in the form accepted by the chmod and umask system "
						 "calls. (To use the customary octal format the number "
						 "must start with a 0 (zero).)"),
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | GUC_RUNTIME_COMPUTED
		},
		&data_directory_mode,
		0700, 0000, 0777,
		NULL, NULL, show_data_directory_mode
	},

	{
		{"work_mem", PGC_USERSET, RESOURCES_MEM,
			gettext_noop("Sets the maximum memory to be used for query workspaces."),
			gettext_noop("This much memory can be used by each internal "
						 "sort operation and hash table before switching to "
						 "temporary disk files."),
			GUC_UNIT_KB | GUC_EXPLAIN
		},
		&work_mem,
		4096, 64, MAX_KILOBYTES,
		NULL, NULL, NULL
	},

	/*
	 * Dynamic shared memory has a higher overhead than local memory contexts,
	 * so when testing low-memory scenarios that could use shared memory, the
	 * recommended minimum is 1MB.
	 */
	{
		{"maintenance_work_mem", PGC_USERSET, RESOURCES_MEM,
			gettext_noop("Sets the maximum memory to be used for maintenance operations."),
			gettext_noop("This includes operations such as VACUUM and CREATE INDEX."),
			GUC_UNIT_KB
		},
		&maintenance_work_mem,
		65536, 64, MAX_KILOBYTES,
		NULL, NULL, NULL
	},

	{
		{"logical_decoding_work_mem", PGC_USERSET, RESOURCES_MEM,
			gettext_noop("Sets the maximum memory to be used for logical decoding."),
			gettext_noop("This much memory can be used by each internal "
						 "reorder buffer before spilling to disk."),
			GUC_UNIT_KB
		},
		&logical_decoding_work_mem,
		65536, 64, MAX_KILOBYTES,
		NULL, NULL, NULL
	},

	/*
	 * We use the hopefully-safely-small value of 100kB as the compiled-in
	 * default for max_stack_depth.  InitializeGUCOptions will increase it if
	 * possible, depending on the actual platform-specific stack limit.
	 */
	{
		{"max_stack_depth", PGC_SUSET, RESOURCES_MEM,
			gettext_noop("Sets the maximum stack depth, in kilobytes."),
			NULL,
			GUC_UNIT_KB
		},
		&max_stack_depth,
		100, 100, MAX_KILOBYTES,
		check_max_stack_depth, assign_max_stack_depth, NULL
	},

	{
		{"temp_file_limit", PGC_SUSET, RESOURCES_DISK,
			gettext_noop("Limits the total size of all temporary files used by each process."),
			gettext_noop("-1 means no limit."),
			GUC_UNIT_KB
		},
		&temp_file_limit,
		-1, -1, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"vacuum_cost_page_hit", PGC_USERSET, RESOURCES_VACUUM_DELAY,
			gettext_noop("Vacuum cost for a page found in the buffer cache."),
			NULL
		},
		&VacuumCostPageHit,
		1, 0, 10000,
		NULL, NULL, NULL
	},

	{
		{"vacuum_cost_page_miss", PGC_USERSET, RESOURCES_VACUUM_DELAY,
			gettext_noop("Vacuum cost for a page not found in the buffer cache."),
			NULL
		},
		&VacuumCostPageMiss,
		2, 0, 10000,
		NULL, NULL, NULL
	},

	{
		{"vacuum_cost_page_dirty", PGC_USERSET, RESOURCES_VACUUM_DELAY,
			gettext_noop("Vacuum cost for a page dirtied by vacuum."),
			NULL
		},
		&VacuumCostPageDirty,
		20, 0, 10000,
		NULL, NULL, NULL
	},

	{
		{"vacuum_cost_limit", PGC_USERSET, RESOURCES_VACUUM_DELAY,
			gettext_noop("Vacuum cost amount available before napping."),
			NULL
		},
		&VacuumCostLimit,
		200, 1, 10000,
		NULL, NULL, NULL
	},

	{
		{"autovacuum_vacuum_cost_limit", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Vacuum cost amount available before napping, for autovacuum."),
			NULL
		},
		&autovacuum_vac_cost_limit,
		-1, -1, 10000,
		NULL, NULL, NULL
	},

	{
		{"max_files_per_process", PGC_POSTMASTER, RESOURCES_KERNEL,
			gettext_noop("Sets the maximum number of simultaneously open files for each server process."),
			NULL
		},
		&max_files_per_process,
		1000, 64, INT_MAX,
		NULL, NULL, NULL
	},

	/*
	 * See also CheckRequiredParameterValues() if this parameter changes
	 */
	{
		{"max_prepared_transactions", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Sets the maximum number of simultaneously prepared transactions."),
			NULL
		},
		&max_prepared_xacts,
		0, 0, MAX_BACKENDS,
		NULL, NULL, NULL
	},

#ifdef LOCK_DEBUG
	{
		{"trace_lock_oidmin", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Sets the minimum OID of tables for tracking locks."),
			gettext_noop("Is used to avoid output on system tables."),
			GUC_NOT_IN_SAMPLE
		},
		&Trace_lock_oidmin,
		FirstNormalObjectId, 0, INT_MAX,
		NULL, NULL, NULL
	},
	{
		{"trace_lock_table", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Sets the OID of the table with unconditionally lock tracing."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&Trace_lock_table,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},
#endif

	{
		{"statement_timeout", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the maximum allowed duration of any statement."),
			gettext_noop("A value of 0 turns off the timeout."),
			GUC_UNIT_MS
		},
		&StatementTimeout,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"lock_timeout", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the maximum allowed duration of any wait for a lock."),
			gettext_noop("A value of 0 turns off the timeout."),
			GUC_UNIT_MS
		},
		&LockTimeout,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"idle_in_transaction_session_timeout", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the maximum allowed idle time between queries, when in a transaction."),
			gettext_noop("A value of 0 turns off the timeout."),
			GUC_UNIT_MS
		},
		&IdleInTransactionSessionTimeout,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"transaction_timeout", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the maximum allowed duration of any transaction within a session (not a prepared transaction)."),
			gettext_noop("A value of 0 turns off the timeout."),
			GUC_UNIT_MS
		},
		&TransactionTimeout,
		0, 0, INT_MAX,
		NULL, assign_transaction_timeout, NULL
	},

	{
		{"idle_session_timeout", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the maximum allowed idle time between queries, when not in a transaction."),
			gettext_noop("A value of 0 turns off the timeout."),
			GUC_UNIT_MS
		},
		&IdleSessionTimeout,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"vacuum_freeze_min_age", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Minimum age at which VACUUM should freeze a table row."),
			NULL
		},
		&vacuum_freeze_min_age,
		50000000, 0, 1000000000,
		NULL, NULL, NULL
	},

	{
		{"vacuum_freeze_table_age", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Age at which VACUUM should scan whole table to freeze tuples."),
			NULL
		},
		&vacuum_freeze_table_age,
		150000000, 0, 2000000000,
		NULL, NULL, NULL
	},

	{
		{"vacuum_multixact_freeze_min_age", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Minimum age at which VACUUM should freeze a MultiXactId in a table row."),
			NULL
		},
		&vacuum_multixact_freeze_min_age,
		5000000, 0, 1000000000,
		NULL, NULL, NULL
	},

	{
		{"vacuum_multixact_freeze_table_age", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Multixact age at which VACUUM should scan whole table to freeze tuples."),
			NULL
		},
		&vacuum_multixact_freeze_table_age,
		150000000, 0, 2000000000,
		NULL, NULL, NULL
	},

	{
		{"vacuum_failsafe_age", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Age at which VACUUM should trigger failsafe to avoid a wraparound outage."),
			NULL
		},
		&vacuum_failsafe_age,
		1600000000, 0, 2100000000,
		NULL, NULL, NULL
	},
	{
		{"vacuum_multixact_failsafe_age", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Multixact age at which VACUUM should trigger failsafe to avoid a wraparound outage."),
			NULL
		},
		&vacuum_multixact_failsafe_age,
		1600000000, 0, 2100000000,
		NULL, NULL, NULL
	},

	/*
	 * See also CheckRequiredParameterValues() if this parameter changes
	 */
	{
		{"max_locks_per_transaction", PGC_POSTMASTER, LOCK_MANAGEMENT,
			gettext_noop("Sets the maximum number of locks per transaction."),
			gettext_noop("The shared lock table is sized on the assumption that at most "
						 "\"max_locks_per_transaction\" objects per server process or prepared "
						 "transaction will need to be locked at any one time.")
		},
		&max_locks_per_xact,
		64, 10, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"max_pred_locks_per_transaction", PGC_POSTMASTER, LOCK_MANAGEMENT,
			gettext_noop("Sets the maximum number of predicate locks per transaction."),
			gettext_noop("The shared predicate lock table is sized on the assumption that "
						 "at most \"max_pred_locks_per_transaction\" objects per server process "
						 "or prepared transaction will need to be locked at any one time.")
		},
		&max_predicate_locks_per_xact,
		64, 10, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"max_pred_locks_per_relation", PGC_SIGHUP, LOCK_MANAGEMENT,
			gettext_noop("Sets the maximum number of predicate-locked pages and tuples per relation."),
			gettext_noop("If more than this total of pages and tuples in the same relation are locked "
						 "by a connection, those locks are replaced by a relation-level lock.")
		},
		&max_predicate_locks_per_relation,
		-2, INT_MIN, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"max_pred_locks_per_page", PGC_SIGHUP, LOCK_MANAGEMENT,
			gettext_noop("Sets the maximum number of predicate-locked tuples per page."),
			gettext_noop("If more than this number of tuples on the same page are locked "
						 "by a connection, those locks are replaced by a page-level lock.")
		},
		&max_predicate_locks_per_page,
		2, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"authentication_timeout", PGC_SIGHUP, CONN_AUTH_AUTH,
			gettext_noop("Sets the maximum allowed time to complete client authentication."),
			NULL,
			GUC_UNIT_S
		},
		&AuthenticationTimeout,
		60, 1, 600,
		NULL, NULL, NULL
	},

	{
		/* Not for general use */
		{"pre_auth_delay", PGC_SIGHUP, DEVELOPER_OPTIONS,
			gettext_noop("Sets the amount of time to wait before "
						 "authentication on connection startup."),
			gettext_noop("This allows attaching a debugger to the process."),
			GUC_NOT_IN_SAMPLE | GUC_UNIT_S
		},
		&PreAuthDelay,
		0, 0, 60,
		NULL, NULL, NULL
	},

	{
		{"max_notify_queue_pages", PGC_POSTMASTER, RESOURCES_DISK,
			gettext_noop("Sets the maximum number of allocated pages for NOTIFY / LISTEN queue."),
			NULL,
		},
		&max_notify_queue_pages,
		1048576, 64, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"wal_decode_buffer_size", PGC_POSTMASTER, WAL_RECOVERY,
			gettext_noop("Buffer size for reading ahead in the WAL during recovery."),
			gettext_noop("Maximum distance to read ahead in the WAL to prefetch referenced data blocks."),
			GUC_UNIT_BYTE
		},
		&wal_decode_buffer_size,
		512 * 1024, 64 * 1024, MaxAllocSize,
		NULL, NULL, NULL
	},

	{
		{"wal_keep_size", PGC_SIGHUP, REPLICATION_SENDING,
			gettext_noop("Sets the size of WAL files held for standby servers."),
			NULL,
			GUC_UNIT_MB
		},
		&wal_keep_size_mb,
		0, 0, MAX_KILOBYTES,
		NULL, NULL, NULL
	},

	{
		{"min_wal_size", PGC_SIGHUP, WAL_CHECKPOINTS,
			gettext_noop("Sets the minimum size to shrink the WAL to."),
			NULL,
			GUC_UNIT_MB
		},
		&min_wal_size_mb,
		DEFAULT_MIN_WAL_SEGS * (DEFAULT_XLOG_SEG_SIZE / (1024 * 1024)),
		2, MAX_KILOBYTES,
		NULL, NULL, NULL
	},

	{
		{"max_wal_size", PGC_SIGHUP, WAL_CHECKPOINTS,
			gettext_noop("Sets the WAL size that triggers a checkpoint."),
			NULL,
			GUC_UNIT_MB
		},
		&max_wal_size_mb,
		DEFAULT_MAX_WAL_SEGS * (DEFAULT_XLOG_SEG_SIZE / (1024 * 1024)),
		2, MAX_KILOBYTES,
		NULL, assign_max_wal_size, NULL
	},

	{
		{"checkpoint_timeout", PGC_SIGHUP, WAL_CHECKPOINTS,
			gettext_noop("Sets the maximum time between automatic WAL checkpoints."),
			NULL,
			GUC_UNIT_S
		},
		&CheckPointTimeout,
		300, 30, 86400,
		NULL, NULL, NULL
	},

	{
		{"checkpoint_warning", PGC_SIGHUP, WAL_CHECKPOINTS,
			gettext_noop("Sets the maximum time before warning if checkpoints "
						 "triggered by WAL volume happen too frequently."),
			gettext_noop("Write a message to the server log if checkpoints "
						 "caused by the filling of WAL segment files happen more "
						 "frequently than this amount of time. "
						 "Zero turns off the warning."),
			GUC_UNIT_S
		},
		&CheckPointWarning,
		30, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"checkpoint_flush_after", PGC_SIGHUP, WAL_CHECKPOINTS,
			gettext_noop("Number of pages after which previously performed writes are flushed to disk."),
			NULL,
			GUC_UNIT_BLOCKS
		},
		&checkpoint_flush_after,
		DEFAULT_CHECKPOINT_FLUSH_AFTER, 0, WRITEBACK_MAX_PENDING_FLUSHES,
		NULL, NULL, NULL
	},

	{
		{"wal_buffers", PGC_POSTMASTER, WAL_SETTINGS,
			gettext_noop("Sets the number of disk-page buffers in shared memory for WAL."),
			gettext_noop("Specify -1 to have this value determined as a fraction of \"shared_buffers\"."),
			GUC_UNIT_XBLOCKS
		},
		&XLOGbuffers,
		-1, -1, (INT_MAX / XLOG_BLCKSZ),
		check_wal_buffers, NULL, NULL
	},

	{
		{"wal_writer_delay", PGC_SIGHUP, WAL_SETTINGS,
			gettext_noop("Time between WAL flushes performed in the WAL writer."),
			NULL,
			GUC_UNIT_MS
		},
		&WalWriterDelay,
		200, 1, 10000,
		NULL, NULL, NULL
	},

	{
		{"wal_writer_flush_after", PGC_SIGHUP, WAL_SETTINGS,
			gettext_noop("Amount of WAL written out by WAL writer that triggers a flush."),
			NULL,
			GUC_UNIT_XBLOCKS
		},
		&WalWriterFlushAfter,
		DEFAULT_WAL_WRITER_FLUSH_AFTER, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"wal_skip_threshold", PGC_USERSET, WAL_SETTINGS,
			gettext_noop("Minimum size of new file to fsync instead of writing WAL."),
			NULL,
			GUC_UNIT_KB
		},
		&wal_skip_threshold,
		2048, 0, MAX_KILOBYTES,
		NULL, NULL, NULL
	},

	{
		{"max_wal_senders", PGC_POSTMASTER, REPLICATION_SENDING,
			gettext_noop("Sets the maximum number of simultaneously running WAL sender processes."),
			NULL
		},
		&max_wal_senders,
		10, 0, MAX_BACKENDS,
		NULL, NULL, NULL
	},

	{
		/* see max_wal_senders */
		{"max_replication_slots", PGC_POSTMASTER, REPLICATION_SENDING,
			gettext_noop("Sets the maximum number of simultaneously defined replication slots."),
			NULL
		},
		&max_replication_slots,
		10, 0, MAX_BACKENDS /* XXX? */ ,
		NULL, NULL, NULL
	},

	{
		{"max_slot_wal_keep_size", PGC_SIGHUP, REPLICATION_SENDING,
			gettext_noop("Sets the maximum WAL size that can be reserved by replication slots."),
			gettext_noop("Replication slots will be marked as failed, and segments released "
						 "for deletion or recycling, if this much space is occupied by WAL "
						 "on disk."),
			GUC_UNIT_MB
		},
		&max_slot_wal_keep_size_mb,
		-1, -1, MAX_KILOBYTES,
		check_max_slot_wal_keep_size, NULL, NULL
	},

	{
		{"wal_sender_timeout", PGC_USERSET, REPLICATION_SENDING,
			gettext_noop("Sets the maximum time to wait for WAL replication."),
			NULL,
			GUC_UNIT_MS
		},
		&wal_sender_timeout,
		60 * 1000, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"commit_delay", PGC_SUSET, WAL_SETTINGS,
			gettext_noop("Sets the delay in microseconds between transaction commit and "
						 "flushing WAL to disk."),
			NULL
			/* we have no microseconds designation, so can't supply units here */
		},
		&CommitDelay,
		0, 0, 100000,
		NULL, NULL, NULL
	},

	{
		{"commit_siblings", PGC_USERSET, WAL_SETTINGS,
			gettext_noop("Sets the minimum number of concurrent open transactions "
						 "required before performing \"commit_delay\"."),
			NULL
		},
		&CommitSiblings,
		5, 0, 1000,
		NULL, NULL, NULL
	},

	{
		{"extra_float_digits", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the number of digits displayed for floating-point values."),
			gettext_noop("This affects real, double precision, and geometric data types. "
						 "A zero or negative parameter value is added to the standard "
						 "number of digits (FLT_DIG or DBL_DIG as appropriate). "
						 "Any value greater than zero selects precise output mode.")
		},
		&extra_float_digits,
		1, -15, 3,
		NULL, NULL, NULL
	},

	{
		{"log_min_duration_sample", PGC_SUSET, LOGGING_WHEN,
			gettext_noop("Sets the minimum execution time above which "
						 "a sample of statements will be logged."
						 " Sampling is determined by \"log_statement_sample_rate\"."),
			gettext_noop("Zero logs a sample of all queries. -1 turns this feature off."),
			GUC_UNIT_MS
		},
		&log_min_duration_sample,
		-1, -1, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"log_min_duration_statement", PGC_SUSET, LOGGING_WHEN,
			gettext_noop("Sets the minimum execution time above which "
						 "all statements will be logged."),
			gettext_noop("Zero prints all queries. -1 turns this feature off."),
			GUC_UNIT_MS
		},
		&log_min_duration_statement,
		-1, -1, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"log_autovacuum_min_duration", PGC_SIGHUP, LOGGING_WHAT,
			gettext_noop("Sets the minimum execution time above which "
						 "autovacuum actions will be logged."),
			gettext_noop("Zero prints all actions. -1 turns autovacuum logging off."),
			GUC_UNIT_MS
		},
		&Log_autovacuum_min_duration,
		600000, -1, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"log_parameter_max_length", PGC_SUSET, LOGGING_WHAT,
			gettext_noop("Sets the maximum length in bytes of data logged for bind "
						 "parameter values when logging statements."),
			gettext_noop("-1 to print values in full."),
			GUC_UNIT_BYTE
		},
		&log_parameter_max_length,
		-1, -1, INT_MAX / 2,
		NULL, NULL, NULL
	},

	{
		{"log_parameter_max_length_on_error", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Sets the maximum length in bytes of data logged for bind "
						 "parameter values when logging statements, on error."),
			gettext_noop("-1 to print values in full."),
			GUC_UNIT_BYTE
		},
		&log_parameter_max_length_on_error,
		0, -1, INT_MAX / 2,
		NULL, NULL, NULL
	},

	{
		{"bgwriter_delay", PGC_SIGHUP, RESOURCES_BGWRITER,
			gettext_noop("Background writer sleep time between rounds."),
			NULL,
			GUC_UNIT_MS
		},
		&BgWriterDelay,
		200, 10, 10000,
		NULL, NULL, NULL
	},

	{
		{"bgwriter_lru_maxpages", PGC_SIGHUP, RESOURCES_BGWRITER,
			gettext_noop("Background writer maximum number of LRU pages to flush per round."),
			NULL
		},
		&bgwriter_lru_maxpages,
		100, 0, INT_MAX / 2,	/* Same upper limit as shared_buffers */
		NULL, NULL, NULL
	},

	{
		{"bgwriter_flush_after", PGC_SIGHUP, RESOURCES_BGWRITER,
			gettext_noop("Number of pages after which previously performed writes are flushed to disk."),
			NULL,
			GUC_UNIT_BLOCKS
		},
		&bgwriter_flush_after,
		DEFAULT_BGWRITER_FLUSH_AFTER, 0, WRITEBACK_MAX_PENDING_FLUSHES,
		NULL, NULL, NULL
	},

	{
		{"effective_io_concurrency",
			PGC_USERSET,
			RESOURCES_ASYNCHRONOUS,
			gettext_noop("Number of simultaneous requests that can be handled efficiently by the disk subsystem."),
			NULL,
			GUC_EXPLAIN
		},
		&effective_io_concurrency,
		DEFAULT_EFFECTIVE_IO_CONCURRENCY,
		0, MAX_IO_CONCURRENCY,
		check_effective_io_concurrency, NULL, NULL
	},

	{
		{"maintenance_io_concurrency",
			PGC_USERSET,
			RESOURCES_ASYNCHRONOUS,
			gettext_noop("A variant of \"effective_io_concurrency\" that is used for maintenance work."),
			NULL,
			GUC_EXPLAIN
		},
		&maintenance_io_concurrency,
		DEFAULT_MAINTENANCE_IO_CONCURRENCY,
		0, MAX_IO_CONCURRENCY,
		check_maintenance_io_concurrency, assign_maintenance_io_concurrency,
		NULL
	},

	{
		{"io_combine_limit",
			PGC_USERSET,
			RESOURCES_ASYNCHRONOUS,
			gettext_noop("Limit on the size of data reads and writes."),
			NULL,
			GUC_UNIT_BLOCKS
		},
		&io_combine_limit,
		DEFAULT_IO_COMBINE_LIMIT,
		1, MAX_IO_COMBINE_LIMIT,
		NULL, NULL, NULL
	},

	{
		{"backend_flush_after", PGC_USERSET, RESOURCES_ASYNCHRONOUS,
			gettext_noop("Number of pages after which previously performed writes are flushed to disk."),
			NULL,
			GUC_UNIT_BLOCKS
		},
		&backend_flush_after,
		DEFAULT_BACKEND_FLUSH_AFTER, 0, WRITEBACK_MAX_PENDING_FLUSHES,
		NULL, NULL, NULL
	},

	{
		{"max_worker_processes",
			PGC_POSTMASTER,
			RESOURCES_ASYNCHRONOUS,
			gettext_noop("Maximum number of concurrent worker processes."),
			NULL,
		},
		&max_worker_processes,
		8, 0, MAX_BACKENDS,
		NULL, NULL, NULL
	},

	{
		{"max_logical_replication_workers",
			PGC_POSTMASTER,
			REPLICATION_SUBSCRIBERS,
			gettext_noop("Maximum number of logical replication worker processes."),
			NULL,
		},
		&max_logical_replication_workers,
		4, 0, MAX_BACKENDS,
		NULL, NULL, NULL
	},

	{
		{"max_sync_workers_per_subscription",
			PGC_SIGHUP,
			REPLICATION_SUBSCRIBERS,
			gettext_noop("Maximum number of table synchronization workers per subscription."),
			NULL,
		},
		&max_sync_workers_per_subscription,
		2, 0, MAX_BACKENDS,
		NULL, NULL, NULL
	},

	{
		{"max_parallel_apply_workers_per_subscription",
			PGC_SIGHUP,
			REPLICATION_SUBSCRIBERS,
			gettext_noop("Maximum number of parallel apply workers per subscription."),
			NULL,
		},
		&max_parallel_apply_workers_per_subscription,
		2, 0, MAX_PARALLEL_WORKER_LIMIT,
		NULL, NULL, NULL
	},

	{
		{"log_rotation_age", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Sets the amount of time to wait before forcing "
						 "log file rotation."),
			NULL,
			GUC_UNIT_MIN
		},
		&Log_RotationAge,
		HOURS_PER_DAY * MINS_PER_HOUR, 0, INT_MAX / SECS_PER_MINUTE,
		NULL, NULL, NULL
	},

	{
		{"log_rotation_size", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Sets the maximum size a log file can reach before "
						 "being rotated."),
			NULL,
			GUC_UNIT_KB
		},
		&Log_RotationSize,
		10 * 1024, 0, INT_MAX / 1024,
		NULL, NULL, NULL
	},

	{
		{"max_function_args", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the maximum number of function arguments."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&max_function_args,
		FUNC_MAX_ARGS, FUNC_MAX_ARGS, FUNC_MAX_ARGS,
		NULL, NULL, NULL
	},

	{
		{"max_index_keys", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the maximum number of index keys."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&max_index_keys,
		INDEX_MAX_KEYS, INDEX_MAX_KEYS, INDEX_MAX_KEYS,
		NULL, NULL, NULL
	},

	{
		{"max_identifier_length", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the maximum identifier length."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&max_identifier_length,
		NAMEDATALEN - 1, NAMEDATALEN - 1, NAMEDATALEN - 1,
		NULL, NULL, NULL
	},

	{
		{"block_size", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the size of a disk block."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&block_size,
		BLCKSZ, BLCKSZ, BLCKSZ,
		NULL, NULL, NULL
	},

	{
		{"segment_size", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the number of pages per disk file."),
			NULL,
			GUC_UNIT_BLOCKS | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&segment_size,
		RELSEG_SIZE, RELSEG_SIZE, RELSEG_SIZE,
		NULL, NULL, NULL
	},

	{
		{"wal_block_size", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the block size in the write ahead log."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&wal_block_size,
		XLOG_BLCKSZ, XLOG_BLCKSZ, XLOG_BLCKSZ,
		NULL, NULL, NULL
	},

	{
		{"wal_retrieve_retry_interval", PGC_SIGHUP, REPLICATION_STANDBY,
			gettext_noop("Sets the time to wait before retrying to retrieve WAL "
						 "after a failed attempt."),
			NULL,
			GUC_UNIT_MS
		},
		&wal_retrieve_retry_interval,
		5000, 1, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"wal_segment_size", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the size of write ahead log segments."),
			NULL,
			GUC_UNIT_BYTE | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | GUC_RUNTIME_COMPUTED
		},
		&wal_segment_size,
		DEFAULT_XLOG_SEG_SIZE,
		WalSegMinSize,
		WalSegMaxSize,
		check_wal_segment_size, NULL, NULL
	},

	{
		{"wal_summary_keep_time", PGC_SIGHUP, WAL_SUMMARIZATION,
			gettext_noop("Time for which WAL summary files should be kept."),
			NULL,
			GUC_UNIT_MIN,
		},
		&wal_summary_keep_time,
		10 * HOURS_PER_DAY * MINS_PER_HOUR, /* 10 days */
		0,
		INT_MAX / SECS_PER_MINUTE,
		NULL, NULL, NULL
	},

	{
		{"autovacuum_naptime", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Time to sleep between autovacuum runs."),
			NULL,
			GUC_UNIT_S
		},
		&autovacuum_naptime,
		60, 1, INT_MAX / 1000,
		NULL, NULL, NULL
	},
	{
		{"autovacuum_vacuum_threshold", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Minimum number of tuple updates or deletes prior to vacuum."),
			NULL
		},
		&autovacuum_vac_thresh,
		50, 0, INT_MAX,
		NULL, NULL, NULL
	},
	{
		{"autovacuum_vacuum_insert_threshold", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Minimum number of tuple inserts prior to vacuum, or -1 to disable insert vacuums."),
			NULL
		},
		&autovacuum_vac_ins_thresh,
		1000, -1, INT_MAX,
		NULL, NULL, NULL
	},
	{
		{"autovacuum_analyze_threshold", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Minimum number of tuple inserts, updates, or deletes prior to analyze."),
			NULL
		},
		&autovacuum_anl_thresh,
		50, 0, INT_MAX,
		NULL, NULL, NULL
	},
	{
		/* see varsup.c for why this is PGC_POSTMASTER not PGC_SIGHUP */
		{"autovacuum_freeze_max_age", PGC_POSTMASTER, AUTOVACUUM,
			gettext_noop("Age at which to autovacuum a table to prevent transaction ID wraparound."),
			NULL
		},
		&autovacuum_freeze_max_age,

		/* see vacuum_failsafe_age if you change the upper-limit value. */
		200000000, 100000, 2000000000,
		NULL, NULL, NULL
	},
	{
		/* see multixact.c for why this is PGC_POSTMASTER not PGC_SIGHUP */
		{"autovacuum_multixact_freeze_max_age", PGC_POSTMASTER, AUTOVACUUM,
			gettext_noop("Multixact age at which to autovacuum a table to prevent multixact wraparound."),
			NULL
		},
		&autovacuum_multixact_freeze_max_age,
		400000000, 10000, 2000000000,
		NULL, NULL, NULL
	},
	{
		/* see max_connections */
		{"autovacuum_max_workers", PGC_POSTMASTER, AUTOVACUUM,
			gettext_noop("Sets the maximum number of simultaneously running autovacuum worker processes."),
			NULL
		},
		&autovacuum_max_workers,
		3, 1, MAX_BACKENDS,
		NULL, NULL, NULL
	},

	{
		{"max_parallel_maintenance_workers", PGC_USERSET, RESOURCES_ASYNCHRONOUS,
			gettext_noop("Sets the maximum number of parallel processes per maintenance operation."),
			NULL
		},
		&max_parallel_maintenance_workers,
		2, 0, 1024,
		NULL, NULL, NULL
	},

	{
		{"max_parallel_workers_per_gather", PGC_USERSET, RESOURCES_ASYNCHRONOUS,
			gettext_noop("Sets the maximum number of parallel processes per executor node."),
			NULL,
			GUC_EXPLAIN
		},
		&max_parallel_workers_per_gather,
		2, 0, MAX_PARALLEL_WORKER_LIMIT,
		NULL, NULL, NULL
	},

	{
		{"max_parallel_workers", PGC_USERSET, RESOURCES_ASYNCHRONOUS,
			gettext_noop("Sets the maximum number of parallel workers that can be active at one time."),
			NULL,
			GUC_EXPLAIN
		},
		&max_parallel_workers,
		8, 0, MAX_PARALLEL_WORKER_LIMIT,
		NULL, NULL, NULL
	},

	{
		{"autovacuum_work_mem", PGC_SIGHUP, RESOURCES_MEM,
			gettext_noop("Sets the maximum memory to be used by each autovacuum worker process."),
			NULL,
			GUC_UNIT_KB
		},
		&autovacuum_work_mem,
		-1, -1, MAX_KILOBYTES,
		check_autovacuum_work_mem, NULL, NULL
	},

	{
		{"tcp_keepalives_idle", PGC_USERSET, CONN_AUTH_TCP,
			gettext_noop("Time between issuing TCP keepalives."),
			gettext_noop("A value of 0 uses the system default."),
			GUC_UNIT_S
		},
		&tcp_keepalives_idle,
		0, 0, INT_MAX,
		NULL, assign_tcp_keepalives_idle, show_tcp_keepalives_idle
	},

	{
		{"tcp_keepalives_interval", PGC_USERSET, CONN_AUTH_TCP,
			gettext_noop("Time between TCP keepalive retransmits."),
			gettext_noop("A value of 0 uses the system default."),
			GUC_UNIT_S
		},
		&tcp_keepalives_interval,
		0, 0, INT_MAX,
		NULL, assign_tcp_keepalives_interval, show_tcp_keepalives_interval
	},

	{
		{"ssl_renegotiation_limit", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("SSL renegotiation is no longer supported; this can only be 0."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE,
		},
		&ssl_renegotiation_limit,
		0, 0, 0,
		NULL, NULL, NULL
	},

	{
		{"tcp_keepalives_count", PGC_USERSET, CONN_AUTH_TCP,
			gettext_noop("Maximum number of TCP keepalive retransmits."),
			gettext_noop("Number of consecutive keepalive retransmits that can be "
						 "lost before a connection is considered dead. A value of 0 uses the "
						 "system default."),
		},
		&tcp_keepalives_count,
		0, 0, INT_MAX,
		NULL, assign_tcp_keepalives_count, show_tcp_keepalives_count
	},

	{
		{"gin_fuzzy_search_limit", PGC_USERSET, CLIENT_CONN_OTHER,
			gettext_noop("Sets the maximum allowed result for exact search by GIN."),
			NULL,
			0
		},
		&GinFuzzySearchLimit,
		0, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"effective_cache_size", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's assumption about the total size of the data caches."),
			gettext_noop("That is, the total size of the caches (kernel cache and shared buffers) used for PostgreSQL data files. "
						 "This is measured in disk pages, which are normally 8 kB each."),
			GUC_UNIT_BLOCKS | GUC_EXPLAIN,
		},
		&effective_cache_size,
		DEFAULT_EFFECTIVE_CACHE_SIZE, 1, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"min_parallel_table_scan_size", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the minimum amount of table data for a parallel scan."),
			gettext_noop("If the planner estimates that it will read a number of table pages too small to reach this limit, a parallel scan will not be considered."),
			GUC_UNIT_BLOCKS | GUC_EXPLAIN,
		},
		&min_parallel_table_scan_size,
		(8 * 1024 * 1024) / BLCKSZ, 0, INT_MAX / 3,
		NULL, NULL, NULL
	},

	{
		{"min_parallel_index_scan_size", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the minimum amount of index data for a parallel scan."),
			gettext_noop("If the planner estimates that it will read a number of index pages too small to reach this limit, a parallel scan will not be considered."),
			GUC_UNIT_BLOCKS | GUC_EXPLAIN,
		},
		&min_parallel_index_scan_size,
		(512 * 1024) / BLCKSZ, 0, INT_MAX / 3,
		NULL, NULL, NULL
	},

	{
		/* Can't be set in postgresql.conf */
		{"server_version_num", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the server version as an integer."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&server_version_num,
		PG_VERSION_NUM, PG_VERSION_NUM, PG_VERSION_NUM,
		NULL, NULL, NULL
	},

	{
		{"log_temp_files", PGC_SUSET, LOGGING_WHAT,
			gettext_noop("Log the use of temporary files larger than this number of kilobytes."),
			gettext_noop("Zero logs all files. The default is -1 (turning this feature off)."),
			GUC_UNIT_KB
		},
		&log_temp_files,
		-1, -1, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"track_activity_query_size", PGC_POSTMASTER, STATS_CUMULATIVE,
			gettext_noop("Sets the size reserved for pg_stat_activity.query, in bytes."),
			NULL,
			GUC_UNIT_BYTE
		},
		&pgstat_track_activity_query_size,
		1024, 100, 1048576,
		NULL, NULL, NULL
	},

	{
		{"gin_pending_list_limit", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the maximum size of the pending list for GIN index."),
			NULL,
			GUC_UNIT_KB
		},
		&gin_pending_list_limit,
		4096, 64, MAX_KILOBYTES,
		NULL, NULL, NULL
	},

	{
		{"tcp_user_timeout", PGC_USERSET, CONN_AUTH_TCP,
			gettext_noop("TCP user timeout."),
			gettext_noop("A value of 0 uses the system default."),
			GUC_UNIT_MS
		},
		&tcp_user_timeout,
		0, 0, INT_MAX,
		NULL, assign_tcp_user_timeout, show_tcp_user_timeout
	},

	{
		{"huge_page_size", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("The size of huge page that should be requested."),
			NULL,
			GUC_UNIT_KB
		},
		&huge_page_size,
		0, 0, INT_MAX,
		check_huge_page_size, NULL, NULL
	},

	{
		{"debug_discard_caches", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Aggressively flush system caches for debugging purposes."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&debug_discard_caches,
#ifdef DISCARD_CACHES_ENABLED
		/* Set default based on older compile-time-only cache clobber macros */
#if defined(CLOBBER_CACHE_RECURSIVELY)
		3,
#elif defined(CLOBBER_CACHE_ALWAYS)
		1,
#else
		0,
#endif
		0, 5,
#else							/* not DISCARD_CACHES_ENABLED */
		0, 0, 0,
#endif							/* not DISCARD_CACHES_ENABLED */
		NULL, NULL, NULL
	},

	{
		{"client_connection_check_interval", PGC_USERSET, CONN_AUTH_TCP,
			gettext_noop("Sets the time interval between checks for disconnection while running queries."),
			NULL,
			GUC_UNIT_MS
		},
		&client_connection_check_interval,
		0, 0, INT_MAX,
		check_client_connection_check_interval, NULL, NULL
	},

	{
		{"log_startup_progress_interval", PGC_SIGHUP, LOGGING_WHEN,
			gettext_noop("Time between progress updates for "
						 "long-running startup operations."),
			gettext_noop("0 turns this feature off."),
			GUC_UNIT_MS,
		},
		&log_startup_progress_interval,
		10000, 0, INT_MAX,
		NULL, NULL, NULL
	},

	{
		{"scram_iterations", PGC_USERSET, CONN_AUTH_AUTH,
			gettext_noop("Sets the iteration count for SCRAM secret generation."),
			NULL,
			GUC_REPORT
		},
		&scram_sha_256_iterations,
		SCRAM_SHA_256_DEFAULT_ITERATIONS, 1, INT_MAX,
		NULL, NULL, NULL
	},

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, 0, 0, 0, NULL, NULL, NULL
	}
};


struct config_real ConfigureNamesReal[] =
{
	{
		{"seq_page_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of the cost of a "
						 "sequentially fetched disk page."),
			NULL,
			GUC_EXPLAIN
		},
		&seq_page_cost,
		DEFAULT_SEQ_PAGE_COST, 0, DBL_MAX,
		NULL, NULL, NULL
	},
	{
		{"random_page_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of the cost of a "
						 "nonsequentially fetched disk page."),
			NULL,
			GUC_EXPLAIN
		},
		&random_page_cost,
		DEFAULT_RANDOM_PAGE_COST, 0, DBL_MAX,
		NULL, NULL, NULL
	},
	{
		{"cpu_tuple_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of the cost of "
						 "processing each tuple (row)."),
			NULL,
			GUC_EXPLAIN
		},
		&cpu_tuple_cost,
		DEFAULT_CPU_TUPLE_COST, 0, DBL_MAX,
		NULL, NULL, NULL
	},
	{
		{"cpu_index_tuple_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of the cost of "
						 "processing each index entry during an index scan."),
			NULL,
			GUC_EXPLAIN
		},
		&cpu_index_tuple_cost,
		DEFAULT_CPU_INDEX_TUPLE_COST, 0, DBL_MAX,
		NULL, NULL, NULL
	},
	{
		{"cpu_operator_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of the cost of "
						 "processing each operator or function call."),
			NULL,
			GUC_EXPLAIN
		},
		&cpu_operator_cost,
		DEFAULT_CPU_OPERATOR_COST, 0, DBL_MAX,
		NULL, NULL, NULL
	},
	{
		{"parallel_tuple_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of the cost of "
						 "passing each tuple (row) from worker to leader backend."),
			NULL,
			GUC_EXPLAIN
		},
		&parallel_tuple_cost,
		DEFAULT_PARALLEL_TUPLE_COST, 0, DBL_MAX,
		NULL, NULL, NULL
	},
	{
		{"parallel_setup_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Sets the planner's estimate of the cost of "
						 "starting up worker processes for parallel query."),
			NULL,
			GUC_EXPLAIN
		},
		&parallel_setup_cost,
		DEFAULT_PARALLEL_SETUP_COST, 0, DBL_MAX,
		NULL, NULL, NULL
	},

	{
		{"jit_above_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Perform JIT compilation if query is more expensive."),
			gettext_noop("-1 disables JIT compilation."),
			GUC_EXPLAIN
		},
		&jit_above_cost,
		100000, -1, DBL_MAX,
		NULL, NULL, NULL
	},

	{
		{"jit_optimize_above_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Optimize JIT-compiled functions if query is more expensive."),
			gettext_noop("-1 disables optimization."),
			GUC_EXPLAIN
		},
		&jit_optimize_above_cost,
		500000, -1, DBL_MAX,
		NULL, NULL, NULL
	},

	{
		{"jit_inline_above_cost", PGC_USERSET, QUERY_TUNING_COST,
			gettext_noop("Perform JIT inlining if query is more expensive."),
			gettext_noop("-1 disables inlining."),
			GUC_EXPLAIN
		},
		&jit_inline_above_cost,
		500000, -1, DBL_MAX,
		NULL, NULL, NULL
	},

	{
		{"cursor_tuple_fraction", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Sets the planner's estimate of the fraction of "
						 "a cursor's rows that will be retrieved."),
			NULL,
			GUC_EXPLAIN
		},
		&cursor_tuple_fraction,
		DEFAULT_CURSOR_TUPLE_FRACTION, 0.0, 1.0,
		NULL, NULL, NULL
	},

	{
		{"recursive_worktable_factor", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Sets the planner's estimate of the average size "
						 "of a recursive query's working table."),
			NULL,
			GUC_EXPLAIN
		},
		&recursive_worktable_factor,
		DEFAULT_RECURSIVE_WORKTABLE_FACTOR, 0.001, 1000000.0,
		NULL, NULL, NULL
	},

	{
		{"geqo_selection_bias", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("GEQO: selective pressure within the population."),
			NULL,
			GUC_EXPLAIN
		},
		&Geqo_selection_bias,
		DEFAULT_GEQO_SELECTION_BIAS,
		MIN_GEQO_SELECTION_BIAS, MAX_GEQO_SELECTION_BIAS,
		NULL, NULL, NULL
	},
	{
		{"geqo_seed", PGC_USERSET, QUERY_TUNING_GEQO,
			gettext_noop("GEQO: seed for random path selection."),
			NULL,
			GUC_EXPLAIN
		},
		&Geqo_seed,
		0.0, 0.0, 1.0,
		NULL, NULL, NULL
	},

	{
		{"hash_mem_multiplier", PGC_USERSET, RESOURCES_MEM,
			gettext_noop("Multiple of \"work_mem\" to use for hash tables."),
			NULL,
			GUC_EXPLAIN
		},
		&hash_mem_multiplier,
		2.0, 1.0, 1000.0,
		NULL, NULL, NULL
	},

	{
		{"bgwriter_lru_multiplier", PGC_SIGHUP, RESOURCES_BGWRITER,
			gettext_noop("Multiple of the average buffer usage to free per round."),
			NULL
		},
		&bgwriter_lru_multiplier,
		2.0, 0.0, 10.0,
		NULL, NULL, NULL
	},

	{
		{"seed", PGC_USERSET, UNGROUPED,
			gettext_noop("Sets the seed for random-number generation."),
			NULL,
			GUC_NO_SHOW_ALL | GUC_NO_RESET | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&phony_random_seed,
		0.0, -1.0, 1.0,
		check_random_seed, assign_random_seed, show_random_seed
	},

	{
		{"vacuum_cost_delay", PGC_USERSET, RESOURCES_VACUUM_DELAY,
			gettext_noop("Vacuum cost delay in milliseconds."),
			NULL,
			GUC_UNIT_MS
		},
		&VacuumCostDelay,
		0, 0, 100,
		NULL, NULL, NULL
	},

	{
		{"autovacuum_vacuum_cost_delay", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Vacuum cost delay in milliseconds, for autovacuum."),
			NULL,
			GUC_UNIT_MS
		},
		&autovacuum_vac_cost_delay,
		2, -1, 100,
		NULL, NULL, NULL
	},

	{
		{"autovacuum_vacuum_scale_factor", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Number of tuple updates or deletes prior to vacuum as a fraction of reltuples."),
			NULL
		},
		&autovacuum_vac_scale,
		0.2, 0.0, 100.0,
		NULL, NULL, NULL
	},

	{
		{"autovacuum_vacuum_insert_scale_factor", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Number of tuple inserts prior to vacuum as a fraction of reltuples."),
			NULL
		},
		&autovacuum_vac_ins_scale,
		0.2, 0.0, 100.0,
		NULL, NULL, NULL
	},

	{
		{"autovacuum_analyze_scale_factor", PGC_SIGHUP, AUTOVACUUM,
			gettext_noop("Number of tuple inserts, updates, or deletes prior to analyze as a fraction of reltuples."),
			NULL
		},
		&autovacuum_anl_scale,
		0.1, 0.0, 100.0,
		NULL, NULL, NULL
	},

	{
		{"checkpoint_completion_target", PGC_SIGHUP, WAL_CHECKPOINTS,
			gettext_noop("Time spent flushing dirty buffers during checkpoint, as fraction of checkpoint interval."),
			NULL
		},
		&CheckPointCompletionTarget,
		0.9, 0.0, 1.0,
		NULL, assign_checkpoint_completion_target, NULL
	},

	{
		{"log_statement_sample_rate", PGC_SUSET, LOGGING_WHEN,
			gettext_noop("Fraction of statements exceeding \"log_min_duration_sample\" to be logged."),
			gettext_noop("Use a value between 0.0 (never log) and 1.0 (always log).")
		},
		&log_statement_sample_rate,
		1.0, 0.0, 1.0,
		NULL, NULL, NULL
	},

	{
		{"log_transaction_sample_rate", PGC_SUSET, LOGGING_WHEN,
			gettext_noop("Sets the fraction of transactions from which to log all statements."),
			gettext_noop("Use a value between 0.0 (never log) and 1.0 (log all "
						 "statements for all transactions).")
		},
		&log_xact_sample_rate,
		0.0, 0.0, 1.0,
		NULL, NULL, NULL
	},

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, 0.0, 0.0, 0.0, NULL, NULL, NULL
	}
};


struct config_string ConfigureNamesString[] =
{
	{
		{"archive_command", PGC_SIGHUP, WAL_ARCHIVING,
			gettext_noop("Sets the shell command that will be called to archive a WAL file."),
			gettext_noop("This is used only if \"archive_library\" is not set.")
		},
		&XLogArchiveCommand,
		"",
		NULL, NULL, show_archive_command
	},

	{
		{"archive_library", PGC_SIGHUP, WAL_ARCHIVING,
			gettext_noop("Sets the library that will be called to archive a WAL file."),
			gettext_noop("An empty string indicates that \"archive_command\" should be used.")
		},
		&XLogArchiveLibrary,
		"",
		NULL, NULL, NULL
	},

	{
		{"restore_command", PGC_SIGHUP, WAL_ARCHIVE_RECOVERY,
			gettext_noop("Sets the shell command that will be called to retrieve an archived WAL file."),
			NULL
		},
		&recoveryRestoreCommand,
		"",
		NULL, NULL, NULL
	},

	{
		{"archive_cleanup_command", PGC_SIGHUP, WAL_ARCHIVE_RECOVERY,
			gettext_noop("Sets the shell command that will be executed at every restart point."),
			NULL
		},
		&archiveCleanupCommand,
		"",
		NULL, NULL, NULL
	},

	{
		{"recovery_end_command", PGC_SIGHUP, WAL_ARCHIVE_RECOVERY,
			gettext_noop("Sets the shell command that will be executed once at the end of recovery."),
			NULL
		},
		&recoveryEndCommand,
		"",
		NULL, NULL, NULL
	},

	{
		{"recovery_target_timeline", PGC_POSTMASTER, WAL_RECOVERY_TARGET,
			gettext_noop("Specifies the timeline to recover into."),
			NULL
		},
		&recovery_target_timeline_string,
		"latest",
		check_recovery_target_timeline, assign_recovery_target_timeline, NULL
	},

	{
		{"recovery_target", PGC_POSTMASTER, WAL_RECOVERY_TARGET,
			gettext_noop("Set to \"immediate\" to end recovery as soon as a consistent state is reached."),
			NULL
		},
		&recovery_target_string,
		"",
		check_recovery_target, assign_recovery_target, NULL
	},
	{
		{"recovery_target_xid", PGC_POSTMASTER, WAL_RECOVERY_TARGET,
			gettext_noop("Sets the transaction ID up to which recovery will proceed."),
			NULL
		},
		&recovery_target_xid_string,
		"",
		check_recovery_target_xid, assign_recovery_target_xid, NULL
	},
	{
		{"recovery_target_time", PGC_POSTMASTER, WAL_RECOVERY_TARGET,
			gettext_noop("Sets the time stamp up to which recovery will proceed."),
			NULL
		},
		&recovery_target_time_string,
		"",
		check_recovery_target_time, assign_recovery_target_time, NULL
	},
	{
		{"recovery_target_name", PGC_POSTMASTER, WAL_RECOVERY_TARGET,
			gettext_noop("Sets the named restore point up to which recovery will proceed."),
			NULL
		},
		&recovery_target_name_string,
		"",
		check_recovery_target_name, assign_recovery_target_name, NULL
	},
	{
		{"recovery_target_lsn", PGC_POSTMASTER, WAL_RECOVERY_TARGET,
			gettext_noop("Sets the LSN of the write-ahead log location up to which recovery will proceed."),
			NULL
		},
		&recovery_target_lsn_string,
		"",
		check_recovery_target_lsn, assign_recovery_target_lsn, NULL
	},

	{
		{"primary_conninfo", PGC_SIGHUP, REPLICATION_STANDBY,
			gettext_noop("Sets the connection string to be used to connect to the sending server."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&PrimaryConnInfo,
		"",
		NULL, NULL, NULL
	},

	{
		{"primary_slot_name", PGC_SIGHUP, REPLICATION_STANDBY,
			gettext_noop("Sets the name of the replication slot to use on the sending server."),
			NULL
		},
		&PrimarySlotName,
		"",
		check_primary_slot_name, NULL, NULL
	},

	{
		{"client_encoding", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the client's character set encoding."),
			NULL,
			GUC_IS_NAME | GUC_REPORT
		},
		&client_encoding_string,
		"SQL_ASCII",
		check_client_encoding, assign_client_encoding, NULL
	},

	{
		{"log_line_prefix", PGC_SIGHUP, LOGGING_WHAT,
			gettext_noop("Controls information prefixed to each log line."),
			gettext_noop("If blank, no prefix is used.")
		},
		&Log_line_prefix,
		"%m [%p] ",
		NULL, NULL, NULL
	},

	{
		{"log_timezone", PGC_SIGHUP, LOGGING_WHAT,
			gettext_noop("Sets the time zone to use in log messages."),
			NULL
		},
		&log_timezone_string,
		"GMT",
		check_log_timezone, assign_log_timezone, show_log_timezone
	},

	{
		{"DateStyle", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the display format for date and time values."),
			gettext_noop("Also controls interpretation of ambiguous "
						 "date inputs."),
			GUC_LIST_INPUT | GUC_REPORT
		},
		&datestyle_string,
		"ISO, MDY",
		check_datestyle, assign_datestyle, NULL
	},

	{
		{"default_table_access_method", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the default table access method for new tables."),
			NULL,
			GUC_IS_NAME
		},
		&default_table_access_method,
		DEFAULT_TABLE_ACCESS_METHOD,
		check_default_table_access_method, NULL, NULL
	},

	{
		{"default_tablespace", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the default tablespace to create tables and indexes in."),
			gettext_noop("An empty string selects the database's default tablespace."),
			GUC_IS_NAME
		},
		&default_tablespace,
		"",
		check_default_tablespace, NULL, NULL
	},

	{
		{"temp_tablespaces", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the tablespace(s) to use for temporary tables and sort files."),
			NULL,
			GUC_LIST_INPUT | GUC_LIST_QUOTE
		},
		&temp_tablespaces,
		"",
		check_temp_tablespaces, assign_temp_tablespaces, NULL
	},

	{
		{"createrole_self_grant", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets whether a CREATEROLE user automatically grants "
						 "the role to themselves, and with which options."),
			NULL,
			GUC_LIST_INPUT
		},
		&createrole_self_grant,
		"",
		check_createrole_self_grant, assign_createrole_self_grant, NULL
	},

	{
		{"dynamic_library_path", PGC_SUSET, CLIENT_CONN_OTHER,
			gettext_noop("Sets the path for dynamically loadable modules."),
			gettext_noop("If a dynamically loadable module needs to be opened and "
						 "the specified name does not have a directory component (i.e., the "
						 "name does not contain a slash), the system will search this path for "
						 "the specified file."),
			GUC_SUPERUSER_ONLY
		},
		&Dynamic_library_path,
		"$libdir",
		NULL, NULL, NULL
	},

	{
		{"krb_server_keyfile", PGC_SIGHUP, CONN_AUTH_AUTH,
			gettext_noop("Sets the location of the Kerberos server key file."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&pg_krb_server_keyfile,
		PG_KRB_SRVTAB,
		NULL, NULL, NULL
	},

	{
		{"bonjour_name", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the Bonjour service name."),
			NULL
		},
		&bonjour_name,
		"",
		NULL, NULL, NULL
	},

	{
		{"lc_messages", PGC_SUSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the language in which messages are displayed."),
			NULL
		},
		&locale_messages,
		"",
		check_locale_messages, assign_locale_messages, NULL
	},

	{
		{"lc_monetary", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the locale for formatting monetary amounts."),
			NULL
		},
		&locale_monetary,
		"C",
		check_locale_monetary, assign_locale_monetary, NULL
	},

	{
		{"lc_numeric", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the locale for formatting numbers."),
			NULL
		},
		&locale_numeric,
		"C",
		check_locale_numeric, assign_locale_numeric, NULL
	},

	{
		{"lc_time", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the locale for formatting date and time values."),
			NULL
		},
		&locale_time,
		"C",
		check_locale_time, assign_locale_time, NULL
	},

	{
		{"session_preload_libraries", PGC_SUSET, CLIENT_CONN_PRELOAD,
			gettext_noop("Lists shared libraries to preload into each backend."),
			NULL,
			GUC_LIST_INPUT | GUC_LIST_QUOTE | GUC_SUPERUSER_ONLY
		},
		&session_preload_libraries_string,
		"",
		NULL, NULL, NULL
	},

	{
		{"shared_preload_libraries", PGC_POSTMASTER, CLIENT_CONN_PRELOAD,
			gettext_noop("Lists shared libraries to preload into server."),
			NULL,
			GUC_LIST_INPUT | GUC_LIST_QUOTE | GUC_SUPERUSER_ONLY
		},
		&shared_preload_libraries_string,
		"",
		NULL, NULL, NULL
	},

	{
		{"local_preload_libraries", PGC_USERSET, CLIENT_CONN_PRELOAD,
			gettext_noop("Lists unprivileged shared libraries to preload into each backend."),
			NULL,
			GUC_LIST_INPUT | GUC_LIST_QUOTE
		},
		&local_preload_libraries_string,
		"",
		NULL, NULL, NULL
	},

	{
		{"search_path", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the schema search order for names that are not schema-qualified."),
			NULL,
			GUC_LIST_INPUT | GUC_LIST_QUOTE | GUC_EXPLAIN | GUC_REPORT
		},
		&namespace_search_path,
		"\"$user\", public",
		check_search_path, assign_search_path, NULL
	},

	{
		/* Can't be set in postgresql.conf */
		{"server_encoding", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the server (database) character set encoding."),
			NULL,
			GUC_IS_NAME | GUC_REPORT | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&server_encoding_string,
		"SQL_ASCII",
		NULL, NULL, NULL
	},

	{
		/* Can't be set in postgresql.conf */
		{"server_version", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the server version."),
			NULL,
			GUC_REPORT | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&server_version_string,
		PG_VERSION,
		NULL, NULL, NULL
	},

	{
		/* Not for general use --- used by SET ROLE */
		{"role", PGC_USERSET, UNGROUPED,
			gettext_noop("Sets the current role."),
			NULL,
			GUC_IS_NAME | GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | GUC_NOT_WHILE_SEC_REST
		},
		&role_string,
		"none",
		check_role, assign_role, show_role
	},

	{
		/* Not for general use --- used by SET SESSION AUTHORIZATION */
		{"session_authorization", PGC_USERSET, UNGROUPED,
			gettext_noop("Sets the session user name."),
			NULL,
			GUC_IS_NAME | GUC_REPORT | GUC_NO_SHOW_ALL | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE | GUC_NOT_WHILE_SEC_REST
		},
		&session_authorization_string,
		NULL,
		check_session_authorization, assign_session_authorization, NULL
	},

	{
		{"log_destination", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Sets the destination for server log output."),
			gettext_noop("Valid values are combinations of \"stderr\", "
						 "\"syslog\", \"csvlog\", \"jsonlog\", and \"eventlog\", "
						 "depending on the platform."),
			GUC_LIST_INPUT
		},
		&Log_destination_string,
		"stderr",
		check_log_destination, assign_log_destination, NULL
	},
	{
		{"log_directory", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Sets the destination directory for log files."),
			gettext_noop("Can be specified as relative to the data directory "
						 "or as absolute path."),
			GUC_SUPERUSER_ONLY
		},
		&Log_directory,
		"log",
		check_canonical_path, NULL, NULL
	},
	{
		{"log_filename", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Sets the file name pattern for log files."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&Log_filename,
		"postgresql-%Y-%m-%d_%H%M%S.log",
		NULL, NULL, NULL
	},

	{
		{"syslog_ident", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Sets the program name used to identify PostgreSQL "
						 "messages in syslog."),
			NULL
		},
		&syslog_ident_str,
		"postgres",
		NULL, assign_syslog_ident, NULL
	},

	{
		{"event_source", PGC_POSTMASTER, LOGGING_WHERE,
			gettext_noop("Sets the application name used to identify "
						 "PostgreSQL messages in the event log."),
			NULL
		},
		&event_source,
		DEFAULT_EVENT_SOURCE,
		NULL, NULL, NULL
	},

	{
		{"TimeZone", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the time zone for displaying and interpreting time stamps."),
			NULL,
			GUC_REPORT
		},
		&timezone_string,
		"GMT",
		check_timezone, assign_timezone, show_timezone
	},
	{
		{"timezone_abbreviations", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Selects a file of time zone abbreviations."),
			NULL
		},
		&timezone_abbreviations_string,
		NULL,
		check_timezone_abbreviations, assign_timezone_abbreviations, NULL
	},

	{
		{"unix_socket_group", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the owning group of the Unix-domain socket."),
			gettext_noop("The owning user of the socket is always the user "
						 "that starts the server.")
		},
		&Unix_socket_group,
		"",
		NULL, NULL, NULL
	},

	{
		{"unix_socket_directories", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the directories where Unix-domain sockets will be created."),
			NULL,
			GUC_LIST_INPUT | GUC_LIST_QUOTE | GUC_SUPERUSER_ONLY
		},
		&Unix_socket_directories,
		DEFAULT_PGSOCKET_DIR,
		NULL, NULL, NULL
	},

	{
		{"listen_addresses", PGC_POSTMASTER, CONN_AUTH_SETTINGS,
			gettext_noop("Sets the host name or IP address(es) to listen to."),
			NULL,
			GUC_LIST_INPUT
		},
		&ListenAddresses,
		"localhost",
		NULL, NULL, NULL
	},

	{
		/*
		 * Can't be set by ALTER SYSTEM as it can lead to recursive definition
		 * of data_directory.
		 */
		{"data_directory", PGC_POSTMASTER, FILE_LOCATIONS,
			gettext_noop("Sets the server's data directory."),
			NULL,
			GUC_SUPERUSER_ONLY | GUC_DISALLOW_IN_AUTO_FILE
		},
		&data_directory,
		NULL,
		NULL, NULL, NULL
	},

	{
		{"config_file", PGC_POSTMASTER, FILE_LOCATIONS,
			gettext_noop("Sets the server's main configuration file."),
			NULL,
			GUC_DISALLOW_IN_FILE | GUC_SUPERUSER_ONLY
		},
		&ConfigFileName,
		NULL,
		NULL, NULL, NULL
	},

	{
		{"hba_file", PGC_POSTMASTER, FILE_LOCATIONS,
			gettext_noop("Sets the server's \"hba\" configuration file."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&HbaFileName,
		NULL,
		NULL, NULL, NULL
	},

	{
		{"ident_file", PGC_POSTMASTER, FILE_LOCATIONS,
			gettext_noop("Sets the server's \"ident\" configuration file."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&IdentFileName,
		NULL,
		NULL, NULL, NULL
	},

	{
		{"external_pid_file", PGC_POSTMASTER, FILE_LOCATIONS,
			gettext_noop("Writes the postmaster PID to the specified file."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&external_pid_file,
		NULL,
		check_canonical_path, NULL, NULL
	},

	{
		{"ssl_library", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Shows the name of the SSL library."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&ssl_library,
#ifdef USE_SSL
		"OpenSSL",
#else
		"",
#endif
		NULL, NULL, NULL
	},

	{
		{"ssl_cert_file", PGC_SIGHUP, CONN_AUTH_SSL,
			gettext_noop("Location of the SSL server certificate file."),
			NULL
		},
		&ssl_cert_file,
		"server.crt",
		NULL, NULL, NULL
	},

	{
		{"ssl_key_file", PGC_SIGHUP, CONN_AUTH_SSL,
			gettext_noop("Location of the SSL server private key file."),
			NULL
		},
		&ssl_key_file,
		"server.key",
		NULL, NULL, NULL
	},

	{
		{"ssl_ca_file", PGC_SIGHUP, CONN_AUTH_SSL,
			gettext_noop("Location of the SSL certificate authority file."),
			NULL
		},
		&ssl_ca_file,
		"",
		NULL, NULL, NULL
	},

	{
		{"ssl_crl_file", PGC_SIGHUP, CONN_AUTH_SSL,
			gettext_noop("Location of the SSL certificate revocation list file."),
			NULL
		},
		&ssl_crl_file,
		"",
		NULL, NULL, NULL
	},

	{
		{"ssl_crl_dir", PGC_SIGHUP, CONN_AUTH_SSL,
			gettext_noop("Location of the SSL certificate revocation list directory."),
			NULL
		},
		&ssl_crl_dir,
		"",
		NULL, NULL, NULL
	},

	{
		{"synchronous_standby_names", PGC_SIGHUP, REPLICATION_PRIMARY,
			gettext_noop("Number of synchronous standbys and list of names of potential synchronous ones."),
			NULL,
			GUC_LIST_INPUT
		},
		&SyncRepStandbyNames,
		"",
		check_synchronous_standby_names, assign_synchronous_standby_names, NULL
	},

	{
		{"default_text_search_config", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets default text search configuration."),
			NULL
		},
		&TSCurrentConfig,
		"pg_catalog.simple",
		check_default_text_search_config, assign_default_text_search_config, NULL
	},

	{
		{"ssl_ciphers", PGC_SIGHUP, CONN_AUTH_SSL,
			gettext_noop("Sets the list of allowed SSL ciphers."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&SSLCipherSuites,
#ifdef USE_OPENSSL
		"HIGH:MEDIUM:+3DES:!aNULL",
#else
		"none",
#endif
		NULL, NULL, NULL
	},

	{
		{"ssl_ecdh_curve", PGC_SIGHUP, CONN_AUTH_SSL,
			gettext_noop("Sets the curve to use for ECDH."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&SSLECDHCurve,
#ifdef USE_SSL
		"prime256v1",
#else
		"none",
#endif
		NULL, NULL, NULL
	},

	{
		{"ssl_dh_params_file", PGC_SIGHUP, CONN_AUTH_SSL,
			gettext_noop("Location of the SSL DH parameters file."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&ssl_dh_params_file,
		"",
		NULL, NULL, NULL
	},

	{
		{"ssl_passphrase_command", PGC_SIGHUP, CONN_AUTH_SSL,
			gettext_noop("Command to obtain passphrases for SSL."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&ssl_passphrase_command,
		"",
		NULL, NULL, NULL
	},

	{
		{"application_name", PGC_USERSET, LOGGING_WHAT,
			gettext_noop("Sets the application name to be reported in statistics and logs."),
			NULL,
			GUC_IS_NAME | GUC_REPORT | GUC_NOT_IN_SAMPLE
		},
		&application_name,
		"",
		check_application_name, assign_application_name, NULL
	},

	{
		{"cluster_name", PGC_POSTMASTER, PROCESS_TITLE,
			gettext_noop("Sets the name of the cluster, which is included in the process title."),
			NULL,
			GUC_IS_NAME
		},
		&cluster_name,
		"",
		check_cluster_name, NULL, NULL
	},

	{
		{"wal_consistency_checking", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Sets the WAL resource managers for which WAL consistency checks are done."),
			gettext_noop("Full-page images will be logged for all data blocks and cross-checked against the results of WAL replay."),
			GUC_LIST_INPUT | GUC_NOT_IN_SAMPLE
		},
		&wal_consistency_checking_string,
		"",
		check_wal_consistency_checking, assign_wal_consistency_checking, NULL
	},

	{
		{"jit_provider", PGC_POSTMASTER, CLIENT_CONN_PRELOAD,
			gettext_noop("JIT provider to use."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&jit_provider,
		"llvmjit",
		NULL, NULL, NULL
	},

	{
		{"backtrace_functions", PGC_SUSET, DEVELOPER_OPTIONS,
			gettext_noop("Log backtrace for errors in these functions."),
			NULL,
			GUC_NOT_IN_SAMPLE
		},
		&backtrace_functions,
		"",
		check_backtrace_functions, assign_backtrace_functions, NULL
	},

	{
		{"debug_io_direct", PGC_POSTMASTER, DEVELOPER_OPTIONS,
			gettext_noop("Use direct I/O for file access."),
			NULL,
			GUC_LIST_INPUT | GUC_NOT_IN_SAMPLE
		},
		&debug_io_direct_string,
		"",
		check_debug_io_direct, assign_debug_io_direct, NULL
	},

	{
		{"synchronized_standby_slots", PGC_SIGHUP, REPLICATION_PRIMARY,
			gettext_noop("Lists streaming replication standby server replication slot "
						 "names that logical WAL sender processes will wait for."),
			gettext_noop("Logical WAL sender processes will send decoded "
						 "changes to output plugins only after the specified "
						 "replication slots have confirmed receiving WAL."),
			GUC_LIST_INPUT
		},
		&synchronized_standby_slots,
		"",
		check_synchronized_standby_slots, assign_synchronized_standby_slots, NULL
	},

	{
		{"restrict_nonsystem_relation_kind", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Prohibits access to non-system relations of specified kinds."),
			NULL,
			GUC_LIST_INPUT | GUC_NOT_IN_SAMPLE
		},
		&restrict_nonsystem_relation_kind_string,
		"",
		check_restrict_nonsystem_relation_kind, assign_restrict_nonsystem_relation_kind, NULL
	},

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, NULL, NULL, NULL, NULL
	}
};


struct config_enum ConfigureNamesEnum[] =
{
	{
		{"backslash_quote", PGC_USERSET, COMPAT_OPTIONS_PREVIOUS,
			gettext_noop("Sets whether \"\\'\" is allowed in string literals."),
			NULL
		},
		&backslash_quote,
		BACKSLASH_QUOTE_SAFE_ENCODING, backslash_quote_options,
		NULL, NULL, NULL
	},

	{
		{"bytea_output", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the output format for bytea."),
			NULL
		},
		&bytea_output,
		BYTEA_OUTPUT_HEX, bytea_output_options,
		NULL, NULL, NULL
	},

	{
		{"client_min_messages", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the message levels that are sent to the client."),
			gettext_noop("Each level includes all the levels that follow it. The later"
						 " the level, the fewer messages are sent.")
		},
		&client_min_messages,
		NOTICE, client_message_level_options,
		NULL, NULL, NULL
	},

	{
		{"compute_query_id", PGC_SUSET, STATS_MONITORING,
			gettext_noop("Enables in-core computation of query identifiers."),
			NULL
		},
		&compute_query_id,
		COMPUTE_QUERY_ID_AUTO, compute_query_id_options,
		NULL, NULL, NULL
	},

	{
		{"constraint_exclusion", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Enables the planner to use constraints to optimize queries."),
			gettext_noop("Table scans will be skipped if their constraints"
						 " guarantee that no rows match the query."),
			GUC_EXPLAIN
		},
		&constraint_exclusion,
		CONSTRAINT_EXCLUSION_PARTITION, constraint_exclusion_options,
		NULL, NULL, NULL
	},

	{
		{"default_toast_compression", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the default compression method for compressible values."),
			NULL
		},
		&default_toast_compression,
		TOAST_PGLZ_COMPRESSION,
		default_toast_compression_options,
		NULL, NULL, NULL
	},

	{
		{"default_transaction_isolation", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the transaction isolation level of each new transaction."),
			NULL
		},
		&DefaultXactIsoLevel,
		XACT_READ_COMMITTED, isolation_level_options,
		NULL, NULL, NULL
	},

	{
		{"transaction_isolation", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the current transaction's isolation level."),
			NULL,
			GUC_NO_RESET | GUC_NO_RESET_ALL | GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&XactIsoLevel,
		XACT_READ_COMMITTED, isolation_level_options,
		check_transaction_isolation, NULL, NULL
	},

	{
		{"IntervalStyle", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Sets the display format for interval values."),
			NULL,
			GUC_REPORT
		},
		&IntervalStyle,
		INTSTYLE_POSTGRES, intervalstyle_options,
		NULL, NULL, NULL
	},

	{
		{"icu_validation_level", PGC_USERSET, CLIENT_CONN_LOCALE,
			gettext_noop("Log level for reporting invalid ICU locale strings."),
			NULL
		},
		&icu_validation_level,
		WARNING, icu_validation_level_options,
		NULL, NULL, NULL
	},

	{
		{"log_error_verbosity", PGC_SUSET, LOGGING_WHAT,
			gettext_noop("Sets the verbosity of logged messages."),
			NULL
		},
		&Log_error_verbosity,
		PGERROR_DEFAULT, log_error_verbosity_options,
		NULL, NULL, NULL
	},

	{
		{"log_min_messages", PGC_SUSET, LOGGING_WHEN,
			gettext_noop("Sets the message levels that are logged."),
			gettext_noop("Each level includes all the levels that follow it. The later"
						 " the level, the fewer messages are sent.")
		},
		&log_min_messages,
		WARNING, server_message_level_options,
		NULL, NULL, NULL
	},

	{
		{"log_min_error_statement", PGC_SUSET, LOGGING_WHEN,
			gettext_noop("Causes all statements generating error at or above this level to be logged."),
			gettext_noop("Each level includes all the levels that follow it. The later"
						 " the level, the fewer messages are sent.")
		},
		&log_min_error_statement,
		ERROR, server_message_level_options,
		NULL, NULL, NULL
	},

	{
		{"log_statement", PGC_SUSET, LOGGING_WHAT,
			gettext_noop("Sets the type of statements logged."),
			NULL
		},
		&log_statement,
		LOGSTMT_NONE, log_statement_options,
		NULL, NULL, NULL
	},

	{
		{"syslog_facility", PGC_SIGHUP, LOGGING_WHERE,
			gettext_noop("Sets the syslog \"facility\" to be used when syslog enabled."),
			NULL
		},
		&syslog_facility,
		DEFAULT_SYSLOG_FACILITY,
		syslog_facility_options,
		NULL, assign_syslog_facility, NULL
	},

	{
		{"session_replication_role", PGC_SUSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets the session's behavior for triggers and rewrite rules."),
			NULL
		},
		&SessionReplicationRole,
		SESSION_REPLICATION_ROLE_ORIGIN, session_replication_role_options,
		NULL, assign_session_replication_role, NULL
	},

	{
		{"synchronous_commit", PGC_USERSET, WAL_SETTINGS,
			gettext_noop("Sets the current transaction's synchronization level."),
			NULL
		},
		&synchronous_commit,
		SYNCHRONOUS_COMMIT_ON, synchronous_commit_options,
		NULL, assign_synchronous_commit, NULL
	},

	{
		{"archive_mode", PGC_POSTMASTER, WAL_ARCHIVING,
			gettext_noop("Allows archiving of WAL files using \"archive_command\"."),
			NULL
		},
		&XLogArchiveMode,
		ARCHIVE_MODE_OFF, archive_mode_options,
		NULL, NULL, NULL
	},

	{
		{"recovery_target_action", PGC_POSTMASTER, WAL_RECOVERY_TARGET,
			gettext_noop("Sets the action to perform upon reaching the recovery target."),
			NULL
		},
		&recoveryTargetAction,
		RECOVERY_TARGET_ACTION_PAUSE, recovery_target_action_options,
		NULL, NULL, NULL
	},

	{
		{"track_functions", PGC_SUSET, STATS_CUMULATIVE,
			gettext_noop("Collects function-level statistics on database activity."),
			NULL
		},
		&pgstat_track_functions,
		TRACK_FUNC_OFF, track_function_options,
		NULL, NULL, NULL
	},


	{
		{"stats_fetch_consistency", PGC_USERSET, STATS_CUMULATIVE,
			gettext_noop("Sets the consistency of accesses to statistics data."),
			NULL
		},
		&pgstat_fetch_consistency,
		PGSTAT_FETCH_CONSISTENCY_CACHE, stats_fetch_consistency,
		NULL, assign_stats_fetch_consistency, NULL
	},

	{
		{"wal_compression", PGC_SUSET, WAL_SETTINGS,
			gettext_noop("Compresses full-page writes written in WAL file with specified method."),
			NULL
		},
		&wal_compression,
		WAL_COMPRESSION_NONE, wal_compression_options,
		NULL, NULL, NULL
	},

	{
		{"wal_level", PGC_POSTMASTER, WAL_SETTINGS,
			gettext_noop("Sets the level of information written to the WAL."),
			NULL
		},
		&wal_level,
		WAL_LEVEL_REPLICA, wal_level_options,
		NULL, NULL, NULL
	},

	{
		{"dynamic_shared_memory_type", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Selects the dynamic shared memory implementation used."),
			NULL
		},
		&dynamic_shared_memory_type,
		DEFAULT_DYNAMIC_SHARED_MEMORY_TYPE, dynamic_shared_memory_options,
		NULL, NULL, NULL
	},

	{
		{"shared_memory_type", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Selects the shared memory implementation used for the main shared memory region."),
			NULL
		},
		&shared_memory_type,
		DEFAULT_SHARED_MEMORY_TYPE, shared_memory_options,
		NULL, NULL, NULL
	},

	{
		{"wal_sync_method", PGC_SIGHUP, WAL_SETTINGS,
			gettext_noop("Selects the method used for forcing WAL updates to disk."),
			NULL
		},
		&wal_sync_method,
		DEFAULT_WAL_SYNC_METHOD, wal_sync_method_options,
		NULL, assign_wal_sync_method, NULL
	},

	{
		{"xmlbinary", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets how binary values are to be encoded in XML."),
			NULL
		},
		&xmlbinary,
		XMLBINARY_BASE64, xmlbinary_options,
		NULL, NULL, NULL
	},

	{
		{"xmloption", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Sets whether XML data in implicit parsing and serialization "
						 "operations is to be considered as documents or content fragments."),
			NULL
		},
		&xmloption,
		XMLOPTION_CONTENT, xmloption_options,
		NULL, NULL, NULL
	},

	{
		{"huge_pages", PGC_POSTMASTER, RESOURCES_MEM,
			gettext_noop("Use of huge pages on Linux or Windows."),
			NULL
		},
		&huge_pages,
		HUGE_PAGES_TRY, huge_pages_options,
		NULL, NULL, NULL
	},

	{
		{"huge_pages_status", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Indicates the status of huge pages."),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&huge_pages_status,
		HUGE_PAGES_UNKNOWN, huge_pages_status_options,
		NULL, NULL, NULL
	},

	{
		{"recovery_prefetch", PGC_SIGHUP, WAL_RECOVERY,
			gettext_noop("Prefetch referenced blocks during recovery."),
			gettext_noop("Look ahead in the WAL to find references to uncached data.")
		},
		&recovery_prefetch,
		RECOVERY_PREFETCH_TRY, recovery_prefetch_options,
		check_recovery_prefetch, assign_recovery_prefetch, NULL
	},

	{
		{"debug_parallel_query", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Forces the planner's use parallel query nodes."),
			gettext_noop("This can be useful for testing the parallel query infrastructure "
						 "by forcing the planner to generate plans that contain nodes "
						 "that perform tuple communication between workers and the main process."),
			GUC_NOT_IN_SAMPLE | GUC_EXPLAIN
		},
		&debug_parallel_query,
		DEBUG_PARALLEL_OFF, debug_parallel_query_options,
		NULL, NULL, NULL
	},

	{
		{"password_encryption", PGC_USERSET, CONN_AUTH_AUTH,
			gettext_noop("Chooses the algorithm for encrypting passwords."),
			NULL
		},
		&Password_encryption,
		PASSWORD_TYPE_SCRAM_SHA_256, password_encryption_options,
		NULL, NULL, NULL
	},

	{
		{"plan_cache_mode", PGC_USERSET, QUERY_TUNING_OTHER,
			gettext_noop("Controls the planner's selection of custom or generic plan."),
			gettext_noop("Prepared statements can have custom and generic plans, and the planner "
						 "will attempt to choose which is better.  This can be set to override "
						 "the default behavior."),
			GUC_EXPLAIN
		},
		&plan_cache_mode,
		PLAN_CACHE_MODE_AUTO, plan_cache_mode_options,
		NULL, NULL, NULL
	},

	{
		{"ssl_min_protocol_version", PGC_SIGHUP, CONN_AUTH_SSL,
			gettext_noop("Sets the minimum SSL/TLS protocol version to use."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&ssl_min_protocol_version,
		PG_TLS1_2_VERSION,
		ssl_protocol_versions_info + 1, /* don't allow PG_TLS_ANY */
		NULL, NULL, NULL
	},

	{
		{"ssl_max_protocol_version", PGC_SIGHUP, CONN_AUTH_SSL,
			gettext_noop("Sets the maximum SSL/TLS protocol version to use."),
			NULL,
			GUC_SUPERUSER_ONLY
		},
		&ssl_max_protocol_version,
		PG_TLS_ANY,
		ssl_protocol_versions_info,
		NULL, NULL, NULL
	},

	{
		{"recovery_init_sync_method", PGC_SIGHUP, ERROR_HANDLING_OPTIONS,
			gettext_noop("Sets the method for synchronizing the data directory before crash recovery."),
		},
		&recovery_init_sync_method,
		DATA_DIR_SYNC_METHOD_FSYNC, recovery_init_sync_method_options,
		NULL, NULL, NULL
	},

	{
		{"debug_logical_replication_streaming", PGC_USERSET, DEVELOPER_OPTIONS,
			gettext_noop("Forces immediate streaming or serialization of changes in large transactions."),
			gettext_noop("On the publisher, it allows streaming or serializing each change in logical decoding. "
						 "On the subscriber, it allows serialization of all changes to files and notifies the "
						 "parallel apply workers to read and apply them at the end of the transaction."),
			GUC_NOT_IN_SAMPLE
		},
		&debug_logical_replication_streaming,
		DEBUG_LOGICAL_REP_STREAMING_BUFFERED, debug_logical_replication_streaming_options,
		NULL, NULL, NULL
	},

	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, 0, NULL, NULL, NULL, NULL
	}
};
