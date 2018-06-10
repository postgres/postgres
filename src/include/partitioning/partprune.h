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

#include "nodes/execnodes.h"
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
	 * This will be set when the context is used from the executor, to allow
	 * Params to be evaluated.
	 */
	PlanState  *planstate;

	/*
	 * Array of ExprStates, indexed as per PruneCtxStateIdx; one for each
	 * partkey in each pruning step.  Allocated if planstate is non-NULL,
	 * otherwise NULL.
	 */
	ExprState **exprstates;

	/*
	 * Similar array of flags, each true if corresponding 'exprstate'
	 * expression contains any PARAM_EXEC Params.  (Can be NULL if planstate
	 * is NULL.)
	 */
	bool	   *exprhasexecparam;

	/* true if it's safe to evaluate PARAM_EXEC Params */
	bool		evalexecparams;
} PartitionPruneContext;

#define PruneCxtStateIdx(partnatts, step_id, keyno) \
	((partnatts) * (step_id) + (keyno))

extern List *make_partition_pruneinfo(PlannerInfo *root, List *partition_rels,
						 List *subpaths, List *prunequal);
extern Relids prune_append_rel_partitions(RelOptInfo *rel);
extern Bitmapset *get_matching_partitions(PartitionPruneContext *context,
						List *pruning_steps);

#endif							/* PARTPRUNE_H */
