/*--------------------------------------------------------------------
 * execPartition.h
 *		POSTGRES partitioning executor interface
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
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

extern void ExecSetupPartitionTupleRouting(Relation rel,
							   Index resultRTindex,
							   EState *estate,
							   PartitionDispatch **pd,
							   ResultRelInfo ***partitions,
							   TupleConversionMap ***tup_conv_maps,
							   TupleTableSlot **partition_tuple_slot,
							   int *num_parted, int *num_partitions);
extern int ExecFindPartition(ResultRelInfo *resultRelInfo,
				  PartitionDispatch *pd,
				  TupleTableSlot *slot,
				  EState *estate);

#endif							/* EXECPARTITION_H */
