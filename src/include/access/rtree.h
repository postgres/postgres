/*-------------------------------------------------------------------------
 *
 * rtree.h
 *	  common declarations for the rtree access method code.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rtree.h,v 1.30 2003/08/04 02:40:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RTREE_H
#define RTREE_H

#include "access/itup.h"
#include "access/sdir.h"
#include "access/skey.h"
#include "access/xlog.h"
#include "utils/rel.h"

/* see rtstrat.c for what all this is about */
#define RTNStrategies					8
#define RTLeftStrategyNumber			1
#define RTOverLeftStrategyNumber		2
#define RTOverlapStrategyNumber			3
#define RTOverRightStrategyNumber		4
#define RTRightStrategyNumber			5
#define RTSameStrategyNumber			6
#define RTContainsStrategyNumber		7
#define RTContainedByStrategyNumber		8

#define RTNProcs						3
#define RT_UNION_PROC					1
#define RT_INTER_PROC					2
#define RT_SIZE_PROC					3

#define F_LEAF			(1 << 0)

typedef struct RTreePageOpaqueData
{
	uint32		flags;
} RTreePageOpaqueData;

typedef RTreePageOpaqueData *RTreePageOpaque;

/*
 *	When we descend a tree, we keep a stack of parent pointers.
 */

typedef struct RTSTACK
{
	struct RTSTACK *rts_parent;
	OffsetNumber rts_child;
	BlockNumber rts_blk;
} RTSTACK;

/*
 *	When we're doing a scan, we need to keep track of the parent stack
 *	for the marked and current items.  Also, rtrees have the following
 *	property:  if you're looking for the box (1,1,2,2), on the internal
 *	nodes you have to search for all boxes that *contain* (1,1,2,2), and
 *	not the ones that match it.  We have a private scan key for internal
 *	nodes in the opaque structure for rtrees for this reason.  See
 *	access/index-rtree/rtscan.c and rtstrat.c for how it gets initialized.
 */

typedef struct RTreeScanOpaqueData
{
	struct RTSTACK *s_stack;
	struct RTSTACK *s_markstk;
	uint16		s_flags;
	int			s_internalNKey;
	ScanKey		s_internalKey;
} RTreeScanOpaqueData;

typedef RTreeScanOpaqueData *RTreeScanOpaque;

/*
 *	When we're doing a scan and updating a tree at the same time, the
 *	updates may affect the scan.  We use the flags entry of the scan's
 *	opaque space to record our actual position in response to updates
 *	that we can't handle simply by adjusting pointers.
 */

#define RTS_CURBEFORE	((uint16) (1 << 0))
#define RTS_MRKBEFORE	((uint16) (1 << 1))

/* root page of an rtree */
#define P_ROOT			0

/*
 *	When we update a relation on which we're doing a scan, we need to
 *	check the scan and fix it if the update affected any of the pages it
 *	touches.  Otherwise, we can miss records that we should see.  The only
 *	times we need to do this are for deletions and splits.	See the code in
 *	rtscan.c for how the scan is fixed.  These two contants tell us what sort
 *	of operation changed the index.
 */

#define RTOP_DEL		0
#define RTOP_SPLIT		1

/* defined in rtree.c */
extern void freestack(RTSTACK *s);

/*
 *		RTree code.
 *		Defined in access/rtree/
 */
extern Datum rtinsert(PG_FUNCTION_ARGS);
extern Datum rtbulkdelete(PG_FUNCTION_ARGS);

extern Datum rtgettuple(PG_FUNCTION_ARGS);
extern Datum rtbeginscan(PG_FUNCTION_ARGS);

extern Datum rtendscan(PG_FUNCTION_ARGS);
extern Datum rtmarkpos(PG_FUNCTION_ARGS);
extern Datum rtrestrpos(PG_FUNCTION_ARGS);
extern Datum rtrescan(PG_FUNCTION_ARGS);
extern Datum rtbuild(PG_FUNCTION_ARGS);
extern void _rtdump(Relation r);

extern void rtree_redo(XLogRecPtr lsn, XLogRecord *record);
extern void rtree_undo(XLogRecPtr lsn, XLogRecord *record);
extern void rtree_desc(char *buf, uint8 xl_info, char *rec);

/* rtscan.c */
extern void rtadjscans(Relation r, int op, BlockNumber blkno,
		   OffsetNumber offnum);
extern void AtEOXact_rtree(void);

/* rtstrat.c */
extern RegProcedure RTMapOperator(Relation r, AttrNumber attnum,
			  RegProcedure proc);

#endif   /* RTREE_H */
