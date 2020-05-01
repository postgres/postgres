/*-------------------------------------------------------------------------
 *
 * nbtree.h
 *	  header file for postgres btree access method implementation.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/nbtree.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NBTREE_H
#define NBTREE_H

#include "access/amapi.h"
#include "access/itup.h"
#include "access/sdir.h"
#include "access/xlogreader.h"
#include "catalog/pg_index.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"
#include "storage/shm_toc.h"

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
 *	We also store a "vacuum cycle ID".  When a page is split while VACUUM is
 *	processing the index, a nonzero value associated with the VACUUM run is
 *	stored into both halves of the split page.  (If VACUUM is not running,
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
#define BTP_INCOMPLETE_SPLIT (1 << 7)	/* right sibling's downlink is missing */

/*
 * The max allowed value of a cycle ID is a bit less than 64K.  This is
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
	uint32		btm_version;	/* nbtree version (always <= BTREE_VERSION) */
	BlockNumber btm_root;		/* current root location */
	uint32		btm_level;		/* tree level of the root page */
	BlockNumber btm_fastroot;	/* current "fast" root location */
	uint32		btm_fastlevel;	/* tree level of the "fast" root page */
	/* remaining fields only valid when btm_version >= BTREE_NOVAC_VERSION */
	TransactionId btm_oldest_btpo_xact; /* oldest btpo_xact among all deleted
										 * pages */
	float8		btm_last_cleanup_num_heap_tuples;	/* number of heap tuples
													 * during last cleanup */
} BTMetaPageData;

#define BTPageGetMeta(p) \
	((BTMetaPageData *) PageGetContents(p))

/*
 * The current Btree version is 4.  That's what you'll get when you create
 * a new index.
 *
 * Btree version 3 was used in PostgreSQL v11.  It is mostly the same as
 * version 4, but heap TIDs were not part of the keyspace.  Index tuples
 * with duplicate keys could be stored in any order.  We continue to
 * support reading and writing Btree versions 2 and 3, so that they don't
 * need to be immediately re-indexed at pg_upgrade.  In order to get the
 * new heapkeyspace semantics, however, a REINDEX is needed.
 *
 * Btree version 2 is mostly the same as version 3.  There are two new
 * fields in the metapage that were introduced in version 3.  A version 2
 * metapage will be automatically upgraded to version 3 on the first
 * insert to it.  INCLUDE indexes cannot use version 2.
 */
#define BTREE_METAPAGE	0		/* first page is meta */
#define BTREE_MAGIC		0x053162	/* magic number in metapage */
#define BTREE_VERSION	4		/* current version number */
#define BTREE_MIN_VERSION	2	/* minimal supported version number */
#define BTREE_NOVAC_VERSION	3	/* minimal version with all meta fields */

/*
 * Maximum size of a btree index entry, including its tuple header.
 *
 * We actually need to be able to fit three items on every page,
 * so restrict any one item to 1/3 the per-page available space.
 *
 * There are rare cases where _bt_truncate() will need to enlarge
 * a heap index tuple to make space for a tiebreaker heap TID
 * attribute, which we account for here.
 */
#define BTMaxItemSize(page) \
	MAXALIGN_DOWN((PageGetPageSize(page) - \
				   MAXALIGN(SizeOfPageHeaderData + \
							3*sizeof(ItemIdData)  + \
							3*sizeof(ItemPointerData)) - \
				   MAXALIGN(sizeof(BTPageOpaqueData))) / 3)
#define BTMaxItemSizeNoHeapTid(page) \
	MAXALIGN_DOWN((PageGetPageSize(page) - \
				   MAXALIGN(SizeOfPageHeaderData + 3*sizeof(ItemIdData)) - \
				   MAXALIGN(sizeof(BTPageOpaqueData))) / 3)

/*
 * The leaf-page fillfactor defaults to 90% but is user-adjustable.
 * For pages above the leaf level, we use a fixed 70% fillfactor.
 * The fillfactor is applied during index build and when splitting
 * a rightmost page; when splitting non-rightmost pages we try to
 * divide the data equally.  When splitting a page that's entirely
 * filled with a single value (duplicates), the effective leaf-page
 * fillfactor is 96%, regardless of whether the page is a rightmost
 * page.
 */
#define BTREE_MIN_FILLFACTOR		10
#define BTREE_DEFAULT_FILLFACTOR	90
#define BTREE_NONLEAF_FILLFACTOR	70
#define BTREE_SINGLEVAL_FILLFACTOR	96

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
#define P_ISLEAF(opaque)		(((opaque)->btpo_flags & BTP_LEAF) != 0)
#define P_ISROOT(opaque)		(((opaque)->btpo_flags & BTP_ROOT) != 0)
#define P_ISDELETED(opaque)		(((opaque)->btpo_flags & BTP_DELETED) != 0)
#define P_ISMETA(opaque)		(((opaque)->btpo_flags & BTP_META) != 0)
#define P_ISHALFDEAD(opaque)	(((opaque)->btpo_flags & BTP_HALF_DEAD) != 0)
#define P_IGNORE(opaque)		(((opaque)->btpo_flags & (BTP_DELETED|BTP_HALF_DEAD)) != 0)
#define P_HAS_GARBAGE(opaque)	(((opaque)->btpo_flags & BTP_HAS_GARBAGE) != 0)
#define P_INCOMPLETE_SPLIT(opaque)	(((opaque)->btpo_flags & BTP_INCOMPLETE_SPLIT) != 0)

/*
 *	Lehman and Yao's algorithm requires a ``high key'' on every non-rightmost
 *	page.  The high key is not a tuple that is used to visit the heap.  It is
 *	a pivot tuple (see "Notes on B-Tree tuple format" below for definition).
 *	The high key on a page is required to be greater than or equal to any
 *	other key that appears on the page.  If we find ourselves trying to
 *	insert a key that is strictly > high key, we know we need to move right
 *	(this should only happen if the page was split since we examined the
 *	parent page).
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
 * Notes on B-Tree tuple format, and key and non-key attributes:
 *
 * INCLUDE B-Tree indexes have non-key attributes.  These are extra
 * attributes that may be returned by index-only scans, but do not influence
 * the order of items in the index (formally, non-key attributes are not
 * considered to be part of the key space).  Non-key attributes are only
 * present in leaf index tuples whose item pointers actually point to heap
 * tuples (non-pivot tuples).  _bt_check_natts() enforces the rules
 * described here.
 *
 * Non-pivot tuple format:
 *
 *  t_tid | t_info | key values | INCLUDE columns, if any
 *
 * t_tid points to the heap TID, which is a tiebreaker key column as of
 * BTREE_VERSION 4.  Currently, the INDEX_ALT_TID_MASK status bit is never
 * set for non-pivot tuples.
 *
 * All other types of index tuples ("pivot" tuples) only have key columns,
 * since pivot tuples only exist to represent how the key space is
 * separated.  In general, any B-Tree index that has more than one level
 * (i.e. any index that does not just consist of a metapage and a single
 * leaf root page) must have some number of pivot tuples, since pivot
 * tuples are used for traversing the tree.  Suffix truncation can omit
 * trailing key columns when a new pivot is formed, which makes minus
 * infinity their logical value.  Since BTREE_VERSION 4 indexes treat heap
 * TID as a trailing key column that ensures that all index tuples are
 * physically unique, it is necessary to represent heap TID as a trailing
 * key column in pivot tuples, though very often this can be truncated
 * away, just like any other key column. (Actually, the heap TID is
 * omitted rather than truncated, since its representation is different to
 * the non-pivot representation.)
 *
 * Pivot tuple format:
 *
 *  t_tid | t_info | key values | [heap TID]
 *
 * We store the number of columns present inside pivot tuples by abusing
 * their t_tid offset field, since pivot tuples never need to store a real
 * offset (downlinks only need to store a block number in t_tid).  The
 * offset field only stores the number of columns/attributes when the
 * INDEX_ALT_TID_MASK bit is set, which doesn't count the trailing heap
 * TID column sometimes stored in pivot tuples -- that's represented by
 * the presence of BT_HEAP_TID_ATTR.  The INDEX_ALT_TID_MASK bit in t_info
 * is always set on BTREE_VERSION 4.  BT_HEAP_TID_ATTR can only be set on
 * BTREE_VERSION 4.
 *
 * In version 3 indexes, the INDEX_ALT_TID_MASK flag might not be set in
 * pivot tuples.  In that case, the number of key columns is implicitly
 * the same as the number of key columns in the index.  It is not usually
 * set on version 2 indexes, which predate the introduction of INCLUDE
 * indexes.  (Only explicitly truncated pivot tuples explicitly represent
 * the number of key columns on versions 2 and 3, whereas all pivot tuples
 * are formed using truncation on version 4.  A version 2 index will have
 * it set for an internal page negative infinity item iff internal page
 * split occurred after upgrade to Postgres 11+.)
 *
 * The 12 least significant offset bits from t_tid are used to represent
 * the number of columns in INDEX_ALT_TID_MASK tuples, leaving 4 status
 * bits (BT_RESERVED_OFFSET_MASK bits), 3 of which that are reserved for
 * future use.  BT_N_KEYS_OFFSET_MASK should be large enough to store any
 * number of columns/attributes <= INDEX_MAX_KEYS.
 *
 * Note well: The macros that deal with the number of attributes in tuples
 * assume that a tuple with INDEX_ALT_TID_MASK set must be a pivot tuple,
 * and that a tuple without INDEX_ALT_TID_MASK set must be a non-pivot
 * tuple (or must have the same number of attributes as the index has
 * generally in the case of !heapkeyspace indexes).  They will need to be
 * updated if non-pivot tuples ever get taught to use INDEX_ALT_TID_MASK
 * for something else.
 */
#define INDEX_ALT_TID_MASK			INDEX_AM_RESERVED_BIT

/* Item pointer offset bits */
#define BT_RESERVED_OFFSET_MASK		0xF000
#define BT_N_KEYS_OFFSET_MASK		0x0FFF
#define BT_HEAP_TID_ATTR			0x1000

/* Get/set downlink block number */
#define BTreeInnerTupleGetDownLink(itup) \
	ItemPointerGetBlockNumberNoCheck(&((itup)->t_tid))
#define BTreeInnerTupleSetDownLink(itup, blkno) \
	ItemPointerSetBlockNumber(&((itup)->t_tid), (blkno))

/*
 * Get/set leaf page highkey's link. During the second phase of deletion, the
 * target leaf page's high key may point to an ancestor page (at all other
 * times, the leaf level high key's link is not used).  See the nbtree README
 * for full details.
 */
#define BTreeTupleGetTopParent(itup) \
	ItemPointerGetBlockNumberNoCheck(&((itup)->t_tid))
#define BTreeTupleSetTopParent(itup, blkno)	\
	do { \
		ItemPointerSetBlockNumber(&((itup)->t_tid), (blkno)); \
		BTreeTupleSetNAtts((itup), 0); \
	} while(0)

/*
 * Get/set number of attributes within B-tree index tuple.
 *
 * Note that this does not include an implicit tiebreaker heap TID
 * attribute, if any.  Note also that the number of key attributes must be
 * explicitly represented in all heapkeyspace pivot tuples.
 */
#define BTreeTupleGetNAtts(itup, rel)	\
	( \
		(itup)->t_info & INDEX_ALT_TID_MASK ? \
		( \
			ItemPointerGetOffsetNumberNoCheck(&(itup)->t_tid) & BT_N_KEYS_OFFSET_MASK \
		) \
		: \
		IndexRelationGetNumberOfAttributes(rel) \
	)
#define BTreeTupleSetNAtts(itup, n) \
	do { \
		(itup)->t_info |= INDEX_ALT_TID_MASK; \
		ItemPointerSetOffsetNumber(&(itup)->t_tid, (n) & BT_N_KEYS_OFFSET_MASK); \
	} while(0)

/*
 * Get tiebreaker heap TID attribute, if any.  Macro works with both pivot
 * and non-pivot tuples, despite differences in how heap TID is represented.
 */
#define BTreeTupleGetHeapTID(itup) \
	( \
	  (itup)->t_info & INDEX_ALT_TID_MASK && \
	  (ItemPointerGetOffsetNumberNoCheck(&(itup)->t_tid) & BT_HEAP_TID_ATTR) != 0 ? \
	  ( \
		(ItemPointer) (((char *) (itup) + IndexTupleSize(itup)) - \
					   sizeof(ItemPointerData)) \
	  ) \
	  : (itup)->t_info & INDEX_ALT_TID_MASK ? NULL : (ItemPointer) &((itup)->t_tid) \
	)
/*
 * Set the heap TID attribute for a tuple that uses the INDEX_ALT_TID_MASK
 * representation (currently limited to pivot tuples)
 */
#define BTreeTupleSetAltHeapTID(itup) \
	do { \
		Assert((itup)->t_info & INDEX_ALT_TID_MASK); \
		ItemPointerSetOffsetNumber(&(itup)->t_tid, \
								   ItemPointerGetOffsetNumberNoCheck(&(itup)->t_tid) | BT_HEAP_TID_ATTR); \
	} while(0)

/*
 *	Operator strategy numbers for B-tree have been moved to access/stratnum.h,
 *	because many places need to use them in ScanKeyInit() calls.
 *
 *	The strategy numbers are chosen so that we can commute them by
 *	subtraction, thus:
 */
#define BTCommuteStrategyNumber(strat)	(BTMaxStrategyNumber + 1 - (strat))

/*
 *	When a new operator class is declared, we require that the user
 *	supply us with an amproc procedure (BTORDER_PROC) for determining
 *	whether, for two keys a and b, a < b, a = b, or a > b.  This routine
 *	must return < 0, 0, > 0, respectively, in these three cases.
 *
 *	To facilitate accelerated sorting, an operator class may choose to
 *	offer a second procedure (BTSORTSUPPORT_PROC).  For full details, see
 *	src/include/utils/sortsupport.h.
 *
 *	To support window frames defined by "RANGE offset PRECEDING/FOLLOWING",
 *	an operator class may choose to offer a third amproc procedure
 *	(BTINRANGE_PROC), independently of whether it offers sortsupport.
 *	For full details, see doc/src/sgml/btree.sgml.
 */

#define BTORDER_PROC		1
#define BTSORTSUPPORT_PROC	2
#define BTINRANGE_PROC		3
#define BTNProcs			3

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
	BlockNumber bts_btentry;
	struct BTStackData *bts_parent;
} BTStackData;

typedef BTStackData *BTStack;

/*
 * BTScanInsertData is the btree-private state needed to find an initial
 * position for an indexscan, or to insert new tuples -- an "insertion
 * scankey" (not to be confused with a search scankey).  It's used to descend
 * a B-Tree using _bt_search.
 *
 * heapkeyspace indicates if we expect all keys in the index to be physically
 * unique because heap TID is used as a tiebreaker attribute, and if index may
 * have truncated key attributes in pivot tuples.  This is actually a property
 * of the index relation itself (not an indexscan).  heapkeyspace indexes are
 * indexes whose version is >= version 4.  It's convenient to keep this close
 * by, rather than accessing the metapage repeatedly.
 *
 * anynullkeys indicates if any of the keys had NULL value when scankey was
 * built from index tuple (note that already-truncated tuple key attributes
 * set NULL as a placeholder key value, which also affects value of
 * anynullkeys).  This is a convenience for unique index non-pivot tuple
 * insertion, which usually temporarily unsets scantid, but shouldn't iff
 * anynullkeys is true.  Value generally matches non-pivot tuple's HasNulls
 * bit, but may not when inserting into an INCLUDE index (tuple header value
 * is affected by the NULL-ness of both key and non-key attributes).
 *
 * When nextkey is false (the usual case), _bt_search and _bt_binsrch will
 * locate the first item >= scankey.  When nextkey is true, they will locate
 * the first item > scan key.
 *
 * pivotsearch is set to true by callers that want to re-find a leaf page
 * using a scankey built from a leaf page's high key.  Most callers set this
 * to false.
 *
 * scantid is the heap TID that is used as a final tiebreaker attribute.  It
 * is set to NULL when index scan doesn't need to find a position for a
 * specific physical tuple.  Must be set when inserting new tuples into
 * heapkeyspace indexes, since every tuple in the tree unambiguously belongs
 * in one exact position (it's never set with !heapkeyspace indexes, though).
 * Despite the representational difference, nbtree search code considers
 * scantid to be just another insertion scankey attribute.
 *
 * scankeys is an array of scan key entries for attributes that are compared
 * before scantid (user-visible attributes).  keysz is the size of the array.
 * During insertion, there must be a scan key for every attribute, but when
 * starting a regular index scan some can be omitted.  The array is used as a
 * flexible array member, though it's sized in a way that makes it possible to
 * use stack allocations.  See nbtree/README for full details.
 */
typedef struct BTScanInsertData
{
	bool		heapkeyspace;
	bool		anynullkeys;
	bool		nextkey;
	bool		pivotsearch;
	ItemPointer scantid;		/* tiebreaker for scankeys */
	int			keysz;			/* Size of scankeys array */
	ScanKeyData scankeys[INDEX_MAX_KEYS];	/* Must appear last */
} BTScanInsertData;

typedef BTScanInsertData *BTScanInsert;

/*
 * BTInsertStateData is a working area used during insertion.
 *
 * This is filled in after descending the tree to the first leaf page the new
 * tuple might belong on.  Tracks the current position while performing
 * uniqueness check, before we have determined which exact page to insert
 * to.
 *
 * (This should be private to nbtinsert.c, but it's also used by
 * _bt_binsrch_insert)
 */
typedef struct BTInsertStateData
{
	IndexTuple	itup;			/* Item we're inserting */
	Size		itemsz;			/* Size of itup -- should be MAXALIGN()'d */
	BTScanInsert itup_key;		/* Insertion scankey */

	/* Buffer containing leaf page we're likely to insert itup on */
	Buffer		buf;

	/*
	 * Cache of bounds within the current buffer.  Only used for insertions
	 * where _bt_check_unique is called.  See _bt_binsrch_insert and
	 * _bt_findinsertloc for details.
	 */
	bool		bounds_valid;
	OffsetNumber low;
	OffsetNumber stricthigh;
} BTInsertStateData;

typedef BTInsertStateData *BTInsertState;

/*
 * BTScanOpaqueData is the btree-private state needed for an indexscan.
 * This consists of preprocessed scan keys (see _bt_preprocess_keys() for
 * details of the preprocessing), information about the current location
 * of the scan, and information about the marked location, if any.  (We use
 * BTScanPosData to represent the data needed for each of current and marked
 * locations.)	In addition we can remember some known-killed index entries
 * that must be marked before we can move off the current page.
 *
 * Index scans work a page at a time: we pin and read-lock the page, identify
 * all the matching items on the page and save them in BTScanPosData, then
 * release the read-lock while returning the items to the caller for
 * processing.  This approach minimizes lock/unlock traffic.  Note that we
 * keep the pin on the index page until the caller is done with all the items
 * (this is needed for VACUUM synchronization, see nbtree/README).  When we
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

	XLogRecPtr	lsn;			/* pos in the WAL stream when page was read */
	BlockNumber currPage;		/* page referenced by items array */
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

#define BTScanPosIsPinned(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BufferIsValid((scanpos).buf) \
)
#define BTScanPosUnpin(scanpos) \
	do { \
		ReleaseBuffer((scanpos).buf); \
		(scanpos).buf = InvalidBuffer; \
	} while (0)
#define BTScanPosUnpinIfPinned(scanpos) \
	do { \
		if (BTScanPosIsPinned(scanpos)) \
			BTScanPosUnpin(scanpos); \
	} while (0)

#define BTScanPosIsValid(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BlockNumberIsValid((scanpos).currPage) \
)
#define BTScanPosInvalidate(scanpos) \
	do { \
		(scanpos).currPage = InvalidBlockNumber; \
		(scanpos).nextPage = InvalidBlockNumber; \
		(scanpos).buf = InvalidBuffer; \
		(scanpos).lsn = InvalidXLogRecPtr; \
		(scanpos).nextTupleOffset = 0; \
	} while (0)

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
	int			arrayKeyCount;	/* count indicating number of array scan keys
								 * processed */
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
 * to use bits 16-31 (see skey.h).  The uppermost bits are copied from the
 * index's indoption[] array entry for the index attribute.
 */
#define SK_BT_REQFWD	0x00010000	/* required to continue forward scan */
#define SK_BT_REQBKWD	0x00020000	/* required to continue backward scan */
#define SK_BT_INDOPTION_SHIFT  24	/* must clear the above bits */
#define SK_BT_DESC			(INDOPTION_DESC << SK_BT_INDOPTION_SHIFT)
#define SK_BT_NULLS_FIRST	(INDOPTION_NULLS_FIRST << SK_BT_INDOPTION_SHIFT)

/*
 * Constant definition for progress reporting.  Phase numbers must match
 * btbuildphasename.
 */
/* PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE is 1 (see progress.h) */
#define PROGRESS_BTREE_PHASE_INDEXBUILD_TABLESCAN		2
#define PROGRESS_BTREE_PHASE_PERFORMSORT_1				3
#define PROGRESS_BTREE_PHASE_PERFORMSORT_2				4
#define PROGRESS_BTREE_PHASE_LEAF_LOAD					5

/*
 * external entry points for btree, in nbtree.c
 */
extern void btbuildempty(Relation index);
extern bool btinsert(Relation rel, Datum *values, bool *isnull,
					 ItemPointer ht_ctid, Relation heapRel,
					 IndexUniqueCheck checkUnique,
					 struct IndexInfo *indexInfo);
extern IndexScanDesc btbeginscan(Relation rel, int nkeys, int norderbys);
extern Size btestimateparallelscan(void);
extern void btinitparallelscan(void *target);
extern bool btgettuple(IndexScanDesc scan, ScanDirection dir);
extern int64 btgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern void btrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					 ScanKey orderbys, int norderbys);
extern void btparallelrescan(IndexScanDesc scan);
extern void btendscan(IndexScanDesc scan);
extern void btmarkpos(IndexScanDesc scan);
extern void btrestrpos(IndexScanDesc scan);
extern IndexBulkDeleteResult *btbulkdelete(IndexVacuumInfo *info,
										   IndexBulkDeleteResult *stats,
										   IndexBulkDeleteCallback callback,
										   void *callback_state);
extern IndexBulkDeleteResult *btvacuumcleanup(IndexVacuumInfo *info,
											  IndexBulkDeleteResult *stats);
extern bool btcanreturn(Relation index, int attno);

/*
 * prototypes for internal functions in nbtree.c
 */
extern bool _bt_parallel_seize(IndexScanDesc scan, BlockNumber *pageno);
extern void _bt_parallel_release(IndexScanDesc scan, BlockNumber scan_page);
extern void _bt_parallel_done(IndexScanDesc scan);
extern void _bt_parallel_advance_array_keys(IndexScanDesc scan);

/*
 * prototypes for functions in nbtinsert.c
 */
extern bool _bt_doinsert(Relation rel, IndexTuple itup,
						 IndexUniqueCheck checkUnique, Relation heapRel);
extern Buffer _bt_getstackbuf(Relation rel, BTStack stack);
extern void _bt_finish_split(Relation rel, Buffer bbuf, BTStack stack);

/*
 * prototypes for functions in nbtsplitloc.c
 */
extern OffsetNumber _bt_findsplitloc(Relation rel, Page page,
									 OffsetNumber newitemoff, Size newitemsz, IndexTuple newitem,
									 bool *newitemonleft);

/*
 * prototypes for functions in nbtpage.c
 */
extern void _bt_initmetapage(Page page, BlockNumber rootbknum, uint32 level);
extern void _bt_update_meta_cleanup_info(Relation rel,
										 TransactionId oldestBtpoXact, float8 numHeapTuples);
extern void _bt_upgrademetapage(Page page);
extern Buffer _bt_getroot(Relation rel, int access);
extern Buffer _bt_gettrueroot(Relation rel);
extern int	_bt_getrootheight(Relation rel);
extern bool _bt_heapkeyspace(Relation rel);
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
extern uint32 _bt_pagedel(Relation rel, Buffer leafbuf,
						  TransactionId *oldestBtpoXact);

/*
 * prototypes for functions in nbtsearch.c
 */
extern BTStack _bt_search(Relation rel, BTScanInsert key, Buffer *bufP,
						  int access, Snapshot snapshot);
extern Buffer _bt_moveright(Relation rel, BTScanInsert key, Buffer buf,
							bool forupdate, BTStack stack, int access, Snapshot snapshot);
extern OffsetNumber _bt_binsrch_insert(Relation rel, BTInsertState insertstate);
extern int32 _bt_compare(Relation rel, BTScanInsert key, Page page, OffsetNumber offnum);
extern bool _bt_first(IndexScanDesc scan, ScanDirection dir);
extern bool _bt_next(IndexScanDesc scan, ScanDirection dir);
extern Buffer _bt_get_endpoint(Relation rel, uint32 level, bool rightmost,
							   Snapshot snapshot);

/*
 * prototypes for functions in nbtutils.c
 */
extern BTScanInsert _bt_mkscankey(Relation rel, IndexTuple itup);
extern void _bt_freestack(BTStack stack);
extern void _bt_preprocess_array_keys(IndexScanDesc scan);
extern void _bt_start_array_keys(IndexScanDesc scan, ScanDirection dir);
extern bool _bt_advance_array_keys(IndexScanDesc scan, ScanDirection dir);
extern void _bt_mark_array_keys(IndexScanDesc scan);
extern void _bt_restore_array_keys(IndexScanDesc scan);
extern void _bt_preprocess_keys(IndexScanDesc scan);
extern bool _bt_checkkeys(IndexScanDesc scan, IndexTuple tuple,
						  int tupnatts, ScanDirection dir, bool *continuescan);
extern void _bt_killitems(IndexScanDesc scan);
extern BTCycleId _bt_vacuum_cycleid(Relation rel);
extern BTCycleId _bt_start_vacuum(Relation rel);
extern void _bt_end_vacuum(Relation rel);
extern void _bt_end_vacuum_callback(int code, Datum arg);
extern Size BTreeShmemSize(void);
extern void BTreeShmemInit(void);
extern bytea *btoptions(Datum reloptions, bool validate);
extern bool btproperty(Oid index_oid, int attno,
					   IndexAMProperty prop, const char *propname,
					   bool *res, bool *isnull);
extern char *btbuildphasename(int64 phasenum);
extern IndexTuple _bt_truncate(Relation rel, IndexTuple lastleft,
							   IndexTuple firstright, BTScanInsert itup_key);
extern int	_bt_keep_natts_fast(Relation rel, IndexTuple lastleft,
								IndexTuple firstright);
extern bool _bt_check_natts(Relation rel, bool heapkeyspace, Page page,
							OffsetNumber offnum);
extern void _bt_check_third_page(Relation rel, Relation heap,
								 bool needheaptidspace, Page page, IndexTuple newtup);

/*
 * prototypes for functions in nbtvalidate.c
 */
extern bool btvalidate(Oid opclassoid);

/*
 * prototypes for functions in nbtsort.c
 */
extern IndexBuildResult *btbuild(Relation heap, Relation index,
								 struct IndexInfo *indexInfo);
extern void _bt_parallel_build_main(dsm_segment *seg, shm_toc *toc);

#endif							/* NBTREE_H */
