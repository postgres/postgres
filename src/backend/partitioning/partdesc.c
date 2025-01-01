/*-------------------------------------------------------------------------
 *
 * partdesc.c
 *		Support routines for manipulating partition descriptors
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/backend/partitioning/partdesc.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

typedef struct PartitionDirectoryData
{
	MemoryContext pdir_mcxt;
	HTAB	   *pdir_hash;
	bool		omit_detached;
}			PartitionDirectoryData;

typedef struct PartitionDirectoryEntry
{
	Oid			reloid;
	Relation	rel;
	PartitionDesc pd;
} PartitionDirectoryEntry;

static PartitionDesc RelationBuildPartitionDesc(Relation rel,
												bool omit_detached);


/*
 * RelationGetPartitionDesc -- get partition descriptor, if relation is partitioned
 *
 * We keep two partdescs in relcache: rd_partdesc includes all partitions
 * (even those being concurrently marked detached), while rd_partdesc_nodetached
 * omits (some of) those.  We store the pg_inherits.xmin value for the latter,
 * to determine whether it can be validly reused in each case, since that
 * depends on the active snapshot.
 *
 * Note: we arrange for partition descriptors to not get freed until the
 * relcache entry's refcount goes to zero (see hacks in RelationClose,
 * RelationClearRelation, and RelationBuildPartitionDesc).  Therefore, even
 * though we hand back a direct pointer into the relcache entry, it's safe
 * for callers to continue to use that pointer as long as (a) they hold the
 * relation open, and (b) they hold a relation lock strong enough to ensure
 * that the data doesn't become stale.
 */
PartitionDesc
RelationGetPartitionDesc(Relation rel, bool omit_detached)
{
	Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	/*
	 * If relcache has a partition descriptor, use that.  However, we can only
	 * do so when we are asked to include all partitions including detached;
	 * and also when we know that there are no detached partitions.
	 *
	 * If there is no active snapshot, detached partitions aren't omitted
	 * either, so we can use the cached descriptor too in that case.
	 */
	if (likely(rel->rd_partdesc &&
			   (!rel->rd_partdesc->detached_exist || !omit_detached ||
				!ActiveSnapshotSet())))
		return rel->rd_partdesc;

	/*
	 * If we're asked to omit detached partitions, we may be able to use a
	 * cached descriptor too.  We determine that based on the pg_inherits.xmin
	 * that was saved alongside that descriptor: if the xmin that was not in
	 * progress for that active snapshot is also not in progress for the
	 * current active snapshot, then we can use it.  Otherwise build one from
	 * scratch.
	 */
	if (omit_detached &&
		rel->rd_partdesc_nodetached &&
		ActiveSnapshotSet())
	{
		Snapshot	activesnap;

		Assert(TransactionIdIsValid(rel->rd_partdesc_nodetached_xmin));
		activesnap = GetActiveSnapshot();

		if (!XidInMVCCSnapshot(rel->rd_partdesc_nodetached_xmin, activesnap))
			return rel->rd_partdesc_nodetached;
	}

	return RelationBuildPartitionDesc(rel, omit_detached);
}

/*
 * RelationBuildPartitionDesc
 *		Form rel's partition descriptor, and store in relcache entry
 *
 * Partition descriptor is a complex structure; to avoid complicated logic to
 * free individual elements whenever the relcache entry is flushed, we give it
 * its own memory context, a child of CacheMemoryContext, which can easily be
 * deleted on its own.  To avoid leaking memory in that context in case of an
 * error partway through this function, the context is initially created as a
 * child of CurTransactionContext and only re-parented to CacheMemoryContext
 * at the end, when no further errors are possible.  Also, we don't make this
 * context the current context except in very brief code sections, out of fear
 * that some of our callees allocate memory on their own which would be leaked
 * permanently.
 *
 * As a special case, partition descriptors that are requested to omit
 * partitions being detached (and which contain such partitions) are transient
 * and are not associated with the relcache entry.  Such descriptors only last
 * through the requesting Portal, so we use the corresponding memory context
 * for them.
 */
static PartitionDesc
RelationBuildPartitionDesc(Relation rel, bool omit_detached)
{
	PartitionDesc partdesc;
	PartitionBoundInfo boundinfo = NULL;
	List	   *inhoids;
	PartitionBoundSpec **boundspecs = NULL;
	Oid		   *oids = NULL;
	bool	   *is_leaf = NULL;
	bool		detached_exist;
	bool		is_omit;
	TransactionId detached_xmin;
	ListCell   *cell;
	int			i,
				nparts;
	bool		retried = false;
	PartitionKey key = RelationGetPartitionKey(rel);
	MemoryContext new_pdcxt;
	MemoryContext oldcxt;
	int		   *mapping;

retry:

	/*
	 * Get partition oids from pg_inherits.  This uses a single snapshot to
	 * fetch the list of children, so while more children may be getting added
	 * or removed concurrently, whatever this function returns will be
	 * accurate as of some well-defined point in time.
	 */
	detached_exist = false;
	detached_xmin = InvalidTransactionId;
	inhoids = find_inheritance_children_extended(RelationGetRelid(rel),
												 omit_detached, NoLock,
												 &detached_exist,
												 &detached_xmin);

	nparts = list_length(inhoids);

	/* Allocate working arrays for OIDs, leaf flags, and boundspecs. */
	if (nparts > 0)
	{
		oids = (Oid *) palloc(nparts * sizeof(Oid));
		is_leaf = (bool *) palloc(nparts * sizeof(bool));
		boundspecs = palloc(nparts * sizeof(PartitionBoundSpec *));
	}

	/* Collect bound spec nodes for each partition. */
	i = 0;
	foreach(cell, inhoids)
	{
		Oid			inhrelid = lfirst_oid(cell);
		HeapTuple	tuple;
		PartitionBoundSpec *boundspec = NULL;

		/* Try fetching the tuple from the catcache, for speed. */
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(inhrelid));
		if (HeapTupleIsValid(tuple))
		{
			Datum		datum;
			bool		isnull;

			datum = SysCacheGetAttr(RELOID, tuple,
									Anum_pg_class_relpartbound,
									&isnull);
			if (!isnull)
				boundspec = stringToNode(TextDatumGetCString(datum));
			ReleaseSysCache(tuple);
		}

		/*
		 * Two problems are possible here.  First, a concurrent ATTACH
		 * PARTITION might be in the process of adding a new partition, but
		 * the syscache doesn't have it, or its copy of it does not yet have
		 * its relpartbound set.  We cannot just AcceptInvalidationMessages(),
		 * because the other process might have already removed itself from
		 * the ProcArray but not yet added its invalidation messages to the
		 * shared queue.  We solve this problem by reading pg_class directly
		 * for the desired tuple.
		 *
		 * If the partition recently detached is also dropped, we get no tuple
		 * from the scan.  In that case, we also retry, and next time through
		 * here, we don't see that partition anymore.
		 *
		 * The other problem is that DETACH CONCURRENTLY is in the process of
		 * removing a partition, which happens in two steps: first it marks it
		 * as "detach pending", commits, then unsets relpartbound.  If
		 * find_inheritance_children_extended included that partition but we
		 * below we see that DETACH CONCURRENTLY has reset relpartbound for
		 * it, we'd see an inconsistent view.  (The inconsistency is seen
		 * because table_open below reads invalidation messages.)  We protect
		 * against this by retrying find_inheritance_children_extended().
		 */
		if (boundspec == NULL)
		{
			Relation	pg_class;
			SysScanDesc scan;
			ScanKeyData key[1];

			pg_class = table_open(RelationRelationId, AccessShareLock);
			ScanKeyInit(&key[0],
						Anum_pg_class_oid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(inhrelid));
			scan = systable_beginscan(pg_class, ClassOidIndexId, true,
									  NULL, 1, key);

			/*
			 * We could get one tuple from the scan (the normal case), or zero
			 * tuples if the table has been dropped meanwhile.
			 */
			tuple = systable_getnext(scan);
			if (HeapTupleIsValid(tuple))
			{
				Datum		datum;
				bool		isnull;

				datum = heap_getattr(tuple, Anum_pg_class_relpartbound,
									 RelationGetDescr(pg_class), &isnull);
				if (!isnull)
					boundspec = stringToNode(TextDatumGetCString(datum));
			}
			systable_endscan(scan);
			table_close(pg_class, AccessShareLock);

			/*
			 * If we still don't get a relpartbound value (either because
			 * boundspec is null or because there was no tuple), then it must
			 * be because of DETACH CONCURRENTLY.  Restart from the top, as
			 * explained above.  We only do this once, for two reasons: first,
			 * only one DETACH CONCURRENTLY session could affect us at a time,
			 * since each of them would have to wait for the snapshot under
			 * which this is running; and second, to avoid possible infinite
			 * loops in case of catalog corruption.
			 *
			 * Note that the current memory context is short-lived enough, so
			 * we needn't worry about memory leaks here.
			 */
			if (!boundspec && !retried)
			{
				AcceptInvalidationMessages();
				retried = true;
				goto retry;
			}
		}

		/* Sanity checks. */
		if (!boundspec)
			elog(ERROR, "missing relpartbound for relation %u", inhrelid);
		if (!IsA(boundspec, PartitionBoundSpec))
			elog(ERROR, "invalid relpartbound for relation %u", inhrelid);

		/*
		 * If the PartitionBoundSpec says this is the default partition, its
		 * OID should match pg_partitioned_table.partdefid; if not, the
		 * catalog is corrupt.
		 */
		if (boundspec->is_default)
		{
			Oid			partdefid;

			partdefid = get_default_partition_oid(RelationGetRelid(rel));
			if (partdefid != inhrelid)
				elog(ERROR, "expected partdefid %u, but got %u",
					 inhrelid, partdefid);
		}

		/* Save results. */
		oids[i] = inhrelid;
		is_leaf[i] = (get_rel_relkind(inhrelid) != RELKIND_PARTITIONED_TABLE);
		boundspecs[i] = boundspec;
		++i;
	}

	/*
	 * Create PartitionBoundInfo and mapping, working in the caller's context.
	 * This could fail, but we haven't done any damage if so.
	 */
	if (nparts > 0)
		boundinfo = partition_bounds_create(boundspecs, nparts, key, &mapping);

	/*
	 * Now build the actual relcache partition descriptor, copying all the
	 * data into a new, small context.  As per above comment, we don't make
	 * this a long-lived context until it's finished.
	 */
	new_pdcxt = AllocSetContextCreate(CurTransactionContext,
									  "partition descriptor",
									  ALLOCSET_SMALL_SIZES);
	MemoryContextCopyAndSetIdentifier(new_pdcxt,
									  RelationGetRelationName(rel));

	partdesc = (PartitionDescData *)
		MemoryContextAllocZero(new_pdcxt, sizeof(PartitionDescData));
	partdesc->nparts = nparts;
	partdesc->detached_exist = detached_exist;
	/* If there are no partitions, the rest of the partdesc can stay zero */
	if (nparts > 0)
	{
		oldcxt = MemoryContextSwitchTo(new_pdcxt);
		partdesc->boundinfo = partition_bounds_copy(boundinfo, key);

		/* Initialize caching fields for speeding up ExecFindPartition */
		partdesc->last_found_datum_index = -1;
		partdesc->last_found_part_index = -1;
		partdesc->last_found_count = 0;

		partdesc->oids = (Oid *) palloc(nparts * sizeof(Oid));
		partdesc->is_leaf = (bool *) palloc(nparts * sizeof(bool));

		/*
		 * Assign OIDs from the original array into mapped indexes of the
		 * result array.  The order of OIDs in the former is defined by the
		 * catalog scan that retrieved them, whereas that in the latter is
		 * defined by canonicalized representation of the partition bounds.
		 * Also save leaf-ness of each partition.
		 */
		for (i = 0; i < nparts; i++)
		{
			int			index = mapping[i];

			partdesc->oids[index] = oids[i];
			partdesc->is_leaf[index] = is_leaf[i];
		}
		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * Are we working with the partdesc that omits the detached partition, or
	 * the one that includes it?
	 *
	 * Note that if a partition was found by the catalog's scan to have been
	 * detached, but the pg_inherit tuple saying so was not visible to the
	 * active snapshot (find_inheritance_children_extended will not have set
	 * detached_xmin in that case), we consider there to be no "omittable"
	 * detached partitions.
	 */
	is_omit = omit_detached && detached_exist && ActiveSnapshotSet() &&
		TransactionIdIsValid(detached_xmin);

	/*
	 * We have a fully valid partdesc.  Reparent it so that it has the right
	 * lifespan.
	 */
	MemoryContextSetParent(new_pdcxt, CacheMemoryContext);

	/*
	 * Store it into relcache.
	 *
	 * But first, a kluge: if there's an old context for this type of
	 * descriptor, it contains an old partition descriptor that may still be
	 * referenced somewhere.  Preserve it, while not leaking it, by
	 * reattaching it as a child context of the new one.  Eventually it will
	 * get dropped by either RelationClose or RelationClearRelation. (We keep
	 * the regular partdesc in rd_pdcxt, and the partdesc-excluding-
	 * detached-partitions in rd_pddcxt.)
	 */
	if (is_omit)
	{
		if (rel->rd_pddcxt != NULL)
			MemoryContextSetParent(rel->rd_pddcxt, new_pdcxt);
		rel->rd_pddcxt = new_pdcxt;
		rel->rd_partdesc_nodetached = partdesc;

		/*
		 * For partdescs built excluding detached partitions, which we save
		 * separately, we also record the pg_inherits.xmin of the detached
		 * partition that was omitted; this informs a future potential user of
		 * such a cached partdesc to only use it after cross-checking that the
		 * xmin is indeed visible to the snapshot it is going to be working
		 * with.
		 */
		Assert(TransactionIdIsValid(detached_xmin));
		rel->rd_partdesc_nodetached_xmin = detached_xmin;
	}
	else
	{
		if (rel->rd_pdcxt != NULL)
			MemoryContextSetParent(rel->rd_pdcxt, new_pdcxt);
		rel->rd_pdcxt = new_pdcxt;
		rel->rd_partdesc = partdesc;
	}

	return partdesc;
}

/*
 * CreatePartitionDirectory
 *		Create a new partition directory object.
 */
PartitionDirectory
CreatePartitionDirectory(MemoryContext mcxt, bool omit_detached)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(mcxt);
	PartitionDirectory pdir;
	HASHCTL		ctl;

	pdir = palloc(sizeof(PartitionDirectoryData));
	pdir->pdir_mcxt = mcxt;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(PartitionDirectoryEntry);
	ctl.hcxt = mcxt;

	pdir->pdir_hash = hash_create("partition directory", 256, &ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	pdir->omit_detached = omit_detached;

	MemoryContextSwitchTo(oldcontext);
	return pdir;
}

/*
 * PartitionDirectoryLookup
 *		Look up the partition descriptor for a relation in the directory.
 *
 * The purpose of this function is to ensure that we get the same
 * PartitionDesc for each relation every time we look it up.  In the
 * face of concurrent DDL, different PartitionDescs may be constructed with
 * different views of the catalog state, but any single particular OID
 * will always get the same PartitionDesc for as long as the same
 * PartitionDirectory is used.
 */
PartitionDesc
PartitionDirectoryLookup(PartitionDirectory pdir, Relation rel)
{
	PartitionDirectoryEntry *pde;
	Oid			relid = RelationGetRelid(rel);
	bool		found;

	pde = hash_search(pdir->pdir_hash, &relid, HASH_ENTER, &found);
	if (!found)
	{
		/*
		 * We must keep a reference count on the relation so that the
		 * PartitionDesc to which we are pointing can't get destroyed.
		 */
		RelationIncrementReferenceCount(rel);
		pde->rel = rel;
		pde->pd = RelationGetPartitionDesc(rel, pdir->omit_detached);
		Assert(pde->pd != NULL);
	}
	return pde->pd;
}

/*
 * DestroyPartitionDirectory
 *		Destroy a partition directory.
 *
 * Release the reference counts we're holding.
 */
void
DestroyPartitionDirectory(PartitionDirectory pdir)
{
	HASH_SEQ_STATUS status;
	PartitionDirectoryEntry *pde;

	hash_seq_init(&status, pdir->pdir_hash);
	while ((pde = hash_seq_search(&status)) != NULL)
		RelationDecrementReferenceCount(pde->rel);
}

/*
 * get_default_oid_from_partdesc
 *
 * Given a partition descriptor, return the OID of the default partition, if
 * one exists; else, return InvalidOid.
 */
Oid
get_default_oid_from_partdesc(PartitionDesc partdesc)
{
	if (partdesc && partdesc->boundinfo &&
		partition_bound_has_default(partdesc->boundinfo))
		return partdesc->oids[partdesc->boundinfo->default_index];

	return InvalidOid;
}
