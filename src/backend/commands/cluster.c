/*-------------------------------------------------------------------------
 *
 * cluster.c
 *	  CLUSTER a table on an index.
 *
 * There is hardly anything left of Paul Brown's original implementation...
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/cluster.c,v 1.91.2.1 2003/03/03 04:37:48 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catalog.h"
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
 * we can rebuild the indexes on the new heap.
 */
typedef struct
{
	Oid			indexOID;
	char	   *indexName;
	IndexInfo  *indexInfo;
	Oid			accessMethodOID;
	Oid		   *classOID;
	bool		isclustered;
} IndexAttrs;

static Oid	make_new_heap(Oid OIDOldHeap, const char *NewName);
static void copy_heap_data(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex);
static List *get_indexattr_list(Relation OldHeap, Oid OldIndex);
static void recreate_indexattr(Oid OIDOldHeap, List *indexes);
static void swap_relfilenodes(Oid r1, Oid r2);


/*
 * cluster
 *
 * This clusters the table by creating a new, clustered table and
 * swapping the relfilenodes of the new table and the old table, so
 * the OID of the original table is preserved.	Thus we do not lose
 * GRANT, inheritance nor references to this table (this was a bug
 * in releases thru 7.3).
 *
 * Also create new indexes and swap the filenodes with the old indexes the
 * same way we do for the relation.  Since we are effectively bulk-loading
 * the new table, it's better to create the indexes afterwards than to fill
 * them incrementally while we load the table.
 *
 * Permissions checks were done already.
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
			 oldindexname, RelationGetRelationName(OldHeap));
	OldIndex = index_open(OIDOldIndex);
	LockRelation(OldIndex, AccessExclusiveLock);

	/*
	 * Check that index is in fact an index on the given relation
	 */
	if (OldIndex->rd_index == NULL ||
		OldIndex->rd_index->indrelid != OIDOldHeap)
		elog(ERROR, "CLUSTER: \"%s\" is not an index for table \"%s\"",
			 RelationGetRelationName(OldIndex),
			 RelationGetRelationName(OldHeap));

	/*
	 * Disallow clustering on incomplete indexes (those that might not index
	 * every row of the relation).  We could relax this by making a separate
	 * seqscan pass over the table to copy the missing rows, but that seems
	 * expensive and tedious.
	 */
	if (VARSIZE(&OldIndex->rd_index->indpred) > VARHDRSZ) /* partial? */
		elog(ERROR, "CLUSTER: cannot cluster on partial index");
	if (!OldIndex->rd_am->amindexnulls)
	{
		AttrNumber	colno;

		/*
		 * If the AM doesn't index nulls, then it's a partial index unless
		 * we can prove all the rows are non-null.  Note we only need look
		 * at the first column; multicolumn-capable AMs are *required* to
		 * index nulls in columns after the first.
		 */
		if (OidIsValid(OldIndex->rd_index->indproc))
			elog(ERROR, "CLUSTER: cannot cluster on functional index when index access method does not handle nulls");
		colno = OldIndex->rd_index->indkey[0];
		if (colno > 0)			/* system columns are non-null */
			if (!OldHeap->rd_att->attrs[colno - 1]->attnotnull)
				elog(ERROR, "CLUSTER: cannot cluster when index access method does not handle nulls"
					 "\n\tYou may be able to work around this by marking column \"%s\" NOT NULL",
					 NameStr(OldHeap->rd_att->attrs[colno - 1]->attname));
	}

	/*
	 * Disallow clustering system relations.  This will definitely NOT
	 * work for shared relations (we have no way to update pg_class rows
	 * in other databases), nor for nailed-in-cache relations (the
	 * relfilenode values for those are hardwired, see relcache.c).  It
	 * might work for other system relations, but I ain't gonna risk it.
	 */
	if (IsSystemRelation(OldHeap))
		elog(ERROR, "CLUSTER: cannot cluster system relation \"%s\"",
			 RelationGetRelationName(OldHeap));

	/* Save the information of all indexes on the relation. */
	indexes = get_indexattr_list(OldHeap, OIDOldIndex);

	/* Drop relcache refcnts, but do NOT give up the locks */
	index_close(OldIndex);
	heap_close(OldHeap, NoLock);

	/*
	 * Create the new heap, using a temporary name in the same namespace
	 * as the existing table.  NOTE: there is some risk of collision with
	 * user relnames.  Working around this seems more trouble than it's
	 * worth; in particular, we can't create the new heap in a different
	 * namespace from the old, or we will have problems with the TEMP
	 * status of temp tables.
	 */
	snprintf(NewHeapName, NAMEDATALEN, "pg_temp_%u", OIDOldHeap);

	OIDNewHeap = make_new_heap(OIDOldHeap, NewHeapName);

	/*
	 * We don't need CommandCounterIncrement() because make_new_heap did
	 * it.
	 */

	/*
	 * Copy the heap data into the new table in the desired order.
	 */
	copy_heap_data(OIDNewHeap, OIDOldHeap, OIDOldIndex);

	/* To make the new heap's data visible (probably not needed?). */
	CommandCounterIncrement();

	/* Swap the relfilenodes of the old and new heaps. */
	swap_relfilenodes(OIDOldHeap, OIDNewHeap);

	CommandCounterIncrement();

	/* Destroy new heap with old filenode */
	object.classId = RelOid_pg_class;
	object.objectId = OIDNewHeap;
	object.objectSubId = 0;

	/*
	 * The new relation is local to our transaction and we know nothing
	 * depends on it, so DROP_RESTRICT should be OK.
	 */
	performDeletion(&object, DROP_RESTRICT);

	/* performDeletion does CommandCounterIncrement at end */

	/*
	 * Recreate each index on the relation.  We do not need
	 * CommandCounterIncrement() because recreate_indexattr does it.
	 */
	recreate_indexattr(OIDOldHeap, indexes);
}

/*
 * Create the new table that we will fill with correctly-ordered data.
 */
static Oid
make_new_heap(Oid OIDOldHeap, const char *NewName)
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

/*
 * Do the physical copying of heap data.
 */
static void
copy_heap_data(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex)
{
	Relation	NewHeap,
				OldHeap,
				OldIndex;
	IndexScanDesc scan;
	HeapTuple	tuple;

	/*
	 * Open the relations I need. Scan through the OldHeap on the OldIndex
	 * and insert each tuple into the NewHeap.
	 */
	NewHeap = heap_open(OIDNewHeap, AccessExclusiveLock);
	OldHeap = heap_open(OIDOldHeap, AccessExclusiveLock);
	OldIndex = index_open(OIDOldIndex);

	scan = index_beginscan(OldHeap, OldIndex, SnapshotNow, 0, (ScanKey) NULL);

	while ((tuple = index_getnext(scan, ForwardScanDirection)) != NULL)
	{
		/*
		 * We must copy the tuple because heap_insert() will overwrite the
		 * commit-status fields of the tuple it's handed, and the
		 * retrieved tuple will actually be in a disk buffer!  Thus, the
		 * source relation would get trashed, which is bad news if we
		 * abort later on.	(This was a bug in releases thru 7.0)
		 *
		 * Note that the copied tuple will have the original OID, if any, so
		 * this does preserve OIDs.
		 */
		HeapTuple	copiedTuple = heap_copytuple(tuple);

		simple_heap_insert(NewHeap, copiedTuple);

		heap_freetuple(copiedTuple);

		CHECK_FOR_INTERRUPTS();
	}

	index_endscan(scan);

	index_close(OldIndex);
	heap_close(OldHeap, NoLock);
	heap_close(NewHeap, NoLock);
}

/*
 * Get the necessary info about the indexes of the relation and
 * return a list of IndexAttrs structures.
 */
static List *
get_indexattr_list(Relation OldHeap, Oid OldIndex)
{
	List	   *indexes = NIL;
	List	   *indlist;

	/* Ask the relcache to produce a list of the indexes of the old rel */
	foreach(indlist, RelationGetIndexList(OldHeap))
	{
		Oid			indexOID = (Oid) lfirsti(indlist);
		HeapTuple	indexTuple;
		HeapTuple	classTuple;
		Form_pg_index indexForm;
		Form_pg_class classForm;
		IndexAttrs *attrs;

		indexTuple = SearchSysCache(INDEXRELID,
									ObjectIdGetDatum(indexOID),
									0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "Cache lookup failed for index %u", indexOID);
		indexForm = (Form_pg_index) GETSTRUCT(indexTuple);
		Assert(indexForm->indexrelid == indexOID);

		attrs = (IndexAttrs *) palloc(sizeof(IndexAttrs));
		attrs->indexOID = indexOID;
		attrs->indexInfo = BuildIndexInfo(indexForm);
		attrs->classOID = (Oid *)
			palloc(sizeof(Oid) * attrs->indexInfo->ii_NumIndexAttrs);
		memcpy(attrs->classOID, indexForm->indclass,
			   sizeof(Oid) * attrs->indexInfo->ii_NumIndexAttrs);
		attrs->isclustered = (OldIndex == indexOID);

		/* Name and access method of each index come from pg_class */
		classTuple = SearchSysCache(RELOID,
									ObjectIdGetDatum(indexOID),
									0, 0, 0);
		if (!HeapTupleIsValid(classTuple))
			elog(ERROR, "Cache lookup failed for index %u", indexOID);
		classForm = (Form_pg_class) GETSTRUCT(classTuple);

		attrs->indexName = pstrdup(NameStr(classForm->relname));
		attrs->accessMethodOID = classForm->relam;

		ReleaseSysCache(classTuple);
		ReleaseSysCache(indexTuple);

		/*
		 * Cons the gathered data into the list.  We do not care about
		 * ordering, and this is more efficient than append.
		 */
		indexes = lcons(attrs, indexes);
	}

	return indexes;
}

/*
 * Create new indexes and swap the filenodes with old indexes.	Then drop
 * the new index (carrying the old index filenode along).
 */
static void
recreate_indexattr(Oid OIDOldHeap, List *indexes)
{
	List	   *elem;

	foreach(elem, indexes)
	{
		IndexAttrs *attrs = (IndexAttrs *) lfirst(elem);
		Oid			newIndexOID;
		char		newIndexName[NAMEDATALEN];
		ObjectAddress object;
		Form_pg_index index;
		HeapTuple	tuple;
		Relation	pg_index;

		/* Create the new index under a temporary name */
		snprintf(newIndexName, NAMEDATALEN, "pg_temp_%u", attrs->indexOID);

		/*
		 * The new index will have primary and constraint status set to
		 * false, but since we will only use its filenode it doesn't
		 * matter: after the filenode swap the index will keep the
		 * constraint status of the old index.
		 */
		newIndexOID = index_create(OIDOldHeap, newIndexName,
								attrs->indexInfo, attrs->accessMethodOID,
								   attrs->classOID, false,
								   false, allowSystemTableMods);
		CommandCounterIncrement();

		/* Swap the filenodes. */
		swap_relfilenodes(attrs->indexOID, newIndexOID);

		CommandCounterIncrement();

		/*
		 * Make sure that indisclustered is correct: it should be set only
		 * for the index we just clustered on.
		 */
		pg_index = heap_openr(IndexRelationName, RowExclusiveLock);
		tuple = SearchSysCacheCopy(INDEXRELID,
								   ObjectIdGetDatum(attrs->indexOID),
								   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for index %u", attrs->indexOID);
		index = (Form_pg_index) GETSTRUCT(tuple);
		if (index->indisclustered != attrs->isclustered)
		{
			index->indisclustered = attrs->isclustered;
			simple_heap_update(pg_index, &tuple->t_self, tuple);
			CatalogUpdateIndexes(pg_index, tuple);
		}
		heap_freetuple(tuple);
		heap_close(pg_index, RowExclusiveLock);

		/* Destroy new index with old filenode */
		object.classId = RelOid_pg_class;
		object.objectId = newIndexOID;
		object.objectSubId = 0;

		/*
		 * The relation is local to our transaction and we know nothing
		 * depends on it, so DROP_RESTRICT should be OK.
		 */
		performDeletion(&object, DROP_RESTRICT);

		/* performDeletion does CommandCounterIncrement() at its end */
	}
}

/*
 * Swap the relfilenodes for two given relations.
 *
 * Also swap any TOAST links, so that the toast data moves along with
 * the main-table data.
 */
static void
swap_relfilenodes(Oid r1, Oid r2)
{
	Relation	relRelation,
				rel;
	HeapTuple	reltup1,
				reltup2;
	Form_pg_class relform1,
				relform2;
	Oid			swaptemp;
	int			i;
	CatalogIndexState indstate;

	/* We need writable copies of both pg_class tuples. */
	relRelation = heap_openr(RelationRelationName, RowExclusiveLock);

	reltup1 = SearchSysCacheCopy(RELOID,
								 ObjectIdGetDatum(r1),
								 0, 0, 0);
	if (!HeapTupleIsValid(reltup1))
		elog(ERROR, "CLUSTER: Cannot find tuple for relation %u", r1);
	relform1 = (Form_pg_class) GETSTRUCT(reltup1);

	reltup2 = SearchSysCacheCopy(RELOID,
								 ObjectIdGetDatum(r2),
								 0, 0, 0);
	if (!HeapTupleIsValid(reltup2))
		elog(ERROR, "CLUSTER: Cannot find tuple for relation %u", r2);
	relform2 = (Form_pg_class) GETSTRUCT(reltup2);

	/*
	 * The buffer manager gets confused if we swap relfilenodes for
	 * relations that are not both local or non-local to this transaction.
	 * Flush the buffers on both relations so the buffer manager can
	 * forget about'em.  (XXX this might not be necessary anymore?)
	 */
	rel = relation_open(r1, NoLock);
	i = FlushRelationBuffers(rel, 0);
	if (i < 0)
		elog(ERROR, "CLUSTER: FlushRelationBuffers returned %d", i);
	relation_close(rel, NoLock);

	rel = relation_open(r2, NoLock);
	i = FlushRelationBuffers(rel, 0);
	if (i < 0)
		elog(ERROR, "CLUSTER: FlushRelationBuffers returned %d", i);
	relation_close(rel, NoLock);

	/*
	 * Actually swap the filenode and TOAST fields in the two tuples
	 */
	swaptemp = relform1->relfilenode;
	relform1->relfilenode = relform2->relfilenode;
	relform2->relfilenode = swaptemp;

	swaptemp = relform1->reltoastrelid;
	relform1->reltoastrelid = relform2->reltoastrelid;
	relform2->reltoastrelid = swaptemp;

	/* we should not swap reltoastidxid */

	/* swap size statistics too, since new rel has freshly-updated stats */
	{
		int4	swap_pages;
		float4	swap_tuples;

		swap_pages = relform1->relpages;
		relform1->relpages = relform2->relpages;
		relform2->relpages = swap_pages;

		swap_tuples = relform1->reltuples;
		relform1->reltuples = relform2->reltuples;
		relform2->reltuples = swap_tuples;
	}

	/* Update the tuples in pg_class */
	simple_heap_update(relRelation, &reltup1->t_self, reltup1);
	simple_heap_update(relRelation, &reltup2->t_self, reltup2);

	/* Keep system catalogs current */
	indstate = CatalogOpenIndexes(relRelation);
	CatalogIndexInsert(indstate, reltup1);
	CatalogIndexInsert(indstate, reltup2);
	CatalogCloseIndexes(indstate);

	/*
	 * If we have toast tables associated with the relations being
	 * swapped, change their dependency links to re-associate them with
	 * their new owning relations.	Otherwise the wrong one will get
	 * dropped ...
	 *
	 * NOTE: for now, we can assume the new table will have a TOAST table if
	 * and only if the old one does.  This logic might need work if we get
	 * smarter about dropped columns.
	 *
	 * NOTE: at present, a TOAST table's only dependency is the one on its
	 * owning table.  If more are ever created, we'd need to use something
	 * more selective than deleteDependencyRecordsFor() to get rid of only
	 * the link we want.
	 */
	if (relform1->reltoastrelid || relform2->reltoastrelid)
	{
		ObjectAddress baseobject,
					toastobject;
		long		count;

		if (!(relform1->reltoastrelid && relform2->reltoastrelid))
			elog(ERROR, "CLUSTER: expected both swapped tables to have TOAST tables");

		/* Delete old dependencies */
		count = deleteDependencyRecordsFor(RelOid_pg_class,
										   relform1->reltoastrelid);
		if (count != 1)
			elog(ERROR, "CLUSTER: expected one dependency record for TOAST table, found %ld",
				 count);
		count = deleteDependencyRecordsFor(RelOid_pg_class,
										   relform2->reltoastrelid);
		if (count != 1)
			elog(ERROR, "CLUSTER: expected one dependency record for TOAST table, found %ld",
				 count);

		/* Register new dependencies */
		baseobject.classId = RelOid_pg_class;
		baseobject.objectId = r1;
		baseobject.objectSubId = 0;
		toastobject.classId = RelOid_pg_class;
		toastobject.objectId = relform1->reltoastrelid;
		toastobject.objectSubId = 0;

		recordDependencyOn(&toastobject, &baseobject, DEPENDENCY_INTERNAL);

		baseobject.objectId = r2;
		toastobject.objectId = relform2->reltoastrelid;

		recordDependencyOn(&toastobject, &baseobject, DEPENDENCY_INTERNAL);
	}

	/*
	 * Blow away the old relcache entries now.	We need this kluge because
	 * relcache.c indexes relcache entries by rd_node as well as OID. It
	 * will get confused if it is asked to (re)build an entry with a new
	 * rd_node value when there is still another entry laying about with
	 * that same rd_node value.  (Fortunately, since one of the entries is
	 * local in our transaction, it's sufficient to clear out our own
	 * relcache this way; the problem cannot arise for other backends when
	 * they see our update on the non-local relation.)
	 */
	RelationForgetRelation(r1);
	RelationForgetRelation(r2);

	/* Clean up. */
	heap_freetuple(reltup1);
	heap_freetuple(reltup2);

	heap_close(relRelation, RowExclusiveLock);
}
