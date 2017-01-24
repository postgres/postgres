/*-------------------------------------------------------------------------
 *
 * partition.h
 *		Header file for structures and utility functions related to
 *		partitioning
 *
 * Copyright (c) 2007-2017, PostgreSQL Global Development Group
 *
 * src/include/catalog/partition.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARTITION_H
#define PARTITION_H

#include "fmgr.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "parser/parse_node.h"
#include "utils/rel.h"

/*
 * PartitionBoundInfo encapsulates a set of partition bounds.  It is usually
 * associated with partitioned tables as part of its partition descriptor.
 *
 * The internal structure is opaque outside partition.c.
 */
typedef struct PartitionBoundInfoData *PartitionBoundInfo;

/*
 * Information about partitions of a partitioned table.
 */
typedef struct PartitionDescData
{
	int			nparts;			/* Number of partitions */
	Oid		   *oids;			/* OIDs of partitions */
	PartitionBoundInfo boundinfo;		/* collection of partition bounds */
} PartitionDescData;

typedef struct PartitionDescData *PartitionDesc;

/*-----------------------
 * PartitionDispatch - information about one partitioned table in a partition
 * hiearchy required to route a tuple to one of its partitions
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
 *				RelationGetPartitionDispatchInfo())
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

extern void RelationBuildPartitionDesc(Relation relation);
extern bool partition_bounds_equal(PartitionKey key,
					   PartitionBoundInfo p1, PartitionBoundInfo p2);

extern void check_new_partition_bound(char *relname, Relation parent, Node *bound);
extern Oid	get_partition_parent(Oid relid);
extern List *get_qual_from_partbound(Relation rel, Relation parent, Node *bound);
extern List *map_partition_varattnos(List *expr, int target_varno,
						Relation partrel, Relation parent);
extern List *RelationGetPartitionQual(Relation rel);

/* For tuple routing */
extern PartitionDispatch *RelationGetPartitionDispatchInfo(Relation rel,
								 int lockmode, int *num_parted,
								 List **leaf_part_oids);
extern int get_partition_for_tuple(PartitionDispatch *pd,
						TupleTableSlot *slot,
						EState *estate,
						Oid *failed_at);
#endif   /* PARTITION_H */
