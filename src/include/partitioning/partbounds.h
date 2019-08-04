/*-------------------------------------------------------------------------
 *
 * partbounds.h
 *
 * Copyright (c) 2007-2019, PostgreSQL Global Development Group
 *
 * src/include/partitioning/partbounds.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARTBOUNDS_H
#define PARTBOUNDS_H

#include "fmgr.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "partitioning/partdefs.h"
#include "utils/relcache.h"


/*
 * PartitionBoundInfoData encapsulates a set of partition bounds. It is
 * usually associated with partitioned tables as part of its partition
 * descriptor, but may also be used to represent a virtual partitioned
 * table such as a partitioned joinrel within the planner.
 *
 * A list partition datum that is known to be NULL is never put into the
 * datums array. Instead, it is tracked using the null_index field.
 *
 * In the case of range partitioning, ndatums will typically be far less than
 * 2 * nparts, because a partition's upper bound and the next partition's lower
 * bound are the same in most common cases, and we only store one of them (the
 * upper bound).  In case of hash partitioning, ndatums will be same as the
 * number of partitions.
 *
 * For range and list partitioned tables, datums is an array of datum-tuples
 * with key->partnatts datums each.  For hash partitioned tables, it is an array
 * of datum-tuples with 2 datums, modulus and remainder, corresponding to a
 * given partition.
 *
 * The datums in datums array are arranged in increasing order as defined by
 * functions qsort_partition_rbound_cmp(), qsort_partition_list_value_cmp() and
 * qsort_partition_hbound_cmp() for range, list and hash partitioned tables
 * respectively. For range and list partitions this simply means that the
 * datums in the datums array are arranged in increasing order as defined by
 * the partition key's operator classes and collations.
 *
 * In the case of list partitioning, the indexes array stores one entry for
 * every datum, which is the index of the partition that accepts a given datum.
 * In case of range partitioning, it stores one entry per distinct range
 * datum, which is the index of the partition for which a given datum
 * is an upper bound.  In the case of hash partitioning, the number of the
 * entries in the indexes array is same as the greatest modulus amongst all
 * partitions.  For a given partition key datum-tuple, the index of the
 * partition which would accept that datum-tuple would be given by the entry
 * pointed by remainder produced when hash value of the datum-tuple is divided
 * by the greatest modulus.
 */
typedef struct PartitionBoundInfoData
{
	char		strategy;		/* hash, list or range? */
	int			ndatums;		/* Length of the datums following array */
	Datum	  **datums;
	PartitionRangeDatumKind **kind; /* The kind of each range bound datum;
									 * NULL for hash and list partitioned
									 * tables */
	int		   *indexes;		/* Partition indexes */
	int			null_index;		/* Index of the null-accepting partition; -1
								 * if there isn't one */
	int			default_index;	/* Index of the default partition; -1 if there
								 * isn't one */
} PartitionBoundInfoData;

#define partition_bound_accepts_nulls(bi) ((bi)->null_index != -1)
#define partition_bound_has_default(bi) ((bi)->default_index != -1)

extern int	get_hash_partition_greatest_modulus(PartitionBoundInfo b);
extern uint64 compute_partition_hash_value(int partnatts, FmgrInfo *partsupfunc,
										   Oid *partcollation,
										   Datum *values, bool *isnull);
extern List *get_qual_from_partbound(Relation rel, Relation parent,
									 PartitionBoundSpec *spec);
extern PartitionBoundInfo partition_bounds_create(PartitionBoundSpec **boundspecs,
												  int nparts, PartitionKey key, int **mapping);
extern bool partition_bounds_equal(int partnatts, int16 *parttyplen,
								   bool *parttypbyval, PartitionBoundInfo b1,
								   PartitionBoundInfo b2);
extern PartitionBoundInfo partition_bounds_copy(PartitionBoundInfo src,
												PartitionKey key);
extern bool partitions_are_ordered(PartitionBoundInfo boundinfo, int nparts);
extern void check_new_partition_bound(char *relname, Relation parent,
									  PartitionBoundSpec *spec);
extern void check_default_partition_contents(Relation parent,
											 Relation defaultRel,
											 PartitionBoundSpec *new_spec);

extern int32 partition_rbound_datum_cmp(FmgrInfo *partsupfunc,
										Oid *partcollation,
										Datum *rb_datums, PartitionRangeDatumKind *rb_kind,
										Datum *tuple_datums, int n_tuple_datums);
extern int	partition_list_bsearch(FmgrInfo *partsupfunc,
								   Oid *partcollation,
								   PartitionBoundInfo boundinfo,
								   Datum value, bool *is_equal);
extern int	partition_range_datum_bsearch(FmgrInfo *partsupfunc,
										  Oid *partcollation,
										  PartitionBoundInfo boundinfo,
										  int nvalues, Datum *values, bool *is_equal);
extern int	partition_hash_bsearch(PartitionBoundInfo boundinfo,
								   int modulus, int remainder);

#endif							/* PARTBOUNDS_H */
