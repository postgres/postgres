/*-------------------------------------------------------------------------
 *
 * partdesc.h
 *
 * Copyright (c) 1996-2021, PostgreSQL Global Development Group
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
 */
typedef struct PartitionDescData
{
	int			nparts;			/* Number of partitions */
	bool		includes_detached;	/* Does it include detached partitions */
	Oid		   *oids;			/* Array of 'nparts' elements containing
								 * partition OIDs in order of the their bounds */
	bool	   *is_leaf;		/* Array of 'nparts' elements storing whether
								 * the corresponding 'oids' element belongs to
								 * a leaf partition or not */
	PartitionBoundInfo boundinfo;	/* collection of partition bounds */
} PartitionDescData;


extern PartitionDesc RelationGetPartitionDesc(Relation rel, bool include_detached);

extern PartitionDirectory CreatePartitionDirectory(MemoryContext mcxt, bool include_detached);
extern PartitionDesc PartitionDirectoryLookup(PartitionDirectory, Relation);
extern void DestroyPartitionDirectory(PartitionDirectory pdir);

extern Oid	get_default_oid_from_partdesc(PartitionDesc partdesc);

#endif							/* PARTCACHE_H */
