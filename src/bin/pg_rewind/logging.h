/*-------------------------------------------------------------------------
 *
 * logging.h
 *	 prototypes for logging functions
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_REWIND_LOGGING_H
#define PG_REWIND_LOGGING_H

/* progress counters */
extern uint64 fetch_size;
extern uint64 fetch_done;

/*
 * Enumeration to denote pg_log modes
 */
typedef enum
{
	PG_DEBUG,
	PG_PROGRESS,
	PG_WARNING,
	PG_FATAL
} eLogType;

extern void pg_log(eLogType type, const char *fmt,...) pg_attribute_printf(2, 3);
extern void pg_fatal(const char *fmt,...) pg_attribute_printf(1, 2) pg_attribute_noreturn();

extern void progress_report(bool force);

#endif   /* PG_REWIND_LOGGING_H */
