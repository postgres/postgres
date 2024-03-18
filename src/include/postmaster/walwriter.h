/*-------------------------------------------------------------------------
 *
 * walwriter.h
 *	  Exports from postmaster/walwriter.c.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/postmaster/walwriter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _WALWRITER_H
#define _WALWRITER_H

#define DEFAULT_WAL_WRITER_FLUSH_AFTER ((1024 * 1024) / XLOG_BLCKSZ)

/* GUC options */
extern PGDLLIMPORT int WalWriterDelay;
extern PGDLLIMPORT int WalWriterFlushAfter;

extern void WalWriterMain(char *startup_data, size_t startup_data_len) pg_attribute_noreturn();

#endif							/* _WALWRITER_H */
