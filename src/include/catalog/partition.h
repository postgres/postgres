/*-------------------------------------------------------------------------
 *
 * partition.h
 *		Header file for structures and utility functions related to
 *		partitioning
 *
 * Copyright (c) 2007-2019, PostgreSQL Global Development Group
 *
 * src/include/catalog/partition.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARTITION_H
#define PARTITION_H

#include "partitioning/partdefs.h"
#include "utils/relcache.h"

/* Seed for the extended hash function */
#define HASH_PARTITION_SEED UINT64CONST(0x7A5B22367996DCFD)

extern Oid	get_partition_parent(Oid relid);
extern List *get_partition_ancestors(Oid relid);
extern Oid	index_get_partition(Relation partition, Oid indexId);
extern List *map_partition_varattnos(List *expr, int fromrel_varno,
									 Relation to_rel, Relation from_rel,
									 bool *found_whole_row);
extern bool has_partition_attrs(Relation rel, Bitmapset *attnums,
								bool *used_in_expr);

extern Oid	get_default_partition_oid(Oid parentId);
extern void update_default_partition_oid(Oid parentId, Oid defaultPartId);
extern List *get_proposed_default_constraint(List *new_part_constraints);

#endif							/* PARTITION_H */
