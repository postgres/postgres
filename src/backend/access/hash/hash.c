/*-------------------------------------------------------------------------
 *
 * hash.c
 *	  Implementation of Margo Seltzer's Hashing package for postgres.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/hash.c,v 1.46 2000/11/30 08:46:20 vadim Exp $
 *
 * NOTES
 *	  This file contains only the public interface routines.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "miscadmin.h"

bool		BuildingHash = false;

#include "access/xlogutils.h"


/*
 *	hashbuild() -- build a new hash index.
 *
 *		We use a global variable to record the fact that we're creating
 *		a new index.  This is used to avoid high-concurrency locking,
 *		since the index won't be visible until this transaction commits
 *		and since building is guaranteed to be single-threaded.
 */
Datum
hashbuild(PG_FUNCTION_ARGS)
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
	HashItem	hitem;
	Node	   *pred = indexInfo->ii_Predicate;
#ifndef OMIT_PARTIAL_INDEX
	TupleTable	tupleTable;
	TupleTableSlot *slot;
#endif
	ExprContext *econtext;
	InsertIndexResult res = NULL;

	/* note that this is a new hash */
	BuildingHash = true;

	/* initialize the hash index metadata page (if this is a new index) */
	if (oldPred == NULL)
		_hash_metapinit(index);

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

		/* form an index tuple and point it at the heap tuple */
		itup = index_formtuple(itupdesc, attdata, nulls);

		/*
		 * If the single index key is null, we don't insert it into the
		 * index.  Hash tables support scans on '='. Relational algebra
		 * says that A = B returns null if either A or B is null.  This
		 * means that no qualification used in an index scan could ever
		 * return true on a null attribute.  It also means that indices
		 * can't be used by ISNULL or NOTNULL scans, but that's an
		 * artifact of the strategy map architecture chosen in 1986, not
		 * of the way nulls are handled here.
		 */

		if (itup->t_info & INDEX_NULL_MASK)
		{
			pfree(itup);
			continue;
		}

		itup->t_tid = htup->t_self;
		hitem = _hash_formitem(itup);

		res = _hash_doinsert(index, hitem);

		pfree(hitem);
		pfree(itup);
		pfree(res);
	}

	/* okay, all heap tuples are indexed */
	heap_endscan(hscan);

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

	/* all done */
	BuildingHash = false;

	PG_RETURN_VOID();
}

/*
 *	hashinsert() -- insert an index tuple into a hash table.
 *
 *	Hash on the index tuple's key, find the appropriate location
 *	for the new tuple, put it there, and return an InsertIndexResult
 *	to the caller.
 */
Datum
hashinsert(PG_FUNCTION_ARGS)
{
	Relation		rel = (Relation) PG_GETARG_POINTER(0);
	Datum		   *datum = (Datum *) PG_GETARG_POINTER(1);
	char		   *nulls = (char *) PG_GETARG_POINTER(2);
	ItemPointer		ht_ctid = (ItemPointer) PG_GETARG_POINTER(3);
#ifdef NOT_USED
	Relation		heapRel = (Relation) PG_GETARG_POINTER(4);
#endif
	InsertIndexResult res;
	HashItem	hitem;
	IndexTuple	itup;

	/* generate an index tuple */
	itup = index_formtuple(RelationGetDescr(rel), datum, nulls);
	itup->t_tid = *ht_ctid;

	if (itup->t_info & INDEX_NULL_MASK)
		PG_RETURN_POINTER((InsertIndexResult) NULL);

	hitem = _hash_formitem(itup);

	res = _hash_doinsert(rel, hitem);

	pfree(hitem);
	pfree(itup);

	PG_RETURN_POINTER(res);
}


/*
 *	hashgettuple() -- Get the next tuple in the scan.
 */
Datum
hashgettuple(PG_FUNCTION_ARGS)
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
		res = _hash_next(scan, dir);
	else
		res = _hash_first(scan, dir);

	PG_RETURN_POINTER(res);
}


/*
 *	hashbeginscan() -- start a scan on a hash index
 */
Datum
hashbeginscan(PG_FUNCTION_ARGS)
{
	Relation	rel = (Relation) PG_GETARG_POINTER(0);
	bool		fromEnd = PG_GETARG_BOOL(1);
	uint16		keysz = PG_GETARG_UINT16(2);
	ScanKey		scankey = (ScanKey) PG_GETARG_POINTER(3);
	IndexScanDesc scan;
	HashScanOpaque so;

	scan = RelationGetIndexScan(rel, fromEnd, keysz, scankey);
	so = (HashScanOpaque) palloc(sizeof(HashScanOpaqueData));
	so->hashso_curbuf = so->hashso_mrkbuf = InvalidBuffer;
	scan->opaque = so;
	scan->flags = 0x0;

	/* register scan in case we change pages it's using */
	_hash_regscan(scan);

	PG_RETURN_POINTER(scan);
}

/*
 *	hashrescan() -- rescan an index relation
 */
Datum
hashrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc	scan = (IndexScanDesc) PG_GETARG_POINTER(0);
#ifdef NOT_USED					/* XXX surely it's wrong to ignore this? */
	bool			fromEnd = PG_GETARG_BOOL(1);
#endif
	ScanKey			scankey = (ScanKey) PG_GETARG_POINTER(2);
	ItemPointer iptr;
	HashScanOpaque so;

	so = (HashScanOpaque) scan->opaque;

	/* we hold a read lock on the current page in the scan */
	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		_hash_relbuf(scan->relation, so->hashso_curbuf, HASH_READ);
		so->hashso_curbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}
	if (ItemPointerIsValid(iptr = &(scan->currentMarkData)))
	{
		_hash_relbuf(scan->relation, so->hashso_mrkbuf, HASH_READ);
		so->hashso_mrkbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	/* reset the scan key */
	if (scan->numberOfKeys > 0)
	{
		memmove(scan->keyData,
				scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
	}

	PG_RETURN_VOID();
}

/*
 *	hashendscan() -- close down a scan
 */
Datum
hashendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc	scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer iptr;
	HashScanOpaque so;

	so = (HashScanOpaque) scan->opaque;

	/* release any locks we still hold */
	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		_hash_relbuf(scan->relation, so->hashso_curbuf, HASH_READ);
		so->hashso_curbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	if (ItemPointerIsValid(iptr = &(scan->currentMarkData)))
	{
		if (BufferIsValid(so->hashso_mrkbuf))
			_hash_relbuf(scan->relation, so->hashso_mrkbuf, HASH_READ);
		so->hashso_mrkbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	/* don't need scan registered anymore */
	_hash_dropscan(scan);

	/* be tidy */
	pfree(scan->opaque);

	PG_RETURN_VOID();
}

/*
 *	hashmarkpos() -- save current scan position
 *
 */
Datum
hashmarkpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc	scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer iptr;
	HashScanOpaque so;

	so = (HashScanOpaque) scan->opaque;

	/* release lock on old marked data, if any */
	if (ItemPointerIsValid(iptr = &(scan->currentMarkData)))
	{
		_hash_relbuf(scan->relation, so->hashso_mrkbuf, HASH_READ);
		so->hashso_mrkbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	/* bump lock on currentItemData and copy to currentMarkData */
	if (ItemPointerIsValid(&(scan->currentItemData)))
	{
		so->hashso_mrkbuf = _hash_getbuf(scan->relation,
								 BufferGetBlockNumber(so->hashso_curbuf),
										 HASH_READ);
		scan->currentMarkData = scan->currentItemData;
	}

	PG_RETURN_VOID();
}

/*
 *	hashrestrpos() -- restore scan to last saved position
 */
Datum
hashrestrpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc	scan = (IndexScanDesc) PG_GETARG_POINTER(0);
	ItemPointer iptr;
	HashScanOpaque so;

	so = (HashScanOpaque) scan->opaque;

	/* release lock on current data, if any */
	if (ItemPointerIsValid(iptr = &(scan->currentItemData)))
	{
		_hash_relbuf(scan->relation, so->hashso_curbuf, HASH_READ);
		so->hashso_curbuf = InvalidBuffer;
		ItemPointerSetInvalid(iptr);
	}

	/* bump lock on currentMarkData and copy to currentItemData */
	if (ItemPointerIsValid(&(scan->currentMarkData)))
	{
		so->hashso_curbuf = _hash_getbuf(scan->relation,
								 BufferGetBlockNumber(so->hashso_mrkbuf),
										 HASH_READ);

		scan->currentItemData = scan->currentMarkData;
	}

	PG_RETURN_VOID();
}

/* stubs */
Datum
hashdelete(PG_FUNCTION_ARGS)
{
	Relation		rel = (Relation) PG_GETARG_POINTER(0);
	ItemPointer		tid = (ItemPointer) PG_GETARG_POINTER(1);

	/* adjust any active scans that will be affected by this deletion */
	_hash_adjscans(rel, tid);

	/* delete the data from the page */
	_hash_pagedel(rel, tid);

	PG_RETURN_VOID();
}

void
hash_redo(XLogRecPtr lsn, XLogRecord *record)
{
	elog(STOP, "hash_redo: unimplemented");
}

void
hash_undo(XLogRecPtr lsn, XLogRecord *record)
{
	elog(STOP, "hash_undo: unimplemented");
}
 
void
hash_desc(char *buf, uint8 xl_info, char* rec)
{
}
