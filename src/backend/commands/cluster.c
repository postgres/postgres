/*-------------------------------------------------------------------------
 *
 * cluster.c
 *	  CLUSTER a table on an index.  This is now also used for VACUUM FULL.
 *
 * There is hardly anything left of Paul Brown's original implementation...
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/cluster.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/relscan.h"
#include "access/rewriteheap.h"
#include "access/transam.h"
#include "access/tuptoaster.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/toasting.h"
#include "commands/cluster.h"
#include "commands/tablecmds.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "optimizer/planner.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tqual.h"
#include "utils/tuplesort.h"


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


static void rebuild_relation(Relation OldHeap, Oid indexOid, bool verbose);
static void copy_heap_data(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex,
			   bool verbose, bool *pSwapToastByContent,
			   TransactionId *pFreezeXid, MultiXactId *pCutoffMulti);
static List *get_tables_to_cluster(MemoryContext cluster_context);
static void reform_and_rewrite_tuple(HeapTuple tuple,
						 TupleDesc oldTupDesc, TupleDesc newTupDesc,
						 Datum *values, bool *isnull,
						 bool newRelHasOids, RewriteState rwstate);


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
 * We also allow a relation to be specified without index.  In that case,
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

		/* Find, lock, and check permissions on the table */
		tableOid = RangeVarGetRelidExtended(stmt->relation,
											AccessExclusiveLock,
											false, false,
											RangeVarCallbackOwnsTable, NULL);
		rel = heap_open(tableOid, NoLock);

		/*
		 * Reject clustering a remote temp table ... their local buffer
		 * manager is not going to cope.
		 */
		if (RELATION_IS_OTHER_TEMP(rel))
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
				idxtuple = SearchSysCache1(INDEXRELID,
										   ObjectIdGetDatum(indexOid));
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

		/* close relation, keep lock till commit */
		heap_close(rel, NoLock);

		/* Do the job. */
		cluster_rel(tableOid, indexOid, false, stmt->verbose);
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
		 * Build the list of relations to cluster.  Note that this lives in
		 * cluster_context.
		 */
		rvs = get_tables_to_cluster(cluster_context);

		/* Commit to get out of starting transaction */
		PopActiveSnapshot();
		CommitTransactionCommand();

		/* Ok, now that we've got them all, cluster them one by one */
		foreach(rv, rvs)
		{
			RelToCluster *rvtc = (RelToCluster *) lfirst(rv);

			/* Start a new transaction for each relation. */
			StartTransactionCommand();
			/* functions in indexes may want a snapshot set */
			PushActiveSnapshot(GetTransactionSnapshot());
			/* Do the job. */
			cluster_rel(rvtc->tableOid, rvtc->indexOid, true, stmt->verbose);
			PopActiveSnapshot();
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
 * the OID of the original table is preserved.  Thus we do not lose
 * GRANT, inheritance nor references to this table (this was a bug
 * in releases thru 7.3).
 *
 * Indexes are rebuilt too, via REINDEX. Since we are effectively bulk-loading
 * the new table, it's better to create the indexes afterwards than to fill
 * them incrementally while we load the table.
 *
 * If indexOid is InvalidOid, the table will be rewritten in physical order
 * instead of index order.  This is the new implementation of VACUUM FULL,
 * and error messages should refer to the operation as VACUUM not CLUSTER.
 */
void
cluster_rel(Oid tableOid, Oid indexOid, bool recheck, bool verbose)
{
	Relation	OldHeap;

	/* Check for user-requested abort. */
	CHECK_FOR_INTERRUPTS();

	/*
	 * We grab exclusive access to the target rel and index for the duration
	 * of the transaction.  (This is redundant for the single-transaction
	 * case, since cluster() already did it.)  The index lock is taken inside
	 * check_index_is_clusterable.
	 */
	OldHeap = try_relation_open(tableOid, AccessExclusiveLock);

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
		if (!pg_class_ownercheck(tableOid, GetUserId()))
		{
			relation_close(OldHeap, AccessExclusiveLock);
			return;
		}

		/*
		 * Silently skip a temp table for a remote session.  Only doing this
		 * check in the "recheck" case is appropriate (which currently means
		 * somebody is executing a database-wide CLUSTER), because there is
		 * another check in cluster() which will stop any attempt to cluster
		 * remote temp tables by name.  There is another check in cluster_rel
		 * which is redundant, but we leave it for extra safety.
		 */
		if (RELATION_IS_OTHER_TEMP(OldHeap))
		{
			relation_close(OldHeap, AccessExclusiveLock);
			return;
		}

		if (OidIsValid(indexOid))
		{
			/*
			 * Check that the index still exists
			 */
			if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(indexOid)))
			{
				relation_close(OldHeap, AccessExclusiveLock);
				return;
			}

			/*
			 * Check that the index is still the one with indisclustered set.
			 */
			tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexOid));
			if (!HeapTupleIsValid(tuple))		/* probably can't happen */
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
	}

	/*
	 * We allow VACUUM FULL, but not CLUSTER, on shared catalogs.  CLUSTER
	 * would work in most respects, but the index would only get marked as
	 * indisclustered in the current database, leading to unexpected behavior
	 * if CLUSTER were later invoked in another database.
	 */
	if (OidIsValid(indexOid) && OldHeap->rd_rel->relisshared)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster a shared catalog")));

	/*
	 * Don't process temp tables of other backends ... their local buffer
	 * manager is not going to cope.
	 */
	if (RELATION_IS_OTHER_TEMP(OldHeap))
	{
		if (OidIsValid(indexOid))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			   errmsg("cannot cluster temporary tables of other sessions")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("cannot vacuum temporary tables of other sessions")));
	}

	/*
	 * Also check for active uses of the relation in the current transaction,
	 * including open scans and pending AFTER trigger events.
	 */
	CheckTableNotInUse(OldHeap, OidIsValid(indexOid) ? "CLUSTER" : "VACUUM");

	/* Check heap and index are valid to cluster on */
	if (OidIsValid(indexOid))
		check_index_is_clusterable(OldHeap, indexOid, recheck, AccessExclusiveLock);

	/*
	 * Quietly ignore the request if this is a materialized view which has not
	 * been populated from its query. No harm is done because there is no data
	 * to deal with, and we don't want to throw an error if this is part of a
	 * multi-relation request -- for example, CLUSTER was run on the entire
	 * database.
	 */
	if (OldHeap->rd_rel->relkind == RELKIND_MATVIEW &&
		!RelationIsPopulated(OldHeap))
	{
		relation_close(OldHeap, AccessExclusiveLock);
		return;
	}

	/*
	 * All predicate locks on the tuples or pages are about to be made
	 * invalid, because we move tuples around.  Promote them to relation
	 * locks.  Predicate locks on indexes will be promoted when they are
	 * reindexed.
	 */
	TransferPredicateLocksToHeapRelation(OldHeap);

	/* rebuild_relation does all the dirty work */
	rebuild_relation(OldHeap, indexOid, verbose);

	/* NB: rebuild_relation does heap_close() on OldHeap */
}

/*
 * Verify that the specified heap and index are valid to cluster on
 *
 * Side effect: obtains lock on the index.  The caller may
 * in some cases already have AccessExclusiveLock on the table, but
 * not in all cases so we can't rely on the table-level lock for
 * protection here.
 */
void
check_index_is_clusterable(Relation OldHeap, Oid indexOid, bool recheck, LOCKMODE lockmode)
{
	Relation	OldIndex;

	OldIndex = index_open(indexOid, lockmode);

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

	/* Index AM must allow clustering */
	if (!OldIndex->rd_am->amclusterable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on index \"%s\" because access method does not support clustering",
						RelationGetRelationName(OldIndex))));

	/*
	 * Disallow clustering on incomplete indexes (those that might not index
	 * every row of the relation).  We could relax this by making a separate
	 * seqscan pass over the table to copy the missing rows, but that seems
	 * expensive and tedious.
	 */
	if (!heap_attisnull(OldIndex->rd_indextuple, Anum_pg_index_indpred))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on partial index \"%s\"",
						RelationGetRelationName(OldIndex))));

	/*
	 * Disallow if index is left over from a failed CREATE INDEX CONCURRENTLY;
	 * it might well not contain entries for every heap row, or might not even
	 * be internally consistent.  (But note that we don't check indcheckxmin;
	 * the worst consequence of following broken HOT chains would be that we
	 * might put recently-dead tuples out-of-order in the new table, and there
	 * is little harm in that.)
	 */
	if (!IndexIsValid(OldIndex->rd_index))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on invalid index \"%s\"",
						RelationGetRelationName(OldIndex))));

	/* Drop relcache refcnt on OldIndex, but keep lock */
	index_close(OldIndex, NoLock);
}

/*
 * mark_index_clustered: mark the specified index as the one clustered on
 *
 * With indexOid == InvalidOid, will mark all indexes of rel not-clustered.
 */
void
mark_index_clustered(Relation rel, Oid indexOid, bool is_internal)
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
		indexTuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexOid));
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

		indexTuple = SearchSysCacheCopy1(INDEXRELID,
										 ObjectIdGetDatum(thisIndexOid));
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
		}
		else if (thisIndexOid == indexOid)
		{
			/* this was checked earlier, but let's be real sure */
			if (!IndexIsValid(indexForm))
				elog(ERROR, "cannot cluster on invalid index %u", indexOid);
			indexForm->indisclustered = true;
			simple_heap_update(pg_index, &indexTuple->t_self, indexTuple);
			CatalogUpdateIndexes(pg_index, indexTuple);
		}

		InvokeObjectPostAlterHookArg(IndexRelationId, thisIndexOid, 0,
									 InvalidOid, is_internal);

		heap_freetuple(indexTuple);
	}

	heap_close(pg_index, RowExclusiveLock);
}

/*
 * rebuild_relation: rebuild an existing relation in index or physical order
 *
 * OldHeap: table to rebuild --- must be opened and exclusive-locked!
 * indexOid: index to cluster by, or InvalidOid to rewrite in physical order.
 *
 * NB: this routine closes OldHeap at the right time; caller should not.
 */
static void
rebuild_relation(Relation OldHeap, Oid indexOid, bool verbose)
{
	Oid			tableOid = RelationGetRelid(OldHeap);
	Oid			tableSpace = OldHeap->rd_rel->reltablespace;
	Oid			OIDNewHeap;
	bool		is_system_catalog;
	bool		swap_toast_by_content;
	TransactionId frozenXid;
	MultiXactId cutoffMulti;

	/* Mark the correct index as clustered */
	if (OidIsValid(indexOid))
		mark_index_clustered(OldHeap, indexOid, true);

	/* Remember if it's a system catalog */
	is_system_catalog = IsSystemRelation(OldHeap);

	/* Close relcache entry, but keep lock until transaction commit */
	heap_close(OldHeap, NoLock);

	/* Create the transient table that will receive the re-ordered data */
	OIDNewHeap = make_new_heap(tableOid, tableSpace,
							   OldHeap->rd_rel->relpersistence,
							   AccessExclusiveLock);

	/* Copy the heap data into the new table in the desired order */
	copy_heap_data(OIDNewHeap, tableOid, indexOid, verbose,
				   &swap_toast_by_content, &frozenXid, &cutoffMulti);

	/*
	 * Swap the physical files of the target and transient tables, then
	 * rebuild the target's indexes and throw away the transient table.
	 */
	finish_heap_swap(tableOid, OIDNewHeap, is_system_catalog,
					 swap_toast_by_content, false, true,
					 frozenXid, cutoffMulti,
					 OldHeap->rd_rel->relpersistence);
}


/*
 * Create the transient table that will be filled with new data during
 * CLUSTER, ALTER TABLE, and similar operations.  The transient table
 * duplicates the logical structure of the OldHeap, but is placed in
 * NewTableSpace which might be different from OldHeap's.  Also, it's built
 * with the specified persistence, which might differ from the original's.
 *
 * After this, the caller should load the new heap with transferred/modified
 * data, then call finish_heap_swap to complete the operation.
 */
Oid
make_new_heap(Oid OIDOldHeap, Oid NewTableSpace, char relpersistence,
			  LOCKMODE lockmode)
{
	TupleDesc	OldHeapDesc;
	char		NewHeapName[NAMEDATALEN];
	Oid			OIDNewHeap;
	Oid			toastid;
	Relation	OldHeap;
	HeapTuple	tuple;
	Datum		reloptions;
	bool		isNull;
	Oid			namespaceid;

	OldHeap = heap_open(OIDOldHeap, lockmode);
	OldHeapDesc = RelationGetDescr(OldHeap);

	/*
	 * Note that the NewHeap will not receive any of the defaults or
	 * constraints associated with the OldHeap; we don't need 'em, and there's
	 * no reason to spend cycles inserting them into the catalogs only to
	 * delete them.
	 */

	/*
	 * But we do want to use reloptions of the old heap for new heap.
	 */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(OIDOldHeap));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", OIDOldHeap);
	reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
								 &isNull);
	if (isNull)
		reloptions = (Datum) 0;

	if (relpersistence == RELPERSISTENCE_TEMP)
		namespaceid = LookupCreationNamespace("pg_temp");
	else
		namespaceid = RelationGetNamespace(OldHeap);

	/*
	 * Create the new heap, using a temporary name in the same namespace as
	 * the existing table.  NOTE: there is some risk of collision with user
	 * relnames.  Working around this seems more trouble than it's worth; in
	 * particular, we can't create the new heap in a different namespace from
	 * the old, or we will have problems with the TEMP status of temp tables.
	 *
	 * Note: the new heap is not a shared relation, even if we are rebuilding
	 * a shared rel.  However, we do make the new heap mapped if the source is
	 * mapped.  This simplifies swap_relation_files, and is absolutely
	 * necessary for rebuilding pg_class, for reasons explained there.
	 */
	snprintf(NewHeapName, sizeof(NewHeapName), "pg_temp_%u", OIDOldHeap);

	OIDNewHeap = heap_create_with_catalog(NewHeapName,
										  namespaceid,
										  NewTableSpace,
										  InvalidOid,
										  InvalidOid,
										  InvalidOid,
										  OldHeap->rd_rel->relowner,
										  OldHeapDesc,
										  NIL,
										  RELKIND_RELATION,
										  relpersistence,
										  false,
										  RelationIsMapped(OldHeap),
										  true,
										  0,
										  ONCOMMIT_NOOP,
										  reloptions,
										  false,
										  true,
										  true,
										  NULL);
	Assert(OIDNewHeap != InvalidOid);

	ReleaseSysCache(tuple);

	/*
	 * Advance command counter so that the newly-created relation's catalog
	 * tuples will be visible to heap_open.
	 */
	CommandCounterIncrement();

	/*
	 * If necessary, create a TOAST table for the new relation.
	 *
	 * If the relation doesn't have a TOAST table already, we can't need one
	 * for the new relation.  The other way around is possible though: if some
	 * wide columns have been dropped, NewHeapCreateToastTable can decide that
	 * no TOAST table is needed for the new table.
	 *
	 * Note that NewHeapCreateToastTable ends with CommandCounterIncrement, so
	 * that the TOAST table will be visible for insertion.
	 */
	toastid = OldHeap->rd_rel->reltoastrelid;
	if (OidIsValid(toastid))
	{
		/* keep the existing toast table's reloptions, if any */
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(toastid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", toastid);
		reloptions = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
									 &isNull);
		if (isNull)
			reloptions = (Datum) 0;

		NewHeapCreateToastTable(OIDNewHeap, reloptions, lockmode);

		ReleaseSysCache(tuple);
	}

	heap_close(OldHeap, NoLock);

	return OIDNewHeap;
}

/*
 * Do the physical copying of heap data.
 *
 * There are three output parameters:
 * *pSwapToastByContent is set true if toast tables must be swapped by content.
 * *pFreezeXid receives the TransactionId used as freeze cutoff point.
 * *pCutoffMulti receives the MultiXactId used as a cutoff point.
 */
static void
copy_heap_data(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex, bool verbose,
			   bool *pSwapToastByContent, TransactionId *pFreezeXid,
			   MultiXactId *pCutoffMulti)
{
	Relation	NewHeap,
				OldHeap,
				OldIndex;
	TupleDesc	oldTupDesc;
	TupleDesc	newTupDesc;
	int			natts;
	Datum	   *values;
	bool	   *isnull;
	IndexScanDesc indexScan;
	HeapScanDesc heapScan;
	bool		use_wal;
	bool		is_system_catalog;
	TransactionId OldestXmin;
	TransactionId FreezeXid;
	MultiXactId MultiXactCutoff;
	RewriteState rwstate;
	bool		use_sort;
	Tuplesortstate *tuplesort;
	double		num_tuples = 0,
				tups_vacuumed = 0,
				tups_recently_dead = 0;
	int			elevel = verbose ? INFO : DEBUG2;
	PGRUsage	ru0;

	pg_rusage_init(&ru0);

	/*
	 * Open the relations we need.
	 */
	NewHeap = heap_open(OIDNewHeap, AccessExclusiveLock);
	OldHeap = heap_open(OIDOldHeap, AccessExclusiveLock);
	if (OidIsValid(OIDOldIndex))
		OldIndex = index_open(OIDOldIndex, AccessExclusiveLock);
	else
		OldIndex = NULL;

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
	 * If the OldHeap has a toast table, get lock on the toast table to keep
	 * it from being vacuumed.  This is needed because autovacuum processes
	 * toast tables independently of their main tables, with no lock on the
	 * latter.  If an autovacuum were to start on the toast table after we
	 * compute our OldestXmin below, it would use a later OldestXmin, and then
	 * possibly remove as DEAD toast tuples belonging to main tuples we think
	 * are only RECENTLY_DEAD.  Then we'd fail while trying to copy those
	 * tuples.
	 *
	 * We don't need to open the toast relation here, just lock it.  The lock
	 * will be held till end of transaction.
	 */
	if (OldHeap->rd_rel->reltoastrelid)
		LockRelationOid(OldHeap->rd_rel->reltoastrelid, AccessExclusiveLock);

	/*
	 * We need to log the copied data in WAL iff WAL archiving/streaming is
	 * enabled AND it's a WAL-logged rel.
	 */
	use_wal = XLogIsNeeded() && RelationNeedsWAL(NewHeap);

	/* use_wal off requires smgr_targblock be initially invalid */
	Assert(RelationGetTargetBlock(NewHeap) == InvalidBlockNumber);

	/*
	 * If both tables have TOAST tables, perform toast swap by content.  It is
	 * possible that the old table has a toast table but the new one doesn't,
	 * if toastable columns have been dropped.  In that case we have to do
	 * swap by links.  This is okay because swap by content is only essential
	 * for system catalogs, and we don't support schema changes for them.
	 */
	if (OldHeap->rd_rel->reltoastrelid && NewHeap->rd_rel->reltoastrelid)
	{
		*pSwapToastByContent = true;

		/*
		 * When doing swap by content, any toast pointers written into NewHeap
		 * must use the old toast table's OID, because that's where the toast
		 * data will eventually be found.  Set this up by setting rd_toastoid.
		 * This also tells toast_save_datum() to preserve the toast value
		 * OIDs, which we want so as not to invalidate toast pointers in
		 * system catalog caches, and to avoid making multiple copies of a
		 * single toast value.
		 *
		 * Note that we must hold NewHeap open until we are done writing data,
		 * since the relcache will not guarantee to remember this setting once
		 * the relation is closed.  Also, this technique depends on the fact
		 * that no one will try to read from the NewHeap until after we've
		 * finished writing it and swapping the rels --- otherwise they could
		 * follow the toast pointers to the wrong place.  (It would actually
		 * work for values copied over from the old toast table, but not for
		 * any values that we toast which were previously not toasted.)
		 */
		NewHeap->rd_toastoid = OldHeap->rd_rel->reltoastrelid;
	}
	else
		*pSwapToastByContent = false;

	/*
	 * Compute xids used to freeze and weed out dead tuples and multixacts.
	 * Since we're going to rewrite the whole table anyway, there's no reason
	 * not to be aggressive about this.
	 */
	vacuum_set_xid_limits(OldHeap, 0, 0, 0, 0,
						  &OldestXmin, &FreezeXid, NULL, &MultiXactCutoff,
						  NULL);

	/*
	 * FreezeXid will become the table's new relfrozenxid, and that mustn't go
	 * backwards, so take the max.
	 */
	if (TransactionIdPrecedes(FreezeXid, OldHeap->rd_rel->relfrozenxid))
		FreezeXid = OldHeap->rd_rel->relfrozenxid;

	/*
	 * MultiXactCutoff, similarly, shouldn't go backwards either.
	 */
	if (MultiXactIdPrecedes(MultiXactCutoff, OldHeap->rd_rel->relminmxid))
		MultiXactCutoff = OldHeap->rd_rel->relminmxid;

	/* return selected values to caller */
	*pFreezeXid = FreezeXid;
	*pCutoffMulti = MultiXactCutoff;

	/* Remember if it's a system catalog */
	is_system_catalog = IsSystemRelation(OldHeap);

	/* Initialize the rewrite operation */
	rwstate = begin_heap_rewrite(OldHeap, NewHeap, OldestXmin, FreezeXid,
								 MultiXactCutoff, use_wal);

	/*
	 * Decide whether to use an indexscan or seqscan-and-optional-sort to scan
	 * the OldHeap.  We know how to use a sort to duplicate the ordering of a
	 * btree index, and will use seqscan-and-sort for that case if the planner
	 * tells us it's cheaper.  Otherwise, always indexscan if an index is
	 * provided, else plain seqscan.
	 */
	if (OldIndex != NULL && OldIndex->rd_rel->relam == BTREE_AM_OID)
		use_sort = plan_cluster_use_sort(OIDOldHeap, OIDOldIndex);
	else
		use_sort = false;

	/* Set up sorting if wanted */
	if (use_sort)
		tuplesort = tuplesort_begin_cluster(oldTupDesc, OldIndex,
											maintenance_work_mem, false);
	else
		tuplesort = NULL;

	/*
	 * Prepare to scan the OldHeap.  To ensure we see recently-dead tuples
	 * that still need to be copied, we scan with SnapshotAny and use
	 * HeapTupleSatisfiesVacuum for the visibility test.
	 */
	if (OldIndex != NULL && !use_sort)
	{
		heapScan = NULL;
		indexScan = index_beginscan(OldHeap, OldIndex, SnapshotAny, 0, 0);
		index_rescan(indexScan, NULL, 0, NULL, 0);
	}
	else
	{
		heapScan = heap_beginscan(OldHeap, SnapshotAny, 0, (ScanKey) NULL);
		indexScan = NULL;
	}

	/* Log what we're doing */
	if (indexScan != NULL)
		ereport(elevel,
				(errmsg("clustering \"%s.%s\" using index scan on \"%s\"",
						get_namespace_name(RelationGetNamespace(OldHeap)),
						RelationGetRelationName(OldHeap),
						RelationGetRelationName(OldIndex))));
	else if (tuplesort != NULL)
		ereport(elevel,
				(errmsg("clustering \"%s.%s\" using sequential scan and sort",
						get_namespace_name(RelationGetNamespace(OldHeap)),
						RelationGetRelationName(OldHeap))));
	else
		ereport(elevel,
				(errmsg("vacuuming \"%s.%s\"",
						get_namespace_name(RelationGetNamespace(OldHeap)),
						RelationGetRelationName(OldHeap))));

	/*
	 * Scan through the OldHeap, either in OldIndex order or sequentially;
	 * copy each tuple into the NewHeap, or transiently to the tuplesort
	 * module.  Note that we don't bother sorting dead tuples (they won't get
	 * to the new table anyway).
	 */
	for (;;)
	{
		HeapTuple	tuple;
		Buffer		buf;
		bool		isdead;

		CHECK_FOR_INTERRUPTS();

		if (indexScan != NULL)
		{
			tuple = index_getnext(indexScan, ForwardScanDirection);
			if (tuple == NULL)
				break;

			/* Since we used no scan keys, should never need to recheck */
			if (indexScan->xs_recheck)
				elog(ERROR, "CLUSTER does not support lossy index conditions");

			buf = indexScan->xs_cbuf;
		}
		else
		{
			tuple = heap_getnext(heapScan, ForwardScanDirection);
			if (tuple == NULL)
				break;

			buf = heapScan->rs_cbuf;
		}

		LockBuffer(buf, BUFFER_LOCK_SHARE);

		switch (HeapTupleSatisfiesVacuum(tuple, OldestXmin, buf))
		{
			case HEAPTUPLE_DEAD:
				/* Definitely dead */
				isdead = true;
				break;
			case HEAPTUPLE_RECENTLY_DEAD:
				tups_recently_dead += 1;
				/* fall through */
			case HEAPTUPLE_LIVE:
				/* Live or recently dead, must copy it */
				isdead = false;
				break;
			case HEAPTUPLE_INSERT_IN_PROGRESS:

				/*
				 * Since we hold exclusive lock on the relation, normally the
				 * only way to see this is if it was inserted earlier in our
				 * own transaction.  However, it can happen in system
				 * catalogs, since we tend to release write lock before commit
				 * there.  Give a warning if neither case applies; but in any
				 * case we had better copy it.
				 */
				if (!is_system_catalog &&
					!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tuple->t_data)))
					elog(WARNING, "concurrent insert in progress within table \"%s\"",
						 RelationGetRelationName(OldHeap));
				/* treat as live */
				isdead = false;
				break;
			case HEAPTUPLE_DELETE_IN_PROGRESS:

				/*
				 * Similar situation to INSERT_IN_PROGRESS case.
				 */
				if (!is_system_catalog &&
					!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetUpdateXid(tuple->t_data)))
					elog(WARNING, "concurrent delete in progress within table \"%s\"",
						 RelationGetRelationName(OldHeap));
				/* treat as recently dead */
				tups_recently_dead += 1;
				isdead = false;
				break;
			default:
				elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
				isdead = false; /* keep compiler quiet */
				break;
		}

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		if (isdead)
		{
			tups_vacuumed += 1;
			/* heap rewrite module still needs to see it... */
			if (rewrite_heap_dead_tuple(rwstate, tuple))
			{
				/* A previous recently-dead tuple is now known dead */
				tups_vacuumed += 1;
				tups_recently_dead -= 1;
			}
			continue;
		}

		num_tuples += 1;
		if (tuplesort != NULL)
			tuplesort_putheaptuple(tuplesort, tuple);
		else
			reform_and_rewrite_tuple(tuple,
									 oldTupDesc, newTupDesc,
									 values, isnull,
									 NewHeap->rd_rel->relhasoids, rwstate);
	}

	if (indexScan != NULL)
		index_endscan(indexScan);
	if (heapScan != NULL)
		heap_endscan(heapScan);

	/*
	 * In scan-and-sort mode, complete the sort, then read out all live tuples
	 * from the tuplestore and write them to the new relation.
	 */
	if (tuplesort != NULL)
	{
		tuplesort_performsort(tuplesort);

		for (;;)
		{
			HeapTuple	tuple;
			bool		shouldfree;

			CHECK_FOR_INTERRUPTS();

			tuple = tuplesort_getheaptuple(tuplesort, true, &shouldfree);
			if (tuple == NULL)
				break;

			reform_and_rewrite_tuple(tuple,
									 oldTupDesc, newTupDesc,
									 values, isnull,
									 NewHeap->rd_rel->relhasoids, rwstate);

			if (shouldfree)
				heap_freetuple(tuple);
		}

		tuplesort_end(tuplesort);
	}

	/* Write out any remaining tuples, and fsync if needed */
	end_heap_rewrite(rwstate);

	/* Reset rd_toastoid just to be tidy --- it shouldn't be looked at again */
	NewHeap->rd_toastoid = InvalidOid;

	/* Log what we did */
	ereport(elevel,
			(errmsg("\"%s\": found %.0f removable, %.0f nonremovable row versions in %u pages",
					RelationGetRelationName(OldHeap),
					tups_vacuumed, num_tuples,
					RelationGetNumberOfBlocks(OldHeap)),
			 errdetail("%.0f dead row versions cannot be removed yet.\n"
					   "%s.",
					   tups_recently_dead,
					   pg_rusage_show(&ru0))));

	/* Clean up */
	pfree(values);
	pfree(isnull);

	if (OldIndex != NULL)
		index_close(OldIndex, NoLock);
	heap_close(OldHeap, NoLock);
	heap_close(NewHeap, NoLock);
}

/*
 * Swap the physical files of two given relations.
 *
 * We swap the physical identity (reltablespace, relfilenode) while keeping the
 * same logical identities of the two relations.  relpersistence is also
 * swapped, which is critical since it determines where buffers live for each
 * relation.
 *
 * We can swap associated TOAST data in either of two ways: recursively swap
 * the physical content of the toast tables (and their indexes), or swap the
 * TOAST links in the given relations' pg_class entries.  The former is needed
 * to manage rewrites of shared catalogs (where we cannot change the pg_class
 * links) while the latter is the only way to handle cases in which a toast
 * table is added or removed altogether.
 *
 * Additionally, the first relation is marked with relfrozenxid set to
 * frozenXid.  It seems a bit ugly to have this here, but the caller would
 * have to do it anyway, so having it here saves a heap_update.  Note: in
 * the swap-toast-links case, we assume we don't need to change the toast
 * table's relfrozenxid: the new version of the toast table should already
 * have relfrozenxid set to RecentXmin, which is good enough.
 *
 * Lastly, if r2 and its toast table and toast index (if any) are mapped,
 * their OIDs are emitted into mapped_tables[].  This is hacky but beats
 * having to look the information up again later in finish_heap_swap.
 */
static void
swap_relation_files(Oid r1, Oid r2, bool target_is_pg_class,
					bool swap_toast_by_content,
					bool is_internal,
					TransactionId frozenXid,
					MultiXactId cutoffMulti,
					Oid *mapped_tables)
{
	Relation	relRelation;
	HeapTuple	reltup1,
				reltup2;
	Form_pg_class relform1,
				relform2;
	Oid			relfilenode1,
				relfilenode2;
	Oid			swaptemp;
	char		swptmpchr;
	CatalogIndexState indstate;

	/* We need writable copies of both pg_class tuples. */
	relRelation = heap_open(RelationRelationId, RowExclusiveLock);

	reltup1 = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(r1));
	if (!HeapTupleIsValid(reltup1))
		elog(ERROR, "cache lookup failed for relation %u", r1);
	relform1 = (Form_pg_class) GETSTRUCT(reltup1);

	reltup2 = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(r2));
	if (!HeapTupleIsValid(reltup2))
		elog(ERROR, "cache lookup failed for relation %u", r2);
	relform2 = (Form_pg_class) GETSTRUCT(reltup2);

	relfilenode1 = relform1->relfilenode;
	relfilenode2 = relform2->relfilenode;

	if (OidIsValid(relfilenode1) && OidIsValid(relfilenode2))
	{
		/*
		 * Normal non-mapped relations: swap relfilenodes, reltablespaces,
		 * relpersistence
		 */
		Assert(!target_is_pg_class);

		swaptemp = relform1->relfilenode;
		relform1->relfilenode = relform2->relfilenode;
		relform2->relfilenode = swaptemp;

		swaptemp = relform1->reltablespace;
		relform1->reltablespace = relform2->reltablespace;
		relform2->reltablespace = swaptemp;

		swptmpchr = relform1->relpersistence;
		relform1->relpersistence = relform2->relpersistence;
		relform2->relpersistence = swptmpchr;

		/* Also swap toast links, if we're swapping by links */
		if (!swap_toast_by_content)
		{
			swaptemp = relform1->reltoastrelid;
			relform1->reltoastrelid = relform2->reltoastrelid;
			relform2->reltoastrelid = swaptemp;
		}
	}
	else
	{
		/*
		 * Mapped-relation case.  Here we have to swap the relation mappings
		 * instead of modifying the pg_class columns.  Both must be mapped.
		 */
		if (OidIsValid(relfilenode1) || OidIsValid(relfilenode2))
			elog(ERROR, "cannot swap mapped relation \"%s\" with non-mapped relation",
				 NameStr(relform1->relname));

		/*
		 * We can't change the tablespace nor persistence of a mapped rel, and
		 * we can't handle toast link swapping for one either, because we must
		 * not apply any critical changes to its pg_class row.  These cases
		 * should be prevented by upstream permissions tests, so these checks
		 * are non-user-facing emergency backstop.
		 */
		if (relform1->reltablespace != relform2->reltablespace)
			elog(ERROR, "cannot change tablespace of mapped relation \"%s\"",
				 NameStr(relform1->relname));
		if (relform1->relpersistence != relform2->relpersistence)
			elog(ERROR, "cannot change persistence of mapped relation \"%s\"",
				 NameStr(relform1->relname));
		if (!swap_toast_by_content &&
			(relform1->reltoastrelid || relform2->reltoastrelid))
			elog(ERROR, "cannot swap toast by links for mapped relation \"%s\"",
				 NameStr(relform1->relname));

		/*
		 * Fetch the mappings --- shouldn't fail, but be paranoid
		 */
		relfilenode1 = RelationMapOidToFilenode(r1, relform1->relisshared);
		if (!OidIsValid(relfilenode1))
			elog(ERROR, "could not find relation mapping for relation \"%s\", OID %u",
				 NameStr(relform1->relname), r1);
		relfilenode2 = RelationMapOidToFilenode(r2, relform2->relisshared);
		if (!OidIsValid(relfilenode2))
			elog(ERROR, "could not find relation mapping for relation \"%s\", OID %u",
				 NameStr(relform2->relname), r2);

		/*
		 * Send replacement mappings to relmapper.  Note these won't actually
		 * take effect until CommandCounterIncrement.
		 */
		RelationMapUpdateMap(r1, relfilenode2, relform1->relisshared, false);
		RelationMapUpdateMap(r2, relfilenode1, relform2->relisshared, false);

		/* Pass OIDs of mapped r2 tables back to caller */
		*mapped_tables++ = r2;
	}

	/*
	 * In the case of a shared catalog, these next few steps will only affect
	 * our own database's pg_class row; but that's okay, because they are all
	 * noncritical updates.  That's also an important fact for the case of a
	 * mapped catalog, because it's possible that we'll commit the map change
	 * and then fail to commit the pg_class update.
	 */

	/* set rel1's frozen Xid and minimum MultiXid */
	if (relform1->relkind != RELKIND_INDEX)
	{
		Assert(TransactionIdIsNormal(frozenXid));
		relform1->relfrozenxid = frozenXid;
		Assert(MultiXactIdIsValid(cutoffMulti));
		relform1->relminmxid = cutoffMulti;
	}

	/* swap size statistics too, since new rel has freshly-updated stats */
	{
		int32		swap_pages;
		float4		swap_tuples;
		int32		swap_allvisible;

		swap_pages = relform1->relpages;
		relform1->relpages = relform2->relpages;
		relform2->relpages = swap_pages;

		swap_tuples = relform1->reltuples;
		relform1->reltuples = relform2->reltuples;
		relform2->reltuples = swap_tuples;

		swap_allvisible = relform1->relallvisible;
		relform1->relallvisible = relform2->relallvisible;
		relform2->relallvisible = swap_allvisible;
	}

	/*
	 * Update the tuples in pg_class --- unless the target relation of the
	 * swap is pg_class itself.  In that case, there is zero point in making
	 * changes because we'd be updating the old data that we're about to throw
	 * away.  Because the real work being done here for a mapped relation is
	 * just to change the relation map settings, it's all right to not update
	 * the pg_class rows in this case. The most important changes will instead
	 * performed later, in finish_heap_swap() itself.
	 */
	if (!target_is_pg_class)
	{
		simple_heap_update(relRelation, &reltup1->t_self, reltup1);
		simple_heap_update(relRelation, &reltup2->t_self, reltup2);

		/* Keep system catalogs current */
		indstate = CatalogOpenIndexes(relRelation);
		CatalogIndexInsert(indstate, reltup1);
		CatalogIndexInsert(indstate, reltup2);
		CatalogCloseIndexes(indstate);
	}
	else
	{
		/* no update ... but we do still need relcache inval */
		CacheInvalidateRelcacheByTuple(reltup1);
		CacheInvalidateRelcacheByTuple(reltup2);
	}

	/*
	 * Post alter hook for modified relations. The change to r2 is always
	 * internal, but r1 depends on the invocation context.
	 */
	InvokeObjectPostAlterHookArg(RelationRelationId, r1, 0,
								 InvalidOid, is_internal);
	InvokeObjectPostAlterHookArg(RelationRelationId, r2, 0,
								 InvalidOid, true);

	/*
	 * If we have toast tables associated with the relations being swapped,
	 * deal with them too.
	 */
	if (relform1->reltoastrelid || relform2->reltoastrelid)
	{
		if (swap_toast_by_content)
		{
			if (relform1->reltoastrelid && relform2->reltoastrelid)
			{
				/* Recursively swap the contents of the toast tables */
				swap_relation_files(relform1->reltoastrelid,
									relform2->reltoastrelid,
									target_is_pg_class,
									swap_toast_by_content,
									is_internal,
									frozenXid,
									cutoffMulti,
									mapped_tables);
			}
			else
			{
				/* caller messed up */
				elog(ERROR, "cannot swap toast files by content when there's only one");
			}
		}
		else
		{
			/*
			 * We swapped the ownership links, so we need to change dependency
			 * data to match.
			 *
			 * NOTE: it is possible that only one table has a toast table.
			 *
			 * NOTE: at present, a TOAST table's only dependency is the one on
			 * its owning table.  If more are ever created, we'd need to use
			 * something more selective than deleteDependencyRecordsFor() to
			 * get rid of just the link we want.
			 */
			ObjectAddress baseobject,
						toastobject;
			long		count;

			/*
			 * We disallow this case for system catalogs, to avoid the
			 * possibility that the catalog we're rebuilding is one of the
			 * ones the dependency changes would change.  It's too late to be
			 * making any data changes to the target catalog.
			 */
			if (IsSystemClass(r1, relform1))
				elog(ERROR, "cannot swap toast files by links for system catalogs");

			/* Delete old dependencies */
			if (relform1->reltoastrelid)
			{
				count = deleteDependencyRecordsFor(RelationRelationId,
												   relform1->reltoastrelid,
												   false);
				if (count != 1)
					elog(ERROR, "expected one dependency record for TOAST table, found %ld",
						 count);
			}
			if (relform2->reltoastrelid)
			{
				count = deleteDependencyRecordsFor(RelationRelationId,
												   relform2->reltoastrelid,
												   false);
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
				recordDependencyOn(&toastobject, &baseobject,
								   DEPENDENCY_INTERNAL);
			}

			if (relform2->reltoastrelid)
			{
				baseobject.objectId = r2;
				toastobject.objectId = relform2->reltoastrelid;
				recordDependencyOn(&toastobject, &baseobject,
								   DEPENDENCY_INTERNAL);
			}
		}
	}

	/*
	 * If we're swapping two toast tables by content, do the same for their
	 * valid index. The swap can actually be safely done only if the relations
	 * have indexes.
	 */
	if (swap_toast_by_content &&
		relform1->relkind == RELKIND_TOASTVALUE &&
		relform2->relkind == RELKIND_TOASTVALUE)
	{
		Oid			toastIndex1,
					toastIndex2;

		/* Get valid index for each relation */
		toastIndex1 = toast_get_valid_index(r1,
											AccessExclusiveLock);
		toastIndex2 = toast_get_valid_index(r2,
											AccessExclusiveLock);

		swap_relation_files(toastIndex1,
							toastIndex2,
							target_is_pg_class,
							swap_toast_by_content,
							is_internal,
							InvalidTransactionId,
							InvalidMultiXactId,
							mapped_tables);
	}

	/* Clean up. */
	heap_freetuple(reltup1);
	heap_freetuple(reltup2);

	heap_close(relRelation, RowExclusiveLock);

	/*
	 * Close both relcache entries' smgr links.  We need this kluge because
	 * both links will be invalidated during upcoming CommandCounterIncrement.
	 * Whichever of the rels is the second to be cleared will have a dangling
	 * reference to the other's smgr entry.  Rather than trying to avoid this
	 * by ordering operations just so, it's easiest to close the links first.
	 * (Fortunately, since one of the entries is local in our transaction,
	 * it's sufficient to clear out our own relcache this way; the problem
	 * cannot arise for other backends when they see our update on the
	 * non-transient relation.)
	 *
	 * Caution: the placement of this step interacts with the decision to
	 * handle toast rels by recursion.  When we are trying to rebuild pg_class
	 * itself, the smgr close on pg_class must happen after all accesses in
	 * this function.
	 */
	RelationCloseSmgrByOid(r1);
	RelationCloseSmgrByOid(r2);
}

/*
 * Remove the transient table that was built by make_new_heap, and finish
 * cleaning up (including rebuilding all indexes on the old heap).
 */
void
finish_heap_swap(Oid OIDOldHeap, Oid OIDNewHeap,
				 bool is_system_catalog,
				 bool swap_toast_by_content,
				 bool check_constraints,
				 bool is_internal,
				 TransactionId frozenXid,
				 MultiXactId cutoffMulti,
				 char newrelpersistence)
{
	ObjectAddress object;
	Oid			mapped_tables[4];
	int			reindex_flags;
	int			i;

	/* Zero out possible results from swapped_relation_files */
	memset(mapped_tables, 0, sizeof(mapped_tables));

	/*
	 * Swap the contents of the heap relations (including any toast tables).
	 * Also set old heap's relfrozenxid to frozenXid.
	 */
	swap_relation_files(OIDOldHeap, OIDNewHeap,
						(OIDOldHeap == RelationRelationId),
						swap_toast_by_content, is_internal,
						frozenXid, cutoffMulti, mapped_tables);

	/*
	 * If it's a system catalog, queue an sinval message to flush all
	 * catcaches on the catalog when we reach CommandCounterIncrement.
	 */
	if (is_system_catalog)
		CacheInvalidateCatalog(OIDOldHeap);

	/*
	 * Rebuild each index on the relation (but not the toast table, which is
	 * all-new at this point).  It is important to do this before the DROP
	 * step because if we are processing a system catalog that will be used
	 * during DROP, we want to have its indexes available.  There is no
	 * advantage to the other order anyway because this is all transactional,
	 * so no chance to reclaim disk space before commit.  We do not need a
	 * final CommandCounterIncrement() because reindex_relation does it.
	 *
	 * Note: because index_build is called via reindex_relation, it will never
	 * set indcheckxmin true for the indexes.  This is OK even though in some
	 * sense we are building new indexes rather than rebuilding existing ones,
	 * because the new heap won't contain any HOT chains at all, let alone
	 * broken ones, so it can't be necessary to set indcheckxmin.
	 */
	reindex_flags = REINDEX_REL_SUPPRESS_INDEX_USE;
	if (check_constraints)
		reindex_flags |= REINDEX_REL_CHECK_CONSTRAINTS;

	/*
	 * Ensure that the indexes have the same persistence as the parent
	 * relation.
	 */
	if (newrelpersistence == RELPERSISTENCE_UNLOGGED)
		reindex_flags |= REINDEX_REL_FORCE_INDEXES_UNLOGGED;
	else if (newrelpersistence == RELPERSISTENCE_PERMANENT)
		reindex_flags |= REINDEX_REL_FORCE_INDEXES_PERMANENT;

	reindex_relation(OIDOldHeap, reindex_flags, 0);

	/*
	 * If the relation being rebuild is pg_class, swap_relation_files()
	 * couldn't update pg_class's own pg_class entry (check comments in
	 * swap_relation_files()), thus relfrozenxid was not updated. That's
	 * annoying because a potential reason for doing a VACUUM FULL is a
	 * imminent or actual anti-wraparound shutdown.  So, now that we can
	 * access the new relation using it's indices, update relfrozenxid.
	 * pg_class doesn't have a toast relation, so we don't need to update the
	 * corresponding toast relation. Not that there's little point moving all
	 * relfrozenxid updates here since swap_relation_files() needs to write to
	 * pg_class for non-mapped relations anyway.
	 */
	if (OIDOldHeap == RelationRelationId)
	{
		Relation	relRelation;
		HeapTuple	reltup;
		Form_pg_class relform;

		relRelation = heap_open(RelationRelationId, RowExclusiveLock);

		reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(OIDOldHeap));
		if (!HeapTupleIsValid(reltup))
			elog(ERROR, "cache lookup failed for relation %u", OIDOldHeap);
		relform = (Form_pg_class) GETSTRUCT(reltup);

		relform->relfrozenxid = frozenXid;
		relform->relminmxid = cutoffMulti;

		simple_heap_update(relRelation, &reltup->t_self, reltup);
		CatalogUpdateIndexes(relRelation, reltup);

		heap_close(relRelation, RowExclusiveLock);
	}

	/* Destroy new heap with old filenode */
	object.classId = RelationRelationId;
	object.objectId = OIDNewHeap;
	object.objectSubId = 0;

	/*
	 * The new relation is local to our transaction and we know nothing
	 * depends on it, so DROP_RESTRICT should be OK.
	 */
	performDeletion(&object, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	/* performDeletion does CommandCounterIncrement at end */

	/*
	 * Now we must remove any relation mapping entries that we set up for the
	 * transient table, as well as its toast table and toast index if any. If
	 * we fail to do this before commit, the relmapper will complain about new
	 * permanent map entries being added post-bootstrap.
	 */
	for (i = 0; OidIsValid(mapped_tables[i]); i++)
		RelationMapRemoveMapping(mapped_tables[i]);

	/*
	 * At this point, everything is kosher except that, if we did toast swap
	 * by links, the toast table's name corresponds to the transient table.
	 * The name is irrelevant to the backend because it's referenced by OID,
	 * but users looking at the catalogs could be confused.  Rename it to
	 * prevent this problem.
	 *
	 * Note no lock required on the relation, because we already hold an
	 * exclusive lock on it.
	 */
	if (!swap_toast_by_content)
	{
		Relation	newrel;

		newrel = heap_open(OIDOldHeap, NoLock);
		if (OidIsValid(newrel->rd_rel->reltoastrelid))
		{
			Oid			toastidx;
			char		NewToastName[NAMEDATALEN];

			/* Get the associated valid index to be renamed */
			toastidx = toast_get_valid_index(newrel->rd_rel->reltoastrelid,
											 AccessShareLock);

			/* rename the toast table ... */
			snprintf(NewToastName, NAMEDATALEN, "pg_toast_%u",
					 OIDOldHeap);
			RenameRelationInternal(newrel->rd_rel->reltoastrelid,
								   NewToastName, true);

			/* ... and its valid index too. */
			snprintf(NewToastName, NAMEDATALEN, "pg_toast_%u_index",
					 OIDOldHeap);

			RenameRelationInternal(toastidx,
								   NewToastName, true);
		}
		relation_close(newrel, NoLock);
	}
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
	scan = heap_beginscan_catalog(indRelation, 1, &entry);
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


/*
 * Reconstruct and rewrite the given tuple
 *
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
static void
reform_and_rewrite_tuple(HeapTuple tuple,
						 TupleDesc oldTupDesc, TupleDesc newTupDesc,
						 Datum *values, bool *isnull,
						 bool newRelHasOids, RewriteState rwstate)
{
	HeapTuple	copiedTuple;
	int			i;

	heap_deform_tuple(tuple, oldTupDesc, values, isnull);

	/* Be sure to null out any dropped columns */
	for (i = 0; i < newTupDesc->natts; i++)
	{
		if (newTupDesc->attrs[i]->attisdropped)
			isnull[i] = true;
	}

	copiedTuple = heap_form_tuple(newTupDesc, values, isnull);

	/* Preserve OID, if any */
	if (newRelHasOids)
		HeapTupleSetOid(copiedTuple, HeapTupleGetOid(tuple));

	/* The heap rewrite module does the rest */
	rewrite_heap_tuple(rwstate, tuple, copiedTuple);

	heap_freetuple(copiedTuple);
}
