/*-------------------------------------------------------------------------
 *
 * pgarch.h
 *	  Exports from postmaster/pgarch.c.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/pgarch.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PGARCH_H
#define _PGARCH_H

/* ----------
 * Archiver control info.
 *
 * We expect that archivable files within pg_wal will have names between
 * MIN_XFN_CHARS and MAX_XFN_CHARS in length, consisting only of characters
 * appearing in VALID_XFN_CHARS.  The status files in archive_status have
 * corresponding names with ".ready" or ".done" appended.
 * ----------
 */
#define MIN_XFN_CHARS	16
#define MAX_XFN_CHARS	40
#define VALID_XFN_CHARS "0123456789ABCDEF.history.backup.partial"

extern Size PgArchShmemSize(void);
extern void PgArchShmemInit(void);
extern bool PgArchCanRestart(void);
extern void PgArchiverMain(char *startup_data, size_t startup_data_len) pg_attribute_noreturn();
extern void PgArchWakeup(void);
extern void PgArchForceDirScan(void);

#endif							/* _PGARCH_H */
