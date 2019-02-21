/*-------------------------------------------------------------------------
 *
 * partdesc.c
 *		Support routines for manipulating partition descriptors
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/backend/partitioning/partdesc.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/partcache.h"
#include "utils/syscache.h"

/*
 * RelationBuildPartitionDesc
 *		Form rel's partition descriptor
 *
 * Not flushed from the cache by RelationClearRelation() unless changed because
 * of addition or removal of partition.
 */
void
RelationBuildPartitionDesc(Relation rel)
{
	PartitionDesc partdesc;
	PartitionBoundInfo boundinfo = NULL;
	List	   *inhoids;
	PartitionBoundSpec **boundspecs = NULL;
	Oid		   *oids = NULL;
	ListCell   *cell;
	int			i,
				nparts;
	PartitionKey key = RelationGetPartitionKey(rel);
	MemoryContext oldcxt;
	int		   *mapping;

	/* Get partition oids from pg_inherits */
	inhoids = find_inheritance_children(RelationGetRelid(rel), NoLock);
	nparts = list_length(inhoids);

	if (nparts > 0)
	{
		oids = palloc(nparts * sizeof(Oid));
		boundspecs = palloc(nparts * sizeof(PartitionBoundSpec *));
	}

	/* Collect bound spec nodes for each partition */
	i = 0;
	foreach(cell, inhoids)
	{
		Oid			inhrelid = lfirst_oid(cell);
		HeapTuple	tuple;
		Datum		datum;
		bool		isnull;
		PartitionBoundSpec *boundspec;

		tuple = SearchSysCache1(RELOID, inhrelid);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", inhrelid);

		datum = SysCacheGetAttr(RELOID, tuple,
								Anum_pg_class_relpartbound,
								&isnull);
		if (isnull)
			elog(ERROR, "null relpartbound for relation %u", inhrelid);
		boundspec = stringToNode(TextDatumGetCString(datum));
		if (!IsA(boundspec, PartitionBoundSpec))
			elog(ERROR, "invalid relpartbound for relation %u", inhrelid);

		/*
		 * Sanity check: If the PartitionBoundSpec says this is the default
		 * partition, its OID should correspond to whatever's stored in
		 * pg_partitioned_table.partdefid; if not, the catalog is corrupt.
		 */
		if (boundspec->is_default)
		{
			Oid			partdefid;

			partdefid = get_default_partition_oid(RelationGetRelid(rel));
			if (partdefid != inhrelid)
				elog(ERROR, "expected partdefid %u, but got %u",
					 inhrelid, partdefid);
		}

		oids[i] = inhrelid;
		boundspecs[i] = boundspec;
		++i;
		ReleaseSysCache(tuple);
	}

	/* Now build the actual relcache partition descriptor */
	rel->rd_pdcxt = AllocSetContextCreate(CacheMemoryContext,
										  "partition descriptor",
										  ALLOCSET_DEFAULT_SIZES);
	MemoryContextCopyAndSetIdentifier(rel->rd_pdcxt,
									  RelationGetRelationName(rel));

	oldcxt = MemoryContextSwitchTo(rel->rd_pdcxt);
	partdesc = (PartitionDescData *) palloc0(sizeof(PartitionDescData));
	partdesc->nparts = nparts;
	/* oids and boundinfo are allocated below. */

	MemoryContextSwitchTo(oldcxt);

	if (nparts == 0)
	{
		rel->rd_partdesc = partdesc;
		return;
	}

	/* First create PartitionBoundInfo */
	boundinfo = partition_bounds_create(boundspecs, nparts, key, &mapping);

	/* Now copy boundinfo and oids into partdesc. */
	oldcxt = MemoryContextSwitchTo(rel->rd_pdcxt);
	partdesc->boundinfo = partition_bounds_copy(boundinfo, key);
	partdesc->oids = (Oid *) palloc(partdesc->nparts * sizeof(Oid));
	partdesc->is_leaf = (bool *) palloc(partdesc->nparts * sizeof(bool));

	/*
	 * Now assign OIDs from the original array into mapped indexes of the
	 * result array.  The order of OIDs in the former is defined by the
	 * catalog scan that retrieved them, whereas that in the latter is defined
	 * by canonicalized representation of the partition bounds.
	 */
	for (i = 0; i < partdesc->nparts; i++)
	{
		int			index = mapping[i];

		partdesc->oids[index] = oids[i];
		/* Record if the partition is a leaf partition */
		partdesc->is_leaf[index] =
				(get_rel_relkind(oids[i]) != RELKIND_PARTITIONED_TABLE);
	}
	MemoryContextSwitchTo(oldcxt);

	rel->rd_partdesc = partdesc;
}

/*
 * equalPartitionDescs
 *		Compare two partition descriptors for logical equality
 */
bool
equalPartitionDescs(PartitionKey key, PartitionDesc partdesc1,
					PartitionDesc partdesc2)
{
	int			i;

	if (partdesc1 != NULL)
	{
		if (partdesc2 == NULL)
			return false;
		if (partdesc1->nparts != partdesc2->nparts)
			return false;

		Assert(key != NULL || partdesc1->nparts == 0);

		/*
		 * Same oids? If the partitioning structure did not change, that is,
		 * no partitions were added or removed to the relation, the oids array
		 * should still match element-by-element.
		 */
		for (i = 0; i < partdesc1->nparts; i++)
		{
			if (partdesc1->oids[i] != partdesc2->oids[i])
				return false;
		}

		/*
		 * Now compare partition bound collections.  The logic to iterate over
		 * the collections is private to partition.c.
		 */
		if (partdesc1->boundinfo != NULL)
		{
			if (partdesc2->boundinfo == NULL)
				return false;

			if (!partition_bounds_equal(key->partnatts, key->parttyplen,
										key->parttypbyval,
										partdesc1->boundinfo,
										partdesc2->boundinfo))
				return false;
		}
		else if (partdesc2->boundinfo != NULL)
			return false;
	}
	else if (partdesc2 != NULL)
		return false;

	return true;
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
