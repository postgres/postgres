/*-------------------------------------------------------------------------
 *
 * gistutil.c
 *	  utilities routines for the postgres GiST index access method.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			$PostgreSQL: pgsql/src/backend/access/gist/gistutil.c,v 1.14 2006/05/24 11:01:39 teodor Exp $
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

static float gistpenalty(GISTSTATE *giststate, int attno,
			GISTENTRY *key1, bool isNull1,
			GISTENTRY *key2, bool isNull2);


/*
 * static *S used for temrorary storage (saves stack and palloc() call)
 */

static int 		attrsizeS[INDEX_MAX_KEYS];
static Datum 	attrS[INDEX_MAX_KEYS];
static bool		isnullS[INDEX_MAX_KEYS];

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
gistnospace(Page page, IndexTuple *itvec, int len, OffsetNumber todelete)
{
	unsigned int size = 0, deleted = 0;
	int			i;

	for (i = 0; i < len; i++)
		size += IndexTupleSize(itvec[i]) + sizeof(ItemIdData);

	if ( todelete != InvalidOffsetNumber ) {
		IndexTuple itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, todelete));
		deleted = IndexTupleSize(itup) + sizeof(ItemIdData);
	}

	return (PageGetFreeSpace(page) + deleted < size);
}

bool
gistfitpage(IndexTuple *itvec, int len) {
	int i;
	Size size=0;

	for(i=0;i<len;i++)
		size += IndexTupleSize(itvec[i]) + sizeof(ItemIdData);

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
gistfillitupvec(IndexTuple *vec, int veclen, int *memlen) {
	char *ptr, *ret;
	int i;

	*memlen=0;
					
	for (i = 0; i < veclen; i++)
		*memlen += IndexTupleSize(vec[i]);

	ptr = ret = palloc(*memlen);

	for (i = 0; i < veclen; i++) { 
		memcpy(ptr, vec[i], IndexTupleSize(vec[i]));
		ptr += IndexTupleSize(vec[i]);
	}

	return (IndexTupleData*)ret;
}

/*
 * Make unions of keys in IndexTuple vector, return FALSE if itvec contains 
 * invalid tuple. Resulting Datums aren't compressed.
 */

static bool 
gistMakeUnionItVec(GISTSTATE *giststate, IndexTuple *itvec, int len, int startkey, 
					Datum *attr, bool *isnull, int *attrsize ) {
	int			i;
	GistEntryVector *evec;

	evec = (GistEntryVector *) palloc(((len == 1) ? 2 : len) * sizeof(GISTENTRY) + GEVHDRSZ);

	for (i = startkey; i < giststate->tupdesc->natts; i++) {
		int		j;

		evec->n = 0;

		for (j = 0; j < len; j++) {
			Datum 	datum;
			bool	IsNull;

			if (GistTupleIsInvalid(itvec[j]))
				return FALSE; /* signals that union with invalid tuple => result is invalid */

			datum = index_getattr(itvec[j], i + 1, giststate->tupdesc, &IsNull);
			if (IsNull)
				continue;

			gistdentryinit(giststate, i,
						   evec->vector + evec->n,
						   datum,
						   NULL, NULL, (OffsetNumber) 0,
						   ATTSIZE(datum, giststate->tupdesc, i + 1, IsNull),
						   FALSE, IsNull);
			evec->n++;
		}

		/* If this tuple vector was all NULLs, the union is NULL */
		if ( evec->n == 0 ) {
			attr[i] = (Datum) 0;
			attrsize[i] = (Datum) 0;
			isnull[i] = TRUE;
		} else {
			if (evec->n == 1) {
				evec->n = 2;
				evec->vector[1] = evec->vector[0];
			} 

			/* Make union and store in attr array */
			attr[i] = FunctionCall2(&giststate->unionFn[i],
								  PointerGetDatum(evec),
								  PointerGetDatum(attrsize + i));

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
	if ( !gistMakeUnionItVec(giststate, itvec, len, 0, attrS, isnullS, attrsizeS) ) 
			return gist_form_invalid_tuple(InvalidBlockNumber);

	return  gistFormTuple(giststate, r, attrS, attrsizeS, isnullS);
}

/* 
 * makes union of two key
 */
static void
gistMakeUnionKey( GISTSTATE *giststate, int attno,
					GISTENTRY	*entry1, bool isnull1, 	
					GISTENTRY	*entry2, bool isnull2,
					Datum	*dst, int *dstsize, bool *dstisnull ) {

	static char storage[ 2 * sizeof(GISTENTRY) + GEVHDRSZ ];
	GistEntryVector *evec = (GistEntryVector*)storage;

	evec->n = 2;

	if ( isnull1 && isnull2 ) {
		*dstisnull = TRUE;
		*dst = (Datum)0;
		*dstsize = 0;
	} else {
		if ( isnull1 == FALSE && isnull2 == FALSE ) {
			evec->vector[0] = *entry1;
			evec->vector[1] = *entry2;
		} else if ( isnull1 == FALSE ) {
			evec->vector[0] = *entry1;
			evec->vector[1] = *entry1;
		} else {
			evec->vector[0] = *entry2;
			evec->vector[1] = *entry2;
		}

		*dstisnull = FALSE;
		*dst = FunctionCall2(&giststate->unionFn[attno],
							  PointerGetDatum(evec),
							  PointerGetDatum(dstsize));
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
	IndexTuple	newtup = NULL;
	int			i;

	if (GistTupleIsInvalid(oldtup) || GistTupleIsInvalid(addtup))
		return gist_form_invalid_tuple(ItemPointerGetBlockNumber(&(oldtup->t_tid)));

	gistDeCompressAtt(giststate, r, oldtup, NULL,
					  (OffsetNumber) 0, oldentries, oldisnull);

	gistDeCompressAtt(giststate, r, addtup, NULL,
					  (OffsetNumber) 0, addentries, addisnull);

	for(i = 0; i < r->rd_att->natts; i++) {
		gistMakeUnionKey( giststate, i,
							oldentries + i, oldisnull[i],
							addentries + i, addisnull[i],
							attrS + i, attrsizeS + i, isnullS + i );

		if ( neednew )
			/* we already need new key, so we can skip check */
			continue;

		if ( isnullS[i] )
			/* union of key may be NULL if and only if both keys are NULL */
			continue;

		if ( !addisnull[i] ) {
			if ( oldisnull[i] )
				neednew = true;
			else {
				bool        result;

				FunctionCall3(&giststate->equalFn[i],
								oldentries[i].key,
								attrS[i],
								PointerGetDatum(&result));

				if (!result)
					neednew = true;
			}
		}
	}

	if (neednew)
	{
		/* need to update key */
		newtup = gistFormTuple(giststate, r, attrS, attrsizeS, isnullS);
		newtup->t_tid = oldtup->t_tid;
	}

	return newtup;
}

/*
 * Forms unions of subkeys after page split, but
 * uses only tuples aren't in groups of equalent tuples
 */
void 
gistunionsubkeyvec(GISTSTATE *giststate,  IndexTuple *itvec, 
							GistSplitVec *gsvp, int startkey) {
	IndexTuple	*cleanedItVec;
	int			i, cleanedLen=0;

	cleanedItVec = (IndexTuple*)palloc(sizeof(IndexTuple) * gsvp->len);

	for(i=0;i<gsvp->len;i++) {
		if ( gsvp->idgrp && gsvp->idgrp[gsvp->entries[i]])
			continue;

		cleanedItVec[cleanedLen++] = itvec[gsvp->entries[i] - 1];
	}

    gistMakeUnionItVec(giststate, cleanedItVec, cleanedLen, startkey, 
		gsvp->attr, gsvp->isnull, gsvp->attrsize);

	pfree( cleanedItVec );
}

/*
 * unions subkeys for after user picksplit over attno-1 column
 */
void
gistunionsubkey(GISTSTATE *giststate, IndexTuple *itvec, GIST_SPLITVEC *spl, int attno)
{
	GistSplitVec	gsvp;

	gsvp.idgrp = spl->spl_idgrp;

	gsvp.attrsize = spl->spl_lattrsize;
	gsvp.attr = spl->spl_lattr;
	gsvp.len = spl->spl_nleft;
	gsvp.entries = spl->spl_left;
	gsvp.isnull = spl->spl_lisnull;

	gistunionsubkeyvec(giststate, itvec, &gsvp, attno);

	gsvp.attrsize = spl->spl_rattrsize;
	gsvp.attr = spl->spl_rattr;
	gsvp.len = spl->spl_nright;
	gsvp.entries = spl->spl_right;
	gsvp.isnull = spl->spl_risnull;

	gistunionsubkeyvec(giststate, itvec, &gsvp, attno);
}

/*
 * find group in vector with equal value
 */
static int
gistfindgroup(GISTSTATE *giststate, GISTENTRY *valvec, GIST_SPLITVEC *spl, int attno)
{
	int			i;
	int			curid = 1;

	/*
	 * attno key is always not null (see gistSplitByKey), so we may not check for
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
			FunctionCall3(&giststate->equalFn[attno],
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
				FunctionCall3(&giststate->equalFn[attno],
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
static void
gistadjsubkey(Relation r,
			  IndexTuple *itup, /* contains compressed entry */
			  int len,
			  GIST_SPLITVEC *v,
			  GISTSTATE *giststate,
			  int attno)
{
	int			curlen;
	OffsetNumber *curwpos;
	GISTENTRY	entry,
				identry[INDEX_MAX_KEYS];
	float		lpenalty = 0,
				rpenalty = 0;
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

	/* add equivalent tuple */
	for (i = 0; i < len; i++)
	{
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
			for (j = attno+1; j < r->rd_att->natts; j++)
			{
				gistentryinit(entry, v->spl_lattr[j], r, NULL,
							  (OffsetNumber) 0, v->spl_lattrsize[j], FALSE);
				lpenalty = gistpenalty(giststate, j, &entry, v->spl_lisnull[j],
										&identry[j], isnull[j]);

				gistentryinit(entry, v->spl_rattr[j], r, NULL,
							  (OffsetNumber) 0, v->spl_rattrsize[j], FALSE);
				rpenalty = gistpenalty(giststate, j, &entry, v->spl_risnull[j],
										&identry[j], isnull[j]);

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
			v->spl_left[v->spl_nleft++] = i + 1;

			for (j = attno+1; j < r->rd_att->natts; j++)
			{
				gistentryinit(entry, v->spl_lattr[j], r, NULL,
							  (OffsetNumber) 0, v->spl_lattrsize[j], FALSE);
				gistMakeUnionKey( giststate, j,
							&entry, v->spl_lisnull[j],
							identry + j, isnull[j],
							v->spl_lattr + j, v->spl_lattrsize + j, v->spl_lisnull + j );
			}
		}
		else
		{
			v->spl_grpflag[v->spl_idgrp[i + 1]] |= RIGHT_ADDED;
			v->spl_right[v->spl_nright++] = i + 1;

			for (j = attno+1; j < r->rd_att->natts; j++)
			{
				gistentryinit(entry, v->spl_rattr[j], r, NULL,
							  (OffsetNumber) 0, v->spl_rattrsize[j], FALSE);
				gistMakeUnionKey( giststate, j,
							&entry, v->spl_risnull[j],
							identry + j, isnull[j],
							v->spl_rattr + j, v->spl_rattrsize + j, v->spl_risnull + j );
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
			usize = gistpenalty(giststate, j, &entry, IsNull,
								&identry[j], isnull[j]);

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
						   r, NULL, (OffsetNumber) 0,
						   (datumsize) ? datumsize[i] : -1, 
						   (datumsize) ? FALSE : TRUE, 
						   FALSE);
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

static float
gistpenalty(GISTSTATE *giststate, int attno,
			GISTENTRY *orig, bool isNullOrig,
			GISTENTRY *add, bool isNullAdd)
{
	float penalty = 0.0;

	if ( giststate->penaltyFn[attno].fn_strict==FALSE || ( isNullOrig == FALSE && isNullAdd == FALSE ) ) 
		FunctionCall3(&giststate->penaltyFn[attno],
					  PointerGetDatum(orig),
					  PointerGetDatum(add),
					  PointerGetDatum(&penalty));
	else if ( isNullOrig && isNullAdd )
		penalty = 0.0;
	else
		penalty = 1e10; /* try to prevent to mix null and non-null value */
	
	return penalty;
}

void
gistUserPicksplit(Relation r, GistEntryVector *entryvec, int attno, GIST_SPLITVEC *v,
				  IndexTuple *itup, int len, GISTSTATE *giststate)
{
	/*
	 * now let the user-defined picksplit function set up the split vector; in
	 * entryvec have no null value!!
	 */
	FunctionCall2(&giststate->picksplitFn[attno],
				  PointerGetDatum(entryvec),
				  PointerGetDatum(v));

	/* compatibility with old code */
	if (v->spl_left[v->spl_nleft - 1] == InvalidOffsetNumber)
		v->spl_left[v->spl_nleft - 1] = (OffsetNumber) (entryvec->n - 1);
	if (v->spl_right[v->spl_nright - 1] == InvalidOffsetNumber)
		v->spl_right[v->spl_nright - 1] = (OffsetNumber) (entryvec->n - 1);

	v->spl_lattr[attno] = v->spl_ldatum;
	v->spl_rattr[attno] = v->spl_rdatum;
	v->spl_lisnull[attno] = false;
	v->spl_risnull[attno] = false;

	/*
	 * if index is multikey, then we must to try get smaller bounding box for
	 * subkey(s)
	 */
	if (giststate->tupdesc->natts > 1 && attno+1 != giststate->tupdesc->natts)
	{
		int			MaxGrpId;

		v->spl_idgrp = (int *) palloc0(sizeof(int) * entryvec->n);
		v->spl_grpflag = (char *) palloc0(sizeof(char) * entryvec->n);
		v->spl_ngrp = (int *) palloc(sizeof(int) * entryvec->n);

		MaxGrpId = gistfindgroup(giststate, entryvec->vector, v, attno);

		/* form union of sub keys for each page (l,p) */
		gistunionsubkey(giststate, itup, v, attno + 1);

		/*
		 * if possible, we insert equivalent tuples with control by penalty
		 * for a subkey(s)
		 */
		if (MaxGrpId > 1)
			gistadjsubkey(r, itup, len, v, giststate, attno);
	}
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
	opaque->flags = f;
	opaque->rightlink = InvalidBlockNumber;
	/* page was already zeroed by PageInit, so this is not needed: */
	/* memset(&(opaque->nsn), 0, sizeof(GistNSN)); */
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
	 * page header or is all-zero.	We have to defend against the all-zero
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
	if (((PageHeader) (page))->pd_special !=
		(BLCKSZ - MAXALIGN(sizeof(GISTPageOpaqueData))))
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
		BlockNumber blkno = GetFreeIndexPage(&r->rd_node);

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
