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
} PartitionPruneContext;


extern Relids prune_append_rel_partitions(RelOptInfo *rel);
extern Bitmapset *get_matching_partitions(PartitionPruneContext *context,
						List *pruning_steps);
extern List *gen_partprune_steps(RelOptInfo *rel, List *clauses,
					bool *contradictory);

#endif							/* PARTPRUNE_H */
