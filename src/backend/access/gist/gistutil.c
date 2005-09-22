/*-------------------------------------------------------------------------
 *
 * gistutil.c
 *	  utilities routines for the postgres GiST index access method.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			$PostgreSQL: pgsql/src/backend/access/gist/gistutil.c,v 1.7 2005/09/22 20:44:36 momjian Exp $
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/gistscan.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/freespace.h"

/* group flags ( in gistadjsubkey ) */
#define LEFT_ADDED		0x01
#define RIGHT_ADDED 0x02
#define BOTH_ADDED		( LEFT_ADDED | RIGHT_ADDED )


/*
 * This defines is only for shorter code, used in gistgetadjusted
 * and gistadjsubkey only
 */
#define FILLITEM(evp, isnullkey, okey, okeyb, rkey, rkeyb)	 do { \
	if (isnullkey) {											  \
		gistentryinit((evp), rkey, r, NULL,						  \
					  (OffsetNumber) 0, rkeyb, FALSE);			  \
	} else {													  \
		gistentryinit((evp), okey, r, NULL,						  \
					  (OffsetNumber) 0, okeyb, FALSE);			  \
	}															  \
} while(0)

#define FILLEV(isnull1, key1, key1b, isnull2, key2, key2b) do { \
	FILLITEM(*ev0p, isnull1, key1, key1b, key2, key2b);		\
	FILLITEM(*ev1p, isnull2, key2, key2b, key1, key1b);		\
} while(0);


static void gistpenalty(GISTSTATE *giststate, int attno,
			GISTENTRY *key1, bool isNull1,
			GISTENTRY *key2, bool isNull2, float *penalty);

/*
 * Write itup vector to page, has no control of free space
 */
OffsetNumber
gistfillbuffer(Relation r, Page page, IndexTuple *itup,
			   int len, OffsetNumber off)
{
	OffsetNumber l = InvalidOffsetNumber;
	int			i;

	if (off == InvalidOffsetNumber)
		off = (PageIsEmpty(page)) ? FirstOffsetNumber :
			OffsetNumberNext(PageGetMaxOffsetNumber(page));

	for (i = 0; i < len; i++)
	{
		l = PageAddItem(page, (Item) itup[i], IndexTupleSize(itup[i]),
						off, LP_USED);
		if (l == InvalidOffsetNumber)
			elog(ERROR, "failed to add item to index page in \"%s\"",
				 RelationGetRelationName(r));
		off++;
	}
	return l;
}

/*
 * Check space for itup vector on page
 */
bool
gistnospace(Page page, IndexTuple *itvec, int len)
{
	unsigned int size = 0;
	int			i;

	for (i = 0; i < len; i++)
		size += IndexTupleSize(itvec[i]) + sizeof(ItemIdData);

	return (PageGetFreeSpace(page) < size);
}

/*
 * Read buffer into itup vector
 */
IndexTuple *
gistextractbuffer(Buffer buffer, int *len /* out */ )
{
	OffsetNumber i,
				maxoff;
	IndexTuple *itvec;
	Page		p = (Page) BufferGetPage(buffer);

	maxoff = PageGetMaxOffsetNumber(p);
	*len = maxoff;
	itvec = palloc(sizeof(IndexTuple) * maxoff);
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		itvec[i - 1] = (IndexTuple) PageGetItem(p, PageGetItemId(p, i));

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
 * Return an IndexTuple containing the result of applying the "union"
 * method to the specified IndexTuple vector.
 */
IndexTuple
gistunion(Relation r, IndexTuple *itvec, int len, GISTSTATE *giststate)
{
	Datum		attr[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	GistEntryVector *evec;
	int			i;
	GISTENTRY	centry[INDEX_MAX_KEYS];
	IndexTuple	res;

	evec = (GistEntryVector *) palloc(((len == 1) ? 2 : len) * sizeof(GISTENTRY) + GEVHDRSZ);

	for (i = 0; i < len; i++)
		if (GistTupleIsInvalid(itvec[i]))
			return gist_form_invalid_tuple(InvalidBlockNumber);

	for (i = 0; i < r->rd_att->natts; i++)
	{
		Datum		datum;
		int			j;
		int			real_len;

		real_len = 0;
		for (j = 0; j < len; j++)
		{
			bool		IsNull;

			datum = index_getattr(itvec[j], i + 1, giststate->tupdesc, &IsNull);
			if (IsNull)
				continue;

			gistdentryinit(giststate, i,
						   &(evec->vector[real_len]),
						   datum,
						   NULL, NULL, (OffsetNumber) 0,
						   ATTSIZE(datum, giststate->tupdesc, i + 1, IsNull),
						   FALSE, IsNull);
			real_len++;
		}

		/* If this tuple vector was all NULLs, the union is NULL */
		if (real_len == 0)
		{
			attr[i] = (Datum) 0;
			isnull[i] = TRUE;
		}
		else
		{
			int			datumsize;

			if (real_len == 1)
			{
				evec->n = 2;
				gistentryinit(evec->vector[1],
							  evec->vector[0].key, r, NULL,
							  (OffsetNumber) 0, evec->vector[0].bytes, FALSE);
			}
			else
				evec->n = real_len;

			/* Compress the result of the union and store in attr array */
			datum = FunctionCall2(&giststate->unionFn[i],
								  PointerGetDatum(evec),
								  PointerGetDatum(&datumsize));

			gistcentryinit(giststate, i, &centry[i], datum,
						   NULL, NULL, (OffsetNumber) 0,
						   datumsize, FALSE, FALSE);
			isnull[i] = FALSE;
			attr[i] = centry[i].key;
		}
	}

	res = index_form_tuple(giststate->tupdesc, attr, isnull);
	GistTupleSetValid(res);
	return res;
}


/*
 * Forms union of oldtup and addtup, if union == oldtup then return NULL
 */
IndexTuple
gistgetadjusted(Relation r, IndexTuple oldtup, IndexTuple addtup, GISTSTATE *giststate)
{
	GistEntryVector *evec;
	bool		neednew = false;
	bool		isnull[INDEX_MAX_KEYS];
	Datum		attr[INDEX_MAX_KEYS];
	GISTENTRY	centry[INDEX_MAX_KEYS],
				oldatt[INDEX_MAX_KEYS],
				addatt[INDEX_MAX_KEYS],
			   *ev0p,
			   *ev1p;
	bool		oldisnull[INDEX_MAX_KEYS],
				addisnull[INDEX_MAX_KEYS];
	IndexTuple	newtup = NULL;
	int			i;

	if (GistTupleIsInvalid(oldtup) || GistTupleIsInvalid(addtup))
		return gist_form_invalid_tuple(ItemPointerGetBlockNumber(&(oldtup->t_tid)));

	evec = palloc(2 * sizeof(GISTENTRY) + GEVHDRSZ);
	evec->n = 2;
	ev0p = &(evec->vector[0]);
	ev1p = &(evec->vector[1]);


	gistDeCompressAtt(giststate, r, oldtup, NULL,
					  (OffsetNumber) 0, oldatt, oldisnull);

	gistDeCompressAtt(giststate, r, addtup, NULL,
					  (OffsetNumber) 0, addatt, addisnull);

	for (i = 0; i < r->rd_att->natts; i++)
	{
		if (oldisnull[i] && addisnull[i])
		{
			attr[i] = (Datum) 0;
			isnull[i] = TRUE;
		}
		else
		{
			Datum		datum;
			int			datumsize;

			FILLEV(oldisnull[i], oldatt[i].key, oldatt[i].bytes,
				   addisnull[i], addatt[i].key, addatt[i].bytes);

			datum = FunctionCall2(&giststate->unionFn[i],
								  PointerGetDatum(evec),
								  PointerGetDatum(&datumsize));

			if (oldisnull[i] || addisnull[i])
			{
				if (oldisnull[i])
					neednew = true;
			}
			else
			{
				bool		result;

				FunctionCall3(&giststate->equalFn[i],
							  ev0p->key,
							  datum,
							  PointerGetDatum(&result));

				if (!result)
					neednew = true;
			}

			gistcentryinit(giststate, i, &centry[i], datum,
						   NULL, NULL, (OffsetNumber) 0,
						   datumsize, FALSE, FALSE);

			attr[i] = centry[i].key;
			isnull[i] = FALSE;
		}
	}

	if (neednew)
	{
		/* need to update key */
		newtup = index_form_tuple(giststate->tupdesc, attr, isnull);
		newtup->t_tid = oldtup->t_tid;
	}

	return newtup;
}

void
gistunionsubkey(Relation r, GISTSTATE *giststate, IndexTuple *itvec, GIST_SPLITVEC *spl, bool isall)
{
	int			lr;

	for (lr = 0; lr < 2; lr++)
	{
		OffsetNumber *entries;
		int			i;
		Datum	   *attr;
		int			len,
				   *attrsize;
		bool	   *isnull;
		GistEntryVector *evec;

		if (lr)
		{
			attrsize = spl->spl_lattrsize;
			attr = spl->spl_lattr;
			len = spl->spl_nleft;
			entries = spl->spl_left;
			isnull = spl->spl_lisnull;
		}
		else
		{
			attrsize = spl->spl_rattrsize;
			attr = spl->spl_rattr;
			len = spl->spl_nright;
			entries = spl->spl_right;
			isnull = spl->spl_risnull;
		}

		evec = palloc(((len < 2) ? 2 : len) * sizeof(GISTENTRY) + GEVHDRSZ);

		for (i = (isall) ? 0 : 1; i < r->rd_att->natts; i++)
		{
			int			j;
			Datum		datum;
			int			datumsize;
			int			real_len;

			real_len = 0;
			for (j = 0; j < len; j++)
			{
				bool		IsNull;

				if (spl->spl_idgrp[entries[j]])
					continue;
				datum = index_getattr(itvec[entries[j] - 1], i + 1,
									  giststate->tupdesc, &IsNull);
				if (IsNull)
					continue;
				gistdentryinit(giststate, i,
							   &(evec->vector[real_len]),
							   datum,
							   NULL, NULL, (OffsetNumber) 0,
						   ATTSIZE(datum, giststate->tupdesc, i + 1, IsNull),
							   FALSE, IsNull);
				real_len++;

			}

			if (real_len == 0)
			{
				datum = (Datum) 0;
				datumsize = 0;
				isnull[i] = true;
			}
			else
			{
				/*
				 * evec->vector[0].bytes may be not defined, so form union
				 * with itself
				 */
				if (real_len == 1)
				{
					evec->n = 2;
					memcpy(&(evec->vector[1]), &(evec->vector[0]),
						   sizeof(GISTENTRY));
				}
				else
					evec->n = real_len;
				datum = FunctionCall2(&giststate->unionFn[i],
									  PointerGetDatum(evec),
									  PointerGetDatum(&datumsize));
				isnull[i] = false;
			}

			attr[i] = datum;
			attrsize[i] = datumsize;
		}
	}
}

/*
 * find group in vector with equal value
 */
int
gistfindgroup(GISTSTATE *giststate, GISTENTRY *valvec, GIST_SPLITVEC *spl)
{
	int			i;
	int			curid = 1;

	/*
	 * first key is always not null (see gistinsert), so we may not check for
	 * nulls
	 */
	for (i = 0; i < spl->spl_nleft; i++)
	{
		int			j;
		int			len;
		bool		result;

		if (spl->spl_idgrp[spl->spl_left[i]])
			continue;
		len = 0;
		/* find all equal value in right part */
		for (j = 0; j < spl->spl_nright; j++)
		{
			if (spl->spl_idgrp[spl->spl_right[j]])
				continue;
			FunctionCall3(&giststate->equalFn[0],
						  valvec[spl->spl_left[i]].key,
						  valvec[spl->spl_right[j]].key,
						  PointerGetDatum(&result));
			if (result)
			{
				spl->spl_idgrp[spl->spl_right[j]] = curid;
				len++;
			}
		}
		/* find all other equal value in left part */
		if (len)
		{
			/* add current val to list of equal values */
			spl->spl_idgrp[spl->spl_left[i]] = curid;
			/* searching .. */
			for (j = i + 1; j < spl->spl_nleft; j++)
			{
				if (spl->spl_idgrp[spl->spl_left[j]])
					continue;
				FunctionCall3(&giststate->equalFn[0],
							  valvec[spl->spl_left[i]].key,
							  valvec[spl->spl_left[j]].key,
							  PointerGetDatum(&result));
				if (result)
				{
					spl->spl_idgrp[spl->spl_left[j]] = curid;
					len++;
				}
			}
			spl->spl_ngrp[curid] = len + 1;
			curid++;
		}
	}

	return curid;
}

/*
 * Insert equivalent tuples to left or right page with minimum
 * penalty
 */
void
gistadjsubkey(Relation r,
			  IndexTuple *itup, /* contains compressed entry */
			  int len,
			  GIST_SPLITVEC *v,
			  GISTSTATE *giststate)
{
	int			curlen;
	OffsetNumber *curwpos;
	GISTENTRY	entry,
				identry[INDEX_MAX_KEYS],
			   *ev0p,
			   *ev1p;
	float		lpenalty,
				rpenalty;
	GistEntryVector *evec;
	int			datumsize;
	bool		isnull[INDEX_MAX_KEYS];
	int			i,
				j;

	/* clear vectors */
	curlen = v->spl_nleft;
	curwpos = v->spl_left;
	for (i = 0; i < v->spl_nleft; i++)
	{
		if (v->spl_idgrp[v->spl_left[i]] == 0)
		{
			*curwpos = v->spl_left[i];
			curwpos++;
		}
		else
			curlen--;
	}
	v->spl_nleft = curlen;

	curlen = v->spl_nright;
	curwpos = v->spl_right;
	for (i = 0; i < v->spl_nright; i++)
	{
		if (v->spl_idgrp[v->spl_right[i]] == 0)
		{
			*curwpos = v->spl_right[i];
			curwpos++;
		}
		else
			curlen--;
	}
	v->spl_nright = curlen;

	evec = palloc(2 * sizeof(GISTENTRY) + GEVHDRSZ);
	evec->n = 2;
	ev0p = &(evec->vector[0]);
	ev1p = &(evec->vector[1]);

	/* add equivalent tuple */
	for (i = 0; i < len; i++)
	{
		Datum		datum;

		if (v->spl_idgrp[i + 1] == 0)	/* already inserted */
			continue;
		gistDeCompressAtt(giststate, r, itup[i], NULL, (OffsetNumber) 0,
						  identry, isnull);

		v->spl_ngrp[v->spl_idgrp[i + 1]]--;
		if (v->spl_ngrp[v->spl_idgrp[i + 1]] == 0 &&
			(v->spl_grpflag[v->spl_idgrp[i + 1]] & BOTH_ADDED) != BOTH_ADDED)
		{
			/* force last in group */
			rpenalty = 1.0;
			lpenalty = (v->spl_grpflag[v->spl_idgrp[i + 1]] & LEFT_ADDED) ? 2.0 : 0.0;
		}
		else
		{
			/* where? */
			for (j = 1; j < r->rd_att->natts; j++)
			{
				gistentryinit(entry, v->spl_lattr[j], r, NULL,
							  (OffsetNumber) 0, v->spl_lattrsize[j], FALSE);
				gistpenalty(giststate, j, &entry, v->spl_lisnull[j],
							&identry[j], isnull[j], &lpenalty);

				gistentryinit(entry, v->spl_rattr[j], r, NULL,
							  (OffsetNumber) 0, v->spl_rattrsize[j], FALSE);
				gistpenalty(giststate, j, &entry, v->spl_risnull[j],
							&identry[j], isnull[j], &rpenalty);

				if (lpenalty != rpenalty)
					break;
			}
		}

		/*
		 * add XXX: refactor this to avoid duplicating code
		 */
		if (lpenalty < rpenalty)
		{
			v->spl_grpflag[v->spl_idgrp[i + 1]] |= LEFT_ADDED;
			v->spl_left[v->spl_nleft] = i + 1;
			v->spl_nleft++;
			for (j = 1; j < r->rd_att->natts; j++)
			{
				if (isnull[j] && v->spl_lisnull[j])
				{
					v->spl_lattr[j] = (Datum) 0;
					v->spl_lattrsize[j] = 0;
				}
				else
				{
					FILLEV(v->spl_lisnull[j], v->spl_lattr[j], v->spl_lattrsize[j],
						   isnull[j], identry[j].key, identry[j].bytes);

					datum = FunctionCall2(&giststate->unionFn[j],
										  PointerGetDatum(evec),
										  PointerGetDatum(&datumsize));

					v->spl_lattr[j] = datum;
					v->spl_lattrsize[j] = datumsize;
					v->spl_lisnull[j] = false;
				}
			}
		}
		else
		{
			v->spl_grpflag[v->spl_idgrp[i + 1]] |= RIGHT_ADDED;
			v->spl_right[v->spl_nright] = i + 1;
			v->spl_nright++;
			for (j = 1; j < r->rd_att->natts; j++)
			{
				if (isnull[j] && v->spl_risnull[j])
				{
					v->spl_rattr[j] = (Datum) 0;
					v->spl_rattrsize[j] = 0;
				}
				else
				{
					FILLEV(v->spl_risnull[j], v->spl_rattr[j], v->spl_rattrsize[j],
						   isnull[j], identry[j].key, identry[j].bytes);

					datum = FunctionCall2(&giststate->unionFn[j],
										  PointerGetDatum(evec),
										  PointerGetDatum(&datumsize));

					v->spl_rattr[j] = datum;
					v->spl_rattrsize[j] = datumsize;
					v->spl_risnull[j] = false;
				}
			}
		}
	}
}

/*
 * find entry with lowest penalty
 */
OffsetNumber
gistchoose(Relation r, Page p, IndexTuple it,	/* it has compressed entry */
		   GISTSTATE *giststate)
{
	OffsetNumber maxoff;
	OffsetNumber i;
	OffsetNumber which;
	float		sum_grow,
				which_grow[INDEX_MAX_KEYS];
	GISTENTRY	entry,
				identry[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	maxoff = PageGetMaxOffsetNumber(p);
	*which_grow = -1.0;
	which = InvalidOffsetNumber;
	sum_grow = 1;
	gistDeCompressAtt(giststate, r,
					  it, NULL, (OffsetNumber) 0,
					  identry, isnull);

	for (i = FirstOffsetNumber; i <= maxoff && sum_grow; i = OffsetNumberNext(i))
	{
		int			j;
		IndexTuple	itup = (IndexTuple) PageGetItem(p, PageGetItemId(p, i));

		if (!GistPageIsLeaf(p) && GistTupleIsInvalid(itup))
		{
			ereport(LOG,
					(errmsg("index \"%s\" needs VACUUM or REINDEX to finish crash recovery",
							RelationGetRelationName(r))));
			continue;
		}

		sum_grow = 0;
		for (j = 0; j < r->rd_att->natts; j++)
		{
			Datum		datum;
			float		usize;
			bool		IsNull;

			datum = index_getattr(itup, j + 1, giststate->tupdesc, &IsNull);
			gistdentryinit(giststate, j, &entry, datum, r, p, i,
						   ATTSIZE(datum, giststate->tupdesc, j + 1, IsNull),
						   FALSE, IsNull);
			gistpenalty(giststate, j, &entry, IsNull,
						&identry[j], isnull[j], &usize);

			if (which_grow[j] < 0 || usize < which_grow[j])
			{
				which = i;
				which_grow[j] = usize;
				if (j < r->rd_att->natts - 1 && i == FirstOffsetNumber)
					which_grow[j + 1] = -1;
				sum_grow += which_grow[j];
			}
			else if (which_grow[j] == usize)
				sum_grow += usize;
			else
			{
				sum_grow = 1;
				break;
			}
		}
	}

	if (which == InvalidOffsetNumber)
		which = FirstOffsetNumber;

	return which;
}

/*
 * initialize a GiST entry with a decompressed version of key
 */
void
gistdentryinit(GISTSTATE *giststate, int nkey, GISTENTRY *e,
			   Datum k, Relation r, Page pg, OffsetNumber o,
			   int b, bool l, bool isNull)
{
	if (b && !isNull)
	{
		GISTENTRY  *dep;

		gistentryinit(*e, k, r, pg, o, b, l);
		dep = (GISTENTRY *)
			DatumGetPointer(FunctionCall1(&giststate->decompressFn[nkey],
										  PointerGetDatum(e)));
		/* decompressFn may just return the given pointer */
		if (dep != e)
			gistentryinit(*e, dep->key, dep->rel, dep->page, dep->offset,
						  dep->bytes, dep->leafkey);
	}
	else
		gistentryinit(*e, (Datum) 0, r, pg, o, 0, l);
}


/*
 * initialize a GiST entry with a compressed version of key
 */
void
gistcentryinit(GISTSTATE *giststate, int nkey,
			   GISTENTRY *e, Datum k, Relation r,
			   Page pg, OffsetNumber o, int b, bool l, bool isNull)
{
	if (!isNull)
	{
		GISTENTRY  *cep;

		gistentryinit(*e, k, r, pg, o, b, l);
		cep = (GISTENTRY *)
			DatumGetPointer(FunctionCall1(&giststate->compressFn[nkey],
										  PointerGetDatum(e)));
		/* compressFn may just return the given pointer */
		if (cep != e)
			gistentryinit(*e, cep->key, cep->rel, cep->page, cep->offset,
						  cep->bytes, cep->leafkey);
	}
	else
		gistentryinit(*e, (Datum) 0, r, pg, o, 0, l);
}

IndexTuple
gistFormTuple(GISTSTATE *giststate, Relation r,
			  Datum attdata[], int datumsize[], bool isnull[])
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
						   NULL, NULL, (OffsetNumber) 0,
						   datumsize[i], FALSE, FALSE);
			compatt[i] = centry[i].key;
		}
	}

	res = index_form_tuple(giststate->tupdesc, compatt, isnull);
	GistTupleSetValid(res);
	return res;
}

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
					   ATTSIZE(datum, giststate->tupdesc, i + 1, isnull[i]),
					   FALSE, isnull[i]);
	}
}

static void
gistpenalty(GISTSTATE *giststate, int attno,
			GISTENTRY *key1, bool isNull1,
			GISTENTRY *key2, bool isNull2, float *penalty)
{
	if (giststate->penaltyFn[attno].fn_strict && (isNull1 || isNull2))
		*penalty = 0.0;
	else
		FunctionCall3(&giststate->penaltyFn[attno],
					  PointerGetDatum(key1),
					  PointerGetDatum(key2),
					  PointerGetDatum(penalty));
}

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
	opaque->flags = f;
	opaque->rightlink = InvalidBlockNumber;
	memset(&(opaque->nsn), 0, sizeof(GistNSN));
}

void
gistUserPicksplit(Relation r, GistEntryVector *entryvec, GIST_SPLITVEC *v,
				  IndexTuple *itup, int len, GISTSTATE *giststate)
{
	/*
	 * now let the user-defined picksplit function set up the split vector; in
	 * entryvec have no null value!!
	 */
	FunctionCall2(&giststate->picksplitFn[0],
				  PointerGetDatum(entryvec),
				  PointerGetDatum(v));

	/* compatibility with old code */
	if (v->spl_left[v->spl_nleft - 1] == InvalidOffsetNumber)
		v->spl_left[v->spl_nleft - 1] = (OffsetNumber) (entryvec->n - 1);
	if (v->spl_right[v->spl_nright - 1] == InvalidOffsetNumber)
		v->spl_right[v->spl_nright - 1] = (OffsetNumber) (entryvec->n - 1);

	v->spl_lattr[0] = v->spl_ldatum;
	v->spl_rattr[0] = v->spl_rdatum;
	v->spl_lisnull[0] = false;
	v->spl_risnull[0] = false;

	/*
	 * if index is multikey, then we must to try get smaller bounding box for
	 * subkey(s)
	 */
	if (r->rd_att->natts > 1)
	{
		int			MaxGrpId;

		v->spl_idgrp = (int *) palloc0(sizeof(int) * entryvec->n);
		v->spl_grpflag = (char *) palloc0(sizeof(char) * entryvec->n);
		v->spl_ngrp = (int *) palloc(sizeof(int) * entryvec->n);

		MaxGrpId = gistfindgroup(giststate, entryvec->vector, v);

		/* form union of sub keys for each page (l,p) */
		gistunionsubkey(r, giststate, itup, v, false);

		/*
		 * if possible, we insert equivalent tuples with control by penalty
		 * for a subkey(s)
		 */
		if (MaxGrpId > 1)
			gistadjsubkey(r, itup, len, v, giststate);
	}
}

Buffer
gistNewBuffer(Relation r)
{
	Buffer		buffer = InvalidBuffer;
	bool		needLock;

	while (true)
	{
		BlockNumber blkno = GetFreeIndexPage(&r->rd_node);

		if (blkno == InvalidBlockNumber)
			break;

		buffer = ReadBuffer(r, blkno);
		if (ConditionalLockBuffer(buffer))
		{
			Page		page = BufferGetPage(buffer);

			if (GistPageIsDeleted(page))
			{
				GistPageSetNonDeleted(page);
				return buffer;
			}
			else
				LockBuffer(buffer, GIST_UNLOCK);
		}

		ReleaseBuffer(buffer);
	}

	needLock = !RELATION_IS_LOCAL(r);

	if (needLock)
		LockRelationForExtension(r, ExclusiveLock);

	buffer = ReadBuffer(r, P_NEW);
	LockBuffer(buffer, GIST_EXCLUSIVE);

	if (needLock)
		UnlockRelationForExtension(r, ExclusiveLock);

	return buffer;
}
