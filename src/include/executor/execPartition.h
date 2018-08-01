/*--------------------------------------------------------------------
 * execPartition.h
 *		POSTGRES partitioning executor interface
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
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

/*-----------------------
 * PartitionDispatch - information about one partitioned table in a partition
 * hierarchy required to route a tuple to one of its partitions
 *
 *	reldesc		Relation descriptor of the table
 *	key			Partition key information of the table
 *	keystate	Execution state required for expressions in the partition key
 *	partdesc	Partition descriptor of the table
 *	tupslot		A standalone TupleTableSlot initialized with this table's tuple
 *				descriptor
 *	tupmap		TupleConversionMap to convert from the parent's rowtype to
 *				this table's rowtype (when extracting the partition key of a
 *				tuple just before routing it through this table)
 *	indexes		Array with partdesc->nparts members (for details on what
 *				individual members represent, see how they are set in
 *				get_partition_dispatch_recurse())
 *-----------------------
 */
typedef struct PartitionDispatchData
{
	Relation	reldesc;
	PartitionKey key;
	List	   *keystate;		/* list of ExprState */
	PartitionDesc partdesc;
	TupleTableSlot *tupslot;
	TupleConversionMap *tupmap;
	int		   *indexes;
} PartitionDispatchData;

typedef struct PartitionDispatchData *PartitionDispatch;

/*-----------------------
 * PartitionTupleRouting - Encapsulates all information required to execute
 * tuple-routing between partitions.
 *
 * partition_dispatch_info		Array of PartitionDispatch objects with one
 *								entry for every partitioned table in the
 *								partition tree.
 * num_dispatch					number of partitioned tables in the partition
 *								tree (= length of partition_dispatch_info[])
 * partition_oids				Array of leaf partitions OIDs with one entry
 *								for every leaf partition in the partition tree,
 *								initialized in full by
 *								ExecSetupPartitionTupleRouting.
 * partitions					Array of ResultRelInfo* objects with one entry
 *								for every leaf partition in the partition tree,
 *								initialized lazily by ExecInitPartitionInfo.
 * num_partitions				Number of leaf partitions in the partition tree
 *								(= 'partitions_oid'/'partitions' array length)
 * parent_child_tupconv_maps	Array of TupleConversionMap objects with one
 *								entry for every leaf partition (required to
 *								convert tuple from the root table's rowtype to
 *								a leaf partition's rowtype after tuple routing
 *								is done)
 * child_parent_tupconv_maps	Array of TupleConversionMap objects with one
 *								entry for every leaf partition (required to
 *								convert an updated tuple from the leaf
 *								partition's rowtype to the root table's rowtype
 *								so that tuple routing can be done)
 * child_parent_map_not_required  Array of bool. True value means that a map is
 *								determined to be not required for the given
 *								partition. False means either we haven't yet
 *								checked if a map is required, or it was
 *								determined to be required.
 * subplan_partition_offsets	Integer array ordered by UPDATE subplans. Each
 *								element of this array has the index into the
 *								corresponding partition in partitions array.
 * num_subplan_partition_offsets  Length of 'subplan_partition_offsets' array
 * partition_tuple_slot			TupleTableSlot to be used to manipulate any
 *								given leaf partition's rowtype after that
 *								partition is chosen for insertion by
 *								tuple-routing.
 * root_tuple_slot				TupleTableSlot to be used to transiently hold
 *								copy of a tuple that's being moved across
 *								partitions in the root partitioned table's
 *								rowtype
 *-----------------------
 */
typedef struct PartitionTupleRouting
{
	PartitionDispatch *partition_dispatch_info;
	int			num_dispatch;
	Oid		   *partition_oids;
	ResultRelInfo **partitions;
	int			num_partitions;
	TupleConversionMap **parent_child_tupconv_maps;
	TupleConversionMap **child_parent_tupconv_maps;
	bool	   *child_parent_map_not_required;
	int		   *subplan_partition_offsets;
	int			num_subplan_partition_offsets;
	TupleTableSlot *partition_tuple_slot;
	TupleTableSlot *root_tuple_slot;
} PartitionTupleRouting;

/*-----------------------
 * PartitionPruningData - Per-partitioned-table data for run-time pruning
 * of partitions.  For a multilevel partitioned table, we have one of these
 * for the topmost partition plus one for each non-leaf child partition,
 * ordered such that parents appear before their children.
 *
 * subplan_map[] and subpart_map[] have the same definitions as in
 * PartitionPruneInfo (see plannodes.h); though note that here,
 * subpart_map contains indexes into PartitionPruneState.partprunedata[].
 *
 * subplan_map					Subplan index by partition index, or -1.
 * subpart_map					Subpart index by partition index, or -1.
 * present_parts				A Bitmapset of the partition indexes that we
 *								have subplans or subparts for.
 * context						Contains the context details required to call
 *								the partition pruning code.
 * pruning_steps				List of PartitionPruneSteps used to
 *								perform the actual pruning.
 * do_initial_prune				true if pruning should be performed during
 *								executor startup (for this partitioning level).
 * do_exec_prune				true if pruning should be performed during
 *								executor run (for this partitioning level).
 *-----------------------
 */
typedef struct PartitionPruningData
{
	int		   *subplan_map;
	int		   *subpart_map;
	Bitmapset  *present_parts;
	PartitionPruneContext context;
	List	   *pruning_steps;
	bool		do_initial_prune;
	bool		do_exec_prune;
} PartitionPruningData;

/*-----------------------
 * PartitionPruneState - State object required for plan nodes to perform
 * run-time partition pruning.
 *
 * This struct can be attached to plan types which support arbitrary Lists of
 * subplans containing partitions to allow subplans to be eliminated due to
 * the clauses being unable to match to any tuple that the subplan could
 * possibly produce.  Note that we currently support only one partitioned
 * table per parent plan node, hence partprunedata[] need describe only one
 * partitioning hierarchy.
 *
 * partprunedata		Array of PartitionPruningData for the plan's
 *						partitioned relation, ordered such that parent tables
 *						appear before children (hence, topmost table is first).
 * num_partprunedata	Number of items in 'partprunedata' array.
 * do_initial_prune		true if pruning should be performed during executor
 *						startup (at any hierarchy level).
 * do_exec_prune		true if pruning should be performed during
 *						executor run (at any hierarchy level).
 * execparamids			Contains paramids of PARAM_EXEC Params found within
 *						any of the partprunedata structs.  Pruning must be
 *						done again each time the value of one of these
 *						parameters changes.
 * prune_context		A short-lived memory context in which to execute the
 *						partition pruning functions.
 *-----------------------
 */
typedef struct PartitionPruneState
{
	PartitionPruningData *partprunedata;
	int			num_partprunedata;
	bool		do_initial_prune;
	bool		do_exec_prune;
	Bitmapset  *execparamids;
	MemoryContext prune_context;
} PartitionPruneState;

extern PartitionTupleRouting *ExecSetupPartitionTupleRouting(ModifyTableState *mtstate,
							   Relation rel);
extern int ExecFindPartition(ResultRelInfo *resultRelInfo,
				  PartitionDispatch *pd,
				  TupleTableSlot *slot,
				  EState *estate);
extern ResultRelInfo *ExecInitPartitionInfo(ModifyTableState *mtstate,
					  ResultRelInfo *resultRelInfo,
					  PartitionTupleRouting *proute,
					  EState *estate, int partidx);
extern void ExecInitRoutingInfo(ModifyTableState *mtstate,
					EState *estate,
					PartitionTupleRouting *proute,
					ResultRelInfo *partRelInfo,
					int partidx);
extern void ExecSetupChildParentMapForLeaf(PartitionTupleRouting *proute);
extern TupleConversionMap *TupConvMapForLeaf(PartitionTupleRouting *proute,
				  ResultRelInfo *rootRelInfo, int leaf_index);
extern HeapTuple ConvertPartitionTupleSlot(TupleConversionMap *map,
						  HeapTuple tuple,
						  TupleTableSlot *new_slot,
						  TupleTableSlot **p_my_slot,
						  bool shouldFree);
extern void ExecCleanupTupleRouting(ModifyTableState *mtstate,
						PartitionTupleRouting *proute);
extern PartitionPruneState *ExecCreatePartitionPruneState(PlanState *planstate,
							  List *partitionpruneinfo);
extern void ExecDestroyPartitionPruneState(PartitionPruneState *prunestate);
extern Bitmapset *ExecFindMatchingSubPlans(PartitionPruneState *prunestate);
extern Bitmapset *ExecFindInitialMatchingSubPlans(PartitionPruneState *prunestate,
								int nsubplans);

#endif							/* EXECPARTITION_H */
