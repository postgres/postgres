/*-------------------------------------------------------------------------
 *
 * btree.c--
 *    Implementation of Lehman and Yao's btree management algorithm for
 *    Postgres.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtree.c,v 1.10 1996/11/13 20:47:18 scrappy Exp $
 *
 * NOTES
 *    This file contains only the public interface routines.
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/genam.h>
#include <storage/bufpage.h>
#include <storage/bufmgr.h>
#include <access/nbtree.h>
#include <executor/executor.h>
#include <access/heapam.h>
#include <catalog/index.h>
#include <miscadmin.h>

#ifndef HAVE_MEMMOVE
# include <regex/utils.h>
#else
# include <string.h>
#endif

bool	BuildingBtree = false;
bool	FastBuild = false; /* turn this on to make bulk builds work*/

/*
 *  btbuild() -- build a new btree index.
 *
 *	We use a global variable to record the fact that we're creating
 *	a new index.  This is used to avoid high-concurrency locking,
 *	since the index won't be visible until this transaction commits
 *	and since building is guaranteed to be single-threaded.
 */
void
btbuild(Relation heap,
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
    InsertIndexResult res = 0;
    int nhtups, nitups;
    int i;
    BTItem btitem;
#ifndef OMIT_PARTIAL_INDEX
    ExprContext *econtext;
    TupleTable tupleTable;
    TupleTableSlot *slot;
#endif
    Oid hrelid, irelid;
    Node *pred, *oldPred;
    void *spool;
    bool isunique;
    
    /* note that this is a new btree */
    BuildingBtree = true;
    
    pred = predInfo->pred;
    oldPred = predInfo->oldPred;

    /* see if index is unique */
    isunique = IndexIsUniqueNoCache(RelationGetRelationId(index));

    /* initialize the btree index metadata page (if this is a new index) */
    if (oldPred == NULL)
	_bt_metapinit(index);
    
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
	FillDummyExprContext(econtext, slot, htupdesc, InvalidBuffer);
    }
	else
	{
		econtext = NULL;
		tupleTable = NULL;
		slot = NULL;
	}
#endif /* OMIT_PARTIAL_INDEX */
    
    /* start a heap scan */
    hscan = heap_beginscan(heap, 0, NowTimeQual, 0, (ScanKey) NULL);
    htup = heap_getnext(hscan, 0, &buffer);
    
    /* build the index */
    nhtups = nitups = 0;
    
    if (FastBuild) {
	spool = _bt_spoolinit(index, 7);
	res = (InsertIndexResult) NULL;
    }
	else
		spool = NULL;

    for (; HeapTupleIsValid(htup); htup = heap_getnext(hscan, 0, &buffer)) {
	
	nhtups++;
	
	/*
	 * If oldPred != NULL, this is an EXTEND INDEX command, so skip
	 * this tuple if it was already in the existing partial index
	 */
	if (oldPred != NULL) {
#ifndef OMIT_PARTIAL_INDEX

	    /*SetSlotContents(slot, htup);*/
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
	    /* SetSlotContents(slot, htup); */
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
	    int  attoff;
	    bool attnull;
	    
	    /*
	     *  Offsets are from the start of the tuple, and are
	     *  zero-based; indices are one-based.  The next call
	     *  returns i - 1.  That's data hiding for you.
	     */
	    
	    attoff = AttrNumberGetAttrOffset(i);
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
	 *  the index.  Btrees support scans on <, <=, =, >=, and >.
	 *  Relational algebra says that A op B (where op is one of the
	 *  operators above) returns null if either A or B is null.  This
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
	btitem = _bt_formitem(itup);

	/*
	 * if we are doing bottom-up btree build, we insert the index
	 * into a spool page for subsequent processing.  otherwise, we
	 * insert into the btree.
	 */
	if (FastBuild) {
	    _bt_spool(index, btitem, spool);
	} else {
	    res = _bt_doinsert(index, btitem, isunique, false);
	}

	pfree(btitem);
	pfree(itup);
	if (res) {
	    pfree(res);
	}
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
     * if we are doing bottom-up btree build, we now have a bunch of
     * sorted runs in the spool pages.  finish the build by (1)
     * merging the runs, (2) inserting the sorted tuples into btree
     * pages and (3) building the upper levels.
     */
    if (FastBuild) {
	_bt_spool(index, (BTItem) NULL, spool);	/* flush spool */
	_bt_leafbuild(index, spool);
	_bt_spooldestroy(spool);
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
    BuildingBtree = false;
}

/*
 *  btinsert() -- insert an index tuple into a btree.
 *
 *	Descend the tree recursively, find the appropriate location for our
 *	new tuple, put it there, set its unique OID as appropriate, and
 *	return an InsertIndexResult to the caller.
 */
InsertIndexResult
btinsert(Relation rel, Datum *datum, char *nulls, ItemPointer ht_ctid, bool is_update)
{
    BTItem btitem;
    IndexTuple itup;
    InsertIndexResult res;
    
    /* generate an index tuple */
    itup = index_formtuple(RelationGetTupleDescriptor(rel), datum, nulls);
    itup->t_tid = *ht_ctid;

    if (itup->t_info & INDEX_NULL_MASK)
	return ((InsertIndexResult) NULL);
    
    btitem = _bt_formitem(itup);
    
    res = _bt_doinsert(rel, btitem, 
		       IndexIsUnique(RelationGetRelationId(rel)), is_update);

    pfree(btitem);
    pfree(itup);
    
    return (res);
}

/*
 *  btgettuple() -- Get the next tuple in the scan.
 */
char *
btgettuple(IndexScanDesc scan, ScanDirection dir)
{
    RetrieveIndexResult res;
    
    /*
     *  If we've already initialized this scan, we can just advance it
     *  in the appropriate direction.  If we haven't done so yet, we
     *  call a routine to get the first item in the scan.
     */
    
    if (ItemPointerIsValid(&(scan->currentItemData)))
	res = _bt_next(scan, dir);
    else
	res = _bt_first(scan, dir);
    
    return ((char *) res);
}

/*
 *  btbeginscan() -- start a scan on a btree index
 */
char *
btbeginscan(Relation rel, bool fromEnd, uint16 keysz, ScanKey scankey)
{
    IndexScanDesc scan;
    
    /* get the scan */
    scan = RelationGetIndexScan(rel, fromEnd, keysz, scankey);
    
    /* register scan in case we change pages it's using */
    _bt_regscan(scan);
    
    return ((char *) scan);
}

/*
 *  btrescan() -- rescan an index relation
 */
void
btrescan(IndexScanDesc scan, bool fromEnd, ScanKey scankey)
{
    ItemPointer iptr;
    BTScanOpaque so;
    StrategyNumber strat;
    
    so = (BTScanOpaque) scan->opaque;
    
    /* we hold a read lock on the current page in the scan */
    if (ItemPointerIsValid(iptr = &(scan->currentItemData))) {
	_bt_relbuf(scan->relation, so->btso_curbuf, BT_READ);
	so->btso_curbuf = InvalidBuffer;
	ItemPointerSetInvalid(iptr);
    }
    
    /* and we hold a read lock on the last marked item in the scan */
    if (ItemPointerIsValid(iptr = &(scan->currentMarkData))) {
	_bt_relbuf(scan->relation, so->btso_mrkbuf, BT_READ);
	so->btso_mrkbuf = InvalidBuffer;
	ItemPointerSetInvalid(iptr);
    }
    
    if ( so == NULL )		/* if called from btbeginscan */
    {
	so = (BTScanOpaque) palloc(sizeof(BTScanOpaqueData));
	so->btso_curbuf = so->btso_mrkbuf = InvalidBuffer;
	so->keyData = (ScanKey) NULL;
	if ( scan->numberOfKeys > 0)
		so->keyData = (ScanKey) palloc (scan->numberOfKeys * sizeof(ScanKeyData));
	scan->opaque = so;
	scan->flags = 0x0;
    }
    
    /* reset the scan key */
    so->numberOfKeys = scan->numberOfKeys;
    so->qual_ok = 1;			/* may be changed by _bt_orderkeys */
    if (scan->numberOfKeys > 0) {
	memmove(scan->keyData,
		scankey,
		scan->numberOfKeys * sizeof(ScanKeyData));
	memmove(so->keyData,
		scankey,
		so->numberOfKeys * sizeof(ScanKeyData));
	/* order the keys in the qualification */
	if (so->numberOfKeys > 1)
		_bt_orderkeys(scan->relation, &so->numberOfKeys, so->keyData, &so->qual_ok);
    }
    
    /* finally, be sure that the scan exploits the tree order */
    scan->scanFromEnd = false;
    if ( so->numberOfKeys > 0 ) {
	strat = _bt_getstrat(scan->relation, 1 /* XXX */,
			     so->keyData[0].sk_procedure);
	
	if (strat == BTLessStrategyNumber
	    || strat == BTLessEqualStrategyNumber)
	    scan->scanFromEnd = true;
    } else {
	scan->scanFromEnd = true;
    }

}

void
btmovescan(IndexScanDesc scan, Datum v)
{
    ItemPointer iptr;
    BTScanOpaque so;
    
    so = (BTScanOpaque) scan->opaque;
    
    /* release any locks we still hold */
    if (ItemPointerIsValid(iptr = &(scan->currentItemData))) {
	_bt_relbuf(scan->relation, so->btso_curbuf, BT_READ);
	so->btso_curbuf = InvalidBuffer;
	ItemPointerSetInvalid(iptr);
    }
    
/*    scan->keyData[0].sk_argument = v;	*/
    so->keyData[0].sk_argument = v;
}

/*
 *  btendscan() -- close down a scan
 */
void
btendscan(IndexScanDesc scan)
{
    ItemPointer iptr;
    BTScanOpaque so;
    
    so = (BTScanOpaque) scan->opaque;
    
    /* release any locks we still hold */
    if (ItemPointerIsValid(iptr = &(scan->currentItemData))) {
	if (BufferIsValid(so->btso_curbuf))
	    _bt_relbuf(scan->relation, so->btso_curbuf, BT_READ);
	so->btso_curbuf = InvalidBuffer;
	ItemPointerSetInvalid(iptr);
    }
    
    if (ItemPointerIsValid(iptr = &(scan->currentMarkData))) {
	if (BufferIsValid(so->btso_mrkbuf))
	    _bt_relbuf(scan->relation, so->btso_mrkbuf, BT_READ);
	so->btso_mrkbuf = InvalidBuffer;
	ItemPointerSetInvalid(iptr);
    }
    
    /* don't need scan registered anymore */
    _bt_dropscan(scan);
    
    /* be tidy */
    if ( so->keyData != (ScanKey) NULL )
    	pfree (so->keyData);
    pfree (scan->opaque);
}

/*
 *  btmarkpos() -- save current scan position
 */
void
btmarkpos(IndexScanDesc scan)
{
    ItemPointer iptr;
    BTScanOpaque so;
    
    so = (BTScanOpaque) scan->opaque;
    
    /* release lock on old marked data, if any */
    if (ItemPointerIsValid(iptr = &(scan->currentMarkData))) {
	_bt_relbuf(scan->relation, so->btso_mrkbuf, BT_READ);
	so->btso_mrkbuf = InvalidBuffer;
	ItemPointerSetInvalid(iptr);
    }
    
    /* bump lock on currentItemData and copy to currentMarkData */
    if (ItemPointerIsValid(&(scan->currentItemData))) {
	so->btso_mrkbuf = _bt_getbuf(scan->relation,
				     BufferGetBlockNumber(so->btso_curbuf),
				     BT_READ);
	scan->currentMarkData = scan->currentItemData;
    }
}

/*
 *  btrestrpos() -- restore scan to last saved position
 */
void
btrestrpos(IndexScanDesc scan)
{
    ItemPointer iptr;
    BTScanOpaque so;
    
    so = (BTScanOpaque) scan->opaque;
    
    /* release lock on current data, if any */
    if (ItemPointerIsValid(iptr = &(scan->currentItemData))) {
	_bt_relbuf(scan->relation, so->btso_curbuf, BT_READ);
	so->btso_curbuf = InvalidBuffer;
	ItemPointerSetInvalid(iptr);
    }
    
    /* bump lock on currentMarkData and copy to currentItemData */
    if (ItemPointerIsValid(&(scan->currentMarkData))) {
	so->btso_curbuf = _bt_getbuf(scan->relation,
				     BufferGetBlockNumber(so->btso_mrkbuf),
				     BT_READ);
	
	scan->currentItemData = scan->currentMarkData;
    }
}

/* stubs */
void
btdelete(Relation rel, ItemPointer tid)
{
    /* adjust any active scans that will be affected by this deletion */
    _bt_adjscans(rel, tid);
    
    /* delete the data from the page */
    _bt_pagedel(rel, tid);
}
