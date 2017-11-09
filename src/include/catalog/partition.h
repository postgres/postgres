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

/* Seed for the extended hash function */
#define HASH_PARTITION_SEED UINT64CONST(0x7A5B22367996DCFD)

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
	PartitionBoundInfo boundinfo;	/* collection of partition bounds */
} PartitionDescData;

typedef struct PartitionDescData *PartitionDesc;

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
extern bool partition_bounds_equal(int partnatts, int16 *parttyplen,
					   bool *parttypbyval, PartitionBoundInfo b1,
					   PartitionBoundInfo b2);
extern PartitionBoundInfo partition_bounds_copy(PartitionBoundInfo src,
					  PartitionKey key);

extern void check_new_partition_bound(char *relname, Relation parent,
						  PartitionBoundSpec *spec);
extern Oid	get_partition_parent(Oid relid);
extern List *get_qual_from_partbound(Relation rel, Relation parent,
						PartitionBoundSpec *spec);
extern List *map_partition_varattnos(List *expr, int target_varno,
						Relation partrel, Relation parent,
						bool *found_whole_row);
extern List *RelationGetPartitionQual(Relation rel);
extern Expr *get_partition_qual_relid(Oid relid);

/* For tuple routing */
extern PartitionDispatch *RelationGetPartitionDispatchInfo(Relation rel,
								 int *num_parted, List **leaf_part_oids);
extern void FormPartitionKeyDatum(PartitionDispatch pd,
					  TupleTableSlot *slot,
					  EState *estate,
					  Datum *values,
					  bool *isnull);
extern int get_partition_for_tuple(PartitionDispatch *pd,
						TupleTableSlot *slot,
						EState *estate,
						PartitionDispatchData **failed_at,
						TupleTableSlot **failed_slot);
extern Oid	get_default_oid_from_partdesc(PartitionDesc partdesc);
extern Oid	get_default_partition_oid(Oid parentId);
extern void update_default_partition_oid(Oid parentId, Oid defaultPartId);
extern void check_default_allows_bound(Relation parent, Relation defaultRel,
						   PartitionBoundSpec *new_spec);
extern List *get_proposed_default_constraint(List *new_part_constaints);

#endif							/* PARTITION_H */
