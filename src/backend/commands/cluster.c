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
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/cluster.c,v 1.85 2002/08/10 21:00:34 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/catname.h"
#include "commands/cluster.h"
#include "commands/tablecmds.h"
#include "miscadmin.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/relcache.h"

/*
 * We need one of these structs for each index in the relation to be
 * clustered.  It's basically the data needed by index_create() so
 * we can recreate the indexes after destroying the old heap.
 */
typedef struct
{
	char	   *indexName;
	IndexInfo  *indexInfo;
	Oid			accessMethodOID;
	Oid		   *classOID;
	Oid			indexOID;
	bool		isPrimary;
} IndexAttrs;

static Oid	copy_heap(Oid OIDOldHeap, const char *NewName);
static void rebuildheap(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex);
static List *get_indexattr_list (Oid OIDOldHeap);
static void recreate_indexattr(Oid OIDOldHeap, List *indexes);
static void swap_relfilenodes(Oid r1, Oid r2);

Relation RelationIdGetRelation(Oid relationId);

/*
 * cluster
 *
 * This clusters the table by creating a new, clustered table and
 * swapping the relfilenodes of the new table and the old table, so
 * the OID of the original table is preserved.  Thus we do not lose
 * GRANT, inheritance nor references to this table (this was a bug
 * in releases thru 7.3)
 *
 * Also create new indexes and swap the filenodes with the old indexes
 * the same way we do for the relation.
 *
 * TODO:
 * 		maybe we can get away with AccessShareLock for the table.
 * 		Concurrency would be much improved.  Only acquire
 * 		AccessExclusiveLock right before swapping the filenodes.
 * 		This would allow users to CLUSTER on a regular basis,
 * 		practically eliminating the need for auto-clustered indexes.
 */
void
cluster(RangeVar *oldrelation, char *oldindexname)
{
	Oid			OIDOldHeap,
				OIDOldIndex,
				OIDNewHeap;
	Relation	OldHeap,
				OldIndex;
	char		NewHeapName[NAMEDATALEN];
	ObjectAddress object;
	List	   *indexes;

	/*
	 * We grab exclusive access to the target rel and index for the
	 * duration of the transaction.
	 */
	OldHeap = heap_openrv(oldrelation, AccessExclusiveLock);
	OIDOldHeap = RelationGetRelid(OldHeap);

	/*
	 * The index is expected to be in the same namespace as the relation.
	 */
	OIDOldIndex = get_relname_relid(oldindexname,
									RelationGetNamespace(OldHeap));
	if (!OidIsValid(OIDOldIndex))
		elog(ERROR, "CLUSTER: cannot find index \"%s\" for table \"%s\"",
			 oldindexname, oldrelation->relname);
	OldIndex = index_open(OIDOldIndex);
	LockRelation(OldIndex, AccessExclusiveLock);

	/*
	 * Check that index is in fact an index on the given relation
	 */
	if (OldIndex->rd_index->indrelid != OIDOldHeap)
		elog(ERROR, "CLUSTER: \"%s\" is not an index for table \"%s\"",
			 oldindexname, oldrelation->relname);

	/* Drop relcache refcnts, but do NOT give up the locks */
	heap_close(OldHeap, NoLock);
	index_close(OldIndex);

	/* Save the information of all indexes on the relation. */
	indexes = get_indexattr_list(OIDOldHeap);

	/*
	 * Create the new heap with a temporary name.
	 */
	snprintf(NewHeapName, NAMEDATALEN, "temp_%u", OIDOldHeap);

	OIDNewHeap = copy_heap(OIDOldHeap, NewHeapName);

	/* We do not need CommandCounterIncrement() because copy_heap did it. */

	/*
	 * Copy the heap data into the new table in the desired order.
	 */
	rebuildheap(OIDNewHeap, OIDOldHeap, OIDOldIndex);

	/* To make the new heap's data visible. */
	CommandCounterIncrement();

	/* Swap the relfilenodes of the old and new heaps. */
	swap_relfilenodes(OIDNewHeap, OIDOldHeap);

	CommandCounterIncrement();

	/* Destroy new heap with old filenode */
	object.classId = RelOid_pg_class;
	object.objectId = OIDNewHeap;
	object.objectSubId = 0;

	/* The relation is local to our transaction and we know nothin
	 * depends on it, so DROP_RESTRICT should be OK.
	 */
	performDeletion(&object, DROP_RESTRICT);

	/* performDeletion does CommandCounterIncrement at end */

 	/* Recreate the indexes on the relation.  We do not need
  	 * CommandCounterIncrement() because recreate_indexattr does it.
   	 */
  	recreate_indexattr(OIDOldHeap, indexes);
}

static Oid
copy_heap(Oid OIDOldHeap, const char *NewName)
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
										  OldHeap->rd_rel->relisshared,
										  OldHeap->rd_rel->relhasoids,
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
rebuildheap(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex)
{
	Relation	LocalNewHeap,
				LocalOldHeap,
				LocalOldIndex;
	IndexScanDesc ScanDesc;
	HeapTuple	LocalHeapTuple;

	/*
	 * Open the relations I need. Scan through the OldHeap on the OldIndex
	 * and insert each tuple into the NewHeap.
	 */
	LocalNewHeap = heap_open(OIDNewHeap, AccessExclusiveLock);
	LocalOldHeap = heap_open(OIDOldHeap, AccessExclusiveLock);
	LocalOldIndex = index_open(OIDOldIndex);

	ScanDesc = index_beginscan(LocalOldHeap, LocalOldIndex,
							   SnapshotNow, 0, (ScanKey) NULL);

	while ((LocalHeapTuple = index_getnext(ScanDesc, ForwardScanDirection)) != NULL)
	{
		/*
		 * We must copy the tuple because heap_insert() will overwrite
		 * the commit-status fields of the tuple it's handed, and the
		 * retrieved tuple will actually be in a disk buffer!  Thus,
		 * the source relation would get trashed, which is bad news if
		 * we abort later on.  (This was a bug in releases thru 7.0)
		 */
		HeapTuple	copiedTuple = heap_copytuple(LocalHeapTuple);

		simple_heap_insert(LocalNewHeap, copiedTuple);
		heap_freetuple(copiedTuple);

		CHECK_FOR_INTERRUPTS();
	}

	index_endscan(ScanDesc);

	index_close(LocalOldIndex);
	heap_close(LocalOldHeap, NoLock);
	heap_close(LocalNewHeap, NoLock);
}

/* Get the necessary info about the indexes in the relation and
 * return a List of IndexAttrs.
 */
List *
get_indexattr_list (Oid OIDOldHeap)
{
	ScanKeyData	entry;
	HeapScanDesc scan;
	Relation indexRelation;
	HeapTuple indexTuple;
	List *indexes = NIL;
	IndexAttrs *attrs;
	HeapTuple tuple;
	Form_pg_index index;
	
	/* Grab the index tuples by looking into RelationRelationName
	 * by the OID of the old heap.
	 */
	indexRelation = heap_openr(IndexRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry, 0, Anum_pg_index_indrelid,
			F_OIDEQ, ObjectIdGetDatum(OIDOldHeap));
	scan = heap_beginscan(indexRelation, SnapshotNow, 1, &entry);
	while ((indexTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		index = (Form_pg_index) GETSTRUCT(indexTuple);

		attrs = (IndexAttrs *) palloc(sizeof(IndexAttrs));
		attrs->indexInfo = BuildIndexInfo(index);
		attrs->isPrimary = index->indisprimary;
		attrs->indexOID = index->indexrelid;

		/* The opclasses are copied verbatim from the original indexes.
		*/
		attrs->classOID = (Oid *)palloc(sizeof(Oid) *
				attrs->indexInfo->ii_NumIndexAttrs);
		memcpy(attrs->classOID, index->indclass,
				sizeof(Oid) * attrs->indexInfo->ii_NumIndexAttrs);

		/* Name and access method of each index come from
		 * RelationRelationName.
		 */
		tuple = SearchSysCache(RELOID,
				ObjectIdGetDatum(attrs->indexOID),
				0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "CLUSTER: cannot find index %u", attrs->indexOID);
		attrs->indexName = pstrdup(NameStr(((Form_pg_class) GETSTRUCT(tuple))->relname));
		attrs->accessMethodOID = ((Form_pg_class) GETSTRUCT(tuple))->relam;
		ReleaseSysCache(tuple);

		/* Cons the gathered data into the list.  We do not care about
		 * ordering, and this is more efficient than append.
		 */
		indexes=lcons((void *)attrs, indexes);
	}
	heap_endscan(scan);
	heap_close(indexRelation, AccessShareLock);
	return indexes;
}

/* Create new indexes and swap the filenodes with old indexes.  Then drop
 * the new index (carrying the old heap along).
 */
void
recreate_indexattr(Oid OIDOldHeap, List *indexes)
{
	IndexAttrs *attrs;
	List 	   *elem;
	Oid			newIndexOID;
	char		newIndexName[NAMEDATALEN];
	ObjectAddress object;

	foreach (elem, indexes)
	{
		attrs=(IndexAttrs *) lfirst(elem);

		/* Create the new index under a temporary name */
		snprintf(newIndexName, NAMEDATALEN, "temp_%u", attrs->indexOID);

		/* The new index will have constraint status set to false,
		 * but since we will only use its filenode it doesn't matter:
		 * after the filenode swap the index will keep the constraint
		 * status of the old index.
		 */
		newIndexOID = index_create(OIDOldHeap, newIndexName,
								   attrs->indexInfo, attrs->accessMethodOID,
								   attrs->classOID, attrs->isPrimary,
								   false, allowSystemTableMods);
		CommandCounterIncrement();

		/* Swap the filenodes. */
		swap_relfilenodes(newIndexOID, attrs->indexOID);
		setRelhasindex(OIDOldHeap, true, attrs->isPrimary, InvalidOid);

		/* Destroy new index with old filenode */
		object.classId = RelOid_pg_class;
		object.objectId = newIndexOID;
		object.objectSubId = 0;
		
		/* The relation is local to our transaction and we know
		 * nothing depends on it, so DROP_RESTRICT should be OK.
		 */
		performDeletion(&object, DROP_RESTRICT);
		
		/* performDeletion does CommandCounterIncrement() at its end */
		
		pfree(attrs->classOID);
		pfree(attrs);
	}
	freeList(indexes);
}

/* Swap the relfilenodes for two given relations.
 */
void
swap_relfilenodes(Oid r1, Oid r2)
{
	/* I can probably keep RelationRelationName open in the main
	 * function and pass the Relation around so I don't have to open
	 * it every time.
	 */
	Relation	relRelation,
				rel;
	HeapTuple	reltup[2];
	Oid			tempRFNode;
	int			i;
	CatalogIndexState indstate;

	/* We need both RelationRelationName tuples.  */
	relRelation = heap_openr(RelationRelationName, RowExclusiveLock);

	reltup[0] = SearchSysCacheCopy(RELOID,
								   ObjectIdGetDatum(r1),
								   0, 0, 0);
	if (!HeapTupleIsValid(reltup[0]))
		elog(ERROR, "CLUSTER: Cannot find tuple for relation %u", r1);
	reltup[1] = SearchSysCacheCopy(RELOID,
								   ObjectIdGetDatum(r2),
								   0, 0, 0);
	if (!HeapTupleIsValid(reltup[1]))
		elog(ERROR, "CLUSTER: Cannot find tuple for relation %u", r2);

	/* The buffer manager gets confused if we swap relfilenodes for
	 * relations that are not both local or non-local to this transaction.
	 * Flush the buffers on both relations so the buffer manager can
	 * forget about'em.
	 */

	rel = RelationIdGetRelation(r1);
	i = FlushRelationBuffers(rel, 0);
	if (i < 0)
		elog(ERROR, "CLUSTER: FlushRelationBuffers returned %d", i);
	RelationClose(rel);
	rel = RelationIdGetRelation(r1);
	i = FlushRelationBuffers(rel, 0);
	if (i < 0)
		elog(ERROR, "CLUSTER: FlushRelationBuffers returned %d", i);
	RelationClose(rel);

	/* Actually swap the filenodes */

	tempRFNode = ((Form_pg_class) GETSTRUCT(reltup[0]))->relfilenode;
	((Form_pg_class) GETSTRUCT(reltup[0]))->relfilenode =
		((Form_pg_class) GETSTRUCT(reltup[1]))->relfilenode;
	((Form_pg_class) GETSTRUCT(reltup[1]))->relfilenode = tempRFNode;

	/* Update the RelationRelationName tuples */
	simple_heap_update(relRelation, &reltup[1]->t_self, reltup[1]);
	simple_heap_update(relRelation, &reltup[0]->t_self, reltup[0]);
	
	/* To keep system catalogs current. */
	indstate = CatalogOpenIndexes(relRelation);
	CatalogIndexInsert(indstate, reltup[1]);
	CatalogIndexInsert(indstate, reltup[0]);
	CatalogCloseIndexes(indstate);

	heap_close(relRelation, NoLock);
	heap_freetuple(reltup[0]);
	heap_freetuple(reltup[1]);
}
