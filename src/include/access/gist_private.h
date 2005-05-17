/*-------------------------------------------------------------------------
 *
 * gist_private.h
 *	  private declarations for GiST -- declarations related to the
 *	  internal implementation of GiST, not the public API
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/gist_private.h,v 1.1 2005/05/17 03:34:18 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_PRIVATE_H
#define GIST_PRIVATE_H

#include "access/gist.h"
#include "access/xlog.h"
#include "fmgr.h"

/*
 * When we descend a tree, we keep a stack of parent pointers. This
 * allows us to follow a chain of internal node points until we reach
 * a leaf node, and then back up the stack to re-examine the internal
 * nodes.
 *
 * 'parent' is the previous stack entry -- i.e. the node we arrived
 * from. 'block' is the node's block number. 'offset' is the offset in
 * the node's page that we stopped at (i.e. we followed the child
 * pointer located at the specified offset).
 */
typedef struct GISTSTACK
{
	struct GISTSTACK *parent;
	OffsetNumber offset;
	BlockNumber block;
} GISTSTACK;

typedef struct GISTSTATE
{
	FmgrInfo	consistentFn[INDEX_MAX_KEYS];
	FmgrInfo	unionFn[INDEX_MAX_KEYS];
	FmgrInfo	compressFn[INDEX_MAX_KEYS];
	FmgrInfo	decompressFn[INDEX_MAX_KEYS];
	FmgrInfo	penaltyFn[INDEX_MAX_KEYS];
	FmgrInfo	picksplitFn[INDEX_MAX_KEYS];
	FmgrInfo	equalFn[INDEX_MAX_KEYS];

	TupleDesc	tupdesc;
} GISTSTATE;

/*
 *	When we're doing a scan, we need to keep track of the parent stack
 *	for the marked and current items.
 */
typedef struct GISTScanOpaqueData
{
	GISTSTACK			*stack;
	GISTSTACK			*markstk;
	uint16				 flags;
	GISTSTATE			*giststate;
	MemoryContext		 tempCxt;
	Buffer				 curbuf;
	Buffer				 markbuf;
} GISTScanOpaqueData;

typedef GISTScanOpaqueData *GISTScanOpaque;

/*
 * When we're doing a scan and updating a tree at the same time, the
 * updates may affect the scan.  We use the flags entry of the scan's
 * opaque space to record our actual position in response to updates
 * that we can't handle simply by adjusting pointers.
 */
#define GS_CURBEFORE	((uint16) (1 << 0))
#define GS_MRKBEFORE	((uint16) (1 << 1))

/* root page of a gist index */
#define GIST_ROOT_BLKNO				0

/*
 * When we update a relation on which we're doing a scan, we need to
 * check the scan and fix it if the update affected any of the pages
 * it touches.  Otherwise, we can miss records that we should see.
 * The only times we need to do this are for deletions and splits. See
 * the code in gistscan.c for how the scan is fixed. These two
 * constants tell us what sort of operation changed the index.
 */
#define GISTOP_DEL		0
#define GISTOP_SPLIT	1

/* gist.c */
extern Datum gistbuild(PG_FUNCTION_ARGS);
extern Datum gistinsert(PG_FUNCTION_ARGS);
extern Datum gistbulkdelete(PG_FUNCTION_ARGS);
extern MemoryContext createTempGistContext(void);
extern void initGISTstate(GISTSTATE *giststate, Relation index);
extern void freeGISTstate(GISTSTATE *giststate);
extern void gistdentryinit(GISTSTATE *giststate, int nkey, GISTENTRY *e,
			   Datum k, Relation r, Page pg, OffsetNumber o,
			   int b, bool l, bool isNull);
extern void gist_redo(XLogRecPtr lsn, XLogRecord *record);
extern void gist_undo(XLogRecPtr lsn, XLogRecord *record);
extern void gist_desc(char *buf, uint8 xl_info, char *rec);

/* gistget.c */
extern Datum gistgettuple(PG_FUNCTION_ARGS);
extern Datum gistgetmulti(PG_FUNCTION_ARGS);

#endif	/* GIST_PRIVATE_H */
