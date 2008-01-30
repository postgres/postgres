/*-------------------------------------------------------------------------
 *
 * cluster.c
 *	  CLUSTER a table on an index.
 *
 * There is hardly anything left of Paul Brown's original implementation...
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/cluster.c,v 1.169 2008/01/30 19:46:48 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/rewriteheap.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/toasting.h"
#include "commands/cluster.h"
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/relcache.h"
#include "utils/syscache.h"


/*
 * This struct is used to pass around the information on tables to be
 * clustered. We need this so we can make a list of them when invoked without
 * a specific table/index pair.
 */
typedef struct
{
	Oid			tableOid;
	Oid			indexOid;
} RelToCluster;


static void cluster_rel(RelToCluster *rv, bool recheck);
static void rebuild_relation(Relation OldHeap, Oid indexOid);
static TransactionId copy_heap_data(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex);
static List *get_tables_to_cluster(MemoryContext cluster_context);



/*---------------------------------------------------------------------------
 * This cluster code allows for clustering multiple tables at once. Because
 * of this, we cannot just run everything on a single transaction, or we
 * would be forced to acquire exclusive locks on all the tables being
 * clustered, simultaneously --- very likely leading to deadlock.
 *
 * To solve this we follow a similar strategy to VACUUM code,
 * clustering each relation in a separate transaction. For this to work,
 * we need to:
 *	- provide a separate memory context so that we can pass information in
 *	  a way that survives across transactions
 *	- start a new transaction every time a new relation is clustered
 *	- check for validity of the information on to-be-clustered relations,
 *	  as someone might have deleted a relation behind our back, or
 *	  clustered one on a different index
 *	- end the transaction
 *
 * The single-relation case does not have any such overhead.
 *
 * We also allow a relation to be specified without index.	In that case,
 * the indisclustered bit will be looked up, and an ERROR will be thrown
 * if there is no index with the bit set.
 *---------------------------------------------------------------------------
 */
void
cluster(ClusterStmt *stmt, bool isTopLevel)
{
	if (stmt->relation != NULL)
	{
		/* This is the single-relation case. */
		Oid			tableOid,
					indexOid = InvalidOid;
		Relation	rel;
		RelToCluster rvtc;

		/* Find and lock the table */
		rel = heap_openrv(stmt->relation, AccessExclusiveLock);

		tableOid = RelationGetRelid(rel);

		/* Check permissions */
		if (!pg_class_ownercheck(tableOid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
						   RelationGetRelationName(rel));

		/*
		 * Reject clustering a remote temp table ... their local buffer
		 * manager is not going to cope.
		 */
		if (isOtherTempNamespace(RelationGetNamespace(rel)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			   errmsg("cannot cluster temporary tables of other sessions")));

		if (stmt->indexname == NULL)
		{
			ListCell   *index;

			/* We need to find the index that has indisclustered set. */
			foreach(index, RelationGetIndexList(rel))
			{
				HeapTuple	idxtuple;
				Form_pg_index indexForm;

				indexOid = lfirst_oid(index);
				idxtuple = SearchSysCache(INDEXRELID,
										  ObjectIdGetDatum(indexOid),
										  0, 0, 0);
				if (!HeapTupleIsValid(idxtuple))
					elog(ERROR, "cache lookup failed for index %u", indexOid);
				indexForm = (Form_pg_index) GETSTRUCT(idxtuple);
				if (indexForm->indisclustered)
				{
					ReleaseSysCache(idxtuple);
					break;
				}
				ReleaseSysCache(idxtuple);
				indexOid = InvalidOid;
			}

			if (!OidIsValid(indexOid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("there is no previously clustered index for table \"%s\"",
								stmt->relation->relname)));
		}
		else
		{
			/*
			 * The index is expected to be in the same namespace as the
			 * relation.
			 */
			indexOid = get_relname_relid(stmt->indexname,
										 rel->rd_rel->relnamespace);
			if (!OidIsValid(indexOid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
					   errmsg("index \"%s\" for table \"%s\" does not exist",
							  stmt->indexname, stmt->relation->relname)));
		}

		/* All other checks are done in cluster_rel() */
		rvtc.tableOid = tableOid;
		rvtc.indexOid = indexOid;

		/* close relation, keep lock till commit */
		heap_close(rel, NoLock);

		/* Do the job */
		cluster_rel(&rvtc, false);
	}
	else
	{
		/*
		 * This is the "multi relation" case. We need to cluster all tables
		 * that have some index with indisclustered set.
		 */
		MemoryContext cluster_context;
		List	   *rvs;
		ListCell   *rv;

		/*
		 * We cannot run this form of CLUSTER inside a user transaction block;
		 * we'd be holding locks way too long.
		 */
		PreventTransactionChain(isTopLevel, "CLUSTER");

		/*
		 * Create special memory context for cross-transaction storage.
		 *
		 * Since it is a child of PortalContext, it will go away even in case
		 * of error.
		 */
		cluster_context = AllocSetContextCreate(PortalContext,
												"Cluster",
												ALLOCSET_DEFAULT_MINSIZE,
												ALLOCSET_DEFAULT_INITSIZE,
												ALLOCSET_DEFAULT_MAXSIZE);

		/*
		 * Build the list of relations to cluster.	Note that this lives in
		 * cluster_context.
		 */
		rvs = get_tables_to_cluster(cluster_context);

		/* Commit to get out of starting transaction */
		CommitTransactionCommand();

		/* Ok, now that we've got them all, cluster them one by one */
		foreach(rv, rvs)
		{
			RelToCluster *rvtc = (RelToCluster *) lfirst(rv);

			/* Start a new transaction for each relation. */
			StartTransactionCommand();
			/* functions in indexes may want a snapshot set */
			ActiveSnapshot = CopySnapshot(GetTransactionSnapshot());
			cluster_rel(rvtc, true);
			CommitTransactionCommand();
		}

		/* Start a new transaction for the cleanup work. */
		StartTransactionCommand();

		/* Clean up working storage */
		MemoryContextDelete(cluster_context);
	}
}

/*
 * cluster_rel
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
 */
static void
cluster_rel(RelToCluster *rvtc, bool recheck)
{
	Relation	OldHeap;

	/* Check for user-requested abort. */
	CHECK_FOR_INTERRUPTS();

	/*
	 * We grab exclusive access to the target rel and index for the duration
	 * of the transaction.	(This is redundant for the single-transaction
	 * case, since cluster() already did it.)  The index lock is taken inside
	 * check_index_is_clusterable.
	 */
	OldHeap = try_relation_open(rvtc->tableOid, AccessExclusiveLock);

	/* If the table has gone away, we can skip processing it */
	if (!OldHeap)
		return;

	/*
	 * Since we may open a new transaction for each relation, we have to check
	 * that the relation still is what we think it is.
	 *
	 * If this is a single-transaction CLUSTER, we can skip these tests. We
	 * *must* skip the one on indisclustered since it would reject an attempt
	 * to cluster a not-previously-clustered index.
	 */
	if (recheck)
	{
		HeapTuple	tuple;
		Form_pg_index indexForm;

		/* Check that the user still owns the relation */
		if (!pg_class_ownercheck(rvtc->tableOid, GetUserId()))
		{
			relation_close(OldHeap, AccessExclusiveLock);
			return;
		}

		/*
		 * Silently skip a temp table for a remote session.  Only doing this
		 * check in the "recheck" case is appropriate (which currently means
		 * somebody is executing a database-wide CLUSTER), because there is
		 * another check in cluster() which will stop any attempt to cluster
		 * remote temp tables by name.	There is another check in
		 * check_index_is_clusterable which is redundant, but we leave it for
		 * extra safety.
		 */
		if (isOtherTempNamespace(RelationGetNamespace(OldHeap)))
		{
			relation_close(OldHeap, AccessExclusiveLock);
			return;
		}

		/*
		 * Check that the index still exists
		 */
		if (!SearchSysCacheExists(RELOID,
								  ObjectIdGetDatum(rvtc->indexOid),
								  0, 0, 0))
		{
			relation_close(OldHeap, AccessExclusiveLock);
			return;
		}

		/*
		 * Check that the index is still the one with indisclustered set.
		 */
		tuple = SearchSysCache(INDEXRELID,
							   ObjectIdGetDatum(rvtc->indexOid),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))	/* probably can't happen */
		{
			relation_close(OldHeap, AccessExclusiveLock);
			return;
		}
		indexForm = (Form_pg_index) GETSTRUCT(tuple);
		if (!indexForm->indisclustered)
		{
			ReleaseSysCache(tuple);
			relation_close(OldHeap, AccessExclusiveLock);
			return;
		}
		ReleaseSysCache(tuple);
	}

	/* Check index is valid to cluster on */
	check_index_is_clusterable(OldHeap, rvtc->indexOid, recheck);

	/* rebuild_relation does all the dirty work */
	rebuild_relation(OldHeap, rvtc->indexOid);

	/* NB: rebuild_relation does heap_close() on OldHeap */
}

/*
 * Verify that the specified index is a legitimate index to cluster on
 *
 * Side effect: obtains exclusive lock on the index.  The caller should
 * already have exclusive lock on the table, so the index lock is likely
 * redundant, but it seems best to grab it anyway to ensure the index
 * definition can't change under us.
 */
void
check_index_is_clusterable(Relation OldHeap, Oid indexOid, bool recheck)
{
	Relation	OldIndex;

	OldIndex = index_open(indexOid, AccessExclusiveLock);

	/*
	 * Check that index is in fact an index on the given relation
	 */
	if (OldIndex->rd_index == NULL ||
		OldIndex->rd_index->indrelid != RelationGetRelid(OldHeap))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index for table \"%s\"",
						RelationGetRelationName(OldIndex),
						RelationGetRelationName(OldHeap))));

	/*
	 * Disallow clustering on incomplete indexes (those that might not index
	 * every row of the relation).	We could relax this by making a separate
	 * seqscan pass over the table to copy the missing rows, but that seems
	 * expensive and tedious.
	 */
	if (!heap_attisnull(OldIndex->rd_indextuple, Anum_pg_index_indpred))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on partial index \"%s\"",
						RelationGetRelationName(OldIndex))));

	if (!OldIndex->rd_am->amclusterable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on index \"%s\" because access method does not support clustering",
						RelationGetRelationName(OldIndex))));

	if (!OldIndex->rd_am->amindexnulls)
	{
		AttrNumber	colno;

		/*
		 * If the AM doesn't index nulls, then it's a partial index unless we
		 * can prove all the rows are non-null.  Note we only need look at the
		 * first column; multicolumn-capable AMs are *required* to index nulls
		 * in columns after the first.
		 */
		colno = OldIndex->rd_index->indkey.values[0];
		if (colno > 0)
		{
			/* ordinary user attribute */
			if (!OldHeap->rd_att->attrs[colno - 1]->attnotnull)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot cluster on index \"%s\" because access method does not handle null values",
								RelationGetRelationName(OldIndex)),
						 recheck
						 ? errhint("You might be able to work around this by marking column \"%s\" NOT NULL, or use ALTER TABLE ... SET WITHOUT CLUSTER to remove the cluster specification from the table.",
						 NameStr(OldHeap->rd_att->attrs[colno - 1]->attname))
						 : errhint("You might be able to work around this by marking column \"%s\" NOT NULL.",
					  NameStr(OldHeap->rd_att->attrs[colno - 1]->attname))));
		}
		else if (colno < 0)
		{
			/* system column --- okay, always non-null */
		}
		else
			/* index expression, lose... */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot cluster on expressional index \"%s\" because its index access method does not handle null values",
							RelationGetRelationName(OldIndex))));
	}

	/*
	 * Disallow if index is left over from a failed CREATE INDEX CONCURRENTLY;
	 * it might well not contain entries for every heap row, or might not even
	 * be internally consistent.  (But note that we don't check indcheckxmin;
	 * the worst consequence of following broken HOT chains would be that we
	 * might put recently-dead tuples out-of-order in the new table, and there
	 * is little harm in that.)
	 */
	if (!OldIndex->rd_index->indisvalid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on invalid index \"%s\"",
						RelationGetRelationName(OldIndex))));

	/*
	 * Disallow clustering system relations.  This will definitely NOT work
	 * for shared relations (we have no way to update pg_class rows in other
	 * databases), nor for nailed-in-cache relations (the relfilenode values
	 * for those are hardwired, see relcache.c).  It might work for other
	 * system relations, but I ain't gonna risk it.
	 */
	if (IsSystemRelation(OldHeap))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" is a system catalog",
						RelationGetRelationName(OldHeap))));

	/*
	 * Don't allow cluster on temp tables of other backends ... their local
	 * buffer manager is not going to cope.
	 */
	if (isOtherTempNamespace(RelationGetNamespace(OldHeap)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			   errmsg("cannot cluster temporary tables of other sessions")));

	/*
	 * Also check for active uses of the relation in the current transaction,
	 * including open scans and pending AFTER trigger events.
	 */
	CheckTableNotInUse(OldHeap, "CLUSTER");

	/* Drop relcache refcnt on OldIndex, but keep lock */
	index_close(OldIndex, NoLock);
}

/*
 * mark_index_clustered: mark the specified index as the one clustered on
 *
 * With indexOid == InvalidOid, will mark all indexes of rel not-clustered.
 */
void
mark_index_clustered(Relation rel, Oid indexOid)
{
	HeapTuple	indexTuple;
	Form_pg_index indexForm;
	Relation	pg_index;
	ListCell   *index;

	/*
	 * If the index is already marked clustered, no need to do anything.
	 */
	if (OidIsValid(indexOid))
	{
		indexTuple = SearchSysCache(INDEXRELID,
									ObjectIdGetDatum(indexOid),
									0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", indexOid);
		indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

		if (indexForm->indisclustered)
		{
			ReleaseSysCache(indexTuple);
			return;
		}

		ReleaseSysCache(indexTuple);
	}

	/*
	 * Check each index of the relation and set/clear the bit as needed.
	 */
	pg_index = heap_open(IndexRelationId, RowExclusiveLock);

	foreach(index, RelationGetIndexList(rel))
	{
		Oid			thisIndexOid = lfirst_oid(index);

		indexTuple = SearchSysCacheCopy(INDEXRELID,
										ObjectIdGetDatum(thisIndexOid),
										0, 0, 0);
		if (!HeapTupleIsValid(indexTuple))
			elog(ERROR, "cache lookup failed for index %u", thisIndexOid);
		indexForm = (Form_pg_index) GETSTRUCT(indexTuple);

		/*
		 * Unset the bit if set.  We know it's wrong because we checked this
		 * earlier.
		 */
		if (indexForm->indisclustered)
		{
			indexForm->indisclustered = false;
			simple_heap_update(pg_index, &indexTuple->t_self, indexTuple);
			CatalogUpdateIndexes(pg_index, indexTuple);
			/* Ensure we see the update in the index's relcache entry */
			CacheInvalidateRelcacheByRelid(thisIndexOid);
		}
		else if (thisIndexOid == indexOid)
		{
			indexForm->indisclustered = true;
			simple_heap_update(pg_index, &indexTuple->t_self, indexTuple);
			CatalogUpdateIndexes(pg_index, indexTuple);
			/* Ensure we see the update in the index's relcache entry */
			CacheInvalidateRelcacheByRelid(thisIndexOid);
		}
		heap_freetuple(indexTuple);
	}

	heap_close(pg_index, RowExclusiveLock);
}

/*
 * rebuild_relation: rebuild an existing relation in index order
 *
 * OldHeap: table to rebuild --- must be opened and exclusive-locked!
 * indexOid: index to cluster by
 *
 * NB: this routine closes OldHeap at the right time; caller should not.
 */
static void
rebuild_relation(Relation OldHeap, Oid indexOid)
{
	Oid			tableOid = RelationGetRelid(OldHeap);
	Oid			tableSpace = OldHeap->rd_rel->reltablespace;
	Oid			OIDNewHeap;
	char		NewHeapName[NAMEDATALEN];
	TransactionId frozenXid;
	ObjectAddress object;

	/* Mark the correct index as clustered */
	mark_index_clustered(OldHeap, indexOid);

	/* Close relcache entry, but keep lock until transaction commit */
	heap_close(OldHeap, NoLock);

	/*
	 * Create the new heap, using a temporary name in the same namespace as
	 * the existing table.	NOTE: there is some risk of collision with user
	 * relnames.  Working around this seems more trouble than it's worth; in
	 * particular, we can't create the new heap in a different namespace from
	 * the old, or we will have problems with the TEMP status of temp tables.
	 */
	snprintf(NewHeapName, sizeof(NewHeapName), "pg_temp_%u", tableOid);

	OIDNewHeap = make_new_heap(tableOid, NewHeapName, tableSpace);

	/*
	 * We don't need CommandCounterIncrement() because make_new_heap did it.
	 */

	/*
	 * Copy the heap data into the new table in the desired order.
	 */
	frozenXid = copy_heap_data(OIDNewHeap, tableOid, indexOid);

	/* To make the new heap's data visible (probably not needed?). */
	CommandCounterIncrement();

	/* Swap the physical files of the old and new heaps. */
	swap_relation_files(tableOid, OIDNewHeap, frozenXid);

	CommandCounterIncrement();

	/* Destroy new heap with old filenode */
	object.classId = RelationRelationId;
	object.objectId = OIDNewHeap;
	object.objectSubId = 0;

	/*
	 * The new relation is local to our transaction and we know nothing
	 * depends on it, so DROP_RESTRICT should be OK.
	 */
	performDeletion(&object, DROP_RESTRICT);

	/* performDeletion does CommandCounterIncrement at end */

	/*
	 * Rebuild each index on the relation (but not the toast table, which is
	 * all-new at this point).	We do not need CommandCounterIncrement()
	 * because reindex_relation does it.
	 */
	reindex_relation(tableOid, false);
}

/*
 * Create the new table that we will fill with correctly-ordered data.
 */
Oid
make_new_heap(Oid OIDOldHeap, const char *NewName, Oid NewTableSpace)
{
	TupleDesc	OldHeapDesc,
				tupdesc;
	Oid			OIDNewHeap;
	Relation	OldHeap;
	HeapTuple	tuple;
	Datum		reloptions;
	bool		isNull;

	OldHeap = heap_open(OIDOldHeap, AccessExclusiveLock);
	OldHeapDesc = RelationGetDescr(OldHeap);

	/*
	 * Need to make a copy of the tuple descriptor, since
	 * heap_create_with_catalog modifies it.
	 */
	tupdesc = CreateTupleDescCopyConstr(OldHeapDesc);

	/*
	 * Use options of the old heap for new heap.
	 */
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(OIDOldHeap),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", OIDOldHeap);
	reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
								 &isNull);
	if (isNull)
		reloptions = (Datum) 0;

	OIDNewHeap = heap_create_with_catalog(NewName,
										  RelationGetNamespace(OldHeap),
										  NewTableSpace,
										  InvalidOid,
										  OldHeap->rd_rel->relowner,
										  tupdesc,
										  OldHeap->rd_rel->relkind,
										  OldHeap->rd_rel->relisshared,
										  true,
										  0,
										  ONCOMMIT_NOOP,
										  reloptions,
										  allowSystemTableMods);

	ReleaseSysCache(tuple);

	/*
	 * Advance command counter so that the newly-created relation's catalog
	 * tuples will be visible to heap_open.
	 */
	CommandCounterIncrement();

	/*
	 * If necessary, create a TOAST table for the new relation. Note that
	 * AlterTableCreateToastTable ends with CommandCounterIncrement(), so that
	 * the TOAST table will be visible for insertion.
	 */
	AlterTableCreateToastTable(OIDNewHeap);

	heap_close(OldHeap, NoLock);

	return OIDNewHeap;
}

/*
 * Do the physical copying of heap data.  Returns the TransactionId used as
 * freeze cutoff point for the tuples.
 */
static TransactionId
copy_heap_data(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex)
{
	Relation	NewHeap,
				OldHeap,
				OldIndex;
	TupleDesc	oldTupDesc;
	TupleDesc	newTupDesc;
	int			natts;
	Datum	   *values;
	bool	   *isnull;
	IndexScanDesc scan;
	HeapTuple	tuple;
	bool		use_wal;
	TransactionId OldestXmin;
	TransactionId FreezeXid;
	RewriteState rwstate;

	/*
	 * Open the relations we need.
	 */
	NewHeap = heap_open(OIDNewHeap, AccessExclusiveLock);
	OldHeap = heap_open(OIDOldHeap, AccessExclusiveLock);
	OldIndex = index_open(OIDOldIndex, AccessExclusiveLock);

	/*
	 * Their tuple descriptors should be exactly alike, but here we only need
	 * assume that they have the same number of columns.
	 */
	oldTupDesc = RelationGetDescr(OldHeap);
	newTupDesc = RelationGetDescr(NewHeap);
	Assert(newTupDesc->natts == oldTupDesc->natts);

	/* Preallocate values/isnull arrays */
	natts = newTupDesc->natts;
	values = (Datum *) palloc(natts * sizeof(Datum));
	isnull = (bool *) palloc(natts * sizeof(bool));

	/*
	 * We need to log the copied data in WAL iff WAL archiving is enabled AND
	 * it's not a temp rel.
	 */
	use_wal = XLogArchivingActive() && !NewHeap->rd_istemp;

	/* use_wal off requires rd_targblock be initially invalid */
	Assert(NewHeap->rd_targblock == InvalidBlockNumber);

	/*
	 * compute xids used to freeze and weed out dead tuples.  We use -1
	 * freeze_min_age to avoid having CLUSTER freeze tuples earlier than a
	 * plain VACUUM would.
	 */
	vacuum_set_xid_limits(-1, OldHeap->rd_rel->relisshared,
						  &OldestXmin, &FreezeXid);

	/*
	 * FreezeXid will become the table's new relfrozenxid, and that mustn't
	 * go backwards, so take the max.
	 */
	if (TransactionIdPrecedes(FreezeXid, OldHeap->rd_rel->relfrozenxid))
		FreezeXid = OldHeap->rd_rel->relfrozenxid;

	/* Initialize the rewrite operation */
	rwstate = begin_heap_rewrite(NewHeap, OldestXmin, FreezeXid, use_wal);

	/*
	 * Scan through the OldHeap in OldIndex order and copy each tuple into the
	 * NewHeap.  To ensure we see recently-dead tuples that still need to be
	 * copied, we scan with SnapshotAny and use HeapTupleSatisfiesVacuum for
	 * the visibility test.
	 */
	scan = index_beginscan(OldHeap, OldIndex,
						   SnapshotAny, 0, (ScanKey) NULL);

	while ((tuple = index_getnext(scan, ForwardScanDirection)) != NULL)
	{
		HeapTuple	copiedTuple;
		bool		isdead;
		int			i;

		CHECK_FOR_INTERRUPTS();

		LockBuffer(scan->xs_cbuf, BUFFER_LOCK_SHARE);

		switch (HeapTupleSatisfiesVacuum(tuple->t_data, OldestXmin,
										 scan->xs_cbuf))
		{
			case HEAPTUPLE_DEAD:
				/* Definitely dead */
				isdead = true;
				break;
			case HEAPTUPLE_LIVE:
			case HEAPTUPLE_RECENTLY_DEAD:
				/* Live or recently dead, must copy it */
				isdead = false;
				break;
			case HEAPTUPLE_INSERT_IN_PROGRESS:

				/*
				 * We should not see this unless it's been inserted earlier in
				 * our own transaction.
				 */
				if (!TransactionIdIsCurrentTransactionId(
									  HeapTupleHeaderGetXmin(tuple->t_data)))
					elog(ERROR, "concurrent insert in progress");
				/* treat as live */
				isdead = false;
				break;
			case HEAPTUPLE_DELETE_IN_PROGRESS:

				/*
				 * We should not see this unless it's been deleted earlier in
				 * our own transaction.
				 */
				Assert(!(tuple->t_data->t_infomask & HEAP_XMAX_IS_MULTI));
				if (!TransactionIdIsCurrentTransactionId(
									  HeapTupleHeaderGetXmax(tuple->t_data)))
					elog(ERROR, "concurrent delete in progress");
				/* treat as recently dead */
				isdead = false;
				break;
			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				isdead = false; /* keep compiler quiet */
				break;
		}

		LockBuffer(scan->xs_cbuf, BUFFER_LOCK_UNLOCK);

		if (isdead)
		{
			/* heap rewrite module still needs to see it... */
			rewrite_heap_dead_tuple(rwstate, tuple);
			continue;
		}

		/*
		 * We cannot simply copy the tuple as-is, for several reasons:
		 *
		 * 1. We'd like to squeeze out the values of any dropped columns, both
		 * to save space and to ensure we have no corner-case failures. (It's
		 * possible for example that the new table hasn't got a TOAST table
		 * and so is unable to store any large values of dropped cols.)
		 *
		 * 2. The tuple might not even be legal for the new table; this is
		 * currently only known to happen as an after-effect of ALTER TABLE
		 * SET WITHOUT OIDS.
		 *
		 * So, we must reconstruct the tuple from component Datums.
		 */
		heap_deform_tuple(tuple, oldTupDesc, values, isnull);

		/* Be sure to null out any dropped columns */
		for (i = 0; i < natts; i++)
		{
			if (newTupDesc->attrs[i]->attisdropped)
				isnull[i] = true;
		}

		copiedTuple = heap_form_tuple(newTupDesc, values, isnull);

		/* Preserve OID, if any */
		if (NewHeap->rd_rel->relhasoids)
			HeapTupleSetOid(copiedTuple, HeapTupleGetOid(tuple));

		/* The heap rewrite module does the rest */
		rewrite_heap_tuple(rwstate, tuple, copiedTuple);

		heap_freetuple(copiedTuple);
	}

	index_endscan(scan);

	/* Write out any remaining tuples, and fsync if needed */
	end_heap_rewrite(rwstate);

	pfree(values);
	pfree(isnull);

	index_close(OldIndex, NoLock);
	heap_close(OldHeap, NoLock);
	heap_close(NewHeap, NoLock);

	return FreezeXid;
}

/*
 * Swap the physical files of two given relations.
 *
 * We swap the physical identity (reltablespace and relfilenode) while
 * keeping the same logical identities of the two relations.
 *
 * Also swap any TOAST links, so that the toast data moves along with
 * the main-table data.
 *
 * Additionally, the first relation is marked with relfrozenxid set to
 * frozenXid.  It seems a bit ugly to have this here, but all callers would
 * have to do it anyway, so having it here saves a heap_update.  Note: the
 * TOAST table needs no special handling, because since we swapped the links,
 * the entry for the TOAST table will now contain RecentXmin in relfrozenxid,
 * which is the correct value.
 */
void
swap_relation_files(Oid r1, Oid r2, TransactionId frozenXid)
{
	Relation	relRelation;
	HeapTuple	reltup1,
				reltup2;
	Form_pg_class relform1,
				relform2;
	Oid			swaptemp;
	CatalogIndexState indstate;

	/* We need writable copies of both pg_class tuples. */
	relRelation = heap_open(RelationRelationId, RowExclusiveLock);

	reltup1 = SearchSysCacheCopy(RELOID,
								 ObjectIdGetDatum(r1),
								 0, 0, 0);
	if (!HeapTupleIsValid(reltup1))
		elog(ERROR, "cache lookup failed for relation %u", r1);
	relform1 = (Form_pg_class) GETSTRUCT(reltup1);

	reltup2 = SearchSysCacheCopy(RELOID,
								 ObjectIdGetDatum(r2),
								 0, 0, 0);
	if (!HeapTupleIsValid(reltup2))
		elog(ERROR, "cache lookup failed for relation %u", r2);
	relform2 = (Form_pg_class) GETSTRUCT(reltup2);

	/*
	 * Actually swap the fields in the two tuples
	 */
	swaptemp = relform1->relfilenode;
	relform1->relfilenode = relform2->relfilenode;
	relform2->relfilenode = swaptemp;

	swaptemp = relform1->reltablespace;
	relform1->reltablespace = relform2->reltablespace;
	relform2->reltablespace = swaptemp;

	swaptemp = relform1->reltoastrelid;
	relform1->reltoastrelid = relform2->reltoastrelid;
	relform2->reltoastrelid = swaptemp;

	/* we should not swap reltoastidxid */

	/* set rel1's frozen Xid */
	Assert(TransactionIdIsNormal(frozenXid));
	relform1->relfrozenxid = frozenXid;

	/* swap size statistics too, since new rel has freshly-updated stats */
	{
		int4		swap_pages;
		float4		swap_tuples;

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
	 * If we have toast tables associated with the relations being swapped,
	 * change their dependency links to re-associate them with their new
	 * owning relations.  Otherwise the wrong one will get dropped ...
	 *
	 * NOTE: it is possible that only one table has a toast table; this can
	 * happen in CLUSTER if there were dropped columns in the old table, and
	 * in ALTER TABLE when adding or changing type of columns.
	 *
	 * NOTE: at present, a TOAST table's only dependency is the one on its
	 * owning table.  If more are ever created, we'd need to use something
	 * more selective than deleteDependencyRecordsFor() to get rid of only the
	 * link we want.
	 */
	if (relform1->reltoastrelid || relform2->reltoastrelid)
	{
		ObjectAddress baseobject,
					toastobject;
		long		count;

		/* Delete old dependencies */
		if (relform1->reltoastrelid)
		{
			count = deleteDependencyRecordsFor(RelationRelationId,
											   relform1->reltoastrelid);
			if (count != 1)
				elog(ERROR, "expected one dependency record for TOAST table, found %ld",
					 count);
		}
		if (relform2->reltoastrelid)
		{
			count = deleteDependencyRecordsFor(RelationRelationId,
											   relform2->reltoastrelid);
			if (count != 1)
				elog(ERROR, "expected one dependency record for TOAST table, found %ld",
					 count);
		}

		/* Register new dependencies */
		baseobject.classId = RelationRelationId;
		baseobject.objectSubId = 0;
		toastobject.classId = RelationRelationId;
		toastobject.objectSubId = 0;

		if (relform1->reltoastrelid)
		{
			baseobject.objectId = r1;
			toastobject.objectId = relform1->reltoastrelid;
			recordDependencyOn(&toastobject, &baseobject, DEPENDENCY_INTERNAL);
		}

		if (relform2->reltoastrelid)
		{
			baseobject.objectId = r2;
			toastobject.objectId = relform2->reltoastrelid;
			recordDependencyOn(&toastobject, &baseobject, DEPENDENCY_INTERNAL);
		}
	}

	/*
	 * Blow away the old relcache entries now.	We need this kluge because
	 * relcache.c keeps a link to the smgr relation for the physical file, and
	 * that will be out of date as soon as we do CommandCounterIncrement.
	 * Whichever of the rels is the second to be cleared during cache
	 * invalidation will have a dangling reference to an already-deleted smgr
	 * relation.  Rather than trying to avoid this by ordering operations just
	 * so, it's easiest to not have the relcache entries there at all.
	 * (Fortunately, since one of the entries is local in our transaction,
	 * it's sufficient to clear out our own relcache this way; the problem
	 * cannot arise for other backends when they see our update on the
	 * non-local relation.)
	 */
	RelationForgetRelation(r1);
	RelationForgetRelation(r2);

	/* Clean up. */
	heap_freetuple(reltup1);
	heap_freetuple(reltup2);

	heap_close(relRelation, RowExclusiveLock);
}

/*
 * Get a list of tables that the current user owns and
 * have indisclustered set.  Return the list in a List * of rvsToCluster
 * with the tableOid and the indexOid on which the table is already
 * clustered.
 */
static List *
get_tables_to_cluster(MemoryContext cluster_context)
{
	Relation	indRelation;
	HeapScanDesc scan;
	ScanKeyData entry;
	HeapTuple	indexTuple;
	Form_pg_index index;
	MemoryContext old_context;
	RelToCluster *rvtc;
	List	   *rvs = NIL;

	/*
	 * Get all indexes that have indisclustered set and are owned by
	 * appropriate user. System relations or nailed-in relations cannot ever
	 * have indisclustered set, because CLUSTER will refuse to set it when
	 * called with one of them as argument.
	 */
	indRelation = heap_open(IndexRelationId, AccessShareLock);
	ScanKeyInit(&entry,
				Anum_pg_index_indisclustered,
				BTEqualStrategyNumber, F_BOOLEQ,
				BoolGetDatum(true));
	scan = heap_beginscan(indRelation, SnapshotNow, 1, &entry);
	while ((indexTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		index = (Form_pg_index) GETSTRUCT(indexTuple);

		if (!pg_class_ownercheck(index->indrelid, GetUserId()))
			continue;

		/*
		 * We have to build the list in a different memory context so it will
		 * survive the cross-transaction processing
		 */
		old_context = MemoryContextSwitchTo(cluster_context);

		rvtc = (RelToCluster *) palloc(sizeof(RelToCluster));
		rvtc->tableOid = index->indrelid;
		rvtc->indexOid = index->indexrelid;
		rvs = lcons(rvtc, rvs);

		MemoryContextSwitchTo(old_context);
	}
	heap_endscan(scan);

	relation_close(indRelation, AccessShareLock);

	return rvs;
}
