/*-------------------------------------------------------------------------
 *
 * gistsplit.c
 *	  Split page algorithm
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/gist/gistsplit.c,v 1.5 2008/01/01 19:45:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gist_private.h"

typedef struct
{
	Datum	   *attr;
	int			len;
	OffsetNumber *entries;
	bool	   *isnull;
	bool	   *equiv;
} GistSplitUnion;


/*
 * Forms unions of subkeys after page split, but
 * uses only tuples aren't in groups of equalent tuples
 */
static void
gistunionsubkeyvec(GISTSTATE *giststate, IndexTuple *itvec,
				   GistSplitUnion *gsvp, int startkey)
{
	IndexTuple *cleanedItVec;
	int			i,
				cleanedLen = 0;

	cleanedItVec = (IndexTuple *) palloc(sizeof(IndexTuple) * gsvp->len);

	for (i = 0; i < gsvp->len; i++)
	{
		if (gsvp->equiv && gsvp->equiv[gsvp->entries[i]])
			continue;

		cleanedItVec[cleanedLen++] = itvec[gsvp->entries[i] - 1];
	}

	gistMakeUnionItVec(giststate, cleanedItVec, cleanedLen, startkey,
					   gsvp->attr, gsvp->isnull);

	pfree(cleanedItVec);
}

/*
 * unions subkeys for after user picksplit over attno-1 column
 */
static void
gistunionsubkey(GISTSTATE *giststate, IndexTuple *itvec, GistSplitVector *spl, int attno)
{
	GistSplitUnion gsvp;

	gsvp.equiv = spl->spl_equiv;

	gsvp.attr = spl->spl_lattr;
	gsvp.len = spl->splitVector.spl_nleft;
	gsvp.entries = spl->splitVector.spl_left;
	gsvp.isnull = spl->spl_lisnull;

	gistunionsubkeyvec(giststate, itvec, &gsvp, attno);

	gsvp.attr = spl->spl_rattr;
	gsvp.len = spl->splitVector.spl_nright;
	gsvp.entries = spl->splitVector.spl_right;
	gsvp.isnull = spl->spl_risnull;

	gistunionsubkeyvec(giststate, itvec, &gsvp, attno);
}

/*
 * find group in vector with equivalent value
 */
static int
gistfindgroup(Relation r, GISTSTATE *giststate, GISTENTRY *valvec, GistSplitVector *spl, int attno)
{
	int			i;
	GISTENTRY	entry;
	int			len = 0;

	/*
	 * attno key is always not null (see gistSplitByKey), so we may not check
	 * for nulls
	 */
	gistentryinit(entry, spl->splitVector.spl_rdatum, r, NULL, (OffsetNumber) 0, FALSE);
	for (i = 0; i < spl->splitVector.spl_nleft; i++)
	{
		float		penalty = gistpenalty(giststate, attno, &entry, false,
							   &valvec[spl->splitVector.spl_left[i]], false);

		if (penalty == 0.0)
		{
			spl->spl_equiv[spl->splitVector.spl_left[i]] = true;
			len++;
		}
	}

	gistentryinit(entry, spl->splitVector.spl_ldatum, r, NULL, (OffsetNumber) 0, FALSE);
	for (i = 0; i < spl->splitVector.spl_nright; i++)
	{
		float		penalty = gistpenalty(giststate, attno, &entry, false,
							  &valvec[spl->splitVector.spl_right[i]], false);

		if (penalty == 0.0)
		{
			spl->spl_equiv[spl->splitVector.spl_right[i]] = true;
			len++;
		}
	}

	return len;
}

static void
cleanupOffsets(OffsetNumber *a, int *len, bool *equiv, int *LenEquiv)
{
	int			curlen,
				i;
	OffsetNumber *curwpos;

	curlen = *len;
	curwpos = a;
	for (i = 0; i < *len; i++)
	{
		if (equiv[a[i]] == FALSE)
		{
			*curwpos = a[i];
			curwpos++;
		}
		else
		{
			/* corner case: we shouldn't make void array */
			if (curlen == 1)
			{
				equiv[a[i]] = FALSE;	/* mark item as non-equivalent */
				i--;			/* redo the same */
				*LenEquiv -= 1;
				continue;
			}
			else
				curlen--;
		}
	}

	*len = curlen;
}

static void
placeOne(Relation r, GISTSTATE *giststate, GistSplitVector *v, IndexTuple itup, OffsetNumber off, int attno)
{
	GISTENTRY	identry[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	bool		toLeft = true;

	gistDeCompressAtt(giststate, r, itup, NULL, (OffsetNumber) 0, identry, isnull);

	for (; attno < giststate->tupdesc->natts; attno++)
	{
		float		lpenalty,
					rpenalty;
		GISTENTRY	entry;

		gistentryinit(entry, v->spl_lattr[attno], r, NULL, 0, FALSE);
		lpenalty = gistpenalty(giststate, attno, &entry, v->spl_lisnull[attno], identry + attno, isnull[attno]);
		gistentryinit(entry, v->spl_rattr[attno], r, NULL, 0, FALSE);
		rpenalty = gistpenalty(giststate, attno, &entry, v->spl_risnull[attno], identry + attno, isnull[attno]);

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
 * adjust left and right unions according to splits by previous
 * split by firsts columns. This function is called only in case
 * when pickSplit doesn't support subspplit.
 */

static void
supportSecondarySplit(Relation r, GISTSTATE *giststate, int attno, GIST_SPLITVEC *sv, Datum oldL, Datum oldR)
{
	bool		leaveOnLeft = true,
				tmpBool;
	GISTENTRY	entryL,
				entryR,
				entrySL,
				entrySR;

	gistentryinit(entryL, oldL, r, NULL, 0, FALSE);
	gistentryinit(entryR, oldR, r, NULL, 0, FALSE);
	gistentryinit(entrySL, sv->spl_ldatum, r, NULL, 0, FALSE);
	gistentryinit(entrySR, sv->spl_rdatum, r, NULL, 0, FALSE);

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
		 * there is only one previously defined union, so we just choose swap
		 * or not by lowest penalty
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
		gistentryinit(entrySL, sv->spl_ldatum, r, NULL, 0, FALSE);
		gistentryinit(entrySR, sv->spl_rdatum, r, NULL, 0, FALSE);
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
 * Calls user picksplit method for attno columns to split vector to
 * two vectors. May use attno+n columns data to
 * get better split.
 * Returns TRUE and v->spl_equiv = NULL if left and right unions of attno columns are the same,
 * so caller may find better split
 * Returns TRUE and v->spl_equiv != NULL if there is tuples which may be freely moved
 */
static bool
gistUserPicksplit(Relation r, GistEntryVector *entryvec, int attno, GistSplitVector *v,
				  IndexTuple *itup, int len, GISTSTATE *giststate)
{
	GIST_SPLITVEC *sv = &v->splitVector;

	/*
	 * now let the user-defined picksplit function set up the split vector; in
	 * entryvec have no null value!!
	 */

	sv->spl_ldatum_exists = (v->spl_lisnull[attno]) ? false : true;
	sv->spl_rdatum_exists = (v->spl_risnull[attno]) ? false : true;
	sv->spl_ldatum = v->spl_lattr[attno];
	sv->spl_rdatum = v->spl_rattr[attno];

	FunctionCall2(&giststate->picksplitFn[attno],
				  PointerGetDatum(entryvec),
				  PointerGetDatum(sv));

	/* compatibility with old code */
	if (sv->spl_left[sv->spl_nleft - 1] == InvalidOffsetNumber)
		sv->spl_left[sv->spl_nleft - 1] = (OffsetNumber) (entryvec->n - 1);
	if (sv->spl_right[sv->spl_nright - 1] == InvalidOffsetNumber)
		sv->spl_right[sv->spl_nright - 1] = (OffsetNumber) (entryvec->n - 1);

	if (sv->spl_ldatum_exists || sv->spl_rdatum_exists)
	{
		elog(LOG, "PickSplit method of %d columns of index '%s' doesn't support secondary split",
			 attno + 1, RelationGetRelationName(r));

		supportSecondarySplit(r, giststate, attno, sv, v->spl_lattr[attno], v->spl_rattr[attno]);
	}

	v->spl_lattr[attno] = sv->spl_ldatum;
	v->spl_rattr[attno] = sv->spl_rdatum;
	v->spl_lisnull[attno] = false;
	v->spl_risnull[attno] = false;

	/*
	 * if index is multikey, then we must to try get smaller bounding box for
	 * subkey(s)
	 */
	v->spl_equiv = NULL;

	if (giststate->tupdesc->natts > 1 && attno + 1 != giststate->tupdesc->natts)
	{
		if (gistKeyIsEQ(giststate, attno, sv->spl_ldatum, sv->spl_rdatum))
		{
			/*
			 * Left and right key's unions are equial, so we can get better
			 * split by following columns. Note, unions for attno columns are
			 * already done.
			 */

			return true;
		}
		else
		{
			int			LenEquiv;

			v->spl_equiv = (bool *) palloc0(sizeof(bool) * (entryvec->n + 1));

			LenEquiv = gistfindgroup(r, giststate, entryvec->vector, v, attno);

			/*
			 * if possible, we should distribute equivalent tuples
			 */
			if (LenEquiv == 0)
			{
				gistunionsubkey(giststate, itup, v, attno + 1);
			}
			else
			{
				cleanupOffsets(sv->spl_left, &sv->spl_nleft, v->spl_equiv, &LenEquiv);
				cleanupOffsets(sv->spl_right, &sv->spl_nright, v->spl_equiv, &LenEquiv);

				gistunionsubkey(giststate, itup, v, attno + 1);
				if (LenEquiv == 1)
				{
					/*
					 * In case with one tuple we just choose left-right by
					 * penalty. It's simplify user-defined pickSplit
					 */
					OffsetNumber toMove = InvalidOffsetNumber;

					for (toMove = FirstOffsetNumber; toMove < entryvec->n; toMove++)
						if (v->spl_equiv[toMove])
							break;
					Assert(toMove < entryvec->n);

					placeOne(r, giststate, v, itup[toMove - 1], toMove, attno + 1);

					/*
					 * redo gistunionsubkey(): it will not degradate
					 * performance, because it's very rarely
					 */
					v->spl_equiv = NULL;
					gistunionsubkey(giststate, itup, v, attno + 1);

					return false;
				}
				else if (LenEquiv > 1)
					return true;
			}
		}
	}

	return false;
}

/*
 * simple split page
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
}

/*
 * if it was invalid tuple then we need special processing.
 * We move all invalid tuples on right page.
 *
 * if there is no place on left page, gistSplit will be called one more
 * time for left page.
 *
 * Normally, we never exec this code, but after crash replay it's possible
 * to get 'invalid' tuples (probability is low enough)
 */
static void
gistSplitByInvalid(GISTSTATE *giststate, GistSplitVector *v, IndexTuple *itup, int len)
{
	int			i;
	static OffsetNumber offInvTuples[MaxOffsetNumber];
	int			nOffInvTuples = 0;

	for (i = 1; i <= len; i++)
		if (GistTupleIsInvalid(itup[i - 1]))
			offInvTuples[nOffInvTuples++] = i;

	if (nOffInvTuples == len)
	{
		/* corner case, all tuples are invalid */
		v->spl_rightvalid = v->spl_leftvalid = false;
		gistSplitHalf(&v->splitVector, len);
	}
	else
	{
		GistSplitUnion gsvp;

		v->splitVector.spl_right = offInvTuples;
		v->splitVector.spl_nright = nOffInvTuples;
		v->spl_rightvalid = false;

		v->splitVector.spl_left = (OffsetNumber *) palloc(len * sizeof(OffsetNumber));
		v->splitVector.spl_nleft = 0;
		for (i = 1; i <= len; i++)
			if (!GistTupleIsInvalid(itup[i - 1]))
				v->splitVector.spl_left[v->splitVector.spl_nleft++] = i;
		v->spl_leftvalid = true;

		gsvp.equiv = NULL;
		gsvp.attr = v->spl_lattr;
		gsvp.len = v->splitVector.spl_nleft;
		gsvp.entries = v->splitVector.spl_left;
		gsvp.isnull = v->spl_lisnull;

		gistunionsubkeyvec(giststate, itup, &gsvp, 0);
	}
}

/*
 * trys to split page by attno key, in a case of null
 * values move its to separate page.
 */
void
gistSplitByKey(Relation r, Page page, IndexTuple *itup, int len, GISTSTATE *giststate,
			   GistSplitVector *v, GistEntryVector *entryvec, int attno)
{
	int			i;
	static OffsetNumber offNullTuples[MaxOffsetNumber];
	int			nOffNullTuples = 0;

	for (i = 1; i <= len; i++)
	{
		Datum		datum;
		bool		IsNull;

		if (!GistPageIsLeaf(page) && GistTupleIsInvalid(itup[i - 1]))
		{
			gistSplitByInvalid(giststate, v, itup, len);
			return;
		}

		datum = index_getattr(itup[i - 1], attno + 1, giststate->tupdesc, &IsNull);
		gistdentryinit(giststate, attno, &(entryvec->vector[i]),
					   datum, r, page, i,
					   FALSE, IsNull);
		if (IsNull)
			offNullTuples[nOffNullTuples++] = i;
	}

	v->spl_leftvalid = v->spl_rightvalid = true;

	if (nOffNullTuples == len)
	{
		/*
		 * Corner case: All keys in attno column are null, we should try to
		 * split by keys in next column. It all keys in all columns are NULL
		 * just split page half by half
		 */
		v->spl_risnull[attno] = v->spl_lisnull[attno] = TRUE;

		if (attno + 1 == r->rd_att->natts)
			gistSplitHalf(&v->splitVector, len);
		else
			gistSplitByKey(r, page, itup, len, giststate, v, entryvec, attno + 1);
	}
	else if (nOffNullTuples > 0)
	{
		int			j = 0;

		/*
		 * We don't want to mix NULLs and not-NULLs keys on one page, so move
		 * nulls to right page
		 */
		v->splitVector.spl_right = offNullTuples;
		v->splitVector.spl_nright = nOffNullTuples;
		v->spl_risnull[attno] = TRUE;

		v->splitVector.spl_left = (OffsetNumber *) palloc(len * sizeof(OffsetNumber));
		v->splitVector.spl_nleft = 0;
		for (i = 1; i <= len; i++)
			if (j < v->splitVector.spl_nright && offNullTuples[j] == i)
				j++;
			else
				v->splitVector.spl_left[v->splitVector.spl_nleft++] = i;

		v->spl_equiv = NULL;
		gistunionsubkey(giststate, itup, v, attno);
	}
	else
	{
		/*
		 * all keys are not-null
		 */
		entryvec->n = len + 1;

		if (gistUserPicksplit(r, entryvec, attno, v, itup, len, giststate) && attno + 1 != r->rd_att->natts)
		{
			/*
			 * Splitting on attno column is not optimized: there is a tuples
			 * which can be freely left or right page, we will try to split
			 * page by following columns
			 */
			if (v->spl_equiv == NULL)
			{
				/*
				 * simple case: left and right keys for attno column are
				 * equial
				 */
				gistSplitByKey(r, page, itup, len, giststate, v, entryvec, attno + 1);
			}
			else
			{
				/* we should clean up vector from already distributed tuples */
				IndexTuple *newitup = (IndexTuple *) palloc((len + 1) * sizeof(IndexTuple));
				OffsetNumber *map = (OffsetNumber *) palloc((len + 1) * sizeof(IndexTuple));
				int			newlen = 0;
				GIST_SPLITVEC backupSplit = v->splitVector;

				for (i = 0; i < len; i++)
					if (v->spl_equiv[i + 1])
					{
						map[newlen] = i + 1;
						newitup[newlen++] = itup[i];
					}

				Assert(newlen > 0);

				backupSplit.spl_left = (OffsetNumber *) palloc(sizeof(OffsetNumber) * len);
				memcpy(backupSplit.spl_left, v->splitVector.spl_left, sizeof(OffsetNumber) * v->splitVector.spl_nleft);
				backupSplit.spl_right = (OffsetNumber *) palloc(sizeof(OffsetNumber) * len);
				memcpy(backupSplit.spl_right, v->splitVector.spl_right, sizeof(OffsetNumber) * v->splitVector.spl_nright);

				gistSplitByKey(r, page, newitup, newlen, giststate, v, entryvec, attno + 1);

				/* merge result of subsplit */
				for (i = 0; i < v->splitVector.spl_nleft; i++)
					backupSplit.spl_left[backupSplit.spl_nleft++] = map[v->splitVector.spl_left[i] - 1];
				for (i = 0; i < v->splitVector.spl_nright; i++)
					backupSplit.spl_right[backupSplit.spl_nright++] = map[v->splitVector.spl_right[i] - 1];

				v->splitVector = backupSplit;
				/* reunion left and right datums */
				gistunionsubkey(giststate, itup, v, attno);
			}
		}
	}
}
