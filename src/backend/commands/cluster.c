/*-------------------------------------------------------------------------
 *
 * cluster.c
 *	  CLUSTER a table on an index.
 *
 * There is hardly anything left of Paul Brown's original implementation...
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/cluster.c,v 1.116.2.2 2005/02/06 20:19:42 tgl Exp $
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
static Oid	make_new_heap(Oid OIDOldHeap, const char *NewName);
static void copy_heap_data(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex);
static List *get_indexattr_list(Relation OldHeap, Oid OldIndex);
static void rebuild_indexes(Oid OIDOldHeap, List *indexes);
static void swap_relfilenodes(Oid r1, Oid r2);
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
 * We also allow a relation being specified without index.	In that case,
 * the indisclustered bit will be looked up, and an ERROR will be thrown
 * if there is no index with the bit set.
 *---------------------------------------------------------------------------
 */
void
cluster(ClusterStmt *stmt)
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

		if (stmt->indexname == NULL)
		{
			List	   *index;

			/* We need to find the index that has indisclustered set. */
			foreach(index, RelationGetIndexList(rel))
			{
				HeapTuple	idxtuple;
				Form_pg_index indexForm;

				indexOid = lfirsto(index);
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
		 * This is the "multi relation" case. We need to cluster all
		 * tables that have some index with indisclustered set.
		 */
		MemoryContext cluster_context;
		List	   *rv,
				   *rvs;

		/*
		 * We cannot run this form of CLUSTER inside a user transaction
		 * block; we'd be holding locks way too long.
		 */
		PreventTransactionChain((void *) stmt, "CLUSTER");

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
		 * Build the list of relations to cluster.	Note that this lives
		 * in cluster_context.
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
			SetQuerySnapshot(); /* might be needed for functions in
								 * indexes */
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
	Relation	OldHeap,
				OldIndex;

	/* Check for user-requested abort. */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Since we may open a new transaction for each relation, we have to
	 * check that the relation still is what we think it is.
	 *
	 * If this is a single-transaction CLUSTER, we can skip these tests. We
	 * *must* skip the one on indisclustered since it would reject an
	 * attempt to cluster a not-previously-clustered index.
	 */
	if (recheck)
	{
		HeapTuple	tuple;
		Form_pg_index indexForm;

		/*
		 * Check if the relation and index still exist before opening them
		 */
		if (!SearchSysCacheExists(RELOID,
								  ObjectIdGetDatum(rvtc->tableOid),
								  0, 0, 0) ||
			!SearchSysCacheExists(RELOID,
								  ObjectIdGetDatum(rvtc->indexOid),
								  0, 0, 0))
			return;

		/* Check that the user still owns the relation */
		if (!pg_class_ownercheck(rvtc->tableOid, GetUserId()))
			return;

		/*
		 * Check that the index is still the one with indisclustered set.
		 */
		tuple = SearchSysCache(INDEXRELID,
							   ObjectIdGetDatum(rvtc->indexOid),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			return;				/* could have gone away... */
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
	 * duration of the transaction.  (This is redundant for the single-
	 * transaction case, since cluster() already did it.)
	 */
	OldHeap = heap_open(rvtc->tableOid, AccessExclusiveLock);

	OldIndex = index_open(rvtc->indexOid);
	LockRelation(OldIndex, AccessExclusiveLock);

	/*
	 * Check that index is in fact an index on the given relation
	 */
	if (OldIndex->rd_index == NULL ||
		OldIndex->rd_index->indrelid != rvtc->tableOid)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index for table \"%s\"",
						RelationGetRelationName(OldIndex),
						RelationGetRelationName(OldHeap))));

	/*
	 * Disallow clustering on incomplete indexes (those that might not
	 * index every row of the relation).  We could relax this by making a
	 * separate seqscan pass over the table to copy the missing rows, but
	 * that seems expensive and tedious.
	 */
	if (!heap_attisnull(OldIndex->rd_indextuple, Anum_pg_index_indpred))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot cluster on partial index")));
	if (!OldIndex->rd_am->amindexnulls)
	{
		AttrNumber	colno;

		/*
		 * If the AM doesn't index nulls, then it's a partial index unless
		 * we can prove all the rows are non-null.	Note we only need look
		 * at the first column; multicolumn-capable AMs are *required* to
		 * index nulls in columns after the first.
		 */
		colno = OldIndex->rd_index->indkey[0];
		if (colno > 0)
		{
			/* ordinary user attribute */
			if (!OldHeap->rd_att->attrs[colno - 1]->attnotnull)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot cluster when index access method does not handle null values"),
						 errhint("You may be able to work around this by marking column \"%s\" NOT NULL.",
				  NameStr(OldHeap->rd_att->attrs[colno - 1]->attname))));
		}
		else if (colno < 0)
		{
			/* system column --- okay, always non-null */
		}
		else
		{
			/* index expression, lose... */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot cluster on expressional index when index access method does not handle null values")));
		}
	}

	/*
	 * Disallow clustering system relations.  This will definitely NOT
	 * work for shared relations (we have no way to update pg_class rows
	 * in other databases), nor for nailed-in-cache relations (the
	 * relfilenode values for those are hardwired, see relcache.c).  It
	 * might work for other system relations, but I ain't gonna risk it.
	 */
	if (IsSystemRelation(OldHeap))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" is a system catalog",
						RelationGetRelationName(OldHeap))));

	/*
	 * Don't allow cluster on temp tables of other backends ... their
	 * local buffer manager is not going to cope.
	 */
	if (isOtherTempNamespace(RelationGetNamespace(OldHeap)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			   errmsg("cannot cluster temporary tables of other sessions")));

	/* Drop relcache refcnt on OldIndex, but keep lock */
	index_close(OldIndex);

	/* rebuild_relation does all the dirty work */
	rebuild_relation(OldHeap, rvtc->indexOid);

	/* NB: rebuild_relation does heap_close() on OldHeap */
}

/*
 * rebuild_relation: rebuild an existing relation
 *
 * This is shared code between CLUSTER and TRUNCATE.  In the TRUNCATE
 * case, the new relation is built and left empty.	In the CLUSTER case,
 * it is filled with data read from the old relation in the order specified
 * by the index.
 *
 * OldHeap: table to rebuild --- must be opened and exclusive-locked!
 * indexOid: index to cluster by, or InvalidOid in TRUNCATE case
 *
 * NB: this routine closes OldHeap at the right time; caller should not.
 */
void
rebuild_relation(Relation OldHeap, Oid indexOid)
{
	Oid			tableOid = RelationGetRelid(OldHeap);
	List	   *indexes;
	Oid			OIDNewHeap;
	char		NewHeapName[NAMEDATALEN];
	ObjectAddress object;

	/* Save the information about all indexes on the relation. */
	indexes = get_indexattr_list(OldHeap, indexOid);

	/* Close relcache entry, but keep lock until transaction commit */
	heap_close(OldHeap, NoLock);

	/*
	 * Create the new heap, using a temporary name in the same namespace
	 * as the existing table.  NOTE: there is some risk of collision with
	 * user relnames.  Working around this seems more trouble than it's
	 * worth; in particular, we can't create the new heap in a different
	 * namespace from the old, or we will have problems with the TEMP
	 * status of temp tables.
	 */
	snprintf(NewHeapName, sizeof(NewHeapName), "pg_temp_%u", tableOid);

	OIDNewHeap = make_new_heap(tableOid, NewHeapName);

	/*
	 * We don't need CommandCounterIncrement() because make_new_heap did
	 * it.
	 */

	/*
	 * Copy the heap data into the new table in the desired order.
	 */
	if (OidIsValid(indexOid))
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
	 * CommandCounterIncrement() because rebuild_indexes does it.
	 */
	rebuild_indexes(tableOid, indexes);
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
	TupleDesc	oldTupDesc;
	TupleDesc	newTupDesc;
	int			natts;
	Datum	   *values;
	char	   *nulls;
	IndexScanDesc scan;
	HeapTuple	tuple;

	/*
	 * Open the relations we need.
	 */
	NewHeap = heap_open(OIDNewHeap, AccessExclusiveLock);
	OldHeap = heap_open(OIDOldHeap, AccessExclusiveLock);
	OldIndex = index_open(OIDOldIndex);

	/*
	 * Their tuple descriptors should be exactly alike, but here we only
	 * need assume that they have the same number of columns.
	 */
	oldTupDesc = RelationGetDescr(OldHeap);
	newTupDesc = RelationGetDescr(NewHeap);
	Assert(newTupDesc->natts == oldTupDesc->natts);

	/* Preallocate values/nulls arrays */
	natts = newTupDesc->natts;
	values = (Datum *) palloc0(natts * sizeof(Datum));
	nulls = (char *) palloc(natts * sizeof(char));
	memset(nulls, 'n', natts * sizeof(char));

	/*
	 * Scan through the OldHeap on the OldIndex and copy each tuple into the
	 * NewHeap.
	 */
	scan = index_beginscan(OldHeap, OldIndex, SnapshotNow, 0, (ScanKey) NULL);

	while ((tuple = index_getnext(scan, ForwardScanDirection)) != NULL)
	{
		/*
		 * We cannot simply pass the tuple to heap_insert(), for several
		 * reasons:
		 *
		 * 1. heap_insert() will overwrite the commit-status fields of the
		 * tuple it's handed.  This would trash the source relation, which is
		 * bad news if we abort later on.  (This was a bug in releases thru
		 * 7.0)
		 *
		 * 2. We'd like to squeeze out the values of any dropped columns,
		 * both to save space and to ensure we have no corner-case failures.
		 * (It's possible for example that the new table hasn't got a TOAST
		 * table and so is unable to store any large values of dropped cols.)
		 *
		 * 3. The tuple might not even be legal for the new table; this is
		 * currently only known to happen as an after-effect of ALTER TABLE
		 * SET WITHOUT OIDS.
		 *
		 * So, we must reconstruct the tuple from component Datums.
		 */
		HeapTuple	copiedTuple;
		int			i;

		heap_deformtuple(tuple, oldTupDesc, values, nulls);

		/* Be sure to null out any dropped columns */
		for (i = 0; i < natts; i++)
		{
			if (newTupDesc->attrs[i]->attisdropped)
				nulls[i] = 'n';
		}

		copiedTuple = heap_formtuple(newTupDesc, values, nulls);

		/* Preserve OID, if any */
		if (NewHeap->rd_rel->relhasoids)
			HeapTupleSetOid(copiedTuple, HeapTupleGetOid(tuple));

		simple_heap_insert(NewHeap, copiedTuple);

		heap_freetuple(copiedTuple);

		CHECK_FOR_INTERRUPTS();
	}

	index_endscan(scan);

	pfree(values);
	pfree(nulls);

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
		Oid			indexOID = lfirsto(indlist);
		Relation	oldIndex;
		IndexAttrs *attrs;

		oldIndex = index_open(indexOID);

		attrs = (IndexAttrs *) palloc(sizeof(IndexAttrs));
		attrs->indexOID = indexOID;
		attrs->indexName = pstrdup(NameStr(oldIndex->rd_rel->relname));
		attrs->accessMethodOID = oldIndex->rd_rel->relam;
		attrs->indexInfo = BuildIndexInfo(oldIndex);
		attrs->classOID = (Oid *)
			palloc(sizeof(Oid) * attrs->indexInfo->ii_NumIndexAttrs);
		memcpy(attrs->classOID, oldIndex->rd_index->indclass,
			   sizeof(Oid) * attrs->indexInfo->ii_NumIndexAttrs);
		/* We adjust the isclustered attribute to correct new state */
		attrs->isclustered = (indexOID == OldIndex);

		index_close(oldIndex);

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
rebuild_indexes(Oid OIDOldHeap, List *indexes)
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
		snprintf(newIndexName, sizeof(newIndexName),
				 "pg_temp_%u", attrs->indexOID);

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
		elog(ERROR, "cache lookup failed for relation %u", r1);
	relform1 = (Form_pg_class) GETSTRUCT(reltup1);

	reltup2 = SearchSysCacheCopy(RELOID,
								 ObjectIdGetDatum(r2),
								 0, 0, 0);
	if (!HeapTupleIsValid(reltup2))
		elog(ERROR, "cache lookup failed for relation %u", r2);
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
		elog(ERROR, "FlushRelationBuffers returned %d", i);
	relation_close(rel, NoLock);

	rel = relation_open(r2, NoLock);
	i = FlushRelationBuffers(rel, 0);
	if (i < 0)
		elog(ERROR, "FlushRelationBuffers returned %d", i);
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
	 * If we have toast tables associated with the relations being
	 * swapped, change their dependency links to re-associate them with
	 * their new owning relations.	Otherwise the wrong one will get
	 * dropped ...
	 *
	 * NOTE: it is possible that only one table has a toast table; this can
	 * happen in CLUSTER if there were dropped columns in the old table.
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

		/* Delete old dependencies */
		if (relform1->reltoastrelid)
		{
			count = deleteDependencyRecordsFor(RelOid_pg_class,
											   relform1->reltoastrelid);
			if (count != 1)
				elog(ERROR, "expected one dependency record for TOAST table, found %ld",
					 count);
		}
		if (relform2->reltoastrelid)
		{
			count = deleteDependencyRecordsFor(RelOid_pg_class,
											   relform2->reltoastrelid);
			if (count != 1)
				elog(ERROR, "expected one dependency record for TOAST table, found %ld",
					 count);
		}

		/* Register new dependencies */
		baseobject.classId = RelOid_pg_class;
		baseobject.objectSubId = 0;
		toastobject.classId = RelOid_pg_class;
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
	 * appropriate user. System relations or nailed-in relations cannot
	 * ever have indisclustered set, because CLUSTER will refuse to set it
	 * when called with one of them as argument.
	 */
	indRelation = relation_openr(IndexRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry, 0,
						   Anum_pg_index_indisclustered,
						   F_BOOLEQ,
						   BoolGetDatum(true));
	scan = heap_beginscan(indRelation, SnapshotNow, 1, &entry);
	while ((indexTuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		index = (Form_pg_index) GETSTRUCT(indexTuple);

		if (!pg_class_ownercheck(index->indrelid, GetUserId()))
			continue;

		/*
		 * We have to build the list in a different memory context so it
		 * will survive the cross-transaction processing
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
