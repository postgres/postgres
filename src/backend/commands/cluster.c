/*-------------------------------------------------------------------------
 *
 * cluster.c
 *	  Paul Brown's implementation of cluster index.
 *
 *	  I am going to use the rename function as a model for this in the
 *	  parser and executor, and the vacuum code as an example in this
 *	  file. As I go - in contrast to the rest of postgres - there will
 *	  be BUCKETS of comments. This is to allow reviewers to understand
 *	  my (probably bogus) assumptions about the way this works.
 *														[pbrown '94]
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/cluster.c,v 1.73 2002/03/26 19:15:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/pg_index.h"
#include "catalog/pg_proc.h"
#include "commands/cluster.h"
#include "commands/command.h"
#include "commands/rename.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/temprel.h"


static Oid	copy_heap(Oid OIDOldHeap, char *NewName, bool istemp);
static void copy_index(Oid OIDOldIndex, Oid OIDNewHeap, char *NewIndexName,
					   bool istemp);
static void rebuildheap(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex);

/*
 * cluster
 *
 * STILL TO DO:
 *	 Create a list of all the other indexes on this relation. Because
 *	 the cluster will wreck all the tids, I'll need to destroy bogus
 *	 indexes. The user will have to re-create them. Not nice, but
 *	 I'm not a nice guy. The alternative is to try some kind of post
 *	 destroy re-build. This may be possible. I'll check out what the
 *	 index create functiond want in the way of paramaters. On the other
 *	 hand, re-creating n indexes may blow out the space.
 */
void
cluster(RangeVar *oldrelation, char *oldindexname)
{
	Oid			OIDOldHeap,
				OIDOldIndex,
				OIDNewHeap;
	Relation	OldHeap,
				OldIndex;
	bool		istemp;
	char		NewHeapName[NAMEDATALEN];
	char		NewIndexName[NAMEDATALEN];
	RangeVar   *saveoldrelation;
	RangeVar   *saveoldindex;
	RangeVar   *NewHeap;
	RangeVar   *NewIndex;

	/*
	 * FIXME SCHEMAS: The old code had the comment:
	 * "Copy the arguments into local storage, just to be safe."
	 * By using copyObject we are not using local storage.
	 * Was that really necessary?
	 */
	saveoldrelation = copyObject(oldrelation);
	saveoldindex = copyObject(oldrelation);
	saveoldindex->relname = pstrdup(oldindexname);

	/*
	 * We grab exclusive access to the target rel and index for the
	 * duration of the transaction.
	 */
	OldHeap = heap_openrv(saveoldrelation, AccessExclusiveLock);
	OIDOldHeap = RelationGetRelid(OldHeap);

	OldIndex = index_openrv(saveoldindex);
	LockRelation(OldIndex, AccessExclusiveLock);
	OIDOldIndex = RelationGetRelid(OldIndex);

	istemp = is_temp_rel_name(saveoldrelation->relname);

	/*
	 * Check that index is in fact an index on the given relation
	 */
	if (OldIndex->rd_index->indrelid != OIDOldHeap)
		elog(ERROR, "CLUSTER: \"%s\" is not an index for table \"%s\"",
			 saveoldindex->relname, saveoldrelation->relname);

	/* Drop relcache refcnts, but do NOT give up the locks */
	heap_close(OldHeap, NoLock);
	index_close(OldIndex);

	/*
	 * Create the new heap with a temporary name.
	 */
	snprintf(NewHeapName, NAMEDATALEN, "temp_%u", OIDOldHeap);

	OIDNewHeap = copy_heap(OIDOldHeap, NewHeapName, istemp);

	/* We do not need CommandCounterIncrement() because copy_heap did it. */

	/*
	 * Copy the heap data into the new table in the desired order.
	 */
	rebuildheap(OIDNewHeap, OIDOldHeap, OIDOldIndex);

	/* To make the new heap's data visible. */
	CommandCounterIncrement();

	/* Create new index over the tuples of the new heap. */
	snprintf(NewIndexName, NAMEDATALEN, "temp_%u", OIDOldIndex);

	copy_index(OIDOldIndex, OIDNewHeap, NewIndexName, istemp);

	CommandCounterIncrement();

	/* Destroy old heap (along with its index) and rename new. */
	heap_drop_with_catalog(saveoldrelation->relname, allowSystemTableMods);

	CommandCounterIncrement();

	NewHeap = copyObject(saveoldrelation);
	NewHeap->relname = NewHeapName;
	NewIndex = copyObject(saveoldindex);
	NewIndex->relname = NewIndexName;
	
	renamerel(NewHeap, saveoldrelation->relname);

	/* This one might be unnecessary, but let's be safe. */
	CommandCounterIncrement();

	renamerel(NewIndex, saveoldindex->relname);
}

static Oid
copy_heap(Oid OIDOldHeap, char *NewName, bool istemp)
{
	TupleDesc	OldHeapDesc,
				tupdesc;
	Oid			OIDNewHeap;
	Relation	OldHeap;

	OldHeap = heap_open(OIDOldHeap, AccessExclusiveLock);
	OldHeapDesc = RelationGetDescr(OldHeap);

	/*
	 * Need to make a copy of the tuple descriptor, since
	 * heap_create_with_catalog modifies it.
	 */
	tupdesc = CreateTupleDescCopyConstr(OldHeapDesc);

	OIDNewHeap = heap_create_with_catalog(NewName,
										  RelationGetNamespace(OldHeap),
										  tupdesc,
										  OldHeap->rd_rel->relkind,
										  OldHeap->rd_rel->relhasoids,
										  istemp,
										  allowSystemTableMods);

	/*
	 * Advance command counter so that the newly-created relation's
	 * catalog tuples will be visible to heap_open.
	 */
	CommandCounterIncrement();

	/*
	 * If necessary, create a TOAST table for the new relation. Note that
	 * AlterTableCreateToastTable ends with CommandCounterIncrement(), so
	 * that the TOAST table will be visible for insertion.
	 */
	AlterTableCreateToastTable(OIDNewHeap, true);

	heap_close(OldHeap, NoLock);

	return OIDNewHeap;
}

static void
copy_index(Oid OIDOldIndex, Oid OIDNewHeap, char *NewIndexName, bool istemp)
{
	Relation	OldIndex,
				NewHeap;
	IndexInfo  *indexInfo;

	NewHeap = heap_open(OIDNewHeap, AccessExclusiveLock);
	OldIndex = index_open(OIDOldIndex);

	/*
	 * Create a new index like the old one.  To do this I get the info
	 * from pg_index, and add a new index with a temporary name (that will
	 * be changed later).
	 */
	indexInfo = BuildIndexInfo(OldIndex->rd_index);

	index_create(OIDNewHeap,
				 NewIndexName,
				 indexInfo,
				 OldIndex->rd_rel->relam,
				 OldIndex->rd_index->indclass,
				 istemp,
				 OldIndex->rd_index->indisprimary,
				 allowSystemTableMods);

	setRelhasindex(OIDNewHeap, true,
				   OldIndex->rd_index->indisprimary, InvalidOid);

	index_close(OldIndex);
	heap_close(NewHeap, NoLock);
}


static void
rebuildheap(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex)
{
	Relation	LocalNewHeap,
				LocalOldHeap,
				LocalOldIndex;
	IndexScanDesc ScanDesc;
	RetrieveIndexResult ScanResult;

	/*
	 * Open the relations I need. Scan through the OldHeap on the OldIndex
	 * and insert each tuple into the NewHeap.
	 */
	LocalNewHeap = heap_open(OIDNewHeap, AccessExclusiveLock);
	LocalOldHeap = heap_open(OIDOldHeap, AccessExclusiveLock);
	LocalOldIndex = index_open(OIDOldIndex);

	ScanDesc = index_beginscan(LocalOldIndex, false, 0, (ScanKey) NULL);

	while ((ScanResult = index_getnext(ScanDesc, ForwardScanDirection)) != NULL)
	{
		HeapTupleData LocalHeapTuple;
		Buffer		LocalBuffer;

		CHECK_FOR_INTERRUPTS();

		LocalHeapTuple.t_self = ScanResult->heap_iptr;
		LocalHeapTuple.t_datamcxt = NULL;
		LocalHeapTuple.t_data = NULL;
		heap_fetch(LocalOldHeap, SnapshotNow, &LocalHeapTuple, &LocalBuffer,
				   ScanDesc);
		if (LocalHeapTuple.t_data != NULL)
		{
			/*
			 * We must copy the tuple because heap_insert() will overwrite
			 * the commit-status fields of the tuple it's handed, and the
			 * retrieved tuple will actually be in a disk buffer!  Thus,
			 * the source relation would get trashed, which is bad news if
			 * we abort later on.  (This was a bug in releases thru 7.0)
			 */
			HeapTuple	copiedTuple = heap_copytuple(&LocalHeapTuple);

			ReleaseBuffer(LocalBuffer);
			heap_insert(LocalNewHeap, copiedTuple);
			heap_freetuple(copiedTuple);
		}
		pfree(ScanResult);
	}

	index_endscan(ScanDesc);

	index_close(LocalOldIndex);
	heap_close(LocalOldHeap, NoLock);
	heap_close(LocalNewHeap, NoLock);
}
