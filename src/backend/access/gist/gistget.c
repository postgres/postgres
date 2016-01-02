/*-------------------------------------------------------------------------
 *
 * gistget.c
 *	  fetch tuples from a GiST scan.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistget.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist_private.h"
#include "access/relscan.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "lib/pairingheap.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * gistkillitems() -- set LP_DEAD state for items an indexscan caller has
 * told us were killed.
 *
 * We re-read page here, so it's important to check page LSN. If the page
 * has been modified since the last read (as determined by LSN), we cannot
 * flag any entries because it is possible that the old entry was vacuumed
 * away and the TID was re-used by a completely different heap tuple.
 */
static void
gistkillitems(IndexScanDesc scan)
{
	GISTScanOpaque 	so = (GISTScanOpaque) scan->opaque;
	Buffer			buffer;
	Page			page;
	OffsetNumber	offnum;
	ItemId			iid;
	int				i;
	bool			killedsomething = false;

	Assert(so->curBlkno != InvalidBlockNumber);
	Assert(!XLogRecPtrIsInvalid(so->curPageLSN));
	Assert(so->killedItems != NULL);

	buffer = ReadBuffer(scan->indexRelation, so->curBlkno);
	if (!BufferIsValid(buffer))
		return;

	LockBuffer(buffer, GIST_SHARE);
	gistcheckpage(scan->indexRelation, buffer);
	page = BufferGetPage(buffer);

	/*
	 * If page LSN differs it means that the page was modified since the last read.
	 * killedItems could be not valid so LP_DEAD hints applying is not safe.
	 */
	if(PageGetLSN(page) != so->curPageLSN)
	{
		UnlockReleaseBuffer(buffer);
		so->numKilled = 0; /* reset counter */
		return;
	}

	Assert(GistPageIsLeaf(page));

	/*
	 * Mark all killedItems as dead. We need no additional recheck,
	 * because, if page was modified, pageLSN must have changed.
	 */
	for (i = 0; i < so->numKilled; i++)
	{
		offnum = so->killedItems[i];
		iid = PageGetItemId(page, offnum);
		ItemIdMarkDead(iid);
		killedsomething = true;
	}

	if (killedsomething)
	{
		GistMarkPageHasGarbage(page);
		MarkBufferDirtyHint(buffer, true);
	}

	UnlockReleaseBuffer(buffer);

	/*
	 * Always reset the scan state, so we don't look for same items on other
	 * pages.
	 */
	so->numKilled = 0;
}

/*
 * gistindex_keytest() -- does this index tuple satisfy the scan key(s)?
 *
 * The index tuple might represent either a heap tuple or a lower index page,
 * depending on whether the containing page is a leaf page or not.
 *
 * On success return for a heap tuple, *recheck_p is set to indicate whether
 * the quals need to be rechecked.  We recheck if any of the consistent()
 * functions request it.  recheck is not interesting when examining a non-leaf
 * entry, since we must visit the lower index page if there's any doubt.
 * Similarly, *recheck_distances_p is set to indicate whether the distances
 * need to be rechecked, and it is also ignored for non-leaf entries.
 *
 * If we are doing an ordered scan, so->distances[] is filled with distance
 * data from the distance() functions before returning success.
 *
 * We must decompress the key in the IndexTuple before passing it to the
 * sk_funcs (which actually are the opclass Consistent or Distance methods).
 *
 * Note that this function is always invoked in a short-lived memory context,
 * so we don't need to worry about cleaning up allocated memory, either here
 * or in the implementation of any Consistent or Distance methods.
 */
static bool
gistindex_keytest(IndexScanDesc scan,
				  IndexTuple tuple,
				  Page page,
				  OffsetNumber offset,
				  bool *recheck_p,
				  bool *recheck_distances_p)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	GISTSTATE  *giststate = so->giststate;
	ScanKey		key = scan->keyData;
	int			keySize = scan->numberOfKeys;
	double	   *distance_p;
	Relation	r = scan->indexRelation;

	*recheck_p = false;
	*recheck_distances_p = false;

	/*
	 * If it's a leftover invalid tuple from pre-9.1, treat it as a match with
	 * minimum possible distances.  This means we'll always follow it to the
	 * referenced page.
	 */
	if (GistTupleIsInvalid(tuple))
	{
		int			i;

		if (GistPageIsLeaf(page))		/* shouldn't happen */
			elog(ERROR, "invalid GiST tuple found on leaf page");
		for (i = 0; i < scan->numberOfOrderBys; i++)
			so->distances[i] = -get_float8_infinity();
		return true;
	}

	/* Check whether it matches according to the Consistent functions */
	while (keySize > 0)
	{
		Datum		datum;
		bool		isNull;

		datum = index_getattr(tuple,
							  key->sk_attno,
							  giststate->tupdesc,
							  &isNull);

		if (key->sk_flags & SK_ISNULL)
		{
			/*
			 * On non-leaf page we can't conclude that child hasn't NULL
			 * values because of assumption in GiST: union (VAL, NULL) is VAL.
			 * But if on non-leaf page key IS NULL, then all children are
			 * NULL.
			 */
			if (key->sk_flags & SK_SEARCHNULL)
			{
				if (GistPageIsLeaf(page) && !isNull)
					return false;
			}
			else
			{
				Assert(key->sk_flags & SK_SEARCHNOTNULL);
				if (isNull)
					return false;
			}
		}
		else if (isNull)
		{
			return false;
		}
		else
		{
			Datum		test;
			bool		recheck;
			GISTENTRY	de;

			gistdentryinit(giststate, key->sk_attno - 1, &de,
						   datum, r, page, offset,
						   FALSE, isNull);

			/*
			 * Call the Consistent function to evaluate the test.  The
			 * arguments are the index datum (as a GISTENTRY*), the comparison
			 * datum, the comparison operator's strategy number and subtype
			 * from pg_amop, and the recheck flag.
			 *
			 * (Presently there's no need to pass the subtype since it'll
			 * always be zero, but might as well pass it for possible future
			 * use.)
			 *
			 * We initialize the recheck flag to true (the safest assumption)
			 * in case the Consistent function forgets to set it.
			 */
			recheck = true;

			test = FunctionCall5Coll(&key->sk_func,
									 key->sk_collation,
									 PointerGetDatum(&de),
									 key->sk_argument,
									 Int32GetDatum(key->sk_strategy),
									 ObjectIdGetDatum(key->sk_subtype),
									 PointerGetDatum(&recheck));

			if (!DatumGetBool(test))
				return false;
			*recheck_p |= recheck;
		}

		key++;
		keySize--;
	}

	/* OK, it passes --- now let's compute the distances */
	key = scan->orderByData;
	distance_p = so->distances;
	keySize = scan->numberOfOrderBys;
	while (keySize > 0)
	{
		Datum		datum;
		bool		isNull;

		datum = index_getattr(tuple,
							  key->sk_attno,
							  giststate->tupdesc,
							  &isNull);

		if ((key->sk_flags & SK_ISNULL) || isNull)
		{
			/* Assume distance computes as null and sorts to the end */
			*distance_p = get_float8_infinity();
		}
		else
		{
			Datum		dist;
			bool		recheck;
			GISTENTRY	de;

			gistdentryinit(giststate, key->sk_attno - 1, &de,
						   datum, r, page, offset,
						   FALSE, isNull);

			/*
			 * Call the Distance function to evaluate the distance.  The
			 * arguments are the index datum (as a GISTENTRY*), the comparison
			 * datum, the ordering operator's strategy number and subtype from
			 * pg_amop, and the recheck flag.
			 *
			 * (Presently there's no need to pass the subtype since it'll
			 * always be zero, but might as well pass it for possible future
			 * use.)
			 *
			 * If the function sets the recheck flag, the returned distance is
			 * a lower bound on the true distance and needs to be rechecked.
			 * We initialize the flag to 'false'.  This flag was added in
			 * version 9.5; distance functions written before that won't know
			 * about the flag, but are expected to never be lossy.
			 */
			recheck = false;
			dist = FunctionCall5Coll(&key->sk_func,
									 key->sk_collation,
									 PointerGetDatum(&de),
									 key->sk_argument,
									 Int32GetDatum(key->sk_strategy),
									 ObjectIdGetDatum(key->sk_subtype),
									 PointerGetDatum(&recheck));
			*recheck_distances_p |= recheck;
			*distance_p = DatumGetFloat8(dist);
		}

		key++;
		distance_p++;
		keySize--;
	}

	return true;
}

/*
 * Scan all items on the GiST index page identified by *pageItem, and insert
 * them into the queue (or directly to output areas)
 *
 * scan: index scan we are executing
 * pageItem: search queue item identifying an index page to scan
 * myDistances: distances array associated with pageItem, or NULL at the root
 * tbm: if not NULL, gistgetbitmap's output bitmap
 * ntids: if not NULL, gistgetbitmap's output tuple counter
 *
 * If tbm/ntids aren't NULL, we are doing an amgetbitmap scan, and heap
 * tuples should be reported directly into the bitmap.  If they are NULL,
 * we're doing a plain or ordered indexscan.  For a plain indexscan, heap
 * tuple TIDs are returned into so->pageData[].  For an ordered indexscan,
 * heap tuple TIDs are pushed into individual search queue items.  In an
 * index-only scan, reconstructed index tuples are returned along with the
 * TIDs.
 *
 * If we detect that the index page has split since we saw its downlink
 * in the parent, we push its new right sibling onto the queue so the
 * sibling will be processed next.
 */
static void
gistScanPage(IndexScanDesc scan, GISTSearchItem *pageItem, double *myDistances,
			 TIDBitmap *tbm, int64 *ntids)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	GISTSTATE  *giststate = so->giststate;
	Relation	r = scan->indexRelation;
	Buffer		buffer;
	Page		page;
	GISTPageOpaque opaque;
	OffsetNumber maxoff;
	OffsetNumber i;
	MemoryContext oldcxt;

	Assert(!GISTSearchItemIsHeap(*pageItem));

	buffer = ReadBuffer(scan->indexRelation, pageItem->blkno);
	LockBuffer(buffer, GIST_SHARE);
	gistcheckpage(scan->indexRelation, buffer);
	page = BufferGetPage(buffer);
	opaque = GistPageGetOpaque(page);

	/*
	 * Check if we need to follow the rightlink. We need to follow it if the
	 * page was concurrently split since we visited the parent (in which case
	 * parentlsn < nsn), or if the system crashed after a page split but
	 * before the downlink was inserted into the parent.
	 */
	if (!XLogRecPtrIsInvalid(pageItem->data.parentlsn) &&
		(GistFollowRight(page) ||
		 pageItem->data.parentlsn < GistPageGetNSN(page)) &&
		opaque->rightlink != InvalidBlockNumber /* sanity check */ )
	{
		/* There was a page split, follow right link to add pages */
		GISTSearchItem *item;

		/* This can't happen when starting at the root */
		Assert(myDistances != NULL);

		oldcxt = MemoryContextSwitchTo(so->queueCxt);

		/* Create new GISTSearchItem for the right sibling index page */
		item = palloc(SizeOfGISTSearchItem(scan->numberOfOrderBys));
		item->blkno = opaque->rightlink;
		item->data.parentlsn = pageItem->data.parentlsn;

		/* Insert it into the queue using same distances as for this page */
		memcpy(item->distances, myDistances,
			   sizeof(double) * scan->numberOfOrderBys);

		pairingheap_add(so->queue, &item->phNode);

		MemoryContextSwitchTo(oldcxt);
	}

	so->nPageData = so->curPageData = 0;
	if (so->pageDataCxt)
		MemoryContextReset(so->pageDataCxt);

	/*
	 * We save the LSN of the page as we read it, so that we know whether it
	 * safe to apply LP_DEAD hints to the page later. This allows us to drop
	 * the pin for MVCC scans, which allows vacuum to avoid blocking.
	 */
	so->curPageLSN = PageGetLSN(page);

	/*
	 * check all tuples on page
	 */
	maxoff = PageGetMaxOffsetNumber(page);
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		ItemId      iid = PageGetItemId(page, i);
		IndexTuple	it;
		bool		match;
		bool		recheck;
		bool		recheck_distances;

		/*
		 * If the scan specifies not to return killed tuples, then we treat a
		 * killed tuple as not passing the qual.
		 */
		if(scan->ignore_killed_tuples && ItemIdIsDead(iid))
			continue;

		it = (IndexTuple) PageGetItem(page, iid);
		/*
		 * Must call gistindex_keytest in tempCxt, and clean up any leftover
		 * junk afterward.
		 */
		oldcxt = MemoryContextSwitchTo(so->giststate->tempCxt);

		match = gistindex_keytest(scan, it, page, i,
								  &recheck, &recheck_distances);

		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(so->giststate->tempCxt);

		/* Ignore tuple if it doesn't match */
		if (!match)
			continue;

		if (tbm && GistPageIsLeaf(page))
		{
			/*
			 * getbitmap scan, so just push heap tuple TIDs into the bitmap
			 * without worrying about ordering
			 */
			tbm_add_tuples(tbm, &it->t_tid, 1, recheck);
			(*ntids)++;
		}
		else if (scan->numberOfOrderBys == 0 && GistPageIsLeaf(page))
		{
			/*
			 * Non-ordered scan, so report tuples in so->pageData[]
			 */
			so->pageData[so->nPageData].heapPtr = it->t_tid;
			so->pageData[so->nPageData].recheck = recheck;
			so->pageData[so->nPageData].offnum = i;

			/*
			 * In an index-only scan, also fetch the data from the tuple.
			 */
			if (scan->xs_want_itup)
			{
				oldcxt = MemoryContextSwitchTo(so->pageDataCxt);
				so->pageData[so->nPageData].ftup =
					gistFetchTuple(giststate, r, it);
				MemoryContextSwitchTo(oldcxt);
			}
			so->nPageData++;
		}
		else
		{
			/*
			 * Must push item into search queue.  We get here for any lower
			 * index page, and also for heap tuples if doing an ordered
			 * search.
			 */
			GISTSearchItem *item;

			oldcxt = MemoryContextSwitchTo(so->queueCxt);

			/* Create new GISTSearchItem for this item */
			item = palloc(SizeOfGISTSearchItem(scan->numberOfOrderBys));

			if (GistPageIsLeaf(page))
			{
				/* Creating heap-tuple GISTSearchItem */
				item->blkno = InvalidBlockNumber;
				item->data.heap.heapPtr = it->t_tid;
				item->data.heap.recheck = recheck;
				item->data.heap.recheckDistances = recheck_distances;

				/*
				 * In an index-only scan, also fetch the data from the tuple.
				 */
				if (scan->xs_want_itup)
					item->data.heap.ftup = gistFetchTuple(giststate, r, it);
			}
			else
			{
				/* Creating index-page GISTSearchItem */
				item->blkno = ItemPointerGetBlockNumber(&it->t_tid);

				/*
				 * LSN of current page is lsn of parent page for child. We
				 * only have a shared lock, so we need to get the LSN
				 * atomically.
				 */
				item->data.parentlsn = BufferGetLSNAtomic(buffer);
			}

			/* Insert it into the queue using new distance data */
			memcpy(item->distances, so->distances,
				   sizeof(double) * scan->numberOfOrderBys);

			pairingheap_add(so->queue, &item->phNode);

			MemoryContextSwitchTo(oldcxt);
		}
	}

	UnlockReleaseBuffer(buffer);
}

/*
 * Extract next item (in order) from search queue
 *
 * Returns a GISTSearchItem or NULL.  Caller must pfree item when done with it.
 */
static GISTSearchItem *
getNextGISTSearchItem(GISTScanOpaque so)
{
	GISTSearchItem *item;

	if (!pairingheap_is_empty(so->queue))
	{
		item = (GISTSearchItem *) pairingheap_remove_first(so->queue);
	}
	else
	{
		/* Done when both heaps are empty */
		item = NULL;
	}

	/* Return item; caller is responsible to pfree it */
	return item;
}

/*
 * Fetch next heap tuple in an ordered search
 */
static bool
getNextNearest(IndexScanDesc scan)
{
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	bool		res = false;
	int			i;

	if (scan->xs_itup)
	{
		/* free previously returned tuple */
		pfree(scan->xs_itup);
		scan->xs_itup = NULL;
	}

	do
	{
		GISTSearchItem *item = getNextGISTSearchItem(so);

		if (!item)
			break;

		if (GISTSearchItemIsHeap(*item))
		{
			/* found a heap item at currently minimal distance */
			scan->xs_ctup.t_self = item->data.heap.heapPtr;
			scan->xs_recheck = item->data.heap.recheck;
			scan->xs_recheckorderby = item->data.heap.recheckDistances;
			for (i = 0; i < scan->numberOfOrderBys; i++)
			{
				if (so->orderByTypes[i] == FLOAT8OID)
				{
#ifndef USE_FLOAT8_BYVAL
					/* must free any old value to avoid memory leakage */
					if (!scan->xs_orderbynulls[i])
						pfree(DatumGetPointer(scan->xs_orderbyvals[i]));
#endif
					scan->xs_orderbyvals[i] = Float8GetDatum(item->distances[i]);
					scan->xs_orderbynulls[i] = false;
				}
				else if (so->orderByTypes[i] == FLOAT4OID)
				{
					/* convert distance function's result to ORDER BY type */
#ifndef USE_FLOAT4_BYVAL
					/* must free any old value to avoid memory leakage */
					if (!scan->xs_orderbynulls[i])
						pfree(DatumGetPointer(scan->xs_orderbyvals[i]));
#endif
					scan->xs_orderbyvals[i] = Float4GetDatum((float4) item->distances[i]);
					scan->xs_orderbynulls[i] = false;
				}
				else
				{
					/*
					 * If the ordering operator's return value is anything
					 * else, we don't know how to convert the float8 bound
					 * calculated by the distance function to that.  The
					 * executor won't actually need the order by values we
					 * return here, if there are no lossy results, so only
					 * insist on converting if the *recheck flag is set.
					 */
					if (scan->xs_recheckorderby)
						elog(ERROR, "GiST operator family's FOR ORDER BY operator must return float8 or float4 if the distance function is lossy");
					scan->xs_orderbynulls[i] = true;
				}
			}

			/* in an index-only scan, also return the reconstructed tuple. */
			if (scan->xs_want_itup)
				scan->xs_itup = item->data.heap.ftup;
			res = true;
		}
		else
		{
			/* visit an index page, extract its items into queue */
			CHECK_FOR_INTERRUPTS();

			gistScanPage(scan, item, item->distances, NULL, NULL);
		}

		pfree(item);
	} while (!res);

	return res;
}

/*
 * gistgettuple() -- Get the next tuple in the scan
 */
Datum
gistgettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanDirection dir = (ScanDirection) PG_GETARG_INT32(1);
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;

	if (dir != ForwardScanDirection)
		elog(ERROR, "GiST only supports forward scan direction");

	if (!so->qual_ok)
		PG_RETURN_BOOL(false);

	if (so->firstCall)
	{
		/* Begin the scan by processing the root page */
		GISTSearchItem fakeItem;

		pgstat_count_index_scan(scan->indexRelation);

		so->firstCall = false;
		so->curPageData = so->nPageData = 0;
		if (so->pageDataCxt)
			MemoryContextReset(so->pageDataCxt);

		fakeItem.blkno = GIST_ROOT_BLKNO;
		memset(&fakeItem.data.parentlsn, 0, sizeof(GistNSN));
		gistScanPage(scan, &fakeItem, NULL, NULL, NULL);
	}

	if (scan->numberOfOrderBys > 0)
	{
		/* Must fetch tuples in strict distance order */
		PG_RETURN_BOOL(getNextNearest(scan));
	}
	else
	{
		/* Fetch tuples index-page-at-a-time */
		for (;;)
		{
			if (so->curPageData < so->nPageData)
			{
				if (scan->kill_prior_tuple && so->curPageData > 0)
				{

					if (so->killedItems == NULL)
					{
						MemoryContext oldCxt =
							MemoryContextSwitchTo(so->giststate->scanCxt);

						so->killedItems =
							(OffsetNumber *) palloc(MaxIndexTuplesPerPage
								* sizeof(OffsetNumber));

						MemoryContextSwitchTo(oldCxt);
					}
					if (so->numKilled < MaxIndexTuplesPerPage)
						so->killedItems[so->numKilled++] =
							so->pageData[so->curPageData - 1].offnum;
				}
				/* continuing to return tuples from a leaf page */
				scan->xs_ctup.t_self = so->pageData[so->curPageData].heapPtr;
				scan->xs_recheck = so->pageData[so->curPageData].recheck;

				/* in an index-only scan, also return the reconstructed tuple */
				if (scan->xs_want_itup)
					scan->xs_itup = so->pageData[so->curPageData].ftup;

				so->curPageData++;

				PG_RETURN_BOOL(true);
			}

			/*
			 * Check the last returned tuple and add it to killitems if
			 * necessary
			 */
			if (scan->kill_prior_tuple
				&& so->curPageData > 0
				&& so->curPageData == so->nPageData)
			{

				if (so->killedItems == NULL)
				{
					MemoryContext oldCxt =
						MemoryContextSwitchTo(so->giststate->scanCxt);

					so->killedItems =
						(OffsetNumber *) palloc(MaxIndexTuplesPerPage
							* sizeof(OffsetNumber));

					MemoryContextSwitchTo(oldCxt);
				}
				if (so->numKilled < MaxIndexTuplesPerPage)
					so->killedItems[so->numKilled++] =
						so->pageData[so->curPageData - 1].offnum;
			}
			/* find and process the next index page */
			do
			{
				GISTSearchItem *item;

				if ((so->curBlkno != InvalidBlockNumber) && (so->numKilled > 0))
					gistkillitems(scan);

				item = getNextGISTSearchItem(so);

				if (!item)
					PG_RETURN_BOOL(false);

				CHECK_FOR_INTERRUPTS();

				/* save current item BlockNumber for next gistkillitems() call */
				so->curBlkno = item->blkno;

				/*
				 * While scanning a leaf page, ItemPointers of matching heap
				 * tuples are stored in so->pageData.  If there are any on
				 * this page, we fall out of the inner "do" and loop around to
				 * return them.
				 */
				gistScanPage(scan, item, item->distances, NULL, NULL);

				pfree(item);
			} while (so->nPageData == 0);
		}
	}
}

/*
 * gistgetbitmap() -- Get a bitmap of all heap tuple locations
 */
Datum
gistgetbitmap(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	TIDBitmap  *tbm = (TIDBitmap *) PG_GETARG_POINTER(1);
	GISTScanOpaque so = (GISTScanOpaque) scan->opaque;
	int64		ntids = 0;
	GISTSearchItem fakeItem;

	if (!so->qual_ok)
		PG_RETURN_INT64(0);

	pgstat_count_index_scan(scan->indexRelation);

	/* Begin the scan by processing the root page */
	so->curPageData = so->nPageData = 0;
	if (so->pageDataCxt)
		MemoryContextReset(so->pageDataCxt);

	fakeItem.blkno = GIST_ROOT_BLKNO;
	memset(&fakeItem.data.parentlsn, 0, sizeof(GistNSN));
	gistScanPage(scan, &fakeItem, NULL, tbm, &ntids);

	/*
	 * While scanning a leaf page, ItemPointers of matching heap tuples will
	 * be stored directly into tbm, so we don't need to deal with them here.
	 */
	for (;;)
	{
		GISTSearchItem *item = getNextGISTSearchItem(so);

		if (!item)
			break;

		CHECK_FOR_INTERRUPTS();

		gistScanPage(scan, item, item->distances, tbm, &ntids);

		pfree(item);
	}

	PG_RETURN_INT64(ntids);
}

/*
 * Can we do index-only scans on the given index column?
 *
 * Opclasses that implement a fetch function support index-only scans.
 */
Datum
gistcanreturn(PG_FUNCTION_ARGS)
{
	Relation	index = (Relation) PG_GETARG_POINTER(0);
	int			attno = PG_GETARG_INT32(1);

	if (OidIsValid(index_getprocid(index, attno, GIST_FETCH_PROC)))
		PG_RETURN_BOOL(true);
	else
		PG_RETURN_BOOL(false);
}
