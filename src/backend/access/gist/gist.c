/*-------------------------------------------------------------------------
 *
 * gist.c
 *	  interface routines for the postgres GiST index access method.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/gist/gist.c,v 1.105.4.1 2005/08/30 08:36:52 teodor Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist.h"
#include "access/gistscan.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "miscadmin.h"


#undef GIST_PAGEADDITEM

#define ATTSIZE( datum, TupDesc, i, isnull ) \
	( \
		( isnull ) ? 0 : \
			att_addlength(0, (TupDesc)->attrs[(i)-1]->attlen, (datum)) \
	)

/* result's status */
#define INSERTED	0x01
#define SPLITED		0x02

/* group flags ( in gistSplit ) */
#define LEFT_ADDED	0x01
#define RIGHT_ADDED 0x02
#define BOTH_ADDED	( LEFT_ADDED | RIGHT_ADDED )

/*
 * This defines only for shorter code, used in gistgetadjusted
 * and gistadjsubkey only
 */
#define FILLITEM(evp, isnullkey, okey, okeyb, rkey, rkeyb)	 do { \
		if ( isnullkey ) {										  \
				gistentryinit((evp), rkey, r, (Page) NULL ,		  \
						(OffsetNumber) 0, rkeyb, FALSE);		  \
		} else {												  \
				gistentryinit((evp), okey, r, (Page) NULL,		  \
						(OffsetNumber) 0, okeyb, FALSE);		  \
		}														  \
} while(0)

#define FILLEV(isnull1, key1, key1b, isnull2, key2, key2b) do { \
	FILLITEM(*ev0p, isnull1, key1, key1b, key2, key2b);		\
	FILLITEM(*ev1p, isnull2, key2, key2b, key1, key1b);		\
} while(0);

/* Working state for gistbuild and its callback */
typedef struct
{
	GISTSTATE	giststate;
	int			numindexattrs;
	double		indtuples;
} GISTBuildState;


/* non-export function prototypes */
static void gistbuildCallback(Relation index,
				  HeapTuple htup,
				  Datum *attdata,
				  char *nulls,
				  bool tupleIsAlive,
				  void *state);
static void gistdoinsert(Relation r,
			 IndexTuple itup,
			 InsertIndexResult *res,
			 GISTSTATE *GISTstate);
static int gistlayerinsert(Relation r, BlockNumber blkno,
				IndexTuple **itup,
				int *len,
				InsertIndexResult *res,
				GISTSTATE *giststate);
static OffsetNumber gistwritebuffer(Relation r,
				Page page,
				IndexTuple *itup,
				int len,
				OffsetNumber off);
static int gistnospace(Page page,
			IndexTuple *itvec, int len);
static IndexTuple *gistreadbuffer(Buffer buffer, int *len);
static IndexTuple *gistjoinvector(
			   IndexTuple *itvec, int *len,
			   IndexTuple *additvec, int addlen);
static IndexTuple gistunion(Relation r, IndexTuple *itvec,
		  int len, GISTSTATE *giststate);

static IndexTuple gistgetadjusted(Relation r,
				IndexTuple oldtup,
				IndexTuple addtup,
				GISTSTATE *giststate);
static int gistfindgroup(GISTSTATE *giststate,
			  GISTENTRY *valvec, GIST_SPLITVEC *spl);
static void gistadjsubkey(Relation r,
			  IndexTuple *itup, int *len,
			  GIST_SPLITVEC *v,
			  GISTSTATE *giststate);
static IndexTuple gistFormTuple(GISTSTATE *giststate,
			Relation r, Datum attdata[], int datumsize[], bool isnull[]);
static IndexTuple *gistSplit(Relation r,
		  Buffer buffer,
		  IndexTuple *itup,
		  int *len,
		  GISTSTATE *giststate,
		  InsertIndexResult *res);
static void gistnewroot(Relation r,
			IndexTuple *itup, int len);
static void GISTInitBuffer(Buffer b, uint32 f);
static OffsetNumber gistchoose(Relation r, Page p,
		   IndexTuple it,
		   GISTSTATE *giststate);
static void gistdelete(Relation r, ItemPointer tid);

#ifdef GIST_PAGEADDITEM
static IndexTuple gist_tuple_replacekey(Relation r,
					  GISTENTRY entry, IndexTuple t);
#endif
static void gistcentryinit(GISTSTATE *giststate, int nkey,
			   GISTENTRY *e, Datum k,
			   Relation r, Page pg,
			   OffsetNumber o, int b, bool l, bool isNull);
static void gistDeCompressAtt(GISTSTATE *giststate, Relation r,
				  IndexTuple tuple, Page p, OffsetNumber o,
				  GISTENTRY attdata[], bool decompvec[], bool isnull[]);
static void gistFreeAtt(Relation r, GISTENTRY attdata[], bool decompvec[]);
static void gistpenalty(GISTSTATE *giststate, int attno,
			GISTENTRY *key1, bool isNull1,
			GISTENTRY *key2, bool isNull2,
			float *penalty);

#undef GISTDEBUG

#ifdef GISTDEBUG
static void gist_dumptree(Relation r, int level, BlockNumber blk, OffsetNumber coff);
#endif

/*
 * routine to build an index.  Basically calls insert over and over
 */
Datum
gistbuild(PG_FUNCTION_ARGS)
{
	Relation	heap = (Relation) PG_GETARG_POINTER(0);
	Relation	index = (Relation) PG_GETARG_POINTER(1);
	IndexInfo  *indexInfo = (IndexInfo *) PG_GETARG_POINTER(2);
	double		reltuples;
	GISTBuildState buildstate;
	Buffer		buffer;

	/* no locking is needed */

	initGISTstate(&buildstate.giststate, index);

	/*
	 * We expect to be called exactly once for any index relation. If
	 * that's not the case, big trouble's what we have.
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/* initialize the root page */
	buffer = ReadBuffer(index, P_NEW);
	GISTInitBuffer(buffer, F_LEAF);
	WriteBuffer(buffer);

	/* build the index */
	buildstate.numindexattrs = indexInfo->ii_NumIndexAttrs;
	buildstate.indtuples = 0;

	/* do the heap scan */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo,
								gistbuildCallback, (void *) &buildstate);

	/* okay, all heap tuples are indexed */

	/*
	 * Since we just counted the tuples in the heap, we update its stats
	 * in pg_class to guarantee that the planner takes advantage of the
	 * index we just created.  But, only update statistics during normal
	 * index definitions, not for indices on system catalogs created
	 * during bootstrap processing.  We must close the relations before
	 * updating statistics to guarantee that the relcache entries are
	 * flushed when we increment the command counter in UpdateStats(). But
	 * we do not release any locks on the relations; those will be held
	 * until end of transaction.
	 */
	if (IsNormalProcessingMode())
	{
		Oid			hrelid = RelationGetRelid(heap);
		Oid			irelid = RelationGetRelid(index);

		heap_close(heap, NoLock);
		index_close(index);
		UpdateStats(hrelid, reltuples);
		UpdateStats(irelid, buildstate.indtuples);
	}

	freeGISTstate(&buildstate.giststate);
#ifdef GISTDEBUG
	gist_dumptree(index, 0, GISTP_ROOT, 0);
#endif

	PG_RETURN_VOID();
}

/*
 * Per-tuple callback from IndexBuildHeapScan
 */
static void
gistbuildCallback(Relation index,
				  HeapTuple htup,
				  Datum *attdata,
				  char *nulls,
				  bool tupleIsAlive,
				  void *state)
{
	GISTBuildState *buildstate = (GISTBuildState *) state;
	IndexTuple	itup;
	bool		compvec[INDEX_MAX_KEYS];
	GISTENTRY	tmpcentry;
	int			i;

	/* GiST cannot index tuples with leading NULLs */
	if (nulls[0] == 'n')
		return;

	/* immediately compress keys to normalize */
	for (i = 0; i < buildstate->numindexattrs; i++)
	{
		if (nulls[i] == 'n')
		{
			attdata[i] = (Datum) 0;
			compvec[i] = FALSE;
		}
		else
		{
			gistcentryinit(&buildstate->giststate, i, &tmpcentry, attdata[i],
						   (Relation) NULL, (Page) NULL, (OffsetNumber) 0,
						 -1 /* size is currently bogus */ , TRUE, FALSE);
			if (attdata[i] != tmpcentry.key &&
				!(isAttByVal(&buildstate->giststate, i)))
				compvec[i] = TRUE;
			else
				compvec[i] = FALSE;
			attdata[i] = tmpcentry.key;
		}
	}

	/* form an index tuple and point it at the heap tuple */
	itup = index_formtuple(buildstate->giststate.tupdesc, attdata, nulls);
	itup->t_tid = htup->t_self;

	/*
	 * Since we already have the index relation locked, we call
	 * gistdoinsert directly.  Normal access method calls dispatch through
	 * gistinsert, which locks the relation for write.	This is the right
	 * thing to do if you're inserting single tups, but not when you're
	 * initializing the whole index at once.
	 */
	gistdoinsert(index, itup, NULL, &buildstate->giststate);

	buildstate->indtuples += 1;

	for (i = 0; i < buildstate->numindexattrs; i++)
		if (compvec[i])
			pfree(DatumGetPointer(attdata[i]));

	pfree(itup);
}

/*
 *	gistinsert -- wrapper for GiST tuple insertion.
 *
 *	  This is the public interface routine for tuple insertion in GiSTs.
 *	  It doesn't do any work; just locks the relation and passes the buck.
 */
Datum
gistinsert(PG_FUNCTION_ARGS)
{
	Relation	r = (Relation) PG_GETARG_POINTER(0);
	Datum	   *datum = (Datum *) PG_GETARG_POINTER(1);
	char	   *nulls = (char *) PG_GETARG_POINTER(2);
	ItemPointer ht_ctid = (ItemPointer) PG_GETARG_POINTER(3);

#ifdef NOT_USED
	Relation	heapRel = (Relation) PG_GETARG_POINTER(4);
	bool		checkUnique = PG_GETARG_BOOL(5);
#endif
	InsertIndexResult res;
	IndexTuple	itup;
	GISTSTATE	giststate;
	GISTENTRY	tmpentry;
	int			i;
	bool		compvec[INDEX_MAX_KEYS];

	/*
	 * Since GIST is not marked "amconcurrent" in pg_am, caller should
	 * have acquired exclusive lock on index relation.	We need no locking
	 * here.
	 */

	/* GiST cannot index tuples with leading NULLs */
	if (nulls[0] == 'n')
	{
		res = NULL;
		PG_RETURN_POINTER(res);
	}

	initGISTstate(&giststate, r);

	/* immediately compress keys to normalize */
	for (i = 0; i < r->rd_att->natts; i++)
	{
		if (nulls[i] == 'n')
		{
			datum[i] = (Datum) 0;
			compvec[i] = FALSE;
		}
		else
		{
			gistcentryinit(&giststate, i, &tmpentry, datum[i],
						   (Relation) NULL, (Page) NULL, (OffsetNumber) 0,
						 -1 /* size is currently bogus */ , TRUE, FALSE);
			if (datum[i] != tmpentry.key && !(isAttByVal(&giststate, i)))
				compvec[i] = TRUE;
			else
				compvec[i] = FALSE;
			datum[i] = tmpentry.key;
		}
	}
	itup = index_formtuple(giststate.tupdesc, datum, nulls);
	itup->t_tid = *ht_ctid;

	res = (InsertIndexResult) palloc(sizeof(InsertIndexResultData));
	gistdoinsert(r, itup, &res, &giststate);

	for (i = 0; i < r->rd_att->natts; i++)
		if (compvec[i] == TRUE)
			pfree(DatumGetPointer(datum[i]));
	pfree(itup);
	freeGISTstate(&giststate);

	PG_RETURN_POINTER(res);
}

#ifdef GIST_PAGEADDITEM
/*
 * Take a compressed entry, and install it on a page.  Since we now know
 * where the entry will live, we decompress it and recompress it using
 * that knowledge (some compression routines may want to fish around
 * on the page, for example, or do something special for leaf nodes.)
 */
static OffsetNumber
gistPageAddItem(GISTSTATE *giststate,
				Relation r,
				Page page,
				Item item,
				Size size,
				OffsetNumber offsetNumber,
				ItemIdFlags flags,
				GISTENTRY *dentry,
				IndexTuple *newtup)
{
	GISTENTRY	tmpcentry;
	IndexTuple	itup = (IndexTuple) item;
	OffsetNumber retval;
	Datum		datum;
	bool		IsNull;

	/*
	 * recompress the item given that we now know the exact page and
	 * offset for insertion
	 */
	datum = index_getattr(itup, 1, r->rd_att, &IsNull);
	gistdentryinit(giststate, 0, dentry, datum,
				   (Relation) 0, (Page) 0,
				   (OffsetNumber) InvalidOffsetNumber,
				   ATTSIZE(datum, r, 1, IsNull),
				   FALSE, IsNull);
	gistcentryinit(giststate, 0, &tmpcentry, dentry->key, r, page,
				   offsetNumber, dentry->bytes, FALSE);
	*newtup = gist_tuple_replacekey(r, tmpcentry, itup);
	retval = PageAddItem(page, (Item) *newtup, IndexTupleSize(*newtup),
						 offsetNumber, flags);
	if (retval == InvalidOffsetNumber)
		elog(ERROR, "failed to add index item to \"%s\"",
			 RelationGetRelationName(r));
	/* be tidy */
	if (DatumGetPointer(tmpcentry.key) != NULL &&
		tmpcentry.key != dentry->key &&
		tmpcentry.key != datum)
		pfree(DatumGetPointer(tmpcentry.key));
	return (retval);
}
#endif

static void
gistdoinsert(Relation r,
			 IndexTuple itup,
			 InsertIndexResult *res,
			 GISTSTATE *giststate)
{
	IndexTuple *instup;
	int			i,
				ret,
				len = 1;

	instup = (IndexTuple *) palloc(sizeof(IndexTuple));
	instup[0] = (IndexTuple) palloc(IndexTupleSize(itup));
	memcpy(instup[0], itup, IndexTupleSize(itup));

	ret = gistlayerinsert(r, GISTP_ROOT, &instup, &len, res, giststate);
	if (ret & SPLITED)
		gistnewroot(r, instup, len);

	for (i = 0; i < len; i++)
		pfree(instup[i]);
	pfree(instup);
}

static int
gistlayerinsert(Relation r, BlockNumber blkno,
				IndexTuple **itup,		/* in - out, has compressed entry */
				int *len,		/* in - out */
				InsertIndexResult *res, /* out */
				GISTSTATE *giststate)
{
	Buffer		buffer;
	Page		page;
	OffsetNumber child;
	int			ret;
	GISTPageOpaque opaque;

	buffer = ReadBuffer(r, blkno);
	page = (Page) BufferGetPage(buffer);
	opaque = (GISTPageOpaque) PageGetSpecialPointer(page);

	if (!(opaque->flags & F_LEAF))
	{
		/* internal page, so we must walk on tree */
		/* len IS equal 1 */
		ItemId		iid;
		BlockNumber nblkno;
		ItemPointerData oldtid;
		IndexTuple	oldtup;

		child = gistchoose(r, page, *(*itup), giststate);
		iid = PageGetItemId(page, child);
		oldtup = (IndexTuple) PageGetItem(page, iid);
		nblkno = ItemPointerGetBlockNumber(&(oldtup->t_tid));

		/*
		 * After this call: 1. if child page was splited, then itup
		 * contains keys for each page 2. if  child page wasn't splited,
		 * then itup contains additional for adjustment of current key
		 */
		ret = gistlayerinsert(r, nblkno, itup, len, res, giststate);

		/* nothing inserted in child */
		if (!(ret & INSERTED))
		{
			ReleaseBuffer(buffer);
			return 0x00;
		}

		/* child does not splited */
		if (!(ret & SPLITED))
		{
			IndexTuple	newtup = gistgetadjusted(r, oldtup, (*itup)[0], giststate);

			if (!newtup)
			{
				/* not need to update key */
				ReleaseBuffer(buffer);
				return 0x00;
			}

			pfree((*itup)[0]);	/* !!! */
			(*itup)[0] = newtup;
		}

		/* key is modified, so old version must be deleted */
		ItemPointerSet(&oldtid, blkno, child);
		gistdelete(r, &oldtid);

		/*
		 * if child was splitted, new key for child will be inserted in
		 * the end list of child, so we must say to any scans that page is
		 * changed beginning from 'child' offset
		 */
		if (ret & SPLITED)
			gistadjscans(r, GISTOP_SPLIT, nblkno, FirstOffsetNumber);
	}

	ret = INSERTED;

	if (gistnospace(page, (*itup), *len))
	{
		/* no space for insertion */
		IndexTuple *itvec,
				   *newitup;
		int			tlen,
					oldlen;

		ret |= SPLITED;
		itvec = gistreadbuffer(buffer, &tlen);
		itvec = gistjoinvector(itvec, &tlen, (*itup), *len);
		oldlen = *len;
		newitup = gistSplit(r, buffer, itvec, &tlen, giststate,
							(opaque->flags & F_LEAF) ? res : NULL);		/* res only for
																		 * inserting in leaf */
		ReleaseBuffer(buffer);
		do
			pfree((*itup)[oldlen - 1]);
		while ((--oldlen) > 0);
		pfree((*itup));
		pfree(itvec);
		*itup = newitup;
		*len = tlen;			/* now tlen >= 2 */
	}
	else
	{
		/* enogth space */
		OffsetNumber off,
					l;

		off = (PageIsEmpty(page)) ?
			FirstOffsetNumber
			:
			OffsetNumberNext(PageGetMaxOffsetNumber(page));
		l = gistwritebuffer(r, page, (*itup), *len, off);
		WriteBuffer(buffer);

		/*
		 * set res if insert into leaf page, in this case, len = 1 always
		 */
		if (res && (opaque->flags & F_LEAF))
			ItemPointerSet(&((*res)->pointerData), blkno, l);

		if (*len > 1)
		{						/* previous insert ret & SPLITED != 0 */
			int			i;

			/*
			 * child was splited, so we must form union for insertion in
			 * parent
			 */
			IndexTuple	newtup = gistunion(r, (*itup), *len, giststate);

			ItemPointerSet(&(newtup->t_tid), blkno, 1);

			for (i = 0; i < *len; i++)
				pfree((*itup)[i]);
			(*itup)[0] = newtup;
			*len = 1;
		}
	}

	return ret;
}

/*
 * Write itup vector to page, has no control of free space
 */
static OffsetNumber
gistwritebuffer(Relation r, Page page, IndexTuple *itup,
				int len, OffsetNumber off)
{
	OffsetNumber l = InvalidOffsetNumber;
	int			i;

#ifdef GIST_PAGEADDITEM
	GISTENTRY	tmpdentry;
	IndexTuple	newtup;
	bool		IsNull;
#endif
	for (i = 0; i < len; i++)
	{
#ifdef GIST_PAGEADDITEM
		l = gistPageAddItem(giststate, r, page,
							(Item) itup[i], IndexTupleSize(itup[i]),
							off, LP_USED, &tmpdentry, &newtup);
		off = OffsetNumberNext(off);
		if (DatumGetPointer(tmpdentry.key) != NULL &&
		  tmpdentry.key != index_getattr(itup[i], 1, r->rd_att, &IsNull))
			pfree(DatumGetPointer(tmpdentry.key));
		if (itup[i] != newtup)
			pfree(newtup);
#else
		l = PageAddItem(page, (Item) itup[i], IndexTupleSize(itup[i]),
						off, LP_USED);
		if (l == InvalidOffsetNumber)
			elog(ERROR, "failed to add index item to \"%s\"",
				 RelationGetRelationName(r));
#endif
	}
	return l;
}

/*
 * Check space for itup vector on page
 */
static int
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
static IndexTuple *
gistreadbuffer(Buffer buffer, int *len /* out */ )
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
static IndexTuple *
gistjoinvector(IndexTuple *itvec, int *len, IndexTuple *additvec, int addlen)
{
	itvec = (IndexTuple *) repalloc((void *) itvec, sizeof(IndexTuple) * ((*len) + addlen));
	memmove(&itvec[*len], additvec, sizeof(IndexTuple) * addlen);
	*len += addlen;
	return itvec;
}

/*
 * return union of itup vector
 */
static IndexTuple
gistunion(Relation r, IndexTuple *itvec, int len, GISTSTATE *giststate)
{
	Datum		attr[INDEX_MAX_KEYS];
	bool		whatfree[INDEX_MAX_KEYS];
	char		isnull[INDEX_MAX_KEYS];
	char	   *storage;
	bytea	   *evec;
	Datum		datum;
	int			datumsize,
				i,
				j;
	GISTENTRY	centry[INDEX_MAX_KEYS];
	bool	   *needfree;
	IndexTuple	newtup;
	bool		IsNull;
	int			reallen;

	needfree = (bool *) palloc(((len == 1) ? 2 : len) * sizeof(bool));
	/* workaround for 64-bit: ensure GISTENTRY array is maxaligned */
	storage = (char *) palloc(((len == 1) ? 2 : len) * sizeof(GISTENTRY) + MAXALIGN(VARHDRSZ));
	evec = (bytea *) (storage + MAXALIGN(VARHDRSZ) - VARHDRSZ);

	for (j = 0; j < r->rd_att->natts; j++)
	{
		reallen = 0;
		for (i = 0; i < len; i++)
		{
			datum = index_getattr(itvec[i], j + 1, giststate->tupdesc, &IsNull);
			if (IsNull)
				continue;

			gistdentryinit(giststate, j,
						   &((GISTENTRY *) VARDATA(evec))[reallen],
						   datum,
					   (Relation) NULL, (Page) NULL, (OffsetNumber) NULL,
						   ATTSIZE(datum, giststate->tupdesc, j + 1, IsNull), FALSE, IsNull);
			if ((!isAttByVal(giststate, j)) &&
				((GISTENTRY *) VARDATA(evec))[reallen].key != datum)
				needfree[reallen] = TRUE;
			else
				needfree[reallen] = FALSE;
			reallen++;
		}

		if (reallen == 0)
		{
			attr[j] = (Datum) 0;
			isnull[j] = 'n';
			whatfree[j] = FALSE;
		}
		else
		{
			if (reallen == 1)
			{
				VARATT_SIZEP(evec) = 2 * sizeof(GISTENTRY) + VARHDRSZ;
				gistentryinit(((GISTENTRY *) VARDATA(evec))[1],
					((GISTENTRY *) VARDATA(evec))[0].key, r, (Page) NULL,
							  (OffsetNumber) 0, ((GISTENTRY *) VARDATA(evec))[0].bytes, FALSE);

			}
			else
				VARATT_SIZEP(evec) = reallen * sizeof(GISTENTRY) + VARHDRSZ;
			datum = FunctionCall2(&giststate->unionFn[j],
								  PointerGetDatum(evec),
								  PointerGetDatum(&datumsize));

			for (i = 0; i < reallen; i++)
				if (needfree[i])
					pfree(DatumGetPointer(((GISTENTRY *) VARDATA(evec))[i].key));

			gistcentryinit(giststate, j, &centry[j], datum,
					   (Relation) NULL, (Page) NULL, (OffsetNumber) NULL,
						   datumsize, FALSE, FALSE);
			isnull[j] = ' ';
			attr[j] = centry[j].key;
			if (!isAttByVal(giststate, j))
			{
				whatfree[j] = TRUE;
				if (centry[j].key != datum)
					pfree(DatumGetPointer(datum));
			}
			else
				whatfree[j] = FALSE;
		}
	}

	pfree(storage);				/* pfree(evec); */
	pfree(needfree);

	newtup = (IndexTuple) index_formtuple(giststate->tupdesc, attr, isnull);
	for (j = 0; j < r->rd_att->natts; j++)
		if (whatfree[j])
			pfree(DatumGetPointer(attr[j]));

	return newtup;
}


/*
 * Forms union of oldtup and addtup, if union == oldtup then return NULL
 */
static IndexTuple
gistgetadjusted(Relation r, IndexTuple oldtup, IndexTuple addtup, GISTSTATE *giststate)
{
	bytea	   *evec;
	Datum		datum;
	int			datumsize;
	bool		result,
				neednew = false;
	char		isnull[INDEX_MAX_KEYS],
				whatfree[INDEX_MAX_KEYS];
	Datum		attr[INDEX_MAX_KEYS];
	GISTENTRY	centry[INDEX_MAX_KEYS],
				oldatt[INDEX_MAX_KEYS],
				addatt[INDEX_MAX_KEYS],
			   *ev0p,
			   *ev1p;
	bool		olddec[INDEX_MAX_KEYS],
				adddec[INDEX_MAX_KEYS];
	bool		oldisnull[INDEX_MAX_KEYS],
				addisnull[INDEX_MAX_KEYS];
	IndexTuple	newtup = NULL;
	char	   *storage;
	int			j;

	/* workaround for 64-bit: ensure GISTENTRY array is maxaligned */
	storage = (char *) palloc(2 * sizeof(GISTENTRY) + MAXALIGN(VARHDRSZ));
	evec = (bytea *) (storage + MAXALIGN(VARHDRSZ) - VARHDRSZ);
	VARATT_SIZEP(evec) = 2 * sizeof(GISTENTRY) + VARHDRSZ;
	ev0p = &((GISTENTRY *) VARDATA(evec))[0];
	ev1p = &((GISTENTRY *) VARDATA(evec))[1];

	gistDeCompressAtt(giststate, r, oldtup, (Page) NULL,
					  (OffsetNumber) 0, oldatt, olddec, oldisnull);

	gistDeCompressAtt(giststate, r, addtup, (Page) NULL,
					  (OffsetNumber) 0, addatt, adddec, addisnull);


	for (j = 0; j < r->rd_att->natts; j++)
	{
		if (oldisnull[j] && addisnull[j])
		{
			isnull[j] = 'n';
			attr[j] = (Datum) 0;
			whatfree[j] = FALSE;
		}
		else
		{
			FILLEV(
				   oldisnull[j], oldatt[j].key, oldatt[j].bytes,
				   addisnull[j], addatt[j].key, addatt[j].bytes
				);

			datum = FunctionCall2(&giststate->unionFn[j],
								  PointerGetDatum(evec),
								  PointerGetDatum(&datumsize));

			if (oldisnull[j] || addisnull[j])
			{
				if (oldisnull[j])
					neednew = true;
			}
			else
			{
				FunctionCall3(&giststate->equalFn[j],
							  ev0p->key,
							  datum,
							  PointerGetDatum(&result));

				if (!result)
					neednew = true;
			}

			if (olddec[j])
				pfree(DatumGetPointer(oldatt[j].key));
			if (adddec[j])
				pfree(DatumGetPointer(addatt[j].key));

			gistcentryinit(giststate, j, &centry[j], datum,
					   (Relation) NULL, (Page) NULL, (OffsetNumber) NULL,
						   datumsize, FALSE, FALSE);

			isnull[j] = ' ';
			attr[j] = centry[j].key;
			if ((!isAttByVal(giststate, j)))
			{
				whatfree[j] = TRUE;
				if (centry[j].key != datum)
					pfree(DatumGetPointer(datum));
			}
			else
				whatfree[j] = FALSE;
		}
	}
	pfree(storage);				/* pfree(evec); */

	if (neednew)
	{
		/* need to update key */
		newtup = (IndexTuple) index_formtuple(giststate->tupdesc, attr, isnull);
		newtup->t_tid = oldtup->t_tid;
	}

	for (j = 0; j < r->rd_att->natts; j++)
		if (whatfree[j])
			pfree(DatumGetPointer(attr[j]));

	return newtup;
}

static void
gistunionsubkey(Relation r, GISTSTATE *giststate, IndexTuple *itvec, GIST_SPLITVEC *spl)
{
	int			i,
				j,
				lr;
	Datum	   *attr;
	bool	   *needfree,
				IsNull;
	int			len,
			   *attrsize;
	OffsetNumber *entries;
	bytea	   *evec;
	char	   *storage;
	Datum		datum;
	int			datumsize;
	int			reallen;
	bool	   *isnull;

	for (lr = 0; lr <= 1; lr++)
	{
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

		needfree = (bool *) palloc(((len == 1) ? 2 : len) * sizeof(bool));
		/* workaround for 64-bit: ensure GISTENTRY array is maxaligned */
		storage = (char *) palloc(((len == 1) ? 2 : len) * sizeof(GISTENTRY) + MAXALIGN(VARHDRSZ));
		evec = (bytea *) (storage + MAXALIGN(VARHDRSZ) - VARHDRSZ);

		for (j = 1; j < r->rd_att->natts; j++)
		{
			reallen = 0;
			for (i = 0; i < len; i++)
			{
				if (spl->spl_idgrp[entries[i]])
					continue;
				datum = index_getattr(itvec[entries[i] - 1], j + 1,
									  giststate->tupdesc, &IsNull);
				if (IsNull)
					continue;
				gistdentryinit(giststate, j,
							   &((GISTENTRY *) VARDATA(evec))[reallen],
							   datum,
							   (Relation) NULL, (Page) NULL,
							   (OffsetNumber) NULL,
							   ATTSIZE(datum, giststate->tupdesc, j + 1, IsNull), FALSE, IsNull);
				if ((!isAttByVal(giststate, j)) &&
					((GISTENTRY *) VARDATA(evec))[reallen].key != datum)
					needfree[reallen] = TRUE;
				else
					needfree[reallen] = FALSE;
				reallen++;

			}
			if (reallen == 0)
			{
				datum = (Datum) 0;
				datumsize = 0;
				isnull[j] = true;
			}
			else
			{
				/*
				 * ((GISTENTRY *) VARDATA(evec))[0].bytes may be not
				 * defined, so form union with itself
				 */
				if (reallen == 1)
				{
					VARATT_SIZEP(evec) = 2 * sizeof(GISTENTRY) + VARHDRSZ;
					memcpy((void *) &((GISTENTRY *) VARDATA(evec))[1],
						   (void *) &((GISTENTRY *) VARDATA(evec))[0],
						   sizeof(GISTENTRY));
				}
				else
					VARATT_SIZEP(evec) = reallen * sizeof(GISTENTRY) + VARHDRSZ;
				datum = FunctionCall2(&giststate->unionFn[j],
									  PointerGetDatum(evec),
									  PointerGetDatum(&datumsize));
				isnull[j] = false;
			}

			for (i = 0; i < reallen; i++)
				if (needfree[i])
					pfree(DatumGetPointer(((GISTENTRY *) VARDATA(evec))[i].key));

			attr[j] = datum;
			attrsize[j] = datumsize;
		}
		pfree(storage);			/* pfree(evec); */
		pfree(needfree);
	}
}

/*
 * find group in vector with equial value
 */
static int
gistfindgroup(GISTSTATE *giststate, GISTENTRY *valvec, GIST_SPLITVEC *spl)
{
	int			i,
				j,
				len;
	int			curid = 1;
	bool		result;

	/*
	 * first key is always not null (see gistinsert), so we may not check
	 * for nulls
	 */

	for (i = 0; i < spl->spl_nleft; i++)
	{
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
			/* add current val to list of equial values */
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
 * Insert equivalent tuples to left or right page
 * with minimize penalty
 */
static void
gistadjsubkey(Relation r,
			  IndexTuple *itup, /* contains compressed entry */
			  int *len,
			  GIST_SPLITVEC *v,
			  GISTSTATE *giststate
)
{
	int			curlen;
	OffsetNumber *curwpos;
	bool		decfree[INDEX_MAX_KEYS];
	GISTENTRY	entry,
				identry[INDEX_MAX_KEYS],
			   *ev0p,
			   *ev1p;
	float		lpenalty,
				rpenalty;
	bytea	   *evec;
	char	   *storage;
	int			datumsize;
	bool		isnull[INDEX_MAX_KEYS];
	int			i,
				j;
	Datum		datum;

	/* clear vectors */
	curlen = v->spl_nleft;
	curwpos = v->spl_left;
	for (i = 0; i < v->spl_nleft; i++)
		if (v->spl_idgrp[v->spl_left[i]] == 0)
		{
			*curwpos = v->spl_left[i];
			curwpos++;
		}
		else
			curlen--;
	v->spl_nleft = curlen;

	curlen = v->spl_nright;
	curwpos = v->spl_right;
	for (i = 0; i < v->spl_nright; i++)
		if (v->spl_idgrp[v->spl_right[i]] == 0)
		{
			*curwpos = v->spl_right[i];
			curwpos++;
		}
		else
			curlen--;
	v->spl_nright = curlen;

	/* workaround for 64-bit: ensure GISTENTRY array is maxaligned */
	storage = (char *) palloc(2 * sizeof(GISTENTRY) + MAXALIGN(VARHDRSZ));
	evec = (bytea *) (storage + MAXALIGN(VARHDRSZ) - VARHDRSZ);
	VARATT_SIZEP(evec) = 2 * sizeof(GISTENTRY) + VARHDRSZ;
	ev0p = &((GISTENTRY *) VARDATA(evec))[0];
	ev1p = &((GISTENTRY *) VARDATA(evec))[1];

	/* add equivalent tuple */
	for (i = 0; i < *len; i++)
	{
		if (v->spl_idgrp[i + 1] == 0)	/* already inserted */
			continue;
		gistDeCompressAtt(giststate, r, itup[i], (Page) NULL, (OffsetNumber) 0,
						  identry, decfree, isnull);

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
				gistentryinit(entry, v->spl_lattr[j], r, (Page) NULL,
						   (OffsetNumber) 0, v->spl_lattrsize[j], FALSE);
				gistpenalty(giststate, j, &entry, v->spl_lisnull[j],
							&identry[j], isnull[j], &lpenalty);

				gistentryinit(entry, v->spl_rattr[j], r, (Page) NULL,
						   (OffsetNumber) 0, v->spl_rattrsize[j], FALSE);
				gistpenalty(giststate, j, &entry, v->spl_risnull[j],
							&identry[j], isnull[j], &rpenalty);

				if (lpenalty != rpenalty)
					break;
			}
		}
		/* add */
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
					FILLEV(
						   v->spl_lisnull[j], v->spl_lattr[j], v->spl_lattrsize[j],
						   isnull[j], identry[j].key, identry[j].bytes
						);

					datum = FunctionCall2(&giststate->unionFn[j],
										  PointerGetDatum(evec),
										  PointerGetDatum(&datumsize));

					if ((!isAttByVal(giststate, j)) && !v->spl_lisnull[j])
						pfree(DatumGetPointer(v->spl_lattr[j]));
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
					FILLEV(
						   v->spl_risnull[j], v->spl_rattr[j], v->spl_rattrsize[j],
						   isnull[j], identry[j].key, identry[j].bytes
						);

					datum = FunctionCall2(&giststate->unionFn[j],
										  PointerGetDatum(evec),
										  PointerGetDatum(&datumsize));

					if ((!isAttByVal(giststate, j)) && !v->spl_risnull[j])
						pfree(DatumGetPointer(v->spl_rattr[j]));

					v->spl_rattr[j] = datum;
					v->spl_rattrsize[j] = datumsize;
					v->spl_risnull[j] = false;
				}
			}

		}
		gistFreeAtt(r, identry, decfree);
	}
	pfree(storage);				/* pfree(evec); */
}

/*
 *	gistSplit -- split a page in the tree.
 */
static IndexTuple *
gistSplit(Relation r,
		  Buffer buffer,
		  IndexTuple *itup,		/* contains compressed entry */
		  int *len,
		  GISTSTATE *giststate,
		  InsertIndexResult *res)
{
	Page		p;
	Buffer		leftbuf,
				rightbuf;
	Page		left,
				right;
	IndexTuple *lvectup,
			   *rvectup,
			   *newtup;
	BlockNumber lbknum,
				rbknum;
	GISTPageOpaque opaque;
	GIST_SPLITVEC v;
	bytea	   *entryvec;
	char	   *storage;
	bool	   *decompvec;
	int			i,
				j,
				nlen;
	int			MaxGrpId = 1;
	Datum		datum;
	bool		IsNull;

	p = (Page) BufferGetPage(buffer);
	opaque = (GISTPageOpaque) PageGetSpecialPointer(p);

	/*
	 * The root of the tree is the first block in the relation.  If we're
	 * about to split the root, we need to do some hocus-pocus to enforce
	 * this guarantee.
	 */

	if (BufferGetBlockNumber(buffer) == GISTP_ROOT)
	{
		leftbuf = ReadBuffer(r, P_NEW);
		GISTInitBuffer(leftbuf, opaque->flags);
		lbknum = BufferGetBlockNumber(leftbuf);
		left = (Page) BufferGetPage(leftbuf);
	}
	else
	{
		leftbuf = buffer;
		IncrBufferRefCount(buffer);
		lbknum = BufferGetBlockNumber(buffer);
		left = (Page) PageGetTempPage(p, sizeof(GISTPageOpaqueData));
	}

	rightbuf = ReadBuffer(r, P_NEW);
	GISTInitBuffer(rightbuf, opaque->flags);
	rbknum = BufferGetBlockNumber(rightbuf);
	right = (Page) BufferGetPage(rightbuf);

	/* generate the item array */
	/* workaround for 64-bit: ensure GISTENTRY array is maxaligned */
	storage = palloc(MAXALIGN(VARHDRSZ) + (*len + 1) * sizeof(GISTENTRY));
	entryvec = (bytea *) (storage + MAXALIGN(VARHDRSZ) - VARHDRSZ);
	decompvec = (bool *) palloc((*len + 1) * sizeof(bool));
	VARATT_SIZEP(entryvec) = (*len + 1) * sizeof(GISTENTRY) + VARHDRSZ;
	for (i = 1; i <= *len; i++)
	{
		datum = index_getattr(itup[i - 1], 1, giststate->tupdesc, &IsNull);
		gistdentryinit(giststate, 0, &((GISTENTRY *) VARDATA(entryvec))[i],
					   datum, r, p, i,
		   ATTSIZE(datum, giststate->tupdesc, 1, IsNull), FALSE, IsNull);
		if ((!isAttByVal(giststate, 0)) && ((GISTENTRY *) VARDATA(entryvec))[i].key != datum)
			decompvec[i] = TRUE;
		else
			decompvec[i] = FALSE;
	}

	/*
	 * now let the user-defined picksplit function set up the split
	 * vector; in entryvec have no null value!!
	 */
	FunctionCall2(&giststate->picksplitFn[0],
				  PointerGetDatum(entryvec),
				  PointerGetDatum(&v));

	/* compatibility with old code */
	if (v.spl_left[v.spl_nleft - 1] == InvalidOffsetNumber)
		v.spl_left[v.spl_nleft - 1] = (OffsetNumber) *len;
	if (v.spl_right[v.spl_nright - 1] == InvalidOffsetNumber)
		v.spl_right[v.spl_nright - 1] = (OffsetNumber) *len;

	v.spl_lattr[0] = v.spl_ldatum;
	v.spl_rattr[0] = v.spl_rdatum;
	v.spl_lisnull[0] = false;
	v.spl_risnull[0] = false;

	/*
	 * if index is multikey, then we must to try get smaller bounding box
	 * for subkey(s)
	 */
	if (r->rd_att->natts > 1)
	{
		v.spl_idgrp = (int *) palloc0(sizeof(int) * (*len + 1));
		v.spl_grpflag = (char *) palloc0(sizeof(char) * (*len + 1));
		v.spl_ngrp = (int *) palloc(sizeof(int) * (*len + 1));

		MaxGrpId = gistfindgroup(giststate, (GISTENTRY *) VARDATA(entryvec), &v);

		/* form union of sub keys for each page (l,p) */
		gistunionsubkey(r, giststate, itup, &v);

		/*
		 * if possible, we insert equivalent tuples with control by
		 * penalty for a subkey(s)
		 */
		if (MaxGrpId > 1)
			gistadjsubkey(r, itup, len, &v, giststate);

		pfree(v.spl_idgrp);
		pfree(v.spl_grpflag);
		pfree(v.spl_ngrp);
	}

	/* clean up the entry vector: its keys need to be deleted, too */
	for (i = 1; i <= *len; i++)
		if (decompvec[i])
			pfree(DatumGetPointer(((GISTENTRY *) VARDATA(entryvec))[i].key));
	pfree(storage);				/* pfree(entryvec); */
	pfree(decompvec);

	/* form left and right vector */
	lvectup = (IndexTuple *) palloc(sizeof(IndexTuple) * v.spl_nleft);
	rvectup = (IndexTuple *) palloc(sizeof(IndexTuple) * v.spl_nright);

	for (i = 0; i < v.spl_nleft; i++)
		lvectup[i] = itup[v.spl_left[i] - 1];

	for (i = 0; i < v.spl_nright; i++)
		rvectup[i] = itup[v.spl_right[i] - 1];


	/* write on disk (may be need another split) */
	if (gistnospace(right, rvectup, v.spl_nright))
	{
		nlen = v.spl_nright;
		newtup = gistSplit(r, rightbuf, rvectup, &nlen, giststate,
			  (res && rvectup[nlen - 1] == itup[*len - 1]) ? res : NULL);
		ReleaseBuffer(rightbuf);
		for (j = 1; j < r->rd_att->natts; j++)
			if ((!isAttByVal(giststate, j)) && !v.spl_risnull[j])
				pfree(DatumGetPointer(v.spl_rattr[j]));
	}
	else
	{
		OffsetNumber l;

		l = gistwritebuffer(r, right, rvectup, v.spl_nright, FirstOffsetNumber);
		WriteBuffer(rightbuf);

		if (res)
			ItemPointerSet(&((*res)->pointerData), rbknum, l);

		nlen = 1;
		newtup = (IndexTuple *) palloc(sizeof(IndexTuple) * 1);
		newtup[0] = gistFormTuple(giststate, r, v.spl_rattr, v.spl_rattrsize, v.spl_risnull);
		ItemPointerSet(&(newtup[0]->t_tid), rbknum, 1);
	}


	if (gistnospace(left, lvectup, v.spl_nleft))
	{
		int			llen = v.spl_nleft;
		IndexTuple *lntup;

		lntup = gistSplit(r, leftbuf, lvectup, &llen, giststate,
			  (res && lvectup[llen - 1] == itup[*len - 1]) ? res : NULL);
		ReleaseBuffer(leftbuf);

		for (j = 1; j < r->rd_att->natts; j++)
			if ((!isAttByVal(giststate, j)) && !v.spl_lisnull[j])
				pfree(DatumGetPointer(v.spl_lattr[j]));

		newtup = gistjoinvector(newtup, &nlen, lntup, llen);
		pfree(lntup);
	}
	else
	{
		OffsetNumber l;

		l = gistwritebuffer(r, left, lvectup, v.spl_nleft, FirstOffsetNumber);
		if (BufferGetBlockNumber(buffer) != GISTP_ROOT)
			PageRestoreTempPage(left, p);

		WriteBuffer(leftbuf);

		if (res)
			ItemPointerSet(&((*res)->pointerData), lbknum, l);

		nlen += 1;
		newtup = (IndexTuple *) repalloc((void *) newtup, sizeof(IndexTuple) * nlen);
		newtup[nlen - 1] = gistFormTuple(giststate, r, v.spl_lattr, v.spl_lattrsize, v.spl_lisnull);
		ItemPointerSet(&(newtup[nlen - 1]->t_tid), lbknum, 1);
	}

	/* !!! pfree */
	pfree(rvectup);
	pfree(lvectup);
	pfree(v.spl_left);
	pfree(v.spl_right);

	*len = nlen;
	return newtup;
}

static void
gistnewroot(Relation r, IndexTuple *itup, int len)
{
	Buffer		b;
	Page		p;

	b = ReadBuffer(r, GISTP_ROOT);

	gistadjscans(r, GISTOP_SPLIT, GISTP_ROOT, FirstOffsetNumber);

	GISTInitBuffer(b, 0);
	p = BufferGetPage(b);

	gistwritebuffer(r, p, itup, len, FirstOffsetNumber);
	WriteBuffer(b);
}

static void
GISTInitBuffer(Buffer b, uint32 f)
{
	GISTPageOpaque opaque;
	Page		page;
	Size		pageSize;

	pageSize = BufferGetPageSize(b);

	page = BufferGetPage(b);

	PageInit(page, pageSize, sizeof(GISTPageOpaqueData));

	opaque = (GISTPageOpaque) PageGetSpecialPointer(page);
	opaque->flags = f;
}


/*
** find entry with lowest penalty
*/
static OffsetNumber
gistchoose(Relation r, Page p, IndexTuple it,	/* it has compressed entry */
		   GISTSTATE *giststate)
{
	OffsetNumber maxoff;
	OffsetNumber i;
	Datum		datum;
	float		usize;
	OffsetNumber which;
	float		sum_grow,
				which_grow[INDEX_MAX_KEYS];
	GISTENTRY	entry,
				identry[INDEX_MAX_KEYS];
	bool		IsNull,
				decompvec[INDEX_MAX_KEYS],
				isnull[INDEX_MAX_KEYS];
	int			j;

	maxoff = PageGetMaxOffsetNumber(p);
	*which_grow = -1.0;
	which = -1;
	sum_grow = 1;
	gistDeCompressAtt(giststate, r,
					  it, (Page) NULL, (OffsetNumber) 0,
					  identry, decompvec, isnull);

	for (i = FirstOffsetNumber; i <= maxoff && sum_grow; i = OffsetNumberNext(i))
	{
		IndexTuple	itup = (IndexTuple) PageGetItem(p, PageGetItemId(p, i));

		sum_grow = 0;
		for (j = 0; j < r->rd_att->natts; j++)
		{
			datum = index_getattr(itup, j + 1, giststate->tupdesc, &IsNull);
			gistdentryinit(giststate, j, &entry, datum, r, p, i, ATTSIZE(datum, giststate->tupdesc, j + 1, IsNull), FALSE, IsNull);
			gistpenalty(giststate, j, &entry, IsNull, &identry[j], isnull[j], &usize);

			if ((!isAttByVal(giststate, j)) && entry.key != datum)
				pfree(DatumGetPointer(entry.key));

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

	gistFreeAtt(r, identry, decompvec);
	return which;
}

void
gistfreestack(GISTSTACK *s)
{
	GISTSTACK  *p;

	while (s != (GISTSTACK *) NULL)
	{
		p = s->gs_parent;
		pfree(s);
		s = p;
	}
}


/*
 * Retail deletion of a single tuple.
 *
 * NB: this is no longer called externally, but is still needed by
 * gistlayerinsert().  That dependency will have to be fixed if GIST
 * is ever going to allow concurrent insertions.
 */
static void
gistdelete(Relation r, ItemPointer tid)
{
	BlockNumber blkno;
	OffsetNumber offnum;
	Buffer		buf;
	Page		page;

	/*
	 * Since GIST is not marked "amconcurrent" in pg_am, caller should
	 * have acquired exclusive lock on index relation.	We need no locking
	 * here.
	 */

	blkno = ItemPointerGetBlockNumber(tid);
	offnum = ItemPointerGetOffsetNumber(tid);

	/* adjust any scans that will be affected by this deletion */
	/* NB: this works only for scans in *this* backend! */
	gistadjscans(r, GISTOP_DEL, blkno, offnum);

	/* delete the index tuple */
	buf = ReadBuffer(r, blkno);
	page = BufferGetPage(buf);

	PageIndexTupleDelete(page, offnum);

	WriteBuffer(buf);
}

/*
 * Bulk deletion of all index entries pointing to a set of heap tuples.
 * The set of target tuples is specified via a callback routine that tells
 * whether any given heap tuple (identified by ItemPointer) is being deleted.
 *
 * Result: a palloc'd struct containing statistical info for VACUUM displays.
 */
Datum
gistbulkdelete(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback) PG_GETARG_POINTER(1);
	void	   *callback_state = (void *) PG_GETARG_POINTER(2);
	IndexBulkDeleteResult *result;
	BlockNumber num_pages;
	double		tuples_removed;
	double		num_index_tuples;
	IndexScanDesc iscan;

	tuples_removed = 0;
	num_index_tuples = 0;

	/*
	 * Since GIST is not marked "amconcurrent" in pg_am, caller should
	 * have acquired exclusive lock on index relation.	We need no locking
	 * here.
	 */

	/*
	 * XXX generic implementation --- should be improved!
	 */

	/* walk through the entire index */
	iscan = index_beginscan(NULL, rel, SnapshotAny, 0, (ScanKey) NULL);
	/* including killed tuples */
	iscan->ignore_killed_tuples = false;

	while (index_getnext_indexitem(iscan, ForwardScanDirection))
	{
		if (callback(&iscan->xs_ctup.t_self, callback_state))
		{
			ItemPointerData indextup = iscan->currentItemData;
			BlockNumber blkno;
			OffsetNumber offnum;
			Buffer		buf;
			Page		page;

			blkno = ItemPointerGetBlockNumber(&indextup);
			offnum = ItemPointerGetOffsetNumber(&indextup);

			/* adjust any scans that will be affected by this deletion */
			gistadjscans(rel, GISTOP_DEL, blkno, offnum);

			/* delete the index tuple */
			buf = ReadBuffer(rel, blkno);
			page = BufferGetPage(buf);

			PageIndexTupleDelete(page, offnum);

			WriteBuffer(buf);

			tuples_removed += 1;
		}
		else
			num_index_tuples += 1;
	}

	index_endscan(iscan);

	/* return statistics */
	num_pages = RelationGetNumberOfBlocks(rel);

	result = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	result->num_pages = num_pages;
	result->num_index_tuples = num_index_tuples;
	result->tuples_removed = tuples_removed;

	PG_RETURN_POINTER(result);
}


void
initGISTstate(GISTSTATE *giststate, Relation index)
{
	int			i;

	if (index->rd_att->natts > INDEX_MAX_KEYS)
		elog(ERROR, "numberOfAttributes %d > %d",
			 index->rd_att->natts, INDEX_MAX_KEYS);

	giststate->tupdesc = index->rd_att;

	for (i = 0; i < index->rd_att->natts; i++)
	{
		fmgr_info_copy(&(giststate->consistentFn[i]),
				   index_getprocinfo(index, i + 1, GIST_CONSISTENT_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(giststate->unionFn[i]),
					   index_getprocinfo(index, i + 1, GIST_UNION_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(giststate->compressFn[i]),
					 index_getprocinfo(index, i + 1, GIST_COMPRESS_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(giststate->decompressFn[i]),
				   index_getprocinfo(index, i + 1, GIST_DECOMPRESS_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(giststate->penaltyFn[i]),
					   index_getprocinfo(index, i + 1, GIST_PENALTY_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(giststate->picksplitFn[i]),
					index_getprocinfo(index, i + 1, GIST_PICKSPLIT_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(giststate->equalFn[i]),
					   index_getprocinfo(index, i + 1, GIST_EQUAL_PROC),
					   CurrentMemoryContext);
	}
}

void
freeGISTstate(GISTSTATE *giststate)
{
	/* no work */
}

#ifdef GIST_PAGEADDITEM
/*
** Given an IndexTuple to be inserted on a page, this routine replaces
** the key with another key, which may involve generating a new IndexTuple
** if the sizes don't match or if the null status changes.
**
** XXX this only works for a single-column index tuple!
*/
static IndexTuple
gist_tuple_replacekey(Relation r, GISTENTRY entry, IndexTuple t)
{
	bool		IsNull;
	Datum		datum = index_getattr(t, 1, r->rd_att, &IsNull);

	/*
	 * If new entry fits in index tuple, copy it in.  To avoid worrying
	 * about null-value bitmask, pass it off to the general
	 * index_formtuple routine if either the previous or new value is
	 * NULL.
	 */
	if (!IsNull && DatumGetPointer(entry.key) != NULL &&
		(Size) entry.bytes <= ATTSIZE(datum, r, 1, IsNull))
	{
		memcpy(DatumGetPointer(datum),
			   DatumGetPointer(entry.key),
			   entry.bytes);
		/* clear out old size */
		t->t_info &= ~INDEX_SIZE_MASK;
		/* or in new size */
		t->t_info |= MAXALIGN(entry.bytes + sizeof(IndexTupleData));

		return t;
	}
	else
	{
		/* generate a new index tuple for the compressed entry */
		TupleDesc	tupDesc = r->rd_att;
		IndexTuple	newtup;
		char		isnull;

		isnull = DatumGetPointer(entry.key) != NULL ? ' ' : 'n';
		newtup = (IndexTuple) index_formtuple(tupDesc,
											  &(entry.key),
											  &isnull);
		newtup->t_tid = t->t_tid;
		return newtup;
	}
}
#endif

/*
** initialize a GiST entry with a decompressed version of key
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
		{
			gistentryinit(*e, dep->key, dep->rel, dep->page, dep->offset,
						  dep->bytes, dep->leafkey);
			pfree(dep);
		}
	}
	else
		gistentryinit(*e, (Datum) 0, r, pg, o, 0, l);
}


/*
** initialize a GiST entry with a compressed version of key
*/
static void
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
		{
			gistentryinit(*e, cep->key, cep->rel, cep->page, cep->offset,
						  cep->bytes, cep->leafkey);
			pfree(cep);
		}
	}
	else
		gistentryinit(*e, (Datum) 0, r, pg, o, 0, l);
}

static IndexTuple
gistFormTuple(GISTSTATE *giststate, Relation r,
			  Datum attdata[], int datumsize[], bool isnull[])
{
	IndexTuple	tup;
	char		isnullchar[INDEX_MAX_KEYS];
	bool		whatfree[INDEX_MAX_KEYS];
	GISTENTRY	centry[INDEX_MAX_KEYS];
	Datum		compatt[INDEX_MAX_KEYS];
	int			j;

	for (j = 0; j < r->rd_att->natts; j++)
	{
		if (isnull[j])
		{
			isnullchar[j] = 'n';
			compatt[j] = (Datum) 0;
			whatfree[j] = FALSE;
		}
		else
		{
			gistcentryinit(giststate, j, &centry[j], attdata[j],
					   (Relation) NULL, (Page) NULL, (OffsetNumber) NULL,
						   datumsize[j], FALSE, FALSE);
			isnullchar[j] = ' ';
			compatt[j] = centry[j].key;
			if (!isAttByVal(giststate, j))
			{
				whatfree[j] = TRUE;
				if (centry[j].key != attdata[j])
					pfree(DatumGetPointer(attdata[j]));
			}
			else
				whatfree[j] = FALSE;
		}
	}

	tup = (IndexTuple) index_formtuple(giststate->tupdesc, compatt, isnullchar);
	for (j = 0; j < r->rd_att->natts; j++)
		if (whatfree[j])
			pfree(DatumGetPointer(compatt[j]));

	return tup;
}

static void
gistDeCompressAtt(GISTSTATE *giststate, Relation r, IndexTuple tuple, Page p,
	OffsetNumber o, GISTENTRY attdata[], bool decompvec[], bool isnull[])
{
	int			i;
	Datum		datum;

	for (i = 0; i < r->rd_att->natts; i++)
	{
		datum = index_getattr(tuple, i + 1, giststate->tupdesc, &isnull[i]);
		gistdentryinit(giststate, i, &attdata[i],
					   datum, r, p, o,
					   ATTSIZE(datum, giststate->tupdesc, i + 1, isnull[i]), FALSE, isnull[i]);
		if (isAttByVal(giststate, i))
			decompvec[i] = FALSE;
		else
		{
			if (attdata[i].key == datum || isnull[i])
				decompvec[i] = FALSE;
			else
				decompvec[i] = TRUE;
		}
	}
}

static void
gistFreeAtt(Relation r, GISTENTRY attdata[], bool decompvec[])
{
	int			i;

	for (i = 0; i < r->rd_att->natts; i++)
		if (decompvec[i])
			pfree(DatumGetPointer(attdata[i].key));
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

#ifdef GISTDEBUG
static void
gist_dumptree(Relation r, int level, BlockNumber blk, OffsetNumber coff)
{
	Buffer		buffer;
	Page		page;
	GISTPageOpaque opaque;
	IndexTuple	which;
	ItemId		iid;
	OffsetNumber i,
				maxoff;
	BlockNumber cblk;
	char	   *pred;

	pred = (char *) palloc(sizeof(char) * level + 1);
	MemSet(pred, '\t', level);
	pred[level] = '\0';

	buffer = ReadBuffer(r, blk);
	page = (Page) BufferGetPage(buffer);
	opaque = (GISTPageOpaque) PageGetSpecialPointer(page);

	maxoff = PageGetMaxOffsetNumber(page);

	elog(DEBUG4, "%sPage: %d %s blk: %d maxoff: %d free: %d", pred,
		 coff, (opaque->flags & F_LEAF) ? "LEAF" : "INTE", (int) blk,
		 (int) maxoff, PageGetFreeSpace(page));

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		iid = PageGetItemId(page, i);
		which = (IndexTuple) PageGetItem(page, iid);
		cblk = ItemPointerGetBlockNumber(&(which->t_tid));
#ifdef PRINTTUPLE
		elog(DEBUG4, "%s  Tuple. blk: %d size: %d", pred, (int) cblk,
			 IndexTupleSize(which));
#endif

		if (!(opaque->flags & F_LEAF))
			gist_dumptree(r, level + 1, cblk, i);
	}
	ReleaseBuffer(buffer);
	pfree(pred);
}
#endif   /* defined GISTDEBUG */

void
gist_redo(XLogRecPtr lsn, XLogRecord *record)
{
	elog(PANIC, "gist_redo: unimplemented");
}

void
gist_undo(XLogRecPtr lsn, XLogRecord *record)
{
	elog(PANIC, "gist_undo: unimplemented");
}

void
gist_desc(char *buf, uint8 xl_info, char *rec)
{
}
