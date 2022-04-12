/*-------------------------------------------------------------------------
 *
 * pg_backup_utils.h
 *	Utility routines shared by pg_dump and pg_restore.
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/pg_backup_utils.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_BACKUP_UTILS_H
#define PG_BACKUP_UTILS_H

#include "common/logging.h"

/* bits returned by set_dump_section */
#define DUMP_PRE_DATA		0x01
#define DUMP_DATA			0x02
#define DUMP_POST_DATA		0x04
#define DUMP_UNSECTIONED	0xff

typedef void (*on_exit_nicely_callback) (int code, void *arg);

extern const char *progname;

extern void set_dump_section(const char *arg, int *dumpSections);
extern void on_exit_nicely(on_exit_nicely_callback function, void *arg);
extern void exit_nicely(int code) pg_attribute_noreturn();

/* In pg_dump, we modify pg_fatal to call exit_nicely instead of exit */
#undef pg_fatal
#define pg_fatal(...) do { \
		pg_log_generic(PG_LOG_ERROR, PG_LOG_PRIMARY, __VA_ARGS__); \
		exit_nicely(1); \
	} while(0)

#endif							/* PG_BACKUP_UTILS_H */
