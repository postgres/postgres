/*-------------------------------------------------------------------------
 *
 * nbtree.h
 *	  header file for postgres btree access method implementation.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/nbtree.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NBTREE_H
#define NBTREE_H

#include "access/genam.h"
#include "access/itup.h"
#include "access/sdir.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "catalog/pg_index.h"

/* There's room for a 16-bit vacuum cycle ID in BTPageOpaqueData */
typedef uint16 BTCycleId;

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
 *	We also store a "vacuum cycle ID".	When a page is split while VACUUM is
 *	processing the index, a nonzero value associated with the VACUUM run is
 *	stored into both halves of the split page.	(If VACUUM is not running,
 *	both pages receive zero cycleids.)	This allows VACUUM to detect whether
 *	a page was split since it started, with a small probability of false match
 *	if the page was last split some exact multiple of MAX_BT_CYCLE_ID VACUUMs
 *	ago.  Also, during a split, the BTP_SPLIT_END flag is cleared in the left
 *	(original) page, and set in the right page, but only if the next page
 *	to its right has a different cycleid.
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
	BTCycleId	btpo_cycleid;	/* vacuum cycle ID of latest split */
} BTPageOpaqueData;

typedef BTPageOpaqueData *BTPageOpaque;

/* Bits defined in btpo_flags */
#define BTP_LEAF		(1 << 0)	/* leaf page, i.e. not internal page */
#define BTP_ROOT		(1 << 1)	/* root page (has no parent) */
#define BTP_DELETED		(1 << 2)	/* page has been deleted from tree */
#define BTP_META		(1 << 3)	/* meta-page */
#define BTP_HALF_DEAD	(1 << 4)	/* empty, but still in tree */
#define BTP_SPLIT_END	(1 << 5)	/* rightmost page of split group */
#define BTP_HAS_GARBAGE (1 << 6)	/* page has LP_DEAD tuples */

/*
 * The max allowed value of a cycle ID is a bit less than 64K.	This is
 * for convenience of pg_filedump and similar utilities: we want to use
 * the last 2 bytes of special space as an index type indicator, and
 * restricting cycle ID lets btree use that space for vacuum cycle IDs
 * while still allowing index type to be identified.
 */
#define MAX_BT_CYCLE_ID		0xFF7F


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
 * Maximum size of a btree index entry, including its tuple header.
 *
 * We actually need to be able to fit three items on every page,
 * so restrict any one item to 1/3 the per-page available space.
 */
#define BTMaxItemSize(page) \
	MAXALIGN_DOWN((PageGetPageSize(page) - \
				   MAXALIGN(SizeOfPageHeaderData + 3*sizeof(ItemIdData)) - \
				   MAXALIGN(sizeof(BTPageOpaqueData))) / 3)

/*
 * The leaf-page fillfactor defaults to 90% but is user-adjustable.
 * For pages above the leaf level, we use a fixed 70% fillfactor.
 * The fillfactor is applied during index build and when splitting
 * a rightmost page; when splitting non-rightmost pages we try to
 * divide the data equally.
 */
#define BTREE_MIN_FILLFACTOR		10
#define BTREE_DEFAULT_FILLFACTOR	90
#define BTREE_NONLEAF_FILLFACTOR	70

/*
 *	Test whether two btree entries are "the same".
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
 *	are unique, not in ALL INDEX. So, we can use the t_tid
 *	as unique identifier for a given index tuple (logical position
 *	within a level). - vadim 04/09/97
 */
#define BTTidSame(i1, i2)	\
	( (i1).ip_blkid.bi_hi == (i2).ip_blkid.bi_hi && \
	  (i1).ip_blkid.bi_lo == (i2).ip_blkid.bi_lo && \
	  (i1).ip_posid == (i2).ip_posid )
#define BTEntrySame(i1, i2) \
	BTTidSame((i1)->t_tid, (i2)->t_tid)


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
#define P_ISHALFDEAD(opaque)	((opaque)->btpo_flags & BTP_HALF_DEAD)
#define P_IGNORE(opaque)		((opaque)->btpo_flags & (BTP_DELETED|BTP_HALF_DEAD))
#define P_HAS_GARBAGE(opaque)	((opaque)->btpo_flags & BTP_HAS_GARBAGE)

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
#define XLOG_BTREE_INSERT_LEAF	0x00	/* add index tuple without split */
#define XLOG_BTREE_INSERT_UPPER 0x10	/* same, on a non-leaf page */
#define XLOG_BTREE_INSERT_META	0x20	/* same, plus update metapage */
#define XLOG_BTREE_SPLIT_L		0x30	/* add index tuple with split */
#define XLOG_BTREE_SPLIT_R		0x40	/* as above, new item on right */
#define XLOG_BTREE_SPLIT_L_ROOT 0x50	/* add tuple with split of root */
#define XLOG_BTREE_SPLIT_R_ROOT 0x60	/* as above, new item on right */
#define XLOG_BTREE_DELETE		0x70	/* delete leaf index tuples for a page */
#define XLOG_BTREE_DELETE_PAGE	0x80	/* delete an entire page */
#define XLOG_BTREE_DELETE_PAGE_META 0x90		/* same, and update metapage */
#define XLOG_BTREE_NEWROOT		0xA0	/* new root page */
#define XLOG_BTREE_DELETE_PAGE_HALF 0xB0		/* page deletion that makes
												 * parent half-dead */
#define XLOG_BTREE_VACUUM		0xC0	/* delete entries on a page during
										 * vacuum */
#define XLOG_BTREE_REUSE_PAGE	0xD0	/* old page is about to be reused from
										 * FSM */

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
	/* BlockNumber downlink field FOLLOWS IF NOT XLOG_BTREE_INSERT_LEAF */
	/* xl_btree_metadata FOLLOWS IF XLOG_BTREE_INSERT_META */
	/* INDEX TUPLE FOLLOWS AT END OF STRUCT */
} xl_btree_insert;

#define SizeOfBtreeInsert	(offsetof(xl_btreetid, tid) + SizeOfIptrData)

/*
 * On insert with split, we save all the items going into the right sibling
 * so that we can restore it completely from the log record.  This way takes
 * less xlog space than the normal approach, because if we did it standardly,
 * XLogInsert would almost always think the right page is new and store its
 * whole page image.  The left page, however, is handled in the normal
 * incremental-update fashion.
 *
 * Note: the four XLOG_BTREE_SPLIT xl_info codes all use this data record.
 * The _L and _R variants indicate whether the inserted tuple went into the
 * left or right split page (and thus, whether newitemoff and the new item
 * are stored or not).	The _ROOT variants indicate that we are splitting
 * the root page, and thus that a newroot record rather than an insert or
 * split record should follow.	Note that a split record never carries a
 * metapage update --- we'll do that in the parent-level update.
 */
typedef struct xl_btree_split
{
	RelFileNode node;
	BlockNumber leftsib;		/* orig page / new left page */
	BlockNumber rightsib;		/* new right page */
	BlockNumber rnext;			/* next block (orig page's rightlink) */
	uint32		level;			/* tree level of page being split */
	OffsetNumber firstright;	/* first item moved to right page */

	/*
	 * If level > 0, BlockIdData downlink follows.	(We use BlockIdData rather
	 * than BlockNumber for alignment reasons: SizeOfBtreeSplit is only 16-bit
	 * aligned.)
	 *
	 * If level > 0, an IndexTuple representing the HIKEY of the left page
	 * follows.  We don't need this on leaf pages, because it's the same as
	 * the leftmost key in the new right page.	Also, it's suppressed if
	 * XLogInsert chooses to store the left page's whole page image.
	 *
	 * In the _L variants, next are OffsetNumber newitemoff and the new item.
	 * (In the _R variants, the new item is one of the right page's tuples.)
	 * The new item, but not newitemoff, is suppressed if XLogInsert chooses
	 * to store the left page's whole page image.
	 *
	 * Last are the right page's tuples in the form used by _bt_restore_page.
	 */
} xl_btree_split;

#define SizeOfBtreeSplit	(offsetof(xl_btree_split, firstright) + sizeof(OffsetNumber))

/*
 * This is what we need to know about delete of individual leaf index tuples.
 * The WAL record can represent deletion of any number of index tuples on a
 * single index page when *not* executed by VACUUM.
 */
typedef struct xl_btree_delete
{
	RelFileNode node;			/* RelFileNode of the index */
	BlockNumber block;
	RelFileNode hnode;			/* RelFileNode of the heap the index currently
								 * points at */
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
 * The correctness requirement for applying these changes during recovery is
 * that we must do one of these two things for every block in the index:
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
	RelFileNode node;
	BlockNumber block;
	BlockNumber lastBlockVacuumed;

	/* TARGET OFFSET NUMBERS FOLLOW */
} xl_btree_vacuum;

#define SizeOfBtreeVacuum	(offsetof(xl_btree_vacuum, lastBlockVacuumed) + sizeof(BlockNumber))

/*
 * This is what we need to know about deletion of a btree page.  The target
 * identifies the tuple removed from the parent page (note that we remove
 * this tuple's downlink and the *following* tuple's key).	Note we do not
 * store any content for the deleted page --- it is just rewritten as empty
 * during recovery, apart from resetting the btpo.xact.
 */
typedef struct xl_btree_delete_page
{
	xl_btreetid target;			/* deleted tuple id in parent page */
	BlockNumber deadblk;		/* child block being deleted */
	BlockNumber leftblk;		/* child block's left sibling, if any */
	BlockNumber rightblk;		/* child block's right sibling */
	TransactionId btpo_xact;	/* value of btpo.xact for use in recovery */
	/* xl_btree_metadata FOLLOWS IF XLOG_BTREE_DELETE_PAGE_META */
} xl_btree_delete_page;

#define SizeOfBtreeDeletePage	(offsetof(xl_btree_delete_page, btpo_xact) + sizeof(TransactionId))

/*
 * New root log record.  There are zero tuples if this is to establish an
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
	/* 0 or 2 INDEX TUPLES FOLLOW AT END OF STRUCT */
} xl_btree_newroot;

#define SizeOfBtreeNewroot	(offsetof(xl_btree_newroot, level) + sizeof(uint32))


/*
 *	Operator strategy numbers for B-tree have been moved to access/skey.h,
 *	because many places need to use them in ScanKeyInit() calls.
 *
 *	The strategy numbers are chosen so that we can commute them by
 *	subtraction, thus:
 */
#define BTCommuteStrategyNumber(strat)	(BTMaxStrategyNumber + 1 - (strat))

/*
 *	When a new operator class is declared, we require that the user
 *	supply us with an amproc procedure (BTORDER_PROC) for determining
 *	whether, for two keys a and b, a < b, a = b, or a > b.	This routine
 *	must return < 0, 0, > 0, respectively, in these three cases.  (It must
 *	not return INT_MIN, since we may negate the result before using it.)
 *
 *	To facilitate accelerated sorting, an operator class may choose to
 *	offer a second procedure (BTSORTSUPPORT_PROC).	For full details, see
 *	src/include/utils/sortsupport.h.
 */

#define BTORDER_PROC		1
#define BTSORTSUPPORT_PROC	2

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
	IndexTupleData bts_btentry;
	struct BTStackData *bts_parent;
} BTStackData;

typedef BTStackData *BTStack;

/*
 * BTScanOpaqueData is the btree-private state needed for an indexscan.
 * This consists of preprocessed scan keys (see _bt_preprocess_keys() for
 * details of the preprocessing), information about the current location
 * of the scan, and information about the marked location, if any.	(We use
 * BTScanPosData to represent the data needed for each of current and marked
 * locations.)	In addition we can remember some known-killed index entries
 * that must be marked before we can move off the current page.
 *
 * Index scans work a page at a time: we pin and read-lock the page, identify
 * all the matching items on the page and save them in BTScanPosData, then
 * release the read-lock while returning the items to the caller for
 * processing.	This approach minimizes lock/unlock traffic.  Note that we
 * keep the pin on the index page until the caller is done with all the items
 * (this is needed for VACUUM synchronization, see nbtree/README).	When we
 * are ready to step to the next page, if the caller has told us any of the
 * items were killed, we re-lock the page to mark them killed, then unlock.
 * Finally we drop the pin and step to the next page in the appropriate
 * direction.
 *
 * If we are doing an index-only scan, we save the entire IndexTuple for each
 * matched item, otherwise only its heap TID and offset.  The IndexTuples go
 * into a separate workspace array; each BTScanPosItem stores its tuple's
 * offset within that array.
 */

typedef struct BTScanPosItem	/* what we remember about each match */
{
	ItemPointerData heapTid;	/* TID of referenced heap item */
	OffsetNumber indexOffset;	/* index item's location within page */
	LocationIndex tupleOffset;	/* IndexTuple's offset in workspace, if any */
} BTScanPosItem;

typedef struct BTScanPosData
{
	Buffer		buf;			/* if valid, the buffer is pinned */

	BlockNumber nextPage;		/* page's right link when we scanned it */

	/*
	 * moreLeft and moreRight track whether we think there may be matching
	 * index entries to the left and right of the current page, respectively.
	 * We can clear the appropriate one of these flags when _bt_checkkeys()
	 * returns continuescan = false.
	 */
	bool		moreLeft;
	bool		moreRight;

	/*
	 * If we are doing an index-only scan, nextTupleOffset is the first free
	 * location in the associated tuple storage workspace.
	 */
	int			nextTupleOffset;

	/*
	 * The items array is always ordered in index order (ie, increasing
	 * indexoffset).  When scanning backwards it is convenient to fill the
	 * array back-to-front, so we start at the last slot and fill downwards.
	 * Hence we need both a first-valid-entry and a last-valid-entry counter.
	 * itemIndex is a cursor showing which entry was last returned to caller.
	 */
	int			firstItem;		/* first valid index in items[] */
	int			lastItem;		/* last valid index in items[] */
	int			itemIndex;		/* current index in items[] */

	BTScanPosItem items[MaxIndexTuplesPerPage]; /* MUST BE LAST */
} BTScanPosData;

typedef BTScanPosData *BTScanPos;

#define BTScanPosIsValid(scanpos) BufferIsValid((scanpos).buf)

/* We need one of these for each equality-type SK_SEARCHARRAY scan key */
typedef struct BTArrayKeyInfo
{
	int			scan_key;		/* index of associated key in arrayKeyData */
	int			cur_elem;		/* index of current element in elem_values */
	int			mark_elem;		/* index of marked element in elem_values */
	int			num_elems;		/* number of elems in current array value */
	Datum	   *elem_values;	/* array of num_elems Datums */
} BTArrayKeyInfo;

typedef struct BTScanOpaqueData
{
	/* these fields are set by _bt_preprocess_keys(): */
	bool		qual_ok;		/* false if qual can never be satisfied */
	int			numberOfKeys;	/* number of preprocessed scan keys */
	ScanKey		keyData;		/* array of preprocessed scan keys */

	/* workspace for SK_SEARCHARRAY support */
	ScanKey		arrayKeyData;	/* modified copy of scan->keyData */
	int			numArrayKeys;	/* number of equality-type array keys (-1 if
								 * there are any unsatisfiable array keys) */
	BTArrayKeyInfo *arrayKeys;	/* info about each equality-type array key */
	MemoryContext arrayContext; /* scan-lifespan context for array data */

	/* info about killed items if any (killedItems is NULL if never used) */
	int		   *killedItems;	/* currPos.items indexes of killed items */
	int			numKilled;		/* number of currently stored items */

	/*
	 * If we are doing an index-only scan, these are the tuple storage
	 * workspaces for the currPos and markPos respectively.  Each is of size
	 * BLCKSZ, so it can hold as much as a full page's worth of tuples.
	 */
	char	   *currTuples;		/* tuple storage for currPos */
	char	   *markTuples;		/* tuple storage for markPos */

	/*
	 * If the marked position is on the same page as current position, we
	 * don't use markPos, but just keep the marked itemIndex in markItemIndex
	 * (all the rest of currPos is valid for the mark position). Hence, to
	 * determine if there is a mark, first look at markItemIndex, then at
	 * markPos.
	 */
	int			markItemIndex;	/* itemIndex, or -1 if not valid */

	/* keep these last in struct for efficiency */
	BTScanPosData currPos;		/* current position data */
	BTScanPosData markPos;		/* marked position, if any */
} BTScanOpaqueData;

typedef BTScanOpaqueData *BTScanOpaque;

/*
 * We use some private sk_flags bits in preprocessed scan keys.  We're allowed
 * to use bits 16-31 (see skey.h).	The uppermost bits are copied from the
 * index's indoption[] array entry for the index attribute.
 */
#define SK_BT_REQFWD	0x00010000		/* required to continue forward scan */
#define SK_BT_REQBKWD	0x00020000		/* required to continue backward scan */
#define SK_BT_INDOPTION_SHIFT  24		/* must clear the above bits */
#define SK_BT_DESC			(INDOPTION_DESC << SK_BT_INDOPTION_SHIFT)
#define SK_BT_NULLS_FIRST	(INDOPTION_NULLS_FIRST << SK_BT_INDOPTION_SHIFT)

/*
 * prototypes for functions in nbtree.c (external entry points for btree)
 */
extern Datum btbuild(PG_FUNCTION_ARGS);
extern Datum btbuildempty(PG_FUNCTION_ARGS);
extern Datum btinsert(PG_FUNCTION_ARGS);
extern Datum btbeginscan(PG_FUNCTION_ARGS);
extern Datum btgettuple(PG_FUNCTION_ARGS);
extern Datum btgetbitmap(PG_FUNCTION_ARGS);
extern Datum btrescan(PG_FUNCTION_ARGS);
extern Datum btendscan(PG_FUNCTION_ARGS);
extern Datum btmarkpos(PG_FUNCTION_ARGS);
extern Datum btrestrpos(PG_FUNCTION_ARGS);
extern Datum btbulkdelete(PG_FUNCTION_ARGS);
extern Datum btvacuumcleanup(PG_FUNCTION_ARGS);
extern Datum btcanreturn(PG_FUNCTION_ARGS);
extern Datum btoptions(PG_FUNCTION_ARGS);

/*
 * prototypes for functions in nbtinsert.c
 */
extern bool _bt_doinsert(Relation rel, IndexTuple itup,
			 IndexUniqueCheck checkUnique, Relation heapRel);
extern Buffer _bt_getstackbuf(Relation rel, BTStack stack, int access);
extern void _bt_insert_parent(Relation rel, Buffer buf, Buffer rbuf,
				  BTStack stack, bool is_root, bool is_only);

/*
 * prototypes for functions in nbtpage.c
 */
extern void _bt_initmetapage(Page page, BlockNumber rootbknum, uint32 level);
extern Buffer _bt_getroot(Relation rel, int access);
extern Buffer _bt_gettrueroot(Relation rel);
extern void _bt_checkpage(Relation rel, Buffer buf);
extern Buffer _bt_getbuf(Relation rel, BlockNumber blkno, int access);
extern Buffer _bt_relandgetbuf(Relation rel, Buffer obuf,
				 BlockNumber blkno, int access);
extern void _bt_relbuf(Relation rel, Buffer buf);
extern void _bt_pageinit(Page page, Size size);
extern bool _bt_page_recyclable(Page page);
extern void _bt_delitems_delete(Relation rel, Buffer buf,
					OffsetNumber *itemnos, int nitems, Relation heapRel);
extern void _bt_delitems_vacuum(Relation rel, Buffer buf,
					OffsetNumber *itemnos, int nitems,
					BlockNumber lastBlockVacuumed);
extern int	_bt_pagedel(Relation rel, Buffer buf, BTStack stack);

/*
 * prototypes for functions in nbtsearch.c
 */
extern BTStack _bt_search(Relation rel,
		   int keysz, ScanKey scankey, bool nextkey,
		   Buffer *bufP, int access);
extern Buffer _bt_moveright(Relation rel, Buffer buf, int keysz,
			  ScanKey scankey, bool nextkey, int access);
extern OffsetNumber _bt_binsrch(Relation rel, Buffer buf, int keysz,
			ScanKey scankey, bool nextkey);
extern int32 _bt_compare(Relation rel, int keysz, ScanKey scankey,
			Page page, OffsetNumber offnum);
extern bool _bt_first(IndexScanDesc scan, ScanDirection dir);
extern bool _bt_next(IndexScanDesc scan, ScanDirection dir);
extern Buffer _bt_get_endpoint(Relation rel, uint32 level, bool rightmost);

/*
 * prototypes for functions in nbtutils.c
 */
extern ScanKey _bt_mkscankey(Relation rel, IndexTuple itup);
extern ScanKey _bt_mkscankey_nodata(Relation rel);
extern void _bt_freeskey(ScanKey skey);
extern void _bt_freestack(BTStack stack);
extern void _bt_preprocess_array_keys(IndexScanDesc scan);
extern void _bt_start_array_keys(IndexScanDesc scan, ScanDirection dir);
extern bool _bt_advance_array_keys(IndexScanDesc scan, ScanDirection dir);
extern void _bt_mark_array_keys(IndexScanDesc scan);
extern void _bt_restore_array_keys(IndexScanDesc scan);
extern void _bt_preprocess_keys(IndexScanDesc scan);
extern IndexTuple _bt_checkkeys(IndexScanDesc scan,
			  Page page, OffsetNumber offnum,
			  ScanDirection dir, bool *continuescan);
extern void _bt_killitems(IndexScanDesc scan, bool haveLock);
extern BTCycleId _bt_vacuum_cycleid(Relation rel);
extern BTCycleId _bt_start_vacuum(Relation rel);
extern void _bt_end_vacuum(Relation rel);
extern void _bt_end_vacuum_callback(int code, Datum arg);
extern Size BTreeShmemSize(void);
extern void BTreeShmemInit(void);

/*
 * prototypes for functions in nbtsort.c
 */
typedef struct BTSpool BTSpool; /* opaque type known only within nbtsort.c */

extern BTSpool *_bt_spoolinit(Relation index, bool isunique, bool isdead);
extern void _bt_spooldestroy(BTSpool *btspool);
extern void _bt_spool(IndexTuple itup, BTSpool *btspool);
extern void _bt_leafbuild(BTSpool *btspool, BTSpool *spool2);

/*
 * prototypes for functions in nbtxlog.c
 */
extern void btree_redo(XLogRecPtr lsn, XLogRecord *record);
extern void btree_desc(StringInfo buf, uint8 xl_info, char *rec);
extern void btree_xlog_startup(void);
extern void btree_xlog_cleanup(void);
extern bool btree_safe_restartpoint(void);

#endif   /* NBTREE_H */
