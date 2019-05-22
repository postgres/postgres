/*-------------------------------------------------------------------------
 *
 * partdesc.h
 *
 * Copyright (c) 1996-2019, PostgreSQL Global Development Group
 *
 * src/include/utils/partdesc.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PARTDESC_H
#define PARTDESC_H

#include "partitioning/partdefs.h"
#include "utils/relcache.h"

/*
 * Information about partitions of a partitioned table.
 */
typedef struct PartitionDescData
{
	int			nparts;			/* Number of partitions */
	Oid		   *oids;			/* Array of 'nparts' elements containing
								 * partition OIDs in order of the their bounds */
	bool	   *is_leaf;		/* Array of 'nparts' elements storing whether
								 * the corresponding 'oids' element belongs to
								 * a leaf partition or not */
	PartitionBoundInfo boundinfo;	/* collection of partition bounds */
} PartitionDescData;

extern void RelationBuildPartitionDesc(Relation rel);

extern PartitionDirectory CreatePartitionDirectory(MemoryContext mcxt);
extern PartitionDesc PartitionDirectoryLookup(PartitionDirectory, Relation);
extern void DestroyPartitionDirectory(PartitionDirectory pdir);

extern Oid	get_default_oid_from_partdesc(PartitionDesc partdesc);

extern bool equalPartitionDescs(PartitionKey key, PartitionDesc partdesc1,
								PartitionDesc partdesc2);

#endif							/* PARTCACHE_H */
