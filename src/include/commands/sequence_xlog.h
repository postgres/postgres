/*-------------------------------------------------------------------------
 *
 * sequence_xlog.h
 *	  Sequence WAL definitions.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/sequence_xlog.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef SEQUENCE_XLOG_H
#define SEQUENCE_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"

/* Record identifier */
#define XLOG_SEQ_LOG			0x00

/*
 * The "special area" of a sequence's buffer page looks like this.
 */
#define SEQ_MAGIC		0x1717

typedef struct sequence_magic
{
	uint32		magic;
} sequence_magic;

/* Sequence WAL record */
typedef struct xl_seq_rec
{
	RelFileLocator locator;
	/* SEQUENCE TUPLE DATA FOLLOWS AT THE END */
} xl_seq_rec;

extern void seq_redo(XLogReaderState *record);
extern void seq_desc(StringInfo buf, XLogReaderState *record);
extern const char *seq_identify(uint8 info);
extern void seq_mask(char *page, BlockNumber blkno);

#endif							/* SEQUENCE_XLOG_H */
