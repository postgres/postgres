/*-------------------------------------------------------------------------
 *
 * hash.c--
 *    Implementation of Margo Seltzer's Hashing package for postgres. 
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/hash/hash.c,v 1.5 1996/10/21 05:45:11 scrappy Exp $
 *
 * NOTES
 *    This file contains only the public interface routines.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_attribute.h"
#include "access/attnum.h"
#include "nodes/pg_list.h"
#include "access/tupdesc.h"
#include "storage/fd.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "nodes/nodes.h"
#include "rewrite/prs2lock.h"
#include "access/skey.h"
#include "access/strat.h"
#include "utils/rel.h"

#include "storage/block.h"  
#include "storage/off.h" 
#include "storage/itemptr.h"
#include <time.h>
#include "utils/nabstime.h"
#include "access/htup.h"
#include "access/itup.h"
#include "storage/itemid.h" 
#include "storage/item.h"   
#include "storage/buf.h"  
#include "storage/bufpage.h"
#include "access/sdir.h"
#include "access/funcindex.h"
#include "utils/tqual.h"  
#include "access/relscan.h"
#include "access/hash.h"
 
#include "nodes/params.h"
#include "executor/hashjoin.h"
#include "nodes/primnodes.h"
#include "nodes/memnodes.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"

#include "nodes/plannodes.h" 
#include "nodes/parsenodes.h"
#include "tcop/dest.h"
#include "executor/execdesc.h"
#include <stdio.h>
#include "catalog/pg_index.h"
#include "executor/executor.h"

#include "access/heapam.h"

#include "access/genam.h"

#include "utils/palloc.h"

#include "catalog/index.h"

#ifndef HAVE_MEMMOVE
# include "regex/utils.h"
#else
# include <string.h>
#endif

#include "storage/ipc.h"
#include "storage/bufmgr.h"

#include "miscadmin.h"

bool	BuildingHash = false;

/*
 *  hashbuild() -- build a new hash index.
 *
 *	We use a global variable to record the fact that we're creating
 *	a new index.  This is used to avoid high-concurrency locking,
 *	since the index won't be visible until this transaction commits
 *	and since building is guaranteed to be single-threaded.
 */
void
hashbuild(Relation heap,
	  Relation index,
	  int natts,
	  AttrNumber *attnum,
	  IndexStrategy istrat,
	  uint16 pcount,
	  Datum *params,
	  FuncIndexInfo *finfo,
	  PredInfo *predInfo)
{
    HeapScanDesc hscan;
    Buffer buffer;
    HeapTuple htup;
    IndexTuple itup;
    TupleDesc htupdesc, itupdesc;
    Datum *attdata;
    bool *nulls;
    InsertIndexResult res;
    int nhtups, nitups;
    int i;
    HashItem hitem;
    ExprContext *econtext;
    TupleTable tupleTable;
    TupleTableSlot *slot;
    Oid hrelid, irelid;
    Node *pred, *oldPred;
    
    /* note that this is a new btree */
    BuildingHash = true;
    
    pred = predInfo->pred;
    oldPred = predInfo->oldPred;
    
    /*  initialize the hash index metadata page (if this is a new index) */
    if (oldPred == NULL)
	_hash_metapinit(index);
    
    /* get tuple descriptors for heap and index relations */
    htupdesc = RelationGetTupleDescriptor(heap);
    itupdesc = RelationGetTupleDescriptor(index);
    
    /* get space for data items that'll appear in the index tuple */
    attdata = (Datum *) palloc(natts * sizeof(Datum));
    nulls = (bool *) palloc(natts * sizeof(bool));
    
    /*
     * If this is a predicate (partial) index, we will need to evaluate the
     * predicate using ExecQual, which requires the current tuple to be in a
     * slot of a TupleTable.  In addition, ExecQual must have an ExprContext
     * referring to that slot.  Here, we initialize dummy TupleTable and
     * ExprContext objects for this purpose. --Nels, Feb '92
     */
#ifndef OMIT_PARTIAL_INDEX
    if (pred != NULL || oldPred != NULL) {
	tupleTable = ExecCreateTupleTable(1);
	slot = ExecAllocTableSlot(tupleTable);
	econtext = makeNode(ExprContext);
	FillDummyExprContext(econtext, slot, htupdesc, buffer);
    }
#endif /* OMIT_PARTIAL_INDEX */
    
    /* start a heap scan */
    hscan = heap_beginscan(heap, 0, NowTimeQual, 0, (ScanKey) NULL);
    htup = heap_getnext(hscan, 0, &buffer);
    
    /* build the index */
    nhtups = nitups = 0;
    
    for (; HeapTupleIsValid(htup); htup = heap_getnext(hscan, 0, &buffer)) {
	
	nhtups++;
	
	/*
	 * If oldPred != NULL, this is an EXTEND INDEX command, so skip
	 * this tuple if it was already in the existing partial index
	 */
	if (oldPred != NULL) {
	    /*SetSlotContents(slot, htup); */
#ifndef OMIT_PARTIAL_INDEX
	    slot->val = htup;
	    if (ExecQual((List*)oldPred, econtext) == true) {
		nitups++;
		continue;
	    }
#endif /* OMIT_PARTIAL_INDEX */    
	}
	
	/* Skip this tuple if it doesn't satisfy the partial-index predicate */
	if (pred != NULL) {
#ifndef OMIT_PARTIAL_INDEX
	    /*SetSlotContents(slot, htup); */
	    slot->val = htup;
	    if (ExecQual((List*)pred, econtext) == false)
		continue;
#endif /* OMIT_PARTIAL_INDEX */    	
}
	
	nitups++;
	
	/*
	 *  For the current heap tuple, extract all the attributes
	 *  we use in this index, and note which are null.
	 */
	for (i = 1; i <= natts; i++) {
	    int attoff;
	    bool attnull;
	    
	    /*
	     *  Offsets are from the start of the tuple, and are
	     *  zero-based; indices are one-based.  The next call
	     *  returns i - 1.  That's data hiding for you.
	     */
	    
	    /* attoff = i - 1 */
	    attoff = AttrNumberGetAttrOffset(i);
	    
	    /* below, attdata[attoff] set to equal some datum &
	     * attnull is changed to indicate whether or not the attribute 
	     * is null for this tuple
	     */
	    attdata[attoff] = GetIndexValue(htup, 
					    htupdesc,
					    attoff, 
					    attnum, 
					    finfo, 
					    &attnull,
					    buffer);
	    nulls[attoff] = (attnull ? 'n' : ' ');
	}
	
	/* form an index tuple and point it at the heap tuple */
	itup = index_formtuple(itupdesc, attdata, nulls);
	
	/*
	 *  If the single index key is null, we don't insert it into
	 *  the index.  Hash tables support scans on '='.
	 *  Relational algebra says that A = B
	 *  returns null if either A or B is null.  This
	 *  means that no qualification used in an index scan could ever
	 *  return true on a null attribute.  It also means that indices
	 *  can't be used by ISNULL or NOTNULL scans, but that's an
	 *  artifact of the strategy map architecture chosen in 1986, not
	 *  of the way nulls are handled here.
	 */
	
	if (itup->t_info & INDEX_NULL_MASK) {
	    pfree(itup);
	    continue;
	}
	
	itup->t_tid = htup->t_ctid;
	hitem = _hash_formitem(itup);
	res = _hash_doinsert(index, hitem);
	pfree(hitem);
	pfree(itup);
	pfree(res);
    }
    
    /* okay, all heap tuples are indexed */
    heap_endscan(hscan);
    
    if (pred != NULL || oldPred != NULL) {
#ifndef OMIT_PARTIAL_INDEX
	ExecDestroyTupleTable(tupleTable, true);
	pfree(econtext);
#endif /* OMIT_PARTIAL_INDEX */    	
    }
    
    /*
     *  Since we just counted the tuples in the heap, we update its
     *  stats in pg_class to guarantee that the planner takes advantage
     *  of the index we just created. Finally, only update statistics
     *  during normal index definitions, not for indices on system catalogs
     *  created during bootstrap processing.  We must close the relations
     *  before updatings statistics to guarantee that the relcache entries
     *  are flushed when we increment the command counter in UpdateStats().
     */
    if (IsNormalProcessingMode())
	{
	    hrelid = heap->rd_id;
	    irelid = index->rd_id;
	    heap_close(heap);
	    index_close(index);
	    UpdateStats(hrelid, nhtups, true);
	    UpdateStats(irelid, nitups, false);
	    if (oldPred != NULL) {
		if (nitups == nhtups) pred = NULL;
		UpdateIndexPredicate(irelid, oldPred, pred);
	    }  
	}
    
    /* be tidy */
    pfree(nulls);
    pfree(attdata);
    
    /* all done */
    BuildingHash = false;
}

/*
 *  hashinsert() -- insert an index tuple into a hash table. 
 *
 *  Hash on the index tuple's key, find the appropriate location 
 *  for the new tuple, put it there, and return an InsertIndexResult
 *  to the caller. 
 */
InsertIndexResult
hashinsert(Relation rel, Datum *datum, char *nulls, ItemPointer ht_ctid)
{
    HashItem hitem;
    IndexTuple itup;
    InsertIndexResult res;
    

    /* generate an index tuple */
    itup = index_formtuple(RelationGetTupleDescriptor(rel), datum, nulls);
    itup->t_tid = *ht_ctid;

    if (itup->t_info & INDEX_NULL_MASK)
	return ((InsertIndexResult) NULL);
    
    hitem = _hash_formitem(itup);
    
    res = _hash_doinsert(rel, hitem);
    
    pfree(hitem);
    pfree(itup);
    
    return (res);
}


/*
 *  hashgettuple() -- Get the next tuple in the scan.
 */
char *
hashgettuple(IndexScanDesc scan, ScanDirection dir)
{
    RetrieveIndexResult res;
    
    /*
     *  If we've already initialized this scan, we can just advance it
     *  in the appropriate direction.  If we haven't done so yet, we
     *  call a routine to get the first item in the scan.
     */
    
    if (ItemPointerIsValid(&(scan->currentItemData)))
	res = _hash_next(scan, dir);
    else
	res = _hash_first(scan, dir);
    
    return ((char *) res);
}


/*
 *  hashbeginscan() -- start a scan on a hash index
 */
char *
hashbeginscan(Relation rel,
	      bool fromEnd,
	      uint16 keysz,
	      ScanKey scankey)
{
    IndexScanDesc scan;
    HashScanOpaque so;
    
    scan = RelationGetIndexScan(rel, fromEnd, keysz, scankey);
    so = (HashScanOpaque) palloc(sizeof(HashScanOpaqueData)); 
    so->hashso_curbuf = so->hashso_mrkbuf = InvalidBuffer;
    scan->opaque = so; 
    scan->flags = 0x0;
    
    /* register scan in case we change pages it's using */
    _hash_regscan(scan);
    
    return ((char *) scan);
}

/*
 *  hashrescan() -- rescan an index relation
 */
void
hashrescan(IndexScanDesc scan, bool fromEnd, ScanKey scankey)
{
    ItemPointer iptr;
    HashScanOpaque so;
    
    so = (HashScanOpaque) scan->opaque;
    
    /* we hold a read lock on the current page in the scan */
    if (ItemPointerIsValid(iptr = &(scan->currentItemData))) {
	_hash_relbuf(scan->relation, so->hashso_curbuf, HASH_READ);
	so->hashso_curbuf = InvalidBuffer;
	ItemPointerSetInvalid(iptr);
    }
    if (ItemPointerIsValid(iptr = &(scan->currentMarkData))) {
	_hash_relbuf(scan->relation, so->hashso_mrkbuf, HASH_READ);
	so->hashso_mrkbuf = InvalidBuffer;
	ItemPointerSetInvalid(iptr);
    }
    
    /* reset the scan key */
    if (scan->numberOfKeys > 0) {
	memmove(scan->keyData,
		scankey,
		scan->numberOfKeys * sizeof(ScanKeyData));
    }
}

/*
 *  hashendscan() -- close down a scan
 */
void
hashendscan(IndexScanDesc scan)
{
    
    ItemPointer iptr;
    HashScanOpaque so;
    
    so = (HashScanOpaque) scan->opaque;
    
    /* release any locks we still hold */
    if (ItemPointerIsValid(iptr = &(scan->currentItemData))) {
	_hash_relbuf(scan->relation, so->hashso_curbuf, HASH_READ);
	so->hashso_curbuf = InvalidBuffer;
	ItemPointerSetInvalid(iptr);
    }
    
    if (ItemPointerIsValid(iptr = &(scan->currentMarkData))) {
	if (BufferIsValid(so->hashso_mrkbuf))
	    _hash_relbuf(scan->relation, so->hashso_mrkbuf, HASH_READ);
	so->hashso_mrkbuf = InvalidBuffer;
	ItemPointerSetInvalid(iptr);
    }
    
    /* don't need scan registered anymore */
    _hash_dropscan(scan);
    
    /* be tidy */
#ifdef PERFECT_MMGR
    pfree (scan->opaque);
#endif /* PERFECT_MMGR */
}

/*
 *  hashmarkpos() -- save current scan position
 *
 */
void
hashmarkpos(IndexScanDesc scan)
{
    ItemPointer iptr;
    HashScanOpaque so;
    
    /*  see if we ever call this code. if we do, then so_mrkbuf a
     *  useful element in the scan->opaque structure. if this procedure
     *  is never called, so_mrkbuf should be removed from the scan->opaque
     *  structure. 
     */
    elog(NOTICE, "Hashmarkpos() called.");
    
    so = (HashScanOpaque) scan->opaque;
    
    /* release lock on old marked data, if any */
    if (ItemPointerIsValid(iptr = &(scan->currentMarkData))) {
	_hash_relbuf(scan->relation, so->hashso_mrkbuf, HASH_READ);
	so->hashso_mrkbuf = InvalidBuffer;
	ItemPointerSetInvalid(iptr);
    }
    
    /* bump lock on currentItemData and copy to currentMarkData */
    if (ItemPointerIsValid(&(scan->currentItemData))) {
	so->hashso_mrkbuf = _hash_getbuf(scan->relation,
					 BufferGetBlockNumber(so->hashso_curbuf),
					 HASH_READ);
	scan->currentMarkData = scan->currentItemData;
    }
}

/*
 *  hashrestrpos() -- restore scan to last saved position
 */
void
hashrestrpos(IndexScanDesc scan)
{
    ItemPointer iptr;
    HashScanOpaque so;
    
    /*  see if we ever call this code. if we do, then so_mrkbuf a
     *  useful element in the scan->opaque structure. if this procedure
     *  is never called, so_mrkbuf should be removed from the scan->opaque
     *  structure. 
     */
    elog(NOTICE, "Hashrestrpos() called.");
    
    so = (HashScanOpaque) scan->opaque;
    
    /* release lock on current data, if any */
    if (ItemPointerIsValid(iptr = &(scan->currentItemData))) {
	_hash_relbuf(scan->relation, so->hashso_curbuf, HASH_READ);
	so->hashso_curbuf = InvalidBuffer;
	ItemPointerSetInvalid(iptr);
    }
    
    /* bump lock on currentMarkData and copy to currentItemData */
    if (ItemPointerIsValid(&(scan->currentMarkData))) {
	so->hashso_curbuf =
	    _hash_getbuf(scan->relation,
			 BufferGetBlockNumber(so->hashso_mrkbuf),
			 HASH_READ);
	
	scan->currentItemData = scan->currentMarkData;
    }
}

/* stubs */
void
hashdelete(Relation rel, ItemPointer tid)
{
    /* adjust any active scans that will be affected by this deletion */
    _hash_adjscans(rel, tid);
    
    /* delete the data from the page */
    _hash_pagedel(rel, tid);
}

