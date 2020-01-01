/*-------------------------------------------------------------------------
 *
 * pg_backup_utils.h
 *	Utility routines shared by pg_dump and pg_restore.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/pg_backup_utils.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_BACKUP_UTILS_H
#define PG_BACKUP_UTILS_H

#include "common/logging.h"

typedef enum					/* bits returned by set_dump_section */
{
	DUMP_PRE_DATA = 0x01,
	DUMP_DATA = 0x02,
	DUMP_POST_DATA = 0x04,
	DUMP_UNSECTIONED = 0xff
} DumpSections;

typedef void (*on_exit_nicely_callback) (int code, void *arg);

extern const char *progname;

extern void set_dump_section(const char *arg, int *dumpSections);
extern void on_exit_nicely(on_exit_nicely_callback function, void *arg);
extern void exit_nicely(int code) pg_attribute_noreturn();

#define fatal(...) do { pg_log_error(__VA_ARGS__); exit_nicely(1); } while(0)

#endif							/* PG_BACKUP_UTILS_H */
