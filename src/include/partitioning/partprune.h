/*-------------------------------------------------------------------------
 *
 * partprune.h
 *	  prototypes for partprune.c
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/partitioning/partprune.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARTPRUNE_H
#define PARTPRUNE_H

#include "catalog/partition.h"
#include "nodes/relation.h"

/*
 * PartitionPruneContext
 *
 * Information about a partitioned table needed to perform partition pruning.
 */
typedef struct PartitionPruneContext
{
	/* Partition key information */
	char		strategy;
	int			partnatts;
	Oid		   *partopfamily;
	Oid		   *partopcintype;
	Oid		   *partcollation;
	FmgrInfo   *partsupfunc;

	/* Number of partitions */
	int			nparts;

	/* Partition boundary info */
	PartitionBoundInfo boundinfo;

	/*
	 * Can be set when the context is used from the executor to allow params
	 * found matching the partition key to be evaulated.
	 */
	PlanState  *planstate;

	/*
	 * Parameters that are safe to be used for partition pruning. execparams
	 * are not safe to use until the executor is running.
	 */
	Bitmapset  *safeparams;
} PartitionPruneContext;


extern List *make_partition_pruneinfo(PlannerInfo *root, List *partition_rels,
						 List *subpaths, List *prunequal);
extern Relids prune_append_rel_partitions(RelOptInfo *rel);
extern Bitmapset *get_matching_partitions(PartitionPruneContext *context,
						List *pruning_steps);
extern List *gen_partprune_steps(RelOptInfo *rel, List *clauses,
					bool *contradictory);

#endif							/* PARTPRUNE_H */
