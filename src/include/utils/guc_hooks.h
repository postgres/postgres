/*-------------------------------------------------------------------------
 *
 * guc_hooks.h
 *		Declarations of per-variable callback functions used by GUC.
 *
 * These functions are scattered throughout the system, but we
 * declare them all here to avoid having to propagate guc.h into
 * a lot of unrelated header files.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 *	  src/include/utils/guc_hooks.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GUC_HOOKS_H
#define GUC_HOOKS_H 1

#include "utils/guc.h"

/*
 * See guc.h for the typedefs that these hook functions should match
 * (GucBoolCheckHook and so on).
 *
 * Please keep the declarations in order by GUC variable name.
 */

extern bool check_application_name(char **newval, void **extra,
								   GucSource source);
extern void assign_application_name(const char *newval, void *extra);
extern const char *show_archive_command(void);
extern bool check_autovacuum_work_mem(int *newval, void **extra,
									  GucSource source);
extern bool check_vacuum_buffer_usage_limit(int *newval, void **extra,
											GucSource source);
extern bool check_backtrace_functions(char **newval, void **extra,
									  GucSource source);
extern void assign_backtrace_functions(const char *newval, void *extra);
extern bool check_bonjour(bool *newval, void **extra, GucSource source);
extern bool check_canonical_path(char **newval, void **extra, GucSource source);
extern void assign_checkpoint_completion_target(double newval, void *extra);
extern bool check_client_connection_check_interval(int *newval, void **extra,
												   GucSource source);
extern bool check_client_encoding(char **newval, void **extra, GucSource source);
extern void assign_client_encoding(const char *newval, void *extra);
extern bool check_cluster_name(char **newval, void **extra, GucSource source);
extern bool check_commit_ts_buffers(int *newval, void **extra,
									GucSource source);
extern const char *show_data_directory_mode(void);
extern bool check_datestyle(char **newval, void **extra, GucSource source);
extern void assign_datestyle(const char *newval, void *extra);
extern bool check_debug_io_direct(char **newval, void **extra, GucSource source);
extern void assign_debug_io_direct(const char *newval, void *extra);
extern bool check_default_table_access_method(char **newval, void **extra,
											  GucSource source);
extern bool check_default_tablespace(char **newval, void **extra,
									 GucSource source);
extern bool check_default_text_search_config(char **newval, void **extra, GucSource source);
extern void assign_default_text_search_config(const char *newval, void *extra);
extern bool check_default_with_oids(bool *newval, void **extra,
									GucSource source);
extern bool check_effective_io_concurrency(int *newval, void **extra,
										   GucSource source);
extern bool check_huge_page_size(int *newval, void **extra, GucSource source);
extern const char *show_in_hot_standby(void);
extern bool check_locale_messages(char **newval, void **extra, GucSource source);
extern void assign_locale_messages(const char *newval, void *extra);
extern bool check_locale_monetary(char **newval, void **extra, GucSource source);
extern void assign_locale_monetary(const char *newval, void *extra);
extern bool check_locale_numeric(char **newval, void **extra, GucSource source);
extern void assign_locale_numeric(const char *newval, void *extra);
extern bool check_locale_time(char **newval, void **extra, GucSource source);
extern void assign_locale_time(const char *newval, void *extra);
extern bool check_log_destination(char **newval, void **extra,
								  GucSource source);
extern void assign_log_destination(const char *newval, void *extra);
extern const char *show_log_file_mode(void);
extern bool check_log_stats(bool *newval, void **extra, GucSource source);
extern bool check_log_timezone(char **newval, void **extra, GucSource source);
extern void assign_log_timezone(const char *newval, void *extra);
extern const char *show_log_timezone(void);
extern bool check_maintenance_io_concurrency(int *newval, void **extra,
											 GucSource source);
extern void assign_maintenance_io_concurrency(int newval, void *extra);
extern bool check_max_slot_wal_keep_size(int *newval, void **extra,
										 GucSource source);
extern void assign_max_wal_size(int newval, void *extra);
extern bool check_max_stack_depth(int *newval, void **extra, GucSource source);
extern void assign_max_stack_depth(int newval, void *extra);
extern bool check_multixact_member_buffers(int *newval, void **extra,
										   GucSource source);
extern bool check_multixact_offset_buffers(int *newval, void **extra,
										   GucSource source);
extern bool check_notify_buffers(int *newval, void **extra, GucSource source);
extern bool check_primary_slot_name(char **newval, void **extra,
									GucSource source);
extern bool check_random_seed(double *newval, void **extra, GucSource source);
extern void assign_random_seed(double newval, void *extra);
extern const char *show_random_seed(void);
extern bool check_recovery_prefetch(int *new_value, void **extra,
									GucSource source);
extern void assign_recovery_prefetch(int new_value, void *extra);
extern bool check_recovery_target(char **newval, void **extra,
								  GucSource source);
extern void assign_recovery_target(const char *newval, void *extra);
extern bool check_recovery_target_lsn(char **newval, void **extra,
									  GucSource source);
extern void assign_recovery_target_lsn(const char *newval, void *extra);
extern bool check_recovery_target_name(char **newval, void **extra,
									   GucSource source);
extern void assign_recovery_target_name(const char *newval, void *extra);
extern bool check_recovery_target_time(char **newval, void **extra,
									   GucSource source);
extern void assign_recovery_target_time(const char *newval, void *extra);
extern bool check_recovery_target_timeline(char **newval, void **extra,
										   GucSource source);
extern void assign_recovery_target_timeline(const char *newval, void *extra);
extern bool check_recovery_target_xid(char **newval, void **extra,
									  GucSource source);
extern void assign_recovery_target_xid(const char *newval, void *extra);
extern bool check_role(char **newval, void **extra, GucSource source);
extern void assign_role(const char *newval, void *extra);
extern const char *show_role(void);
extern bool check_restrict_nonsystem_relation_kind(char **newval, void **extra,
												   GucSource source);
extern void assign_restrict_nonsystem_relation_kind(const char *newval, void *extra);
extern bool check_search_path(char **newval, void **extra, GucSource source);
extern void assign_search_path(const char *newval, void *extra);
extern bool check_serial_buffers(int *newval, void **extra, GucSource source);
extern bool check_session_authorization(char **newval, void **extra, GucSource source);
extern void assign_session_authorization(const char *newval, void *extra);
extern void assign_session_replication_role(int newval, void *extra);
extern void assign_stats_fetch_consistency(int newval, void *extra);
extern bool check_ssl(bool *newval, void **extra, GucSource source);
extern bool check_stage_log_stats(bool *newval, void **extra, GucSource source);
extern bool check_subtrans_buffers(int *newval, void **extra,
								   GucSource source);
extern bool check_synchronous_standby_names(char **newval, void **extra,
											GucSource source);
extern void assign_synchronous_standby_names(const char *newval, void *extra);
extern void assign_synchronous_commit(int newval, void *extra);
extern void assign_syslog_facility(int newval, void *extra);
extern void assign_syslog_ident(const char *newval, void *extra);
extern void assign_tcp_keepalives_count(int newval, void *extra);
extern const char *show_tcp_keepalives_count(void);
extern void assign_tcp_keepalives_idle(int newval, void *extra);
extern const char *show_tcp_keepalives_idle(void);
extern void assign_tcp_keepalives_interval(int newval, void *extra);
extern const char *show_tcp_keepalives_interval(void);
extern void assign_tcp_user_timeout(int newval, void *extra);
extern const char *show_tcp_user_timeout(void);
extern bool check_temp_buffers(int *newval, void **extra, GucSource source);
extern bool check_temp_tablespaces(char **newval, void **extra,
								   GucSource source);
extern void assign_temp_tablespaces(const char *newval, void *extra);
extern bool check_timezone(char **newval, void **extra, GucSource source);
extern void assign_timezone(const char *newval, void *extra);
extern const char *show_timezone(void);
extern bool check_timezone_abbreviations(char **newval, void **extra,
										 GucSource source);
extern void assign_timezone_abbreviations(const char *newval, void *extra);
extern bool check_transaction_buffers(int *newval, void **extra, GucSource source);
extern bool check_transaction_deferrable(bool *newval, void **extra, GucSource source);
extern bool check_transaction_isolation(int *newval, void **extra, GucSource source);
extern bool check_transaction_read_only(bool *newval, void **extra, GucSource source);
extern void assign_transaction_timeout(int newval, void *extra);
extern const char *show_unix_socket_permissions(void);
extern bool check_wal_buffers(int *newval, void **extra, GucSource source);
extern bool check_wal_consistency_checking(char **newval, void **extra,
										   GucSource source);
extern void assign_wal_consistency_checking(const char *newval, void *extra);
extern bool check_wal_segment_size(int *newval, void **extra, GucSource source);
extern void assign_wal_sync_method(int new_wal_sync_method, void *extra);
extern bool check_synchronized_standby_slots(char **newval, void **extra,
											 GucSource source);
extern void assign_synchronized_standby_slots(const char *newval, void *extra);

#endif							/* GUC_HOOKS_H */
