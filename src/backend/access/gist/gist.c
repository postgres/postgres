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
 *	  $PostgreSQL: pgsql/src/backend/access/gist/gist.c,v 1.119 2005/06/14 11:45:13 teodor Exp $
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
	MemoryContext tmpCxt;
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


typedef struct PageLayout {
	gistxlogPage	block;
	OffsetNumber	*list;
	Buffer		buffer; /* to write after all proceed */

	struct PageLayout *next;
} PageLayout;


#define ROTATEDIST(d) do { \
	PageLayout *tmp=(PageLayout*)palloc(sizeof(PageLayout)); \
	memset(tmp,0,sizeof(PageLayout)); \
	tmp->next = (d); \
	(d)=tmp; \
} while(0)
	

static IndexTuple *gistSplit(Relation r,
		  Buffer buffer,
		  IndexTuple *itup,
		  int *len,
		  PageLayout	**dist,
		  GISTSTATE *giststate);


#undef GISTDEBUG

#ifdef GISTDEBUG
static void gist_dumptree(Relation r, int level, BlockNumber blk, OffsetNumber coff);
#endif

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
	buffer = ReadBuffer(index, P_NEW);
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
	buildstate.tmpCxt = createTempGistContext();

	/* do the heap scan */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo,
								   gistbuildCallback, (void *) &buildstate);

	/* okay, all heap tuples are indexed */
	MemoryContextDelete(buildstate.tmpCxt);

	/* since we just counted the # of tuples, may as well update stats */
	IndexCloseAndUpdateStats(heap, reltuples, index, buildstate.indtuples);

	freeGISTstate(&buildstate.giststate);

#ifdef GISTDEBUG
	gist_dumptree(index, 0, GIST_ROOT_BLKNO, 0);
#endif
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
	MemoryContext oldCxt;

	/* GiST cannot index tuples with leading NULLs */
	if (isnull[0])
		return;

	oldCxt = MemoryContextSwitchTo(buildstate->tmpCxt);

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
	MemoryContextSwitchTo(oldCxt);
	MemoryContextReset(buildstate->tmpCxt);
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
	MemoryContext oldCxt;
	MemoryContext insertCxt;

	/*
	 * Since GIST is not marked "amconcurrent" in pg_am, caller should
	 * have acquired exclusive lock on index relation.	We need no locking
	 * here.
	 */

	/* GiST cannot index tuples with leading NULLs */
	if (isnull[0])
		PG_RETURN_BOOL(false);

	insertCxt = createTempGistContext();
	oldCxt = MemoryContextSwitchTo(insertCxt);

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
	MemoryContextSwitchTo(oldCxt);
	MemoryContextDelete(insertCxt);

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
	state.xlog_mode = false;

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
		PageLayout	*dist=NULL, *ptr;

		memset(&dist, 0, sizeof(PageLayout));
		is_splitted = true;
		itvec = gistextractbuffer(state->stack->buffer, &tlen);
		olen=tlen;
		itvec = gistjoinvector(itvec, &tlen, state->itup, state->ituplen);
		newitup = gistSplit(state->r, state->stack->buffer, itvec, &tlen, &dist, giststate);

		if ( !state->r->rd_istemp && !state->xlog_mode) {
			gistxlogPageSplit	xlrec;
			XLogRecPtr		recptr;
			XLogRecData		*rdata;
			int i, npage = 0, cur=1;

			ptr=dist;
			while( ptr ) {
				npage++;
				ptr=ptr->next;
			}

			rdata = (XLogRecData*)palloc(sizeof(XLogRecData)*(npage*2 + state->ituplen + 2));

			xlrec.node = state->r->rd_node;
			xlrec.origblkno = state->stack->blkno;
			xlrec.npage = npage;
			xlrec.nitup = state->ituplen;
			xlrec.todeleteoffnum = ( state->stack->todelete ) ? state->stack->childoffnum : InvalidOffsetNumber;
			xlrec.key = state->key;
			xlrec.pathlen = (uint16)state->pathlen;

			rdata[0].buffer = InvalidBuffer;
			rdata[0].data   = (char *) &xlrec;
			rdata[0].len    = sizeof( gistxlogPageSplit );
			rdata[0].next	= NULL;

			if ( state->pathlen>=0 ) {
				rdata[0].next	= &(rdata[1]);
				rdata[1].buffer = InvalidBuffer;
				rdata[1].data   = (char *) (state->path);
				rdata[1].len    = sizeof( BlockNumber ) * state->pathlen;
				rdata[1].next	= NULL;
				cur++;
			}
			
			/* new tuples */	
			for(i=0;i<state->ituplen;i++) {
				rdata[cur].buffer = InvalidBuffer;
				rdata[cur].data   = (char*)(state->itup[i]);
				rdata[cur].len	= IndexTupleSize(state->itup[i]);
				rdata[cur-1].next = &(rdata[cur]);
				cur++;
			}

			/* new page layout */
			ptr=dist;
			while(ptr) {
				rdata[cur].buffer = InvalidBuffer;
				rdata[cur].data   = (char*)&(ptr->block);
				rdata[cur].len  = sizeof(gistxlogPage);
				rdata[cur-1].next = &(rdata[cur]);
				cur++;

				rdata[cur].buffer = InvalidBuffer;
				rdata[cur].data   = (char*)(ptr->list);
				rdata[cur].len    = MAXALIGN(sizeof(OffsetNumber)*ptr->block.num);
				if ( rdata[cur].len > sizeof(OffsetNumber)*ptr->block.num )
					rdata[cur].data = repalloc( rdata[cur].data, rdata[cur].len );
				rdata[cur-1].next = &(rdata[cur]);
				rdata[cur].next=NULL;
				cur++;
				
				ptr=ptr->next;
			}

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
			gistnewroot(state->r, state->itup, state->ituplen, &(state->key), state->xlog_mode);
			state->needInsertComplete=false;
		}
		if ( state->xlog_mode ) 
			LockBuffer(state->stack->buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(state->stack->buffer);
	}
	else
	{
		/* enough space */
		OffsetNumber off, l;

		off = (PageIsEmpty(state->stack->page)) ?
			FirstOffsetNumber
			:
			OffsetNumberNext(PageGetMaxOffsetNumber(state->stack->page));
		l = gistfillbuffer(state->r, state->stack->page, state->itup, state->ituplen, off);
		if ( !state->r->rd_istemp && !state->xlog_mode) {
			gistxlogEntryUpdate	xlrec;
			XLogRecPtr		recptr;
			XLogRecData		*rdata = (XLogRecData*)palloc( sizeof(XLogRecData) * ( state->ituplen + 2 ) );
			int i, cur=0;
			
			xlrec.node = state->r->rd_node;
			xlrec.blkno = state->stack->blkno;
			xlrec.todeleteoffnum = ( state->stack->todelete ) ? state->stack->childoffnum : InvalidOffsetNumber;
			xlrec.key = state->key;
			xlrec.pathlen = (uint16)state->pathlen;

			rdata[0].buffer = InvalidBuffer;
			rdata[0].data   = (char *) &xlrec;
			rdata[0].len    = sizeof( gistxlogEntryUpdate );
			rdata[0].next   = NULL;

			if ( state->pathlen>=0 ) {
				rdata[0].next	= &(rdata[1]);
				rdata[1].buffer = InvalidBuffer;
				rdata[1].data   = (char *) (state->path);
				rdata[1].len    = sizeof( BlockNumber ) * state->pathlen;
				rdata[1].next	= NULL;
				cur++;
			}

			for(i=1; i<=state->ituplen; i++) { /* adding tuples */
				rdata[i+cur].buffer = InvalidBuffer;
				rdata[i+cur].data   = (char*)(state->itup[i-1]);
				rdata[i+cur].len	= IndexTupleSize(state->itup[i-1]);
				rdata[i+cur].next	= NULL;
				rdata[i-1+cur].next = &(rdata[i+cur]);
			}	
			
			START_CRIT_SECTION();

			recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_ENTRY_UPDATE, rdata);
			PageSetLSN(state->stack->page, recptr);
			PageSetTLI(state->stack->page, ThisTimeLineID);

			END_CRIT_SECTION();
		}

		if ( state->stack->blkno == GIST_ROOT_BLKNO ) 
                        state->needInsertComplete=false;

		if ( state->xlog_mode ) 
			LockBuffer(state->stack->buffer, BUFFER_LOCK_UNLOCK);
		WriteBuffer(state->stack->buffer);

		if (state->ituplen > 1)
		{						/* previous is_splitted==true */
			/*
			 * child was splited, so we must form union for insertion in
			 * parent
			 */
			IndexTuple	newtup = gistunion(state->r, state->itup, state->ituplen, giststate);
			ItemPointerSet(&(newtup->t_tid), state->stack->blkno, FirstOffsetNumber);
			state->itup[0] = newtup;
			state->ituplen = 1;
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
		GISTPageOpaque opaque;

		state->stack->buffer = ReadBuffer(state->r, state->stack->blkno);
		state->stack->page = (Page) BufferGetPage(state->stack->buffer);
		opaque = (GISTPageOpaque) PageGetSpecialPointer(state->stack->page);
	
		if (!(opaque->flags & F_LEAF))
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
	state->path=(BlockNumber*)palloc(sizeof(BlockNumber)*state->pathlen);
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

		is_splitted = gistplacetopage(state, giststate );

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
		if ( state->xlog_mode ) 
			LockBuffer(state->stack->buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(state->stack->buffer);
		state->stack = state->stack->parent;
	}

	/* say to xlog that insert is completed */
	if ( !state->xlog_mode && state->needInsertComplete && !state->r->rd_istemp ) {
		gistxlogInsertComplete	xlrec;
		XLogRecData		rdata;
			
		xlrec.node = state->r->rd_node;
		xlrec.key = state->key;
			
		rdata.buffer = InvalidBuffer;
		rdata.data   = (char *) &xlrec;
		rdata.len    = sizeof( gistxlogInsertComplete );
		rdata.next   = NULL;

		START_CRIT_SECTION();

		XLogInsert(RM_GIST_ID, XLOG_GIST_INSERT_COMPLETE, &rdata);

		END_CRIT_SECTION();
	}
}

/*
 *	gistSplit -- split a page in the tree.
 */
static IndexTuple *
gistSplit(Relation r,
		  Buffer buffer,
		  IndexTuple *itup,		/* contains compressed entry */
		  int *len,
		  PageLayout	**dist,
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
	int			i,
				nlen;

	p = (Page) BufferGetPage(buffer);
	opaque = (GISTPageOpaque) PageGetSpecialPointer(p);

	/*
	 * The root of the tree is the first block in the relation.  If we're
	 * about to split the root, we need to do some hocus-pocus to enforce
	 * this guarantee.
	 */
	if (BufferGetBlockNumber(buffer) == GIST_ROOT_BLKNO)
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
	entryvec = palloc(GEVHDRSZ + (*len + 1) * sizeof(GISTENTRY));
	entryvec->n = *len + 1;

	for (i = 1; i <= *len; i++)
	{
		Datum		datum;
		bool		IsNull;

		datum = index_getattr(itup[i - 1], 1, giststate->tupdesc, &IsNull);
		gistdentryinit(giststate, 0, &(entryvec->vector[i]),
					   datum, r, p, i,
					   ATTSIZE(datum, giststate->tupdesc, 1, IsNull),
					   FALSE, IsNull);
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
		int			MaxGrpId;

		v.spl_idgrp = (int *) palloc0(sizeof(int) * (*len + 1));
		v.spl_grpflag = (char *) palloc0(sizeof(char) * (*len + 1));
		v.spl_ngrp = (int *) palloc(sizeof(int) * (*len + 1));

		MaxGrpId = gistfindgroup(giststate, entryvec->vector, &v);

		/* form union of sub keys for each page (l,p) */
		gistunionsubkey(r, giststate, itup, &v);

		/*
		 * if possible, we insert equivalent tuples with control by
		 * penalty for a subkey(s)
		 */
		if (MaxGrpId > 1)
			gistadjsubkey(r, itup, len, &v, giststate);
	}

	/* form left and right vector */
	lvectup = (IndexTuple *) palloc(sizeof(IndexTuple) * v.spl_nleft);
	rvectup = (IndexTuple *) palloc(sizeof(IndexTuple) * v.spl_nright);

	for (i = 0; i < v.spl_nleft; i++)
		lvectup[i] = itup[v.spl_left[i] - 1];

	for (i = 0; i < v.spl_nright; i++)
		rvectup[i] = itup[v.spl_right[i] - 1];


	/* write on disk (may need another split) */
	if (gistnospace(right, rvectup, v.spl_nright))
	{
		int i;
		PageLayout *d, *origd=*dist;
	
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
		newtup[0] = gistFormTuple(giststate, r, v.spl_rattr, v.spl_rattrsize, v.spl_risnull);
		ItemPointerSet(&(newtup[0]->t_tid), rbknum, FirstOffsetNumber);
	}

	if (gistnospace(left, lvectup, v.spl_nleft))
	{
		int			llen = v.spl_nleft;
		IndexTuple *lntup;
		int i;
		PageLayout *d, *origd=*dist;

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
		newtup[nlen - 1] = gistFormTuple(giststate, r, v.spl_lattr, v.spl_lattrsize, v.spl_lisnull);
		ItemPointerSet(&(newtup[nlen - 1]->t_tid), lbknum, FirstOffsetNumber);
	}

	*len = nlen;
	return newtup;
}

void
gistnewroot(Relation r, IndexTuple *itup, int len, ItemPointer key, bool xlog_mode)
{
	Buffer		buffer;
	Page		page;

	buffer = (xlog_mode) ? XLogReadBuffer(false, r, GIST_ROOT_BLKNO) : ReadBuffer(r, GIST_ROOT_BLKNO);
	GISTInitBuffer(buffer, 0);
	page = BufferGetPage(buffer);

	gistfillbuffer(r, page, itup, len, FirstOffsetNumber);
	if ( !xlog_mode && !r->rd_istemp ) {
		gistxlogEntryUpdate	xlrec;
		XLogRecPtr		recptr;
		XLogRecData		*rdata = (XLogRecData*)palloc( sizeof(XLogRecData) * ( len + 1 ) );
		int i;
			
		xlrec.node = r->rd_node;
		xlrec.blkno = GIST_ROOT_BLKNO;
		xlrec.todeleteoffnum = InvalidOffsetNumber;
		xlrec.key = *key;
		xlrec.pathlen=0;
			
		rdata[0].buffer = InvalidBuffer;
		rdata[0].data   = (char *) &xlrec;
		rdata[0].len    = sizeof( gistxlogEntryUpdate );
		rdata[0].next   = NULL;

		for(i=1; i<=len; i++) {
			rdata[i].buffer = InvalidBuffer;
			rdata[i].data   = (char*)(itup[i-1]);
			rdata[i].len	= IndexTupleSize(itup[i-1]);
			rdata[i].next	= NULL;
			rdata[i-1].next = &(rdata[i]);
		}	
			
		START_CRIT_SECTION();

		recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_NEW_ROOT, rdata);
		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);

		END_CRIT_SECTION();
	}
	if ( xlog_mode ) 
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);
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
	iscan = index_beginscan(NULL, rel, SnapshotAny, 0, NULL);
	/* including killed tuples */
	iscan->ignore_killed_tuples = false;

	while (index_getnext_indexitem(iscan, ForwardScanDirection))
	{
		vacuum_delay_point();

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
			if ( !rel->rd_istemp ) {
			 	gistxlogEntryUpdate	xlrec;
				XLogRecPtr		recptr;
				XLogRecData		rdata;
			
				xlrec.node = rel->rd_node;
				xlrec.blkno = blkno;
				xlrec.todeleteoffnum = offnum;
				xlrec.pathlen=0;
				ItemPointerSetInvalid( &(xlrec.key) );
			
				rdata.buffer = InvalidBuffer;
				rdata.data   = (char *) &xlrec;
				rdata.len    = sizeof( gistxlogEntryUpdate );
				rdata.next   = NULL;

				START_CRIT_SECTION();

				recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_ENTRY_DELETE, &rdata);
				PageSetLSN(page, recptr);
				PageSetTLI(page, ThisTimeLineID);

				END_CRIT_SECTION();
			}

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

