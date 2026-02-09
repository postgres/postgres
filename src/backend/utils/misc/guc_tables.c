/*--------------------------------------------------------------------
 *
 * guc_tables.c
 *
 * Static tables for the Grand Unified Configuration scheme.
 *
 * Many of these tables are const.  However, ConfigureNames[] is not, because
 * the structs in it are actually the live per-variable state data that guc.c
 * manipulates.  While many of their fields are intended to be constant, some
 * fields change at runtime.
 *
 *
 * Copyright (c) 2000-2026, PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/guc_tables.c
 *
 *--------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef HAVE_COPYFILE_H
#include <copyfile.h>
#endif
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
#include "commands/extension.h"
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
#include "libpq/oauth.h"
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
#include "storage/aio.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/io_worker.h"
#include "storage/large_object.h"
#include "storage/pg_shmem.h"
#include "storage/predicate.h"
#include "storage/procnumber.h"
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

const struct config_enum_entry server_message_level_options[] = {
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

static const struct config_enum_entry file_copy_method_options[] = {
	{"copy", FILE_COPY_METHOD_COPY, false},
#if defined(HAVE_COPYFILE) && defined(COPYFILE_CLONE_FORCE) || defined(HAVE_COPY_FILE_RANGE)
	{"clone", FILE_COPY_METHOD_CLONE, false},
#endif
	{NULL, 0, false}
};

static const struct config_enum_entry file_extend_method_options[] = {
#ifdef HAVE_POSIX_FALLOCATE
	{"posix_fallocate", FILE_EXTEND_METHOD_POSIX_FALLOCATE, false},
#endif
	{"write_zeros", FILE_EXTEND_METHOD_WRITE_ZEROS, false},
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
bool		Debug_print_raw_parse = false;
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
 * These GUCs exist solely for backward compatibility.
 */
static bool default_with_oids = false;
static bool standard_conforming_strings = true;

bool		current_role_is_superuser;

int			log_min_error_statement = ERROR;
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
int			huge_pages_status = HUGE_PAGES_UNKNOWN;

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
static char *log_min_messages_string;

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
static int	effective_wal_level = WAL_LEVEL_REPLICA;
static bool data_checksums;
static bool integer_datetimes;

#ifdef USE_ASSERT_CHECKING
#define DEFAULT_ASSERT_ENABLED true
#else
#define DEFAULT_ASSERT_ENABLED false
#endif
static bool assert_enabled = DEFAULT_ASSERT_ENABLED;

#ifdef EXEC_BACKEND
#define EXEC_BACKEND_ENABLED true
#else
#define EXEC_BACKEND_ENABLED false
#endif
static bool exec_backend_enabled = EXEC_BACKEND_ENABLED;

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
 * set default log_min_messages to WARNING for all process types
 */
int			log_min_messages[] = {
#define PG_PROCTYPE(bktype, bkcategory, description, main_func, shmem_attach) \
	[bktype] = WARNING,
#include "postmaster/proctypelist.h"
#undef PG_PROCTYPE
};

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
	[RESOURCES_BGWRITER] = gettext_noop("Resource Usage / Background Writer"),
	[RESOURCES_IO] = gettext_noop("Resource Usage / I/O"),
	[RESOURCES_WORKER_PROCESSES] = gettext_noop("Resource Usage / Worker Processes"),
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
	[VACUUM_AUTOVACUUM] = gettext_noop("Vacuuming / Automatic Vacuuming"),
	[VACUUM_COST_DELAY] = gettext_noop("Vacuuming / Cost-Based Vacuum Delay"),
	[VACUUM_DEFAULT] = gettext_noop("Vacuuming / Default Behavior"),
	[VACUUM_FREEZING] = gettext_noop("Vacuuming / Freezing"),
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


#include "utils/guc_tables.inc.c"
