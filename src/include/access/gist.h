/*-------------------------------------------------------------------------
 *
 * gist.h
 *	  common declarations for the GiST access method code.
 *
 *
 *
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_H
#define GIST_H

#include "access/funcindex.h"
#include "access/itup.h"
#include "access/relscan.h"
#include "access/sdir.h"

/*
** You can have as many strategies as you please in GiSTs, as
** long as your consistent method can handle them
**
** But strat.h->StrategyEvaluationData->StrategyExpression	expression[12]
** - so 12 is real max # of strategies, or StrategyEvaluationIsValid
** crashes backend...			- vadim 05/21/97

#define GISTNStrategies					100

*/
#define GISTNStrategies					12

/*
** Helper routines
*/
#define GISTNProcs						8
#define GIST_CONSISTENT_PROC			1
#define GIST_UNION_PROC					2
#define GIST_COMPRESS_PROC				3
#define GIST_DECOMPRESS_PROC			4
#define GIST_PENALTY_PROC				5
#define GIST_PICKSPLIT_PROC				6
#define GIST_EQUAL_PROC					7
#define GIST_INFO_PROC					8

#define F_LEAF			(1 << 0)

typedef struct GISTPageOpaqueData
{
	uint32		flags;
} GISTPageOpaqueData;

typedef GISTPageOpaqueData *GISTPageOpaque;

#define GIST_LEAF(entry) (((GISTPageOpaque) PageGetSpecialPointer((entry)->page))->flags & F_LEAF)

/*
 *	When we descend a tree, we keep a stack of parent pointers.
 */

typedef struct GISTSTACK
{
	struct GISTSTACK *gs_parent;
	OffsetNumber gs_child;
	BlockNumber gs_blk;
} GISTSTACK;

typedef struct GISTSTATE
{
	FmgrInfo	consistentFn;
	FmgrInfo	unionFn;
	FmgrInfo	compressFn;
	FmgrInfo	decompressFn;
	FmgrInfo	penaltyFn;
	FmgrInfo	picksplitFn;
	FmgrInfo	equalFn;
	bool		haskeytype;
	bool		keytypbyval;
} GISTSTATE;


/*
**	When we're doing a scan, we need to keep track of the parent stack
**	for the marked and current items.
*/

typedef struct GISTScanOpaqueData
{
	struct GISTSTACK *s_stack;
	struct GISTSTACK *s_markstk;
	uint16		s_flags;
	struct GISTSTATE *giststate;
} GISTScanOpaqueData;

typedef GISTScanOpaqueData *GISTScanOpaque;

/*
**	When we're doing a scan and updating a tree at the same time, the
**	updates may affect the scan.  We use the flags entry of the scan's
**	opaque space to record our actual position in response to updates
**	that we can't handle simply by adjusting pointers.
*/

#define GS_CURBEFORE	((uint16) (1 << 0))
#define GS_MRKBEFORE	((uint16) (1 << 1))

/* root page of a gist */
#define GISTP_ROOT				0

/*
**	When we update a relation on which we're doing a scan, we need to
**	check the scan and fix it if the update affected any of the pages it
**	touches.  Otherwise, we can miss records that we should see.  The only
**	times we need to do this are for deletions and splits.	See the code in
**	gistscan.c for how the scan is fixed. These two constants tell us what sort
**	of operation changed the index.
*/

#define GISTOP_DEL		0
#define GISTOP_SPLIT	1

/*
** This is the Split Vector to be returned by the PickSplit method.
*/
typedef struct GIST_SPLITVEC
{
	OffsetNumber *spl_left;		/* array of entries that go left */
	int			spl_nleft;		/* size of this array */
	char	   *spl_ldatum;		/* Union of keys in spl_left */
	OffsetNumber *spl_right;	/* array of entries that go right */
	int			spl_nright;		/* size of the array */
	char	   *spl_rdatum;		/* Union of keys in spl_right */
} GIST_SPLITVEC;

/*
** An entry on a GiST node.  Contains the key (pred), as well as
** its own location (rel,page,offset) which can supply the matching
** pointer.  The size of the pred is in bytes, and leafkey is a flag to
** tell us if the entry is in a leaf node.
*/
typedef struct GISTENTRY
{
	char	   *pred;
	Relation	rel;
	Page		page;
	OffsetNumber offset;
	int			bytes;
	bool		leafkey;
} GISTENTRY;

/*
** macro to initialize a GISTENTRY
*/
#define gistentryinit(e, pr, r, pg, o, b, l)\
   do {(e).pred = pr; (e).rel = r; (e).page = pg; (e).offset = o; (e).bytes = b; (e).leafkey = l;} while (0)

/* defined in gist.c */
#define TRLOWER(tr) (((tr)->bytes))
#define TRUPPER(tr) (&((tr)->bytes[MAXALIGN(VARSIZE(TRLOWER(tr)))]))

typedef struct txtrange
{

	/*
	 * flag: NINF means that lower is negative infinity; PINF means that *
	 * upper is positive infinity.	0 means that both are numbers.
	 */
	int32		vl_len;
	int32		flag;
	char		bytes[2];
} TXTRANGE;

typedef struct intrange
{
	int			lower;
	int			upper;

	/*
	 * flag: NINF means that lower is negative infinity; PINF means that *
	 * upper is positive infinity.	0 means that both are numbers.
	 */
	int			flag;
} INTRANGE;

extern void gistbuild(Relation heap,
		  Relation index, int natts,
		  AttrNumber *attnum, IndexStrategy istrat,
		  uint16 pint, Datum *params,
		  FuncIndexInfo *finfo,
		  PredInfo *predInfo);
extern InsertIndexResult gistinsert(Relation r, Datum *datum,
		   char *nulls, ItemPointer ht_ctid, Relation heapRel);
extern void _gistdump(Relation r);
extern void gistfreestack(GISTSTACK *s);
extern void initGISTstate(GISTSTATE *giststate, Relation index);
extern void gistdentryinit(GISTSTATE *giststate, GISTENTRY *e, char *pr,
			   Relation r, Page pg, OffsetNumber o, int b, bool l);
extern StrategyNumber RelationGetGISTStrategy(Relation, AttrNumber, RegProcedure);

/* gistget.c */
extern RetrieveIndexResult gistgettuple(IndexScanDesc s, ScanDirection dir);

#endif	 /* GIST_H */
