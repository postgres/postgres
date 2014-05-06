/*-------------------------------------------------------------------------
 *
 * gistutil.c
 *	  utilities routines for the postgres GiST index access method.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			$PostgreSQL: pgsql/src/backend/access/gist/gistutil.c,v 1.34 2009/06/11 14:48:53 momjian Exp $
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/gist_private.h"
#include "access/reloptions.h"
#include "storage/freespace.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"


/*
 * Write itup vector to page, has no control of free space.
 */
void
gistfillbuffer(Page page, IndexTuple *itup, int len, OffsetNumber off)
{
	OffsetNumber l = InvalidOffsetNumber;
	int			i;

	if (off == InvalidOffsetNumber)
		off = (PageIsEmpty(page)) ? FirstOffsetNumber :
			OffsetNumberNext(PageGetMaxOffsetNumber(page));

	for (i = 0; i < len; i++)
	{
		Size		sz = IndexTupleSize(itup[i]);

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
	itvec = (IndexTuple *) repalloc((void *) itvec, sizeof(IndexTuple) * ((*len) + addlen));
	memmove(&itvec[*len], additvec, sizeof(IndexTuple) * addlen);
	*len += addlen;
	return itvec;
}

/*
 * make plain IndexTupleVector
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
 * Fails and returns FALSE if itvec contains any invalid tuples.
 */
bool
gistMakeUnionItVec(GISTSTATE *giststate, IndexTuple *itvec, int len,
				   Datum *attr, bool *isnull)
{
	int			i;
	GistEntryVector *evec;
	int			attrsize;

	evec = (GistEntryVector *) palloc((len + 2) * sizeof(GISTENTRY) + GEVHDRSZ);

	for (i = 0; i < giststate->tupdesc->natts; i++)
	{
		int			j;

		/* Collect non-null datums for this column */
		evec->n = 0;
		for (j = 0; j < len; j++)
		{
			Datum		datum;
			bool		IsNull;

			if (GistTupleIsInvalid(itvec[j]))
				return FALSE;	/* signals that union with invalid tuple =>
								 * result is invalid */

			datum = index_getattr(itvec[j], i + 1, giststate->tupdesc, &IsNull);
			if (IsNull)
				continue;

			gistdentryinit(giststate, i,
						   evec->vector + evec->n,
						   datum,
						   NULL, NULL, (OffsetNumber) 0,
						   FALSE, IsNull);
			evec->n++;
		}

		/* If this column was all NULLs, the union is NULL */
		if (evec->n == 0)
		{
			attr[i] = (Datum) 0;
			isnull[i] = TRUE;
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
			attr[i] = FunctionCall2(&giststate->unionFn[i],
									PointerGetDatum(evec),
									PointerGetDatum(&attrsize));

			isnull[i] = FALSE;
		}
	}

	return TRUE;
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

	if (!gistMakeUnionItVec(giststate, itvec, len, attr, isnull))
		return gist_form_invalid_tuple(InvalidBlockNumber);

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
		*dstisnull = TRUE;
		*dst = (Datum) 0;
	}
	else
	{
		if (isnull1 == FALSE && isnull2 == FALSE)
		{
			evec->vector[0] = *entry1;
			evec->vector[1] = *entry2;
		}
		else if (isnull1 == FALSE)
		{
			evec->vector[0] = *entry1;
			evec->vector[1] = *entry1;
		}
		else
		{
			evec->vector[0] = *entry2;
			evec->vector[1] = *entry2;
		}

		*dstisnull = FALSE;
		*dst = FunctionCall2(&giststate->unionFn[attno],
							 PointerGetDatum(evec),
							 PointerGetDatum(&dstsize));
	}
}

bool
gistKeyIsEQ(GISTSTATE *giststate, int attno, Datum a, Datum b)
{
	bool		result;

	FunctionCall3(&giststate->equalFn[attno],
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

	for (i = 0; i < r->rd_att->natts; i++)
	{
		Datum		datum = index_getattr(tuple, i + 1, giststate->tupdesc, &isnull[i]);

		gistdentryinit(giststate, i, &attdata[i],
					   datum, r, p, o,
					   FALSE, isnull[i]);
	}
}

/*
 * Forms union of oldtup and addtup, if union == oldtup then return NULL
 */
IndexTuple
gistgetadjusted(Relation r, IndexTuple oldtup, IndexTuple addtup, GISTSTATE *giststate)
{
	bool		neednew = FALSE;
	GISTENTRY	oldentries[INDEX_MAX_KEYS],
				addentries[INDEX_MAX_KEYS];
	bool		oldisnull[INDEX_MAX_KEYS],
				addisnull[INDEX_MAX_KEYS];
	Datum		attr[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	IndexTuple	newtup = NULL;
	int			i;

	if (GistTupleIsInvalid(oldtup) || GistTupleIsInvalid(addtup))
		return gist_form_invalid_tuple(ItemPointerGetBlockNumber(&(oldtup->t_tid)));

	gistDeCompressAtt(giststate, r, oldtup, NULL,
					  (OffsetNumber) 0, oldentries, oldisnull);

	gistDeCompressAtt(giststate, r, addtup, NULL,
					  (OffsetNumber) 0, addentries, addisnull);

	for (i = 0; i < r->rd_att->natts; i++)
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
	 * Loop over tuples on page.
	 */
	maxoff = PageGetMaxOffsetNumber(p);
	Assert(maxoff >= FirstOffsetNumber);

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(p, PageGetItemId(p, i));
		bool		zero_penalty;
		int			j;

		if (!GistPageIsLeaf(p) && GistTupleIsInvalid(itup))
		{
			ereport(LOG,
					(errmsg("index \"%s\" needs VACUUM or REINDEX to finish crash recovery",
							RelationGetRelationName(r))));
			continue;
		}

		zero_penalty = true;

		/* Loop over index attributes. */
		for (j = 0; j < r->rd_att->natts; j++)
		{
			Datum		datum;
			float		usize;
			bool		IsNull;

			/* Compute penalty for this column. */
			datum = index_getattr(itup, j + 1, giststate->tupdesc, &IsNull);
			gistdentryinit(giststate, j, &entry, datum, r, p, i,
						   FALSE, IsNull);
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

				if (j < r->rd_att->natts - 1)
					best_penalty[j + 1] = -1;
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
		 * If we find a tuple with zero penalty for all columns, there's no
		 * need to examine remaining tuples; just break out of the loop and
		 * return it.
		 */
		if (zero_penalty)
			break;
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
		dep = (GISTENTRY *)
			DatumGetPointer(FunctionCall1(&giststate->decompressFn[nkey],
										  PointerGetDatum(e)));
		/* decompressFn may just return the given pointer */
		if (dep != e)
			gistentryinit(*e, dep->key, dep->rel, dep->page, dep->offset,
						  dep->leafkey);
	}
	else
		gistentryinit(*e, (Datum) 0, r, pg, o, l);
}


/*
 * initialize a GiST entry with a compressed version of key
 */
void
gistcentryinit(GISTSTATE *giststate, int nkey,
			   GISTENTRY *e, Datum k, Relation r,
			   Page pg, OffsetNumber o, bool l, bool isNull)
{
	if (!isNull)
	{
		GISTENTRY  *cep;

		gistentryinit(*e, k, r, pg, o, l);
		cep = (GISTENTRY *)
			DatumGetPointer(FunctionCall1(&giststate->compressFn[nkey],
										  PointerGetDatum(e)));
		/* compressFn may just return the given pointer */
		if (cep != e)
			gistentryinit(*e, cep->key, cep->rel, cep->page, cep->offset,
						  cep->leafkey);
	}
	else
		gistentryinit(*e, (Datum) 0, r, pg, o, l);
}

IndexTuple
gistFormTuple(GISTSTATE *giststate, Relation r,
			  Datum attdata[], bool isnull[], bool newValues)
{
	GISTENTRY	centry[INDEX_MAX_KEYS];
	Datum		compatt[INDEX_MAX_KEYS];
	int			i;
	IndexTuple	res;

	for (i = 0; i < r->rd_att->natts; i++)
	{
		if (isnull[i])
			compatt[i] = (Datum) 0;
		else
		{
			gistcentryinit(giststate, i, &centry[i], attdata[i],
						   r, NULL, (OffsetNumber) 0,
						   newValues,
						   FALSE);
			compatt[i] = centry[i].key;
		}
	}

	res = index_form_tuple(giststate->tupdesc, compatt, isnull);
	GistTupleSetValid(res);
	return res;
}

float
gistpenalty(GISTSTATE *giststate, int attno,
			GISTENTRY *orig, bool isNullOrig,
			GISTENTRY *add, bool isNullAdd)
{
	float		penalty = 0.0;

	if (giststate->penaltyFn[attno].fn_strict == FALSE ||
		(isNullOrig == FALSE && isNullAdd == FALSE))
	{
		FunctionCall3(&giststate->penaltyFn[attno],
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
		penalty = 1e10;			/* try to prevent mixing null and non-null
								 * values */

	return penalty;
}

/*
 * Initialize a new index page
 */
void
GISTInitBuffer(Buffer b, uint32 f)
{
	GISTPageOpaque opaque;
	Page		page;
	Size		pageSize;

	pageSize = BufferGetPageSize(b);
	page = BufferGetPage(b);
	PageInit(page, pageSize, sizeof(GISTPageOpaqueData));

	opaque = GistPageGetOpaque(page);
	/* page was already zeroed by PageInit, so this is not needed: */
	/* memset(&(opaque->nsn), 0, sizeof(GistNSN)); */
	opaque->rightlink = InvalidBlockNumber;
	opaque->flags = f;
	opaque->gist_page_id = GIST_PAGE_ID;
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
gistNewBuffer(Relation r)
{
	Buffer		buffer;
	bool		needLock;

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

			if (PageIsNew(page))
				return buffer;	/* OK to use, if never initialized */

			gistcheckpage(r, buffer);

			if (GistPageIsDeleted(page))
				return buffer;	/* OK to use */

			LockBuffer(buffer, GIST_UNLOCK);
		}

		/* Can't use it, so release buffer and try again */
		ReleaseBuffer(buffer);
	}

	/* Must extend the file */
	needLock = !RELATION_IS_LOCAL(r);

	if (needLock)
		LockRelationForExtension(r, ExclusiveLock);

	buffer = ReadBuffer(r, P_NEW);
	LockBuffer(buffer, GIST_EXCLUSIVE);

	if (needLock)
		UnlockRelationForExtension(r, ExclusiveLock);

	return buffer;
}

Datum
gistoptions(PG_FUNCTION_ARGS)
{
	Datum		reloptions = PG_GETARG_DATUM(0);
	bool		validate = PG_GETARG_BOOL(1);
	bytea	   *result;

	result = default_reloptions(reloptions, validate, RELOPT_KIND_GIST);

	if (result)
		PG_RETURN_BYTEA_P(result);
	PG_RETURN_NULL();
}

/*
 * Temporary GiST indexes are not WAL-logged, but we need LSNs to detect
 * concurrent page splits anyway. GetXLogRecPtrForTemp() provides a fake
 * sequence of LSNs for that purpose. Each call generates an LSN that is
 * greater than any previous value returned by this function in the same
 * session.
 */
XLogRecPtr
GetXLogRecPtrForTemp(void)
{
	static XLogRecPtr counter = {0, 1};

	counter.xrecoff++;
	if (counter.xrecoff == 0)
	{
		counter.xlogid++;
		counter.xrecoff++;
	}
	return counter;
}
