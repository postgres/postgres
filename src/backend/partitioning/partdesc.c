/*-------------------------------------------------------------------------
 *
 * partdesc.c
 *		Support routines for manipulating partition descriptors
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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
#include "storage/bufmgr.h"
#include "storage/sinval.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

typedef struct PartitionDirectoryData
{
	MemoryContext pdir_mcxt;
	HTAB	   *pdir_hash;
}			PartitionDirectoryData;

typedef struct PartitionDirectoryEntry
{
	Oid			reloid;
	Relation	rel;
	PartitionDesc pd;
} PartitionDirectoryEntry;

static void RelationBuildPartitionDesc(Relation rel);


/*
 * RelationGetPartitionDesc -- get partition descriptor, if relation is partitioned
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
RelationGetPartitionDesc(Relation rel)
{
	if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		return NULL;

	if (unlikely(rel->rd_partdesc == NULL))
		RelationBuildPartitionDesc(rel);

	return rel->rd_partdesc;
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
 */
static void
RelationBuildPartitionDesc(Relation rel)
{
	PartitionDesc partdesc;
	PartitionBoundInfo boundinfo = NULL;
	List	   *inhoids;
	PartitionBoundSpec **boundspecs = NULL;
	Oid		   *oids = NULL;
	bool	   *is_leaf = NULL;
	ListCell   *cell;
	int			i,
				nparts;
	PartitionKey key = RelationGetPartitionKey(rel);
	MemoryContext new_pdcxt;
	MemoryContext oldcxt;
	int		   *mapping;

	/*
	 * Get partition oids from pg_inherits.  This uses a single snapshot to
	 * fetch the list of children, so while more children may be getting added
	 * concurrently, whatever this function returns will be accurate as of
	 * some well-defined point in time.
	 */
	inhoids = find_inheritance_children(RelationGetRelid(rel), NoLock);
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
		tuple = SearchSysCache1(RELOID, inhrelid);
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
		 * The system cache may be out of date; if so, we may find no pg_class
		 * tuple or an old one where relpartbound is NULL.  In that case, try
		 * the table directly.  We can't just AcceptInvalidationMessages() and
		 * retry the system cache lookup because it's possible that a
		 * concurrent ATTACH PARTITION operation has removed itself from the
		 * ProcArray but not yet added invalidation messages to the shared
		 * queue; InvalidateSystemCaches() would work, but seems excessive.
		 *
		 * Note that this algorithm assumes that PartitionBoundSpec we manage
		 * to fetch is the right one -- so this is only good enough for
		 * concurrent ATTACH PARTITION, not concurrent DETACH PARTITION or
		 * some hypothetical operation that changes the partition bounds.
		 */
		if (boundspec == NULL)
		{
			Relation	pg_class;
			SysScanDesc scan;
			ScanKeyData key[1];
			Datum		datum;
			bool		isnull;

			pg_class = table_open(RelationRelationId, AccessShareLock);
			ScanKeyInit(&key[0],
						Anum_pg_class_oid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(inhrelid));
			scan = systable_beginscan(pg_class, ClassOidIndexId, true,
									  NULL, 1, key);
			tuple = systable_getnext(scan);
			datum = heap_getattr(tuple, Anum_pg_class_relpartbound,
								 RelationGetDescr(pg_class), &isnull);
			if (!isnull)
				boundspec = stringToNode(TextDatumGetCString(datum));
			systable_endscan(scan);
			table_close(pg_class, AccessShareLock);
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
	/* If there are no partitions, the rest of the partdesc can stay zero */
	if (nparts > 0)
	{
		oldcxt = MemoryContextSwitchTo(new_pdcxt);
		partdesc->boundinfo = partition_bounds_copy(boundinfo, key);
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
	 * We have a fully valid partdesc ready to store into the relcache.
	 * Reparent it so it has the right lifespan.
	 */
	MemoryContextSetParent(new_pdcxt, CacheMemoryContext);

	/*
	 * But first, a kluge: if there's an old rd_pdcxt, it contains an old
	 * partition descriptor that may still be referenced somewhere.  Preserve
	 * it, while not leaking it, by reattaching it as a child context of the
	 * new rd_pdcxt.  Eventually it will get dropped by either RelationClose
	 * or RelationClearRelation.
	 */
	if (rel->rd_pdcxt != NULL)
		MemoryContextSetParent(rel->rd_pdcxt, new_pdcxt);
	rel->rd_pdcxt = new_pdcxt;
	rel->rd_partdesc = partdesc;
}

/*
 * CreatePartitionDirectory
 *		Create a new partition directory object.
 */
PartitionDirectory
CreatePartitionDirectory(MemoryContext mcxt)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(mcxt);
	PartitionDirectory pdir;
	HASHCTL		ctl;

	MemSet(&ctl, 0, sizeof(HASHCTL));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(PartitionDirectoryEntry);
	ctl.hcxt = mcxt;

	pdir = palloc(sizeof(PartitionDirectoryData));
	pdir->pdir_mcxt = mcxt;
	pdir->pdir_hash = hash_create("partition directory", 256, &ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

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
		pde->pd = RelationGetPartitionDesc(rel);
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
