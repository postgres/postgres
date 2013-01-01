/*
 * xlog_fn.h
 *
 * PostgreSQL transaction log SQL-callable function declarations
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlog_fn.h
 */
#ifndef XLOG_FN_H
#define XLOG_FN_H

#include "fmgr.h"

extern Datum pg_start_backup(PG_FUNCTION_ARGS);
extern Datum pg_stop_backup(PG_FUNCTION_ARGS);
extern Datum pg_switch_xlog(PG_FUNCTION_ARGS);
extern Datum pg_create_restore_point(PG_FUNCTION_ARGS);
extern Datum pg_current_xlog_location(PG_FUNCTION_ARGS);
extern Datum pg_current_xlog_insert_location(PG_FUNCTION_ARGS);
extern Datum pg_last_xlog_receive_location(PG_FUNCTION_ARGS);
extern Datum pg_last_xlog_replay_location(PG_FUNCTION_ARGS);
extern Datum pg_last_xact_replay_timestamp(PG_FUNCTION_ARGS);
extern Datum pg_xlogfile_name_offset(PG_FUNCTION_ARGS);
extern Datum pg_xlogfile_name(PG_FUNCTION_ARGS);
extern Datum pg_is_in_recovery(PG_FUNCTION_ARGS);
extern Datum pg_xlog_replay_pause(PG_FUNCTION_ARGS);
extern Datum pg_xlog_replay_resume(PG_FUNCTION_ARGS);
extern Datum pg_is_xlog_replay_paused(PG_FUNCTION_ARGS);
extern Datum pg_xlog_location_diff(PG_FUNCTION_ARGS);
extern Datum pg_is_in_backup(PG_FUNCTION_ARGS);
extern Datum pg_backup_start_time(PG_FUNCTION_ARGS);

#endif   /* XLOG_FN_H */
