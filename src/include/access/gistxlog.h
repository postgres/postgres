/*-------------------------------------------------------------------------
 *
 * gistxlog.h
 *	  gist xlog routines
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
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
#define XLOG_GIST_DELETE			0x10	/* delete leaf index tuples for a
											 * page */
#define XLOG_GIST_PAGE_REUSE		0x20	/* old page is about to be reused
											 * from FSM */
#define XLOG_GIST_PAGE_SPLIT		0x30
 /* #define XLOG_GIST_INSERT_COMPLETE	 0x40 */	/* not used anymore */
 /* #define XLOG_GIST_CREATE_INDEX		 0x50 */	/* not used anymore */
#define XLOG_GIST_PAGE_DELETE		0x60
#define XLOG_GIST_ASSIGN_LSN		0x70	/* nop, assign new LSN */

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
 * Backup Blk 0: Leaf page, whose index tuples are deleted.
 */
typedef struct gistxlogDelete
{
	TransactionId snapshotConflictHorizon;
	uint16		ntodelete;		/* number of deleted offsets */
	bool		isCatalogRel;	/* to handle recovery conflict during logical
								 * decoding on standby */

	/* TODELETE OFFSET NUMBERS */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} gistxlogDelete;

#define SizeOfGistxlogDelete	offsetof(gistxlogDelete, offsets)

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

/*
 * Backup Blk 0: page that was deleted.
 * Backup Blk 1: parent page, containing the downlink to the deleted page.
 */
typedef struct gistxlogPageDelete
{
	FullTransactionId deleteXid;	/* last Xid which could see page in scan */
	OffsetNumber downlinkOffset;	/* Offset of downlink referencing this
									 * page */
} gistxlogPageDelete;

#define SizeOfGistxlogPageDelete	(offsetof(gistxlogPageDelete, downlinkOffset) + sizeof(OffsetNumber))


/*
 * This is what we need to know about page reuse, for hot standby.
 */
typedef struct gistxlogPageReuse
{
	RelFileLocator locator;
	BlockNumber block;
	FullTransactionId snapshotConflictHorizon;
	bool		isCatalogRel;	/* to handle recovery conflict during logical
								 * decoding on standby */
} gistxlogPageReuse;

#define SizeOfGistxlogPageReuse	(offsetof(gistxlogPageReuse, isCatalogRel) + sizeof(bool))

extern void gist_redo(XLogReaderState *record);
extern void gist_desc(StringInfo buf, XLogReaderState *record);
extern const char *gist_identify(uint8 info);
extern void gist_xlog_startup(void);
extern void gist_xlog_cleanup(void);
extern void gist_mask(char *pagedata, BlockNumber blkno);

#endif
