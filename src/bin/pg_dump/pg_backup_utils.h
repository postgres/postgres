/*-------------------------------------------------------------------------
 *
 * pg_backup_utils.h
 *	Utility routines shared by pg_dump and pg_restore.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/pg_backup_utils.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_BACKUP_UTILS_H
#define PG_BACKUP_UTILS_H

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
extern void
write_msg(const char *modulename, const char *fmt,...)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 3)));
extern void
vwrite_msg(const char *modulename, const char *fmt, va_list ap)
__attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 0)));
extern void on_exit_nicely(on_exit_nicely_callback function, void *arg);
extern void exit_nicely(int code) __attribute__((noreturn));

#endif   /* PG_BACKUP_UTILS_H */
