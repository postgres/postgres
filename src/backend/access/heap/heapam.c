/*-------------------------------------------------------------------------
 *
 * heapam.c--
 *    heap access method code
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/heap/heapam.c,v 1.15 1997/08/27 09:00:20 vadim Exp $
 *
 *
 * INTERFACE ROUTINES
 *	heapgettup 	- fetch next heap tuple from a scan
 *	heap_open 	- open a heap relation by relationId
 *	heap_openr 	- open a heap relation by name
 *	heap_close 	- close a heap relation
 *	heap_beginscan	- begin relation scan
 *	heap_rescan	- restart a relation scan
 *	heap_endscan	- end relation scan
 *	heap_getnext	- retrieve next tuple in scan
 *	heap_fetch	- retrive tuple with tid
 *	heap_insert	- insert tuple into a relation
 *	heap_delete	- delete a tuple from a relation
 *	heap_replace	- replace a tuple in a relation with another tuple
 *	heap_markpos	- mark scan position
 *	heap_restrpos	- restore position to marked location
 *	
 * NOTES
 *    This file contains the heap_ routines which implement
 *    the POSTGRES heap access method used for all POSTGRES
 *    relations.  
 *
 * OLD COMMENTS
 *	struct relscan hints:  (struct should be made AM independent?)
 *
 *	rs_ctid is the tid of the last tuple returned by getnext.
 *	rs_ptid and rs_ntid are the tids of the previous and next tuples
 *	returned by getnext, respectively.  NULL indicates an end of
 *	scan (either direction); NON indicates an unknow value.
 *
 *	possible combinations:
 *	rs_p	rs_c	rs_n		interpretation
 *	NULL	NULL	NULL		empty scan
 *	NULL	NULL	NON		at begining of scan
 *	NULL	NULL	t1		at begining of scan (with cached tid)
 *	NON	NULL	NULL		at end of scan
 *	t1	NULL	NULL		at end of scan (with cached tid)
 *	NULL	t1	NULL		just returned only tuple
 *	NULL	t1	NON		just returned first tuple
 *	NULL	t1	t2		returned first tuple (with cached tid)
 *	NON	t1	NULL		just returned last tuple
 *	t2	t1	NULL		returned last tuple (with cached tid)
 *	t1	t2	NON		in the middle of a forward scan
 *	NON	t2	t1		in the middle of a reverse scan
 *	ti	tj	tk		in the middle of a scan (w cached tid)
 *
 *	Here NULL is ...tup == NULL && ...buf == InvalidBuffer,
 *	and NON is ...tup == NULL && ...buf == UnknownBuffer.
 *
 *	Currently, the NONTID values are not cached with their actual
 *	values by getnext.  Values may be cached by markpos since it stores
 *	all three tids.
 *
 *	NOTE:  the calls to elog() must stop.  Should decide on an interface
 *	between the general and specific AM calls.
 *
 * 	XXX probably do not need a free tuple routine for heaps.
 * 	Huh?  Free tuple is not necessary for tuples returned by scans, but
 * 	is necessary for tuples which are returned by
 *	RelationGetTupleByItemPointer. -hirohama
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <storage/bufpage.h>
#include <access/heapam.h>
#include <miscadmin.h>
#include <utils/relcache.h>
#include <access/valid.h>
#include <access/hio.h>
#include <storage/lmgr.h>
#include <storage/smgr.h>
#include <catalog/catalog.h>
#include <access/transam.h>
#include <access/xact.h>
#include <utils/inval.h>
#include <utils/memutils.h>


#ifndef HAVE_MEMMOVE
# include <regex/utils.h>
#else
# include <string.h>
#endif

static bool	ImmediateInvalidation;

/* ----------------------------------------------------------------
 *                       heap support routines
 * ----------------------------------------------------------------
 */

/* ----------------
 *	initsdesc - sdesc code common to heap_beginscan and heap_rescan
 * ----------------
 */
static void
initsdesc(HeapScanDesc sdesc,
	  Relation relation,
	  int atend,
	  unsigned nkeys,
	  ScanKey key)
{
    if (!RelationGetNumberOfBlocks(relation)) {
	/* ----------------
	 *  relation is empty
	 * ----------------
	 */
	sdesc->rs_ntup = sdesc->rs_ctup = sdesc->rs_ptup = NULL;
	sdesc->rs_nbuf = sdesc->rs_cbuf = sdesc->rs_pbuf = InvalidBuffer;
    } else if (atend) {
	/* ----------------
	 *  reverse scan
	 * ----------------
	 */
	sdesc->rs_ntup = sdesc->rs_ctup = NULL;
	sdesc->rs_nbuf = sdesc->rs_cbuf = InvalidBuffer;
	sdesc->rs_ptup = NULL;
	sdesc->rs_pbuf = UnknownBuffer;
    } else {
	/* ----------------
	 *  forward scan
	 * ----------------
	 */
	sdesc->rs_ctup = sdesc->rs_ptup = NULL;
	sdesc->rs_cbuf = sdesc->rs_pbuf = InvalidBuffer;
	sdesc->rs_ntup = NULL;
	sdesc->rs_nbuf = UnknownBuffer;
    } /* invalid too */
    
    /* we don't have a marked position... */
    ItemPointerSetInvalid(&(sdesc->rs_mptid));
    ItemPointerSetInvalid(&(sdesc->rs_mctid));
    ItemPointerSetInvalid(&(sdesc->rs_mntid));
    ItemPointerSetInvalid(&(sdesc->rs_mcd));
    
    /* ----------------
     *	copy the scan key, if appropriate
     * ----------------
     */
    if (key != NULL)
	memmove(sdesc->rs_key, key, nkeys * sizeof(ScanKeyData));
}

/* ----------------
 *	unpinsdesc - code common to heap_rescan and heap_endscan
 * ----------------
 */
static void
unpinsdesc(HeapScanDesc sdesc)
{
    if (BufferIsValid(sdesc->rs_pbuf)) {
	ReleaseBuffer(sdesc->rs_pbuf);
    }
    
    /* ------------------------------------
     *  Scan will pin buffer one for each non-NULL tuple pointer
     *  (ptup, ctup, ntup), so they have to be unpinned multiple
     *  times.
     * ------------------------------------
     */
    if (BufferIsValid(sdesc->rs_cbuf)) {
	ReleaseBuffer(sdesc->rs_cbuf);
    }
    
    if (BufferIsValid(sdesc->rs_nbuf)) {
	ReleaseBuffer(sdesc->rs_nbuf);
    }
}

/* ------------------------------------------
 *	nextpage
 *
 *	figure out the next page to scan after the current page
 *	taking into account of possible adjustment of degrees of
 *	parallelism
 * ------------------------------------------
 */
static int
nextpage(int page, int dir)
{
    return((dir<0)?page-1:page+1);
}

/* ----------------
 *	heapgettup - fetch next heap tuple
 *
 *	routine used by heap_getnext() which does most of the
 *	real work in scanning tuples.
 * ----------------
 */
static HeapTuple 
heapgettup(Relation relation,
	   ItemPointer tid,
	   int dir,
	   Buffer *b,
	   TimeQual timeQual,
	   int nkeys,
	   ScanKey key)
{
    ItemId		lpp;
    Page		dp;
    int			page;
    int			pages;
    int			lines;
    HeapTuple		rtup;
    OffsetNumber	lineoff;
    int			linesleft;
    
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_heapgettup);
    IncrHeapAccessStat(global_heapgettup);
    
    /* ----------------
     *	debugging stuff 
     *
     * check validity of arguments, here and for other functions too
     * Note: no locking manipulations needed--this is a local function
     * ----------------
     */
#ifdef	HEAPDEBUGALL
    if (ItemPointerIsValid(tid)) {
	elog(DEBUG, "heapgettup(%.16s, tid=0x%x[%d,%d], dir=%d, ...)",
	     RelationGetRelationName(relation), tid, tid->ip_blkid,
	     tid->ip_posid, dir);
    } else {
	elog(DEBUG, "heapgettup(%.16s, tid=0x%x, dir=%d, ...)",
	     RelationGetRelationName(relation), tid, dir);
    }
    elog(DEBUG, "heapgettup(..., b=0x%x, timeQ=0x%x, nkeys=%d, key=0x%x",
	 b, timeQual, nkeys, key);
    if (timeQual == SelfTimeQual) {
	elog(DEBUG, "heapgettup: relation(%c)=`%.16s', SelfTimeQual",
	     relation->rd_rel->relkind, &relation->rd_rel->relname);
    } else {
	elog(DEBUG, "heapgettup: relation(%c)=`%.16s', timeQual=%d",
	     relation->rd_rel->relkind, &relation->rd_rel->relname,
	     timeQual);
    }
#endif	/* !defined(HEAPDEBUGALL) */
    
    if (!ItemPointerIsValid(tid)) {
	Assert(!PointerIsValid(tid));
    }
    
    /* ----------------
     *	return null immediately if relation is empty
     * ----------------
     */
    if (!(pages = relation->rd_nblocks))
	return (NULL);
    
    /* ----------------
     *	calculate next starting lineoff, given scan direction
     * ----------------
     */
    if (!dir) {
	/* ----------------
	 * ``no movement'' scan direction
	 * ----------------
	 */
	/* assume it is a valid TID XXX	*/
	if (ItemPointerIsValid(tid) == false) {
	    *b = InvalidBuffer;
	    return (NULL);
	}
	*b = RelationGetBufferWithBuffer(relation,
			                 ItemPointerGetBlockNumber(tid),
			                 *b);
	
#ifndef NO_BUFFERISVALID
	if (!BufferIsValid(*b)) {
	    elog(WARN, "heapgettup: failed ReadBuffer");
	}
#endif
	
	dp = (Page) BufferGetPage(*b);
	lineoff = ItemPointerGetOffsetNumber(tid);
	lpp = PageGetItemId(dp, lineoff);
	
	rtup = (HeapTuple)PageGetItem((Page) dp, lpp);
	return (rtup);
	
    } else if (dir < 0) {
	/* ----------------
	 *  reverse scan direction
	 * ----------------
	 */
	if (ItemPointerIsValid(tid) == false) {
	    tid = NULL;
	}
	if (tid == NULL) {
	    page = pages - 1;				/* final page */
	} else {
	    page = ItemPointerGetBlockNumber(tid);	/* current page */
	}
	if (page < 0) {	
	    *b = InvalidBuffer;
	    return (NULL);
	}
	
	*b = RelationGetBufferWithBuffer(relation, page, *b);
#ifndef NO_BUFFERISVALID
	if (!BufferIsValid(*b)) {
	    elog(WARN, "heapgettup: failed ReadBuffer");
	}
#endif
	
	dp = (Page) BufferGetPage(*b);
	lines = PageGetMaxOffsetNumber(dp);
	if (tid == NULL) {
	    lineoff = lines;				/* final offnum */
	} else {
	    lineoff =					/* previous offnum */
		OffsetNumberPrev(ItemPointerGetOffsetNumber(tid));
	}
	/* page and lineoff now reference the physically previous tid */

    } else {
	/* ----------------
	 *  forward scan direction
	 * ----------------
	 */
	if (ItemPointerIsValid(tid) == false) {
	    page = 0;					/* first page */
	    lineoff = FirstOffsetNumber;		/* first offnum */
	} else {
	    page = ItemPointerGetBlockNumber(tid);	/* current page */
	    lineoff =					/* next offnum */
		OffsetNumberNext(ItemPointerGetOffsetNumber(tid));
	}
	
	if (page >= pages) {
	    *b = InvalidBuffer;
	    return (NULL);
	}
	/* page and lineoff now reference the physically next tid */

	*b = RelationGetBufferWithBuffer(relation, page, *b);
#ifndef NO_BUFFERISVALID
	if (!BufferIsValid(*b)) {
	    elog(WARN, "heapgettup: failed ReadBuffer");
	}
#endif
	
	dp = (Page) BufferGetPage(*b);
	lines = PageGetMaxOffsetNumber(dp);
    }
    
    /* 'dir' is now non-zero */

    /* ----------------
     *	calculate line pointer and number of remaining items
     *  to check on this page.
     * ----------------
     */
    lpp = PageGetItemId(dp, lineoff);
    if (dir < 0) {
	linesleft = lineoff - 1;
    } else {
	linesleft = lines - lineoff;
    }

    /* ----------------
     *	advance the scan until we find a qualifying tuple or
     *  run out of stuff to scan
     * ----------------
     */
    for (;;) {
	while (linesleft >= 0) {
	    /* ----------------
	     *	if current tuple qualifies, return it.
	     * ----------------
	     */
	    if ((rtup = heap_tuple_satisfies(lpp, relation, *b, (PageHeader) dp,
					     timeQual, nkeys, key)) != NULL) {
		ItemPointer iptr = &(rtup->t_ctid); 
		if (ItemPointerGetBlockNumber(iptr) != page) {
		    /*
		     * set block id to the correct page number
		     * --- this is a hack to support the virtual fragment
		     * concept
		     */
		    ItemPointerSetBlockNumber(iptr, page);
		}
		return (rtup);
	    }
	    
	    /* ----------------
	     *	otherwise move to the next item on the page
	     * ----------------
	     */
	    --linesleft;
	    if (dir < 0) {
		--lpp;	/* move back in this page's ItemId array */
	    } else {
		++lpp;	/* move forward in this page's ItemId array */
	    }
	}
	
	/* ----------------
	 *  if we get here, it means we've exhausted the items on
	 *  this page and it's time to move to the next..
	 * ----------------
	 */
	page = nextpage(page, dir);
	
	/* ----------------
	 *  return NULL if we've exhausted all the pages..
	 * ----------------
	 */
	if (page < 0 || page >= pages) {
	    if (BufferIsValid(*b))
		ReleaseBuffer(*b);
	    *b = InvalidBuffer;
	    return (NULL);
	}
	
	*b = ReleaseAndReadBuffer(*b, relation, page);
	
#ifndef NO_BUFFERISVALID
	if (!BufferIsValid(*b)) {
	    elog(WARN, "heapgettup: failed ReadBuffer");
	}
#endif
	dp = (Page) BufferGetPage(*b);
	lines = lineoff = PageGetMaxOffsetNumber((Page) dp);
	linesleft = lines - 1;
	if (dir < 0) {
	    lpp = PageGetItemId(dp, lineoff);
	} else {
	    lpp = PageGetItemId(dp, FirstOffsetNumber);
	}
    }
}

void
doinsert(Relation relation, HeapTuple tup)
{
    RelationPutHeapTupleAtEnd(relation, tup);
    return;
}

/* 
 *	HeapScanIsValid is now a macro in relscan.h -cim 4/27/91
 */

#ifdef NOT_USED
/* ----------------
 *	SetHeapAccessMethodImmediateInvalidation
 * ----------------
 */
void
SetHeapAccessMethodImmediateInvalidation(bool on)
{
    ImmediateInvalidation = on;
}
#endif

/* ----------------------------------------------------------------
 *                   heap access method interface
 * ----------------------------------------------------------------
 */
/* ----------------
 *	heap_open - open a heap relation by relationId
 *
 *	presently the relcache routines do all the work we need
 *	to open/close heap relations.
 * ----------------
 */
Relation
heap_open(Oid relationId)
{
    Relation r;
    
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_open);
    IncrHeapAccessStat(global_open);
    
    r = (Relation) RelationIdGetRelation(relationId);

    if (RelationIsValid(r) && r->rd_rel->relkind == RELKIND_INDEX) {
	elog(WARN, "%s is an index relation", r->rd_rel->relname.data);
    }
    
    return (r);
}

/* ----------------
 *	heap_openr - open a heap relation by name
 *
 *	presently the relcache routines do all the work we need
 *	to open/close heap relations.
 * ----------------
 */
Relation
heap_openr(char *relationName)
{
    Relation r;
    
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_openr);
    IncrHeapAccessStat(global_openr);
    
    r = RelationNameGetRelation(relationName);
    
    if (RelationIsValid(r) && r->rd_rel->relkind == RELKIND_INDEX) {
	elog(WARN, "%s is an index relation", r->rd_rel->relname.data);
    }
    
    return (r);
}

/* ----------------
 *	heap_close - close a heap relation
 *
 *	presently the relcache routines do all the work we need
 *	to open/close heap relations.
 * ----------------
 */
void
heap_close(Relation relation)
{
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_close);
    IncrHeapAccessStat(global_close);
    
    RelationClose(relation);
}


/* ----------------
 *	heap_beginscan	- begin relation scan
 * ----------------
 */
HeapScanDesc
heap_beginscan(Relation relation,
	       int atend,
	       TimeQual timeQual,
	       unsigned nkeys,
	       ScanKey key)
{
    HeapScanDesc	sdesc;
    
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_beginscan);
    IncrHeapAccessStat(global_beginscan);
    
    /* ----------------
     *	sanity checks
     * ----------------
     */
    if (RelationIsValid(relation) == false)
	elog(WARN, "heap_beginscan: !RelationIsValid(relation)");
    
    /* ----------------
     * set relation level read lock
     * ----------------
     */
    RelationSetLockForRead(relation);
    
    /* XXX someday assert SelfTimeQual if relkind == RELKIND_UNCATALOGED */
    if (relation->rd_rel->relkind == RELKIND_UNCATALOGED) {
	timeQual = SelfTimeQual;
    }
    
    /* ----------------
     *  increment relation ref count while scanning relation
     * ----------------
     */
    RelationIncrementReferenceCount(relation);
    
    /* ----------------
     *	allocate and initialize scan descriptor
     * ----------------
     */
    sdesc = (HeapScanDesc) palloc(sizeof(HeapScanDescData));
    
    relation->rd_nblocks = smgrnblocks(relation->rd_rel->relsmgr, relation);
    sdesc->rs_rd = relation;
    
    if (nkeys) {
	/*
	 * we do this here instead of in initsdesc() because heap_rescan also
	 * calls initsdesc() and we don't want to allocate memory again
	 */
	sdesc->rs_key = (ScanKey) palloc(sizeof(ScanKeyData) * nkeys);
    } else {
	sdesc->rs_key = NULL;
    }

    initsdesc(sdesc, relation, atend, nkeys, key);
    
    sdesc->rs_atend = atend;
    sdesc->rs_tr = timeQual;
    sdesc->rs_nkeys = (short)nkeys;
    
    return (sdesc);
}

/* ----------------
 *	heap_rescan	- restart a relation scan
 * ----------------
 */
void
heap_rescan(HeapScanDesc sdesc,
	    bool scanFromEnd,
	    ScanKey key)
{
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_rescan);
    IncrHeapAccessStat(global_rescan);
    
    /* Note: set relation level read lock is still set */
    
    /* ----------------
     *	unpin scan buffers
     * ----------------
     */
    unpinsdesc(sdesc);
    
    /* ----------------
     *	reinitialize scan descriptor
     * ----------------
     */
    initsdesc(sdesc, sdesc->rs_rd, scanFromEnd, sdesc->rs_nkeys, key);
    sdesc->rs_atend = (bool) scanFromEnd;
}

/* ----------------
 *	heap_endscan	- end relation scan
 *
 *	See how to integrate with index scans.
 *	Check handling if reldesc caching.
 * ----------------
 */
void
heap_endscan(HeapScanDesc sdesc)
{
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_endscan);    
    IncrHeapAccessStat(global_endscan);    
    
    /* Note: no locking manipulations needed */
    
    /* ----------------
     *	unpin scan buffers
     * ----------------
     */
    unpinsdesc(sdesc);
    
    /* ----------------
     *	decrement relation reference count and free scan descriptor storage
     * ----------------
     */
    RelationDecrementReferenceCount(sdesc->rs_rd);
    
    /* ----------------
     * Non 2-phase read locks on catalog relations
     * ----------------
     */
    if ( IsSystemRelationName(RelationGetRelationName(sdesc->rs_rd)->data) )

	RelationUnsetLockForRead(sdesc->rs_rd);
    
    pfree(sdesc);	/* XXX */
}

/* ----------------
 *	heap_getnext	- retrieve next tuple in scan
 *
 *	Fix to work with index relations.
 * ----------------
 */

#ifdef HEAPDEBUGALL
#define HEAPDEBUG_1 \
elog(DEBUG, "heap_getnext([%s,nkeys=%d],backw=%d,0x%x) called", \
     sdesc->rs_rd->rd_rel->relname.data, sdesc->rs_nkeys, backw, b)
     
#define HEAPDEBUG_2 \
     elog(DEBUG, "heap_getnext called with backw (no tracing yet)")
     
#define HEAPDEBUG_3 \
     elog(DEBUG, "heap_getnext returns NULL at end")
     
#define HEAPDEBUG_4 \
     elog(DEBUG, "heap_getnext valid buffer UNPIN'd")
     
#define HEAPDEBUG_5 \
     elog(DEBUG, "heap_getnext next tuple was cached")
     
#define HEAPDEBUG_6 \
     elog(DEBUG, "heap_getnext returning EOS")
     
#define HEAPDEBUG_7 \
     elog(DEBUG, "heap_getnext returning tuple");
#else
#define HEAPDEBUG_1
#define HEAPDEBUG_2
#define HEAPDEBUG_3
#define HEAPDEBUG_4
#define HEAPDEBUG_5
#define HEAPDEBUG_6
#define HEAPDEBUG_7
#endif	/* !defined(HEAPDEBUGALL) */
     
     
HeapTuple
heap_getnext(HeapScanDesc scandesc,
	     int backw,
	     Buffer *b)
{
    register HeapScanDesc sdesc = scandesc;
    Buffer		  localb;
    
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_getnext);    
    IncrHeapAccessStat(global_getnext);    
    
    /* Note: no locking manipulations needed */
    
    /* ----------------
     *	argument checks
     * ----------------
     */
    if (sdesc == NULL)
	elog(WARN, "heap_getnext: NULL relscan");
    
    /* ----------------
     *	initialize return buffer to InvalidBuffer
     * ----------------
     */
    if (! PointerIsValid(b)) b = &localb;
    (*b) = InvalidBuffer;
    
    HEAPDEBUG_1; /* heap_getnext( info ) */
    
    if (backw) {
	/* ----------------
	 *  handle reverse scan
	 * ----------------
	 */
	HEAPDEBUG_2; /* heap_getnext called with backw */
	
	if (sdesc->rs_ptup == sdesc->rs_ctup &&
	    BufferIsInvalid(sdesc->rs_pbuf))
	    {
		if (BufferIsValid(sdesc->rs_nbuf))
		    ReleaseBuffer(sdesc->rs_nbuf);
		return (NULL);
	    }
	
	/*
	 * Copy the "current" tuple/buffer
	 * to "next". Pin/unpin the buffers
	 * accordingly
	 */
	if (sdesc->rs_nbuf != sdesc->rs_cbuf) {
	    if (BufferIsValid(sdesc->rs_nbuf))
		ReleaseBuffer(sdesc->rs_nbuf);
	    if (BufferIsValid(sdesc->rs_cbuf))
		IncrBufferRefCount(sdesc->rs_cbuf);
	}
	sdesc->rs_ntup = sdesc->rs_ctup;
	sdesc->rs_nbuf = sdesc->rs_cbuf;
	
	if (sdesc->rs_ptup != NULL) {
	    if (sdesc->rs_cbuf != sdesc->rs_pbuf) {
		if (BufferIsValid(sdesc->rs_cbuf))
		    ReleaseBuffer(sdesc->rs_cbuf);
		if (BufferIsValid(sdesc->rs_pbuf))
		    IncrBufferRefCount(sdesc->rs_pbuf);
	    }
	    sdesc->rs_ctup = sdesc->rs_ptup;
	    sdesc->rs_cbuf = sdesc->rs_pbuf;
	} else { /* NONTUP */
	    ItemPointer iptr;
	    
	    iptr = (sdesc->rs_ctup != NULL) ?
		&(sdesc->rs_ctup->t_ctid) : (ItemPointer) NULL;
	    
            /* Don't release sdesc->rs_cbuf at this point, because
               heapgettup doesn't increase PrivateRefCount if it
               is already set. On a backward scan, both rs_ctup and rs_ntup
               usually point to the same buffer page, so
               PrivateRefCount[rs_cbuf] should be 2 (or more, if for instance
               ctup is stored in a TupleTableSlot).  - 01/09/94 */
	    
	    sdesc->rs_ctup = (HeapTuple)
		heapgettup(sdesc->rs_rd,
			   iptr,
			   -1,
			   &(sdesc->rs_cbuf),
			   sdesc->rs_tr,
			   sdesc->rs_nkeys,
			   sdesc->rs_key);
	}
	
	if (sdesc->rs_ctup == NULL && !BufferIsValid(sdesc->rs_cbuf))
	    {
		if (BufferIsValid(sdesc->rs_pbuf))
		    ReleaseBuffer(sdesc->rs_pbuf);
		sdesc->rs_ptup = NULL;
		sdesc->rs_pbuf = InvalidBuffer;
		if (BufferIsValid(sdesc->rs_nbuf))
		    ReleaseBuffer(sdesc->rs_nbuf);
		sdesc->rs_ntup = NULL;
		sdesc->rs_nbuf = InvalidBuffer;
		return (NULL);
	    }
	
	if (BufferIsValid(sdesc->rs_pbuf))
	    ReleaseBuffer(sdesc->rs_pbuf);
	sdesc->rs_ptup = NULL;
	sdesc->rs_pbuf = UnknownBuffer;
	
    } else {
	/* ----------------
	 *  handle forward scan
	 * ----------------
	 */
	if (sdesc->rs_ctup == sdesc->rs_ntup &&
	    BufferIsInvalid(sdesc->rs_nbuf)) {
	    if (BufferIsValid(sdesc->rs_pbuf))
		ReleaseBuffer(sdesc->rs_pbuf);
	    HEAPDEBUG_3; /* heap_getnext returns NULL at end */
	    return (NULL);
	}
	
	/*
	 * Copy the "current" tuple/buffer
	 * to "previous". Pin/unpin the buffers
	 * accordingly
	 */
	if (sdesc->rs_pbuf != sdesc->rs_cbuf) {
	    if (BufferIsValid(sdesc->rs_pbuf))
		ReleaseBuffer(sdesc->rs_pbuf);
	    if (BufferIsValid(sdesc->rs_cbuf))
		IncrBufferRefCount(sdesc->rs_cbuf);
	}
	sdesc->rs_ptup = sdesc->rs_ctup;
	sdesc->rs_pbuf = sdesc->rs_cbuf;
	
	if (sdesc->rs_ntup != NULL) {
	    if (sdesc->rs_cbuf != sdesc->rs_nbuf) {
		if (BufferIsValid(sdesc->rs_cbuf))
		    ReleaseBuffer(sdesc->rs_cbuf);
		if (BufferIsValid(sdesc->rs_nbuf))
		    IncrBufferRefCount(sdesc->rs_nbuf);
	    }
	    sdesc->rs_ctup = sdesc->rs_ntup;
	    sdesc->rs_cbuf = sdesc->rs_nbuf;
	    HEAPDEBUG_5; /* heap_getnext next tuple was cached */
	} else { /* NONTUP */
	    ItemPointer iptr;
	    
	    iptr = (sdesc->rs_ctup != NULL) ?
		&sdesc->rs_ctup->t_ctid : (ItemPointer) NULL;
	    
            /* Don't release sdesc->rs_cbuf at this point, because
               heapgettup doesn't increase PrivateRefCount if it
               is already set. On a forward scan, both rs_ctup and rs_ptup
               usually point to the same buffer page, so
               PrivateRefCount[rs_cbuf] should be 2 (or more, if for instance
               ctup is stored in a TupleTableSlot).  - 01/09/93 */
	    
	    sdesc->rs_ctup = (HeapTuple)
		heapgettup(sdesc->rs_rd,
			   iptr,
			   1,
			   &sdesc->rs_cbuf,
			   sdesc->rs_tr,
			   sdesc->rs_nkeys,
			   sdesc->rs_key);
	}
	
	if (sdesc->rs_ctup == NULL && !BufferIsValid(sdesc->rs_cbuf)) {
	    if (BufferIsValid(sdesc->rs_nbuf))
		ReleaseBuffer(sdesc->rs_nbuf);
	    sdesc->rs_ntup = NULL;
	    sdesc->rs_nbuf = InvalidBuffer;
	    if (BufferIsValid(sdesc->rs_pbuf))
		ReleaseBuffer(sdesc->rs_pbuf);
	    sdesc->rs_ptup = NULL;
	    sdesc->rs_pbuf = InvalidBuffer;
	    HEAPDEBUG_6; /* heap_getnext returning EOS */
	    return (NULL);
	}
	
	if (BufferIsValid(sdesc->rs_nbuf))
	    ReleaseBuffer(sdesc->rs_nbuf);
	sdesc->rs_ntup = NULL;
	sdesc->rs_nbuf = UnknownBuffer;
    }
    
    /* ----------------
     *	if we get here it means we have a new current scan tuple, so
     *  point to the proper return buffer and return the tuple.
     * ----------------
     */
    (*b) = sdesc->rs_cbuf;
    
    HEAPDEBUG_7; /* heap_getnext returning tuple */
    
    return (sdesc->rs_ctup);
}

/* ----------------
 *	heap_fetch	- retrive tuple with tid
 *
 *	Currently ignores LP_IVALID during processing!
 * ----------------
 */
HeapTuple
heap_fetch(Relation relation,
	   TimeQual timeQual,
	   ItemPointer tid,
	   Buffer *b)
{
    ItemId		lp;
    Buffer		buffer;
    PageHeader		dp;
    HeapTuple		tuple;
    OffsetNumber	offnum;
    
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_fetch);    
    IncrHeapAccessStat(global_fetch);    
    
    /*
     * Note: This is collosally expensive - does two system calls per
     * indexscan tuple fetch.  Not good, and since we should be doing
     * page level locking by the scanner anyway, it is commented out.
     */
    
    /* RelationSetLockForTupleRead(relation, tid); */
    
    /* ----------------
     *	get the buffer from the relation descriptor
     *  Note that this does a buffer pin.
     * ----------------
     */
    
    buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
    
#ifndef NO_BUFFERISVALID
    if (!BufferIsValid(buffer)) {
	elog(WARN, "heap_fetch: %s relation: ReadBuffer(%lx) failed",
	     &relation->rd_rel->relname, (long)tid);
    }
#endif
    
    /* ----------------
     *	get the item line pointer corresponding to the requested tid
     * ----------------
     */
    dp = (PageHeader) BufferGetPage(buffer);
    offnum = ItemPointerGetOffsetNumber(tid);
    lp = PageGetItemId(dp, offnum);
    
    /* ----------------
     *	more sanity checks
     * ----------------
     */
    
    Assert(ItemIdIsUsed(lp)); 
    
    /* ----------------
     *	check time qualification of tid
     * ----------------
     */
    
    tuple = heap_tuple_satisfies(lp, relation, buffer, dp,
				 timeQual, 0,(ScanKey)NULL);
    
    if (tuple == NULL)
	{
	    ReleaseBuffer(buffer);
	    return (NULL);
	}
    
    /* ----------------
     *	all checks passed, now either return a copy of the tuple
     *  or pin the buffer page and return a pointer, depending on
     *  whether caller gave us a valid b.
     * ----------------
     */
    
    if (PointerIsValid(b)) {
	*b = buffer;
    } else {
	tuple = heap_copytuple(tuple);
	ReleaseBuffer(buffer);
    }
    return (tuple);
}

/* ----------------
 *	heap_insert	- insert tuple
 *
 *	The assignment of t_min (and thus the others) should be
 *	removed eventually.
 *
 *	Currently places the tuple onto the last page.  If there is no room,
 *	it is placed on new pages.  (Heap relations)
 *	Note that concurrent inserts during a scan will probably have
 *	unexpected results, though this will be fixed eventually.
 *
 *	Fix to work with indexes.
 * ----------------
 */
Oid
heap_insert(Relation relation, HeapTuple tup)
{
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_insert);
    IncrHeapAccessStat(global_insert);
    
    /* ----------------
     *	set relation level write lock. If this is a "local" relation (not
     *  visible to others), we don't need to set a write lock.
     * ----------------
     */
    if (!relation->rd_islocal)
	RelationSetLockForWrite(relation);

    /* ----------------
     *  If the object id of this tuple has already been assigned, trust
     *  the caller.  There are a couple of ways this can happen.  At initial
     *  db creation, the backend program sets oids for tuples.  When we
     *  define an index, we set the oid.  Finally, in the future, we may
     *  allow users to set their own object ids in order to support a
     *  persistent object store (objects need to contain pointers to one
     *  another).
     * ----------------
     */
    if (!OidIsValid(tup->t_oid)) {
	tup->t_oid = newoid();
	LastOidProcessed = tup->t_oid;
    }
    else
    	CheckMaxObjectId(tup->t_oid);
    
    TransactionIdStore(GetCurrentTransactionId(), &(tup->t_xmin));
    tup->t_cmin = GetCurrentCommandId();
    StoreInvalidTransactionId(&(tup->t_xmax));
    tup->t_tmin = INVALID_ABSTIME;
    tup->t_tmax = CURRENT_ABSTIME;
    
    doinsert(relation, tup);
    
    if ( IsSystemRelationName(RelationGetRelationName(relation)->data)) {
	RelationUnsetLockForWrite(relation);
    
	/* ----------------
	 *	invalidate caches (only works for system relations)
	 * ----------------
	 */
	SetRefreshWhenInvalidate(ImmediateInvalidation);
	RelationInvalidateHeapTuple(relation, tup);
	SetRefreshWhenInvalidate((bool)!ImmediateInvalidation);
    }
    
    return(tup->t_oid);
}

/* ----------------
 *	heap_delete	- delete a tuple
 *
 *	Must decide how to handle errors.
 * ----------------
 */
int
heap_delete(Relation relation, ItemPointer tid)
{
    ItemId		lp;
    HeapTuple		tp;
    PageHeader		dp;
    Buffer		b;
    
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_delete);
    IncrHeapAccessStat(global_delete);
    
    /* ----------------
     *	sanity check
     * ----------------
     */
    Assert(ItemPointerIsValid(tid));
    
    /* ----------------
     *	set relation level write lock
     * ----------------
     */
    RelationSetLockForWrite(relation);
    
    b = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));
    
#ifndef NO_BUFFERISVALID
    if (!BufferIsValid(b)) { /* XXX L_SH better ??? */
	elog(WARN, "heap_delete: failed ReadBuffer");
    }
#endif /* NO_BUFFERISVALID */
    
    dp = (PageHeader) BufferGetPage(b);
    lp = PageGetItemId(dp, ItemPointerGetOffsetNumber(tid));
    
    /* 
     * Just like test against non-functional updates we try to catch
     * non-functional delete attempts.		- vadim 05/05/97
     */
    tp = (HeapTuple) PageGetItem((Page)dp, lp);
    Assert(HeapTupleIsValid(tp));
    if (TupleUpdatedByCurXactAndCmd(tp)) {
	elog(NOTICE, "Non-functional delete, tuple already deleted");
	if ( IsSystemRelationName(RelationGetRelationName(relation)->data) )
	    RelationUnsetLockForWrite(relation);
	ReleaseBuffer(b);
	return (1);
    }
    /* ----------------
     *	check that we're deleteing a valid item
     * ----------------
     */
    if (!(tp = heap_tuple_satisfies(lp, relation, b, dp,
				    NowTimeQual, 0, (ScanKey) NULL))) {
	
	/* XXX call something else */
	ReleaseBuffer(b);
	
	elog(WARN, "heap_delete: (am)invalid tid");
    }
    
    /* ----------------
     *	get the tuple and lock tell the buffer manager we want
     *  exclusive access to the page
     * ----------------
     */
    
    /* ----------------
     *	store transaction information of xact deleting the tuple
     * ----------------
     */
    TransactionIdStore(GetCurrentTransactionId(), &(tp->t_xmax));
    tp->t_cmax = GetCurrentCommandId();
    ItemPointerSetInvalid(&tp->t_chain);
    
    /* ----------------
     *	invalidate caches
     * ----------------
     */
    SetRefreshWhenInvalidate(ImmediateInvalidation);
    RelationInvalidateHeapTuple(relation, tp);
    SetRefreshWhenInvalidate((bool)!ImmediateInvalidation);
    
    WriteBuffer(b);
    if ( IsSystemRelationName(RelationGetRelationName(relation)->data) )
	RelationUnsetLockForWrite(relation);
    
    return(0);
}

/* ----------------
 *	heap_replace	- replace a tuple
 *
 *	Must decide how to handle errors.
 *
 *	Fix arguments, work with indexes.
 * 
 *      12/30/93 - modified the return value to be 1 when
 *	           a non-functional update is detected. This
 *		   prevents the calling routine from updating
 *		   indices unnecessarily. -kw
 *
 * ----------------
 */
int
heap_replace(Relation relation, ItemPointer otid, HeapTuple tup)
{
    ItemId		lp;
    HeapTuple		tp;
    Page		dp;
    Buffer		buffer;
    
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_replace);
    IncrHeapAccessStat(global_replace);
    
    /* ----------------
     *	sanity checks
     * ----------------
     */
    Assert(ItemPointerIsValid(otid));
    
    /* ----------------
     *	set relation level write lock
     * ----------------
     */
    if (!relation->rd_islocal)
	RelationSetLockForWrite(relation);
    
    buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(otid));
#ifndef NO_BUFFERISVALID
    if (!BufferIsValid(buffer)) {
	/* XXX L_SH better ??? */
	elog(WARN, "amreplace: failed ReadBuffer");
    }	
#endif /* NO_BUFFERISVALID */
    
    dp = (Page) BufferGetPage(buffer);
    lp = PageGetItemId(dp, ItemPointerGetOffsetNumber(otid));
    
    /* ----------------
     *	logically delete old item
     * ----------------
     */
    
    tp = (HeapTuple) PageGetItem(dp, lp);
    Assert(HeapTupleIsValid(tp));
    
    /* -----------------
     *  the following test should be able to catch all non-functional
     *  update attempts and shut out all ghost tuples.
     *  XXX In the future, Spyros may need to update the rule lock on a tuple
     *  more than once within the same command and same transaction.
     *  He will have to introduce a new flag to override the following check.
     *  -- Wei
     *
     * -----------------
     */
    
    if (TupleUpdatedByCurXactAndCmd(tp)) {
	elog(NOTICE, "Non-functional update, only first update is performed");
	if ( IsSystemRelationName(RelationGetRelationName(relation)->data) )
	    RelationUnsetLockForWrite(relation);
	ReleaseBuffer(buffer);
	return(1);
    }
    
    /* ----------------
     *	check that we're replacing a valid item -
     *
     *  NOTE that this check must follow the non-functional update test
     *       above as it can happen that we try to 'replace' the same tuple
     *       twice in a single transaction.  The second time around the
     *       tuple will fail the NowTimeQual.  We don't want to abort the
     *       xact, we only want to flag the 'non-functional' NOTICE. -mer
     * ----------------
     */
    if (!heap_tuple_satisfies(lp,
			      relation,
			      buffer,
			      (PageHeader)dp,
			      NowTimeQual,
			      0,
			      (ScanKey)NULL))
    {
	ReleaseBuffer(buffer);
	elog(WARN, "heap_replace: (am)invalid otid");
    }
    
    /* XXX order problems if not atomic assignment ??? */
    tup->t_oid = tp->t_oid;
    TransactionIdStore(GetCurrentTransactionId(), &(tup->t_xmin));
    tup->t_cmin = GetCurrentCommandId();
    StoreInvalidTransactionId(&(tup->t_xmax));
    tup->t_tmin = INVALID_ABSTIME;
    tup->t_tmax = CURRENT_ABSTIME;
    ItemPointerSetInvalid(&tup->t_chain);
    
    /* ----------------
     *	insert new item
     * ----------------
     */
    if ((unsigned)DOUBLEALIGN(tup->t_len) <= PageGetFreeSpace((Page) dp)) {
	RelationPutHeapTuple(relation, BufferGetBlockNumber(buffer), tup);
    } else {
	/* ----------------
	 *  new item won't fit on same page as old item, have to look
	 *  for a new place to put it.
	 * ----------------
	 */
	doinsert(relation, tup);
    }

    /* ----------------
     *	new item in place, now record transaction information
     * ----------------
     */
    TransactionIdStore(GetCurrentTransactionId(), &(tp->t_xmax));
    tp->t_cmax = GetCurrentCommandId();
    tp->t_chain = tup->t_ctid;
    
    /* ----------------
     *	invalidate caches
     * ----------------
     */
    SetRefreshWhenInvalidate(ImmediateInvalidation);
    RelationInvalidateHeapTuple(relation, tp);
    SetRefreshWhenInvalidate((bool)!ImmediateInvalidation);
    
    WriteBuffer(buffer);
    
    if ( IsSystemRelationName(RelationGetRelationName(relation)->data) )
	RelationUnsetLockForWrite(relation);
    
    return(0);
}

/* ----------------
 *	heap_markpos	- mark scan position
 *
 *	Note:
 *		Should only one mark be maintained per scan at one time.
 *	Check if this can be done generally--say calls to get the
 *	next/previous tuple and NEVER pass struct scandesc to the
 *	user AM's.  Now, the mark is sent to the executor for safekeeping.
 *	Probably can store this info into a GENERAL scan structure.
 *
 *	May be best to change this call to store the marked position
 *	(up to 2?) in the scan structure itself.
 *	Fix to use the proper caching structure.
 * ----------------
 */
void
heap_markpos(HeapScanDesc sdesc)
{
    
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_markpos);
    IncrHeapAccessStat(global_markpos);
    
    /* Note: no locking manipulations needed */
    
    if (sdesc->rs_ptup == NULL &&
	BufferIsUnknown(sdesc->rs_pbuf)) { /* == NONTUP */
	sdesc->rs_ptup = (HeapTuple)
	    heapgettup(sdesc->rs_rd,
		       (sdesc->rs_ctup == NULL) ?
		       (ItemPointer)NULL : &sdesc->rs_ctup->t_ctid,
		       -1,
		       &sdesc->rs_pbuf,
		       sdesc->rs_tr,
		       sdesc->rs_nkeys,
		       sdesc->rs_key);
	
    } else if (sdesc->rs_ntup == NULL &&
	       BufferIsUnknown(sdesc->rs_nbuf)) { /* == NONTUP */
	sdesc->rs_ntup = (HeapTuple)
	    heapgettup(sdesc->rs_rd,
		       (sdesc->rs_ctup == NULL) ?
		       (ItemPointer)NULL : &sdesc->rs_ctup->t_ctid,
		       1,
		       &sdesc->rs_nbuf,
		       sdesc->rs_tr,
		       sdesc->rs_nkeys,
		       sdesc->rs_key);
    }
    
    /* ----------------
     * Should not unpin the buffer pages.  They may still be in use.
     * ----------------
     */
    if (sdesc->rs_ptup != NULL) {
	sdesc->rs_mptid = sdesc->rs_ptup->t_ctid;
    } else {
	ItemPointerSetInvalid(&sdesc->rs_mptid);
    }
    if (sdesc->rs_ctup != NULL) {
	sdesc->rs_mctid = sdesc->rs_ctup->t_ctid;
    } else {
	ItemPointerSetInvalid(&sdesc->rs_mctid);
    }
    if (sdesc->rs_ntup != NULL) {
	sdesc->rs_mntid = sdesc->rs_ntup->t_ctid;
    } else {
	ItemPointerSetInvalid(&sdesc->rs_mntid);
    }
}

/* ----------------
 *	heap_restrpos	- restore position to marked location
 *
 *	Note:  there are bad side effects here.  If we were past the end
 *	of a relation when heapmarkpos is called, then if the relation is
 *	extended via insert, then the next call to heaprestrpos will set
 *	cause the added tuples to be visible when the scan continues.
 *	Problems also arise if the TID's are rearranged!!!
 *
 *	Now pins buffer once for each valid tuple pointer (rs_ptup,
 *	rs_ctup, rs_ntup) referencing it.
 *	 - 01/13/94
 *
 * XXX	might be better to do direct access instead of
 *	using the generality of heapgettup().
 *
 * XXX It is very possible that when a scan is restored, that a tuple
 * XXX which previously qualified may fail for time range purposes, unless
 * XXX some form of locking exists (ie., portals currently can act funny.
 * ----------------
 */
void
heap_restrpos(HeapScanDesc sdesc)
{
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_restrpos);
    IncrHeapAccessStat(global_restrpos);
    
    /* XXX no amrestrpos checking that ammarkpos called */
    
    /* Note: no locking manipulations needed */
    
    unpinsdesc(sdesc);
    
    /* force heapgettup to pin buffer for each loaded tuple */
    sdesc->rs_pbuf = InvalidBuffer;
    sdesc->rs_cbuf = InvalidBuffer;
    sdesc->rs_nbuf = InvalidBuffer;
    
    if (!ItemPointerIsValid(&sdesc->rs_mptid)) {
	sdesc->rs_ptup = NULL;
    } else {
	sdesc->rs_ptup = (HeapTuple)
	    heapgettup(sdesc->rs_rd,
		       &sdesc->rs_mptid,
		       0,
		       &sdesc->rs_pbuf,
		       NowTimeQual,
		       0,
		       (ScanKey) NULL);
    }
    
    if (!ItemPointerIsValid(&sdesc->rs_mctid)) {
	sdesc->rs_ctup = NULL;
    } else {
	sdesc->rs_ctup = (HeapTuple)
	    heapgettup(sdesc->rs_rd,
		       &sdesc->rs_mctid,
		       0,
		       &sdesc->rs_cbuf,
		       NowTimeQual,
		       0,
		       (ScanKey) NULL);
    }
    
    if (!ItemPointerIsValid(&sdesc->rs_mntid)) {
	sdesc->rs_ntup = NULL;
    } else {
	sdesc->rs_ntup = (HeapTuple)
	    heapgettup(sdesc->rs_rd,
		       &sdesc->rs_mntid,
		       0,
		       &sdesc->rs_nbuf,
		       NowTimeQual,
		       0,
		       (ScanKey) NULL);
    }
}
