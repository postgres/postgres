/*-------------------------------------------------------------------------
 *
 * rtree.h--
 *    common declarations for the rtree access method code.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rtree.h,v 1.5 1997/01/10 09:36:34 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RTREE_H
#define RTREE_H

#include <access/funcindex.h>
#include <access/itup.h>
#include <access/relscan.h>
#include <access/sdir.h>
#include <access/skey.h>
#include <access/strat.h>
#include <storage/block.h>
#include <storage/off.h>

/* see rtstrat.c for what all this is about */
#define RTNStrategies			8
#define RTLeftStrategyNumber		1
#define RTOverLeftStrategyNumber	2
#define RTOverlapStrategyNumber		3
#define RTOverRightStrategyNumber	4
#define RTRightStrategyNumber		5
#define RTSameStrategyNumber		6
#define RTContainsStrategyNumber	7
#define RTContainedByStrategyNumber	8

#define RTNProcs			3
#define RT_UNION_PROC			1
#define RT_INTER_PROC			2
#define RT_SIZE_PROC			3

#define F_LEAF		(1 << 0)

typedef struct RTreePageOpaqueData {
	uint32		flags;
} RTreePageOpaqueData;

typedef RTreePageOpaqueData	*RTreePageOpaque;

/*
 *  When we descend a tree, we keep a stack of parent pointers.
 */

typedef struct RTSTACK {
	struct RTSTACK	*rts_parent;
	OffsetNumber	rts_child;
	BlockNumber	rts_blk;
} RTSTACK;

/*
 *  When we're doing a scan, we need to keep track of the parent stack
 *  for the marked and current items.  Also, rtrees have the following
 *  property:  if you're looking for the box (1,1,2,2), on the internal
 *  nodes you have to search for all boxes that *contain* (1,1,2,2), and
 *  not the ones that match it.  We have a private scan key for internal
 *  nodes in the opaque structure for rtrees for this reason.  See
 *  access/index-rtree/rtscan.c and rtstrat.c for how it gets initialized.
 */

typedef struct RTreeScanOpaqueData {
	struct RTSTACK	*s_stack;
	struct RTSTACK	*s_markstk;
	uint16		s_flags;
	uint16		s_internalNKey;
	ScanKey		s_internalKey;
} RTreeScanOpaqueData;

typedef RTreeScanOpaqueData	*RTreeScanOpaque;

/*
 *  When we're doing a scan and updating a tree at the same time, the
 *  updates may affect the scan.  We use the flags entry of the scan's
 *  opaque space to record our actual position in response to updates
 *  that we can't handle simply by adjusting pointers.
 */

#define RTS_CURBEFORE	((uint16) (1 << 0))
#define RTS_MRKBEFORE	((uint16) (1 << 1))

/* root page of an rtree */
#define P_ROOT		0

/*
 *  When we update a relation on which we're doing a scan, we need to
 *  check the scan and fix it if the update affected any of the pages it
 *  touches.  Otherwise, we can miss records that we should see.  The only
 *  times we need to do this are for deletions and splits.  See the code in
 *  rtscan.c for how the scan is fixed.  These two contants tell us what sort
 *  of operation changed the index.
 */

#define	RTOP_DEL	0
#define	RTOP_SPLIT	1

/* defined in rtree.c */
extern void freestack(RTSTACK *s);

/* rget.c */
extern RetrieveIndexResult rtgettuple(IndexScanDesc s, ScanDirection dir);

/*
 *	RTree code.
 *	Defined in access/index-rtree/
 */
extern InsertIndexResult rtinsert(Relation r, Datum *datum, char *nulls,
			ItemPointer ht_ctid, Relation heapRel);
extern char *rtdelete(Relation r, ItemPointer tid);

extern RetrieveIndexResult rtgettuple(IndexScanDesc s, ScanDirection dir);
extern IndexScanDesc rtbeginscan(Relation r, bool fromEnd, uint16 nkeys,
	    ScanKey key);

extern void rtendscan(IndexScanDesc s);
extern void rtmarkpos(IndexScanDesc s);
extern void rtrestrpos(IndexScanDesc s);
extern void rtrescan(IndexScanDesc s, bool fromEnd, ScanKey key);
extern void rtbuild(Relation heap, Relation index, int natts,
	AttrNumber *attnum, IndexStrategy istrat, uint16 pcount,
	Datum *params, FuncIndexInfo *finfo, PredInfo *predInfo);
extern void _rtdump(Relation r);

/* rtscan.c */
extern void rtadjscans(Relation r, int op, BlockNumber blkno,
				OffsetNumber offnum);
/* rtstrat.h */
extern StrategyNumber RelationGetRTStrategy(Relation r,
			AttrNumber attnum, RegProcedure proc);
extern bool RelationInvokeRTStrategy(Relation r, AttrNumber attnum,
			StrategyNumber s, Datum left, Datum right);
extern RegProcedure RTMapOperator(Relation r, AttrNumber attnum,
			RegProcedure proc);

#endif /* RTREE_H */
