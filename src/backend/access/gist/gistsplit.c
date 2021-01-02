/*-------------------------------------------------------------------------
 *
 * gistsplit.c
 *	  Multi-column page splitting algorithm
 *
 * This file is concerned with making good page-split decisions in multi-column
 * GiST indexes.  The opclass-specific picksplit functions can only be expected
 * to produce answers based on a single column.  We first run the picksplit
 * function for column 1; then, if there are more columns, we check if any of
 * the tuples are "don't cares" so far as the column 1 split is concerned
 * (that is, they could go to either side for no additional penalty).  If so,
 * we try to redistribute those tuples on the basis of the next column.
 * Repeat till we're out of columns.
 *
 * gistSplitByKey() is the entry point to this file.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistsplit.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist_private.h"
#include "utils/rel.h"

typedef struct
{
	OffsetNumber *entries;
	int			len;
	Datum	   *attr;
	bool	   *isnull;
	bool	   *dontcare;
} GistSplitUnion;


/*
 * Form unions of subkeys in itvec[] entries listed in gsvp->entries[],
 * ignoring any tuples that are marked in gsvp->dontcare[].  Subroutine for
 * gistunionsubkey.
 */
static void
gistunionsubkeyvec(GISTSTATE *giststate, IndexTuple *itvec,
				   GistSplitUnion *gsvp)
{
	IndexTuple *cleanedItVec;
	int			i,
				cleanedLen = 0;

	cleanedItVec = (IndexTuple *) palloc(sizeof(IndexTuple) * gsvp->len);

	for (i = 0; i < gsvp->len; i++)
	{
		if (gsvp->dontcare && gsvp->dontcare[gsvp->entries[i]])
			continue;

		cleanedItVec[cleanedLen++] = itvec[gsvp->entries[i] - 1];
	}

	gistMakeUnionItVec(giststate, cleanedItVec, cleanedLen,
					   gsvp->attr, gsvp->isnull);

	pfree(cleanedItVec);
}

/*
 * Recompute unions of left- and right-side subkeys after a page split,
 * ignoring any tuples that are marked in spl->spl_dontcare[].
 *
 * Note: we always recompute union keys for all index columns.  In some cases
 * this might represent duplicate work for the leftmost column(s), but it's
 * not safe to assume that "zero penalty to move a tuple" means "the union
 * key doesn't change at all".  Penalty functions aren't 100% accurate.
 */
static void
gistunionsubkey(GISTSTATE *giststate, IndexTuple *itvec, GistSplitVector *spl)
{
	GistSplitUnion gsvp;

	gsvp.dontcare = spl->spl_dontcare;

	gsvp.entries = spl->splitVector.spl_left;
	gsvp.len = spl->splitVector.spl_nleft;
	gsvp.attr = spl->spl_lattr;
	gsvp.isnull = spl->spl_lisnull;

	gistunionsubkeyvec(giststate, itvec, &gsvp);

	gsvp.entries = spl->splitVector.spl_right;
	gsvp.len = spl->splitVector.spl_nright;
	gsvp.attr = spl->spl_rattr;
	gsvp.isnull = spl->spl_risnull;

	gistunionsubkeyvec(giststate, itvec, &gsvp);
}

/*
 * Find tuples that are "don't cares", that is could be moved to the other
 * side of the split with zero penalty, so far as the attno column is
 * concerned.
 *
 * Don't-care tuples are marked by setting the corresponding entry in
 * spl->spl_dontcare[] to "true".  Caller must have initialized that array
 * to zeroes.
 *
 * Returns number of don't-cares found.
 */
static int
findDontCares(Relation r, GISTSTATE *giststate, GISTENTRY *valvec,
			  GistSplitVector *spl, int attno)
{
	int			i;
	GISTENTRY	entry;
	int			NumDontCare = 0;

	/*
	 * First, search the left-side tuples to see if any have zero penalty to
	 * be added to the right-side union key.
	 *
	 * attno column is known all-not-null (see gistSplitByKey), so we need not
	 * check for nulls
	 */
	gistentryinit(entry, spl->splitVector.spl_rdatum, r, NULL,
				  (OffsetNumber) 0, false);
	for (i = 0; i < spl->splitVector.spl_nleft; i++)
	{
		int			j = spl->splitVector.spl_left[i];
		float		penalty = gistpenalty(giststate, attno, &entry, false,
										  &valvec[j], false);

		if (penalty == 0.0)
		{
			spl->spl_dontcare[j] = true;
			NumDontCare++;
		}
	}

	/* And conversely for the right-side tuples */
	gistentryinit(entry, spl->splitVector.spl_ldatum, r, NULL,
				  (OffsetNumber) 0, false);
	for (i = 0; i < spl->splitVector.spl_nright; i++)
	{
		int			j = spl->splitVector.spl_right[i];
		float		penalty = gistpenalty(giststate, attno, &entry, false,
										  &valvec[j], false);

		if (penalty == 0.0)
		{
			spl->spl_dontcare[j] = true;
			NumDontCare++;
		}
	}

	return NumDontCare;
}

/*
 * Remove tuples that are marked don't-cares from the tuple index array a[]
 * of length *len.  This is applied separately to the spl_left and spl_right
 * arrays.
 */
static void
removeDontCares(OffsetNumber *a, int *len, const bool *dontcare)
{
	int			origlen,
				newlen,
				i;
	OffsetNumber *curwpos;

	origlen = newlen = *len;
	curwpos = a;
	for (i = 0; i < origlen; i++)
	{
		OffsetNumber ai = a[i];

		if (dontcare[ai] == false)
		{
			/* re-emit item into a[] */
			*curwpos = ai;
			curwpos++;
		}
		else
			newlen--;
	}

	*len = newlen;
}

/*
 * Place a single don't-care tuple into either the left or right side of the
 * split, according to which has least penalty for merging the tuple into
 * the previously-computed union keys.  We need consider only columns starting
 * at attno.
 */
static void
placeOne(Relation r, GISTSTATE *giststate, GistSplitVector *v,
		 IndexTuple itup, OffsetNumber off, int attno)
{
	GISTENTRY	identry[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	bool		toLeft = true;

	gistDeCompressAtt(giststate, r, itup, NULL, (OffsetNumber) 0,
					  identry, isnull);

	for (; attno < giststate->nonLeafTupdesc->natts; attno++)
	{
		float		lpenalty,
					rpenalty;
		GISTENTRY	entry;

		gistentryinit(entry, v->spl_lattr[attno], r, NULL, 0, false);
		lpenalty = gistpenalty(giststate, attno, &entry, v->spl_lisnull[attno],
							   identry + attno, isnull[attno]);
		gistentryinit(entry, v->spl_rattr[attno], r, NULL, 0, false);
		rpenalty = gistpenalty(giststate, attno, &entry, v->spl_risnull[attno],
							   identry + attno, isnull[attno]);

		if (lpenalty != rpenalty)
		{
			if (lpenalty > rpenalty)
				toLeft = false;
			break;
		}
	}

	if (toLeft)
		v->splitVector.spl_left[v->splitVector.spl_nleft++] = off;
	else
		v->splitVector.spl_right[v->splitVector.spl_nright++] = off;
}

#define SWAPVAR( s, d, t ) \
do {	\
	(t) = (s); \
	(s) = (d); \
	(d) = (t); \
} while(0)

/*
 * Clean up when we did a secondary split but the user-defined PickSplit
 * method didn't support it (leaving spl_ldatum_exists or spl_rdatum_exists
 * true).
 *
 * We consider whether to swap the left and right outputs of the secondary
 * split; this can be worthwhile if the penalty for merging those tuples into
 * the previously chosen sets is less that way.
 *
 * In any case we must update the union datums for the current column by
 * adding in the previous union keys (oldL/oldR), since the user-defined
 * PickSplit method didn't do so.
 */
static void
supportSecondarySplit(Relation r, GISTSTATE *giststate, int attno,
					  GIST_SPLITVEC *sv, Datum oldL, Datum oldR)
{
	bool		leaveOnLeft = true,
				tmpBool;
	GISTENTRY	entryL,
				entryR,
				entrySL,
				entrySR;

	gistentryinit(entryL, oldL, r, NULL, 0, false);
	gistentryinit(entryR, oldR, r, NULL, 0, false);
	gistentryinit(entrySL, sv->spl_ldatum, r, NULL, 0, false);
	gistentryinit(entrySR, sv->spl_rdatum, r, NULL, 0, false);

	if (sv->spl_ldatum_exists && sv->spl_rdatum_exists)
	{
		float		penalty1,
					penalty2;

		penalty1 = gistpenalty(giststate, attno, &entryL, false, &entrySL, false) +
			gistpenalty(giststate, attno, &entryR, false, &entrySR, false);
		penalty2 = gistpenalty(giststate, attno, &entryL, false, &entrySR, false) +
			gistpenalty(giststate, attno, &entryR, false, &entrySL, false);

		if (penalty1 > penalty2)
			leaveOnLeft = false;
	}
	else
	{
		GISTENTRY  *entry1 = (sv->spl_ldatum_exists) ? &entryL : &entryR;
		float		penalty1,
					penalty2;

		/*
		 * There is only one previously defined union, so we just choose swap
		 * or not by lowest penalty for that side.  We can only get here if a
		 * secondary split happened to have all NULLs in its column in the
		 * tuples that the outer recursion level had assigned to one side.
		 * (Note that the null checks in gistSplitByKey don't prevent the
		 * case, because they'll only be checking tuples that were considered
		 * don't-cares at the outer recursion level, not the tuples that went
		 * into determining the passed-down left and right union keys.)
		 */
		penalty1 = gistpenalty(giststate, attno, entry1, false, &entrySL, false);
		penalty2 = gistpenalty(giststate, attno, entry1, false, &entrySR, false);

		if (penalty1 < penalty2)
			leaveOnLeft = (sv->spl_ldatum_exists) ? true : false;
		else
			leaveOnLeft = (sv->spl_rdatum_exists) ? true : false;
	}

	if (leaveOnLeft == false)
	{
		/*
		 * swap left and right
		 */
		OffsetNumber *off,
					noff;
		Datum		datum;

		SWAPVAR(sv->spl_left, sv->spl_right, off);
		SWAPVAR(sv->spl_nleft, sv->spl_nright, noff);
		SWAPVAR(sv->spl_ldatum, sv->spl_rdatum, datum);
		gistentryinit(entrySL, sv->spl_ldatum, r, NULL, 0, false);
		gistentryinit(entrySR, sv->spl_rdatum, r, NULL, 0, false);
	}

	if (sv->spl_ldatum_exists)
		gistMakeUnionKey(giststate, attno, &entryL, false, &entrySL, false,
						 &sv->spl_ldatum, &tmpBool);

	if (sv->spl_rdatum_exists)
		gistMakeUnionKey(giststate, attno, &entryR, false, &entrySR, false,
						 &sv->spl_rdatum, &tmpBool);

	sv->spl_ldatum_exists = sv->spl_rdatum_exists = false;
}

/*
 * Trivial picksplit implementation. Function called only
 * if user-defined picksplit puts all keys on the same side of the split.
 * That is a bug of user-defined picksplit but we don't want to fail.
 */
static void
genericPickSplit(GISTSTATE *giststate, GistEntryVector *entryvec, GIST_SPLITVEC *v, int attno)
{
	OffsetNumber i,
				maxoff;
	int			nbytes;
	GistEntryVector *evec;

	maxoff = entryvec->n - 1;

	nbytes = (maxoff + 2) * sizeof(OffsetNumber);

	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);
	v->spl_nleft = v->spl_nright = 0;

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		if (i <= (maxoff - FirstOffsetNumber + 1) / 2)
		{
			v->spl_left[v->spl_nleft] = i;
			v->spl_nleft++;
		}
		else
		{
			v->spl_right[v->spl_nright] = i;
			v->spl_nright++;
		}
	}

	/*
	 * Form union datums for each side
	 */
	evec = palloc(sizeof(GISTENTRY) * entryvec->n + GEVHDRSZ);

	evec->n = v->spl_nleft;
	memcpy(evec->vector, entryvec->vector + FirstOffsetNumber,
		   sizeof(GISTENTRY) * evec->n);
	v->spl_ldatum = FunctionCall2Coll(&giststate->unionFn[attno],
									  giststate->supportCollation[attno],
									  PointerGetDatum(evec),
									  PointerGetDatum(&nbytes));

	evec->n = v->spl_nright;
	memcpy(evec->vector, entryvec->vector + FirstOffsetNumber + v->spl_nleft,
		   sizeof(GISTENTRY) * evec->n);
	v->spl_rdatum = FunctionCall2Coll(&giststate->unionFn[attno],
									  giststate->supportCollation[attno],
									  PointerGetDatum(evec),
									  PointerGetDatum(&nbytes));
}

/*
 * Calls user picksplit method for attno column to split tuples into
 * two vectors.
 *
 * Returns false if split is complete (there are no more index columns, or
 * there is no need to consider them because split is optimal already).
 *
 * Returns true and v->spl_dontcare = NULL if the picksplit result is
 * degenerate (all tuples seem to be don't-cares), so we should just
 * disregard this column and split on the next column(s) instead.
 *
 * Returns true and v->spl_dontcare != NULL if there are don't-care tuples
 * that could be relocated based on the next column(s).  The don't-care
 * tuples have been removed from the split and must be reinserted by caller.
 * There is at least one non-don't-care tuple on each side of the split,
 * and union keys for all columns are updated to include just those tuples.
 *
 * A true result implies there is at least one more index column.
 */
static bool
gistUserPicksplit(Relation r, GistEntryVector *entryvec, int attno, GistSplitVector *v,
				  IndexTuple *itup, int len, GISTSTATE *giststate)
{
	GIST_SPLITVEC *sv = &v->splitVector;

	/*
	 * Prepare spl_ldatum/spl_rdatum/spl_ldatum_exists/spl_rdatum_exists in
	 * case we are doing a secondary split (see comments in gist.h).
	 */
	sv->spl_ldatum_exists = (v->spl_lisnull[attno]) ? false : true;
	sv->spl_rdatum_exists = (v->spl_risnull[attno]) ? false : true;
	sv->spl_ldatum = v->spl_lattr[attno];
	sv->spl_rdatum = v->spl_rattr[attno];

	/*
	 * Let the opclass-specific PickSplit method do its thing.  Note that at
	 * this point we know there are no null keys in the entryvec.
	 */
	FunctionCall2Coll(&giststate->picksplitFn[attno],
					  giststate->supportCollation[attno],
					  PointerGetDatum(entryvec),
					  PointerGetDatum(sv));

	if (sv->spl_nleft == 0 || sv->spl_nright == 0)
	{
		/*
		 * User-defined picksplit failed to create an actual split, ie it put
		 * everything on the same side.  Complain but cope.
		 */
		ereport(DEBUG1,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("picksplit method for column %d of index \"%s\" failed",
						attno + 1, RelationGetRelationName(r)),
				 errhint("The index is not optimal. To optimize it, contact a developer, or try to use the column as the second one in the CREATE INDEX command.")));

		/*
		 * Reinit GIST_SPLITVEC. Although these fields are not used by
		 * genericPickSplit(), set them up for further processing
		 */
		sv->spl_ldatum_exists = (v->spl_lisnull[attno]) ? false : true;
		sv->spl_rdatum_exists = (v->spl_risnull[attno]) ? false : true;
		sv->spl_ldatum = v->spl_lattr[attno];
		sv->spl_rdatum = v->spl_rattr[attno];

		/* Do a generic split */
		genericPickSplit(giststate, entryvec, sv, attno);
	}
	else
	{
		/* hack for compatibility with old picksplit API */
		if (sv->spl_left[sv->spl_nleft - 1] == InvalidOffsetNumber)
			sv->spl_left[sv->spl_nleft - 1] = (OffsetNumber) (entryvec->n - 1);
		if (sv->spl_right[sv->spl_nright - 1] == InvalidOffsetNumber)
			sv->spl_right[sv->spl_nright - 1] = (OffsetNumber) (entryvec->n - 1);
	}

	/* Clean up if PickSplit didn't take care of a secondary split */
	if (sv->spl_ldatum_exists || sv->spl_rdatum_exists)
		supportSecondarySplit(r, giststate, attno, sv,
							  v->spl_lattr[attno], v->spl_rattr[attno]);

	/* emit union datums computed by PickSplit back to v arrays */
	v->spl_lattr[attno] = sv->spl_ldatum;
	v->spl_rattr[attno] = sv->spl_rdatum;
	v->spl_lisnull[attno] = false;
	v->spl_risnull[attno] = false;

	/*
	 * If index columns remain, then consider whether we can improve the split
	 * by using them.
	 */
	v->spl_dontcare = NULL;

	if (attno + 1 < giststate->nonLeafTupdesc->natts)
	{
		int			NumDontCare;

		/*
		 * Make a quick check to see if left and right union keys are equal;
		 * if so, the split is certainly degenerate, so tell caller to
		 * re-split with the next column.
		 */
		if (gistKeyIsEQ(giststate, attno, sv->spl_ldatum, sv->spl_rdatum))
			return true;

		/*
		 * Locate don't-care tuples, if any.  If there are none, the split is
		 * optimal, so just fall out and return false.
		 */
		v->spl_dontcare = (bool *) palloc0(sizeof(bool) * (entryvec->n + 1));

		NumDontCare = findDontCares(r, giststate, entryvec->vector, v, attno);

		if (NumDontCare > 0)
		{
			/*
			 * Remove don't-cares from spl_left[] and spl_right[].
			 */
			removeDontCares(sv->spl_left, &sv->spl_nleft, v->spl_dontcare);
			removeDontCares(sv->spl_right, &sv->spl_nright, v->spl_dontcare);

			/*
			 * If all tuples on either side were don't-cares, the split is
			 * degenerate, and we're best off to ignore it and split on the
			 * next column.  (We used to try to press on with a secondary
			 * split by forcing a random tuple on each side to be treated as
			 * non-don't-care, but it seems unlikely that that technique
			 * really gives a better result.  Note that we don't want to try a
			 * secondary split with empty left or right primary split sides,
			 * because then there is no union key on that side for the
			 * PickSplit function to try to expand, so it can have no good
			 * figure of merit for what it's doing.  Also note that this check
			 * ensures we can't produce a bogus one-side-only split in the
			 * NumDontCare == 1 special case below.)
			 */
			if (sv->spl_nleft == 0 || sv->spl_nright == 0)
			{
				v->spl_dontcare = NULL;
				return true;
			}

			/*
			 * Recompute union keys, considering only non-don't-care tuples.
			 * NOTE: this will set union keys for remaining index columns,
			 * which will cause later calls of gistUserPicksplit to pass those
			 * values down to user-defined PickSplit methods with
			 * spl_ldatum_exists/spl_rdatum_exists set true.
			 */
			gistunionsubkey(giststate, itup, v);

			if (NumDontCare == 1)
			{
				/*
				 * If there's only one don't-care tuple then we can't do a
				 * PickSplit on it, so just choose whether to send it left or
				 * right by comparing penalties.  We needed the
				 * gistunionsubkey step anyway so that we have appropriate
				 * union keys for figuring the penalties.
				 */
				OffsetNumber toMove;

				/* find it ... */
				for (toMove = FirstOffsetNumber; toMove < entryvec->n; toMove++)
				{
					if (v->spl_dontcare[toMove])
						break;
				}
				Assert(toMove < entryvec->n);

				/* ... and assign it to cheaper side */
				placeOne(r, giststate, v, itup[toMove - 1], toMove, attno + 1);

				/*
				 * At this point the union keys are wrong, but we don't care
				 * because we're done splitting.  The outermost recursion
				 * level of gistSplitByKey will fix things before returning.
				 */
			}
			else
				return true;
		}
	}

	return false;
}

/*
 * simply split page in half
 */
static void
gistSplitHalf(GIST_SPLITVEC *v, int len)
{
	int			i;

	v->spl_nright = v->spl_nleft = 0;
	v->spl_left = (OffsetNumber *) palloc(len * sizeof(OffsetNumber));
	v->spl_right = (OffsetNumber *) palloc(len * sizeof(OffsetNumber));
	for (i = 1; i <= len; i++)
		if (i < len / 2)
			v->spl_right[v->spl_nright++] = i;
		else
			v->spl_left[v->spl_nleft++] = i;

	/* we need not compute union keys, caller took care of it */
}

/*
 * gistSplitByKey: main entry point for page-splitting algorithm
 *
 * r: index relation
 * page: page being split
 * itup: array of IndexTuples to be processed
 * len: number of IndexTuples to be processed (must be at least 2)
 * giststate: additional info about index
 * v: working state and output area
 * attno: column we are working on (zero-based index)
 *
 * Outside caller must initialize v->spl_lisnull and v->spl_risnull arrays
 * to all-true.  On return, spl_left/spl_nleft contain indexes of tuples
 * to go left, spl_right/spl_nright contain indexes of tuples to go right,
 * spl_lattr/spl_lisnull contain left-side union key values, and
 * spl_rattr/spl_risnull contain right-side union key values.  Other fields
 * in this struct are workspace for this file.
 *
 * Outside caller must pass zero for attno.  The function may internally
 * recurse to the next column by passing attno+1.
 */
void
gistSplitByKey(Relation r, Page page, IndexTuple *itup, int len,
			   GISTSTATE *giststate, GistSplitVector *v, int attno)
{
	GistEntryVector *entryvec;
	OffsetNumber *offNullTuples;
	int			nOffNullTuples = 0;
	int			i;

	/* generate the item array, and identify tuples with null keys */
	/* note that entryvec->vector[0] goes unused in this code */
	entryvec = palloc(GEVHDRSZ + (len + 1) * sizeof(GISTENTRY));
	entryvec->n = len + 1;
	offNullTuples = (OffsetNumber *) palloc(len * sizeof(OffsetNumber));

	for (i = 1; i <= len; i++)
	{
		Datum		datum;
		bool		IsNull;

		datum = index_getattr(itup[i - 1], attno + 1, giststate->leafTupdesc,
							  &IsNull);
		gistdentryinit(giststate, attno, &(entryvec->vector[i]),
					   datum, r, page, i,
					   false, IsNull);
		if (IsNull)
			offNullTuples[nOffNullTuples++] = i;
	}

	if (nOffNullTuples == len)
	{
		/*
		 * Corner case: All keys in attno column are null, so just transfer
		 * our attention to the next column.  If there's no next column, just
		 * split page in half.
		 */
		v->spl_risnull[attno] = v->spl_lisnull[attno] = true;

		if (attno + 1 < giststate->nonLeafTupdesc->natts)
			gistSplitByKey(r, page, itup, len, giststate, v, attno + 1);
		else
			gistSplitHalf(&v->splitVector, len);
	}
	else if (nOffNullTuples > 0)
	{
		int			j = 0;

		/*
		 * We don't want to mix NULL and not-NULL keys on one page, so split
		 * nulls to right page and not-nulls to left.
		 */
		v->splitVector.spl_right = offNullTuples;
		v->splitVector.spl_nright = nOffNullTuples;
		v->spl_risnull[attno] = true;

		v->splitVector.spl_left = (OffsetNumber *) palloc(len * sizeof(OffsetNumber));
		v->splitVector.spl_nleft = 0;
		for (i = 1; i <= len; i++)
			if (j < v->splitVector.spl_nright && offNullTuples[j] == i)
				j++;
			else
				v->splitVector.spl_left[v->splitVector.spl_nleft++] = i;

		/* Compute union keys, unless outer recursion level will handle it */
		if (attno == 0 && giststate->nonLeafTupdesc->natts == 1)
		{
			v->spl_dontcare = NULL;
			gistunionsubkey(giststate, itup, v);
		}
	}
	else
	{
		/*
		 * All keys are not-null, so apply user-defined PickSplit method
		 */
		if (gistUserPicksplit(r, entryvec, attno, v, itup, len, giststate))
		{
			/*
			 * Splitting on attno column is not optimal, so consider
			 * redistributing don't-care tuples according to the next column
			 */
			Assert(attno + 1 < giststate->nonLeafTupdesc->natts);

			if (v->spl_dontcare == NULL)
			{
				/*
				 * This split was actually degenerate, so ignore it altogether
				 * and just split according to the next column.
				 */
				gistSplitByKey(r, page, itup, len, giststate, v, attno + 1);
			}
			else
			{
				/*
				 * Form an array of just the don't-care tuples to pass to a
				 * recursive invocation of this function for the next column.
				 */
				IndexTuple *newitup = (IndexTuple *) palloc(len * sizeof(IndexTuple));
				OffsetNumber *map = (OffsetNumber *) palloc(len * sizeof(OffsetNumber));
				int			newlen = 0;
				GIST_SPLITVEC backupSplit;

				for (i = 0; i < len; i++)
				{
					if (v->spl_dontcare[i + 1])
					{
						newitup[newlen] = itup[i];
						map[newlen] = i + 1;
						newlen++;
					}
				}

				Assert(newlen > 0);

				/*
				 * Make a backup copy of v->splitVector, since the recursive
				 * call will overwrite that with its own result.
				 */
				backupSplit = v->splitVector;
				backupSplit.spl_left = (OffsetNumber *) palloc(sizeof(OffsetNumber) * len);
				memcpy(backupSplit.spl_left, v->splitVector.spl_left, sizeof(OffsetNumber) * v->splitVector.spl_nleft);
				backupSplit.spl_right = (OffsetNumber *) palloc(sizeof(OffsetNumber) * len);
				memcpy(backupSplit.spl_right, v->splitVector.spl_right, sizeof(OffsetNumber) * v->splitVector.spl_nright);

				/* Recursively decide how to split the don't-care tuples */
				gistSplitByKey(r, page, newitup, newlen, giststate, v, attno + 1);

				/* Merge result of subsplit with non-don't-care tuples */
				for (i = 0; i < v->splitVector.spl_nleft; i++)
					backupSplit.spl_left[backupSplit.spl_nleft++] = map[v->splitVector.spl_left[i] - 1];
				for (i = 0; i < v->splitVector.spl_nright; i++)
					backupSplit.spl_right[backupSplit.spl_nright++] = map[v->splitVector.spl_right[i] - 1];

				v->splitVector = backupSplit;
			}
		}
	}

	/*
	 * If we're handling a multicolumn index, at the end of the recursion
	 * recompute the left and right union datums for all index columns.  This
	 * makes sure we hand back correct union datums in all corner cases,
	 * including when we haven't processed all columns to start with, or when
	 * a secondary split moved "don't care" tuples from one side to the other
	 * (we really shouldn't assume that that didn't change the union datums).
	 *
	 * Note: when we're in an internal recursion (attno > 0), we do not worry
	 * about whether the union datums we return with are sensible, since
	 * calling levels won't care.  Also, in a single-column index, we expect
	 * that PickSplit (or the special cases above) produced correct union
	 * datums.
	 */
	if (attno == 0 && giststate->nonLeafTupdesc->natts > 1)
	{
		v->spl_dontcare = NULL;
		gistunionsubkey(giststate, itup, v);
	}
}
