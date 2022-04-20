/*-------------------------------------------------------------------------
 *
 * partdesc.h
 *
 * Copyright (c) 1996-2022, PostgreSQL Global Development Group
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
								 * partition OIDs in order of the their bounds */
	bool	   *is_leaf;		/* Array of 'nparts' elements storing whether
								 * the corresponding 'oids' element belongs to
								 * a leaf partition or not */
	PartitionBoundInfo boundinfo;	/* collection of partition bounds */
} PartitionDescData;


extern PartitionDesc RelationGetPartitionDesc(Relation rel, bool omit_detached);

extern PartitionDirectory CreatePartitionDirectory(MemoryContext mcxt, bool omit_detached);
extern PartitionDesc PartitionDirectoryLookup(PartitionDirectory, Relation);
extern void DestroyPartitionDirectory(PartitionDirectory pdir);

extern Oid	get_default_oid_from_partdesc(PartitionDesc partdesc);

#endif							/* PARTCACHE_H */
