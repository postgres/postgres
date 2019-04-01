/*-------------------------------------------------------------------------
 *
 * logging.h
 *	 prototypes for logging functions
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_REWIND_LOGGING_H
#define PG_REWIND_LOGGING_H

#include "fe_utils/logging.h"

/* progress counters */
extern uint64 fetch_size;
extern uint64 fetch_done;

extern void progress_report(bool force);

#define pg_fatal(...) do { pg_log_fatal(__VA_ARGS__); exit(1); } while(0)

#endif							/* PG_REWIND_LOGGING_H */
