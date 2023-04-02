/*-------------------------------------------------------------------------
 *
 * hash_xlog.h
 *	  header file for Postgres hash AM implementation
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/hash_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASH_XLOG_H
#define HASH_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/off.h"

/* Number of buffers required for XLOG_HASH_SQUEEZE_PAGE operation */
#define HASH_XLOG_FREE_OVFL_BUFS	6

/*
 * XLOG records for hash operations
 */
#define XLOG_HASH_INIT_META_PAGE	0x00	/* initialize the meta page */
#define XLOG_HASH_INIT_BITMAP_PAGE	0x10	/* initialize the bitmap page */
#define XLOG_HASH_INSERT		0x20	/* add index tuple without split */
#define XLOG_HASH_ADD_OVFL_PAGE 0x30	/* add overflow page */
#define XLOG_HASH_SPLIT_ALLOCATE_PAGE	0x40	/* allocate new page for split */
#define XLOG_HASH_SPLIT_PAGE	0x50	/* split page */
#define XLOG_HASH_SPLIT_COMPLETE	0x60	/* completion of split operation */
#define XLOG_HASH_MOVE_PAGE_CONTENTS	0x70	/* remove tuples from one page
												 * and add to another page */
#define XLOG_HASH_SQUEEZE_PAGE	0x80	/* add tuples to one of the previous
										 * pages in chain and free the ovfl
										 * page */
#define XLOG_HASH_DELETE		0x90	/* delete index tuples from a page */
#define XLOG_HASH_SPLIT_CLEANUP 0xA0	/* clear split-cleanup flag in primary
										 * bucket page after deleting tuples
										 * that are moved due to split	*/
#define XLOG_HASH_UPDATE_META_PAGE	0xB0	/* update meta page after vacuum */

#define XLOG_HASH_VACUUM_ONE_PAGE	0xC0	/* remove dead tuples from index
											 * page */

/*
 * xl_hash_split_allocate_page flag values, 8 bits are available.
 */
#define XLH_SPLIT_META_UPDATE_MASKS		(1<<0)
#define XLH_SPLIT_META_UPDATE_SPLITPOINT		(1<<1)

/*
 * This is what we need to know about simple (without split) insert.
 *
 * This data record is used for XLOG_HASH_INSERT
 *
 * Backup Blk 0: original page (data contains the inserted tuple)
 * Backup Blk 1: metapage (HashMetaPageData)
 */
typedef struct xl_hash_insert
{
	OffsetNumber offnum;
} xl_hash_insert;

#define SizeOfHashInsert	(offsetof(xl_hash_insert, offnum) + sizeof(OffsetNumber))

/*
 * This is what we need to know about addition of overflow page.
 *
 * This data record is used for XLOG_HASH_ADD_OVFL_PAGE
 *
 * Backup Blk 0: newly allocated overflow page
 * Backup Blk 1: page before new overflow page in the bucket chain
 * Backup Blk 2: bitmap page
 * Backup Blk 3: new bitmap page
 * Backup Blk 4: metapage
 */
typedef struct xl_hash_add_ovfl_page
{
	uint16		bmsize;
	bool		bmpage_found;
} xl_hash_add_ovfl_page;

#define SizeOfHashAddOvflPage	\
	(offsetof(xl_hash_add_ovfl_page, bmpage_found) + sizeof(bool))

/*
 * This is what we need to know about allocating a page for split.
 *
 * This data record is used for XLOG_HASH_SPLIT_ALLOCATE_PAGE
 *
 * Backup Blk 0: page for old bucket
 * Backup Blk 1: page for new bucket
 * Backup Blk 2: metapage
 */
typedef struct xl_hash_split_allocate_page
{
	uint32		new_bucket;
	uint16		old_bucket_flag;
	uint16		new_bucket_flag;
	uint8		flags;
} xl_hash_split_allocate_page;

#define SizeOfHashSplitAllocPage	\
	(offsetof(xl_hash_split_allocate_page, flags) + sizeof(uint8))

/*
 * This is what we need to know about completing the split operation.
 *
 * This data record is used for XLOG_HASH_SPLIT_COMPLETE
 *
 * Backup Blk 0: page for old bucket
 * Backup Blk 1: page for new bucket
 */
typedef struct xl_hash_split_complete
{
	uint16		old_bucket_flag;
	uint16		new_bucket_flag;
} xl_hash_split_complete;

#define SizeOfHashSplitComplete \
	(offsetof(xl_hash_split_complete, new_bucket_flag) + sizeof(uint16))

/*
 * This is what we need to know about move page contents required during
 * squeeze operation.
 *
 * This data record is used for XLOG_HASH_MOVE_PAGE_CONTENTS
 *
 * Backup Blk 0: bucket page
 * Backup Blk 1: page containing moved tuples
 * Backup Blk 2: page from which tuples will be removed
 */
typedef struct xl_hash_move_page_contents
{
	uint16		ntups;
	bool		is_prim_bucket_same_wrt;	/* true if the page to which
											 * tuples are moved is same as
											 * primary bucket page */
} xl_hash_move_page_contents;

#define SizeOfHashMovePageContents	\
	(offsetof(xl_hash_move_page_contents, is_prim_bucket_same_wrt) + sizeof(bool))

/*
 * This is what we need to know about the squeeze page operation.
 *
 * This data record is used for XLOG_HASH_SQUEEZE_PAGE
 *
 * Backup Blk 0: page containing tuples moved from freed overflow page
 * Backup Blk 1: freed overflow page
 * Backup Blk 2: page previous to the freed overflow page
 * Backup Blk 3: page next to the freed overflow page
 * Backup Blk 4: bitmap page containing info of freed overflow page
 * Backup Blk 5: meta page
 */
typedef struct xl_hash_squeeze_page
{
	BlockNumber prevblkno;
	BlockNumber nextblkno;
	uint16		ntups;
	bool		is_prim_bucket_same_wrt;	/* true if the page to which
											 * tuples are moved is same as
											 * primary bucket page */
	bool		is_prev_bucket_same_wrt;	/* true if the page to which
											 * tuples are moved is the page
											 * previous to the freed overflow
											 * page */
} xl_hash_squeeze_page;

#define SizeOfHashSqueezePage	\
	(offsetof(xl_hash_squeeze_page, is_prev_bucket_same_wrt) + sizeof(bool))

/*
 * This is what we need to know about the deletion of index tuples from a page.
 *
 * This data record is used for XLOG_HASH_DELETE
 *
 * Backup Blk 0: primary bucket page
 * Backup Blk 1: page from which tuples are deleted
 */
typedef struct xl_hash_delete
{
	bool		clear_dead_marking; /* true if this operation clears
									 * LH_PAGE_HAS_DEAD_TUPLES flag */
	bool		is_primary_bucket_page; /* true if the operation is for
										 * primary bucket page */
} xl_hash_delete;

#define SizeOfHashDelete	(offsetof(xl_hash_delete, is_primary_bucket_page) + sizeof(bool))

/*
 * This is what we need for metapage update operation.
 *
 * This data record is used for XLOG_HASH_UPDATE_META_PAGE
 *
 * Backup Blk 0: meta page
 */
typedef struct xl_hash_update_meta_page
{
	double		ntuples;
} xl_hash_update_meta_page;

#define SizeOfHashUpdateMetaPage	\
	(offsetof(xl_hash_update_meta_page, ntuples) + sizeof(double))

/*
 * This is what we need to initialize metapage.
 *
 * This data record is used for XLOG_HASH_INIT_META_PAGE
 *
 * Backup Blk 0: meta page
 */
typedef struct xl_hash_init_meta_page
{
	double		num_tuples;
	RegProcedure procid;
	uint16		ffactor;
} xl_hash_init_meta_page;

#define SizeOfHashInitMetaPage		\
	(offsetof(xl_hash_init_meta_page, ffactor) + sizeof(uint16))

/*
 * This is what we need to initialize bitmap page.
 *
 * This data record is used for XLOG_HASH_INIT_BITMAP_PAGE
 *
 * Backup Blk 0: bitmap page
 * Backup Blk 1: meta page
 */
typedef struct xl_hash_init_bitmap_page
{
	uint16		bmsize;
} xl_hash_init_bitmap_page;

#define SizeOfHashInitBitmapPage	\
	(offsetof(xl_hash_init_bitmap_page, bmsize) + sizeof(uint16))

/*
 * This is what we need for index tuple deletion and to
 * update the meta page.
 *
 * This data record is used for XLOG_HASH_VACUUM_ONE_PAGE
 *
 * Backup Blk 0: bucket page
 * Backup Blk 1: meta page
 */
typedef struct xl_hash_vacuum_one_page
{
	TransactionId snapshotConflictHorizon;
	uint16		ntuples;
	bool		isCatalogRel;	/* to handle recovery conflict during logical
								 * decoding on standby */

	/* TARGET OFFSET NUMBERS */
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} xl_hash_vacuum_one_page;

#define SizeOfHashVacuumOnePage offsetof(xl_hash_vacuum_one_page, offsets)

extern void hash_redo(XLogReaderState *record);
extern void hash_desc(StringInfo buf, XLogReaderState *record);
extern const char *hash_identify(uint8 info);
extern void hash_mask(char *pagedata, BlockNumber blkno);

#endif							/* HASH_XLOG_H */
