/*-------------------------------------------------------------------------
 *
 * brin_xlog.h
 *	  POSTGRES BRIN access XLOG definitions.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/brin_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BRIN_XLOG_H
#define BRIN_XLOG_H

#include "access/xlogrecord.h"
#include "lib/stringinfo.h"
#include "storage/bufpage.h"
#include "storage/itemptr.h"
#include "storage/relfilenode.h"
#include "utils/relcache.h"


/*
 * WAL record definitions for BRIN's WAL operations
 *
 * XLOG allows to store some information in high 4 bits of log
 * record xl_info field.
 */
#define XLOG_BRIN_CREATE_INDEX		0x00
#define XLOG_BRIN_INSERT			0x10
#define XLOG_BRIN_UPDATE			0x20
#define XLOG_BRIN_SAMEPAGE_UPDATE	0x30
#define XLOG_BRIN_REVMAP_EXTEND		0x40
#define XLOG_BRIN_REVMAP_VACUUM		0x50

#define XLOG_BRIN_OPMASK			0x70
/*
 * When we insert the first item on a new page, we restore the entire page in
 * redo.
 */
#define XLOG_BRIN_INIT_PAGE		0x80

/* This is what we need to know about a BRIN index create */
typedef struct xl_brin_createidx
{
	BlockNumber pagesPerRange;
	RelFileNode node;
	uint16		version;
} xl_brin_createidx;
#define SizeOfBrinCreateIdx (offsetof(xl_brin_createidx, version) + sizeof(uint16))

/*
 * This is what we need to know about a BRIN tuple insert
 */
typedef struct xl_brin_insert
{
	RelFileNode node;
	BlockNumber heapBlk;

	/* extra information needed to update the revmap */
	BlockNumber revmapBlk;
	BlockNumber pagesPerRange;

	uint16		tuplen;
	ItemPointerData tid;
	/* tuple data follows at end of struct */
} xl_brin_insert;

#define SizeOfBrinInsert	(offsetof(xl_brin_insert, tid) + sizeof(ItemPointerData))

/*
 * A cross-page update is the same as an insert, but also store the old tid.
 */
typedef struct xl_brin_update
{
	ItemPointerData oldtid;
	xl_brin_insert insert;
} xl_brin_update;

#define SizeOfBrinUpdate	(offsetof(xl_brin_update, insert) + SizeOfBrinInsert)

/* This is what we need to know about a BRIN tuple samepage update */
typedef struct xl_brin_samepage_update
{
	RelFileNode node;
	ItemPointerData tid;
	/* tuple data follows at end of struct */
} xl_brin_samepage_update;

#define SizeOfBrinSamepageUpdate		(offsetof(xl_brin_samepage_update, tid) + sizeof(ItemPointerData))

/* This is what we need to know about a revmap extension */
typedef struct xl_brin_revmap_extend
{
	RelFileNode node;
	BlockNumber targetBlk;
} xl_brin_revmap_extend;

#define SizeOfBrinRevmapExtend	(offsetof(xl_brin_revmap_extend, targetBlk) + \
								 sizeof(BlockNumber))


extern void brin_desc(StringInfo buf, XLogRecord *record);
extern void brin_redo(XLogRecPtr lsn, XLogRecord *record);
extern const char *brin_identify(uint8 info);

#endif   /* BRIN_XLOG_H */
