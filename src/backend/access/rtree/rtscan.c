/*-------------------------------------------------------------------------
 *
 * rtscan.c--
 *	  routines to manage scans on index relations
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/rtree/Attic/rtscan.c,v 1.17 1998/08/19 02:01:21 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <storage/bufmgr.h>
#include <access/genam.h>
#include <storage/lmgr.h>
#include <storage/bufpage.h>
#include <access/rtree.h>
#include <access/rtstrat.h>
#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif


/* routines defined and used here */
static void rtregscan(IndexScanDesc s);
static void rtdropscan(IndexScanDesc s);
static void
rtadjone(IndexScanDesc s, int op, BlockNumber blkno,
		 OffsetNumber offnum);
static void
adjuststack(RTSTACK *stk, BlockNumber blkno,
			OffsetNumber offnum);
static void
adjustiptr(IndexScanDesc s, ItemPointer iptr,
		   int op, BlockNumber blkno, OffsetNumber offnum);

/*
 *	Whenever we start an rtree scan in a backend, we register it in private
 *	space.	Then if the rtree index gets updated, we check all registered
 *	scans and adjust them if the tuple they point at got moved by the
 *	update.  We only need to do this in private space, because when we update
 *	an rtree we have a write lock on the tree, so no other process can have
 *	any locks at all on it.  A single transaction can have write and read
 *	locks on the same object, so that's why we need to handle this case.
 */

typedef struct RTScanListData
{
	IndexScanDesc rtsl_scan;
	struct RTScanListData *rtsl_next;
} RTScanListData;

typedef RTScanListData *RTScanList;

/* pointer to list of local scans on rtrees */
static RTScanList RTScans = (RTScanList) NULL;

IndexScanDesc
rtbeginscan(Relation r,
			bool fromEnd,
			uint16 nkeys,
			ScanKey key)
{
	IndexScanDesc s;

	RelationSetLockForRead(r);
	s = RelationGetIndexScan(r, fromEnd, nkeys, key);
	rtregscan(s);

	return (s);
}

void
rtrescan(IndexScanDesc s, bool fromEnd, ScanKey key)
{
	RTreeScanOpaque p;
	RegProcedure internal_proc;
	int			i;

	if (!IndexScanIsValid(s))
	{
		elog(ERROR, "rtrescan: invalid scan.");
		return;
	}

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

	p = (RTreeScanOpaque) s->opaque;
	if (p != (RTreeScanOpaque) NULL)
	{
		freestack(p->s_stack);
		freestack(p->s_markstk);
		p->s_stack = p->s_markstk = (RTSTACK *) NULL;
		p->s_flags = 0x0;
		for (i = 0; i < s->numberOfKeys; i++)
			p->s_internalKey[i].sk_argument = s->keyData[i].sk_argument;
	}
	else
	{
		/* initialize opaque data */
		p = (RTreeScanOpaque) palloc(sizeof(RTreeScanOpaqueData));
		p->s_stack = p->s_markstk = (RTSTACK *) NULL;
		p->s_internalNKey = s->numberOfKeys;
		p->s_flags = 0x0;
		s->opaque = p;
		if (s->numberOfKeys > 0)
		{
			p->s_internalKey =
				(ScanKey) palloc(sizeof(ScanKeyData) * s->numberOfKeys);

			/*
			 * Scans on internal pages use different operators than they
			 * do on leaf pages.  For example, if the user wants all boxes
			 * that exactly match (x1,y1,x2,y2), then on internal pages we
			 * need to find all boxes that contain (x1,y1,x2,y2).
			 */

			for (i = 0; i < s->numberOfKeys; i++)
			{
				p->s_internalKey[i].sk_argument = s->keyData[i].sk_argument;
				internal_proc = RTMapOperator(s->relation,
											  s->keyData[i].sk_attno,
											  s->keyData[i].sk_procedure);
				ScanKeyEntryInitialize(&(p->s_internalKey[i]),
									   s->keyData[i].sk_flags,
									   s->keyData[i].sk_attno,
									   internal_proc,
									   s->keyData[i].sk_argument);
			}
		}
	}
}

void
rtmarkpos(IndexScanDesc s)
{
	RTreeScanOpaque p;
	RTSTACK    *o,
			   *n,
			   *tmp;

	s->currentMarkData = s->currentItemData;
	p = (RTreeScanOpaque) s->opaque;
	if (p->s_flags & RTS_CURBEFORE)
		p->s_flags |= RTS_MRKBEFORE;
	else
		p->s_flags &= ~RTS_MRKBEFORE;

	o = (RTSTACK *) NULL;
	n = p->s_stack;

	/* copy the parent stack from the current item data */
	while (n != (RTSTACK *) NULL)
	{
		tmp = (RTSTACK *) palloc(sizeof(RTSTACK));
		tmp->rts_child = n->rts_child;
		tmp->rts_blk = n->rts_blk;
		tmp->rts_parent = o;
		o = tmp;
		n = n->rts_parent;
	}

	freestack(p->s_markstk);
	p->s_markstk = o;
}

void
rtrestrpos(IndexScanDesc s)
{
	RTreeScanOpaque p;
	RTSTACK    *o,
			   *n,
			   *tmp;

	s->currentItemData = s->currentMarkData;
	p = (RTreeScanOpaque) s->opaque;
	if (p->s_flags & RTS_MRKBEFORE)
		p->s_flags |= RTS_CURBEFORE;
	else
		p->s_flags &= ~RTS_CURBEFORE;

	o = (RTSTACK *) NULL;
	n = p->s_markstk;

	/* copy the parent stack from the current item data */
	while (n != (RTSTACK *) NULL)
	{
		tmp = (RTSTACK *) palloc(sizeof(RTSTACK));
		tmp->rts_child = n->rts_child;
		tmp->rts_blk = n->rts_blk;
		tmp->rts_parent = o;
		o = tmp;
		n = n->rts_parent;
	}

	freestack(p->s_stack);
	p->s_stack = o;
}

void
rtendscan(IndexScanDesc s)
{
	RTreeScanOpaque p;

	p = (RTreeScanOpaque) s->opaque;

	if (p != (RTreeScanOpaque) NULL)
	{
		freestack(p->s_stack);
		freestack(p->s_markstk);
		pfree(s->opaque);
	}

	rtdropscan(s);
	/* XXX don't unset read lock -- two-phase locking */
}

static void
rtregscan(IndexScanDesc s)
{
	RTScanList	l;

	l = (RTScanList) palloc(sizeof(RTScanListData));
	l->rtsl_scan = s;
	l->rtsl_next = RTScans;
	RTScans = l;
}

static void
rtdropscan(IndexScanDesc s)
{
	RTScanList	l;
	RTScanList	prev;

	prev = (RTScanList) NULL;

	for (l = RTScans;
		 l != (RTScanList) NULL && l->rtsl_scan != s;
		 l = l->rtsl_next)
		prev = l;

	if (l == (RTScanList) NULL)
		elog(ERROR, "rtree scan list corrupted -- cannot find 0x%lx", s);

	if (prev == (RTScanList) NULL)
		RTScans = l->rtsl_next;
	else
		prev->rtsl_next = l->rtsl_next;

	pfree(l);
}

void
rtadjscans(Relation r, int op, BlockNumber blkno, OffsetNumber offnum)
{
	RTScanList	l;
	Oid			relid;

	relid = RelationGetRelid(r);
	for (l = RTScans; l != (RTScanList) NULL; l = l->rtsl_next)
	{
		if (RelationGetRelid(l->rtsl_scan->relation) == relid)
			rtadjone(l->rtsl_scan, op, blkno, offnum);
	}
}

/*
 *	rtadjone() -- adjust one scan for update.
 *
 *		By here, the scan passed in is on a modified relation.	Op tells
 *		us what the modification is, and blkno and offind tell us what
 *		block and offset index were affected.  This routine checks the
 *		current and marked positions, and the current and marked stacks,
 *		to see if any stored location needs to be changed because of the
 *		update.  If so, we make the change here.
 */
static void
rtadjone(IndexScanDesc s,
		 int op,
		 BlockNumber blkno,
		 OffsetNumber offnum)
{
	RTreeScanOpaque so;

	adjustiptr(s, &(s->currentItemData), op, blkno, offnum);
	adjustiptr(s, &(s->currentMarkData), op, blkno, offnum);

	so = (RTreeScanOpaque) s->opaque;

	if (op == RTOP_SPLIT)
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
	RTreeScanOpaque so;

	if (ItemPointerIsValid(iptr))
	{
		if (ItemPointerGetBlockNumber(iptr) == blkno)
		{
			curoff = ItemPointerGetOffsetNumber(iptr);
			so = (RTreeScanOpaque) s->opaque;

			switch (op)
			{
				case RTOP_DEL:
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
								so->s_flags |= RTS_CURBEFORE;
							else
								so->s_flags |= RTS_MRKBEFORE;
						}
					}
					break;

				case RTOP_SPLIT:
					/* back to start of page on split */
					ItemPointerSet(iptr, blkno, FirstOffsetNumber);
					if (iptr == &(s->currentItemData))
						so->s_flags &= ~RTS_CURBEFORE;
					else
						so->s_flags &= ~RTS_MRKBEFORE;
					break;

				default:
					elog(ERROR, "Bad operation in rtree scan adjust: %d", op);
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
 *		the split algorithm for rtrees doesn't order tuples in any useful
 *		way on a single page.  This means on that a split, we may wind up
 *		looking at some heap tuples more than once.  This is handled in the
 *		access method update code for heaps; if we've modified the tuple we
 *		are looking at already in this transaction, we ignore the update
 *		request.
 */
/*ARGSUSED*/
static void
adjuststack(RTSTACK *stk,
			BlockNumber blkno,
			OffsetNumber offnum)
{
	while (stk != (RTSTACK *) NULL)
	{
		if (stk->rts_blk == blkno)
			stk->rts_child = FirstOffsetNumber;

		stk = stk->rts_parent;
	}
}
