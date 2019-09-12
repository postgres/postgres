/*-------------------------------------------------------------------------
 *
 * nbtxlog.h
 *	  header file for postgres btree xlog routines
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/nbtxlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NBTXLOG_H
#define NBTXLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/off.h"

/*
 * XLOG records for btree operations
 *
 * XLOG allows to store some information in high 4 bits of log
 * record xl_info field
 */
#define XLOG_BTREE_INSERT_LEAF	0x00	/* add index tuple without split */
#define XLOG_BTREE_INSERT_UPPER 0x10	/* same, on a non-leaf page */
#define XLOG_BTREE_INSERT_META	0x20	/* same, plus update metapage */
#define XLOG_BTREE_SPLIT_L		0x30	/* add index tuple with split */
#define XLOG_BTREE_SPLIT_R		0x40	/* as above, new item on right */
/* 0x50 and 0x60 are unused */
#define XLOG_BTREE_DELETE		0x70	/* delete leaf index tuples for a page */
#define XLOG_BTREE_UNLINK_PAGE	0x80	/* delete a half-dead page */
#define XLOG_BTREE_UNLINK_PAGE_META 0x90	/* same, and update metapage */
#define XLOG_BTREE_NEWROOT		0xA0	/* new root page */
#define XLOG_BTREE_MARK_PAGE_HALFDEAD 0xB0	/* mark a leaf as half-dead */
#define XLOG_BTREE_VACUUM		0xC0	/* delete entries on a page during
										 * vacuum */
#define XLOG_BTREE_REUSE_PAGE	0xD0	/* old page is about to be reused from
										 * FSM */
#define XLOG_BTREE_META_CLEANUP	0xE0	/* update cleanup-related data in the
										 * metapage */

/*
 * All that we need to regenerate the meta-data page
 */
typedef struct xl_btree_metadata
{
	uint32		version;
	BlockNumber root;
	uint32		level;
	BlockNumber fastroot;
	uint32		fastlevel;
	TransactionId oldest_btpo_xact;
	float8		last_cleanup_num_heap_tuples;
} xl_btree_metadata;

/*
 * This is what we need to know about simple (without split) insert.
 *
 * This data record is used for INSERT_LEAF, INSERT_UPPER, INSERT_META.
 * Note that INSERT_META implies it's not a leaf page.
 *
 * Backup Blk 0: original page (data contains the inserted tuple)
 * Backup Blk 1: child's left sibling, if INSERT_UPPER or INSERT_META
 * Backup Blk 2: xl_btree_metadata, if INSERT_META
 */
typedef struct xl_btree_insert
{
	OffsetNumber offnum;
} xl_btree_insert;

#define SizeOfBtreeInsert	(offsetof(xl_btree_insert, offnum) + sizeof(OffsetNumber))

/*
 * On insert with split, we save all the items going into the right sibling
 * so that we can restore it completely from the log record.  This way takes
 * less xlog space than the normal approach, because if we did it standardly,
 * XLogInsert would almost always think the right page is new and store its
 * whole page image.  The left page, however, is handled in the normal
 * incremental-update fashion.
 *
 * Note: XLOG_BTREE_SPLIT_L and XLOG_BTREE_SPLIT_R share this data record.
 * There are two variants to indicate whether the inserted tuple went into the
 * left or right split page (and thus, whether the new item is stored or not).
 * We always log the left page high key because suffix truncation can generate
 * a new leaf high key using user-defined code.  This is also necessary on
 * internal pages, since the first right item that the left page's high key
 * was based on will have been truncated to zero attributes in the right page
 * (the original is unavailable from the right page).
 *
 * Backup Blk 0: original page / new left page
 *
 * The left page's data portion contains the new item, if it's the _L variant.
 * An IndexTuple representing the high key of the left page must follow with
 * either variant.
 *
 * Backup Blk 1: new right page
 *
 * The right page's data portion contains the right page's tuples in the form
 * used by _bt_restore_page.  This includes the new item, if it's the _R
 * variant.  The right page's tuples also include the right page's high key
 * with either variant (moved from the left/original page during the split),
 * unless the split happened to be of the rightmost page on its level, where
 * there is no high key for new right page.
 *
 * Backup Blk 2: next block (orig page's rightlink), if any
 * Backup Blk 3: child's left sibling, if non-leaf split
 */
typedef struct xl_btree_split
{
	uint32		level;			/* tree level of page being split */
	OffsetNumber firstright;	/* first item moved to right page */
	OffsetNumber newitemoff;	/* new item's offset (useful for _L variant) */
} xl_btree_split;

#define SizeOfBtreeSplit	(offsetof(xl_btree_split, newitemoff) + sizeof(OffsetNumber))

/*
 * This is what we need to know about delete of individual leaf index tuples.
 * The WAL record can represent deletion of any number of index tuples on a
 * single index page when *not* executed by VACUUM.
 *
 * Backup Blk 0: index page
 */
typedef struct xl_btree_delete
{
	TransactionId latestRemovedXid;
	int			nitems;

	/* TARGET OFFSET NUMBERS FOLLOW AT THE END */
} xl_btree_delete;

#define SizeOfBtreeDelete	(offsetof(xl_btree_delete, nitems) + sizeof(int))

/*
 * This is what we need to know about page reuse within btree.
 */
typedef struct xl_btree_reuse_page
{
	RelFileNode node;
	BlockNumber block;
	TransactionId latestRemovedXid;
} xl_btree_reuse_page;

#define SizeOfBtreeReusePage	(sizeof(xl_btree_reuse_page))

/*
 * This is what we need to know about vacuum of individual leaf index tuples.
 * The WAL record can represent deletion of any number of index tuples on a
 * single index page when executed by VACUUM.
 *
 * For MVCC scans, lastBlockVacuumed will be set to InvalidBlockNumber.
 * For a non-MVCC index scans there is an additional correctness requirement
 * for applying these changes during recovery, which is that we must do one
 * of these two things for every block in the index:
 *		* lock the block for cleanup and apply any required changes
 *		* EnsureBlockUnpinned()
 * The purpose of this is to ensure that no index scans started before we
 * finish scanning the index are still running by the time we begin to remove
 * heap tuples.
 *
 * Any changes to any one block are registered on just one WAL record. All
 * blocks that we need to run EnsureBlockUnpinned() are listed as a block range
 * starting from the last block vacuumed through until this one. Individual
 * block numbers aren't given.
 *
 * Note that the *last* WAL record in any vacuum of an index is allowed to
 * have a zero length array of offsets. Earlier records must have at least one.
 */
typedef struct xl_btree_vacuum
{
	BlockNumber lastBlockVacuumed;

	/* TARGET OFFSET NUMBERS FOLLOW */
} xl_btree_vacuum;

#define SizeOfBtreeVacuum	(offsetof(xl_btree_vacuum, lastBlockVacuumed) + sizeof(BlockNumber))

/*
 * This is what we need to know about marking an empty branch for deletion.
 * The target identifies the tuple removed from the parent page (note that we
 * remove this tuple's downlink and the *following* tuple's key).  Note that
 * the leaf page is empty, so we don't need to store its content --- it is
 * just reinitialized during recovery using the rest of the fields.
 *
 * Backup Blk 0: leaf block
 * Backup Blk 1: top parent
 */
typedef struct xl_btree_mark_page_halfdead
{
	OffsetNumber poffset;		/* deleted tuple id in parent page */

	/* information needed to recreate the leaf page: */
	BlockNumber leafblk;		/* leaf block ultimately being deleted */
	BlockNumber leftblk;		/* leaf block's left sibling, if any */
	BlockNumber rightblk;		/* leaf block's right sibling */
	BlockNumber topparent;		/* topmost internal page in the branch */
} xl_btree_mark_page_halfdead;

#define SizeOfBtreeMarkPageHalfDead (offsetof(xl_btree_mark_page_halfdead, topparent) + sizeof(BlockNumber))

/*
 * This is what we need to know about deletion of a btree page.  Note we do
 * not store any content for the deleted page --- it is just rewritten as empty
 * during recovery, apart from resetting the btpo.xact.
 *
 * Backup Blk 0: target block being deleted
 * Backup Blk 1: target block's left sibling, if any
 * Backup Blk 2: target block's right sibling
 * Backup Blk 3: leaf block (if different from target)
 * Backup Blk 4: metapage (if rightsib becomes new fast root)
 */
typedef struct xl_btree_unlink_page
{
	BlockNumber leftsib;		/* target block's left sibling, if any */
	BlockNumber rightsib;		/* target block's right sibling */

	/*
	 * Information needed to recreate the leaf page, when target is an
	 * internal page.
	 */
	BlockNumber leafleftsib;
	BlockNumber leafrightsib;
	BlockNumber topparent;		/* next child down in the branch */

	TransactionId btpo_xact;	/* value of btpo.xact for use in recovery */
	/* xl_btree_metadata FOLLOWS IF XLOG_BTREE_UNLINK_PAGE_META */
} xl_btree_unlink_page;

#define SizeOfBtreeUnlinkPage	(offsetof(xl_btree_unlink_page, btpo_xact) + sizeof(TransactionId))

/*
 * New root log record.  There are zero tuples if this is to establish an
 * empty root, or two if it is the result of splitting an old root.
 *
 * Note that although this implies rewriting the metadata page, we don't need
 * an xl_btree_metadata record --- the rootblk and level are sufficient.
 *
 * Backup Blk 0: new root page (2 tuples as payload, if splitting old root)
 * Backup Blk 1: left child (if splitting an old root)
 * Backup Blk 2: metapage
 */
typedef struct xl_btree_newroot
{
	BlockNumber rootblk;		/* location of new root (redundant with blk 0) */
	uint32		level;			/* its tree level */
} xl_btree_newroot;

#define SizeOfBtreeNewroot	(offsetof(xl_btree_newroot, level) + sizeof(uint32))


/*
 * prototypes for functions in nbtxlog.c
 */
extern void btree_redo(XLogReaderState *record);
extern void btree_desc(StringInfo buf, XLogReaderState *record);
extern const char *btree_identify(uint8 info);
extern void btree_mask(char *pagedata, BlockNumber blkno);

#endif							/* NBXLOG_H */
