/*-------------------------------------------------------------------------
 *
 * nbtree.h--
 *	  header file for postgres btree access method implementation.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nbtree.h,v 1.18 1997/09/08 21:50:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NBTREE_H
#define NBTREE_H

#include <access/sdir.h>
#include <access/relscan.h>
#include <storage/itemid.h>
#include <storage/page.h>
#include <access/funcindex.h>
#include <access/itup.h>
#include <storage/buf.h>
#include <storage/itemptr.h>

/*
 *	BTPageOpaqueData -- At the end of every page, we store a pointer
 *	to both siblings in the tree.  See Lehman and Yao's paper for more
 *	info.  In addition, we need to know what sort of page this is
 *	(leaf or internal), and whether the page is available for reuse.
 *
 *	Lehman and Yao's algorithm requires a ``high key'' on every page.
 *	The high key on a page is guaranteed to be greater than or equal
 *	to any key that appears on this page.  Our insertion algorithm
 *	guarantees that we can use the initial least key on our right
 *	sibling as the high key.  We allocate space for the line pointer
 *	to the high key in the opaque data at the end of the page.
 *
 *	Rightmost pages in the tree have no high key.
 */

typedef struct BTPageOpaqueData
{
	BlockNumber btpo_prev;
	BlockNumber btpo_next;
	uint16		btpo_flags;

#define BTP_LEAF		(1 << 0)
#define BTP_ROOT		(1 << 1)
#define BTP_FREE		(1 << 2)
#define BTP_META		(1 << 3)
#define BTP_CHAIN		(1 << 4)

} BTPageOpaqueData;

typedef BTPageOpaqueData *BTPageOpaque;

/*
 *	ScanOpaqueData is used to remember which buffers we're currently
 *	examining in the scan.	We keep these buffers locked and pinned
 *	and recorded in the opaque entry of the scan in order to avoid
 *	doing a ReadBuffer() for every tuple in the index.	This avoids
 *	semop() calls, which are expensive.
 *
 *	And it's used to remember actual scankey info (we need in it
 *	if some scankeys evaled at runtime.
 */

typedef struct BTScanOpaqueData
{
	Buffer		btso_curbuf;
	Buffer		btso_mrkbuf;
	uint16		qual_ok;		/* 0 for quals like key == 1 && key > 2 */
	uint16		numberOfKeys;	/* number of keys */
	uint16		numberOfFirstKeys;		/* number of keys for 1st
										 * attribute */
	ScanKey		keyData;		/* key descriptor */
} BTScanOpaqueData;

typedef BTScanOpaqueData *BTScanOpaque;

/*
 *	BTItems are what we store in the btree.  Each item has an index
 *	tuple, including key and pointer values.  In addition, we must
 *	guarantee that all tuples in the index are unique, in order to
 *	satisfy some assumptions in Lehman and Yao.  The way that we do
 *	this is by generating a new OID for every insertion that we do in
 *	the tree.  This adds eight bytes to the size of btree index
 *	tuples.  Note that we do not use the OID as part of a composite
 *	key; the OID only serves as a unique identifier for a given index
 *	tuple (logical position within a page).
 *
 *	New comments:
 *	actually, we must guarantee that all tuples in A LEVEL
 *	are unique, not in ALL INDEX. So, we can use bti_itup->t_tid
 *	as unique identifier for a given index tuple (logical position
 *	within a level).	- vadim 04/09/97
 */

typedef struct BTItemData
{
#ifndef BTREE_VERSION_1
	Oid			bti_oid;
	int32		bti_dummy;		/* padding to make bti_itup align at
								 * 8-byte boundary */
#endif
	IndexTupleData bti_itup;
} BTItemData;

typedef BTItemData *BTItem;

#ifdef BTREE_VERSION_1
#define BTItemSame(i1, i2)	  ( i1->bti_itup.t_tid.ip_blkid.bi_hi == \
								i2->bti_itup.t_tid.ip_blkid.bi_hi && \
								i1->bti_itup.t_tid.ip_blkid.bi_lo == \
								i2->bti_itup.t_tid.ip_blkid.bi_lo && \
								i1->bti_itup.t_tid.ip_posid == \
								i2->bti_itup.t_tid.ip_posid )
#else
#define BTItemSame(i1, i2)	  ( i1->bti_oid == i2->bti_oid )
#endif

/*
 *	BTStackData -- As we descend a tree, we push the (key, pointer)
 *	pairs from internal nodes onto a private stack.  If we split a
 *	leaf, we use this stack to walk back up the tree and insert data
 *	into parent nodes (and possibly to split them, too).  Lehman and
 *	Yao's update algorithm guarantees that under no circumstances can
 *	our private stack give us an irredeemably bad picture up the tree.
 *	Again, see the paper for details.
 */

typedef struct BTStackData
{
	BlockNumber bts_blkno;
	OffsetNumber bts_offset;
	BTItem		bts_btitem;
	struct BTStackData *bts_parent;
} BTStackData;

typedef BTStackData *BTStack;

typedef struct BTPageState
{
	Buffer		btps_buf;
	Page		btps_page;
	BTItem		btps_lastbti;
	OffsetNumber btps_lastoff;
	OffsetNumber btps_firstoff;
	int			btps_level;
	bool		btps_doupper;
	struct BTPageState *btps_next;
} BTPageState;

/*
 *	We need to be able to tell the difference between read and write
 *	requests for pages, in order to do locking correctly.
 */

#define BT_READ			0
#define BT_WRITE		1

/*
 *	Similarly, the difference between insertion and non-insertion binary
 *	searches on a given page makes a difference when we're descending the
 *	tree.
 */

#define BT_INSERTION	0
#define BT_DESCENT		1

/*
 *	We must classify index modification types for the benefit of
 *	_bt_adjscans.
 */
#define BT_INSERT		0
#define BT_DELETE		1

/*
 *	In general, the btree code tries to localize its knowledge about
 *	page layout to a couple of routines.  However, we need a special
 *	value to indicate "no page number" in those places where we expect
 *	page numbers.
 */

#define P_NONE			0
#define P_LEFTMOST(opaque)		((opaque)->btpo_prev == P_NONE)
#define P_RIGHTMOST(opaque)		((opaque)->btpo_next == P_NONE)

#define P_HIKEY			((OffsetNumber) 1)
#define P_FIRSTKEY		((OffsetNumber) 2)

/*
 *	Strategy numbers -- ordering of these is <, <=, =, >=, >
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
 * prototypes for functions in nbtinsert.c
 */
extern InsertIndexResult
_bt_doinsert(Relation rel, BTItem btitem,
			 bool index_is_unique, Relation heapRel);

 /* default is to allow duplicates */
extern bool
_bt_itemcmp(Relation rel, Size keysz, BTItem item1, BTItem item2,
			StrategyNumber strat);

/*
 * prototypes for functions in nbtpage.c
 */
extern void _bt_metapinit(Relation rel);
extern Buffer _bt_getroot(Relation rel, int access);
extern Buffer _bt_getbuf(Relation rel, BlockNumber blkno, int access);
extern void _bt_relbuf(Relation rel, Buffer buf, int access);
extern void _bt_wrtbuf(Relation rel, Buffer buf);
extern void _bt_wrtnorelbuf(Relation rel, Buffer buf);
extern void _bt_pageinit(Page page, Size size);
extern void _bt_metaproot(Relation rel, BlockNumber rootbknum, int level);
extern Buffer _bt_getstackbuf(Relation rel, BTStack stack, int access);
extern void _bt_pagedel(Relation rel, ItemPointer tid);

/*
 * prototypes for functions in nbtree.c
 */
extern bool BuildingBtree;		/* in nbtree.c */

extern void
btbuild(Relation heap, Relation index, int natts,
		AttrNumber *attnum, IndexStrategy istrat, uint16 pcount,
		Datum *params, FuncIndexInfo *finfo, PredInfo *predInfo);
extern InsertIndexResult
btinsert(Relation rel, Datum *datum, char *nulls,
		 ItemPointer ht_ctid, Relation heapRel);
extern char *btgettuple(IndexScanDesc scan, ScanDirection dir);
extern char *
btbeginscan(Relation rel, bool fromEnd, uint16 keysz,
			ScanKey scankey);

extern void btrescan(IndexScanDesc scan, bool fromEnd, ScanKey scankey);
extern void btmovescan(IndexScanDesc scan, Datum v);
extern void btendscan(IndexScanDesc scan);
extern void btmarkpos(IndexScanDesc scan);
extern void btrestrpos(IndexScanDesc scan);
extern void btdelete(Relation rel, ItemPointer tid);

/*
 * prototypes for functions in nbtscan.c
 */
extern void _bt_regscan(IndexScanDesc scan);
extern void _bt_dropscan(IndexScanDesc scan);
extern void _bt_adjscans(Relation rel, ItemPointer tid, int op);

/*
 * prototypes for functions in nbtsearch.c
 */
extern BTStack
_bt_search(Relation rel, int keysz, ScanKey scankey,
		   Buffer *bufP);
extern Buffer
_bt_moveright(Relation rel, Buffer buf, int keysz,
			  ScanKey scankey, int access);
extern bool
_bt_skeycmp(Relation rel, Size keysz, ScanKey scankey,
			Page page, ItemId itemid, StrategyNumber strat);
extern OffsetNumber
_bt_binsrch(Relation rel, Buffer buf, int keysz,
			ScanKey scankey, int srchtype);
extern RetrieveIndexResult _bt_next(IndexScanDesc scan, ScanDirection dir);
extern RetrieveIndexResult _bt_first(IndexScanDesc scan, ScanDirection dir);
extern bool _bt_step(IndexScanDesc scan, Buffer *bufP, ScanDirection dir);

/*
 * prototypes for functions in nbtstrat.c
 */
extern StrategyNumber
_bt_getstrat(Relation rel, AttrNumber attno,
			 RegProcedure proc);
extern bool
_bt_invokestrat(Relation rel, AttrNumber attno,
				StrategyNumber strat, Datum left, Datum right);

/*
 * prototypes for functions in nbtutils.c
 */
extern ScanKey _bt_mkscankey(Relation rel, IndexTuple itup);
extern void _bt_freeskey(ScanKey skey);
extern void _bt_freestack(BTStack stack);
extern void _bt_orderkeys(Relation relation, BTScanOpaque so);
extern bool _bt_checkkeys(IndexScanDesc scan, IndexTuple tuple, Size *keysok);
extern BTItem _bt_formitem(IndexTuple itup);

/*
 * prototypes for functions in nbtsort.c
 */
extern void *_bt_spoolinit(Relation index, int ntapes, bool isunique);
extern void _bt_spooldestroy(void *spool);
extern void _bt_spool(Relation index, BTItem btitem, void *spool);
extern void _bt_leafbuild(Relation index, void *spool);

#endif							/* NBTREE_H */
