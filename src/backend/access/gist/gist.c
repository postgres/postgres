/*-------------------------------------------------------------------------
 *
 * gist.c
 *	  interface routines for the postgres GiST index access method.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/gist/gist.c,v 1.120 2005/06/20 10:29:36 teodor Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/gistscan.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "utils/memutils.h"

/* Working state for gistbuild and its callback */
typedef struct
{
	GISTSTATE	giststate;
	int			numindexattrs;
	double		indtuples;
	MemoryContext tmpCtx;
} GISTBuildState;


/* non-export function prototypes */
static void gistbuildCallback(Relation index,
				  HeapTuple htup,
				  Datum *values,
				  bool *isnull,
				  bool tupleIsAlive,
				  void *state);
static void gistdoinsert(Relation r,
			 IndexTuple itup,
			 GISTSTATE *GISTstate);
static void gistfindleaf(GISTInsertState *state,
				GISTSTATE *giststate);


#define ROTATEDIST(d) do { \
	SplitedPageLayout *tmp=(SplitedPageLayout*)palloc(sizeof(SplitedPageLayout)); \
	memset(tmp,0,sizeof(SplitedPageLayout)); \
	tmp->next = (d); \
	(d)=tmp; \
} while(0)
	

/*
 * Create and return a temporary memory context for use by GiST. We
 * _always_ invoke user-provided methods in a temporary memory
 * context, so that memory leaks in those functions cannot cause
 * problems. Also, we use some additional temporary contexts in the
 * GiST code itself, to avoid the need to do some awkward manual
 * memory management.
 */
MemoryContext                                                                                 
createTempGistContext(void)                                                                   
{                                                                                             
    return AllocSetContextCreate(CurrentMemoryContext,                                        
                                 "GiST temporary context",                                    
                                 ALLOCSET_DEFAULT_MINSIZE,                                    
                                 ALLOCSET_DEFAULT_INITSIZE,                                   
                                 ALLOCSET_DEFAULT_MAXSIZE);                                   
}                                                                                             

/*
 * Routine to build an index.  Basically calls insert over and over.
 *
 * XXX: it would be nice to implement some sort of bulk-loading
 * algorithm, but it is not clear how to do that.
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

	/*
	 * We expect to be called exactly once for any index relation. If
	 * that's not the case, big trouble's what we have.
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/* no locking is needed */
	initGISTstate(&buildstate.giststate, index);

	/* initialize the root page */
	buffer = gistReadBuffer(index, P_NEW);
	GISTInitBuffer(buffer, F_LEAF);
	if ( !index->rd_istemp ) {
		XLogRecPtr		recptr;
		XLogRecData		rdata;
		Page			page;

		rdata.buffer     = InvalidBuffer;
		rdata.data       = (char*)&(index->rd_node);
		rdata.len        = sizeof(RelFileNode);
		rdata.next       = NULL;

		page = BufferGetPage(buffer);

		START_CRIT_SECTION();

		recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_CREATE_INDEX, &rdata);
		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);

		END_CRIT_SECTION();
	}
	WriteBuffer(buffer);

	/* build the index */
	buildstate.numindexattrs = indexInfo->ii_NumIndexAttrs;
	buildstate.indtuples = 0;
	/*
	 * create a temporary memory context that is reset once for each
	 * tuple inserted into the index
	 */
	buildstate.tmpCtx = createTempGistContext();

	/* do the heap scan */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo,
								   gistbuildCallback, (void *) &buildstate);

	/* okay, all heap tuples are indexed */
	MemoryContextDelete(buildstate.tmpCtx);

	/* since we just counted the # of tuples, may as well update stats */
	IndexCloseAndUpdateStats(heap, reltuples, index, buildstate.indtuples);

	freeGISTstate(&buildstate.giststate);

	PG_RETURN_VOID();
}

/*
 * Per-tuple callback from IndexBuildHeapScan
 */
static void
gistbuildCallback(Relation index,
				  HeapTuple htup,
				  Datum *values,
				  bool *isnull,
				  bool tupleIsAlive,
				  void *state)
{
	GISTBuildState *buildstate = (GISTBuildState *) state;
	IndexTuple	itup;
	GISTENTRY	tmpcentry;
	int			i;
	MemoryContext oldCtx;

	/* GiST cannot index tuples with leading NULLs */
	if (isnull[0])
		return;

	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	/* immediately compress keys to normalize */
	for (i = 0; i < buildstate->numindexattrs; i++)
	{
		if (isnull[i])
			values[i] = (Datum) 0;
		else
		{
			gistcentryinit(&buildstate->giststate, i, &tmpcentry, values[i],
						   NULL, NULL, (OffsetNumber) 0,
						   -1 /* size is currently bogus */, TRUE, FALSE);
			values[i] = tmpcentry.key;
		}
	}

	/* form an index tuple and point it at the heap tuple */
	itup = index_form_tuple(buildstate->giststate.tupdesc, values, isnull);
	itup->t_tid = htup->t_self;

	/*
	 * Since we already have the index relation locked, we call
	 * gistdoinsert directly.  Normal access method calls dispatch through
	 * gistinsert, which locks the relation for write.	This is the right
	 * thing to do if you're inserting single tups, but not when you're
	 * initializing the whole index at once.
	 */
	gistdoinsert(index, itup, &buildstate->giststate);

	buildstate->indtuples += 1;
	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(buildstate->tmpCtx);
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
	Datum	   *values = (Datum *) PG_GETARG_POINTER(1);
	bool	   *isnull = (bool *) PG_GETARG_POINTER(2);
	ItemPointer ht_ctid = (ItemPointer) PG_GETARG_POINTER(3);
#ifdef NOT_USED
	Relation	heapRel = (Relation) PG_GETARG_POINTER(4);
	bool		checkUnique = PG_GETARG_BOOL(5);
#endif
	IndexTuple	itup;
	GISTSTATE	giststate;
	GISTENTRY	tmpentry;
	int			i;
	MemoryContext oldCtx;
	MemoryContext insertCtx;

	/*
	 * Since GIST is not marked "amconcurrent" in pg_am, caller should
	 * have acquired exclusive lock on index relation.	We need no locking
	 * here.
	 */

	/* GiST cannot index tuples with leading NULLs */
	if (isnull[0])
		PG_RETURN_BOOL(false);

	insertCtx = createTempGistContext();
	oldCtx = MemoryContextSwitchTo(insertCtx);

	initGISTstate(&giststate, r);

	/* immediately compress keys to normalize */
	for (i = 0; i < r->rd_att->natts; i++)
	{
		if (isnull[i])
			values[i] = (Datum) 0;
		else
		{
			gistcentryinit(&giststate, i, &tmpentry, values[i],
						   NULL, NULL, (OffsetNumber) 0,
						   -1 /* size is currently bogus */, TRUE, FALSE);
			values[i] = tmpentry.key;
		}
	}
	itup = index_form_tuple(giststate.tupdesc, values, isnull);
	itup->t_tid = *ht_ctid;

	gistdoinsert(r, itup, &giststate);

	/* cleanup */
	freeGISTstate(&giststate);
	MemoryContextSwitchTo(oldCtx);
	MemoryContextDelete(insertCtx);

	PG_RETURN_BOOL(true);
}


/*
 * Workhouse routine for doing insertion into a GiST index. Note that
 * this routine assumes it is invoked in a short-lived memory context,
 * so it does not bother releasing palloc'd allocations.
 */
static void
gistdoinsert(Relation r, IndexTuple itup, GISTSTATE *giststate)
{
	GISTInsertState	state;

	memset(&state, 0, sizeof(GISTInsertState));

	state.itup = (IndexTuple *) palloc(sizeof(IndexTuple));
	state.itup[0] = (IndexTuple) palloc(IndexTupleSize(itup));
	memcpy(state.itup[0], itup, IndexTupleSize(itup));
	state.ituplen=1;
	state.r = r;
	state.key = itup->t_tid;
	state.needInsertComplete = true; 

	state.stack = (GISTInsertStack*)palloc(sizeof(GISTInsertStack));
	memset( state.stack, 0, sizeof(GISTInsertStack));
	state.stack->blkno=GIST_ROOT_BLKNO;

	gistfindleaf(&state, giststate);
	gistmakedeal(&state, giststate);
}

static bool
gistplacetopage(GISTInsertState *state, GISTSTATE *giststate) {
	bool is_splitted = false;

	if (gistnospace(state->stack->page, state->itup, state->ituplen))
	{
		/* no space for insertion */
		IndexTuple *itvec,
				   *newitup;
		int			tlen,olen;
		SplitedPageLayout	*dist=NULL, *ptr;

		is_splitted = true;
		itvec = gistextractbuffer(state->stack->buffer, &tlen);
		olen=tlen;
		itvec = gistjoinvector(itvec, &tlen, state->itup, state->ituplen);
		newitup = gistSplit(state->r, state->stack->buffer, itvec, &tlen, &dist, giststate);

		if ( !state->r->rd_istemp ) {
			OffsetNumber	noffs=0, offs[ MAXALIGN( sizeof(OffsetNumber) ) / sizeof(OffsetNumber) ];
			XLogRecPtr	recptr;
			XLogRecData	*rdata;
	
			if ( state->stack->todelete ) {
				offs[0] = state->stack->childoffnum;
				noffs=1;
			}

			rdata = formSplitRdata(state->r->rd_node, state->stack->blkno,
				offs, noffs, state->itup, state->ituplen, 
				&(state->key), state->path, state->pathlen, dist); 

			START_CRIT_SECTION();

			recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_PAGE_SPLIT, rdata);
			ptr = dist;
			while(ptr) {	
				PageSetLSN(BufferGetPage(ptr->buffer), recptr);
				PageSetTLI(BufferGetPage(ptr->buffer), ThisTimeLineID);
				ptr=ptr->next;
			}

			END_CRIT_SECTION();
		}

		ptr = dist;
		while(ptr) {
			WriteBuffer(ptr->buffer);
			ptr=ptr->next;
		}

		state->itup = newitup;
		state->ituplen = tlen;			/* now tlen >= 2 */

		if ( state->stack->blkno == GIST_ROOT_BLKNO ) {
			gistnewroot(state->r, state->itup, state->ituplen, &(state->key));
			state->needInsertComplete=false;
		}
		ReleaseBuffer(state->stack->buffer);
	}
	else
	{
		/* enough space */
		OffsetNumber off, l;
		bool is_leaf = (GistPageIsLeaf(state->stack->page)) ? true : false;

		off = (PageIsEmpty(state->stack->page)) ?
			FirstOffsetNumber
			:
			OffsetNumberNext(PageGetMaxOffsetNumber(state->stack->page));
		l = gistfillbuffer(state->r, state->stack->page, state->itup, state->ituplen, off);
		if ( !state->r->rd_istemp ) {
			OffsetNumber	noffs=0, offs[ MAXALIGN( sizeof(OffsetNumber) ) / sizeof(OffsetNumber) ];
			XLogRecPtr	recptr;
			XLogRecData	*rdata;
	
			if ( state->stack->todelete ) {
				offs[0] = state->stack->childoffnum;
				noffs=1;
			}
	
			rdata = formUpdateRdata(state->r->rd_node, state->stack->blkno,
				offs, noffs, false, state->itup, state->ituplen, 
				&(state->key), state->path, state->pathlen); 

			START_CRIT_SECTION();

			recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_ENTRY_UPDATE, rdata);
			PageSetLSN(state->stack->page, recptr);
			PageSetTLI(state->stack->page, ThisTimeLineID);

			END_CRIT_SECTION();
		}

		if ( state->stack->blkno == GIST_ROOT_BLKNO ) 
                        state->needInsertComplete=false;
		WriteBuffer(state->stack->buffer);

		if (state->ituplen > 1)
		{						/* previous is_splitted==true */
			/*
			 * child was splited, so we must form union for insertion in
			 * parent
			 */
			IndexTuple	newtup = gistunion(state->r, state->itup, state->ituplen, giststate);
			ItemPointerSetBlockNumber(&(newtup->t_tid), state->stack->blkno);
			state->itup[0] = newtup;
			state->ituplen = 1;
		} else if (is_leaf) {
			/* itup[0] store key to adjust parent, we set it to valid
			   to correct check by GistTupleIsInvalid macro in gistgetadjusted() */  
			ItemPointerSetBlockNumber(&(state->itup[0]->t_tid), state->stack->blkno);
			GistTupleSetValid( state->itup[0] );
		}
	}
	return is_splitted;
}

static void
gistfindleaf(GISTInsertState *state, GISTSTATE *giststate)
{
	ItemId		iid;
	IndexTuple	oldtup;
	GISTInsertStack	*ptr;

	/* walk down */
	while( true ) { 
		state->stack->buffer = gistReadBuffer(state->r, state->stack->blkno);
		state->stack->page = (Page) BufferGetPage(state->stack->buffer);

		if (!GistPageIsLeaf(state->stack->page))
		{
	        	/*
	         	* This is an internal page, so continue to walk down the
	         	* tree. We find the child node that has the minimum insertion
	         	* penalty and recursively invoke ourselves to modify that
	         	* node. Once the recursive call returns, we may need to
	         	* adjust the parent node for two reasons: the child node
	         	* split, or the key in this node needs to be adjusted for the
	         	* newly inserted key below us.
	         	*/
			GISTInsertStack	*item=(GISTInsertStack*)palloc(sizeof(GISTInsertStack));
	
			state->stack->childoffnum = gistchoose(state->r, state->stack->page, state->itup[0], giststate);

			iid = PageGetItemId(state->stack->page, state->stack->childoffnum);
			oldtup = (IndexTuple) PageGetItem(state->stack->page, iid);
			item->blkno = ItemPointerGetBlockNumber(&(oldtup->t_tid));
			item->parent = state->stack;
			item->todelete = false;
			state->stack = item;
		} else 
			break;
	}

	/* now state->stack->(page, buffer and blkno) points to leaf page, so insert */

	/* form state->path to work xlog */
	ptr = state->stack;
	state->pathlen=1;
	while( ptr ) {
		state->pathlen++;
		ptr=ptr->parent;
	}
	state->path=(BlockNumber*)palloc(MAXALIGN(sizeof(BlockNumber)*state->pathlen));
	ptr = state->stack;
	state->pathlen=0;
	while( ptr ) {
		state->path[ state->pathlen ] = ptr->blkno;
		state->pathlen++;
		ptr=ptr->parent;
	}
	state->pathlen--;
	state->path++;
}


void
gistmakedeal(GISTInsertState *state, GISTSTATE *giststate) {
	int			is_splitted;
	ItemId		iid;
	IndexTuple	oldtup, newtup;

	/* walk up */
	while( true ) {
                /*
                 * After this call: 1. if child page was splited, then itup
                 * contains keys for each page 2. if  child page wasn't splited,
                 * then itup contains additional for adjustment of current key
                 */

		is_splitted = gistplacetopage(state, giststate);

		/* pop page from stack */
		state->stack = state->stack->parent;
		state->pathlen--;
		state->path++;
	
		/* stack is void */
		if ( ! state->stack )
			break;


		/* child did not split */
		if (!is_splitted)
		{
			/* parent's tuple */
			iid = PageGetItemId(state->stack->page, state->stack->childoffnum);
			oldtup = (IndexTuple) PageGetItem(state->stack->page, iid);
			newtup = gistgetadjusted(state->r, oldtup, state->itup[0], giststate);
	
			if (!newtup) /* not need to update key */
				break;

			state->itup[0] = newtup;	
		}
	
	        /*
	         * This node's key has been modified, either because a child
	         * split occurred or because we needed to adjust our key for
	         * an insert in a child node. Therefore, remove the old
	         * version of this node's key.
	         */

		gistadjscans(state->r, GISTOP_DEL, state->stack->blkno, state->stack->childoffnum);
		PageIndexTupleDelete(state->stack->page, state->stack->childoffnum);
		if ( !state->r->rd_istemp ) 
			state->stack->todelete = true;
				
		/*
		 * if child was splitted, new key for child will be inserted in
		 * the end list of child, so we must say to any scans that page is
		 * changed beginning from 'child' offset
		 */
		if (is_splitted)
			gistadjscans(state->r, GISTOP_SPLIT, state->stack->blkno, state->stack->childoffnum);
	} /* while */

	/* release all buffers */
	while( state->stack ) {
		ReleaseBuffer(state->stack->buffer);
		state->stack = state->stack->parent;
	}

	/* say to xlog that insert is completed */
	if ( state->needInsertComplete && !state->r->rd_istemp )
		gistxlogInsertCompletion(state->r->rd_node, &(state->key), 1); 
}

static void 
gistToRealOffset(OffsetNumber *arr, int len, OffsetNumber *reasloffset) {
	int i;

	for(i=0;i<len;i++)
		arr[i] = reasloffset[ arr[i] ]; 
}

/*
 *	gistSplit -- split a page in the tree.
 */
IndexTuple *
gistSplit(Relation r,
		  Buffer buffer,
		  IndexTuple *itup,		/* contains compressed entry */
		  int *len,
		  SplitedPageLayout	**dist,
		  GISTSTATE *giststate)
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
	GistEntryVector *entryvec;
	int			i, fakeoffset,
				nlen;
	OffsetNumber	*realoffset;
	IndexTuple	*cleaneditup = itup;
	int	lencleaneditup = *len;

	p = (Page) BufferGetPage(buffer);
	opaque = (GISTPageOpaque) PageGetSpecialPointer(p);

	/*
	 * The root of the tree is the first block in the relation.  If we're
	 * about to split the root, we need to do some hocus-pocus to enforce
	 * this guarantee.
	 */
	if (BufferGetBlockNumber(buffer) == GIST_ROOT_BLKNO)
	{
		leftbuf = gistReadBuffer(r, P_NEW);
		GISTInitBuffer(leftbuf, opaque->flags&F_LEAF);
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

	rightbuf = gistReadBuffer(r, P_NEW);
	GISTInitBuffer(rightbuf, opaque->flags&F_LEAF);
	rbknum = BufferGetBlockNumber(rightbuf);
	right = (Page) BufferGetPage(rightbuf);

	/* generate the item array */
	realoffset = palloc((*len + 1) * sizeof(OffsetNumber));
	entryvec = palloc(GEVHDRSZ + (*len + 1) * sizeof(GISTENTRY));
	entryvec->n = *len + 1;

	fakeoffset = FirstOffsetNumber;
	for (i = 1; i <= *len; i++)
	{
		Datum		datum;
		bool		IsNull;

		if (!GistPageIsLeaf(p) && GistTupleIsInvalid( itup[i - 1] )) {
			entryvec->n--;
			/* remember position of invalid tuple */
			realoffset[ entryvec->n ] = i;
			continue;
		}

		datum = index_getattr(itup[i - 1], 1, giststate->tupdesc, &IsNull);
		gistdentryinit(giststate, 0, &(entryvec->vector[fakeoffset]),
					   datum, r, p, i,
					   ATTSIZE(datum, giststate->tupdesc, 1, IsNull),
					   FALSE, IsNull);
		realoffset[ fakeoffset ] = i;
		fakeoffset++;
	}

	/* 
         * if it was invalid tuple then we need special processing. If
	 * it's possible, we move all invalid tuples on right page.
         * We should remember, that union with invalid tuples 
 	 * is a invalid tuple. 
         */
	if ( entryvec->n != *len + 1 ) {
		lencleaneditup = entryvec->n-1;
		cleaneditup = (IndexTuple*)palloc(lencleaneditup * sizeof(IndexTuple));
		for(i=1;i<entryvec->n;i++)
			cleaneditup[i-1] = itup[ realoffset[ i ]-1 ];

		if ( gistnospace( left, cleaneditup, lencleaneditup ) ) {
			/* no space on left to put all good tuples, so picksplit */ 
			gistUserPicksplit(r, entryvec, &v, cleaneditup, lencleaneditup, giststate);
			v.spl_leftvalid = true;
			v.spl_rightvalid = false;
			gistToRealOffset( v.spl_left, v.spl_nleft, realoffset );
			gistToRealOffset( v.spl_right, v.spl_nright, realoffset );
	 	 } else { 
			/* we can try to store all valid tuples on one page */ 
			v.spl_right = (OffsetNumber*)palloc( entryvec->n * sizeof(OffsetNumber) );
			v.spl_left = (OffsetNumber*)palloc( entryvec->n * sizeof(OffsetNumber) );

			if ( lencleaneditup==0 ) {
				/* all tuples are invalid, so moves half of its to right */
				v.spl_leftvalid = v.spl_rightvalid = false;
				v.spl_nright = 0;
				v.spl_nleft = 0;
				for(i=1;i<=*len;i++) 
					if ( i-1<*len/2 )  
						v.spl_left[ v.spl_nleft++ ] = i;
					else
						v.spl_right[ v.spl_nright++ ] = i;
			} else { 
				/* we will not call gistUserPicksplit, just put good
				   tuples on left and invalid on right */
				v.spl_nleft = lencleaneditup;
				v.spl_nright = 0;
				for(i=1;i<entryvec->n;i++)
					v.spl_left[i-1] = i; 
				gistToRealOffset( v.spl_left, v.spl_nleft, realoffset );
				v.spl_lattr[0] = v.spl_ldatum = (Datum)0;
				v.spl_rattr[0] = v.spl_rdatum = (Datum)0;
				v.spl_lisnull[0] = true;
				v.spl_risnull[0] = true;
				gistunionsubkey(r, giststate, itup, &v, true);
				v.spl_leftvalid = true;
				v.spl_rightvalid = false;
			}
		}
	} else {
		/* there is no invalid tuples, so usial processing */ 
		gistUserPicksplit(r, entryvec, &v, itup, *len, giststate);
		v.spl_leftvalid = v.spl_rightvalid = true;
	}


	/* form left and right vector */
	lvectup = (IndexTuple *) palloc(sizeof(IndexTuple) * (*len+1));
	rvectup = (IndexTuple *) palloc(sizeof(IndexTuple) * (*len+1));

	for (i = 0; i < v.spl_nleft; i++)
		lvectup[i] = itup[v.spl_left[i] - 1];

	for (i = 0; i < v.spl_nright; i++)
		rvectup[i] = itup[v.spl_right[i] - 1];

	/* place invalid tuples on right page if itsn't done yet */
	for (fakeoffset = entryvec->n; fakeoffset < *len+1 && lencleaneditup; fakeoffset++) {
		rvectup[v.spl_nright++] = itup[realoffset[fakeoffset] - 1];
	}

	/* write on disk (may need another split) */
	if (gistnospace(right, rvectup, v.spl_nright))
	{
		int i;
		SplitedPageLayout *d, *origd=*dist;
	
		nlen = v.spl_nright;
		newtup = gistSplit(r, rightbuf, rvectup, &nlen, dist, giststate);
		/* XLOG stuff */
		d=*dist;
		/* translate offsetnumbers to our */
		while( d && d!=origd ) {
			for(i=0;i<d->block.num;i++)
				d->list[i] = v.spl_right[ d->list[i]-1 ]; 
			d=d->next;
		}
		ReleaseBuffer(rightbuf);
	}
	else
	{
		OffsetNumber l;

		l = gistfillbuffer(r, right, rvectup, v.spl_nright, FirstOffsetNumber);
		/* XLOG stuff */
		ROTATEDIST(*dist);
		(*dist)->block.blkno = BufferGetBlockNumber(rightbuf);
		(*dist)->block.num = v.spl_nright;
		(*dist)->list = v.spl_right;
		(*dist)->buffer = rightbuf;
 
		nlen = 1;
		newtup = (IndexTuple *) palloc(sizeof(IndexTuple) * 1);
		newtup[0] = ( v.spl_rightvalid ) ? gistFormTuple(giststate, r, v.spl_rattr, v.spl_rattrsize, v.spl_risnull)
				: gist_form_invalid_tuple( rbknum );
		ItemPointerSetBlockNumber(&(newtup[0]->t_tid), rbknum);
	}

	if (gistnospace(left, lvectup, v.spl_nleft))
	{
		int			llen = v.spl_nleft;
		IndexTuple *lntup;
		int i;
		SplitedPageLayout *d, *origd=*dist;

		lntup = gistSplit(r, leftbuf, lvectup, &llen, dist, giststate);

		/* XLOG stuff */
		d=*dist;
		/* translate offsetnumbers to our */
		while( d && d!=origd ) {
			for(i=0;i<d->block.num;i++)
				d->list[i] = v.spl_left[ d->list[i]-1 ]; 
			d=d->next;
		}
		
		ReleaseBuffer(leftbuf);

		newtup = gistjoinvector(newtup, &nlen, lntup, llen);
	}
	else
	{
		OffsetNumber l;

		l = gistfillbuffer(r, left, lvectup, v.spl_nleft, FirstOffsetNumber);
		if (BufferGetBlockNumber(buffer) != GIST_ROOT_BLKNO)
			PageRestoreTempPage(left, p);

		/* XLOG stuff */
		ROTATEDIST(*dist);
		(*dist)->block.blkno = BufferGetBlockNumber(leftbuf);
		(*dist)->block.num = v.spl_nleft;
		(*dist)->list = v.spl_left;
		(*dist)->buffer = leftbuf;
 
		nlen += 1;
		newtup = (IndexTuple *) repalloc(newtup, sizeof(IndexTuple) * nlen);
		newtup[nlen - 1] = ( v.spl_leftvalid ) ? gistFormTuple(giststate, r, v.spl_lattr, v.spl_lattrsize, v.spl_lisnull)
				: gist_form_invalid_tuple( lbknum );
		ItemPointerSetBlockNumber(&(newtup[nlen - 1]->t_tid), lbknum);
	}

	GistClearTuplesDeleted(p);
 
	*len = nlen;
	return newtup;
}

void
gistnewroot(Relation r, IndexTuple *itup, int len, ItemPointer key)
{
	Buffer		buffer;
	Page		page;

	buffer = gistReadBuffer(r, GIST_ROOT_BLKNO);
	GISTInitBuffer(buffer, 0);
	page = BufferGetPage(buffer);

	gistfillbuffer(r, page, itup, len, FirstOffsetNumber);
	if ( !r->rd_istemp ) {
		XLogRecPtr		recptr;
		XLogRecData		*rdata;
			
		rdata = formUpdateRdata(r->rd_node, GIST_ROOT_BLKNO,
			NULL, 0, false, itup, len, 
			key, NULL, 0); 
			
		START_CRIT_SECTION();

		recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_NEW_ROOT, rdata);
		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);

		END_CRIT_SECTION();
	}
	WriteBuffer(buffer);
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

