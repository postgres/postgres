/*-------------------------------------------------------------------------
 *
 * gistscan.c
 *	  routines to manage scans on index relations
 *
 *
 * IDENTIFICATION
 *	  /usr/local/devel/pglite/cvs/src/backend/access/gist/gistscan.c,v 1.7 1995/06/14 00:10:05 jolly Exp
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/gist.h"
#include "access/gistscan.h"


/* routines defined and used here */
static void gistregscan(IndexScanDesc s);
static void gistdropscan(IndexScanDesc s);
static void gistadjone(IndexScanDesc s, int op, BlockNumber blkno,
		   OffsetNumber offnum);
static void adjuststack(GISTSTACK *stk, BlockNumber blkno,
			OffsetNumber offnum);
static void adjustiptr(IndexScanDesc s, ItemPointer iptr,
		   int op, BlockNumber blkno, OffsetNumber offnum);

/*
 *	Whenever we start a GiST scan in a backend, we register it in private
 *	space.	Then if the GiST index gets updated, we check all registered
 *	scans and adjust them if the tuple they point at got moved by the
 *	update.  We only need to do this in private space, because when we update
 *	an GiST we have a write lock on the tree, so no other process can have
 *	any locks at all on it.  A single transaction can have write and read
 *	locks on the same object, so that's why we need to handle this case.
 */

typedef struct GISTScanListData
{
	IndexScanDesc gsl_scan;
	struct GISTScanListData *gsl_next;
} GISTScanListData;

typedef GISTScanListData *GISTScanList;

/* pointer to list of local scans on GiSTs */
static GISTScanList GISTScans = (GISTScanList) NULL;

Datum
gistbeginscan(PG_FUNCTION_ARGS)
{
	Relation	r = (Relation) PG_GETARG_POINTER(0);
	bool		fromEnd = PG_GETARG_BOOL(1);
	uint16		nkeys = PG_GETARG_UINT16(2);
	ScanKey		key = (ScanKey) PG_GETARG_POINTER(3);
	IndexScanDesc s;

	/*
	 * Let index_beginscan does its work...
	 *
	 * RelationSetLockForRead(r);
	 */

	s = RelationGetIndexScan(r, fromEnd, nkeys, key);
	gistregscan(s);

	PG_RETURN_POINTER(s);
}

Datum
gistrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc s = (IndexScanDesc) PG_GETARG_POINTER(0);
	bool		fromEnd = PG_GETARG_BOOL(1);
	ScanKey		key = (ScanKey) PG_GETARG_POINTER(2);
	GISTScanOpaque p;
	int			i;

	/*
	 * Clear all the pointers.
	 */

	ItemPointerSetInvalid(&s->previousItemData);
	ItemPointerSetInvalid(&s->currentItemData);
	ItemPointerSetInvalid(&s->nextItemData);
	ItemPointerSetInvalid(&s->previousMarkData);
	ItemPointerSetInvalid(&s->currentMarkData);
	ItemPointerSetInvalid(&s->nextMarkData);

	/*
	 * Set flags.
	 */
	if (RelationGetNumberOfBlocks(s->relation) == 0)
		s->flags = ScanUnmarked;
	else if (fromEnd)
		s->flags = ScanUnmarked | ScanUncheckedPrevious;
	else
		s->flags = ScanUnmarked | ScanUncheckedNext;

	s->scanFromEnd = fromEnd;

	if (s->numberOfKeys > 0)
	{
		memmove(s->keyData,
				key,
				s->numberOfKeys * sizeof(ScanKeyData));
	}

	p = (GISTScanOpaque) s->opaque;
	if (p != (GISTScanOpaque) NULL)
	{
		gistfreestack(p->s_stack);
		gistfreestack(p->s_markstk);
		p->s_stack = p->s_markstk = (GISTSTACK *) NULL;
		p->s_flags = 0x0;
		for (i = 0; i < s->numberOfKeys; i++)
		{
			s->keyData[i].sk_procedure
				= RelationGetGISTStrategy(s->relation, s->keyData[i].sk_attno,
										  s->keyData[i].sk_procedure);
			s->keyData[i].sk_func = p->giststate->consistentFn;
		}
	}
	else
	{
		/* initialize opaque data */
		p = (GISTScanOpaque) palloc(sizeof(GISTScanOpaqueData));
		p->s_stack = p->s_markstk = (GISTSTACK *) NULL;
		p->s_flags = 0x0;
		s->opaque = p;
		p->giststate = (GISTSTATE *) palloc(sizeof(GISTSTATE));
		initGISTstate(p->giststate, s->relation);
		if (s->numberOfKeys > 0)

			/*
			 * * Play games here with the scan key to use the Consistent *
			 * function for all comparisons: * 1) the sk_procedure field
			 * will now be used to hold the *	 strategy number * 2) the
			 * sk_func field will point to the Consistent function
			 */
			for (i = 0; i < s->numberOfKeys; i++)
			{

				/*
				 * s->keyData[i].sk_procedure =
				 * index_getprocid(s->relation, 1, GIST_CONSISTENT_PROC);
				 */
				s->keyData[i].sk_procedure
					= RelationGetGISTStrategy(s->relation, s->keyData[i].sk_attno,
											  s->keyData[i].sk_procedure);
				s->keyData[i].sk_func = p->giststate->consistentFn;
			}
	}

	PG_RETURN_VOID();
}

Datum
gistmarkpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc s = (IndexScanDesc) PG_GETARG_POINTER(0);
	GISTScanOpaque p;
	GISTSTACK  *o,
			   *n,
			   *tmp;

	s->currentMarkData = s->currentItemData;
	p = (GISTScanOpaque) s->opaque;
	if (p->s_flags & GS_CURBEFORE)
		p->s_flags |= GS_MRKBEFORE;
	else
		p->s_flags &= ~GS_MRKBEFORE;

	o = (GISTSTACK *) NULL;
	n = p->s_stack;

	/* copy the parent stack from the current item data */
	while (n != (GISTSTACK *) NULL)
	{
		tmp = (GISTSTACK *) palloc(sizeof(GISTSTACK));
		tmp->gs_child = n->gs_child;
		tmp->gs_blk = n->gs_blk;
		tmp->gs_parent = o;
		o = tmp;
		n = n->gs_parent;
	}

	gistfreestack(p->s_markstk);
	p->s_markstk = o;

	PG_RETURN_VOID();
}

Datum
gistrestrpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc s = (IndexScanDesc) PG_GETARG_POINTER(0);
	GISTScanOpaque p;
	GISTSTACK  *o,
			   *n,
			   *tmp;

	s->currentItemData = s->currentMarkData;
	p = (GISTScanOpaque) s->opaque;
	if (p->s_flags & GS_MRKBEFORE)
		p->s_flags |= GS_CURBEFORE;
	else
		p->s_flags &= ~GS_CURBEFORE;

	o = (GISTSTACK *) NULL;
	n = p->s_markstk;

	/* copy the parent stack from the current item data */
	while (n != (GISTSTACK *) NULL)
	{
		tmp = (GISTSTACK *) palloc(sizeof(GISTSTACK));
		tmp->gs_child = n->gs_child;
		tmp->gs_blk = n->gs_blk;
		tmp->gs_parent = o;
		o = tmp;
		n = n->gs_parent;
	}

	gistfreestack(p->s_stack);
	p->s_stack = o;

	PG_RETURN_VOID();
}

Datum
gistendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc s = (IndexScanDesc) PG_GETARG_POINTER(0);
	GISTScanOpaque p;

	p = (GISTScanOpaque) s->opaque;

	if (p != (GISTScanOpaque) NULL)
	{
		gistfreestack(p->s_stack);
		gistfreestack(p->s_markstk);
		pfree(s->opaque);
	}

	gistdropscan(s);
	/* XXX don't unset read lock -- two-phase locking */

	PG_RETURN_VOID();
}

static void
gistregscan(IndexScanDesc s)
{
	GISTScanList l;

	l = (GISTScanList) palloc(sizeof(GISTScanListData));
	l->gsl_scan = s;
	l->gsl_next = GISTScans;
	GISTScans = l;
}

static void
gistdropscan(IndexScanDesc s)
{
	GISTScanList l;
	GISTScanList prev;

	prev = (GISTScanList) NULL;

	for (l = GISTScans;
		 l != (GISTScanList) NULL && l->gsl_scan != s;
		 l = l->gsl_next)
		prev = l;

	if (l == (GISTScanList) NULL)
		elog(ERROR, "GiST scan list corrupted -- cannot find 0x%p", (void *) s);

	if (prev == (GISTScanList) NULL)
		GISTScans = l->gsl_next;
	else
		prev->gsl_next = l->gsl_next;

	pfree(l);
}

void
gistadjscans(Relation rel, int op, BlockNumber blkno, OffsetNumber offnum)
{
	GISTScanList l;
	Oid			relid;

	relid = RelationGetRelid(rel);
	for (l = GISTScans; l != (GISTScanList) NULL; l = l->gsl_next)
	{
		if (l->gsl_scan->relation->rd_id == relid)
			gistadjone(l->gsl_scan, op, blkno, offnum);
	}
}

/*
 *	gistadjone() -- adjust one scan for update.
 *
 *		By here, the scan passed in is on a modified relation.	Op tells
 *		us what the modification is, and blkno and offind tell us what
 *		block and offset index were affected.  This routine checks the
 *		current and marked positions, and the current and marked stacks,
 *		to see if any stored location needs to be changed because of the
 *		update.  If so, we make the change here.
 */
static void
gistadjone(IndexScanDesc s,
		   int op,
		   BlockNumber blkno,
		   OffsetNumber offnum)
{
	GISTScanOpaque so;

	adjustiptr(s, &(s->currentItemData), op, blkno, offnum);
	adjustiptr(s, &(s->currentMarkData), op, blkno, offnum);

	so = (GISTScanOpaque) s->opaque;

	if (op == GISTOP_SPLIT)
	{
		adjuststack(so->s_stack, blkno, offnum);
		adjuststack(so->s_markstk, blkno, offnum);
	}
}

/*
 *	adjustiptr() -- adjust current and marked item pointers in the scan
 *
 *		Depending on the type of update and the place it happened, we
 *		need to do nothing, to back up one record, or to start over on
 *		the same page.
 */
static void
adjustiptr(IndexScanDesc s,
		   ItemPointer iptr,
		   int op,
		   BlockNumber blkno,
		   OffsetNumber offnum)
{
	OffsetNumber curoff;
	GISTScanOpaque so;

	if (ItemPointerIsValid(iptr))
	{
		if (ItemPointerGetBlockNumber(iptr) == blkno)
		{
			curoff = ItemPointerGetOffsetNumber(iptr);
			so = (GISTScanOpaque) s->opaque;

			switch (op)
			{
				case GISTOP_DEL:
					/* back up one if we need to */
					if (curoff >= offnum)
					{

						if (curoff > FirstOffsetNumber)
						{
							/* just adjust the item pointer */
							ItemPointerSet(iptr, blkno, OffsetNumberPrev(curoff));
						}
						else
						{

							/*
							 * remember that we're before the current
							 * tuple
							 */
							ItemPointerSet(iptr, blkno, FirstOffsetNumber);
							if (iptr == &(s->currentItemData))
								so->s_flags |= GS_CURBEFORE;
							else
								so->s_flags |= GS_MRKBEFORE;
						}
					}
					break;

				case GISTOP_SPLIT:
					/* back to start of page on split */
					ItemPointerSet(iptr, blkno, FirstOffsetNumber);
					if (iptr == &(s->currentItemData))
						so->s_flags &= ~GS_CURBEFORE;
					else
						so->s_flags &= ~GS_MRKBEFORE;
					break;

				default:
					elog(ERROR, "Bad operation in GiST scan adjust: %d", op);
			}
		}
	}
}

/*
 *	adjuststack() -- adjust the supplied stack for a split on a page in
 *					 the index we're scanning.
 *
 *		If a page on our parent stack has split, we need to back up to the
 *		beginning of the page and rescan it.  The reason for this is that
 *		the split algorithm for GiSTs doesn't order tuples in any useful
 *		way on a single page.  This means on that a split, we may wind up
 *		looking at some heap tuples more than once.  This is handled in the
 *		access method update code for heaps; if we've modified the tuple we
 *		are looking at already in this transaction, we ignore the update
 *		request.
 */
/*ARGSUSED*/
static void
adjuststack(GISTSTACK *stk,
			BlockNumber blkno,
			OffsetNumber offnum)
{
	while (stk != (GISTSTACK *) NULL)
	{
		if (stk->gs_blk == blkno)
			stk->gs_child = FirstOffsetNumber;

		stk = stk->gs_parent;
	}
}
