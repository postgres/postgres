/*-------------------------------------------------------------------------
 *
 * partdesc.h
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/partitioning/partdesc.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PARTDESC_H
#define PARTDESC_H

#include "partitioning/partdefs.h"
#include "utils/relcache.h"

/*
 * Information about partitions of a partitioned table.
 *
 * For partitioned tables where detached partitions exist, we only cache
 * descriptors that include all partitions, including detached; when we're
 * requested a descriptor without the detached partitions, we create one
 * afresh each time.  (The reason for this is that the set of detached
 * partitions that are visible to each caller depends on the snapshot it has,
 * so it's pretty much impossible to evict a descriptor from cache at the
 * right time.)
 */
typedef struct PartitionDescData
{
	int			nparts;			/* Number of partitions */
	bool		detached_exist; /* Are there any detached partitions? */
	Oid		   *oids;			/* Array of 'nparts' elements containing
								 * partition OIDs in order of their bounds */
	bool	   *is_leaf;		/* Array of 'nparts' elements storing whether
								 * the corresponding 'oids' element belongs to
								 * a leaf partition or not */
	PartitionBoundInfo boundinfo;	/* collection of partition bounds */

	/* Caching fields to cache lookups in get_partition_for_tuple() */

	/*
	 * Index into the PartitionBoundInfo's datum array for the last found
	 * partition or -1 if none.
	 */
	int			last_found_datum_index;

	/*
	 * Partition index of the last found partition or -1 if none has been
	 * found yet.
	 */
	int			last_found_part_index;

	/*
	 * For LIST partitioning, this is the number of times in a row that the
	 * datum we're looking for a partition for matches the datum in the
	 * last_found_datum_index index of the boundinfo->datums array.  For RANGE
	 * partitioning, this is the number of times in a row we've found that the
	 * datum we're looking for a partition for falls into the range of the
	 * partition corresponding to the last_found_datum_index index of the
	 * boundinfo->datums array.
	 */
	int			last_found_count;
} PartitionDescData;


extern PartitionDesc RelationGetPartitionDesc(Relation rel, bool omit_detached);

extern PartitionDirectory CreatePartitionDirectory(MemoryContext mcxt, bool omit_detached);
extern PartitionDesc PartitionDirectoryLookup(PartitionDirectory, Relation);
extern void DestroyPartitionDirectory(PartitionDirectory pdir);

extern Oid	get_default_oid_from_partdesc(PartitionDesc partdesc);

#endif							/* PARTCACHE_H */
