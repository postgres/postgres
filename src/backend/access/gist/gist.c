/*-------------------------------------------------------------------------
 *
 * gist.c
 *	  interface routines for the postgres GiST index access method.
 *
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/gist/gist.c,v 1.70 2001/02/22 21:48:48 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/gist.h"
#include "access/gistscan.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "catalog/pg_index.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/syscache.h"

#include "access/xlogutils.h"

/* result's status */ 
#define INSERTED	0x01
#define SPLITED		0x02

/* non-export function prototypes */
static void gistdoinsert(Relation r, 
			IndexTuple itup,
			InsertIndexResult *res,
			GISTSTATE *GISTstate);
static int gistlayerinsert( Relation r, BlockNumber blkno,
			IndexTuple **itup,
			int *len,
			InsertIndexResult *res,
			GISTSTATE *giststate );
static OffsetNumber gistwritebuffer( Relation r, 
			Page page, 
			IndexTuple *itup, 
			int len, 
			OffsetNumber off,
			GISTSTATE *giststate );
static int gistnospace( Page page,
			IndexTuple *itvec, int len );
static IndexTuple * gistreadbuffer( Relation r, 
			Buffer buffer, int *len );
static IndexTuple * gistjoinvector( 
			IndexTuple *itvec, int *len, 
			IndexTuple *additvec, int addlen );
static IndexTuple gistunion( Relation r, IndexTuple *itvec, 
			int len, GISTSTATE *giststate );
static IndexTuple gistgetadjusted( Relation r, 
			IndexTuple oldtup, 
			IndexTuple addtup, 
			GISTSTATE *giststate );
static IndexTuple * gistSplit(Relation r,
			Buffer buffer,
			IndexTuple *itup,
			int *len,
			GISTSTATE *giststate,
			InsertIndexResult *res);
static void gistnewroot(GISTSTATE *giststate, Relation r, 
			IndexTuple *itup, int len);
static void GISTInitBuffer(Buffer b, uint32 f);
static OffsetNumber gistchoose(Relation r, Page p, 
			IndexTuple it,
			GISTSTATE *giststate);
static IndexTuple gist_tuple_replacekey(Relation r, 
			GISTENTRY entry, IndexTuple t);
static void gistcentryinit(GISTSTATE *giststate, 
			GISTENTRY *e, char *pr,
			Relation r, Page pg, 
			OffsetNumber o, int b, bool l);

#undef GISTDEBUG
#ifdef GISTDEBUG
static void gist_dumptree(Relation r, int level, BlockNumber blk, OffsetNumber coff);
#endif

/*
** routine to build an index.  Basically calls insert over and over
*/
Datum
gistbuild(PG_FUNCTION_ARGS)
{
	Relation		heap = (Relation) PG_GETARG_POINTER(0);
	Relation		index = (Relation) PG_GETARG_POINTER(1);
	IndexInfo	   *indexInfo = (IndexInfo *) PG_GETARG_POINTER(2);
	Node		   *oldPred = (Node *) PG_GETARG_POINTER(3);
#ifdef NOT_USED
	IndexStrategy	istrat = (IndexStrategy) PG_GETARG_POINTER(4);
#endif
	HeapScanDesc hscan;
	HeapTuple	htup;
	IndexTuple	itup;
	TupleDesc	htupdesc,
				itupdesc;
	Datum		attdata[INDEX_MAX_KEYS];
	char		nulls[INDEX_MAX_KEYS];
	int			nhtups,
				nitups;
	Node	   *pred = indexInfo->ii_Predicate;
#ifndef OMIT_PARTIAL_INDEX
	TupleTable	tupleTable;
	TupleTableSlot *slot;
#endif
	ExprContext *econtext;
	GISTSTATE	giststate;
	GISTENTRY	tmpcentry;
	Buffer		buffer = InvalidBuffer;
	bool	   *compvec;
	int			i;

	/* no locking is needed */

	initGISTstate(&giststate, index);

	/*
	 * We expect to be called exactly once for any index relation. If
	 * that's not the case, big trouble's what we have.
	 */
	if (oldPred == NULL && RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "%s already contains data", RelationGetRelationName(index));

	/* initialize the root page (if this is a new index) */
	if (oldPred == NULL)
	{
		buffer = ReadBuffer(index, P_NEW);
		GISTInitBuffer(buffer, F_LEAF);
		WriteBuffer(buffer);
	}

	/* get tuple descriptors for heap and index relations */
	htupdesc = RelationGetDescr(heap);
	itupdesc = RelationGetDescr(index);

	/*
	 * If this is a predicate (partial) index, we will need to evaluate
	 * the predicate using ExecQual, which requires the current tuple to
	 * be in a slot of a TupleTable.  In addition, ExecQual must have an
	 * ExprContext referring to that slot.	Here, we initialize dummy
	 * TupleTable and ExprContext objects for this purpose. --Nels, Feb 92
	 *
	 * We construct the ExprContext anyway since we need a per-tuple
	 * temporary memory context for function evaluation -- tgl July 00
	 */
#ifndef OMIT_PARTIAL_INDEX
	if (pred != NULL || oldPred != NULL)
	{
		tupleTable = ExecCreateTupleTable(1);
		slot = ExecAllocTableSlot(tupleTable);
		ExecSetSlotDescriptor(slot, htupdesc, false);
	}
	else
	{
		tupleTable = NULL;
		slot = NULL;
	}
	econtext = MakeExprContext(slot, TransactionCommandContext);
#else
	econtext = MakeExprContext(NULL, TransactionCommandContext);
#endif	 /* OMIT_PARTIAL_INDEX */

	/* build the index */
	nhtups = nitups = 0;

	compvec = (bool *) palloc(sizeof(bool) * indexInfo->ii_NumIndexAttrs);

	/* start a heap scan */
	hscan = heap_beginscan(heap, 0, SnapshotNow, 0, (ScanKey) NULL);

	while (HeapTupleIsValid(htup = heap_getnext(hscan, 0)))
	{
		MemoryContextReset(econtext->ecxt_per_tuple_memory);

		nhtups++;

#ifndef OMIT_PARTIAL_INDEX
		/*
		 * If oldPred != NULL, this is an EXTEND INDEX command, so skip
		 * this tuple if it was already in the existing partial index
		 */
		if (oldPred != NULL)
		{
			slot->val = htup;
			if (ExecQual((List *) oldPred, econtext, false))
			{
				nitups++;
				continue;
			}
		}

		/*
		 * Skip this tuple if it doesn't satisfy the partial-index
		 * predicate
		 */
		if (pred != NULL)
		{
			slot->val = htup;
			if (!ExecQual((List *) pred, econtext, false))
				continue;
		}
#endif	 /* OMIT_PARTIAL_INDEX */

		nitups++;

		/*
		 * For the current heap tuple, extract all the attributes we use
		 * in this index, and note which are null.
		 */
		FormIndexDatum(indexInfo,
					   htup,
					   htupdesc,
					   econtext->ecxt_per_tuple_memory,
					   attdata,
					   nulls);

		/* immediately compress keys to normalize */
		for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
		{
			gistcentryinit(&giststate, &tmpcentry, (char *) attdata[i],
						   (Relation) NULL, (Page) NULL, (OffsetNumber) 0,
						   -1 /* size is currently bogus */ , TRUE);
			if (attdata[i] != (Datum) tmpcentry.pred &&
				!(giststate.keytypbyval))
				compvec[i] = TRUE;
			else
				compvec[i] = FALSE;
			attdata[i] = (Datum) tmpcentry.pred;
		}

		/* form an index tuple and point it at the heap tuple */
		itup = index_formtuple(itupdesc, attdata, nulls);
		itup->t_tid = htup->t_self;

		/*
		 * Since we already have the index relation locked, we call
		 * gistdoinsert directly.  Normal access method calls dispatch
		 * through gistinsert, which locks the relation for write.	This
		 * is the right thing to do if you're inserting single tups, but
		 * not when you're initializing the whole index at once.
		 */

		gistdoinsert(index, itup, NULL, &giststate);

		for (i = 0; i < indexInfo->ii_NumIndexAttrs; i++)
			if (compvec[i])
				pfree(DatumGetPointer(attdata[i]));

		pfree(itup);
	}

	/* okay, all heap tuples are indexed */
	heap_endscan(hscan);

	pfree(compvec);

#ifndef OMIT_PARTIAL_INDEX
	if (pred != NULL || oldPred != NULL)
	{
		ExecDropTupleTable(tupleTable, true);
	}
#endif	 /* OMIT_PARTIAL_INDEX */
	FreeExprContext(econtext);

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
		UpdateStats(hrelid, nhtups);
		UpdateStats(irelid, nitups);
		if (oldPred != NULL)
		{
			if (nitups == nhtups)
				pred = NULL;
			UpdateIndexPredicate(irelid, oldPred, pred);
		}
	}

#ifdef GISTDEBUG
gist_dumptree(index, 0, GISTP_ROOT, 0);
#endif

	PG_RETURN_VOID();
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
	Relation		r = (Relation) PG_GETARG_POINTER(0);
	Datum		   *datum = (Datum *) PG_GETARG_POINTER(1);
	char		   *nulls = (char *) PG_GETARG_POINTER(2);
	ItemPointer		ht_ctid = (ItemPointer) PG_GETARG_POINTER(3);
#ifdef NOT_USED
	Relation		heapRel = (Relation) PG_GETARG_POINTER(4);
#endif
	InsertIndexResult res;
	IndexTuple	itup;
	GISTSTATE	giststate;
	GISTENTRY	tmpentry;
	int			i;
	bool	   *compvec;

	initGISTstate(&giststate, r);

	/* immediately compress keys to normalize */
	compvec = (bool *) palloc(sizeof(bool) * r->rd_att->natts);
	for (i = 0; i < r->rd_att->natts; i++)
	{
		gistcentryinit(&giststate, &tmpentry, (char *) datum[i],
					   (Relation) NULL, (Page) NULL, (OffsetNumber) 0,
					   -1 /* size is currently bogus */ , TRUE);
		if (datum[i] != (Datum) tmpentry.pred && !(giststate.keytypbyval))
			compvec[i] = TRUE;
		else
			compvec[i] = FALSE;
		datum[i] = (Datum) tmpentry.pred;
	}
	itup = index_formtuple(RelationGetDescr(r), datum, nulls);
	itup->t_tid = *ht_ctid;

	/*
	 * Notes in ExecUtils:ExecOpenIndices()
	 *
	 * RelationSetLockForWrite(r);
	 */

	res = (InsertIndexResult) palloc(sizeof(InsertIndexResultData));
	gistdoinsert(r, itup, &res, &giststate);
	for (i = 0; i < r->rd_att->natts; i++)
		if (compvec[i] == TRUE)
			pfree((char *) datum[i]);
	pfree(itup);
	pfree(compvec);

	PG_RETURN_POINTER(res);
}

/*
** Take a compressed entry, and install it on a page.  Since we now know
** where the entry will live, we decompress it and recompress it using
** that knowledge (some compression routines may want to fish around
** on the page, for example, or do something special for leaf nodes.)
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
	OffsetNumber    retval;

	/*
	 * recompress the item given that we now know the exact page and
	 * offset for insertion
	 */
	gistdentryinit(giststate, dentry,
				   (((char *) itup) + sizeof(IndexTupleData)),
			  (Relation) 0, (Page) 0, (OffsetNumber) InvalidOffsetNumber,
				   IndexTupleSize(itup) - sizeof(IndexTupleData), FALSE);
	gistcentryinit(giststate, &tmpcentry, dentry->pred, r, page,
				   offsetNumber, dentry->bytes, FALSE);
	*newtup = gist_tuple_replacekey(r, tmpcentry, itup);
	retval = PageAddItem(page, (Item) *newtup, IndexTupleSize(*newtup),
						offsetNumber, flags);
	/* be tidy */
	if (tmpcentry.pred && tmpcentry.pred != dentry->pred
		&& tmpcentry.pred != (((char *) itup) + sizeof(IndexTupleData)))
		pfree(tmpcentry.pred);
	return (retval);
}

static void 
gistdoinsert( Relation r, 
		IndexTuple itup, 
		InsertIndexResult *res, 
		GISTSTATE *giststate ) {
	IndexTuple *instup;
	int i,ret,len = 1;

	instup = ( IndexTuple* ) palloc( sizeof(IndexTuple) );
	instup[0] = ( IndexTuple ) palloc( IndexTupleSize( itup ) );
	memcpy( instup[0], itup, IndexTupleSize( itup ) );
 
	ret = gistlayerinsert(r, GISTP_ROOT, &instup, &len, res, giststate);
	if ( ret & SPLITED )
		gistnewroot( giststate, r, instup, len );

	for(i=0;i<len;i++)
		pfree( instup[i] );
	pfree( instup );
}

static int
gistlayerinsert( Relation r, BlockNumber blkno, 
			 IndexTuple **itup,       /* in - out, has compressed entry */
			 int *len    ,           /* in - out */
			 InsertIndexResult *res, /* out */
			 GISTSTATE *giststate ) {
	Buffer	buffer;
	Page	page;
	OffsetNumber    child;
	int ret;
	GISTPageOpaque opaque;

	buffer = ReadBuffer(r, blkno);
	page = (Page) BufferGetPage(buffer);
	opaque = (GISTPageOpaque) PageGetSpecialPointer(page);

	if (!(opaque->flags & F_LEAF)) {
		/* internal page, so we must walk on tree */
		/* len IS equial 1 */
		ItemId iid;
		BlockNumber nblkno;
		ItemPointerData oldtid;
		IndexTuple oldtup;
	
		child = gistchoose( r, page, *(*itup), giststate );
		iid = PageGetItemId(page, child);
		oldtup = (IndexTuple) PageGetItem(page, iid);
		nblkno = ItemPointerGetBlockNumber(&(oldtup->t_tid));

		/* 
		 * After this call:
		 * 1. if child page was splited, then itup contains
		 * keys for each page
		 * 2. if  child page wasn't splited, then itup contains
		 * additional for adjustement of current key 
		 */
		ret = gistlayerinsert( r, nblkno, itup, len, res, giststate );

		/* nothing inserted in child */
		if ( ! (ret & INSERTED) ) {
			ReleaseBuffer(buffer);
			return 0x00;	
		}

		/* child does not splited */ 
		if ( ! (ret & SPLITED) ) {
			IndexTuple newtup = gistgetadjusted( r, oldtup, (*itup)[0], giststate ); 
			if ( ! newtup ) {
				/* not need to update key */
				ReleaseBuffer(buffer);
				return 0x00;
			}

			pfree( (*itup)[0] ); /* !!! */
			(*itup)[0] = newtup;
		}

		/* key is modified, so old version must be deleted */ 
		ItemPointerSet(&oldtid, blkno, child);
		DirectFunctionCall2(gistdelete,
			PointerGetDatum(r),
			PointerGetDatum(&oldtid));
	}

	ret = INSERTED; 

	if (  gistnospace(page, (*itup), *len) ) {
		/* no space for insertion */
		IndexTuple *itvec;
		int tlen;

		ret |= SPLITED;
		itvec = gistreadbuffer( r, buffer, &tlen );
		itvec = gistjoinvector( itvec, &tlen, (*itup), *len ); 
		pfree( (*itup) );
		(*itup) = gistSplit( r, buffer, itvec, &tlen, giststate, 
			(opaque->flags & F_LEAF) ? res : NULL ); /*res only for inserting in leaf*/
		ReleaseBuffer( buffer );
		pfree( itvec );
		*len = tlen;   /* now tlen >= 2 */
	} else {
		/* enogth space */
		OffsetNumber off, l;

		off = ( PageIsEmpty(page) ) ? 
				FirstOffsetNumber
			:
				OffsetNumberNext(PageGetMaxOffsetNumber(page));
		l = gistwritebuffer( r, page, (*itup), *len, off, giststate );
		WriteBuffer(buffer);

		/* set res if insert into leaf page, in
                this case, len = 1 always */
		if ( res && (opaque->flags & F_LEAF) )
			ItemPointerSet(&((*res)->pointerData), blkno, l);

		if ( *len > 1 ) { /* previos insert ret & SPLITED != 0 */ 
			int i;
			/* child was splited, so we must form union
			 * for insertion in parent */
			IndexTuple newtup = gistunion(r, (*itup), *len, giststate);
			for(i=0; i<*len; i++)
				pfree( (*itup)[i] );
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
gistwritebuffer( Relation r, Page page, IndexTuple *itup, 
		int len, OffsetNumber off, GISTSTATE *giststate) {
	OffsetNumber l = InvalidOffsetNumber;
	int i;
	GISTENTRY       tmpdentry;
	IndexTuple newtup;
		
	for(i=0; i<len; i++) { 
		l = gistPageAddItem(giststate, r, page, 
			(Item) itup[i], IndexTupleSize(itup[i]),   
			off, LP_USED, &tmpdentry, &newtup);
		off = OffsetNumberNext( off );
		if (tmpdentry.pred != (((char *) itup[i]) + sizeof(IndexTupleData)) && tmpdentry.pred)
			pfree(tmpdentry.pred);
		if (itup[i] != newtup)
			pfree(newtup);
	}
	return l; 
}

/*
 * Check space for itup vector on page
 */
static int 
gistnospace( Page page, IndexTuple *itvec, int len ) {
	int size = 0;
	int i;
	for(i=0; i<len; i++) 
		size += IndexTupleSize( itvec[i] )+4; /* ??? */

	return  (PageGetFreeSpace(page) < size);
} 

/*
 * Read buffer into itup vector
 */
static IndexTuple *
gistreadbuffer( Relation r, Buffer buffer, int *len /*out*/) {
	OffsetNumber i, maxoff;
	IndexTuple   *itvec;
	Page p = (Page) BufferGetPage(buffer);

	*len=0;
	maxoff = PageGetMaxOffsetNumber(p);
	itvec = palloc( sizeof(IndexTuple) * maxoff );
	for(i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		itvec[ (*len)++ ] = (IndexTuple) PageGetItem(p, PageGetItemId(p, i));

	return itvec;
}

/*
 * join two vectors into one
 */
static IndexTuple *
gistjoinvector( IndexTuple *itvec, int *len, IndexTuple *additvec, int addlen ) {
	itvec = (IndexTuple*) repalloc( (void*)itvec, sizeof(IndexTuple) * ( (*len) + addlen ) );
	memmove( &itvec[*len], additvec, sizeof(IndexTuple) * addlen );
	*len += addlen;
	return itvec;
}

/*
 * return union of itup vector
 */
static IndexTuple
gistunion( Relation r, IndexTuple *itvec, int len, GISTSTATE *giststate ) {
	bytea   *evec;
	char 	*datum;
	int 	datumsize, i;
	GISTENTRY centry;
	char isnull;
	IndexTuple newtup;

	evec = (bytea *) palloc(len * sizeof(GISTENTRY) + VARHDRSZ);
	VARATT_SIZEP(evec) = len * sizeof(GISTENTRY) + VARHDRSZ;

	for ( i = 0 ; i< len ; i++ ) 
		gistdentryinit(giststate, &((GISTENTRY *) VARDATA(evec))[i],
			(char*) itvec[i] + sizeof(IndexTupleData), 
			(Relation)NULL, (Page)NULL, (OffsetNumber)NULL,
			IndexTupleSize((IndexTuple)itvec[i]) - sizeof(IndexTupleData), FALSE);

	datum = (char *)
		DatumGetPointer(FunctionCall2(&giststate->unionFn,
			PointerGetDatum(evec),
			PointerGetDatum(&datumsize)));

	for ( i = 0 ; i< len ; i++ )
		if ( ((GISTENTRY *) VARDATA(evec))[i].pred &&
		   ((GISTENTRY *) VARDATA(evec))[i].pred != 
		   ((char*)( itvec[i] )+ sizeof(IndexTupleData)) )
			pfree( ((GISTENTRY *) VARDATA(evec))[i].pred ); 
	     
	pfree( evec );

	gistcentryinit(giststate, &centry, datum, 
		(Relation)NULL, (Page)NULL, (OffsetNumber)NULL,
		datumsize, FALSE);

	isnull = (centry.pred) ? ' ' : 'n';
	newtup = (IndexTuple) index_formtuple( r->rd_att, (Datum *) &centry.pred, &isnull );
	if (centry.pred != datum)
		pfree( datum );

	return newtup;
} 

/*
 * Forms union of oldtup and addtup, if union == oldtup then return NULL
 */
static IndexTuple
gistgetadjusted( Relation r, IndexTuple oldtup, IndexTuple addtup, GISTSTATE *giststate ) {
	bytea   *evec;
	char 	*datum;
	int 	datumsize;
	bool    result;
	char 	isnull;
	GISTENTRY centry, *ev0p, *ev1p;
	IndexTuple newtup = NULL;
	
	evec = (bytea *) palloc(2 * sizeof(GISTENTRY) + VARHDRSZ);
	VARATT_SIZEP(evec) = 2 * sizeof(GISTENTRY) + VARHDRSZ;

	gistdentryinit(giststate, &((GISTENTRY *) VARDATA(evec))[0],
		(char*) oldtup + sizeof(IndexTupleData), (Relation) NULL, 
		(Page) NULL, (OffsetNumber) 0,
		IndexTupleSize((IndexTuple)oldtup) - sizeof(IndexTupleData), FALSE);
	ev0p = &((GISTENTRY *) VARDATA(evec))[0];

	gistdentryinit(giststate, &((GISTENTRY *) VARDATA(evec))[1],
		(char*) addtup + sizeof(IndexTupleData), (Relation) NULL,
		(Page) NULL, (OffsetNumber) 0,
		IndexTupleSize((IndexTuple)addtup) - sizeof(IndexTupleData), FALSE);
	ev1p = &((GISTENTRY *) VARDATA(evec))[1];

	datum = (char *)
		DatumGetPointer(FunctionCall2(&giststate->unionFn,
			PointerGetDatum(evec),
			PointerGetDatum(&datumsize)));

	if ( ! ( ev0p->pred && ev1p->pred ) ) {
		result = ( ev0p->pred == NULL && ev1p->pred == NULL );
	} else {
		FunctionCall3(&giststate->equalFn,
			PointerGetDatum(ev0p->pred),
			PointerGetDatum(datum),
			PointerGetDatum(&result));
	}

	if ( result ) {
		/* not need to update key */
		pfree( datum );
	} else {
		gistcentryinit(giststate, &centry, datum, ev0p->rel, ev0p->page,
			ev0p->offset, datumsize, FALSE);

		isnull = (centry.pred) ? ' ' : 'n';
		newtup = (IndexTuple) index_formtuple( r->rd_att, (Datum *) &centry.pred, &isnull );
		newtup->t_tid = oldtup->t_tid; 
		if (centry.pred != datum)
			pfree( datum );
	}

	if ( ev0p->pred && 
	     ev0p->pred != (char*) oldtup + sizeof(IndexTupleData)  ) 
		pfree( ev0p->pred ); 
	if ( ev1p->pred && 
	     ev1p->pred != (char*) addtup + sizeof(IndexTupleData)  ) 
		pfree( ev1p->pred ); 
	pfree( evec );

	return newtup;	
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
	Buffer		leftbuf, rightbuf;
	Page		left, right;
	OffsetNumber 	*spl_left, *spl_right;
	IndexTuple	*lvectup, *rvectup, *newtup;
	int leftoff, rightoff;
	BlockNumber lbknum, rbknum;
	GISTPageOpaque opaque;
	char	   isnull;
	GIST_SPLITVEC v;
	bytea	   *entryvec;
	bool	   *decompvec;
	GISTENTRY	tmpentry;
	int i, nlen;

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
	entryvec = (bytea *) palloc(VARHDRSZ + (*len+1) * sizeof(GISTENTRY));
	decompvec = (bool *) palloc(VARHDRSZ + (*len+1) * sizeof(bool));
	VARATT_SIZEP(entryvec) = (*len+1) * sizeof(GISTENTRY) + VARHDRSZ;
	for (i = 1; i <= *len; i++)
	{
		gistdentryinit(giststate, &((GISTENTRY *) VARDATA(entryvec))[i],
					   (((char *) itup[i-1]) + sizeof(IndexTupleData)),
					   r, p, i,
				 IndexTupleSize(itup[i-1]) - sizeof(IndexTupleData), FALSE);
		if ((char *) (((GISTENTRY *) VARDATA(entryvec))[i].pred)
			== (((char *) itup[i-1]) + sizeof(IndexTupleData)))
			decompvec[i] = FALSE;
		else
			decompvec[i] = TRUE;
	}

	/* now let the user-defined picksplit function set up the split vector */
	FunctionCall2(&giststate->picksplitFn,
		PointerGetDatum(entryvec),
		PointerGetDatum(&v));

	/* clean up the entry vector: its preds need to be deleted, too */
	for (i = 1; i <= *len; i++)
		if (decompvec[i] && ((GISTENTRY *) VARDATA(entryvec))[i].pred)
			pfree(((GISTENTRY *) VARDATA(entryvec))[i].pred);
	pfree(entryvec);
	pfree(decompvec);

	spl_left = v.spl_left; spl_right = v.spl_right;
	
	/* form left and right vector */
	lvectup = (IndexTuple*) palloc( sizeof( IndexTuple )*v.spl_nleft );
	rvectup = (IndexTuple*) palloc( sizeof( IndexTuple )*v.spl_nright );
	leftoff = rightoff = 0;
	for( i=1; i <= *len; i++ ) {
		if (i == *(spl_left) || ( i==*len && *(spl_left) != FirstOffsetNumber ) ) {
			lvectup[ leftoff++ ] = itup[ i-1 ];
			spl_left++;
		} else { 
			rvectup[ rightoff++ ] = itup[ i-1 ];
			spl_right++;
		}
	}

	/* write on disk (may be need another split) */
	if ( gistnospace(right, rvectup, v.spl_nright) ) {
		nlen = v.spl_nright;
		newtup = gistSplit(r, rightbuf, rvectup, &nlen, giststate, 
			( res && rvectup[ nlen-1 ] == itup[ *len - 1 ] ) ? res : NULL );
		ReleaseBuffer( rightbuf );
	} else {
		OffsetNumber l;
	
		l = gistwritebuffer( r, right, rvectup, v.spl_nright, FirstOffsetNumber, giststate );
		WriteBuffer(rightbuf);

		if ( res )
			ItemPointerSet(&((*res)->pointerData), rbknum, l);
		gistcentryinit(giststate, &tmpentry, v.spl_rdatum, (Relation) NULL,
					   (Page) NULL, (OffsetNumber) 0,
					   -1, FALSE);
		if (v.spl_rdatum != tmpentry.pred)
			pfree(v.spl_rdatum);
		v.spl_rdatum = tmpentry.pred;

		nlen = 1;
		newtup = (IndexTuple*) palloc( sizeof(IndexTuple) * 1);
		isnull = ( v.spl_rdatum ) ? ' ' : 'n';
		newtup[0] = (IndexTuple) index_formtuple(r->rd_att, (Datum *) &(v.spl_rdatum), &isnull);
		ItemPointerSet(&(newtup[0]->t_tid), rbknum, 1);
	}

	if ( gistnospace(left, lvectup, v.spl_nleft) ) {
		int llen = v.spl_nleft;
		IndexTuple *lntup;

		lntup = gistSplit(r, leftbuf, lvectup, &llen, giststate, 
			( res && lvectup[ llen-1 ] == itup[ *len - 1 ] ) ? res : NULL );
		ReleaseBuffer( leftbuf );

		newtup = gistjoinvector( newtup, &nlen, lntup, llen );
		pfree( lntup ); 
	} else {
		OffsetNumber l;
	
		l = gistwritebuffer( r, left, lvectup, v.spl_nleft, FirstOffsetNumber, giststate );
		if ( BufferGetBlockNumber(buffer) != GISTP_ROOT)
			PageRestoreTempPage(left, p);

		WriteBuffer(leftbuf);

		if ( res )
			ItemPointerSet(&((*res)->pointerData), lbknum, l);
		gistcentryinit(giststate, &tmpentry, v.spl_ldatum, (Relation) NULL,
					   (Page) NULL, (OffsetNumber) 0,
					   -1, FALSE);
		if (v.spl_ldatum != tmpentry.pred)
			pfree(v.spl_ldatum);
		v.spl_ldatum = tmpentry.pred;

		nlen += 1;
		newtup = (IndexTuple*) repalloc( (void*)newtup, sizeof(IndexTuple) * nlen);
		isnull = ( v.spl_ldatum ) ? ' ' : 'n';
		newtup[nlen-1] = (IndexTuple) index_formtuple(r->rd_att, (Datum *) &(v.spl_ldatum), &isnull);
		ItemPointerSet(&(newtup[nlen-1]->t_tid), lbknum, 1);
	}


	/* adjust active scans */
	gistadjscans(r, GISTOP_SPLIT, BufferGetBlockNumber(buffer), FirstOffsetNumber);

	/* !!! pfree */
	pfree( rvectup );
	pfree( lvectup );
	pfree( v.spl_left );
	pfree( v.spl_right );

	*len = nlen;
	return newtup;
}

static void
gistnewroot(GISTSTATE *giststate, Relation r, IndexTuple *itup, int len)
{
	Buffer		b;
	Page		p;

	b = ReadBuffer(r, GISTP_ROOT);
	GISTInitBuffer(b, 0);
	p = BufferGetPage(b);
	
	gistwritebuffer( r, p, itup, len, FirstOffsetNumber, giststate );
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
	MemSet(page, 0, (int) pageSize);
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
	char	   *id;
	char	   *datum;
	float		usize;
	OffsetNumber which;
	float		which_grow;
	GISTENTRY	entry,
				identry;
	int			size,
				idsize;

	idsize = IndexTupleSize(it) - sizeof(IndexTupleData);
	id = ((char *) it) + sizeof(IndexTupleData);
	maxoff = PageGetMaxOffsetNumber(p);
	which_grow = -1.0;
	which = -1;

	gistdentryinit(giststate, &identry, id, (Relation) NULL, (Page) NULL,
				   (OffsetNumber) 0, idsize, FALSE);

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		datum = (char *) PageGetItem(p, PageGetItemId(p, i));
		size = IndexTupleSize(datum) - sizeof(IndexTupleData);
		datum += sizeof(IndexTupleData);
		gistdentryinit(giststate, &entry, datum, r, p, i, size, FALSE);
		FunctionCall3(&giststate->penaltyFn,
					  PointerGetDatum(&entry),
					  PointerGetDatum(&identry),
					  PointerGetDatum(&usize));
		if (which_grow < 0 || usize < which_grow)
		{
			which = i;
			which_grow = usize;
			if (which_grow == 0)
				break;
		}
		if (entry.pred && entry.pred != datum)
			pfree(entry.pred);
	}
	if (identry.pred && identry.pred != id)
		pfree(identry.pred);

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
** remove an entry from a page
*/
Datum
gistdelete(PG_FUNCTION_ARGS)
{
	Relation		r = (Relation) PG_GETARG_POINTER(0);
	ItemPointer		tid = (ItemPointer) PG_GETARG_POINTER(1);
	BlockNumber blkno;
	OffsetNumber offnum;
	Buffer		buf;
	Page		page;

	/*
	 * Notes in ExecUtils:ExecOpenIndices() Also note that only vacuum
	 * deletes index tuples now...
	 *
	 * RelationSetLockForWrite(r);
	 */

	blkno = ItemPointerGetBlockNumber(tid);
	offnum = ItemPointerGetOffsetNumber(tid);

	/* adjust any scans that will be affected by this deletion */
	gistadjscans(r, GISTOP_DEL, blkno, offnum);

	/* delete the index tuple */
	buf = ReadBuffer(r, blkno);
	page = BufferGetPage(buf);

	PageIndexTupleDelete(page, offnum);

	WriteBuffer(buf);

	PG_RETURN_VOID();
}

void
initGISTstate(GISTSTATE *giststate, Relation index)
{
	RegProcedure consistent_proc,
				union_proc,
				compress_proc,
				decompress_proc;
	RegProcedure penalty_proc,
				picksplit_proc,
				equal_proc;
	HeapTuple	htup;
	Form_pg_index itupform;
	Oid			indexrelid;

	consistent_proc = index_getprocid(index, 1, GIST_CONSISTENT_PROC);
	union_proc = index_getprocid(index, 1, GIST_UNION_PROC);
	compress_proc = index_getprocid(index, 1, GIST_COMPRESS_PROC);
	decompress_proc = index_getprocid(index, 1, GIST_DECOMPRESS_PROC);
	penalty_proc = index_getprocid(index, 1, GIST_PENALTY_PROC);
	picksplit_proc = index_getprocid(index, 1, GIST_PICKSPLIT_PROC);
	equal_proc = index_getprocid(index, 1, GIST_EQUAL_PROC);
	fmgr_info(consistent_proc, &giststate->consistentFn);
	fmgr_info(union_proc, &giststate->unionFn);
	fmgr_info(compress_proc, &giststate->compressFn);
	fmgr_info(decompress_proc, &giststate->decompressFn);
	fmgr_info(penalty_proc, &giststate->penaltyFn);
	fmgr_info(picksplit_proc, &giststate->picksplitFn);
	fmgr_info(equal_proc, &giststate->equalFn);

	/* see if key type is different from type of attribute being indexed */
	htup = SearchSysCache(INDEXRELID,
						  ObjectIdGetDatum(RelationGetRelid(index)),
						  0, 0, 0);
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "initGISTstate: index %u not found",
			 RelationGetRelid(index));
	itupform = (Form_pg_index) GETSTRUCT(htup);
	giststate->haskeytype = itupform->indhaskeytype;
	indexrelid = itupform->indexrelid;
	ReleaseSysCache(htup);

	if (giststate->haskeytype)
	{
		/* key type is different -- is it byval? */
		htup = SearchSysCache(ATTNUM,
							  ObjectIdGetDatum(indexrelid),
							  UInt16GetDatum(FirstOffsetNumber),
							  0, 0);
		if (!HeapTupleIsValid(htup))
			elog(ERROR, "initGISTstate: no attribute tuple %u %d",
				 indexrelid, FirstOffsetNumber);
		giststate->keytypbyval = (((Form_pg_attribute) htup)->attbyval);
		ReleaseSysCache(htup);
	}
	else
		giststate->keytypbyval = FALSE;
}


/*
** Given an IndexTuple to be inserted on a page, this routine replaces
** the key with another key, which may involve generating a new IndexTuple
** if the sizes don't match
*/
static IndexTuple
gist_tuple_replacekey(Relation r, GISTENTRY entry, IndexTuple t)
{
	char	   *datum = (((char *) t) + sizeof(IndexTupleData));

	/* if new entry fits in index tuple, copy it in */
	if ((Size) entry.bytes < IndexTupleSize(t) - sizeof(IndexTupleData) || (Size) entry.bytes == 0 )
	{
		memcpy(datum, entry.pred, entry.bytes);
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
		char	   isnull;

		isnull = ( entry.pred ) ? ' ' : 'n';
		newtup = (IndexTuple) index_formtuple(tupDesc,
											  (Datum *) &(entry.pred),
											  &isnull);
		newtup->t_tid = t->t_tid;
		return newtup;
	}
}


/*
** initialize a GiST entry with a decompressed version of pred
*/
void
gistdentryinit(GISTSTATE *giststate, GISTENTRY *e, char *pr, Relation r,
			   Page pg, OffsetNumber o, int b, bool l)
{
	GISTENTRY  *dep;

	gistentryinit(*e, pr, r, pg, o, b, l);
	if (giststate->haskeytype)
	{
		dep = (GISTENTRY *)
			DatumGetPointer(FunctionCall1(&giststate->decompressFn,
										  PointerGetDatum(e)));
		gistentryinit(*e, dep->pred, dep->rel, dep->page, dep->offset, dep->bytes,
					  dep->leafkey);
		if (dep != e)
			pfree(dep);
	}
}


/*
** initialize a GiST entry with a compressed version of pred
*/
static void
gistcentryinit(GISTSTATE *giststate, GISTENTRY *e, char *pr, Relation r,
			   Page pg, OffsetNumber o, int b, bool l)
{
	GISTENTRY  *cep;

	gistentryinit(*e, pr, r, pg, o, b, l);
	if (giststate->haskeytype)
	{
		cep = (GISTENTRY *)
			DatumGetPointer(FunctionCall1(&giststate->compressFn,
										  PointerGetDatum(e)));
		gistentryinit(*e, cep->pred, cep->rel, cep->page, cep->offset, cep->bytes,
					  cep->leafkey);
		if (cep != e)
			pfree(cep);
	}
}

#ifdef GISTDEBUG
static void
gist_dumptree(Relation r, int level, BlockNumber blk, OffsetNumber coff)
{
	Buffer		buffer;
	Page		page;
	GISTPageOpaque opaque;
	IndexTuple	which;
	ItemId          iid;
	OffsetNumber i,maxoff;
	BlockNumber	cblk;
	char	*pred;

	pred  = (char*) palloc( sizeof(char)*level+1 );
	MemSet(pred, '\t', level);
	pred[level]='\0';

	buffer = ReadBuffer(r, blk);
	page = (Page) BufferGetPage(buffer);
	opaque = (GISTPageOpaque) PageGetSpecialPointer(page);
	
	maxoff = PageGetMaxOffsetNumber( page );
	
	elog(NOTICE,"%sPage: %d %s blk: %d maxoff: %d free: %d", pred, coff, ( opaque->flags & F_LEAF ) ? "LEAF" : "INTE", (int)blk, (int)maxoff, PageGetFreeSpace(page));
	
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i)) {
		iid = PageGetItemId(page, i);
		which = (IndexTuple) PageGetItem(page, iid);
		cblk = ItemPointerGetBlockNumber(&(which->t_tid));
#ifdef PRINTTUPLE		
		elog(NOTICE,"%s  Tuple. blk: %d size: %d", pred, (int)cblk, IndexTupleSize( which ) );
#endif 
	
		if ( ! ( opaque->flags & F_LEAF ) ) { 
			gist_dumptree( r, level+1, cblk, i );
		}
	}
	ReleaseBuffer(buffer);
	pfree(pred);
}
#endif	 /* defined GISTDEBUG */

void
gist_redo(XLogRecPtr lsn, XLogRecord *record)
{
	elog(STOP, "gist_redo: unimplemented");
}
 
void
gist_undo(XLogRecPtr lsn, XLogRecord *record)
{
	elog(STOP, "gist_undo: unimplemented");
}
 
void
gist_desc(char *buf, uint8 xl_info, char* rec)
{
}

