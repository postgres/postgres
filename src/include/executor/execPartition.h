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

#include "catalog/partition.h"
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
 * PartitionPruningData - Encapsulates all information required to support
 * elimination of partitions in node types which support arbitrary Lists of
 * subplans.  Information stored here allows the planner's partition pruning
 * functions to be called and the return value of partition indexes translated
 * into the subpath indexes of node types such as Append, thus allowing us to
 * bypass certain subnodes when we have proofs that indicate that no tuple
 * matching the 'pruning_steps' will be found within.
 *
 * subnode_map					An array containing the subnode index which
 *								matches this partition index, or -1 if the
 *								subnode has been pruned already.
 * subpart_map					An array containing the offset into the
 *								'partprunedata' array in PartitionPruning, or
 *								-1 if there is no such element in that array.
 * present_parts				A Bitmapset of the partition index that we have
 *								subnodes mapped for.
 * context						Contains the context details required to call
 *								the partition pruning code.
 * pruning_steps				Contains a list of PartitionPruneStep used to
 *								perform the actual pruning.
 * extparams					Contains paramids of external params found
 *								matching partition keys in 'pruning_steps'.
 * allparams					As 'extparams' but also including exec params.
 *-----------------------
 */
typedef struct PartitionPruningData
{
	int		   *subnode_map;
	int		   *subpart_map;
	Bitmapset  *present_parts;
	PartitionPruneContext context;
	List	   *pruning_steps;
	Bitmapset  *extparams;
	Bitmapset  *allparams;
} PartitionPruningData;

/*-----------------------
 * PartitionPruneState - State object required for executor nodes to perform
 * partition pruning elimination of their subnodes.  This encapsulates a
 * flattened hierarchy of PartitionPruningData structs and also stores all
 * paramids which were found to match the partition keys of each partition.
 * This struct can be attached to node types which support arbitrary Lists of
 * subnodes containing partitions to allow subnodes to be eliminated due to
 * the clauses being unable to match to any tuple that the subnode could
 * possibly produce.
 *
 * partprunedata		Array of PartitionPruningData for the node's target
 *						partitioned relation. First element contains the
 *						details for the target partitioned table.
 * num_partprunedata	Number of items in 'partprunedata' array.
 * prune_context		A memory context which can be used to call the query
 *						planner's partition prune functions.
 * extparams			All PARAM_EXTERN paramids which were found to match a
 *						partition key in each of the contained
 *						PartitionPruningData structs.
 * execparams			As above but for PARAM_EXEC.
 * allparams			Union of 'extparams' and 'execparams', saved to avoid
 *						recalculation.
 *-----------------------
 */
typedef struct PartitionPruneState
{
	PartitionPruningData *partprunedata;
	int			num_partprunedata;
	MemoryContext prune_context;
	Bitmapset  *extparams;
	Bitmapset  *execparams;
	Bitmapset  *allparams;
} PartitionPruneState;

extern PartitionTupleRouting *ExecSetupPartitionTupleRouting(ModifyTableState *mtstate,
							   Relation rel);
extern int ExecFindPartition(ResultRelInfo *resultRelInfo,
				  PartitionDispatch *pd,
				  TupleTableSlot *slot,
				  EState *estate);
extern int ExecFindPartitionByOid(PartitionTupleRouting *proute, Oid partoid);
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
						  TupleTableSlot **p_my_slot);
extern void ExecCleanupTupleRouting(ModifyTableState *mtstate,
						PartitionTupleRouting *proute);
extern PartitionPruneState *ExecSetupPartitionPruneState(PlanState *planstate,
						  List *partitionpruneinfo);
extern Bitmapset *ExecFindMatchingSubPlans(PartitionPruneState *prunestate);
extern Bitmapset *ExecFindInitialMatchingSubPlans(PartitionPruneState *prunestate,
								int nsubnodes);

#endif							/* EXECPARTITION_H */
