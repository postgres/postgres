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
 * partitions					Array of ResultRelInfo* objects with one entry
 *								for every leaf partition in the partition tree.
 * num_partitions				Number of leaf partitions in the partition tree
 *								(= 'partitions' array length)
 * partition_tupconv_maps		Array of TupleConversionMap objects with one
 *								entry for every leaf partition (required to
 *								convert input tuple based on the root table's
 *								rowtype to a leaf partition's rowtype after
 *								tuple routing is done)
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
	ResultRelInfo **partitions;
	int			num_partitions;
	TupleConversionMap **partition_tupconv_maps;
	TupleTableSlot *partition_tuple_slot;
} PartitionTupleRouting;

extern PartitionTupleRouting *ExecSetupPartitionTupleRouting(ModifyTableState *mtstate,
							   Relation rel, Index resultRTindex,
							   EState *estate);
extern int ExecFindPartition(ResultRelInfo *resultRelInfo,
				  PartitionDispatch *pd,
				  TupleTableSlot *slot,
				  EState *estate);
extern void ExecCleanupTupleRouting(PartitionTupleRouting *proute);

#endif							/* EXECPARTITION_H */
