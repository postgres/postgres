/*-------------------------------------------------------------------------
 *
 * nbtree.c
 *	  Implementation of Lehman and Yao's btree management algorithm for
 *	  Postgres.
 *
 * NOTES
 *	  This file contains only the public interface routines.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtree.c,v 1.64 2000/10/13 02:03:00 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "storage/sinval.h"


bool		BuildingBtree = false;		/* see comment in btbuild() */
bool		FastBuild = true;	/* use sort/build instead of insertion
								 * build */

static void _bt_restscan(IndexScanDesc scan);

/*
 *	btbuild() -- build a new btree index.
 *
 *		We use a global variable to record the fact that we're creating
 *		a new index.  This is used to avoid high-concurrency locking,
 *		since the index won't be visible until this transaction commits
 *		and since building is guaranteed to be single-threaded.
 */
Datum
btbuild(PG_FUNCTION_ARGS)
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
	InsertIndexResult res = NULL;
	BTSpool    *spool = NULL;
	BTItem		btitem;
	bool		usefast;
	Snapshot	snapshot;
	TransactionId	XmaxRecent;
	/*
	 * spool2 is needed only when the index is an unique index.
	 * Dead tuples are put into spool2 instead of spool in
	 * order to avoid uniqueness check.
	 */
	BTSpool		*spool2 = NULL;
	bool		tupleIsAlive;
	int		dead_count;

	/* note that this is a new btree */
	BuildingBtree = true;

	/*
	 * bootstrap processing does something strange, so don't use
	 * sort/build for initial catalog indices.	at some point i need to
	 * look harder at this.  (there is some kind of incremental processing
	 * going on there.) -- pma 08/29/95
	 */
	usefast = (FastBuild && IsNormalProcessingMode());

#ifdef BTREE_BUILD_STATS
	if (Show_btree_build_stats)
		ResetUsage();
#endif /* BTREE_BUILD_STATS */

	/* initialize the btree index metadata page (if this is a new index) */
	if (oldPred == NULL)
		_bt_metapinit(index);

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
		ExecSetSlotDescriptor(slot, htupdesc);

		/*
		 * we never want to use sort/build if we are extending an existing
		 * partial index -- it works by inserting the newly-qualifying
		 * tuples into the existing index. (sort/build would overwrite the
		 * existing index with one consisting of the newly-qualifying
		 * tuples.)
		 */
		usefast = false;
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

	if (usefast)
	{
		spool = _bt_spoolinit(index, indexInfo->ii_Unique);
		/*
	 	 * Different from spool,the uniqueness isn't checked
		 * for spool2.
	 	 */
		if (indexInfo->ii_Unique)
			spool2 = _bt_spoolinit(index, false);
	}

	/* start a heap scan */
	dead_count = 0;
	snapshot = (IsBootstrapProcessingMode() ? SnapshotNow : SnapshotAny);
	hscan = heap_beginscan(heap, 0, snapshot, 0, (ScanKey) NULL);
	XmaxRecent = 0;
	if (snapshot == SnapshotAny)
		GetXmaxRecent(&XmaxRecent);

	while (HeapTupleIsValid(htup = heap_getnext(hscan, 0)))
	{
		if (snapshot == SnapshotAny)
		{
			tupleIsAlive = HeapTupleSatisfiesNow(htup->t_data);
			if (!tupleIsAlive)
			{
				if ((htup->t_data->t_infomask & HEAP_XMIN_INVALID) != 0)
					continue;
				if (htup->t_data->t_infomask & HEAP_XMAX_COMMITTED &&
					htup->t_data->t_xmax < XmaxRecent)
					continue;
			}
		}
		else
			tupleIsAlive = true;
		
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

		/* form an index tuple and point it at the heap tuple */
		itup = index_formtuple(itupdesc, attdata, nulls);

		/*
		 * If the single index key is null, we don't insert it into the
		 * index.  Btrees support scans on <, <=, =, >=, and >. Relational
		 * algebra says that A op B (where op is one of the operators
		 * above) returns null if either A or B is null.  This means that
		 * no qualification used in an index scan could ever return true
		 * on a null attribute.  It also means that indices can't be used
		 * by ISNULL or NOTNULL scans, but that's an artifact of the
		 * strategy map architecture chosen in 1986, not of the way nulls
		 * are handled here.
		 */

		/*
		 * New comments: NULLs handling. While we can't do NULL
		 * comparison, we can follow simple rule for ordering items on
		 * btree pages - NULLs greater NOT_NULLs and NULL = NULL is TRUE.
		 * Sure, it's just rule for placing/finding items and no more -
		 * keytest'll return FALSE for a = 5 for items having 'a' isNULL.
		 * Look at _bt_compare for how it works.
		 *				 - vadim 03/23/97
		 *
		 * if (itup->t_info & INDEX_NULL_MASK) { pfree(itup); continue; }
		 */

		itup->t_tid = htup->t_self;
		btitem = _bt_formitem(itup);

		/*
		 * if we are doing bottom-up btree build, we insert the index into
		 * a spool file for subsequent processing.	otherwise, we insert
		 * into the btree.
		 */
		if (usefast)
		{
			if (tupleIsAlive || !spool2)
				_bt_spool(btitem, spool);
			else /* dead tuples are put into spool2 */
			{
				dead_count++;
				_bt_spool(btitem, spool2);
			}
		}
		else
			res = _bt_doinsert(index, btitem, indexInfo->ii_Unique, heap);

		pfree(btitem);
		pfree(itup);
		if (res)
			pfree(res);
	}

	/* okay, all heap tuples are indexed */
	heap_endscan(hscan);
	if (spool2 && !dead_count) /* spool2 was found to be unnecessary */
	{
		_bt_spooldestroy(spool2);
		spool2 = NULL;
	}

#ifndef OMIT_PARTIAL_INDEX
	if (pred != NULL || oldPred != NULL)
	{
		ExecDropTupleTable(tupleTable, true);
	}
#endif	 /* OMIT_PARTIAL_INDEX */
	FreeExprContext(econtext);

	/*
	 * if we are doing bottom-up btree build, finish the build by (1)
	 * completing the sort of the spool file, (2) inserting the sorted
	 * tuples into btree pages and (3) building the upper levels.
	 */
	if (usefast)
	{
		_bt_leafbuild(spool, spool2);
		_bt_spooldestroy(spool);
		if (spool2)
			_bt_spooldestroy(spool2);
	}

#ifdef BTREE_BUILD_STATS
	if (Show_btree_build_stats)
	{
		fprintf(stderr, "BTREE BUILD STATS\n");
		ShowUsage();
		ResetUsage();
	}
#endif /* BTREE_BUILD_STATS */

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
		bool		inplace = IsReindexProcessing();

		heap_close(heap, NoLock);
		index_close(index);

		UpdateStats(hrelid, nhtups, inplace);
		UpdateStats(irelid, nitups, inplace);
		if (oldPred != NULL)
		{
			if (nitups == nhtups)
				pred = NULL;
			if (!inplace)
				UpdateIndexPredicate(irelid, oldPred, pred);
		}
	}

	/* all done */
	BuildingBtree = false;

	PG_RETURN_VOID();
}

/*
 *	btinsert() -- insert an index tuple into a btree.
 *
 *		Descend the tree recursively, find the appropriate location for our
 *		new tuple, put it there, set its unique OID as appropriate, and
 *		return an InsertIndexResult to the caller.
 */
Datum
btinsert(PG_FUNCTION_ARGS)
{
	Relation		rel = (Relation) PG_GETARG_POINTER(0);
	Datum		   *datum = (Datum *) PG_GETARG_POINTER(1);
	char		   *nulls = (char *) PG_GETARG_POINTER(2);
	ItemPointer		ht_ctid = (ItemPointer) PG_GETARG_POINTER(3);
	Relation		heapRel = (Relation) PG_GETARG_POINTER(4);
	InsertIndexResult res;
	BTItem		btitem;
	IndexTuple	itup;

	/* generate an index tuple */
	itup = index_formtuple(RelationGetDescr(rel), datum, nulls);
	itup->t_tid = *ht_ctid;
	btitem = _bt_formitem(itup);

	res = _bt_doinsert(rel, btitem, rel->rd_uniqueindex, heapRel);

	pfree(btitem);
	pfree(itup);

	PG_RETURN_POINTER(res);
}

/*
 *	btgettuple() -- Get the next tuple in the scan.
 */
Datum
btgettuple(PG_FUNCTION_ARGS)
{
	IndexScanDesc		scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanDirection		dir = (ScanDirection) PG_GETARG_INT32(1);
	RetrieveIndexResult res;

	/*
	 * If we've already initialized this scan, we can just advance it in
	 * the appropriate direction.  If we haven't done so yet, we call a
	 * routine to get the first item in the scan.
	 */

	if (ItemPointerIsValid(&(scan->currentItemData)))
	{
		/*
		 * Restore scan position using heap TID returned by previous call
		 * to btgettuple(). _bt_restscan() re-grabs the read lock on
		 * the buffer, too.
		 */
		_bt_restscan(scan);
		res = _bt_next(scan, dir);
	}
	else
		res = _bt_first(scan, dir);

	/*
	 * Save heap TID to use it in _bt_restscan.  Then release the read
	 * lock on the buffer so that we aren't blocking other backends.
	 * NOTE: we do keep the pin on the buffer!
	 */
	if (res)
	{
		((BTScanOpaque) scan->opaque)->curHeapIptr = res->heap_iptr;
		LockBuffer(((BTScanOpaque) scan->opaque)->btso_curbuf,
				   BUFFER_LOCK_UNLOCK);
	}

	PG_RETURN_POINTER(res);
}

/*
 *	btbeginscan() -- start a scan on a btree index
 */
Datum
btbeginscan(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	bool		fromEnd = PG_GETARG_BOOL(1);
	uint16		keysz = PG_GETARG_UINT16(2);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(3);
	IndexScanDesc scan;

	/* get the scan */
	scan = RelationGetIndexScan(rel, fromEnd, keysz, scankey);

	/* register scan in case we change pages it's using */
	_bt_regscan(scan);

	PG_RETURN_POINTER(scan);
}

/*
 *	btrescan() -- rescan an index relation
 */
Datum
btrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc	scan = (IndexScanDesc) PG_GETARG_POINTER(0);
#ifdef NOT_USED					/* XXX surely it's wrong to ignore this? */
	bool			fromEnd = PG_GETARG_BOOL(1);
#endif
	ScanKey			scankey = (ScanKey) PG_GETARG_POINTER(2);
	ItemPointer iptr;
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	if (so == NULL)				/* if called from btbeginscan */
	{
		so = (BTScanOpaque) palloc(sizeof(BTScanOpaqueData));
		so->btso_curbuf = so->btso_mrkbuf = InvalidBuffer;
		so->keyData = (ScanKey) NULL;
		if (scan->numberOfKeys > 0)
			so->keyData = (ScanKey) palloc(scan->numberOfKeys * sizeof(ScanKeyData));
		scan->opaque = so;
		scan->flags = 0x0;
	}

	/* we aren't holding any read locks, but gotta drop the pins */
	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		ReleaseBuffer(so->btso_curbuf);
		so->btso_curbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	if (ItemPointerIsValid(iptr = &(scan->currentMarkData)))
	{
		ReleaseBuffer(so->btso_mrkbuf);
		so->btso_mrkbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	/*
	 * Reset the scan keys. Note that keys ordering stuff moved to
	 * _bt_first.	   - vadim 05/05/97
	 */
	so->numberOfKeys = scan->numberOfKeys;
	if (scan->numberOfKeys > 0)
	{
		memmove(scan->keyData,
				scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
		memmove(so->keyData,
				scankey,
				so->numberOfKeys * sizeof(ScanKeyData));
	}

	PG_RETURN_VOID();
}

void
btmovescan(IndexScanDesc scan, Datum v)
{
	ItemPointer iptr;
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pin */
	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		ReleaseBuffer(so->btso_curbuf);
		so->btso_curbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	so->keyData[0].sk_argument = v;
}

/*
 *	btendscan() -- close down a scan
 */
Datum
btendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc	scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer iptr;
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pins */
	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		if (BufferIsValid(so->btso_curbuf))
			ReleaseBuffer(so->btso_curbuf);
		so->btso_curbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	if (ItemPointerIsValid(iptr = &(scan->currentMarkData)))
	{
		if (BufferIsValid(so->btso_mrkbuf))
			ReleaseBuffer(so->btso_mrkbuf);
		so->btso_mrkbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	if (so->keyData != (ScanKey) NULL)
		pfree(so->keyData);
	pfree(so);

	_bt_dropscan(scan);

	PG_RETURN_VOID();
}

/*
 *	btmarkpos() -- save current scan position
 */
Datum
btmarkpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc	scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer iptr;
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pin */
	if (ItemPointerIsValid(iptr = &(scan->currentMarkData)))
	{
		ReleaseBuffer(so->btso_mrkbuf);
		so->btso_mrkbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	/* bump pin on current buffer for assignment to mark buffer */
	if (ItemPointerIsValid(&(scan->currentItemData)))
	{
		so->btso_mrkbuf = ReadBuffer(scan->relation,
								  BufferGetBlockNumber(so->btso_curbuf));
		scan->currentMarkData = scan->currentItemData;
		so->mrkHeapIptr = so->curHeapIptr;
	}

	PG_RETURN_VOID();
}

/*
 *	btrestrpos() -- restore scan to last saved position
 */
Datum
btrestrpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc	scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer iptr;
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;

	/* we aren't holding any read locks, but gotta drop the pin */
	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		ReleaseBuffer(so->btso_curbuf);
		so->btso_curbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	/* bump pin on marked buffer */
	if (ItemPointerIsValid(&(scan->currentMarkData)))
	{
		so->btso_curbuf = ReadBuffer(scan->relation,
								  BufferGetBlockNumber(so->btso_mrkbuf));
		scan->currentItemData = scan->currentMarkData;
		so->curHeapIptr = so->mrkHeapIptr;
	}

	PG_RETURN_VOID();
}

/* stubs */
Datum
btdelete(PG_FUNCTION_ARGS)
{
	Relation		rel = (Relation) PG_GETARG_POINTER(0);
	ItemPointer		tid = (ItemPointer) PG_GETARG_POINTER(1);

	/* adjust any active scans that will be affected by this deletion */
	_bt_adjscans(rel, tid);

	/* delete the data from the page */
	_bt_pagedel(rel, tid);

	PG_RETURN_VOID();
}

/*
 * Restore scan position when btgettuple is called to continue a scan.
 */
static void
_bt_restscan(IndexScanDesc scan)
{
	Relation	rel = scan->relation;
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Buffer		buf = so->btso_curbuf;
	Page		page;
	ItemPointer current = &(scan->currentItemData);
	OffsetNumber offnum = ItemPointerGetOffsetNumber(current),
				maxoff;
	BTPageOpaque opaque;
	ItemPointerData target = so->curHeapIptr;
	BTItem		item;
	BlockNumber blkno;

	/*
	 * Get back the read lock we were holding on the buffer.
	 * (We still have a reference-count pin on it, though.)
	 */
	LockBuffer(buf, BT_READ);

	page = BufferGetPage(buf);
	maxoff = PageGetMaxOffsetNumber(page);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);

	/*
	 * We use this as flag when first index tuple on page is deleted but
	 * we do not move left (this would slowdown vacuum) - so we set
	 * current->ip_posid before first index tuple on the current page
	 * (_bt_step will move it right)...
	 */
	if (!ItemPointerIsValid(&target))
	{
		ItemPointerSetOffsetNumber(current,
								   OffsetNumberPrev(P_FIRSTDATAKEY(opaque)));
		return;
	}

	/*
	 * The item we were on may have moved right due to insertions.
	 * Find it again.
	 */
	for (;;)
	{
		/* Check for item on this page */
		for (;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			item = (BTItem) PageGetItem(page, PageGetItemId(page, offnum));
			if (item->bti_itup.t_tid.ip_blkid.bi_hi ==
				target.ip_blkid.bi_hi &&
				item->bti_itup.t_tid.ip_blkid.bi_lo ==
				target.ip_blkid.bi_lo &&
				item->bti_itup.t_tid.ip_posid == target.ip_posid)
			{
				current->ip_posid = offnum;
				return;
			}
		}

		/*
		 * By here, the item we're looking for moved right at least one page
		 */
		if (P_RIGHTMOST(opaque))
			elog(FATAL, "_bt_restscan: my bits moved right off the end of the world!"
				 "\n\tRecreate index %s.", RelationGetRelationName(rel));

		blkno = opaque->btpo_next;
		_bt_relbuf(rel, buf, BT_READ);
		buf = _bt_getbuf(rel, blkno, BT_READ);
		page = BufferGetPage(buf);
		maxoff = PageGetMaxOffsetNumber(page);
		opaque = (BTPageOpaque) PageGetSpecialPointer(page);
		offnum = P_FIRSTDATAKEY(opaque);
		ItemPointerSet(current, blkno, offnum);
		so->btso_curbuf = buf;
	}
}

#ifdef XLOG
void btree_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8	info = record->xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_BTREE_DELETE)
		btree_xlog_delete(true, lsn, record);
	else if (info == XLOG_BTREE_INSERT)
		btree_xlog_insert(true, lsn, record);
	else if (info == XLOG_BTREE_SPLIT)
		btree_xlog_split(true, false, lsn, record);	/* new item on the right */
	else if (info == XLOG_BTREE_SPLEFT)
		btree_xlog_split(true, true, lsn, record);	/* new item on the left */
	else if (info == XLOG_BTREE_NEWROOT)
		btree_xlog_newroot(true, lsn, record);
	else
		elog(STOP, "btree_redo: unknown op code %u", info);
}

void btree_undo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8	info = record->xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_BTREE_DELETE)
		btree_xlog_delete(false, lsn, record);
	else if (info == XLOG_BTREE_INSERT)
		btree_xlog_insert(false, lsn, record);
	else if (info == XLOG_BTREE_SPLIT)
		btree_xlog_split(false, false, lsn, record);/* new item on the right */
	else if (info == XLOG_BTREE_SPLEFT)
		btree_xlog_split(false, true, lsn, record);	/* new item on the left */
	else if (info == XLOG_BTREE_NEWROOT)
		btree_xlog_newroot(false, lsn, record);
	else
		elog(STOP, "btree_undo: unknown op code %u", info);
}

static void btree_xlog_delete(bool redo, XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_delete	   *xlrec;
	Relation		   *reln;
	Buffer				buffer;
	Page				page;

	if (!redo)
		return;

	xlrec = (xl_btree_delete*) XLogRecGetData(record);
	reln = XLogOpenRelation(redo, RM_BTREE_ID, xlrec->target.node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer(false, reln, 
				ItemPointerGetBlockNumber(&(xlrec->target.tid)));
	if (!BufferIsValid(buffer))
		elog(STOP, "btree_delete_redo: block unfound");
	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(STOP, "btree_delete_redo: uninitialized page");

	PageIndexTupleDelete(page, ItemPointerGetOffsetNumber(&(xlrec->target.tid)));

	return;
}

static void btree_xlog_insert(bool redo, XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_insert	   *xlrec;
	Relation		   *reln;
	Buffer				buffer;
	Page				page;
	BTPageOpaque		pageop;

	xlrec = (xl_btree_insert*) XLogRecGetData(record);
	reln = XLogOpenRelation(redo, RM_BTREE_ID, xlrec->target.node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer((redo) ? true : false, reln, 
				ItemPointerGetBlockNumber(&(xlrec->target.tid)));
	if (!BufferIsValid(buffer))
		return;
	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(STOP, "btree_insert_%s: uninitialized page",
			(redo) ? "redo" : "undo");
	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	if (redo)
	{
		if (XLByteLE(lsn, PageGetLSN(page)))
			UnlockAndReleaseBuffer(buffer);
		else
		{
			Size		hsize = SizeOfBtreeInsert;
			RelFileNode	hnode;

			if (P_ISLEAF(pageop))
			{
				hsize += (sizeof(CommandId) + sizeof(RelFileNode));
				memcpy(&hnode, (char*)xlrec + SizeOfBtreeInsert + 
							sizeof(CommandId), sizeof(RelFileNode));
			}

			if (! _bt_add_item(page, 
					ItemPointerGetOffsetNumber(&(xlrec->target.tid)),
					(char*)xlrec + hsize,
					record->xl_len - hsize,
					&hnode))
				elog(STOP, "btree_insert_redo: failed to add item");

			PageSetLSN(page, lsn);
			PageSetSUI(page, ThisStartUpID);
			UnlockAndWriteBuffer(buffer);
		}
	}
	else
	{
		BTItemData		btdata;

		if (XLByteLT(PageGetLSN(page), lsn))
			elog(STOP, "btree_insert_undo: bad page LSN");

		if (! P_ISLEAF(pageop))
		{
			UnlockAndReleaseBuffer(buffer);
			return;
		}

		memcpy(&btdata, (char*)xlrec + SizeOfBtreeInsert + 
			sizeof(CommandId) + sizeof(RelFileNode), sizeof(BTItemData));

		_bt_del_item(reln, buffer, &btdata, true, lsn, record);

	}

	return;
}

static void
btree_xlog_split(bool redo, bool onleft, XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_split	   *xlrec;
	Relation		   *reln;
	BlockNumber			blkno;
	BlockNumber			parent;
	Buffer				buffer;
	Page				page;
	BTPageOpaque		pageop;
	char			   *op = (redo) ? "redo" : "undo";
	bool				isleaf;

	xlrec = (xl_btree_split*) XLogRecGetData(record);
	reln = XLogOpenRelation(redo, RM_BTREE_ID, xlrec->target.node);
	if (!RelationIsValid(reln))
		return;

	/* Left (original) sibling */
	blkno = (onleft) ? ItemPointerGetBlockNumber(&(xlrec->target.tid)) :
					BlockIdGetBlockNumber(xlrec->otherblk);
	buffer = XLogReadBuffer(false, reln, blkno);
	if (!BufferIsValid(buffer))
		elog(STOP, "btree_split_%s: lost left sibling", op);

	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(STOP, "btree_split_%s: uninitialized left sibling", op);

	pageop = (BTPageOpaque) PageGetSpecialPointer(page);
	isleaf = P_ISLEAF(pageop);
	parent = pageop->btpo_parent;

	if (redo)
	{
		if (XLByteLE(lsn, PageGetLSN(page)))
			UnlockAndReleaseBuffer(buffer);
		else
		{
			/* Delete items related to new right sibling */
			_bt_thin_left_page(page, record);

			if (onleft)
			{
				BTItemData	btdata;
				Size		hsize = SizeOfBtreeSplit;
				Size		itemsz;
				RelFileNode	hnode;

				pageop->btpo_next = BlockIdGetBlockNumber(xlrec->otherblk);
				if (isleaf)
				{
					hsize += (sizeof(CommandId) + sizeof(RelFileNode));
					memcpy(&hnode, (char*)xlrec + SizeOfBtreeSplit + 
								sizeof(CommandId), sizeof(RelFileNode));
				}

				memcpy(&btdata, (char*)xlrec + hsize, sizeof(BTItemData));
				itemsz = IndexTupleDSize(btdata.bti_itup) +
							(sizeof(BTItemData) - sizeof(IndexTupleData));

				if (! _bt_add_item(page, 
						ItemPointerGetOffsetNumber(&(xlrec->target.tid)),
						(char*)xlrec + hsize,
						itemsz,
						&hnode))
					elog(STOP, "btree_split_redo: failed to add item");
			}
			else
				pageop->btpo_next = ItemPointerGetBlockNumber(&(xlrec->target.tid));

			PageSetLSN(page, lsn);
			PageSetSUI(page, ThisStartUpID);
			UnlockAndWriteBuffer(buffer);
		}
	}
	else	/* undo */
	{
		if (XLByteLT(PageGetLSN(page), lsn))
			elog(STOP, "btree_split_undo: bad left sibling LSN");

		if (! isleaf || ! onleft)
			UnlockAndReleaseBuffer(buffer);
		else
		{
			BTItemData		btdata;

			memcpy(&btdata, (char*)xlrec + SizeOfBtreeSplit + 
				sizeof(CommandId) + sizeof(RelFileNode), sizeof(BTItemData));

			_bt_del_item(reln, buffer, &btdata, false, lsn, record);
		}
	}

	/* Right (new) sibling */
	blkno = (onleft) ? BlockIdGetBlockNumber(xlrec->otherblk) : 
					ItemPointerGetBlockNumber(&(xlrec->target.tid));
	buffer = XLogReadBuffer((redo) ? true : false, reln, blkno);
	if (!BufferIsValid(buffer))
		elog(STOP, "btree_split_%s: lost right sibling", op);

	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
	{
		if (!redo)
			elog(STOP, "btree_split_undo: uninitialized right sibling");
		PageInit(page, BufferGetPageSize(buffer), 0);
	}

	if (redo)
	{
		if (XLByteLE(lsn, PageGetLSN(page)))
			UnlockAndReleaseBuffer(buffer);
		else
		{
			Size		hsize = SizeOfBtreeSplit;
			BTItemData	btdata;
			Size		itemsz;

			_bt_pageinit(page, BufferGetPageSize(buffer));
			pageop = (BTPageOpaque) PageGetSpecialPointer(page);
			if (isleaf)
			{
				pageop->btpo_flags |= BTP_LEAF;
				hsize += (sizeof(CommandId) + sizeof(RelFileNode));
			}
			if (onleft)		/* skip target item */
			{
				memcpy(&btdata, (char*)xlrec + hsize, sizeof(BTItemData));
				itemsz = IndexTupleDSize(btdata.bti_itup) +
							(sizeof(BTItemData) - sizeof(IndexTupleData));
				hsize += itemsz;
			}

			for (char* item = (char*)xlrec + hsize;
					item < (char*)record + record->xl_len; )
			{
				memcpy(&btdata, item, sizeof(BTItemData));
				itemsz = IndexTupleDSize(btdata.bti_itup) +
							(sizeof(BTItemData) - sizeof(IndexTupleData));
				itemsz = MAXALIGN(itemsz);
				if (PageAddItem(page, (Item) item, itemsz, FirstOffsetNumber,	
						LP_USED) == InvalidOffsetNumber)
					elog(STOP, "btree_split_redo: can't add item to right sibling");
				item += itemsz;
			}

			pageop->btpo_prev = (onleft) ? ItemPointerGetBlockNumber(&(xlrec->target.tid)) :
					BlockIdGetBlockNumber(xlrec->otherblk);
			pageop->btpo_next = BlockIdGetBlockNumber(xlrec->rightblk);
			pageop->btpo_parent = parent;

			PageSetLSN(page, lsn);
			PageSetSUI(page, ThisStartUpID);
			UnlockAndWriteBuffer(buffer);
		}
	}
	else	/* undo */
	{
		if (XLByteLT(PageGetLSN(page), lsn))
			elog(STOP, "btree_split_undo: bad right sibling LSN");

		if (! isleaf || onleft)
			UnlockAndReleaseBuffer(buffer);
		else
		{
			char		tbuf[BLCKSZ];
			int			cnt;
			char	   *item;
			Size		itemsz;

			item = (char*)xlrec + SizeOfBtreeSplit +
					sizeof(CommandId) + sizeof(RelFileNode);
			for (cnt = 0; item < (char*)record + record->xl_len; )
			{
				BTItem	btitem = (BTItem)
					(tbuf + cnt * (MAXALIGN(sizeof(BTItemData))));
				memcpy(btitem, item, sizeof(BTItemData));
				itemsz = IndexTupleDSize(btitem->bti_itup) +
							(sizeof(BTItemData) - sizeof(IndexTupleData));
				itemsz = MAXALIGN(itemsz);
				item += itemsz;
				cnt++;
			}
			cnt -= ItemPointerGetOffsetNumber(&(xlrec->target.tid));
			if (cnt < 0)
				elog(STOP, "btree_split_undo: target item unfound in right sibling");

			item = tbuf + cnt * (MAXALIGN(sizeof(BTItemData)));

			_bt_del_item(reln, buffer, (BTItem)item, false, lsn, record);
		}
	}

	/* Right (next) page */
	blkno = BlockIdGetBlockNumber(xlrec->rightblk);
	buffer = XLogReadBuffer(false, reln, blkno);
	if (!BufferIsValid(buffer))
		elog(STOP, "btree_split_%s: lost next right page", op);

	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(STOP, "btree_split_%s: uninitialized next right page", op);

	if (redo)
	{
		if (XLByteLE(lsn, PageGetLSN(page)))
			UnlockAndReleaseBuffer(buffer);
		else
		{
			pageop = (BTPageOpaque) PageGetSpecialPointer(page);
			pageop->btpo_prev = (onleft) ? BlockIdGetBlockNumber(xlrec->otherblk) :
					ItemPointerGetBlockNumber(&(xlrec->target.tid));

			PageSetLSN(page, lsn);
			PageSetSUI(page, ThisStartUpID);
			UnlockAndWriteBuffer(buffer);
		}
	}
	else	/* undo */
	{
		if (XLByteLT(PageGetLSN(page), lsn))
			elog(STOP, "btree_split_undo: bad next right page LSN");

		UnlockAndReleaseBuffer(buffer);
	}

}

static void btree_xlog_newroot(bool redo, XLogRecPtr lsn, XLogRecord *record)
{
	xl_btree_newroot   *xlrec;
	Relation		   *reln;
	Buffer				buffer;
	Page				page;
	Buffer				metabuf;
	Page				metapg;

	if (!redo)
		return;

	xlrec = (xl_btree_newroot*) XLogRecGetData(record);
	reln = XLogOpenRelation(redo, RM_BTREE_ID, xlrec->node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer(true, reln, BlockIdGetBlockNumber(&(xlrec->rootblk)));
	if (!BufferIsValid(buffer))
		elog(STOP, "btree_newroot_redo: no root page");
	metabuf = XLogReadBuffer(false, reln, BTREE_METAPAGE);
	if (!BufferIsValid(buffer))
		elog(STOP, "btree_newroot_redo: no metapage");
	page = (Page) BufferGetPage(buffer);

	if (PageIsNew((PageHeader) page) || XLByteLT(PageGetLSN(page), lsn))
	{
		_bt_pageinit(page, BufferGetPageSize(buffer));
		pageop = (BTPageOpaque) PageGetSpecialPointer(page);

		pageop->btpo_flags |= BTP_ROOT;
		pageop->btpo_prev = pageop->btpo_next = P_NONE;
		pageop->btpo_parent = BTREE_METAPAGE;

		if (record->xl_len == SizeOfBtreeNewroot)	/* no childs */
			pageop->btpo_flags |= BTP_LEAF;
		else
		{
			BTItemData	btdata;
			Size		itemsz;

			for (char* item = (char*)xlrec + SizeOfBtreeNewroot;
					item < (char*)record + record->xl_len; )
			{
				memcpy(&btdata, item, sizeof(BTItemData));
				itemsz = IndexTupleDSize(btdata.bti_itup) +
							(sizeof(BTItemData) - sizeof(IndexTupleData));
				itemsz = MAXALIGN(itemsz);
				if (PageAddItem(page, (Item) item, itemsz, FirstOffsetNumber,	
						LP_USED) == InvalidOffsetNumber)
					elog(STOP, "btree_newroot_redo: can't add item");
				item += itemsz;
			}
		}

		PageSetLSN(page, lsn);
		PageSetSUI(page, ThisStartUpID);
		UnlockAndWriteBuffer(buffer);
	}
	else
		UnlockAndReleaseBuffer(buffer);

	metapg = BufferGetPage(metabuf);
	if (PageIsNew((PageHeader) metapg))
	{
		BTMetaPageData	md;

		_bt_pageinit(metapg, BufferGetPageSize(metabuf));
		md.btm_magic = BTREE_MAGIC;
		md.btm_version = BTREE_VERSION;
		md.btm_root = P_NONE;
		md.btm_level = 0;
		memcpy((char *) BTPageGetMeta(pg), (char *) &md, sizeof(md));
	}

	if (XLByteLT(PageGetLSN(metapg), lsn))
	{
		BTMetaPageData	   *metad = BTPageGetMeta(metapg);

		metad->btm_root = BlockIdGetBlockNumber(&(xlrec->rootblk));
		(metad->btm_level)++;
		PageSetLSN(metapg, lsn);
		PageSetSUI(metapg, ThisStartUpID);
		UnlockAndWriteBuffer(metabuf);
	}
	else
		UnlockAndReleaseBuffer(metabuf);

	return;
}

/*
 * UNDO insertion on *leaf* page: 
 * - find inserted tuple;
 * - delete it if heap tuple was inserted by the same xaction
 */
static void
_bt_del_item(Relation reln, Buffer buffer, BTItem btitem, bool insert, 
				XLogRecPtr lsn, XLogRecord *record)
{
	char		   *xlrec = (char*) XLogRecGetData(record);
	Page			page = (Page) BufferGetPage(buffer);
	BTPageOpaque	pageop;
	BlockNumber		blkno;
	OffsetNumber	offno;
	ItemId			lp;

	for ( ; ; )
	{
		offno = _bt_find_btitem(page, btitem);
		if (offno != InvalidOffsetNumber)
			break;
		pageop = (BTPageOpaque) PageGetSpecialPointer(page);
		if (P_RIGHTMOST(pageop))
			break;
		blkno = pageop->btpo_next;
		UnlockAndReleaseBuffer(buffer);
		buffer = XLogReadBuffer(false, reln, blkno);
		if (!BufferIsValid(buffer))
			elog(STOP, "btree_%s_undo: lost right sibling",
				(insert) ? "insert" : "split");
		page = (Page) BufferGetPage(buffer);
		if (PageIsNew((PageHeader) page))
			elog(STOP, "btree_%s_undo: uninitialized right sibling",
				(insert) ? "insert" : "split");
		if (XLByteLT(PageGetLSN(page), lsn))
			break;
	}

	if (offno == InvalidOffsetNumber)	/* not found */
	{
		if (!InRecovery)
			elog(STOP, "btree_%s_undo: lost target tuple in rollback",
				(insert) ? "insert" : "split");
		UnlockAndReleaseBuffer(buffer);
		return;
	}

	lp = PageGetItemId(page, offno);
	if (ItemIdDeleted(lp))	/* marked for deletion */
	{
		if (!InRecovery)
			elog(STOP, "btree_%s_undo: deleted target tuple in rollback",
				(insert) ? "insert" : "split");
	}
	else if (InRecovery)	/* check heap tuple */
	{
		int			result;
		CommandId	cid;
		RelFileNode	hnode;
		Size		hsize = (insert) ? SizeOfBtreeInsert : SizeOfBtreeSplit;

		memcpy(&cid, (char*)xlrec + hsize, sizeof(CommandId));
		memcpy(&hnode, (char*)xlrec + hsize + sizeof(CommandId), sizeof(RelFileNode));
		result = XLogCheckHeapTuple(hnode, &(btitem->bti_itup.t_tid),
					record->xl_xid, cid);
		if (result <= 0)	/* no tuple or not owner */
		{
			UnlockAndReleaseBuffer(buffer);
			return;
		}
	}
	else if (! BufferIsUpdatable(buffer))	/* normal rollback */
	{
		lp->lp_flags |= LP_DELETE;
		MarkBufferForCleanup(buffer, IndexPageCleanup);
		return;
	}

	PageIndexTupleDelete(page, offno);
	if (InRecovery)
	{
		pageop = (BTPageOpaque) PageGetSpecialPointer(page);
		pageop->btpo_flags |= BTP_REORDER;
	}
	UnlockAndWriteBuffer(buffer);

	return;
}

static bool
_bt_add_item(Page page, OffsetNumber offno, 
	char* item, Size size, RelFileNode* hnode)
{
	BTPageOpaque	pageop = (BTPageOpaque) PageGetSpecialPointer(page);

	if (offno > PageGetMaxOffsetNumber(page) + 1)
	{
		if (! (pageop->btpo_flags & BTP_REORDER))
		{
			elog(NOTICE, "btree_add_item: BTP_REORDER flag was expected");
			pageop->btpo_flags |= BTP_REORDER;
		}
		offno = PageGetMaxOffsetNumber(page) + 1;
	}

	if (PageAddItem(page, (Item) item, size, offno,	
			LP_USED) == InvalidOffsetNumber)
	{
		/* ops, not enough space - try to deleted dead tuples */
		bool		result;

		if (! P_ISLEAF(pageop))
			return(false);
		result = _bt_cleanup_page(page, hnode);
		if (!result || PageAddItem(page, (Item) item, size, offno,	
				LP_USED) == InvalidOffsetNumber)
			return(false);
	}

	return(true);
}

#endif
