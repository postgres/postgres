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
 *	  $Header: /cvsroot/pgsql/src/backend/commands/cluster.c,v 1.96 2002/11/23 04:05:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_constraint.h"
#include "commands/cluster.h"
#include "commands/tablecmds.h"
#include "miscadmin.h"
#include "utils/acl.h"
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

/* This struct is used to pass around the information on tables to be
 * clustered. We need this so we can make a list of them when invoked without
 * a specific table/index pair.
 */
typedef struct
{
	Oid		tableOid;
	Oid		indexOid;
	bool	isPrevious;
} relToCluster;

static Oid	make_new_heap(Oid OIDOldHeap, const char *NewName);
static void copy_heap_data(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex);
static void recreate_indexattr(Oid OIDOldHeap, List *indexes);
static void swap_relfilenodes(Oid r1, Oid r2);
static void cluster_rel(relToCluster *rv);
static bool check_cluster_ownership(Oid relOid);
static List *get_tables_to_cluster(Oid owner);

static MemoryContext cluster_context = NULL;

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
 * Since we may open a new transaction for each relation, we have to
 * check that the relation still is what we think it is.
 */
void
cluster_rel(relToCluster *rvtc)
{
	Relation	OldHeap,
				OldIndex;
	List	   *indexes;

	/* Check for user-requested abort. */
	CHECK_FOR_INTERRUPTS();

	/* Check if the relation and index still exist before opening them
	 */
	if (!SearchSysCacheExists(RELOID,
							  ObjectIdGetDatum(rvtc->tableOid),
							  0, 0, 0) ||
			!SearchSysCacheExists(RELOID,
								  ObjectIdGetDatum(rvtc->indexOid),
								  0, 0, 0))
		return;

	/* Check that the user still owns the relation */
	if (!check_cluster_ownership(rvtc->tableOid))
		return;

	/* Check that the index is still the one with indisclustered set.
	 * If this is a standalone cluster, skip this test.
	 */
	if (rvtc->isPrevious)
	{
		HeapTuple		tuple;
		Form_pg_index	indexForm;

		tuple = SearchSysCache(INDEXRELID,
							   ObjectIdGetDatum(rvtc->indexOid),
							   0, 0, 0);
		indexForm = (Form_pg_index) GETSTRUCT(tuple);
		if (!indexForm->indisclustered)
		{
			ReleaseSysCache(tuple);
			return;
		}
		ReleaseSysCache(tuple);
	}

	/*
	 * We grab exclusive access to the target rel and index for the
	 * duration of the transaction.
	 */
	OldHeap = heap_open(rvtc->tableOid, AccessExclusiveLock);

	OldIndex = index_open(rvtc->indexOid);
	LockRelation(OldIndex, AccessExclusiveLock);

	/*
	 * Check that index is in fact an index on the given relation
	 */
	if (OldIndex->rd_index == NULL ||
		OldIndex->rd_index->indrelid != rvtc->tableOid)
		elog(ERROR, "CLUSTER: \"%s\" is not an index for table \"%s\"",
			 RelationGetRelationName(OldIndex),
			 RelationGetRelationName(OldHeap));

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
	indexes = get_indexattr_list(OldHeap, rvtc->indexOid);

	/* Drop relcache refcnts, but do NOT give up the locks */
	index_close(OldIndex);
	heap_close(OldHeap, NoLock);

	/* rebuild_rel does all the dirty work */
	rebuild_rel(rvtc->tableOid, rvtc->indexOid, indexes, true);
}

void
rebuild_rel(Oid tableOid, Oid indexOid, List *indexes, bool dataCopy)
{
	Oid			OIDNewHeap;
	char		NewHeapName[NAMEDATALEN];
	ObjectAddress object;

	/*
	 * If dataCopy is true, we assume that we will be basing the
	 * copy off an index for cluster operations.
	 */
	Assert(!dataCopy || indexOid != NULL);
	/*
	 * Create the new heap, using a temporary name in the same namespace
	 * as the existing table.  NOTE: there is some risk of collision with
	 * user relnames.  Working around this seems more trouble than it's
	 * worth; in particular, we can't create the new heap in a different
	 * namespace from the old, or we will have problems with the TEMP
	 * status of temp tables.
	 */
	snprintf(NewHeapName, NAMEDATALEN, "pg_temp_%u", tableOid);

	OIDNewHeap = make_new_heap(tableOid, NewHeapName);
	/*
	 * We don't need CommandCounterIncrement() because make_new_heap did
	 * it.
	 */

	/*
	 * Copy the heap data into the new table in the desired order.
	 */
	if (dataCopy)
		copy_heap_data(OIDNewHeap, tableOid, indexOid);

	/* To make the new heap's data visible (probably not needed?). */
	CommandCounterIncrement();

	/* Swap the relfilenodes of the old and new heaps. */
	swap_relfilenodes(tableOid, OIDNewHeap);

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
	recreate_indexattr(tableOid, indexes);
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
										  ONCOMMIT_NOOP,
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
List *
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

/*---------------------------------------------------------------------------
 * This cluster code allows for clustering multiple tables at once.	Because
 * of this, we cannot just run everything on a single transaction, or we
 * would be forced to acquire exclusive locks on all the tables being
 * clustered.	To solve this we follow a similar strategy to VACUUM code,
 * clustering each relation in a separate transaction. For this to work,
 * we need to:
 *  - provide a separate memory context so that we can pass information in
 *    a way that trascends transactions
 *  - start a new transaction every time a new relation is clustered
 *  - check for validity of the information on to-be-clustered relations,
 *    as someone might have deleted a relation behind our back, or
 *    clustered one on a different index
 *  - end the transaction
 *
 * The single relation code does not have any overhead.
 *
 * We also allow a relation being specified without index.  In that case,
 * the indisclustered bit will be looked up, and an ERROR will be thrown
 * if there is no index with the bit set.
 *---------------------------------------------------------------------------
 */
void
cluster(ClusterStmt *stmt)
{

	/* This is the single relation case. */
	if (stmt->relation != NULL)
	{
		Oid				indexOid = InvalidOid,
						tableOid;
		relToCluster	rvtc;
		HeapTuple		tuple;
		Form_pg_class	classForm;

		tableOid = RangeVarGetRelid(stmt->relation, false);
		if (!check_cluster_ownership(tableOid))
			elog(ERROR, "CLUSTER: You do not own relation %s",
					stmt->relation->relname);

		tuple = SearchSysCache(RELOID,
							   ObjectIdGetDatum(tableOid),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "Cache lookup failed for relation %u", tableOid);
		classForm = (Form_pg_class) GETSTRUCT(tuple);

		if (stmt->indexname == NULL)
		{
			List	   *index;
			Relation	rel = RelationIdGetRelation(tableOid);
			HeapTuple	ituple = NULL,
						idxtuple = NULL;

			/* We need to fetch the index that has indisclustered set. */
			foreach (index, RelationGetIndexList(rel))
			{
				Form_pg_index	indexForm;

				indexOid = lfirsti(index);
				ituple = SearchSysCache(RELOID,
									   ObjectIdGetDatum(indexOid),
									   0, 0, 0);
				if (!HeapTupleIsValid(ituple))
					elog(ERROR, "Cache lookup failed for relation %u", indexOid);
				idxtuple = SearchSysCache(INDEXRELID,
										  ObjectIdGetDatum(HeapTupleGetOid(ituple)),
										  0, 0, 0);
				if (!HeapTupleIsValid(idxtuple))
					elog(ERROR, "Cache lookup failed for index %u", HeapTupleGetOid(ituple));
				indexForm = (Form_pg_index) GETSTRUCT(idxtuple);
				if (indexForm->indisclustered)
					break;
				indexOid = InvalidOid;
			}
			if (indexOid == InvalidOid)
				elog(ERROR, "CLUSTER: No previously clustered index found on table %s",
						stmt->relation->relname);
			RelationClose(rel);
			ReleaseSysCache(ituple);
			ReleaseSysCache(idxtuple);
		}
		else
		{
			/* The index is expected to be in the same namespace as the relation. */
			indexOid = get_relname_relid(stmt->indexname, classForm->relnamespace);
		}
		ReleaseSysCache(tuple);

		/* XXX Maybe the namespace should be reported as well */
		if (!OidIsValid(indexOid))
			elog(ERROR, "CLUSTER: cannot find index \"%s\" for table \"%s\"",
					stmt->indexname, stmt->relation->relname);
		rvtc.tableOid = tableOid;
		rvtc.indexOid = indexOid;
		rvtc.isPrevious = false;

		/* Do the job */
		cluster_rel(&rvtc);
	}
	else
	{
		/*
		 * This is the "no relation" case. We need to cluster all tables
		 * that have some index with indisclustered set.
		 */

		relToCluster	*rvtc;
		List			*rv,
						*rvs;

		/*
		 * We cannot run CLUSTER inside a user transaction block; if we were inside
		 * a transaction, then our commit- and start-transaction-command calls
		 * would not have the intended effect!
		 */
		if (IsTransactionBlock())
			elog(ERROR, "CLUSTER cannot run inside a BEGIN/END block");

		/* Running CLUSTER from a function would free the function context */
		if (!MemoryContextContains(QueryContext, stmt))
			elog(ERROR, "CLUSTER cannot be called from a function");
		/*
		 * Create special memory context for cross-transaction storage.
		 *
		 * Since it is a child of QueryContext, it will go away even in case
		 * of error.
		 */
		cluster_context = AllocSetContextCreate(QueryContext,
				"Cluster",
				ALLOCSET_DEFAULT_MINSIZE,
				ALLOCSET_DEFAULT_INITSIZE,
				ALLOCSET_DEFAULT_MAXSIZE);

		/*
		 * Build the list of relations to cluster.  Note that this lives in
		 * cluster_context.
		 */
		rvs = get_tables_to_cluster(GetUserId());

		/* Ok, now that we've got them all, cluster them one by one */
		foreach (rv, rvs)
		{
			rvtc = (relToCluster *)lfirst(rv);

			/* Start a new transaction for this relation. */
			StartTransactionCommand(true);
			cluster_rel(rvtc);
			CommitTransactionCommand(true);
		}
	}

	/* Start a new transaction for the cleanup work. */
	StartTransactionCommand(true);

	/* Clean up working storage */
	if (stmt->relation == NULL)
	{
		MemoryContextDelete(cluster_context);
		cluster_context = NULL;
	}
}

/* Checks if the user owns the relation. Superusers
 * are allowed to cluster any table.
 */
bool
check_cluster_ownership(Oid relOid)
{
	/* Superusers bypass this check */
	return pg_class_ownercheck(relOid, GetUserId());
}

/* Get a list of tables that the current user owns and
 * have indisclustered set.  Return the list in a List * of rvsToCluster
 * with the tableOid and the indexOid on which the table is already 
 * clustered.
 */
List *
get_tables_to_cluster(Oid owner)
{
	Relation		indRelation;
	HeapScanDesc	scan;
	ScanKeyData		entry;
	HeapTuple		indexTuple;
	Form_pg_index	index;
	relToCluster   *rvtc;
	List		   *rvs = NIL;

	/*
	 * Get all indexes that have indisclustered set.	System
	 * relations or nailed-in relations cannot ever have
	 * indisclustered set, because CLUSTER will refuse to
	 * set it when called with one of them as argument.
	 */
	indRelation = relation_openr(IndexRelationName, RowExclusiveLock);
	ScanKeyEntryInitialize(&entry, 0, Anum_pg_index_indisclustered,
						   F_BOOLEQ, true);
	scan = heap_beginscan(indRelation, SnapshotNow, 1, &entry);
	while ((indexTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		MemoryContext	old_context = NULL;

		index = (Form_pg_index) GETSTRUCT(indexTuple);
		if (!check_cluster_ownership(index->indrelid))
			continue;

		/*
		 * We have to build the struct in a different memory context so
		 * it will survive the cross-transaction processing
		 */

		old_context = MemoryContextSwitchTo(cluster_context);

		rvtc = (relToCluster *)palloc(sizeof(relToCluster));
		rvtc->indexOid = index->indexrelid;
		rvtc->tableOid = index->indrelid;
		rvtc->isPrevious = true;
		rvs = lcons((void *)rvtc, rvs);

		MemoryContextSwitchTo(old_context);
	}
	heap_endscan(scan);

	/*
	 * Release the lock on pg_index. We will check the indexes
	 * later again.
	 *
	 */
	relation_close(indRelation, RowExclusiveLock);
	return rvs;
}
