/*-------------------------------------------------------------------------
 *
 * spgxlog.h
 *	  xlog declarations for SP-GiST access method.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/spgxlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPGXLOG_H
#define SPGXLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/off.h"

/* XLOG record types for SPGiST */
 /* #define XLOG_SPGIST_CREATE_INDEX       0x00 */	/* not used anymore */
#define XLOG_SPGIST_ADD_LEAF		0x10
#define XLOG_SPGIST_MOVE_LEAFS		0x20
#define XLOG_SPGIST_ADD_NODE		0x30
#define XLOG_SPGIST_SPLIT_TUPLE		0x40
#define XLOG_SPGIST_PICKSPLIT		0x50
#define XLOG_SPGIST_VACUUM_LEAF		0x60
#define XLOG_SPGIST_VACUUM_ROOT		0x70
#define XLOG_SPGIST_VACUUM_REDIRECT 0x80

/*
 * Some redo functions need an SpGistState, although only a few of its fields
 * need to be valid.  spgxlogState carries the required info in xlog records.
 * (See fillFakeState in spgxlog.c for more comments.)
 */
typedef struct spgxlogState
{
	TransactionId myXid;
	bool		isBuild;
} spgxlogState;

/*
 * Backup Blk 0: destination page for leaf tuple
 * Backup Blk 1: parent page (if any)
 */
typedef struct spgxlogAddLeaf
{
	bool		newPage;		/* init dest page? */
	bool		storesNulls;	/* page is in the nulls tree? */
	OffsetNumber offnumLeaf;	/* offset where leaf tuple gets placed */
	OffsetNumber offnumHeadLeaf;	/* offset of head tuple in chain, if any */

	OffsetNumber offnumParent;	/* where the parent downlink is, if any */
	uint16		nodeI;

	/* new leaf tuple follows (unaligned!) */
} spgxlogAddLeaf;

/*
 * Backup Blk 0: source leaf page
 * Backup Blk 1: destination leaf page
 * Backup Blk 2: parent page
 */
typedef struct spgxlogMoveLeafs
{
	uint16		nMoves;			/* number of tuples moved from source page */
	bool		newPage;		/* init dest page? */
	bool		replaceDead;	/* are we replacing a DEAD source tuple? */
	bool		storesNulls;	/* pages are in the nulls tree? */

	/* where the parent downlink is */
	OffsetNumber offnumParent;
	uint16		nodeI;

	spgxlogState stateSrc;

	/*----------
	 * data follows:
	 *		array of deleted tuple numbers, length nMoves
	 *		array of inserted tuple numbers, length nMoves + 1 or 1
	 *		list of leaf tuples, length nMoves + 1 or 1 (unaligned!)
	 *
	 * Note: if replaceDead is true then there is only one inserted tuple
	 * number and only one leaf tuple in the data, because we are not copying
	 * the dead tuple from the source
	 *----------
	 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} spgxlogMoveLeafs;

#define SizeOfSpgxlogMoveLeafs	offsetof(spgxlogMoveLeafs, offsets)

/*
 * Backup Blk 0: original page
 * Backup Blk 1: where new tuple goes, if not same place
 * Backup Blk 2: where parent downlink is, if updated and different from
 *				 the old and new
 */
typedef struct spgxlogAddNode
{
	/*
	 * Offset of the original inner tuple, in the original page (on backup
	 * block 0).
	 */
	OffsetNumber offnum;

	/*
	 * Offset of the new tuple, on the new page (on backup block 1). Invalid,
	 * if we overwrote the old tuple in the original page).
	 */
	OffsetNumber offnumNew;
	bool		newPage;		/* init new page? */

	/*----
	 * Where is the parent downlink? parentBlk indicates which page it's on,
	 * and offnumParent is the offset within the page. The possible values for
	 * parentBlk are:
	 *
	 * 0: parent == original page
	 * 1: parent == new page
	 * 2: parent == different page (blk ref 2)
	 * -1: parent not updated
	 *----
	 */
	int8		parentBlk;
	OffsetNumber offnumParent;	/* offset within the parent page */

	uint16		nodeI;

	spgxlogState stateSrc;

	/*
	 * updated inner tuple follows (unaligned!)
	 */
} spgxlogAddNode;

/*
 * Backup Blk 0: where the prefix tuple goes
 * Backup Blk 1: where the postfix tuple goes (if different page)
 */
typedef struct spgxlogSplitTuple
{
	/* where the prefix tuple goes */
	OffsetNumber offnumPrefix;

	/* where the postfix tuple goes */
	OffsetNumber offnumPostfix;
	bool		newPage;		/* need to init that page? */
	bool		postfixBlkSame; /* was postfix tuple put on same page as
								 * prefix? */

	/*
	 * new prefix inner tuple follows, then new postfix inner tuple (both are
	 * unaligned!)
	 */
} spgxlogSplitTuple;

/*
 * Buffer references in the rdata array are:
 * Backup Blk 0: Src page (only if not root)
 * Backup Blk 1: Dest page (if used)
 * Backup Blk 2: Inner page
 * Backup Blk 3: Parent page (if any, and different from Inner)
 */
typedef struct spgxlogPickSplit
{
	bool		isRootSplit;

	uint16		nDelete;		/* n to delete from Src */
	uint16		nInsert;		/* n to insert on Src and/or Dest */
	bool		initSrc;		/* re-init the Src page? */
	bool		initDest;		/* re-init the Dest page? */

	/* where to put new inner tuple */
	OffsetNumber offnumInner;
	bool		initInner;		/* re-init the Inner page? */

	bool		storesNulls;	/* pages are in the nulls tree? */

	/* where the parent downlink is, if any */
	bool		innerIsParent;	/* is parent the same as inner page? */
	OffsetNumber offnumParent;
	uint16		nodeI;

	spgxlogState stateSrc;

	/*----------
	 * data follows:
	 *		array of deleted tuple numbers, length nDelete
	 *		array of inserted tuple numbers, length nInsert
	 *		array of page selector bytes for inserted tuples, length nInsert
	 *		new inner tuple (unaligned!)
	 *		list of leaf tuples, length nInsert (unaligned!)
	 *----------
	 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} spgxlogPickSplit;

#define SizeOfSpgxlogPickSplit offsetof(spgxlogPickSplit, offsets)

typedef struct spgxlogVacuumLeaf
{
	uint16		nDead;			/* number of tuples to become DEAD */
	uint16		nPlaceholder;	/* number of tuples to become PLACEHOLDER */
	uint16		nMove;			/* number of tuples to move */
	uint16		nChain;			/* number of tuples to re-chain */

	spgxlogState stateSrc;

	/*----------
	 * data follows:
	 *		tuple numbers to become DEAD
	 *		tuple numbers to become PLACEHOLDER
	 *		tuple numbers to move from (and replace with PLACEHOLDER)
	 *		tuple numbers to move to (replacing what is there)
	 *		tuple numbers to update nextOffset links of
	 *		tuple numbers to insert in nextOffset links
	 *----------
	 */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} spgxlogVacuumLeaf;

#define SizeOfSpgxlogVacuumLeaf offsetof(spgxlogVacuumLeaf, offsets)

typedef struct spgxlogVacuumRoot
{
	/* vacuum a root page when it is also a leaf */
	uint16		nDelete;		/* number of tuples to delete */

	spgxlogState stateSrc;

	/* offsets of tuples to delete follow */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} spgxlogVacuumRoot;

#define SizeOfSpgxlogVacuumRoot offsetof(spgxlogVacuumRoot, offsets)

typedef struct spgxlogVacuumRedirect
{
	uint16		nToPlaceholder; /* number of redirects to make placeholders */
	OffsetNumber firstPlaceholder;	/* first placeholder tuple to remove */
	TransactionId newestRedirectXid;	/* newest XID of removed redirects */

	/* offsets of redirect tuples to make placeholders follow */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} spgxlogVacuumRedirect;

#define SizeOfSpgxlogVacuumRedirect offsetof(spgxlogVacuumRedirect, offsets)

extern void spg_redo(XLogReaderState *record);
extern void spg_desc(StringInfo buf, XLogReaderState *record);
extern const char *spg_identify(uint8 info);
extern void spg_xlog_startup(void);
extern void spg_xlog_cleanup(void);
extern void spg_mask(char *pagedata, BlockNumber blkno);

#endif							/* SPGXLOG_H */
