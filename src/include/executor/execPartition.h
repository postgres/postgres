/*--------------------------------------------------------------------
 * execPartition.h
 *		POSTGRES partitioning executor interface
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/executor/execPartition.h
 *--------------------------------------------------------------------
 */

#ifndef EXECPARTITION_H
#define EXECPARTITION_H

#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "partitioning/partprune.h"

/* See execPartition.c for the definitions. */
typedef struct PartitionDispatchData *PartitionDispatch;
typedef struct PartitionTupleRouting PartitionTupleRouting;

extern PartitionTupleRouting *ExecSetupPartitionTupleRouting(EState *estate,
															 Relation rel);
extern ResultRelInfo *ExecFindPartition(ModifyTableState *mtstate,
										ResultRelInfo *rootResultRelInfo,
										PartitionTupleRouting *proute,
										TupleTableSlot *slot,
										EState *estate);
extern void ExecCleanupTupleRouting(ModifyTableState *mtstate,
									PartitionTupleRouting *proute);


/*
 * PartitionedRelPruningData - Per-partitioned-table data for run-time pruning
 * of partitions.  For a multilevel partitioned table, we have one of these
 * for the topmost partition plus one for each non-leaf child partition.
 *
 * subplan_map[] and subpart_map[] have the same definitions as in
 * PartitionedRelPruneInfo (see plannodes.h); though note that here,
 * subpart_map contains indexes into PartitionPruningData.partrelprunedata[].
 *
 * nparts						Length of subplan_map[] and subpart_map[].
 * subplan_map					Subplan index by partition index, or -1.
 * subpart_map					Subpart index by partition index, or -1.
 * present_parts				A Bitmapset of the partition indexes that we
 *								have subplans or subparts for.
 * initial_pruning_steps		List of PartitionPruneSteps used to
 *								perform executor startup pruning.
 * exec_pruning_steps			List of PartitionPruneSteps used to
 *								perform per-scan pruning.
 * initial_context				If initial_pruning_steps isn't NIL, contains
 *								the details needed to execute those steps.
 * exec_context					If exec_pruning_steps isn't NIL, contains
 *								the details needed to execute those steps.
 */
typedef struct PartitionedRelPruningData
{
	int			nparts;
	int		   *subplan_map;
	int		   *subpart_map;
	Bitmapset  *present_parts;
	List	   *initial_pruning_steps;
	List	   *exec_pruning_steps;
	PartitionPruneContext initial_context;
	PartitionPruneContext exec_context;
} PartitionedRelPruningData;

/*
 * PartitionPruningData - Holds all the run-time pruning information for
 * a single partitioning hierarchy containing one or more partitions.
 * partrelprunedata[] is an array ordered such that parents appear before
 * their children; in particular, the first entry is the topmost partition,
 * which was actually named in the SQL query.
 */
typedef struct PartitionPruningData
{
	int			num_partrelprunedata;	/* number of array entries */
	PartitionedRelPruningData partrelprunedata[FLEXIBLE_ARRAY_MEMBER];
} PartitionPruningData;

/*
 * PartitionPruneState - State object required for plan nodes to perform
 * run-time partition pruning.
 *
 * This struct can be attached to plan types which support arbitrary Lists of
 * subplans containing partitions, to allow subplans to be eliminated due to
 * the clauses being unable to match to any tuple that the subplan could
 * possibly produce.
 *
 * execparamids			Contains paramids of PARAM_EXEC Params found within
 *						any of the partprunedata structs.  Pruning must be
 *						done again each time the value of one of these
 *						parameters changes.
 * other_subplans		Contains indexes of subplans that don't belong to any
 *						"partprunedata", e.g UNION ALL children that are not
 *						partitioned tables, or a partitioned table that the
 *						planner deemed run-time pruning to be useless for.
 *						These must not be pruned.
 * prune_context		A short-lived memory context in which to execute the
 *						partition pruning functions.
 * do_initial_prune		true if pruning should be performed during executor
 *						startup (at any hierarchy level).
 * do_exec_prune		true if pruning should be performed during
 *						executor run (at any hierarchy level).
 * num_partprunedata	Number of items in "partprunedata" array.
 * partprunedata		Array of PartitionPruningData pointers for the plan's
 *						partitioned relation(s), one for each partitioning
 *						hierarchy that requires run-time pruning.
 */
typedef struct PartitionPruneState
{
	Bitmapset  *execparamids;
	Bitmapset  *other_subplans;
	MemoryContext prune_context;
	bool		do_initial_prune;
	bool		do_exec_prune;
	int			num_partprunedata;
	PartitionPruningData *partprunedata[FLEXIBLE_ARRAY_MEMBER];
} PartitionPruneState;

extern PartitionPruneState *ExecInitPartitionPruning(PlanState *planstate,
													 int n_total_subplans,
													 PartitionPruneInfo *pruneinfo,
													 Bitmapset **initially_valid_subplans);
extern Bitmapset *ExecFindMatchingSubPlans(PartitionPruneState *prunestate,
										   bool initial_prune);

#endif							/* EXECPARTITION_H */
