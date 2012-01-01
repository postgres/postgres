/*-------------------------------------------------------------------------
 *
 * spgscan.c
 *	  routines for scanning SP-GiST indexes
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgscan.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relscan.h"
#include "access/spgist_private.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "utils/datum.h"
#include "utils/memutils.h"


typedef struct ScanStackEntry
{
	Datum		reconstructedValue;		/* value reconstructed from parent */
	int			level;			/* level of items on this page */
	ItemPointerData ptr;		/* block and offset to scan from */
} ScanStackEntry;


/* Free a ScanStackEntry */
static void
freeScanStackEntry(SpGistScanOpaque so, ScanStackEntry *stackEntry)
{
	if (!so->state.attType.attbyval &&
		DatumGetPointer(stackEntry->reconstructedValue) != NULL)
		pfree(DatumGetPointer(stackEntry->reconstructedValue));
	pfree(stackEntry);
}

/* Free the entire stack */
static void
freeScanStack(SpGistScanOpaque so)
{
	ListCell   *lc;

	foreach(lc, so->scanStack)
	{
		freeScanStackEntry(so, (ScanStackEntry *) lfirst(lc));
	}
	list_free(so->scanStack);
	so->scanStack = NIL;
}

/*
 * Initialize scanStack with a single entry for the root page, resetting
 * any previously active scan
 */
static void
resetSpGistScanOpaque(SpGistScanOpaque so)
{
	ScanStackEntry *startEntry = palloc0(sizeof(ScanStackEntry));

	ItemPointerSet(&startEntry->ptr, SPGIST_HEAD_BLKNO, FirstOffsetNumber);

	freeScanStack(so);
	so->scanStack = list_make1(startEntry);

	if (so->want_itup)
	{
		/* Must pfree IndexTuples to avoid memory leak */
		int		i;

		for (i = 0; i < so->nPtrs; i++)
			pfree(so->indexTups[i]);
	}
	so->iPtr = so->nPtrs = 0;
}

Datum
spgbeginscan(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	int			keysz = PG_GETARG_INT32(1);
	/* ScanKey			scankey = (ScanKey) PG_GETARG_POINTER(2); */
	IndexScanDesc scan;
	SpGistScanOpaque so;

	scan = RelationGetIndexScan(rel, keysz, 0);

	so = (SpGistScanOpaque) palloc0(sizeof(SpGistScanOpaqueData));
	initSpGistState(&so->state, scan->indexRelation);
	so->tempCxt = AllocSetContextCreate(CurrentMemoryContext,
										"SP-GiST search temporary context",
										ALLOCSET_DEFAULT_MINSIZE,
										ALLOCSET_DEFAULT_INITSIZE,
										ALLOCSET_DEFAULT_MAXSIZE);
	resetSpGistScanOpaque(so);

	/* Set up indexTupDesc and xs_itupdesc in case it's an index-only scan */
	so->indexTupDesc = scan->xs_itupdesc = RelationGetDescr(rel);

	scan->opaque = so;

	PG_RETURN_POINTER(scan);
}

Datum
spgrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(1);

	if (scankey && scan->numberOfKeys > 0)
	{
		memmove(scan->keyData, scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
	}

	resetSpGistScanOpaque(so);

	PG_RETURN_VOID();
}

Datum
spgendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;

	MemoryContextDelete(so->tempCxt);

	PG_RETURN_VOID();
}

Datum
spgmarkpos(PG_FUNCTION_ARGS)
{
	elog(ERROR, "SPGiST does not support mark/restore");
	PG_RETURN_VOID();
}

Datum
spgrestrpos(PG_FUNCTION_ARGS)
{
	elog(ERROR, "SPGiST does not support mark/restore");
	PG_RETURN_VOID();
}

/*
 * Test whether a leaf datum satisfies all the scan keys
 *
 * *leafValue is set to the reconstructed datum, if provided
 * *recheck is set true if any of the operators are lossy
 */
static bool
spgLeafTest(Relation index, SpGistScanOpaque so, Datum leafDatum,
			int level, Datum reconstructedValue,
			Datum *leafValue, bool *recheck)
{
	bool		result = true;
	spgLeafConsistentIn in;
	spgLeafConsistentOut out;
	FmgrInfo   *procinfo;
	MemoryContext oldCtx;
	int			i;

	*leafValue = (Datum) 0;
	*recheck = false;

	/* set up values that are the same for all quals */
	in.reconstructedValue = reconstructedValue;
	in.level = level;
	in.returnData = so->want_itup;
	in.leafDatum = leafDatum;

	/* Apply each leaf consistency check, working in the temp context */
	oldCtx = MemoryContextSwitchTo(so->tempCxt);

	procinfo = index_getprocinfo(index, 1, SPGIST_LEAF_CONSISTENT_PROC);

	for (i = 0; i < so->numberOfKeys; i++)
	{
		ScanKey		skey = &so->keyData[i];

		/* Assume SPGiST-indexable operators are strict */
		if (skey->sk_flags & SK_ISNULL)
		{
			result = false;
			break;
		}

		in.strategy = skey->sk_strategy;
		in.query = skey->sk_argument;

		out.leafValue = (Datum) 0;
		out.recheck = false;

		result = DatumGetBool(FunctionCall2Coll(procinfo,
												skey->sk_collation,
												PointerGetDatum(&in),
												PointerGetDatum(&out)));
		*leafValue = out.leafValue;
		*recheck |= out.recheck;
		if (!result)
			break;
	}
	MemoryContextSwitchTo(oldCtx);

	return result;
}

/*
 * Walk the tree and report all tuples passing the scan quals to the storeRes
 * subroutine.
 *
 * If scanWholeIndex is true, we'll do just that.  If not, we'll stop at the
 * next page boundary once we have reported at least one tuple.
 */
static void
spgWalk(Relation index, SpGistScanOpaque so, bool scanWholeIndex,
		void (*storeRes) (SpGistScanOpaque, ItemPointer, Datum, bool))
{
	Buffer		buffer = InvalidBuffer;
	bool		reportedSome = false;

	while (scanWholeIndex || !reportedSome)
	{
		ScanStackEntry *stackEntry;
		BlockNumber blkno;
		OffsetNumber offset;
		Page		page;

		/* Pull next to-do item from the list */
		if (so->scanStack == NIL)
			break;				/* there are no more pages to scan */

		stackEntry = (ScanStackEntry *) linitial(so->scanStack);
		so->scanStack = list_delete_first(so->scanStack);

redirect:
		/* Check for interrupts, just in case of infinite loop */
		CHECK_FOR_INTERRUPTS();

		blkno = ItemPointerGetBlockNumber(&stackEntry->ptr);
		offset = ItemPointerGetOffsetNumber(&stackEntry->ptr);

		if (buffer == InvalidBuffer)
		{
			buffer = ReadBuffer(index, blkno);
			LockBuffer(buffer, BUFFER_LOCK_SHARE);
		}
		else if (blkno != BufferGetBlockNumber(buffer))
		{
			UnlockReleaseBuffer(buffer);
			buffer = ReadBuffer(index, blkno);
			LockBuffer(buffer, BUFFER_LOCK_SHARE);
		}
		/* else new pointer points to the same page, no work needed */

		page = BufferGetPage(buffer);

		if (SpGistPageIsLeaf(page))
		{
			SpGistLeafTuple leafTuple;
			OffsetNumber max = PageGetMaxOffsetNumber(page);
			Datum		leafValue = (Datum) 0;
			bool		recheck = false;

			if (blkno == SPGIST_HEAD_BLKNO)
			{
				/* When root is a leaf, examine all its tuples */
				for (offset = FirstOffsetNumber; offset <= max; offset++)
				{
					leafTuple = (SpGistLeafTuple)
						PageGetItem(page, PageGetItemId(page, offset));
					if (leafTuple->tupstate != SPGIST_LIVE)
					{
						/* all tuples on root should be live */
						elog(ERROR, "unexpected SPGiST tuple state: %d",
							 leafTuple->tupstate);
					}

					Assert(ItemPointerIsValid(&leafTuple->heapPtr));
					if (spgLeafTest(index, so,
									SGLTDATUM(leafTuple, &so->state),
									stackEntry->level,
									stackEntry->reconstructedValue,
									&leafValue,
									&recheck))
					{
						storeRes(so, &leafTuple->heapPtr, leafValue, recheck);
						reportedSome = true;
					}
				}
			}
			else
			{
				/* Normal case: just examine the chain we arrived at */
				while (offset != InvalidOffsetNumber)
				{
					Assert(offset >= FirstOffsetNumber && offset <= max);
					leafTuple = (SpGistLeafTuple)
						PageGetItem(page, PageGetItemId(page, offset));
					if (leafTuple->tupstate != SPGIST_LIVE)
					{
						if (leafTuple->tupstate == SPGIST_REDIRECT)
						{
							/* redirection tuple should be first in chain */
							Assert(offset == ItemPointerGetOffsetNumber(&stackEntry->ptr));
							/* transfer attention to redirect point */
							stackEntry->ptr = ((SpGistDeadTuple) leafTuple)->pointer;
							Assert(ItemPointerGetBlockNumber(&stackEntry->ptr) != SPGIST_METAPAGE_BLKNO);
							goto redirect;
						}
						if (leafTuple->tupstate == SPGIST_DEAD)
						{
							/* dead tuple should be first in chain */
							Assert(offset == ItemPointerGetOffsetNumber(&stackEntry->ptr));
							/* No live entries on this page */
							Assert(leafTuple->nextOffset == InvalidOffsetNumber);
							break;
						}
						/* We should not arrive at a placeholder */
						elog(ERROR, "unexpected SPGiST tuple state: %d",
							 leafTuple->tupstate);
					}

					Assert(ItemPointerIsValid(&leafTuple->heapPtr));
					if (spgLeafTest(index, so,
									SGLTDATUM(leafTuple, &so->state),
									stackEntry->level,
									stackEntry->reconstructedValue,
									&leafValue,
									&recheck))
					{
						storeRes(so, &leafTuple->heapPtr, leafValue, recheck);
						reportedSome = true;
					}

					offset = leafTuple->nextOffset;
				}
			}
		}
		else	/* page is inner */
		{
			SpGistInnerTuple innerTuple;
			SpGistNodeTuple node;
			int			i;

			innerTuple = (SpGistInnerTuple) PageGetItem(page,
														PageGetItemId(page, offset));

			if (innerTuple->tupstate != SPGIST_LIVE)
			{
				if (innerTuple->tupstate == SPGIST_REDIRECT)
				{
					/* transfer attention to redirect point */
					stackEntry->ptr = ((SpGistDeadTuple) innerTuple)->pointer;
					Assert(ItemPointerGetBlockNumber(&stackEntry->ptr) != SPGIST_METAPAGE_BLKNO);
					goto redirect;
				}
				elog(ERROR, "unexpected SPGiST tuple state: %d",
					 innerTuple->tupstate);
			}

			if (so->numberOfKeys == 0)
			{
				/*
				 * This case cannot happen at the moment, because we don't
				 * set pg_am.amoptionalkey for SP-GiST.  In order for full
				 * index scans to produce correct answers, we'd need to
				 * index nulls, which we don't.
				 */
				Assert(false);

#ifdef NOT_USED
				/*
				 * A full index scan could be done approximately like this,
				 * but note that reconstruction of indexed values would be
				 * impossible unless the API for inner_consistent is changed.
				 */
				SGITITERATE(innerTuple, i, node)
				{
					if (ItemPointerIsValid(&node->t_tid))
					{
						ScanStackEntry *newEntry = palloc(sizeof(ScanStackEntry));

						newEntry->ptr = node->t_tid;
						newEntry->level = -1;
						newEntry->reconstructedValue = (Datum) 0;
						so->scanStack = lcons(newEntry, so->scanStack);
					}
				}
#endif
			}
			else
			{
				spgInnerConsistentIn in;
				spgInnerConsistentOut out;
				FmgrInfo   *procinfo;
				SpGistNodeTuple *nodes;
				int		   *andMap;
				int		   *levelAdds;
				Datum	   *reconstructedValues;
				int			j,
							nMatches = 0;
				MemoryContext oldCtx;

				/* use temp context for calling inner_consistent */
				oldCtx = MemoryContextSwitchTo(so->tempCxt);

				/* set up values that are the same for all scankeys */
				in.reconstructedValue = stackEntry->reconstructedValue;
				in.level = stackEntry->level;
				in.returnData = so->want_itup;
				in.allTheSame = innerTuple->allTheSame;
				in.hasPrefix = (innerTuple->prefixSize > 0);
				in.prefixDatum = SGITDATUM(innerTuple, &so->state);
				in.nNodes = innerTuple->nNodes;
				in.nodeLabels = spgExtractNodeLabels(&so->state, innerTuple);

				/* collect node pointers */
				nodes = (SpGistNodeTuple *) palloc(sizeof(SpGistNodeTuple) * in.nNodes);
				SGITITERATE(innerTuple, i, node)
				{
					nodes[i] = node;
				}

				andMap = (int *) palloc0(sizeof(int) * in.nNodes);
				levelAdds = (int *) palloc0(sizeof(int) * in.nNodes);
				reconstructedValues = (Datum *) palloc0(sizeof(Datum) * in.nNodes);

				procinfo = index_getprocinfo(index, 1, SPGIST_INNER_CONSISTENT_PROC);

				for (j = 0; j < so->numberOfKeys; j++)
				{
					ScanKey		skey = &so->keyData[j];

					/* Assume SPGiST-indexable operators are strict */
					if (skey->sk_flags & SK_ISNULL)
					{
						nMatches = 0;
						break;
					}

					in.strategy = skey->sk_strategy;
					in.query = skey->sk_argument;

					memset(&out, 0, sizeof(out));

					FunctionCall2Coll(procinfo,
									  skey->sk_collation,
									  PointerGetDatum(&in),
									  PointerGetDatum(&out));

					/* If allTheSame, they should all or none of 'em match */
					if (innerTuple->allTheSame)
						if (out.nNodes != 0 && out.nNodes != in.nNodes)
							elog(ERROR, "inconsistent inner_consistent results for allTheSame inner tuple");

					nMatches = 0;
					for (i = 0; i < out.nNodes; i++)
					{
						int		nodeN = out.nodeNumbers[i];

						andMap[nodeN]++;
						if (andMap[nodeN] == j + 1)
							nMatches++;
						if (out.levelAdds)
							levelAdds[nodeN] = out.levelAdds[i];
						if (out.reconstructedValues)
							reconstructedValues[nodeN] = out.reconstructedValues[i];
					}

					/* quit as soon as all nodes have failed some qual */
					if (nMatches == 0)
						break;
				}

				MemoryContextSwitchTo(oldCtx);

				if (nMatches > 0)
				{
					for (i = 0; i < in.nNodes; i++)
					{
						if (andMap[i] == so->numberOfKeys &&
							ItemPointerIsValid(&nodes[i]->t_tid))
						{
							ScanStackEntry *newEntry;

							/* Create new work item for this node */
							newEntry = palloc(sizeof(ScanStackEntry));
							newEntry->ptr = nodes[i]->t_tid;
							newEntry->level = stackEntry->level + levelAdds[i];
							/* Must copy value out of temp context */
							newEntry->reconstructedValue =
								datumCopy(reconstructedValues[i],
										  so->state.attType.attbyval,
										  so->state.attType.attlen);

							so->scanStack = lcons(newEntry, so->scanStack);
						}
					}
				}
			}
		}

		/* done with this scan stack entry */
		freeScanStackEntry(so, stackEntry);
		/* clear temp context before proceeding to the next one */
		MemoryContextReset(so->tempCxt);
	}

	if (buffer != InvalidBuffer)
		UnlockReleaseBuffer(buffer);
}

/* storeRes subroutine for getbitmap case */
static void
storeBitmap(SpGistScanOpaque so, ItemPointer heapPtr,
			Datum leafValue, bool recheck)
{
	tbm_add_tuples(so->tbm, heapPtr, 1, recheck);
	so->ntids++;
}

Datum
spggetbitmap(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	TIDBitmap  *tbm = (TIDBitmap *) PG_GETARG_POINTER(1);
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;

	/* Copy scankey to *so so we don't need to pass it around separately */
	so->numberOfKeys = scan->numberOfKeys;
	so->keyData = scan->keyData;
	/* Ditto for the want_itup flag */
	so->want_itup = false;

	so->tbm = tbm;
	so->ntids = 0;

	spgWalk(scan->indexRelation, so, true, storeBitmap);

	PG_RETURN_INT64(so->ntids);
}

/* storeRes subroutine for gettuple case */
static void
storeGettuple(SpGistScanOpaque so, ItemPointer heapPtr,
			  Datum leafValue, bool recheck)
{
	Assert(so->nPtrs < MaxIndexTuplesPerPage);
	so->heapPtrs[so->nPtrs] = *heapPtr;
	so->recheck[so->nPtrs] = recheck;
	if (so->want_itup)
	{
		/*
		 * Reconstruct desired IndexTuple.  We have to copy the datum out of
		 * the temp context anyway, so we may as well create the tuple here.
		 */
		bool	isnull = false;

		so->indexTups[so->nPtrs] = index_form_tuple(so->indexTupDesc,
													&leafValue,
													&isnull);
	}
	so->nPtrs++;
}

Datum
spggettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanDirection dir = (ScanDirection) PG_GETARG_INT32(1);
	SpGistScanOpaque so = (SpGistScanOpaque) scan->opaque;

	if (dir != ForwardScanDirection)
		elog(ERROR, "SP-GiST only supports forward scan direction");

	/* Copy scankey to *so so we don't need to pass it around separately */
	so->numberOfKeys = scan->numberOfKeys;
	so->keyData = scan->keyData;
	/* Ditto for the want_itup flag */
	so->want_itup = scan->xs_want_itup;

	for (;;)
	{
		if (so->iPtr < so->nPtrs)
		{
			/* continuing to return tuples from a leaf page */
			scan->xs_ctup.t_self = so->heapPtrs[so->iPtr];
			scan->xs_recheck = so->recheck[so->iPtr];
			scan->xs_itup = so->indexTups[so->iPtr];
			so->iPtr++;
			PG_RETURN_BOOL(true);
		}

		if (so->want_itup)
		{
			/* Must pfree IndexTuples to avoid memory leak */
			int		i;

			for (i = 0; i < so->nPtrs; i++)
				pfree(so->indexTups[i]);
		}
		so->iPtr = so->nPtrs = 0;

		spgWalk(scan->indexRelation, so, false, storeGettuple);

		if (so->nPtrs == 0)
			break;				/* must have completed scan */
	}

	PG_RETURN_BOOL(false);
}

Datum
spgcanreturn(PG_FUNCTION_ARGS)
{
	Relation	index = (Relation) PG_GETARG_POINTER(0);
	SpGistCache *cache;

	/* We can do it if the opclass config function says so */
	cache = spgGetCache(index);

	PG_RETURN_BOOL(cache->config.canReturnData);
}
