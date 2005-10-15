/*-------------------------------------------------------------------------
 *
 * gist_private.h
 *	  private declarations for GiST -- declarations related to the
 *	  internal implementation of GiST, not the public API
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/gist_private.h,v 1.8 2005/10/15 02:49:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_PRIVATE_H
#define GIST_PRIVATE_H

#include "access/itup.h"
#include "access/gist.h"
#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "fmgr.h"

#define GIST_UNLOCK BUFFER_LOCK_UNLOCK
#define GIST_SHARE	BUFFER_LOCK_SHARE
#define GIST_EXCLUSIVE	BUFFER_LOCK_EXCLUSIVE


/*
 * XXX old comment!!!
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
typedef struct GISTSearchStack
{
	struct GISTSearchStack *next;
	BlockNumber block;
	/* to identify page changed */
	GistNSN		lsn;
	/* to recognize split occured */
	GistNSN		parentlsn;
} GISTSearchStack;

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
	GISTSearchStack *stack;
	GISTSearchStack *markstk;
	uint16		flags;
	GISTSTATE  *giststate;
	MemoryContext tempCxt;
	Buffer		curbuf;
	Buffer		markbuf;
} GISTScanOpaqueData;

typedef GISTScanOpaqueData *GISTScanOpaque;

/* XLog stuff */
extern const XLogRecPtr XLogRecPtrForTemp;

#define XLOG_GIST_ENTRY_UPDATE	0x00
#define XLOG_GIST_ENTRY_DELETE	0x10
#define XLOG_GIST_NEW_ROOT	0x20

typedef struct gistxlogEntryUpdate
{
	RelFileNode node;
	BlockNumber blkno;

	uint16		ntodelete;
	bool		isemptypage;

	/*
	 * It used to identify completeness of insert. Sets to leaf itup
	 */
	ItemPointerData key;

	/*
	 * follow: 1. todelete OffsetNumbers 2. tuples to insert
	 */
} gistxlogEntryUpdate;

#define XLOG_GIST_PAGE_SPLIT	0x30

typedef struct gistxlogPageSplit
{
	RelFileNode node;
	BlockNumber origblkno;		/* splitted page */
	uint16		npage;

	/* see comments on gistxlogEntryUpdate */
	ItemPointerData key;

	/*
	 * follow: 1. gistxlogPage and array of IndexTupleData per page
	 */
} gistxlogPageSplit;

#define XLOG_GIST_INSERT_COMPLETE  0x40

typedef struct gistxlogPage
{
	BlockNumber blkno;
	int			num;
} gistxlogPage;

#define XLOG_GIST_CREATE_INDEX	0x50

typedef struct gistxlogInsertComplete
{
	RelFileNode node;
	/* follows ItemPointerData key to clean */
} gistxlogInsertComplete;

/* SplitedPageLayout - gistSplit function result */
typedef struct SplitedPageLayout
{
	gistxlogPage block;
	IndexTupleData *list;
	int			lenlist;
	Buffer		buffer;			/* to write after all proceed */

	struct SplitedPageLayout *next;
} SplitedPageLayout;

/*
 * GISTInsertStack used for locking buffers and transfer arguments during
 * insertion
 */

typedef struct GISTInsertStack
{
	/* current page */
	BlockNumber blkno;
	Buffer		buffer;
	Page		page;

	/*
	 * log sequence number from page->lsn to recognize page update	and
	 * compare it with page's nsn to recognize page split
	 */
	GistNSN		lsn;

	/* child's offset */
	OffsetNumber childoffnum;

	/* pointer to parent and child */
	struct GISTInsertStack *parent;
	struct GISTInsertStack *child;

	/* for gistFindPath */
	struct GISTInsertStack *next;
} GISTInsertStack;

#define XLogRecPtrIsInvalid( r )	( (r).xlogid == 0 && (r).xrecoff == 0 )

typedef struct
{
	Relation	r;
	IndexTuple *itup;			/* in/out, points to compressed entry */
	int			ituplen;		/* length of itup */
	GISTInsertStack *stack;
	bool		needInsertComplete;

	/* pointer to heap tuple */
	ItemPointerData key;
} GISTInsertState;

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
 * it touches.	Otherwise, we can miss records that we should see.
 * The only times we need to do this are for deletions and splits. See
 * the code in gistscan.c for how the scan is fixed. These two
 * constants tell us what sort of operation changed the index.
 */
#define GISTOP_DEL		0
/* #define GISTOP_SPLIT 1 */

#define ATTSIZE(datum, tupdesc, i, isnull) \
		( \
				(isnull) ? 0 : \
				   att_addlength(0, (tupdesc)->attrs[(i)-1]->attlen, (datum)) \
		)

/*
 * mark tuples on inner pages during recovery
 */
#define TUPLE_IS_VALID		0xffff
#define TUPLE_IS_INVALID	0xfffe

#define  GistTupleIsInvalid(itup)	( ItemPointerGetOffsetNumber( &((itup)->t_tid) ) == TUPLE_IS_INVALID )
#define  GistTupleSetValid(itup)	ItemPointerSetOffsetNumber( &((itup)->t_tid), TUPLE_IS_VALID )
#define  GistTupleSetInvalid(itup)	ItemPointerSetOffsetNumber( &((itup)->t_tid), TUPLE_IS_INVALID )

/* gist.c */
extern Datum gistbuild(PG_FUNCTION_ARGS);
extern Datum gistinsert(PG_FUNCTION_ARGS);
extern MemoryContext createTempGistContext(void);
extern void initGISTstate(GISTSTATE *giststate, Relation index);
extern void freeGISTstate(GISTSTATE *giststate);
extern void gistmakedeal(GISTInsertState *state, GISTSTATE *giststate);
extern void gistnewroot(Relation r, Buffer buffer, IndexTuple *itup, int len, ItemPointer key);

extern IndexTuple *gistSplit(Relation r, Buffer buffer, IndexTuple *itup,
		  int *len, SplitedPageLayout **dist, GISTSTATE *giststate);

extern GISTInsertStack *gistFindPath(Relation r, BlockNumber child,
			 Buffer (*myReadBuffer) (Relation, BlockNumber));

/* gistxlog.c */
extern void gist_redo(XLogRecPtr lsn, XLogRecord *record);
extern void gist_desc(char *buf, uint8 xl_info, char *rec);
extern void gist_xlog_startup(void);
extern void gist_xlog_cleanup(void);
extern IndexTuple gist_form_invalid_tuple(BlockNumber blkno);

extern XLogRecData *formUpdateRdata(RelFileNode node, BlockNumber blkno,
				OffsetNumber *todelete, int ntodelete, bool emptypage,
				IndexTuple *itup, int ituplen, ItemPointer key);

extern XLogRecData *formSplitRdata(RelFileNode node, BlockNumber blkno,
			   ItemPointer key, SplitedPageLayout *dist);

extern XLogRecPtr gistxlogInsertCompletion(RelFileNode node, ItemPointerData *keys, int len);

/* gistget.c */
extern Datum gistgettuple(PG_FUNCTION_ARGS);
extern Datum gistgetmulti(PG_FUNCTION_ARGS);

/* gistutil.c */
extern Buffer gistNewBuffer(Relation r);
extern OffsetNumber gistfillbuffer(Relation r, Page page, IndexTuple *itup,
			   int len, OffsetNumber off);
extern bool gistnospace(Page page, IndexTuple *itvec, int len);
extern IndexTuple *gistextractbuffer(Buffer buffer, int *len /* out */ );
extern IndexTuple *gistjoinvector(
			   IndexTuple *itvec, int *len,
			   IndexTuple *additvec, int addlen);
extern IndexTuple gistunion(Relation r, IndexTuple *itvec,
		  int len, GISTSTATE *giststate);
extern IndexTuple gistgetadjusted(Relation r,
				IndexTuple oldtup,
				IndexTuple addtup,
				GISTSTATE *giststate);
extern int gistfindgroup(GISTSTATE *giststate,
			  GISTENTRY *valvec, GIST_SPLITVEC *spl);
extern void gistadjsubkey(Relation r,
			  IndexTuple *itup, int len,
			  GIST_SPLITVEC *v,
			  GISTSTATE *giststate);
extern IndexTuple gistFormTuple(GISTSTATE *giststate,
			  Relation r, Datum *attdata, int *datumsize, bool *isnull);

extern OffsetNumber gistchoose(Relation r, Page p,
		   IndexTuple it,
		   GISTSTATE *giststate);
extern void gistcentryinit(GISTSTATE *giststate, int nkey,
			   GISTENTRY *e, Datum k,
			   Relation r, Page pg,
			   OffsetNumber o, int b, bool l, bool isNull);
extern void gistDeCompressAtt(GISTSTATE *giststate, Relation r,
				  IndexTuple tuple, Page p, OffsetNumber o,
				  GISTENTRY *attdata, bool *isnull);
extern void gistunionsubkey(Relation r, GISTSTATE *giststate,
				IndexTuple *itvec, GIST_SPLITVEC *spl, bool isall);
extern void GISTInitBuffer(Buffer b, uint32 f);
extern void gistdentryinit(GISTSTATE *giststate, int nkey, GISTENTRY *e,
			   Datum k, Relation r, Page pg, OffsetNumber o,
			   int b, bool l, bool isNull);
void gistUserPicksplit(Relation r, GistEntryVector *entryvec, GIST_SPLITVEC *v,
				  IndexTuple *itup, int len, GISTSTATE *giststate);

/* gistvacuum.c */
extern Datum gistbulkdelete(PG_FUNCTION_ARGS);
extern Datum gistvacuumcleanup(PG_FUNCTION_ARGS);

#endif   /* GIST_PRIVATE_H */
