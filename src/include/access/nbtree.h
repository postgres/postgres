/*-------------------------------------------------------------------------
 *
 * nbtree.h
 *	  header file for postgres btree access method implementation.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nbtree.h,v 1.71 2003/09/29 23:40:26 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NBTREE_H
#define NBTREE_H

#include "access/itup.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/xlogutils.h"

/*
 *	BTPageOpaqueData -- At the end of every page, we store a pointer
 *	to both siblings in the tree.  This is used to do forward/backward
 *	index scans.  The next-page link is also critical for recovery when
 *	a search has navigated to the wrong page due to concurrent page splits
 *	or deletions; see src/backend/access/nbtree/README for more info.
 *
 *	In addition, we store the page's btree level (counting upwards from
 *	zero at a leaf page) as well as some flag bits indicating the page type
 *	and status.  If the page is deleted, we replace the level with the
 *	next-transaction-ID value indicating when it is safe to reclaim the page.
 *
 *	NOTE: the BTP_LEAF flag bit is redundant since level==0 could be tested
 *	instead.
 */

typedef struct BTPageOpaqueData
{
	BlockNumber btpo_prev;		/* left sibling, or P_NONE if leftmost */
	BlockNumber btpo_next;		/* right sibling, or P_NONE if rightmost */
	union
	{
		uint32		level;		/* tree level --- zero for leaf pages */
		TransactionId xact;		/* next transaction ID, if deleted */
	}			btpo;
	uint16		btpo_flags;		/* flag bits, see below */
} BTPageOpaqueData;

typedef BTPageOpaqueData *BTPageOpaque;

/* Bits defined in btpo_flags */
#define BTP_LEAF		(1 << 0)	/* leaf page, i.e. not internal page */
#define BTP_ROOT		(1 << 1)	/* root page (has no parent) */
#define BTP_DELETED		(1 << 2)	/* page has been deleted from tree */
#define BTP_META		(1 << 3)	/* meta-page */
#define BTP_HALF_DEAD	(1 << 4)	/* empty, but still in tree */


/*
 * The Meta page is always the first page in the btree index.
 * Its primary purpose is to point to the location of the btree root page.
 * We also point to the "fast" root, which is the current effective root;
 * see README for discussion.
 */

typedef struct BTMetaPageData
{
	uint32		btm_magic;		/* should contain BTREE_MAGIC */
	uint32		btm_version;	/* should contain BTREE_VERSION */
	BlockNumber btm_root;		/* current root location */
	uint32		btm_level;		/* tree level of the root page */
	BlockNumber btm_fastroot;	/* current "fast" root location */
	uint32		btm_fastlevel;	/* tree level of the "fast" root page */
} BTMetaPageData;

#define BTPageGetMeta(p) \
	((BTMetaPageData *) PageGetContents(p))

#define BTREE_METAPAGE	0		/* first page is meta */
#define BTREE_MAGIC		0x053162	/* magic number of btree pages */
#define BTREE_VERSION	2		/* current version number */

/*
 * We actually need to be able to fit three items on every page,
 * so restrict any one item to 1/3 the per-page available space.
 */
#define BTMaxItemSize(page) \
	((PageGetPageSize(page) - \
	  sizeof(PageHeaderData) - \
	  MAXALIGN(sizeof(BTPageOpaqueData))) / 3 - sizeof(ItemIdData))

/*
 *	BTItems are what we store in the btree.  Each item is an index tuple,
 *	including key and pointer values.  (In some cases either the key or the
 *	pointer may go unused, see backend/access/nbtree/README for details.)
 *
 *	Old comments:
 *	In addition, we must guarantee that all tuples in the index are unique,
 *	in order to satisfy some assumptions in Lehman and Yao.  The way that we
 *	do this is by generating a new OID for every insertion that we do in the
 *	tree.  This adds eight bytes to the size of btree index tuples.  Note
 *	that we do not use the OID as part of a composite key; the OID only
 *	serves as a unique identifier for a given index tuple (logical position
 *	within a page).
 *
 *	New comments:
 *	actually, we must guarantee that all tuples in A LEVEL
 *	are unique, not in ALL INDEX. So, we can use bti_itup->t_tid
 *	as unique identifier for a given index tuple (logical position
 *	within a level). - vadim 04/09/97
 */

typedef struct BTItemData
{
	IndexTupleData bti_itup;
} BTItemData;

typedef BTItemData *BTItem;

#define CopyBTItem(btitem) ((BTItem) CopyIndexTuple((IndexTuple) (btitem)))

/*
 * For XLOG: size without alignment. Sizeof works as long as
 * IndexTupleData has exactly 8 bytes.
 */
#define SizeOfBTItem	sizeof(BTItemData)

/* Test whether items are the "same" per the above notes */
#define BTTidSame(i1, i2)	\
	( (i1).ip_blkid.bi_hi == (i2).ip_blkid.bi_hi && \
	  (i1).ip_blkid.bi_lo == (i2).ip_blkid.bi_lo && \
	  (i1).ip_posid == (i2).ip_posid )
#define BTItemSame(i1, i2)	\
	BTTidSame((i1)->bti_itup.t_tid, (i2)->bti_itup.t_tid)


/*
 *	In general, the btree code tries to localize its knowledge about
 *	page layout to a couple of routines.  However, we need a special
 *	value to indicate "no page number" in those places where we expect
 *	page numbers.  We can use zero for this because we never need to
 *	make a pointer to the metadata page.
 */

#define P_NONE			0

/*
 * Macros to test whether a page is leftmost or rightmost on its tree level,
 * as well as other state info kept in the opaque data.
 */
#define P_LEFTMOST(opaque)		((opaque)->btpo_prev == P_NONE)
#define P_RIGHTMOST(opaque)		((opaque)->btpo_next == P_NONE)
#define P_ISLEAF(opaque)		((opaque)->btpo_flags & BTP_LEAF)
#define P_ISROOT(opaque)		((opaque)->btpo_flags & BTP_ROOT)
#define P_ISDELETED(opaque)		((opaque)->btpo_flags & BTP_DELETED)
#define P_IGNORE(opaque)		((opaque)->btpo_flags & (BTP_DELETED|BTP_HALF_DEAD))

/*
 *	Lehman and Yao's algorithm requires a ``high key'' on every non-rightmost
 *	page.  The high key is not a data key, but gives info about what range of
 *	keys is supposed to be on this page.  The high key on a page is required
 *	to be greater than or equal to any data key that appears on the page.
 *	If we find ourselves trying to insert a key > high key, we know we need
 *	to move right (this should only happen if the page was split since we
 *	examined the parent page).
 *
 *	Our insertion algorithm guarantees that we can use the initial least key
 *	on our right sibling as the high key.  Once a page is created, its high
 *	key changes only if the page is split.
 *
 *	On a non-rightmost page, the high key lives in item 1 and data items
 *	start in item 2.  Rightmost pages have no high key, so we store data
 *	items beginning in item 1.
 */

#define P_HIKEY				((OffsetNumber) 1)
#define P_FIRSTKEY			((OffsetNumber) 2)
#define P_FIRSTDATAKEY(opaque)	(P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY)

/*
 * XLOG records for btree operations
 *
 * XLOG allows to store some information in high 4 bits of log
 * record xl_info field
 */
#define XLOG_BTREE_INSERT_LEAF	0x00	/* add btitem without split */
#define XLOG_BTREE_INSERT_UPPER 0x10	/* same, on a non-leaf page */
#define XLOG_BTREE_INSERT_META	0x20	/* same, plus update metapage */
#define XLOG_BTREE_SPLIT_L		0x30	/* add btitem with split */
#define XLOG_BTREE_SPLIT_R		0x40	/* as above, new item on right */
#define XLOG_BTREE_SPLIT_L_ROOT 0x50	/* add btitem with split of root */
#define XLOG_BTREE_SPLIT_R_ROOT 0x60	/* as above, new item on right */
#define XLOG_BTREE_DELETE		0x70	/* delete leaf btitem */
#define XLOG_BTREE_DELETE_PAGE	0x80	/* delete an entire page */
#define XLOG_BTREE_DELETE_PAGE_META 0x90		/* same, plus update
												 * metapage */
#define XLOG_BTREE_NEWROOT		0xA0	/* new root page */
#define XLOG_BTREE_NEWMETA		0xB0	/* update metadata page */
#define XLOG_BTREE_NEWPAGE		0xC0	/* new index page during build */
#define XLOG_BTREE_INVALIDMETA	0xD0	/* new metadata, temp. invalid */

/*
 * All that we need to find changed index tuple
 */
typedef struct xl_btreetid
{
	RelFileNode node;
	ItemPointerData tid;		/* changed tuple id */
} xl_btreetid;

/*
 * All that we need to regenerate the meta-data page
 */
typedef struct xl_btree_metadata
{
	BlockNumber root;
	uint32		level;
	BlockNumber fastroot;
	uint32		fastlevel;
} xl_btree_metadata;

/*
 * This is what we need to know about simple (without split) insert.
 *
 * This data record is used for INSERT_LEAF, INSERT_UPPER, INSERT_META.
 * Note that INSERT_META implies it's not a leaf page.
 */
typedef struct xl_btree_insert
{
	xl_btreetid target;			/* inserted tuple id */
	/* xl_btree_metadata FOLLOWS IF XLOG_BTREE_INSERT_META */
	/* BTITEM FOLLOWS AT END OF STRUCT */
} xl_btree_insert;

#define SizeOfBtreeInsert	(offsetof(xl_btreetid, tid) + SizeOfIptrData)

/*
 * On insert with split we save items of both left and right siblings
 * and restore content of both pages from log record.  This way takes less
 * xlog space than the normal approach, because if we did it standardly,
 * XLogInsert would almost always think the right page is new and store its
 * whole page image.
 *
 * Note: the four XLOG_BTREE_SPLIT xl_info codes all use this data record.
 * The _L and _R variants indicate whether the inserted btitem went into the
 * left or right split page (and thus, whether otherblk is the right or left
 * page of the split pair).  The _ROOT variants indicate that we are splitting
 * the root page, and thus that a newroot record rather than an insert or
 * split record should follow.	Note that a split record never carries a
 * metapage update --- we'll do that in the parent-level update.
 */
typedef struct xl_btree_split
{
	xl_btreetid target;			/* inserted tuple id */
	BlockNumber otherblk;		/* second block participated in split: */
	/* first one is stored in target' tid */
	BlockNumber leftblk;		/* prev/left block */
	BlockNumber rightblk;		/* next/right block */
	uint32		level;			/* tree level of page being split */
	uint16		leftlen;		/* len of left page items below */
	/* LEFT AND RIGHT PAGES TUPLES FOLLOW AT THE END */
} xl_btree_split;

#define SizeOfBtreeSplit	(offsetof(xl_btree_split, leftlen) + sizeof(uint16))

/*
 * This is what we need to know about delete of individual leaf btitems.
 * The WAL record can represent deletion of any number of btitems on a
 * single index page.
 */
typedef struct xl_btree_delete
{
	RelFileNode node;
	BlockNumber block;
	/* TARGET OFFSET NUMBERS FOLLOW AT THE END */
} xl_btree_delete;

#define SizeOfBtreeDelete	(offsetof(xl_btree_delete, block) + sizeof(BlockNumber))

/*
 * This is what we need to know about deletion of a btree page.  The target
 * identifies the tuple removed from the parent page (note that we remove
 * this tuple's downlink and the *following* tuple's key).	Note we do not
 * store any content for the deleted page --- it is just rewritten as empty
 * during recovery.
 */
typedef struct xl_btree_delete_page
{
	xl_btreetid target;			/* deleted tuple id in parent page */
	BlockNumber deadblk;		/* child block being deleted */
	BlockNumber leftblk;		/* child block's left sibling, if any */
	BlockNumber rightblk;		/* child block's right sibling */
	/* xl_btree_metadata FOLLOWS IF XLOG_BTREE_DELETE_PAGE_META */
} xl_btree_delete_page;

#define SizeOfBtreeDeletePage	(offsetof(xl_btree_delete_page, rightblk) + sizeof(BlockNumber))

/*
 * New root log record.  There are zero btitems if this is to establish an
 * empty root, or two if it is the result of splitting an old root.
 *
 * Note that although this implies rewriting the metadata page, we don't need
 * an xl_btree_metadata record --- the rootblk and level are sufficient.
 */
typedef struct xl_btree_newroot
{
	RelFileNode node;
	BlockNumber rootblk;		/* location of new root */
	uint32		level;			/* its tree level */
	/* 0 or 2 BTITEMS FOLLOW AT END OF STRUCT */
} xl_btree_newroot;

#define SizeOfBtreeNewroot	(offsetof(xl_btree_newroot, level) + sizeof(uint32))

/*
 * New metapage log record.  This is not issued during routine operations;
 * it's only used when initializing an empty index and at completion of
 * index build.
 */
typedef struct xl_btree_newmeta
{
	RelFileNode node;
	xl_btree_metadata meta;
} xl_btree_newmeta;

#define SizeOfBtreeNewmeta	(sizeof(xl_btree_newmeta))

/*
 * New index page log record.  This is only used while building a new index.
 */
typedef struct xl_btree_newpage
{
	RelFileNode node;
	BlockNumber blkno;			/* location of new page */
	/* entire page contents follow at end of record */
} xl_btree_newpage;

#define SizeOfBtreeNewpage	(offsetof(xl_btree_newpage, blkno) + sizeof(BlockNumber))


/*
 *	Operator strategy numbers -- ordering of these is <, <=, =, >=, >
 */

#define BTLessStrategyNumber			1
#define BTLessEqualStrategyNumber		2
#define BTEqualStrategyNumber			3
#define BTGreaterEqualStrategyNumber	4
#define BTGreaterStrategyNumber			5
#define BTMaxStrategyNumber				5

/*
 *	When a new operator class is declared, we require that the user
 *	supply us with an amproc procedure for determining whether, for
 *	two keys a and b, a < b, a = b, or a > b.  This routine must
 *	return < 0, 0, > 0, respectively, in these three cases.  Since we
 *	only have one such proc in amproc, it's number 1.
 */

#define BTORDER_PROC	1

/*
 *	We need to be able to tell the difference between read and write
 *	requests for pages, in order to do locking correctly.
 */

#define BT_READ			BUFFER_LOCK_SHARE
#define BT_WRITE		BUFFER_LOCK_EXCLUSIVE

/*
 *	BTStackData -- As we descend a tree, we push the (location, downlink)
 *	pairs from internal pages onto a private stack.  If we split a
 *	leaf, we use this stack to walk back up the tree and insert data
 *	into parent pages (and possibly to split them, too).  Lehman and
 *	Yao's update algorithm guarantees that under no circumstances can
 *	our private stack give us an irredeemably bad picture up the tree.
 *	Again, see the paper for details.
 */

typedef struct BTStackData
{
	BlockNumber bts_blkno;
	OffsetNumber bts_offset;
	BTItemData	bts_btitem;
	struct BTStackData *bts_parent;
} BTStackData;

typedef BTStackData *BTStack;

/*
 *	BTScanOpaqueData is used to remember which buffers we're currently
 *	examining in the scan.	We keep these buffers pinned (but not locked,
 *	see nbtree.c) and recorded in the opaque entry of the scan to avoid
 *	doing a ReadBuffer() for every tuple in the index.
 *
 *	And it's used to remember actual scankey info (we need it
 *	if some scankeys evaled at runtime).
 *
 *	curHeapIptr & mrkHeapIptr are heap iptr-s from current/marked
 *	index tuples: we don't adjust scans on insertions (and, if LLL
 *	is ON, don't hold locks on index pages between passes) - we
 *	use these pointers to restore index scan positions...
 *		- vadim 07/29/98
 */

typedef struct BTScanOpaqueData
{
	Buffer		btso_curbuf;
	Buffer		btso_mrkbuf;
	ItemPointerData curHeapIptr;
	ItemPointerData mrkHeapIptr;
	/* these fields are set by _bt_orderkeys(), which see for more info: */
	bool		qual_ok;		/* false if qual can never be satisfied */
	int			numberOfKeys;	/* number of scan keys */
	int			numberOfRequiredKeys;	/* number of keys that must be
										 * matched to continue the scan */
	ScanKey		keyData;		/* array of scan keys */
} BTScanOpaqueData;

typedef BTScanOpaqueData *BTScanOpaque;

/*
 * prototypes for functions in nbtree.c (external entry points for btree)
 */
extern void AtEOXact_nbtree(void);

extern Datum btbuild(PG_FUNCTION_ARGS);
extern Datum btinsert(PG_FUNCTION_ARGS);
extern Datum btgettuple(PG_FUNCTION_ARGS);
extern Datum btbeginscan(PG_FUNCTION_ARGS);
extern Datum btrescan(PG_FUNCTION_ARGS);
extern void btmovescan(IndexScanDesc scan, Datum v);
extern Datum btendscan(PG_FUNCTION_ARGS);
extern Datum btmarkpos(PG_FUNCTION_ARGS);
extern Datum btrestrpos(PG_FUNCTION_ARGS);
extern Datum btbulkdelete(PG_FUNCTION_ARGS);
extern Datum btvacuumcleanup(PG_FUNCTION_ARGS);

/*
 * prototypes for functions in nbtinsert.c
 */
extern InsertIndexResult _bt_doinsert(Relation rel, BTItem btitem,
			 bool index_is_unique, Relation heapRel);
extern Buffer _bt_getstackbuf(Relation rel, BTStack stack, int access);
extern void _bt_insert_parent(Relation rel, Buffer buf, Buffer rbuf,
				  BTStack stack, bool is_root, bool is_only);

/*
 * prototypes for functions in nbtpage.c
 */
extern void _bt_metapinit(Relation rel, bool markvalid);
extern Buffer _bt_getroot(Relation rel, int access);
extern Buffer _bt_gettrueroot(Relation rel);
extern Buffer _bt_getbuf(Relation rel, BlockNumber blkno, int access);
extern void _bt_relbuf(Relation rel, Buffer buf);
extern void _bt_wrtbuf(Relation rel, Buffer buf);
extern void _bt_wrtnorelbuf(Relation rel, Buffer buf);
extern void _bt_pageinit(Page page, Size size);
extern bool _bt_page_recyclable(Page page);
extern void _bt_metaproot(Relation rel, BlockNumber rootbknum, uint32 level);
extern void _bt_delitems(Relation rel, Buffer buf,
			 OffsetNumber *itemnos, int nitems);
extern int	_bt_pagedel(Relation rel, Buffer buf, bool vacuum_full);

/*
 * prototypes for functions in nbtsearch.c
 */
extern BTStack _bt_search(Relation rel, int keysz, ScanKey scankey,
		   Buffer *bufP, int access);
extern Buffer _bt_moveright(Relation rel, Buffer buf, int keysz,
			  ScanKey scankey, int access);
extern OffsetNumber _bt_binsrch(Relation rel, Buffer buf, int keysz,
			ScanKey scankey);
extern int32 _bt_compare(Relation rel, int keysz, ScanKey scankey,
			Page page, OffsetNumber offnum);
extern bool _bt_next(IndexScanDesc scan, ScanDirection dir);
extern bool _bt_first(IndexScanDesc scan, ScanDirection dir);
extern bool _bt_step(IndexScanDesc scan, Buffer *bufP, ScanDirection dir);
extern Buffer _bt_get_endpoint(Relation rel, uint32 level, bool rightmost);

/*
 * prototypes for functions in nbtstrat.c
 */
extern StrategyNumber _bt_getstrat(Relation rel, AttrNumber attno,
			 RegProcedure proc);

/*
 * prototypes for functions in nbtutils.c
 */
extern ScanKey _bt_mkscankey(Relation rel, IndexTuple itup);
extern ScanKey _bt_mkscankey_nodata(Relation rel);
extern void _bt_freeskey(ScanKey skey);
extern void _bt_freestack(BTStack stack);
extern void _bt_orderkeys(IndexScanDesc scan);
extern bool _bt_checkkeys(IndexScanDesc scan, IndexTuple tuple,
			  ScanDirection dir, bool *continuescan);
extern BTItem _bt_formitem(IndexTuple itup);

/*
 * prototypes for functions in nbtsort.c
 */
typedef struct BTSpool BTSpool; /* opaque type known only within nbtsort.c */

extern BTSpool *_bt_spoolinit(Relation index, bool isunique);
extern void _bt_spooldestroy(BTSpool *btspool);
extern void _bt_spool(BTItem btitem, BTSpool *btspool);
extern void _bt_leafbuild(BTSpool *btspool, BTSpool *spool2);

/*
 * prototypes for functions in nbtxlog.c
 */
extern void btree_redo(XLogRecPtr lsn, XLogRecord *record);
extern void btree_undo(XLogRecPtr lsn, XLogRecord *record);
extern void btree_desc(char *buf, uint8 xl_info, char *rec);
extern void btree_xlog_startup(void);
extern void btree_xlog_cleanup(void);

#endif   /* NBTREE_H */
