/*--------------------------------------------------------------------------
 * ginxlog.h
 *	  header file for postgres inverted index xlog implementation.
 *
 *	Copyright (c) 2006-2023, PostgreSQL Global Development Group
 *
 *	src/include/access/ginxlog.h
 *--------------------------------------------------------------------------
 */
#ifndef GINXLOG_H
#define GINXLOG_H

#include "access/ginblock.h"
#include "access/itup.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/off.h"

#define XLOG_GIN_CREATE_PTREE  0x10

typedef struct ginxlogCreatePostingTree
{
	uint32		size;
	/* A compressed posting list follows */
} ginxlogCreatePostingTree;

/*
 * The format of the insertion record varies depending on the page type.
 * ginxlogInsert is the common part between all variants.
 *
 * Backup Blk 0: target page
 * Backup Blk 1: left child, if this insertion finishes an incomplete split
 */

#define XLOG_GIN_INSERT  0x20

typedef struct
{
	uint16		flags;			/* GIN_INSERT_ISLEAF and/or GIN_INSERT_ISDATA */

	/*
	 * FOLLOWS:
	 *
	 * 1. if not leaf page, block numbers of the left and right child pages
	 * whose split this insertion finishes, as BlockIdData[2] (beware of
	 * adding fields in this struct that would make them not 16-bit aligned)
	 *
	 * 2. a ginxlogInsertEntry or ginxlogRecompressDataLeaf struct, depending
	 * on tree type.
	 *
	 * NB: the below structs are only 16-bit aligned when appended to a
	 * ginxlogInsert struct! Beware of adding fields to them that require
	 * stricter alignment.
	 */
} ginxlogInsert;

typedef struct
{
	OffsetNumber offset;
	bool		isDelete;
	IndexTupleData tuple;		/* variable length */
} ginxlogInsertEntry;


typedef struct
{
	uint16		nactions;

	/* Variable number of 'actions' follow */
} ginxlogRecompressDataLeaf;

/*
 * Note: this struct is currently not used in code, and only acts as
 * documentation. The WAL record format is as specified here, but the code
 * uses straight access through a Pointer and memcpy to read/write these.
 */
typedef struct
{
	uint8		segno;			/* segment this action applies to */
	char		type;			/* action type (see below) */

	/*
	 * Action-specific data follows. For INSERT and REPLACE actions that is a
	 * GinPostingList struct. For ADDITEMS, a uint16 for the number of items
	 * added, followed by the items themselves as ItemPointers. DELETE actions
	 * have no further data.
	 */
}			ginxlogSegmentAction;

/* Action types */
#define GIN_SEGMENT_UNMODIFIED	0	/* no action (not used in WAL records) */
#define GIN_SEGMENT_DELETE		1	/* a whole segment is removed */
#define GIN_SEGMENT_INSERT		2	/* a whole segment is added */
#define GIN_SEGMENT_REPLACE		3	/* a segment is replaced */
#define GIN_SEGMENT_ADDITEMS	4	/* items are added to existing segment */

typedef struct
{
	OffsetNumber offset;
	PostingItem newitem;
} ginxlogInsertDataInternal;

/*
 * Backup Blk 0: new left page (= original page, if not root split)
 * Backup Blk 1: new right page
 * Backup Blk 2: original page / new root page, if root split
 * Backup Blk 3: left child, if this insertion completes an earlier split
 */
#define XLOG_GIN_SPLIT	0x30

typedef struct ginxlogSplit
{
	RelFileLocator locator;
	BlockNumber rrlink;			/* right link, or root's blocknumber if root
								 * split */
	BlockNumber leftChildBlkno; /* valid on a non-leaf split */
	BlockNumber rightChildBlkno;
	uint16		flags;			/* see below */
} ginxlogSplit;

/*
 * Flags used in ginxlogInsert and ginxlogSplit records
 */
#define GIN_INSERT_ISDATA	0x01	/* for both insert and split records */
#define GIN_INSERT_ISLEAF	0x02	/* ditto */
#define GIN_SPLIT_ROOT		0x04	/* only for split records */

/*
 * Vacuum simply WAL-logs the whole page, when anything is modified. This
 * is functionally identical to XLOG_FPI records, but is kept separate for
 * debugging purposes. (When inspecting the WAL stream, it's easier to see
 * what's going on when GIN vacuum records are marked as such, not as heap
 * records.) This is currently only used for entry tree leaf pages.
 */
#define XLOG_GIN_VACUUM_PAGE	0x40

/*
 * Vacuuming posting tree leaf page is WAL-logged like recompression caused
 * by insertion.
 */
#define XLOG_GIN_VACUUM_DATA_LEAF_PAGE	0x90

typedef struct ginxlogVacuumDataLeafPage
{
	ginxlogRecompressDataLeaf data;
} ginxlogVacuumDataLeafPage;

/*
 * Backup Blk 0: deleted page
 * Backup Blk 1: parent
 * Backup Blk 2: left sibling
 */
#define XLOG_GIN_DELETE_PAGE	0x50

typedef struct ginxlogDeletePage
{
	OffsetNumber parentOffset;
	BlockNumber rightLink;
	TransactionId deleteXid;	/* last Xid which could see this page in scan */
} ginxlogDeletePage;

#define XLOG_GIN_UPDATE_META_PAGE 0x60

/*
 * Backup Blk 0: metapage
 * Backup Blk 1: tail page
 */
typedef struct ginxlogUpdateMeta
{
	RelFileLocator locator;
	GinMetaPageData metadata;
	BlockNumber prevTail;
	BlockNumber newRightlink;
	int32		ntuples;		/* if ntuples > 0 then metadata.tail was
								 * updated with that many tuples; else new sub
								 * list was inserted */
	/* array of inserted tuples follows */
} ginxlogUpdateMeta;

#define XLOG_GIN_INSERT_LISTPAGE  0x70

typedef struct ginxlogInsertListPage
{
	BlockNumber rightlink;
	int32		ntuples;
	/* array of inserted tuples follows */
} ginxlogInsertListPage;

/*
 * Backup Blk 0: metapage
 * Backup Blk 1 to (ndeleted + 1): deleted pages
 */

#define XLOG_GIN_DELETE_LISTPAGE  0x80

/*
 * The WAL record for deleting list pages must contain a block reference to
 * all the deleted pages, so the number of pages that can be deleted in one
 * record is limited by XLR_MAX_BLOCK_ID. (block_id 0 is used for the
 * metapage.)
 */
#define GIN_NDELETE_AT_ONCE Min(16, XLR_MAX_BLOCK_ID - 1)
typedef struct ginxlogDeleteListPages
{
	GinMetaPageData metadata;
	int32		ndeleted;
} ginxlogDeleteListPages;

extern void gin_redo(XLogReaderState *record);
extern void gin_desc(StringInfo buf, XLogReaderState *record);
extern const char *gin_identify(uint8 info);
extern void gin_xlog_startup(void);
extern void gin_xlog_cleanup(void);
extern void gin_mask(char *pagedata, BlockNumber blkno);

#endif							/* GINXLOG_H */
