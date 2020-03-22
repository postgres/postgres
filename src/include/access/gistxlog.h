/*-------------------------------------------------------------------------
 *
 * gistxlog.h
 *	  gist xlog routines
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/gistxlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_XLOG_H
#define GIST_XLOG_H

#include "access/gist.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"

#define XLOG_GIST_PAGE_UPDATE		0x00
 /* #define XLOG_GIST_NEW_ROOT			 0x20 */	/* not used anymore */
#define XLOG_GIST_PAGE_SPLIT		0x30
 /* #define XLOG_GIST_INSERT_COMPLETE	 0x40 */	/* not used anymore */
#define XLOG_GIST_CREATE_INDEX		0x50
 /* #define XLOG_GIST_PAGE_DELETE		 0x60 */	/* not used anymore */

/*
 * Backup Blk 0: updated page.
 * Backup Blk 1: If this operation completes a page split, by inserting a
 *				 downlink for the split page, the left half of the split
 */
typedef struct gistxlogPageUpdate
{
	/* number of deleted offsets */
	uint16		ntodelete;
	uint16		ntoinsert;

	/*
	 * In payload of blk 0 : 1. todelete OffsetNumbers 2. tuples to insert
	 */
} gistxlogPageUpdate;

/*
 * Backup Blk 0: If this operation completes a page split, by inserting a
 *				 downlink for the split page, the left half of the split
 * Backup Blk 1 - npage: split pages (1 is the original page)
 */
typedef struct gistxlogPageSplit
{
	BlockNumber origrlink;		/* rightlink of the page before split */
	GistNSN		orignsn;		/* NSN of the page before split */
	bool		origleaf;		/* was splitted page a leaf page? */

	uint16		npage;			/* # of pages in the split */
	bool		markfollowright;	/* set F_FOLLOW_RIGHT flags */

	/*
	 * follow: 1. gistxlogPage and array of IndexTupleData per page
	 */
} gistxlogPageSplit;

extern void gist_redo(XLogReaderState *record);
extern void gist_desc(StringInfo buf, XLogReaderState *record);
extern const char *gist_identify(uint8 info);
extern void gist_xlog_startup(void);
extern void gist_xlog_cleanup(void);
extern void gist_mask(char *pagedata, BlockNumber blkno);

#endif
