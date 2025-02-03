/*-------------------------------------------------------------------------
 *
 * gistutil.c
 *	  utilities routines for the postgres GiST index access method.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gist/gistutil.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/gist_private.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "common/pg_prng.h"
#include "storage/indexfsm.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * Write itup vector to page, has no control of free space.
 */
void
gistfillbuffer(Page page, IndexTuple *itup, int len, OffsetNumber off)
{
	int			i;

	if (off == InvalidOffsetNumber)
		off = (PageIsEmpty(page)) ? FirstOffsetNumber :
			OffsetNumberNext(PageGetMaxOffsetNumber(page));

	for (i = 0; i < len; i++)
	{
		Size		sz = IndexTupleSize(itup[i]);
		OffsetNumber l;

		l = PageAddItem(page, (Item) itup[i], sz, off, false, false);
		if (l == InvalidOffsetNumber)
			elog(ERROR, "failed to add item to GiST index page, item %d out of %d, size %d bytes",
				 i, len, (int) sz);
		off++;
	}
}

/*
 * Check space for itup vector on page
 */
bool
gistnospace(Page page, IndexTuple *itvec, int len, OffsetNumber todelete, Size freespace)
{
	unsigned int size = freespace,
				deleted = 0;
	int			i;

	for (i = 0; i < len; i++)
		size += IndexTupleSize(itvec[i]) + sizeof(ItemIdData);

	if (todelete != InvalidOffsetNumber)
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, todelete));

		deleted = IndexTupleSize(itup) + sizeof(ItemIdData);
	}

	return (PageGetFreeSpace(page) + deleted < size);
}

bool
gistfitpage(IndexTuple *itvec, int len)
{
	int			i;
	Size		size = 0;

	for (i = 0; i < len; i++)
		size += IndexTupleSize(itvec[i]) + sizeof(ItemIdData);

	/* TODO: Consider fillfactor */
	return (size <= GiSTPageSize);
}

/*
 * Read buffer into itup vector
 */
IndexTuple *
gistextractpage(Page page, int *len /* out */ )
{
	OffsetNumber i,
				maxoff;
	IndexTuple *itvec;

	maxoff = PageGetMaxOffsetNumber(page);
	*len = maxoff;
	itvec = palloc(sizeof(IndexTuple) * maxoff);
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		itvec[i - FirstOffsetNumber] = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));

	return itvec;
}

/*
 * join two vectors into one
 */
IndexTuple *
gistjoinvector(IndexTuple *itvec, int *len, IndexTuple *additvec, int addlen)
{
	itvec = (IndexTuple *) repalloc(itvec, sizeof(IndexTuple) * ((*len) + addlen));
	memmove(&itvec[*len], additvec, sizeof(IndexTuple) * addlen);
	*len += addlen;
	return itvec;
}

/*
 * make plain IndexTuple vector
 */

IndexTupleData *
gistfillitupvec(IndexTuple *vec, int veclen, int *memlen)
{
	char	   *ptr,
			   *ret;
	int			i;

	*memlen = 0;

	for (i = 0; i < veclen; i++)
		*memlen += IndexTupleSize(vec[i]);

	ptr = ret = palloc(*memlen);

	for (i = 0; i < veclen; i++)
	{
		memcpy(ptr, vec[i], IndexTupleSize(vec[i]));
		ptr += IndexTupleSize(vec[i]);
	}

	return (IndexTupleData *) ret;
}

/*
 * Make unions of keys in IndexTuple vector (one union datum per index column).
 * Union Datums are returned into the attr/isnull arrays.
 * Resulting Datums aren't compressed.
 */
void
gistMakeUnionItVec(GISTSTATE *giststate, IndexTuple *itvec, int len,
				   Datum *attr, bool *isnull)
{
	int			i;
	GistEntryVector *evec;
	int			attrsize;

	evec = (GistEntryVector *) palloc((len + 2) * sizeof(GISTENTRY) + GEVHDRSZ);

	for (i = 0; i < giststate->nonLeafTupdesc->natts; i++)
	{
		int			j;

		/* Collect non-null datums for this column */
		evec->n = 0;
		for (j = 0; j < len; j++)
		{
			Datum		datum;
			bool		IsNull;

			datum = index_getattr(itvec[j], i + 1, giststate->leafTupdesc,
								  &IsNull);
			if (IsNull)
				continue;

			gistdentryinit(giststate, i,
						   evec->vector + evec->n,
						   datum,
						   NULL, NULL, (OffsetNumber) 0,
						   false, IsNull);
			evec->n++;
		}

		/* If this column was all NULLs, the union is NULL */
		if (evec->n == 0)
		{
			attr[i] = (Datum) 0;
			isnull[i] = true;
		}
		else
		{
			if (evec->n == 1)
			{
				/* unionFn may expect at least two inputs */
				evec->n = 2;
				evec->vector[1] = evec->vector[0];
			}

			/* Make union and store in attr array */
			attr[i] = FunctionCall2Coll(&giststate->unionFn[i],
										giststate->supportCollation[i],
										PointerGetDatum(evec),
										PointerGetDatum(&attrsize));

			isnull[i] = false;
		}
	}
}

/*
 * Return an IndexTuple containing the result of applying the "union"
 * method to the specified IndexTuple vector.
 */
IndexTuple
gistunion(Relation r, IndexTuple *itvec, int len, GISTSTATE *giststate)
{
	Datum		attr[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	gistMakeUnionItVec(giststate, itvec, len, attr, isnull);

	return gistFormTuple(giststate, r, attr, isnull, false);
}

/*
 * makes union of two key
 */
void
gistMakeUnionKey(GISTSTATE *giststate, int attno,
				 GISTENTRY *entry1, bool isnull1,
				 GISTENTRY *entry2, bool isnull2,
				 Datum *dst, bool *dstisnull)
{
	/* we need a GistEntryVector with room for exactly 2 elements */
	union
	{
		GistEntryVector gev;
		char		padding[2 * sizeof(GISTENTRY) + GEVHDRSZ];
	}			storage;
	GistEntryVector *evec = &storage.gev;
	int			dstsize;

	evec->n = 2;

	if (isnull1 && isnull2)
	{
		*dstisnull = true;
		*dst = (Datum) 0;
	}
	else
	{
		if (isnull1 == false && isnull2 == false)
		{
			evec->vector[0] = *entry1;
			evec->vector[1] = *entry2;
		}
		else if (isnull1 == false)
		{
			evec->vector[0] = *entry1;
			evec->vector[1] = *entry1;
		}
		else
		{
			evec->vector[0] = *entry2;
			evec->vector[1] = *entry2;
		}

		*dstisnull = false;
		*dst = FunctionCall2Coll(&giststate->unionFn[attno],
								 giststate->supportCollation[attno],
								 PointerGetDatum(evec),
								 PointerGetDatum(&dstsize));
	}
}

bool
gistKeyIsEQ(GISTSTATE *giststate, int attno, Datum a, Datum b)
{
	bool		result = false; /* silence compiler warning */

	FunctionCall3Coll(&giststate->equalFn[attno],
					  giststate->supportCollation[attno],
					  a, b,
					  PointerGetDatum(&result));
	return result;
}

/*
 * Decompress all keys in tuple
 */
void
gistDeCompressAtt(GISTSTATE *giststate, Relation r, IndexTuple tuple, Page p,
				  OffsetNumber o, GISTENTRY *attdata, bool *isnull)
{
	int			i;

	for (i = 0; i < IndexRelationGetNumberOfKeyAttributes(r); i++)
	{
		Datum		datum;

		datum = index_getattr(tuple, i + 1, giststate->leafTupdesc, &isnull[i]);
		gistdentryinit(giststate, i, &attdata[i],
					   datum, r, p, o,
					   false, isnull[i]);
	}
}

/*
 * Forms union of oldtup and addtup, if union == oldtup then return NULL
 */
IndexTuple
gistgetadjusted(Relation r, IndexTuple oldtup, IndexTuple addtup, GISTSTATE *giststate)
{
	bool		neednew = false;
	GISTENTRY	oldentries[INDEX_MAX_KEYS],
				addentries[INDEX_MAX_KEYS];
	bool		oldisnull[INDEX_MAX_KEYS],
				addisnull[INDEX_MAX_KEYS];
	Datum		attr[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	IndexTuple	newtup = NULL;
	int			i;

	gistDeCompressAtt(giststate, r, oldtup, NULL,
					  (OffsetNumber) 0, oldentries, oldisnull);

	gistDeCompressAtt(giststate, r, addtup, NULL,
					  (OffsetNumber) 0, addentries, addisnull);

	for (i = 0; i < IndexRelationGetNumberOfKeyAttributes(r); i++)
	{
		gistMakeUnionKey(giststate, i,
						 oldentries + i, oldisnull[i],
						 addentries + i, addisnull[i],
						 attr + i, isnull + i);

		if (neednew)
			/* we already need new key, so we can skip check */
			continue;

		if (isnull[i])
			/* union of key may be NULL if and only if both keys are NULL */
			continue;

		if (!addisnull[i])
		{
			if (oldisnull[i] ||
				!gistKeyIsEQ(giststate, i, oldentries[i].key, attr[i]))
				neednew = true;
		}
	}

	if (neednew)
	{
		/* need to update key */
		newtup = gistFormTuple(giststate, r, attr, isnull, false);
		newtup->t_tid = oldtup->t_tid;
	}

	return newtup;
}

/*
 * Search an upper index page for the entry with lowest penalty for insertion
 * of the new index key contained in "it".
 *
 * Returns the index of the page entry to insert into.
 */
OffsetNumber
gistchoose(Relation r, Page p, IndexTuple it,	/* it has compressed entry */
		   GISTSTATE *giststate)
{
	OffsetNumber result;
	OffsetNumber maxoff;
	OffsetNumber i;
	float		best_penalty[INDEX_MAX_KEYS];
	GISTENTRY	entry,
				identry[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	int			keep_current_best;

	Assert(!GistPageIsLeaf(p));

	gistDeCompressAtt(giststate, r,
					  it, NULL, (OffsetNumber) 0,
					  identry, isnull);

	/* we'll return FirstOffsetNumber if page is empty (shouldn't happen) */
	result = FirstOffsetNumber;

	/*
	 * The index may have multiple columns, and there's a penalty value for
	 * each column.  The penalty associated with a column that appears earlier
	 * in the index definition is strictly more important than the penalty of
	 * a column that appears later in the index definition.
	 *
	 * best_penalty[j] is the best penalty we have seen so far for column j,
	 * or -1 when we haven't yet examined column j.  Array entries to the
	 * right of the first -1 are undefined.
	 */
	best_penalty[0] = -1;

	/*
	 * If we find a tuple that's exactly as good as the currently best one, we
	 * could use either one.  When inserting a lot of tuples with the same or
	 * similar keys, it's preferable to descend down the same path when
	 * possible, as that's more cache-friendly.  On the other hand, if all
	 * inserts land on the same leaf page after a split, we're never going to
	 * insert anything to the other half of the split, and will end up using
	 * only 50% of the available space.  Distributing the inserts evenly would
	 * lead to better space usage, but that hurts cache-locality during
	 * insertion.  To get the best of both worlds, when we find a tuple that's
	 * exactly as good as the previous best, choose randomly whether to stick
	 * to the old best, or use the new one.  Once we decide to stick to the
	 * old best, we keep sticking to it for any subsequent equally good tuples
	 * we might find.  This favors tuples with low offsets, but still allows
	 * some inserts to go to other equally-good subtrees.
	 *
	 * keep_current_best is -1 if we haven't yet had to make a random choice
	 * whether to keep the current best tuple.  If we have done so, and
	 * decided to keep it, keep_current_best is 1; if we've decided to
	 * replace, keep_current_best is 0.  (This state will be reset to -1 as
	 * soon as we've made the replacement, but sometimes we make the choice in
	 * advance of actually finding a replacement best tuple.)
	 */
	keep_current_best = -1;

	/*
	 * Loop over tuples on page.
	 */
	maxoff = PageGetMaxOffsetNumber(p);
	Assert(maxoff >= FirstOffsetNumber);

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(p, PageGetItemId(p, i));
		bool		zero_penalty;
		int			j;

		zero_penalty = true;

		/* Loop over index attributes. */
		for (j = 0; j < IndexRelationGetNumberOfKeyAttributes(r); j++)
		{
			Datum		datum;
			float		usize;
			bool		IsNull;

			/* Compute penalty for this column. */
			datum = index_getattr(itup, j + 1, giststate->leafTupdesc,
								  &IsNull);
			gistdentryinit(giststate, j, &entry, datum, r, p, i,
						   false, IsNull);
			usize = gistpenalty(giststate, j, &entry, IsNull,
								&identry[j], isnull[j]);
			if (usize > 0)
				zero_penalty = false;

			if (best_penalty[j] < 0 || usize < best_penalty[j])
			{
				/*
				 * New best penalty for column.  Tentatively select this tuple
				 * as the target, and record the best penalty.  Then reset the
				 * next column's penalty to "unknown" (and indirectly, the
				 * same for all the ones to its right).  This will force us to
				 * adopt this tuple's penalty values as the best for all the
				 * remaining columns during subsequent loop iterations.
				 */
				result = i;
				best_penalty[j] = usize;

				if (j < IndexRelationGetNumberOfKeyAttributes(r) - 1)
					best_penalty[j + 1] = -1;

				/* we have new best, so reset keep-it decision */
				keep_current_best = -1;
			}
			else if (best_penalty[j] == usize)
			{
				/*
				 * The current tuple is exactly as good for this column as the
				 * best tuple seen so far.  The next iteration of this loop
				 * will compare the next column.
				 */
			}
			else
			{
				/*
				 * The current tuple is worse for this column than the best
				 * tuple seen so far.  Skip the remaining columns and move on
				 * to the next tuple, if any.
				 */
				zero_penalty = false;	/* so outer loop won't exit */
				break;
			}
		}

		/*
		 * If we looped past the last column, and did not update "result",
		 * then this tuple is exactly as good as the prior best tuple.
		 */
		if (j == IndexRelationGetNumberOfKeyAttributes(r) && result != i)
		{
			if (keep_current_best == -1)
			{
				/* we didn't make the random choice yet for this old best */
				keep_current_best = pg_prng_bool(&pg_global_prng_state) ? 1 : 0;
			}
			if (keep_current_best == 0)
			{
				/* we choose to use the new tuple */
				result = i;
				/* choose again if there are even more exactly-as-good ones */
				keep_current_best = -1;
			}
		}

		/*
		 * If we find a tuple with zero penalty for all columns, and we've
		 * decided we don't want to search for another tuple with equal
		 * penalty, there's no need to examine remaining tuples; just break
		 * out of the loop and return it.
		 */
		if (zero_penalty)
		{
			if (keep_current_best == -1)
			{
				/* we didn't make the random choice yet for this old best */
				keep_current_best = pg_prng_bool(&pg_global_prng_state) ? 1 : 0;
			}
			if (keep_current_best == 1)
				break;
		}
	}

	return result;
}

/*
 * initialize a GiST entry with a decompressed version of key
 */
void
gistdentryinit(GISTSTATE *giststate, int nkey, GISTENTRY *e,
			   Datum k, Relation r, Page pg, OffsetNumber o,
			   bool l, bool isNull)
{
	if (!isNull)
	{
		GISTENTRY  *dep;

		gistentryinit(*e, k, r, pg, o, l);

		/* there may not be a decompress function in opclass */
		if (!OidIsValid(giststate->decompressFn[nkey].fn_oid))
			return;

		dep = (GISTENTRY *)
			DatumGetPointer(FunctionCall1Coll(&giststate->decompressFn[nkey],
											  giststate->supportCollation[nkey],
											  PointerGetDatum(e)));
		/* decompressFn may just return the given pointer */
		if (dep != e)
			gistentryinit(*e, dep->key, dep->rel, dep->page, dep->offset,
						  dep->leafkey);
	}
	else
		gistentryinit(*e, (Datum) 0, r, pg, o, l);
}

IndexTuple
gistFormTuple(GISTSTATE *giststate, Relation r,
			  const Datum *attdata, const bool *isnull, bool isleaf)
{
	Datum		compatt[INDEX_MAX_KEYS];
	IndexTuple	res;

	gistCompressValues(giststate, r, attdata, isnull, isleaf, compatt);

	res = index_form_tuple(isleaf ? giststate->leafTupdesc :
						   giststate->nonLeafTupdesc,
						   compatt, isnull);

	/*
	 * The offset number on tuples on internal pages is unused. For historical
	 * reasons, it is set to 0xffff.
	 */
	ItemPointerSetOffsetNumber(&(res->t_tid), 0xffff);
	return res;
}

void
gistCompressValues(GISTSTATE *giststate, Relation r,
				   const Datum *attdata, const bool *isnull, bool isleaf, Datum *compatt)
{
	int			i;

	/*
	 * Call the compress method on each attribute.
	 */
	for (i = 0; i < IndexRelationGetNumberOfKeyAttributes(r); i++)
	{
		if (isnull[i])
			compatt[i] = (Datum) 0;
		else
		{
			GISTENTRY	centry;
			GISTENTRY  *cep;

			gistentryinit(centry, attdata[i], r, NULL, (OffsetNumber) 0,
						  isleaf);
			/* there may not be a compress function in opclass */
			if (OidIsValid(giststate->compressFn[i].fn_oid))
				cep = (GISTENTRY *)
					DatumGetPointer(FunctionCall1Coll(&giststate->compressFn[i],
													  giststate->supportCollation[i],
													  PointerGetDatum(&centry)));
			else
				cep = &centry;
			compatt[i] = cep->key;
		}
	}

	if (isleaf)
	{
		/*
		 * Emplace each included attribute if any.
		 */
		for (; i < r->rd_att->natts; i++)
		{
			if (isnull[i])
				compatt[i] = (Datum) 0;
			else
				compatt[i] = attdata[i];
		}
	}
}

/*
 * initialize a GiST entry with fetched value in key field
 */
static Datum
gistFetchAtt(GISTSTATE *giststate, int nkey, Datum k, Relation r)
{
	GISTENTRY	fentry;
	GISTENTRY  *fep;

	gistentryinit(fentry, k, r, NULL, (OffsetNumber) 0, false);

	fep = (GISTENTRY *)
		DatumGetPointer(FunctionCall1Coll(&giststate->fetchFn[nkey],
										  giststate->supportCollation[nkey],
										  PointerGetDatum(&fentry)));

	/* fetchFn set 'key', return it to the caller */
	return fep->key;
}

/*
 * Fetch all keys in tuple.
 * Returns a new HeapTuple containing the originally-indexed data.
 */
HeapTuple
gistFetchTuple(GISTSTATE *giststate, Relation r, IndexTuple tuple)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(giststate->tempCxt);
	Datum		fetchatt[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	int			i;

	for (i = 0; i < IndexRelationGetNumberOfKeyAttributes(r); i++)
	{
		Datum		datum;

		datum = index_getattr(tuple, i + 1, giststate->leafTupdesc, &isnull[i]);

		if (giststate->fetchFn[i].fn_oid != InvalidOid)
		{
			if (!isnull[i])
				fetchatt[i] = gistFetchAtt(giststate, i, datum, r);
			else
				fetchatt[i] = (Datum) 0;
		}
		else if (giststate->compressFn[i].fn_oid == InvalidOid)
		{
			/*
			 * If opclass does not provide compress method that could change
			 * original value, att is necessarily stored in original form.
			 */
			if (!isnull[i])
				fetchatt[i] = datum;
			else
				fetchatt[i] = (Datum) 0;
		}
		else
		{
			/*
			 * Index-only scans not supported for this column. Since the
			 * planner chose an index-only scan anyway, it is not interested
			 * in this column, and we can replace it with a NULL.
			 */
			isnull[i] = true;
			fetchatt[i] = (Datum) 0;
		}
	}

	/*
	 * Get each included attribute.
	 */
	for (; i < r->rd_att->natts; i++)
	{
		fetchatt[i] = index_getattr(tuple, i + 1, giststate->leafTupdesc,
									&isnull[i]);
	}
	MemoryContextSwitchTo(oldcxt);

	return heap_form_tuple(giststate->fetchTupdesc, fetchatt, isnull);
}

float
gistpenalty(GISTSTATE *giststate, int attno,
			GISTENTRY *orig, bool isNullOrig,
			GISTENTRY *add, bool isNullAdd)
{
	float		penalty = 0.0;

	if (giststate->penaltyFn[attno].fn_strict == false ||
		(isNullOrig == false && isNullAdd == false))
	{
		FunctionCall3Coll(&giststate->penaltyFn[attno],
						  giststate->supportCollation[attno],
						  PointerGetDatum(orig),
						  PointerGetDatum(add),
						  PointerGetDatum(&penalty));
		/* disallow negative or NaN penalty */
		if (isnan(penalty) || penalty < 0.0)
			penalty = 0.0;
	}
	else if (isNullOrig && isNullAdd)
		penalty = 0.0;
	else
	{
		/* try to prevent mixing null and non-null values */
		penalty = get_float4_infinity();
	}

	return penalty;
}

/*
 * Initialize a new index page
 */
void
gistinitpage(Page page, uint32 f)
{
	GISTPageOpaque opaque;

	PageInit(page, BLCKSZ, sizeof(GISTPageOpaqueData));

	opaque = GistPageGetOpaque(page);
	opaque->rightlink = InvalidBlockNumber;
	opaque->flags = f;
	opaque->gist_page_id = GIST_PAGE_ID;
}

/*
 * Initialize a new index buffer
 */
void
GISTInitBuffer(Buffer b, uint32 f)
{
	Page		page;

	page = BufferGetPage(b);
	gistinitpage(page, f);
}

/*
 * Verify that a freshly-read page looks sane.
 */
void
gistcheckpage(Relation rel, Buffer buf)
{
	Page		page = BufferGetPage(buf);

	/*
	 * ReadBuffer verifies that every newly-read page passes
	 * PageHeaderIsValid, which means it either contains a reasonably sane
	 * page header or is all-zero.  We have to defend against the all-zero
	 * case, however.
	 */
	if (PageIsNew(page))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains unexpected zero page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));

	/*
	 * Additionally check that the special area looks sane.
	 */
	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(GISTPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains corrupted page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));
}


/*
 * Allocate a new page (either by recycling, or by extending the index file)
 *
 * The returned buffer is already pinned and exclusive-locked
 *
 * Caller is responsible for initializing the page by calling GISTInitBuffer
 */
Buffer
gistNewBuffer(Relation r, Relation heaprel)
{
	Buffer		buffer;

	/* First, try to get a page from FSM */
	for (;;)
	{
		BlockNumber blkno = GetFreeIndexPage(r);

		if (blkno == InvalidBlockNumber)
			break;				/* nothing left in FSM */

		buffer = ReadBuffer(r, blkno);

		/*
		 * We have to guard against the possibility that someone else already
		 * recycled this page; the buffer may be locked if so.
		 */
		if (ConditionalLockBuffer(buffer))
		{
			Page		page = BufferGetPage(buffer);

			/*
			 * If the page was never initialized, it's OK to use.
			 */
			if (PageIsNew(page))
				return buffer;

			gistcheckpage(r, buffer);

			/*
			 * Otherwise, recycle it if deleted, and too old to have any
			 * processes interested in it.
			 */
			if (gistPageRecyclable(page))
			{
				/*
				 * If we are generating WAL for Hot Standby then create a WAL
				 * record that will allow us to conflict with queries running
				 * on standby, in case they have snapshots older than the
				 * page's deleteXid.
				 */
				if (XLogStandbyInfoActive() && RelationNeedsWAL(r))
					gistXLogPageReuse(r, heaprel, blkno, GistPageGetDeleteXid(page));

				return buffer;
			}

			LockBuffer(buffer, GIST_UNLOCK);
		}

		/* Can't use it, so release buffer and try again */
		ReleaseBuffer(buffer);
	}

	/* Must extend the file */
	buffer = ExtendBufferedRel(BMR_REL(r), MAIN_FORKNUM, NULL,
							   EB_LOCK_FIRST);

	return buffer;
}

/* Can this page be recycled yet? */
bool
gistPageRecyclable(Page page)
{
	if (PageIsNew(page))
		return true;
	if (GistPageIsDeleted(page))
	{
		/*
		 * The page was deleted, but when? If it was just deleted, a scan
		 * might have seen the downlink to it, and will read the page later.
		 * As long as that can happen, we must keep the deleted page around as
		 * a tombstone.
		 *
		 * For that check if the deletion XID could still be visible to
		 * anyone. If not, then no scan that's still in progress could have
		 * seen its downlink, and we can recycle it.
		 */
		FullTransactionId deletexid_full = GistPageGetDeleteXid(page);

		return GlobalVisCheckRemovableFullXid(NULL, deletexid_full);
	}
	return false;
}

bytea *
gistoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"fillfactor", RELOPT_TYPE_INT, offsetof(GiSTOptions, fillfactor)},
		{"buffering", RELOPT_TYPE_ENUM, offsetof(GiSTOptions, buffering_mode)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_GIST,
									  sizeof(GiSTOptions),
									  tab, lengthof(tab));
}

/*
 *	gistproperty() -- Check boolean properties of indexes.
 *
 * This is optional for most AMs, but is required for GiST because the core
 * property code doesn't support AMPROP_DISTANCE_ORDERABLE.  We also handle
 * AMPROP_RETURNABLE here to save opening the rel to call gistcanreturn.
 */
bool
gistproperty(Oid index_oid, int attno,
			 IndexAMProperty prop, const char *propname,
			 bool *res, bool *isnull)
{
	Oid			opclass,
				opfamily,
				opcintype;
	int16		procno;

	/* Only answer column-level inquiries */
	if (attno == 0)
		return false;

	/*
	 * Currently, GiST distance-ordered scans require that there be a distance
	 * function in the opclass with the default types (i.e. the one loaded
	 * into the relcache entry, see initGISTstate).  So we assume that if such
	 * a function exists, then there's a reason for it (rather than grubbing
	 * through all the opfamily's operators to find an ordered one).
	 *
	 * Essentially the same code can test whether we support returning the
	 * column data, since that's true if the opclass provides a fetch proc.
	 */

	switch (prop)
	{
		case AMPROP_DISTANCE_ORDERABLE:
			procno = GIST_DISTANCE_PROC;
			break;
		case AMPROP_RETURNABLE:
			procno = GIST_FETCH_PROC;
			break;
		default:
			return false;
	}

	/* First we need to know the column's opclass. */
	opclass = get_index_column_opclass(index_oid, attno);
	if (!OidIsValid(opclass))
	{
		*isnull = true;
		return true;
	}

	/* Now look up the opclass family and input datatype. */
	if (!get_opclass_opfamily_and_input_type(opclass, &opfamily, &opcintype))
	{
		*isnull = true;
		return true;
	}

	/* And now we can check whether the function is provided. */

	*res = SearchSysCacheExists4(AMPROCNUM,
								 ObjectIdGetDatum(opfamily),
								 ObjectIdGetDatum(opcintype),
								 ObjectIdGetDatum(opcintype),
								 Int16GetDatum(procno));

	/*
	 * Special case: even without a fetch function, AMPROP_RETURNABLE is true
	 * if the opclass has no compress function.
	 */
	if (prop == AMPROP_RETURNABLE && !*res)
	{
		*res = !SearchSysCacheExists4(AMPROCNUM,
									  ObjectIdGetDatum(opfamily),
									  ObjectIdGetDatum(opcintype),
									  ObjectIdGetDatum(opcintype),
									  Int16GetDatum(GIST_COMPRESS_PROC));
	}

	*isnull = false;

	return true;
}

/*
 * Some indexes are not WAL-logged, but we need LSNs to detect concurrent page
 * splits anyway. This function provides a fake sequence of LSNs for that
 * purpose.
 */
XLogRecPtr
gistGetFakeLSN(Relation rel)
{
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
	{
		/*
		 * Temporary relations are only accessible in our session, so a simple
		 * backend-local counter will do.
		 */
		static XLogRecPtr counter = FirstNormalUnloggedLSN;

		return counter++;
	}
	else if (RelationIsPermanent(rel))
	{
		/*
		 * WAL-logging on this relation will start after commit, so its LSNs
		 * must be distinct numbers smaller than the LSN at the next commit.
		 * Emit a dummy WAL record if insert-LSN hasn't advanced after the
		 * last call.
		 */
		static XLogRecPtr lastlsn = InvalidXLogRecPtr;
		XLogRecPtr	currlsn = GetXLogInsertRecPtr();

		/* Shouldn't be called for WAL-logging relations */
		Assert(!RelationNeedsWAL(rel));

		/* No need for an actual record if we already have a distinct LSN */
		if (!XLogRecPtrIsInvalid(lastlsn) && lastlsn == currlsn)
			currlsn = gistXLogAssignLSN();

		lastlsn = currlsn;
		return currlsn;
	}
	else
	{
		/*
		 * Unlogged relations are accessible from other backends, and survive
		 * (clean) restarts. GetFakeLSNForUnloggedRel() handles that for us.
		 */
		Assert(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED);
		return GetFakeLSNForUnloggedRel();
	}
}

/*
 * This is a stratnum support function for GiST opclasses that use the
 * RT*StrategyNumber constants.
 */
Datum
gist_stratnum_common(PG_FUNCTION_ARGS)
{
	CompareType cmptype = PG_GETARG_INT32(0);

	switch (cmptype)
	{
		case COMPARE_EQ:
			PG_RETURN_UINT16(RTEqualStrategyNumber);
		case COMPARE_LT:
			PG_RETURN_UINT16(RTLessStrategyNumber);
		case COMPARE_LE:
			PG_RETURN_UINT16(RTLessEqualStrategyNumber);
		case COMPARE_GT:
			PG_RETURN_UINT16(RTGreaterStrategyNumber);
		case COMPARE_GE:
			PG_RETURN_UINT16(RTGreaterEqualStrategyNumber);
		case COMPARE_OVERLAP:
			PG_RETURN_UINT16(RTOverlapStrategyNumber);
		case COMPARE_CONTAINED_BY:
			PG_RETURN_UINT16(RTContainedByStrategyNumber);
		default:
			PG_RETURN_UINT16(InvalidStrategy);
	}
}

/*
 * Returns the opclass's private stratnum used for the given compare type.
 *
 * Calls the opclass's GIST_STRATNUM_PROC support function, if any,
 * and returns the result.
 * Returns InvalidStrategy if the function is not defined.
 */
StrategyNumber
gisttranslatecmptype(CompareType cmptype, Oid opfamily, Oid opcintype)
{
	Oid			funcid;
	Datum		result;

	/* Check whether the function is provided. */
	funcid = get_opfamily_proc(opfamily, opcintype, opcintype, GIST_STRATNUM_PROC);
	if (!OidIsValid(funcid))
		return InvalidStrategy;

	/* Ask the translation function */
	result = OidFunctionCall1Coll(funcid, InvalidOid, Int32GetDatum(cmptype));
	return DatumGetUInt16(result);
}
