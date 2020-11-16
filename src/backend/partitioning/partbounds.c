/*-------------------------------------------------------------------------
 *
 * partbounds.c
 *		Support routines for manipulating partition bounds
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/backend/partitioning/partbounds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "commands/tablecmds.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "parser/parse_coerce.h"
#include "partitioning/partbounds.h"
#include "partitioning/partdesc.h"
#include "partitioning/partprune.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * When qsort'ing partition bounds after reading from the catalog, each bound
 * is represented with one of the following structs.
 */

/* One bound of a hash partition */
typedef struct PartitionHashBound
{
	int			modulus;
	int			remainder;
	int			index;
} PartitionHashBound;

/* One value coming from some (index'th) list partition */
typedef struct PartitionListValue
{
	int			index;
	Datum		value;
} PartitionListValue;

/* One bound of a range partition */
typedef struct PartitionRangeBound
{
	int			index;
	Datum	   *datums;			/* range bound datums */
	PartitionRangeDatumKind *kind;	/* the kind of each datum */
	bool		lower;			/* this is the lower (vs upper) bound */
} PartitionRangeBound;

/*
 * Mapping from partitions of a joining relation to partitions of a join
 * relation being computed (a.k.a merged partitions)
 */
typedef struct PartitionMap
{
	int			nparts;			/* number of partitions */
	int		   *merged_indexes; /* indexes of merged partitions */
	bool	   *merged;			/* flags to indicate whether partitions are
								 * merged with non-dummy partitions */
	bool		did_remapping;	/* did we re-map partitions? */
	int		   *old_indexes;	/* old indexes of merged partitions if
								 * did_remapping */
} PartitionMap;

/* Macro for comparing two range bounds */
#define compare_range_bounds(partnatts, partsupfunc, partcollations, \
							 bound1, bound2) \
	(partition_rbound_cmp(partnatts, partsupfunc, partcollations, \
						  (bound1)->datums, (bound1)->kind, (bound1)->lower, \
						  bound2))

static int32 qsort_partition_hbound_cmp(const void *a, const void *b);
static int32 qsort_partition_list_value_cmp(const void *a, const void *b,
											void *arg);
static int32 qsort_partition_rbound_cmp(const void *a, const void *b,
										void *arg);
static PartitionBoundInfo create_hash_bounds(PartitionBoundSpec **boundspecs,
											 int nparts, PartitionKey key, int **mapping);
static PartitionBoundInfo create_list_bounds(PartitionBoundSpec **boundspecs,
											 int nparts, PartitionKey key, int **mapping);
static PartitionBoundInfo create_range_bounds(PartitionBoundSpec **boundspecs,
											  int nparts, PartitionKey key, int **mapping);
static PartitionBoundInfo merge_list_bounds(FmgrInfo *partsupfunc,
											Oid *collations,
											RelOptInfo *outer_rel,
											RelOptInfo *inner_rel,
											JoinType jointype,
											List **outer_parts,
											List **inner_parts);
static PartitionBoundInfo merge_range_bounds(int partnatts,
											 FmgrInfo *partsupfuncs,
											 Oid *partcollations,
											 RelOptInfo *outer_rel,
											 RelOptInfo *inner_rel,
											 JoinType jointype,
											 List **outer_parts,
											 List **inner_parts);
static void init_partition_map(RelOptInfo *rel, PartitionMap *map);
static void free_partition_map(PartitionMap *map);
static bool is_dummy_partition(RelOptInfo *rel, int part_index);
static int	merge_matching_partitions(PartitionMap *outer_map,
									  PartitionMap *inner_map,
									  int outer_part,
									  int inner_part,
									  int *next_index);
static int	process_outer_partition(PartitionMap *outer_map,
									PartitionMap *inner_map,
									bool outer_has_default,
									bool inner_has_default,
									int outer_index,
									int inner_default,
									JoinType jointype,
									int *next_index,
									int *default_index);
static int	process_inner_partition(PartitionMap *outer_map,
									PartitionMap *inner_map,
									bool outer_has_default,
									bool inner_has_default,
									int inner_index,
									int outer_default,
									JoinType jointype,
									int *next_index,
									int *default_index);
static void merge_null_partitions(PartitionMap *outer_map,
								  PartitionMap *inner_map,
								  bool outer_has_null,
								  bool inner_has_null,
								  int outer_null,
								  int inner_null,
								  JoinType jointype,
								  int *next_index,
								  int *null_index);
static void merge_default_partitions(PartitionMap *outer_map,
									 PartitionMap *inner_map,
									 bool outer_has_default,
									 bool inner_has_default,
									 int outer_default,
									 int inner_default,
									 JoinType jointype,
									 int *next_index,
									 int *default_index);
static int	merge_partition_with_dummy(PartitionMap *map, int index,
									   int *next_index);
static void fix_merged_indexes(PartitionMap *outer_map,
							   PartitionMap *inner_map,
							   int nmerged, List *merged_indexes);
static void generate_matching_part_pairs(RelOptInfo *outer_rel,
										 RelOptInfo *inner_rel,
										 PartitionMap *outer_map,
										 PartitionMap *inner_map,
										 int nmerged,
										 List **outer_parts,
										 List **inner_parts);
static PartitionBoundInfo build_merged_partition_bounds(char strategy,
														List *merged_datums,
														List *merged_kinds,
														List *merged_indexes,
														int null_index,
														int default_index);
static int	get_range_partition(RelOptInfo *rel,
								PartitionBoundInfo bi,
								int *lb_pos,
								PartitionRangeBound *lb,
								PartitionRangeBound *ub);
static int	get_range_partition_internal(PartitionBoundInfo bi,
										 int *lb_pos,
										 PartitionRangeBound *lb,
										 PartitionRangeBound *ub);
static bool compare_range_partitions(int partnatts, FmgrInfo *partsupfuncs,
									 Oid *partcollations,
									 PartitionRangeBound *outer_lb,
									 PartitionRangeBound *outer_ub,
									 PartitionRangeBound *inner_lb,
									 PartitionRangeBound *inner_ub,
									 int *lb_cmpval, int *ub_cmpval);
static void get_merged_range_bounds(int partnatts, FmgrInfo *partsupfuncs,
									Oid *partcollations, JoinType jointype,
									PartitionRangeBound *outer_lb,
									PartitionRangeBound *outer_ub,
									PartitionRangeBound *inner_lb,
									PartitionRangeBound *inner_ub,
									int lb_cmpval, int ub_cmpval,
									PartitionRangeBound *merged_lb,
									PartitionRangeBound *merged_ub);
static void add_merged_range_bounds(int partnatts, FmgrInfo *partsupfuncs,
									Oid *partcollations,
									PartitionRangeBound *merged_lb,
									PartitionRangeBound *merged_ub,
									int merged_index,
									List **merged_datums,
									List **merged_kinds,
									List **merged_indexes);
static PartitionRangeBound *make_one_partition_rbound(PartitionKey key, int index,
													  List *datums, bool lower);
static int32 partition_hbound_cmp(int modulus1, int remainder1, int modulus2,
								  int remainder2);
static int32 partition_rbound_cmp(int partnatts, FmgrInfo *partsupfunc,
								  Oid *partcollation, Datum *datums1,
								  PartitionRangeDatumKind *kind1, bool lower1,
								  PartitionRangeBound *b2);
static int	partition_range_bsearch(int partnatts, FmgrInfo *partsupfunc,
									Oid *partcollation,
									PartitionBoundInfo boundinfo,
									PartitionRangeBound *probe, int32 *cmpval);
static int	get_partition_bound_num_indexes(PartitionBoundInfo b);
static Expr *make_partition_op_expr(PartitionKey key, int keynum,
									uint16 strategy, Expr *arg1, Expr *arg2);
static Oid	get_partition_operator(PartitionKey key, int col,
								   StrategyNumber strategy, bool *need_relabel);
static List *get_qual_for_hash(Relation parent, PartitionBoundSpec *spec);
static List *get_qual_for_list(Relation parent, PartitionBoundSpec *spec);
static List *get_qual_for_range(Relation parent, PartitionBoundSpec *spec,
								bool for_default);
static void get_range_key_properties(PartitionKey key, int keynum,
									 PartitionRangeDatum *ldatum,
									 PartitionRangeDatum *udatum,
									 ListCell **partexprs_item,
									 Expr **keyCol,
									 Const **lower_val, Const **upper_val);
static List *get_range_nulltest(PartitionKey key);

/*
 * get_qual_from_partbound
 *		Given a parser node for partition bound, return the list of executable
 *		expressions as partition constraint
 */
List *
get_qual_from_partbound(Relation rel, Relation parent,
						PartitionBoundSpec *spec)
{
	PartitionKey key = RelationGetPartitionKey(parent);
	List	   *my_qual = NIL;

	Assert(key != NULL);

	switch (key->strategy)
	{
		case PARTITION_STRATEGY_HASH:
			Assert(spec->strategy == PARTITION_STRATEGY_HASH);
			my_qual = get_qual_for_hash(parent, spec);
			break;

		case PARTITION_STRATEGY_LIST:
			Assert(spec->strategy == PARTITION_STRATEGY_LIST);
			my_qual = get_qual_for_list(parent, spec);
			break;

		case PARTITION_STRATEGY_RANGE:
			Assert(spec->strategy == PARTITION_STRATEGY_RANGE);
			my_qual = get_qual_for_range(parent, spec, false);
			break;

		default:
			elog(ERROR, "unexpected partition strategy: %d",
				 (int) key->strategy);
	}

	return my_qual;
}

/*
 *	partition_bounds_create
 *		Build a PartitionBoundInfo struct from a list of PartitionBoundSpec
 *		nodes
 *
 * This function creates a PartitionBoundInfo and fills the values of its
 * various members based on the input list.  Importantly, 'datums' array will
 * contain Datum representation of individual bounds (possibly after
 * de-duplication as in case of range bounds), sorted in a canonical order
 * defined by qsort_partition_* functions of respective partitioning methods.
 * 'indexes' array will contain as many elements as there are bounds (specific
 * exceptions to this rule are listed in the function body), which represent
 * the 0-based canonical positions of partitions.
 *
 * Upon return from this function, *mapping is set to an array of
 * list_length(boundspecs) elements, each of which maps the original index of
 * a partition to its canonical index.
 *
 * Note: The objects returned by this function are wholly allocated in the
 * current memory context.
 */
PartitionBoundInfo
partition_bounds_create(PartitionBoundSpec **boundspecs, int nparts,
						PartitionKey key, int **mapping)
{
	int			i;

	Assert(nparts > 0);

	/*
	 * For each partitioning method, we first convert the partition bounds
	 * from their parser node representation to the internal representation,
	 * along with any additional preprocessing (such as de-duplicating range
	 * bounds).  Resulting bound datums are then added to the 'datums' array
	 * in PartitionBoundInfo.  For each datum added, an integer indicating the
	 * canonical partition index is added to the 'indexes' array.
	 *
	 * For each bound, we remember its partition's position (0-based) in the
	 * original list to later map it to the canonical index.
	 */

	/*
	 * Initialize mapping array with invalid values, this is filled within
	 * each sub-routine below depending on the bound type.
	 */
	*mapping = (int *) palloc(sizeof(int) * nparts);
	for (i = 0; i < nparts; i++)
		(*mapping)[i] = -1;

	switch (key->strategy)
	{
		case PARTITION_STRATEGY_HASH:
			return create_hash_bounds(boundspecs, nparts, key, mapping);

		case PARTITION_STRATEGY_LIST:
			return create_list_bounds(boundspecs, nparts, key, mapping);

		case PARTITION_STRATEGY_RANGE:
			return create_range_bounds(boundspecs, nparts, key, mapping);

		default:
			elog(ERROR, "unexpected partition strategy: %d",
				 (int) key->strategy);
			break;
	}

	Assert(false);
	return NULL;				/* keep compiler quiet */
}

/*
 * create_hash_bounds
 *		Create a PartitionBoundInfo for a hash partitioned table
 */
static PartitionBoundInfo
create_hash_bounds(PartitionBoundSpec **boundspecs, int nparts,
				   PartitionKey key, int **mapping)
{
	PartitionBoundInfo boundinfo;
	PartitionHashBound **hbounds = NULL;
	int			i;
	int			ndatums = 0;
	int			greatest_modulus;

	boundinfo = (PartitionBoundInfoData *)
		palloc0(sizeof(PartitionBoundInfoData));
	boundinfo->strategy = key->strategy;
	/* No special hash partitions. */
	boundinfo->null_index = -1;
	boundinfo->default_index = -1;

	ndatums = nparts;
	hbounds = (PartitionHashBound **)
		palloc(nparts * sizeof(PartitionHashBound *));

	/* Convert from node to the internal representation */
	for (i = 0; i < nparts; i++)
	{
		PartitionBoundSpec *spec = boundspecs[i];

		if (spec->strategy != PARTITION_STRATEGY_HASH)
			elog(ERROR, "invalid strategy in partition bound spec");

		hbounds[i] = (PartitionHashBound *) palloc(sizeof(PartitionHashBound));
		hbounds[i]->modulus = spec->modulus;
		hbounds[i]->remainder = spec->remainder;
		hbounds[i]->index = i;
	}

	/* Sort all the bounds in ascending order */
	qsort(hbounds, nparts, sizeof(PartitionHashBound *),
		  qsort_partition_hbound_cmp);

	/* After sorting, moduli are now stored in ascending order. */
	greatest_modulus = hbounds[ndatums - 1]->modulus;

	boundinfo->ndatums = ndatums;
	boundinfo->datums = (Datum **) palloc0(ndatums * sizeof(Datum *));
	boundinfo->indexes = (int *) palloc(greatest_modulus * sizeof(int));
	for (i = 0; i < greatest_modulus; i++)
		boundinfo->indexes[i] = -1;

	/*
	 * For hash partitioning, there are as many datums (modulus and remainder
	 * pairs) as there are partitions.  Indexes are simply values ranging from
	 * 0 to (nparts - 1).
	 */
	for (i = 0; i < nparts; i++)
	{
		int			modulus = hbounds[i]->modulus;
		int			remainder = hbounds[i]->remainder;

		boundinfo->datums[i] = (Datum *) palloc(2 * sizeof(Datum));
		boundinfo->datums[i][0] = Int32GetDatum(modulus);
		boundinfo->datums[i][1] = Int32GetDatum(remainder);

		while (remainder < greatest_modulus)
		{
			/* overlap? */
			Assert(boundinfo->indexes[remainder] == -1);
			boundinfo->indexes[remainder] = i;
			remainder += modulus;
		}

		(*mapping)[hbounds[i]->index] = i;
		pfree(hbounds[i]);
	}
	pfree(hbounds);

	return boundinfo;
}

/*
 * create_list_bounds
 *		Create a PartitionBoundInfo for a list partitioned table
 */
static PartitionBoundInfo
create_list_bounds(PartitionBoundSpec **boundspecs, int nparts,
				   PartitionKey key, int **mapping)
{
	PartitionBoundInfo boundinfo;
	PartitionListValue **all_values = NULL;
	ListCell   *cell;
	int			i = 0;
	int			ndatums = 0;
	int			next_index = 0;
	int			default_index = -1;
	int			null_index = -1;
	List	   *non_null_values = NIL;

	boundinfo = (PartitionBoundInfoData *)
		palloc0(sizeof(PartitionBoundInfoData));
	boundinfo->strategy = key->strategy;
	/* Will be set correctly below. */
	boundinfo->null_index = -1;
	boundinfo->default_index = -1;

	/* Create a unified list of non-null values across all partitions. */
	for (i = 0; i < nparts; i++)
	{
		PartitionBoundSpec *spec = boundspecs[i];
		ListCell   *c;

		if (spec->strategy != PARTITION_STRATEGY_LIST)
			elog(ERROR, "invalid strategy in partition bound spec");

		/*
		 * Note the index of the partition bound spec for the default
		 * partition.  There's no datum to add to the list on non-null datums
		 * for this partition.
		 */
		if (spec->is_default)
		{
			default_index = i;
			continue;
		}

		foreach(c, spec->listdatums)
		{
			Const	   *val = castNode(Const, lfirst(c));
			PartitionListValue *list_value = NULL;

			if (!val->constisnull)
			{
				list_value = (PartitionListValue *)
					palloc0(sizeof(PartitionListValue));
				list_value->index = i;
				list_value->value = val->constvalue;
			}
			else
			{
				/*
				 * Never put a null into the values array; save the index of
				 * the partition that stores nulls, instead.
				 */
				if (null_index != -1)
					elog(ERROR, "found null more than once");
				null_index = i;
			}

			if (list_value)
				non_null_values = lappend(non_null_values, list_value);
		}
	}

	ndatums = list_length(non_null_values);

	/*
	 * Collect all list values in one array. Alongside the value, we also save
	 * the index of partition the value comes from.
	 */
	all_values = (PartitionListValue **)
		palloc(ndatums * sizeof(PartitionListValue *));
	i = 0;
	foreach(cell, non_null_values)
	{
		PartitionListValue *src = lfirst(cell);

		all_values[i] = (PartitionListValue *)
			palloc(sizeof(PartitionListValue));
		all_values[i]->value = src->value;
		all_values[i]->index = src->index;
		i++;
	}

	qsort_arg(all_values, ndatums, sizeof(PartitionListValue *),
			  qsort_partition_list_value_cmp, (void *) key);

	boundinfo->ndatums = ndatums;
	boundinfo->datums = (Datum **) palloc0(ndatums * sizeof(Datum *));
	boundinfo->indexes = (int *) palloc(ndatums * sizeof(int));

	/*
	 * Copy values.  Canonical indexes are values ranging from 0 to (nparts -
	 * 1) assigned to each partition such that all datums of a given partition
	 * receive the same value. The value for a given partition is the index of
	 * that partition's smallest datum in the all_values[] array.
	 */
	for (i = 0; i < ndatums; i++)
	{
		int			orig_index = all_values[i]->index;

		boundinfo->datums[i] = (Datum *) palloc(sizeof(Datum));
		boundinfo->datums[i][0] = datumCopy(all_values[i]->value,
											key->parttypbyval[0],
											key->parttyplen[0]);

		/* If the old index has no mapping, assign one */
		if ((*mapping)[orig_index] == -1)
			(*mapping)[orig_index] = next_index++;

		boundinfo->indexes[i] = (*mapping)[orig_index];
	}

	/*
	 * Set the canonical value for null_index, if any.
	 *
	 * It is possible that the null-accepting partition has not been assigned
	 * an index yet, which could happen if such partition accepts only null
	 * and hence not handled in the above loop which only looked at non-null
	 * values.
	 */
	if (null_index != -1)
	{
		Assert(null_index >= 0);
		if ((*mapping)[null_index] == -1)
			(*mapping)[null_index] = next_index++;
		boundinfo->null_index = (*mapping)[null_index];
	}

	/* Set the canonical value for default_index, if any. */
	if (default_index != -1)
	{
		/*
		 * The default partition accepts any value not specified in the lists
		 * of other partitions, hence it should not get mapped index while
		 * assigning those for non-null datums.
		 */
		Assert(default_index >= 0);
		Assert((*mapping)[default_index] == -1);
		(*mapping)[default_index] = next_index++;
		boundinfo->default_index = (*mapping)[default_index];
	}

	/* All partitions must now have been assigned canonical indexes. */
	Assert(next_index == nparts);
	return boundinfo;
}

/*
 * create_range_bounds
 *		Create a PartitionBoundInfo for a range partitioned table
 */
static PartitionBoundInfo
create_range_bounds(PartitionBoundSpec **boundspecs, int nparts,
					PartitionKey key, int **mapping)
{
	PartitionBoundInfo boundinfo;
	PartitionRangeBound **rbounds = NULL;
	PartitionRangeBound **all_bounds,
			   *prev;
	int			i,
				k;
	int			ndatums = 0;
	int			default_index = -1;
	int			next_index = 0;

	boundinfo = (PartitionBoundInfoData *)
		palloc0(sizeof(PartitionBoundInfoData));
	boundinfo->strategy = key->strategy;
	/* There is no special null-accepting range partition. */
	boundinfo->null_index = -1;
	/* Will be set correctly below. */
	boundinfo->default_index = -1;

	all_bounds = (PartitionRangeBound **)
		palloc0(2 * nparts * sizeof(PartitionRangeBound *));

	/* Create a unified list of range bounds across all the partitions. */
	ndatums = 0;
	for (i = 0; i < nparts; i++)
	{
		PartitionBoundSpec *spec = boundspecs[i];
		PartitionRangeBound *lower,
				   *upper;

		if (spec->strategy != PARTITION_STRATEGY_RANGE)
			elog(ERROR, "invalid strategy in partition bound spec");

		/*
		 * Note the index of the partition bound spec for the default
		 * partition.  There's no datum to add to the all_bounds array for
		 * this partition.
		 */
		if (spec->is_default)
		{
			default_index = i;
			continue;
		}

		lower = make_one_partition_rbound(key, i, spec->lowerdatums, true);
		upper = make_one_partition_rbound(key, i, spec->upperdatums, false);
		all_bounds[ndatums++] = lower;
		all_bounds[ndatums++] = upper;
	}

	Assert(ndatums == nparts * 2 ||
		   (default_index != -1 && ndatums == (nparts - 1) * 2));

	/* Sort all the bounds in ascending order */
	qsort_arg(all_bounds, ndatums,
			  sizeof(PartitionRangeBound *),
			  qsort_partition_rbound_cmp,
			  (void *) key);

	/* Save distinct bounds from all_bounds into rbounds. */
	rbounds = (PartitionRangeBound **)
		palloc(ndatums * sizeof(PartitionRangeBound *));
	k = 0;
	prev = NULL;
	for (i = 0; i < ndatums; i++)
	{
		PartitionRangeBound *cur = all_bounds[i];
		bool		is_distinct = false;
		int			j;

		/* Is the current bound distinct from the previous one? */
		for (j = 0; j < key->partnatts; j++)
		{
			Datum		cmpval;

			if (prev == NULL || cur->kind[j] != prev->kind[j])
			{
				is_distinct = true;
				break;
			}

			/*
			 * If the bounds are both MINVALUE or MAXVALUE, stop now and treat
			 * them as equal, since any values after this point must be
			 * ignored.
			 */
			if (cur->kind[j] != PARTITION_RANGE_DATUM_VALUE)
				break;

			cmpval = FunctionCall2Coll(&key->partsupfunc[j],
									   key->partcollation[j],
									   cur->datums[j],
									   prev->datums[j]);
			if (DatumGetInt32(cmpval) != 0)
			{
				is_distinct = true;
				break;
			}
		}

		/*
		 * Only if the bound is distinct save it into a temporary array, i.e,
		 * rbounds which is later copied into boundinfo datums array.
		 */
		if (is_distinct)
			rbounds[k++] = all_bounds[i];

		prev = cur;
	}

	/* Update ndatums to hold the count of distinct datums. */
	ndatums = k;

	/*
	 * Add datums to boundinfo.  Canonical indexes are values ranging from 0
	 * to nparts - 1, assigned in that order to each partition's upper bound.
	 * For 'datums' elements that are lower bounds, there is -1 in the
	 * 'indexes' array to signify that no partition exists for the values less
	 * than such a bound and greater than or equal to the previous upper
	 * bound.
	 */
	boundinfo->ndatums = ndatums;
	boundinfo->datums = (Datum **) palloc0(ndatums * sizeof(Datum *));
	boundinfo->kind = (PartitionRangeDatumKind **)
		palloc(ndatums *
			   sizeof(PartitionRangeDatumKind *));

	/*
	 * For range partitioning, an additional value of -1 is stored as the last
	 * element.
	 */
	boundinfo->indexes = (int *) palloc((ndatums + 1) * sizeof(int));

	for (i = 0; i < ndatums; i++)
	{
		int			j;

		boundinfo->datums[i] = (Datum *) palloc(key->partnatts *
												sizeof(Datum));
		boundinfo->kind[i] = (PartitionRangeDatumKind *)
			palloc(key->partnatts *
				   sizeof(PartitionRangeDatumKind));
		for (j = 0; j < key->partnatts; j++)
		{
			if (rbounds[i]->kind[j] == PARTITION_RANGE_DATUM_VALUE)
				boundinfo->datums[i][j] =
					datumCopy(rbounds[i]->datums[j],
							  key->parttypbyval[j],
							  key->parttyplen[j]);
			boundinfo->kind[i][j] = rbounds[i]->kind[j];
		}

		/*
		 * There is no mapping for invalid indexes.
		 *
		 * Any lower bounds in the rbounds array have invalid indexes
		 * assigned, because the values between the previous bound (if there
		 * is one) and this (lower) bound are not part of the range of any
		 * existing partition.
		 */
		if (rbounds[i]->lower)
			boundinfo->indexes[i] = -1;
		else
		{
			int			orig_index = rbounds[i]->index;

			/* If the old index has no mapping, assign one */
			if ((*mapping)[orig_index] == -1)
				(*mapping)[orig_index] = next_index++;

			boundinfo->indexes[i] = (*mapping)[orig_index];
		}
	}

	/* Set the canonical value for default_index, if any. */
	if (default_index != -1)
	{
		Assert(default_index >= 0 && (*mapping)[default_index] == -1);
		(*mapping)[default_index] = next_index++;
		boundinfo->default_index = (*mapping)[default_index];
	}

	/* The extra -1 element. */
	Assert(i == ndatums);
	boundinfo->indexes[i] = -1;

	/* All partitions must now have been assigned canonical indexes. */
	Assert(next_index == nparts);
	return boundinfo;
}

/*
 * Are two partition bound collections logically equal?
 *
 * Used in the keep logic of relcache.c (ie, in RelationClearRelation()).
 * This is also useful when b1 and b2 are bound collections of two separate
 * relations, respectively, because PartitionBoundInfo is a canonical
 * representation of partition bounds.
 */
bool
partition_bounds_equal(int partnatts, int16 *parttyplen, bool *parttypbyval,
					   PartitionBoundInfo b1, PartitionBoundInfo b2)
{
	int			i;

	if (b1->strategy != b2->strategy)
		return false;

	if (b1->ndatums != b2->ndatums)
		return false;

	if (b1->null_index != b2->null_index)
		return false;

	if (b1->default_index != b2->default_index)
		return false;

	if (b1->strategy == PARTITION_STRATEGY_HASH)
	{
		int			greatest_modulus = get_hash_partition_greatest_modulus(b1);

		/*
		 * If two hash partitioned tables have different greatest moduli,
		 * their partition schemes don't match.
		 */
		if (greatest_modulus != get_hash_partition_greatest_modulus(b2))
			return false;

		/*
		 * We arrange the partitions in the ascending order of their moduli
		 * and remainders.  Also every modulus is factor of next larger
		 * modulus.  Therefore we can safely store index of a given partition
		 * in indexes array at remainder of that partition.  Also entries at
		 * (remainder + N * modulus) positions in indexes array are all same
		 * for (modulus, remainder) specification for any partition.  Thus
		 * datums array from both the given bounds are same, if and only if
		 * their indexes array will be same.  So, it suffices to compare
		 * indexes array.
		 */
		for (i = 0; i < greatest_modulus; i++)
			if (b1->indexes[i] != b2->indexes[i])
				return false;

#ifdef USE_ASSERT_CHECKING

		/*
		 * Nonetheless make sure that the bounds are indeed same when the
		 * indexes match.  Hash partition bound stores modulus and remainder
		 * at b1->datums[i][0] and b1->datums[i][1] position respectively.
		 */
		for (i = 0; i < b1->ndatums; i++)
			Assert((b1->datums[i][0] == b2->datums[i][0] &&
					b1->datums[i][1] == b2->datums[i][1]));
#endif
	}
	else
	{
		for (i = 0; i < b1->ndatums; i++)
		{
			int			j;

			for (j = 0; j < partnatts; j++)
			{
				/* For range partitions, the bounds might not be finite. */
				if (b1->kind != NULL)
				{
					/* The different kinds of bound all differ from each other */
					if (b1->kind[i][j] != b2->kind[i][j])
						return false;

					/*
					 * Non-finite bounds are equal without further
					 * examination.
					 */
					if (b1->kind[i][j] != PARTITION_RANGE_DATUM_VALUE)
						continue;
				}

				/*
				 * Compare the actual values. Note that it would be both
				 * incorrect and unsafe to invoke the comparison operator
				 * derived from the partitioning specification here.  It would
				 * be incorrect because we want the relcache entry to be
				 * updated for ANY change to the partition bounds, not just
				 * those that the partitioning operator thinks are
				 * significant.  It would be unsafe because we might reach
				 * this code in the context of an aborted transaction, and an
				 * arbitrary partitioning operator might not be safe in that
				 * context.  datumIsEqual() should be simple enough to be
				 * safe.
				 */
				if (!datumIsEqual(b1->datums[i][j], b2->datums[i][j],
								  parttypbyval[j], parttyplen[j]))
					return false;
			}

			if (b1->indexes[i] != b2->indexes[i])
				return false;
		}

		/* There are ndatums+1 indexes in case of range partitions */
		if (b1->strategy == PARTITION_STRATEGY_RANGE &&
			b1->indexes[i] != b2->indexes[i])
			return false;
	}
	return true;
}

/*
 * Return a copy of given PartitionBoundInfo structure. The data types of bounds
 * are described by given partition key specification.
 *
 * Note: it's important that this function and its callees not do any catalog
 * access, nor anything else that would result in allocating memory other than
 * the returned data structure.  Since this is called in a long-lived context,
 * that would result in unwanted memory leaks.
 */
PartitionBoundInfo
partition_bounds_copy(PartitionBoundInfo src,
					  PartitionKey key)
{
	PartitionBoundInfo dest;
	int			i;
	int			ndatums;
	int			partnatts;
	int			num_indexes;
	bool		hash_part;
	int			natts;

	dest = (PartitionBoundInfo) palloc(sizeof(PartitionBoundInfoData));

	dest->strategy = src->strategy;
	ndatums = dest->ndatums = src->ndatums;
	partnatts = key->partnatts;

	num_indexes = get_partition_bound_num_indexes(src);

	/* List partitioned tables have only a single partition key. */
	Assert(key->strategy != PARTITION_STRATEGY_LIST || partnatts == 1);

	dest->datums = (Datum **) palloc(sizeof(Datum *) * ndatums);

	if (src->kind != NULL)
	{
		dest->kind = (PartitionRangeDatumKind **) palloc(ndatums *
														 sizeof(PartitionRangeDatumKind *));
		for (i = 0; i < ndatums; i++)
		{
			dest->kind[i] = (PartitionRangeDatumKind *) palloc(partnatts *
															   sizeof(PartitionRangeDatumKind));

			memcpy(dest->kind[i], src->kind[i],
				   sizeof(PartitionRangeDatumKind) * key->partnatts);
		}
	}
	else
		dest->kind = NULL;

	/*
	 * For hash partitioning, datums array will have two elements - modulus
	 * and remainder.
	 */
	hash_part = (key->strategy == PARTITION_STRATEGY_HASH);
	natts = hash_part ? 2 : partnatts;

	for (i = 0; i < ndatums; i++)
	{
		int			j;

		dest->datums[i] = (Datum *) palloc(sizeof(Datum) * natts);

		for (j = 0; j < natts; j++)
		{
			bool		byval;
			int			typlen;

			if (hash_part)
			{
				typlen = sizeof(int32); /* Always int4 */
				byval = true;	/* int4 is pass-by-value */
			}
			else
			{
				byval = key->parttypbyval[j];
				typlen = key->parttyplen[j];
			}

			if (dest->kind == NULL ||
				dest->kind[i][j] == PARTITION_RANGE_DATUM_VALUE)
				dest->datums[i][j] = datumCopy(src->datums[i][j],
											   byval, typlen);
		}
	}

	dest->indexes = (int *) palloc(sizeof(int) * num_indexes);
	memcpy(dest->indexes, src->indexes, sizeof(int) * num_indexes);

	dest->null_index = src->null_index;
	dest->default_index = src->default_index;

	return dest;
}

/*
 * partition_bounds_merge
 *		Check to see whether every partition of 'outer_rel' matches/overlaps
 *		one partition of 'inner_rel' at most, and vice versa; and if so, build
 *		and return the partition bounds for a join relation between the rels,
 *		generating two lists of the matching/overlapping partitions, which are
 *		returned to *outer_parts and *inner_parts respectively.
 *
 * The lists contain the same number of partitions, and the partitions at the
 * same positions in the lists indicate join pairs used for partitioned join.
 * If a partition on one side matches/overlaps multiple partitions on the other
 * side, this function returns NULL, setting *outer_parts and *inner_parts to
 * NIL.
 */
PartitionBoundInfo
partition_bounds_merge(int partnatts,
					   FmgrInfo *partsupfunc, Oid *partcollation,
					   RelOptInfo *outer_rel, RelOptInfo *inner_rel,
					   JoinType jointype,
					   List **outer_parts, List **inner_parts)
{
	/*
	 * Currently, this function is called only from try_partitionwise_join(),
	 * so the join type should be INNER, LEFT, FULL, SEMI, or ANTI.
	 */
	Assert(jointype == JOIN_INNER || jointype == JOIN_LEFT ||
		   jointype == JOIN_FULL || jointype == JOIN_SEMI ||
		   jointype == JOIN_ANTI);

	/* The partitioning strategies should be the same. */
	Assert(outer_rel->boundinfo->strategy == inner_rel->boundinfo->strategy);

	*outer_parts = *inner_parts = NIL;
	switch (outer_rel->boundinfo->strategy)
	{
		case PARTITION_STRATEGY_HASH:

			/*
			 * For hash partitioned tables, we currently support partitioned
			 * join only when they have exactly the same partition bounds.
			 *
			 * XXX: it might be possible to relax the restriction to support
			 * cases where hash partitioned tables have missing partitions
			 * and/or different moduli, but it's not clear if it would be
			 * useful to support the former case since it's unusual to have
			 * missing partitions.  On the other hand, it would be useful to
			 * support the latter case, but in that case, there is a high
			 * probability that a partition on one side will match multiple
			 * partitions on the other side, which is the scenario the current
			 * implementation of partitioned join can't handle.
			 */
			return NULL;

		case PARTITION_STRATEGY_LIST:
			return merge_list_bounds(partsupfunc,
									 partcollation,
									 outer_rel,
									 inner_rel,
									 jointype,
									 outer_parts,
									 inner_parts);

		case PARTITION_STRATEGY_RANGE:
			return merge_range_bounds(partnatts,
									  partsupfunc,
									  partcollation,
									  outer_rel,
									  inner_rel,
									  jointype,
									  outer_parts,
									  inner_parts);

		default:
			elog(ERROR, "unexpected partition strategy: %d",
				 (int) outer_rel->boundinfo->strategy);
			return NULL;		/* keep compiler quiet */
	}
}

/*
 * merge_list_bounds
 *		Create the partition bounds for a join relation between list
 *		partitioned tables, if possible
 *
 * In this function we try to find sets of matching partitions from both sides
 * by comparing list values stored in their partition bounds.  Since the list
 * values appear in the ascending order, an algorithm similar to merge join is
 * used for that.  If a partition on one side doesn't have a matching
 * partition on the other side, the algorithm tries to match it with the
 * default partition on the other side if any; if not, the algorithm tries to
 * match it with a dummy partition on the other side if it's on the
 * non-nullable side of an outer join.  Also, if both sides have the default
 * partitions, the algorithm tries to match them with each other.  We give up
 * if the algorithm finds a partition matching multiple partitions on the
 * other side, which is the scenario the current implementation of partitioned
 * join can't handle.
 */
static PartitionBoundInfo
merge_list_bounds(FmgrInfo *partsupfunc, Oid *partcollation,
				  RelOptInfo *outer_rel, RelOptInfo *inner_rel,
				  JoinType jointype,
				  List **outer_parts, List **inner_parts)
{
	PartitionBoundInfo merged_bounds = NULL;
	PartitionBoundInfo outer_bi = outer_rel->boundinfo;
	PartitionBoundInfo inner_bi = inner_rel->boundinfo;
	bool		outer_has_default = partition_bound_has_default(outer_bi);
	bool		inner_has_default = partition_bound_has_default(inner_bi);
	int			outer_default = outer_bi->default_index;
	int			inner_default = inner_bi->default_index;
	bool		outer_has_null = partition_bound_accepts_nulls(outer_bi);
	bool		inner_has_null = partition_bound_accepts_nulls(inner_bi);
	PartitionMap outer_map;
	PartitionMap inner_map;
	int			outer_pos;
	int			inner_pos;
	int			next_index = 0;
	int			null_index = -1;
	int			default_index = -1;
	List	   *merged_datums = NIL;
	List	   *merged_indexes = NIL;

	Assert(*outer_parts == NIL);
	Assert(*inner_parts == NIL);
	Assert(outer_bi->strategy == inner_bi->strategy &&
		   outer_bi->strategy == PARTITION_STRATEGY_LIST);
	/* List partitioning doesn't require kinds. */
	Assert(!outer_bi->kind && !inner_bi->kind);

	init_partition_map(outer_rel, &outer_map);
	init_partition_map(inner_rel, &inner_map);

	/*
	 * If the default partitions (if any) have been proven empty, deem them
	 * non-existent.
	 */
	if (outer_has_default && is_dummy_partition(outer_rel, outer_default))
		outer_has_default = false;
	if (inner_has_default && is_dummy_partition(inner_rel, inner_default))
		inner_has_default = false;

	/*
	 * Merge partitions from both sides.  In each iteration we compare a pair
	 * of list values, one from each side, and decide whether the
	 * corresponding partitions match or not.  If the two values match
	 * exactly, move to the next pair of list values, otherwise move to the
	 * next list value on the side with a smaller list value.
	 */
	outer_pos = inner_pos = 0;
	while (outer_pos < outer_bi->ndatums || inner_pos < inner_bi->ndatums)
	{
		int			outer_index = -1;
		int			inner_index = -1;
		Datum	   *outer_datums;
		Datum	   *inner_datums;
		int			cmpval;
		Datum	   *merged_datum = NULL;
		int			merged_index = -1;

		if (outer_pos < outer_bi->ndatums)
		{
			/*
			 * If the partition on the outer side has been proven empty,
			 * ignore it and move to the next datum on the outer side.
			 */
			outer_index = outer_bi->indexes[outer_pos];
			if (is_dummy_partition(outer_rel, outer_index))
			{
				outer_pos++;
				continue;
			}
		}
		if (inner_pos < inner_bi->ndatums)
		{
			/*
			 * If the partition on the inner side has been proven empty,
			 * ignore it and move to the next datum on the inner side.
			 */
			inner_index = inner_bi->indexes[inner_pos];
			if (is_dummy_partition(inner_rel, inner_index))
			{
				inner_pos++;
				continue;
			}
		}

		/* Get the list values. */
		outer_datums = outer_pos < outer_bi->ndatums ?
			outer_bi->datums[outer_pos] : NULL;
		inner_datums = inner_pos < inner_bi->ndatums ?
			inner_bi->datums[inner_pos] : NULL;

		/*
		 * We run this loop till both sides finish.  This allows us to avoid
		 * duplicating code to handle the remaining values on the side which
		 * finishes later.  For that we set the comparison parameter cmpval in
		 * such a way that it appears as if the side which finishes earlier
		 * has an extra value higher than any other value on the unfinished
		 * side. That way we advance the values on the unfinished side till
		 * all of its values are exhausted.
		 */
		if (outer_pos >= outer_bi->ndatums)
			cmpval = 1;
		else if (inner_pos >= inner_bi->ndatums)
			cmpval = -1;
		else
		{
			Assert(outer_datums != NULL && inner_datums != NULL);
			cmpval = DatumGetInt32(FunctionCall2Coll(&partsupfunc[0],
													 partcollation[0],
													 outer_datums[0],
													 inner_datums[0]));
		}

		if (cmpval == 0)
		{
			/* Two list values match exactly. */
			Assert(outer_pos < outer_bi->ndatums);
			Assert(inner_pos < inner_bi->ndatums);
			Assert(outer_index >= 0);
			Assert(inner_index >= 0);

			/*
			 * Try merging both partitions.  If successful, add the list value
			 * and index of the merged partition below.
			 */
			merged_index = merge_matching_partitions(&outer_map, &inner_map,
													 outer_index, inner_index,
													 &next_index);
			if (merged_index == -1)
				goto cleanup;

			merged_datum = outer_datums;

			/* Move to the next pair of list values. */
			outer_pos++;
			inner_pos++;
		}
		else if (cmpval < 0)
		{
			/* A list value missing from the inner side. */
			Assert(outer_pos < outer_bi->ndatums);

			/*
			 * If the inner side has the default partition, or this is an
			 * outer join, try to assign a merged partition to the outer
			 * partition (see process_outer_partition()).  Otherwise, the
			 * outer partition will not contribute to the result.
			 */
			if (inner_has_default || IS_OUTER_JOIN(jointype))
			{
				/* Get the outer partition. */
				outer_index = outer_bi->indexes[outer_pos];
				Assert(outer_index >= 0);
				merged_index = process_outer_partition(&outer_map,
													   &inner_map,
													   outer_has_default,
													   inner_has_default,
													   outer_index,
													   inner_default,
													   jointype,
													   &next_index,
													   &default_index);
				if (merged_index == -1)
					goto cleanup;
				merged_datum = outer_datums;
			}

			/* Move to the next list value on the outer side. */
			outer_pos++;
		}
		else
		{
			/* A list value missing from the outer side. */
			Assert(cmpval > 0);
			Assert(inner_pos < inner_bi->ndatums);

			/*
			 * If the outer side has the default partition, or this is a FULL
			 * join, try to assign a merged partition to the inner partition
			 * (see process_inner_partition()).  Otherwise, the inner
			 * partition will not contribute to the result.
			 */
			if (outer_has_default || jointype == JOIN_FULL)
			{
				/* Get the inner partition. */
				inner_index = inner_bi->indexes[inner_pos];
				Assert(inner_index >= 0);
				merged_index = process_inner_partition(&outer_map,
													   &inner_map,
													   outer_has_default,
													   inner_has_default,
													   inner_index,
													   outer_default,
													   jointype,
													   &next_index,
													   &default_index);
				if (merged_index == -1)
					goto cleanup;
				merged_datum = inner_datums;
			}

			/* Move to the next list value on the inner side. */
			inner_pos++;
		}

		/*
		 * If we assigned a merged partition, add the list value and index of
		 * the merged partition if appropriate.
		 */
		if (merged_index >= 0 && merged_index != default_index)
		{
			merged_datums = lappend(merged_datums, merged_datum);
			merged_indexes = lappend_int(merged_indexes, merged_index);
		}
	}

	/*
	 * If the NULL partitions (if any) have been proven empty, deem them
	 * non-existent.
	 */
	if (outer_has_null &&
		is_dummy_partition(outer_rel, outer_bi->null_index))
		outer_has_null = false;
	if (inner_has_null &&
		is_dummy_partition(inner_rel, inner_bi->null_index))
		inner_has_null = false;

	/* Merge the NULL partitions if any. */
	if (outer_has_null || inner_has_null)
		merge_null_partitions(&outer_map, &inner_map,
							  outer_has_null, inner_has_null,
							  outer_bi->null_index, inner_bi->null_index,
							  jointype, &next_index, &null_index);
	else
		Assert(null_index == -1);

	/* Merge the default partitions if any. */
	if (outer_has_default || inner_has_default)
		merge_default_partitions(&outer_map, &inner_map,
								 outer_has_default, inner_has_default,
								 outer_default, inner_default,
								 jointype, &next_index, &default_index);
	else
		Assert(default_index == -1);

	/* If we have merged partitions, create the partition bounds. */
	if (next_index > 0)
	{
		/* Fix the merged_indexes list if necessary. */
		if (outer_map.did_remapping || inner_map.did_remapping)
		{
			Assert(jointype == JOIN_FULL);
			fix_merged_indexes(&outer_map, &inner_map,
							   next_index, merged_indexes);
		}

		/* Use maps to match partitions from inputs. */
		generate_matching_part_pairs(outer_rel, inner_rel,
									 &outer_map, &inner_map,
									 next_index,
									 outer_parts, inner_parts);
		Assert(*outer_parts != NIL);
		Assert(*inner_parts != NIL);
		Assert(list_length(*outer_parts) == list_length(*inner_parts));
		Assert(list_length(*outer_parts) <= next_index);

		/* Make a PartitionBoundInfo struct to return. */
		merged_bounds = build_merged_partition_bounds(outer_bi->strategy,
													  merged_datums,
													  NIL,
													  merged_indexes,
													  null_index,
													  default_index);
		Assert(merged_bounds);
	}

cleanup:
	/* Free local memory before returning. */
	list_free(merged_datums);
	list_free(merged_indexes);
	free_partition_map(&outer_map);
	free_partition_map(&inner_map);

	return merged_bounds;
}

/*
 * merge_range_bounds
 *		Create the partition bounds for a join relation between range
 *		partitioned tables, if possible
 *
 * In this function we try to find sets of overlapping partitions from both
 * sides by comparing ranges stored in their partition bounds.  Since the
 * ranges appear in the ascending order, an algorithm similar to merge join is
 * used for that.  If a partition on one side doesn't have an overlapping
 * partition on the other side, the algorithm tries to match it with the
 * default partition on the other side if any; if not, the algorithm tries to
 * match it with a dummy partition on the other side if it's on the
 * non-nullable side of an outer join.  Also, if both sides have the default
 * partitions, the algorithm tries to match them with each other.  We give up
 * if the algorithm finds a partition overlapping multiple partitions on the
 * other side, which is the scenario the current implementation of partitioned
 * join can't handle.
 */
static PartitionBoundInfo
merge_range_bounds(int partnatts, FmgrInfo *partsupfuncs,
				   Oid *partcollations,
				   RelOptInfo *outer_rel, RelOptInfo *inner_rel,
				   JoinType jointype,
				   List **outer_parts, List **inner_parts)
{
	PartitionBoundInfo merged_bounds = NULL;
	PartitionBoundInfo outer_bi = outer_rel->boundinfo;
	PartitionBoundInfo inner_bi = inner_rel->boundinfo;
	bool		outer_has_default = partition_bound_has_default(outer_bi);
	bool		inner_has_default = partition_bound_has_default(inner_bi);
	int			outer_default = outer_bi->default_index;
	int			inner_default = inner_bi->default_index;
	PartitionMap outer_map;
	PartitionMap inner_map;
	int			outer_index;
	int			inner_index;
	int			outer_lb_pos;
	int			inner_lb_pos;
	PartitionRangeBound outer_lb;
	PartitionRangeBound outer_ub;
	PartitionRangeBound inner_lb;
	PartitionRangeBound inner_ub;
	int			next_index = 0;
	int			default_index = -1;
	List	   *merged_datums = NIL;
	List	   *merged_kinds = NIL;
	List	   *merged_indexes = NIL;

	Assert(*outer_parts == NIL);
	Assert(*inner_parts == NIL);
	Assert(outer_bi->strategy == inner_bi->strategy &&
		   outer_bi->strategy == PARTITION_STRATEGY_RANGE);

	init_partition_map(outer_rel, &outer_map);
	init_partition_map(inner_rel, &inner_map);

	/*
	 * If the default partitions (if any) have been proven empty, deem them
	 * non-existent.
	 */
	if (outer_has_default && is_dummy_partition(outer_rel, outer_default))
		outer_has_default = false;
	if (inner_has_default && is_dummy_partition(inner_rel, inner_default))
		inner_has_default = false;

	/*
	 * Merge partitions from both sides.  In each iteration we compare a pair
	 * of ranges, one from each side, and decide whether the corresponding
	 * partitions match or not.  If the two ranges overlap, move to the next
	 * pair of ranges, otherwise move to the next range on the side with a
	 * lower range.  outer_lb_pos/inner_lb_pos keep track of the positions of
	 * lower bounds in the datums arrays in the outer/inner
	 * PartitionBoundInfos respectively.
	 */
	outer_lb_pos = inner_lb_pos = 0;
	outer_index = get_range_partition(outer_rel, outer_bi, &outer_lb_pos,
									  &outer_lb, &outer_ub);
	inner_index = get_range_partition(inner_rel, inner_bi, &inner_lb_pos,
									  &inner_lb, &inner_ub);
	while (outer_index >= 0 || inner_index >= 0)
	{
		bool		overlap;
		int			ub_cmpval;
		int			lb_cmpval;
		PartitionRangeBound merged_lb = {-1, NULL, NULL, true};
		PartitionRangeBound merged_ub = {-1, NULL, NULL, false};
		int			merged_index = -1;

		/*
		 * We run this loop till both sides finish.  This allows us to avoid
		 * duplicating code to handle the remaining ranges on the side which
		 * finishes later.  For that we set the comparison parameter cmpval in
		 * such a way that it appears as if the side which finishes earlier
		 * has an extra range higher than any other range on the unfinished
		 * side. That way we advance the ranges on the unfinished side till
		 * all of its ranges are exhausted.
		 */
		if (outer_index == -1)
		{
			overlap = false;
			lb_cmpval = 1;
			ub_cmpval = 1;
		}
		else if (inner_index == -1)
		{
			overlap = false;
			lb_cmpval = -1;
			ub_cmpval = -1;
		}
		else
			overlap = compare_range_partitions(partnatts, partsupfuncs,
											   partcollations,
											   &outer_lb, &outer_ub,
											   &inner_lb, &inner_ub,
											   &lb_cmpval, &ub_cmpval);

		if (overlap)
		{
			/* Two ranges overlap; form a join pair. */

			PartitionRangeBound save_outer_ub;
			PartitionRangeBound save_inner_ub;

			/* Both partitions should not have been merged yet. */
			Assert(outer_index >= 0);
			Assert(outer_map.merged_indexes[outer_index] == -1 &&
				   outer_map.merged[outer_index] == false);
			Assert(inner_index >= 0);
			Assert(inner_map.merged_indexes[inner_index] == -1 &&
				   inner_map.merged[inner_index] == false);

			/*
			 * Get the index of the merged partition.  Both partitions aren't
			 * merged yet, so the partitions should be merged successfully.
			 */
			merged_index = merge_matching_partitions(&outer_map, &inner_map,
													 outer_index, inner_index,
													 &next_index);
			Assert(merged_index >= 0);

			/* Get the range bounds of the merged partition. */
			get_merged_range_bounds(partnatts, partsupfuncs,
									partcollations, jointype,
									&outer_lb, &outer_ub,
									&inner_lb, &inner_ub,
									lb_cmpval, ub_cmpval,
									&merged_lb, &merged_ub);

			/* Save the upper bounds of both partitions for use below. */
			save_outer_ub = outer_ub;
			save_inner_ub = inner_ub;

			/* Move to the next pair of ranges. */
			outer_index = get_range_partition(outer_rel, outer_bi, &outer_lb_pos,
											  &outer_lb, &outer_ub);
			inner_index = get_range_partition(inner_rel, inner_bi, &inner_lb_pos,
											  &inner_lb, &inner_ub);

			/*
			 * If the range of a partition on one side overlaps the range of
			 * the next partition on the other side, that will cause the
			 * partition on one side to match at least two partitions on the
			 * other side, which is the case that we currently don't support
			 * partitioned join for; give up.
			 */
			if (ub_cmpval > 0 && inner_index >= 0 &&
				compare_range_bounds(partnatts, partsupfuncs, partcollations,
									 &save_outer_ub, &inner_lb) > 0)
				goto cleanup;
			if (ub_cmpval < 0 && outer_index >= 0 &&
				compare_range_bounds(partnatts, partsupfuncs, partcollations,
									 &outer_lb, &save_inner_ub) < 0)
				goto cleanup;

			/*
			 * A row from a non-overlapping portion (if any) of a partition on
			 * one side might find its join partner in the default partition
			 * (if any) on the other side, causing the same situation as
			 * above; give up in that case.
			 */
			if ((outer_has_default && (lb_cmpval > 0 || ub_cmpval < 0)) ||
				(inner_has_default && (lb_cmpval < 0 || ub_cmpval > 0)))
				goto cleanup;
		}
		else if (ub_cmpval < 0)
		{
			/* A non-overlapping outer range. */

			/* The outer partition should not have been merged yet. */
			Assert(outer_index >= 0);
			Assert(outer_map.merged_indexes[outer_index] == -1 &&
				   outer_map.merged[outer_index] == false);

			/*
			 * If the inner side has the default partition, or this is an
			 * outer join, try to assign a merged partition to the outer
			 * partition (see process_outer_partition()).  Otherwise, the
			 * outer partition will not contribute to the result.
			 */
			if (inner_has_default || IS_OUTER_JOIN(jointype))
			{
				merged_index = process_outer_partition(&outer_map,
													   &inner_map,
													   outer_has_default,
													   inner_has_default,
													   outer_index,
													   inner_default,
													   jointype,
													   &next_index,
													   &default_index);
				if (merged_index == -1)
					goto cleanup;
				merged_lb = outer_lb;
				merged_ub = outer_ub;
			}

			/* Move to the next range on the outer side. */
			outer_index = get_range_partition(outer_rel, outer_bi, &outer_lb_pos,
											  &outer_lb, &outer_ub);
		}
		else
		{
			/* A non-overlapping inner range. */
			Assert(ub_cmpval > 0);

			/* The inner partition should not have been merged yet. */
			Assert(inner_index >= 0);
			Assert(inner_map.merged_indexes[inner_index] == -1 &&
				   inner_map.merged[inner_index] == false);

			/*
			 * If the outer side has the default partition, or this is a FULL
			 * join, try to assign a merged partition to the inner partition
			 * (see process_inner_partition()).  Otherwise, the inner
			 * partition will not contribute to the result.
			 */
			if (outer_has_default || jointype == JOIN_FULL)
			{
				merged_index = process_inner_partition(&outer_map,
													   &inner_map,
													   outer_has_default,
													   inner_has_default,
													   inner_index,
													   outer_default,
													   jointype,
													   &next_index,
													   &default_index);
				if (merged_index == -1)
					goto cleanup;
				merged_lb = inner_lb;
				merged_ub = inner_ub;
			}

			/* Move to the next range on the inner side. */
			inner_index = get_range_partition(inner_rel, inner_bi, &inner_lb_pos,
											  &inner_lb, &inner_ub);
		}

		/*
		 * If we assigned a merged partition, add the range bounds and index
		 * of the merged partition if appropriate.
		 */
		if (merged_index >= 0 && merged_index != default_index)
			add_merged_range_bounds(partnatts, partsupfuncs, partcollations,
									&merged_lb, &merged_ub, merged_index,
									&merged_datums, &merged_kinds,
									&merged_indexes);
	}

	/* Merge the default partitions if any. */
	if (outer_has_default || inner_has_default)
		merge_default_partitions(&outer_map, &inner_map,
								 outer_has_default, inner_has_default,
								 outer_default, inner_default,
								 jointype, &next_index, &default_index);
	else
		Assert(default_index == -1);

	/* If we have merged partitions, create the partition bounds. */
	if (next_index > 0)
	{
		/*
		 * Unlike the case of list partitioning, we wouldn't have re-merged
		 * partitions, so did_remapping should be left alone.
		 */
		Assert(!outer_map.did_remapping);
		Assert(!inner_map.did_remapping);

		/* Use maps to match partitions from inputs. */
		generate_matching_part_pairs(outer_rel, inner_rel,
									 &outer_map, &inner_map,
									 next_index,
									 outer_parts, inner_parts);
		Assert(*outer_parts != NIL);
		Assert(*inner_parts != NIL);
		Assert(list_length(*outer_parts) == list_length(*inner_parts));
		Assert(list_length(*outer_parts) == next_index);

		/* Make a PartitionBoundInfo struct to return. */
		merged_bounds = build_merged_partition_bounds(outer_bi->strategy,
													  merged_datums,
													  merged_kinds,
													  merged_indexes,
													  -1,
													  default_index);
		Assert(merged_bounds);
	}

cleanup:
	/* Free local memory before returning. */
	list_free(merged_datums);
	list_free(merged_kinds);
	list_free(merged_indexes);
	free_partition_map(&outer_map);
	free_partition_map(&inner_map);

	return merged_bounds;
}

/*
 * init_partition_map
 *		Initialize a PartitionMap struct for given relation
 */
static void
init_partition_map(RelOptInfo *rel, PartitionMap *map)
{
	int			nparts = rel->nparts;
	int			i;

	map->nparts = nparts;
	map->merged_indexes = (int *) palloc(sizeof(int) * nparts);
	map->merged = (bool *) palloc(sizeof(bool) * nparts);
	map->did_remapping = false;
	map->old_indexes = (int *) palloc(sizeof(int) * nparts);
	for (i = 0; i < nparts; i++)
	{
		map->merged_indexes[i] = map->old_indexes[i] = -1;
		map->merged[i] = false;
	}
}

/*
 * free_partition_map
 */
static void
free_partition_map(PartitionMap *map)
{
	pfree(map->merged_indexes);
	pfree(map->merged);
	pfree(map->old_indexes);
}

/*
 * is_dummy_partition --- has partition been proven empty?
 */
static bool
is_dummy_partition(RelOptInfo *rel, int part_index)
{
	RelOptInfo *part_rel;

	Assert(part_index >= 0);
	part_rel = rel->part_rels[part_index];
	if (part_rel == NULL || IS_DUMMY_REL(part_rel))
		return true;
	return false;
}

/*
 * merge_matching_partitions
 *		Try to merge given outer/inner partitions, and return the index of a
 *		merged partition produced from them if successful, -1 otherwise
 *
 * If the merged partition is newly created, *next_index is incremented.
 */
static int
merge_matching_partitions(PartitionMap *outer_map, PartitionMap *inner_map,
						  int outer_index, int inner_index, int *next_index)
{
	int			outer_merged_index;
	int			inner_merged_index;
	bool		outer_merged;
	bool		inner_merged;

	Assert(outer_index >= 0 && outer_index < outer_map->nparts);
	outer_merged_index = outer_map->merged_indexes[outer_index];
	outer_merged = outer_map->merged[outer_index];
	Assert(inner_index >= 0 && inner_index < inner_map->nparts);
	inner_merged_index = inner_map->merged_indexes[inner_index];
	inner_merged = inner_map->merged[inner_index];

	/*
	 * Handle cases where we have already assigned a merged partition to each
	 * of the given partitions.
	 */
	if (outer_merged_index >= 0 && inner_merged_index >= 0)
	{
		/*
		 * If the merged partitions are the same, no need to do anything;
		 * return the index of the merged partitions.  Otherwise, if each of
		 * the given partitions has been merged with a dummy partition on the
		 * other side, re-map them to either of the two merged partitions.
		 * Otherwise, they can't be merged, so return -1.
		 */
		if (outer_merged_index == inner_merged_index)
		{
			Assert(outer_merged);
			Assert(inner_merged);
			return outer_merged_index;
		}
		if (!outer_merged && !inner_merged)
		{
			/*
			 * This can only happen for a list-partitioning case.  We re-map
			 * them to the merged partition with the smaller of the two merged
			 * indexes to preserve the property that the canonical order of
			 * list partitions is determined by the indexes assigned to the
			 * smallest list value of each partition.
			 */
			if (outer_merged_index < inner_merged_index)
			{
				outer_map->merged[outer_index] = true;
				inner_map->merged_indexes[inner_index] = outer_merged_index;
				inner_map->merged[inner_index] = true;
				inner_map->did_remapping = true;
				inner_map->old_indexes[inner_index] = inner_merged_index;
				return outer_merged_index;
			}
			else
			{
				inner_map->merged[inner_index] = true;
				outer_map->merged_indexes[outer_index] = inner_merged_index;
				outer_map->merged[outer_index] = true;
				outer_map->did_remapping = true;
				outer_map->old_indexes[outer_index] = outer_merged_index;
				return inner_merged_index;
			}
		}
		return -1;
	}

	/* At least one of the given partitions should not have yet been merged. */
	Assert(outer_merged_index == -1 || inner_merged_index == -1);

	/*
	 * If neither of them has been merged, merge them.  Otherwise, if one has
	 * been merged with a dummy partition on the other side (and the other
	 * hasn't yet been merged with anything), re-merge them.  Otherwise, they
	 * can't be merged, so return -1.
	 */
	if (outer_merged_index == -1 && inner_merged_index == -1)
	{
		int			merged_index = *next_index;

		Assert(!outer_merged);
		Assert(!inner_merged);
		outer_map->merged_indexes[outer_index] = merged_index;
		outer_map->merged[outer_index] = true;
		inner_map->merged_indexes[inner_index] = merged_index;
		inner_map->merged[inner_index] = true;
		*next_index = *next_index + 1;
		return merged_index;
	}
	if (outer_merged_index >= 0 && !outer_map->merged[outer_index])
	{
		Assert(inner_merged_index == -1);
		Assert(!inner_merged);
		inner_map->merged_indexes[inner_index] = outer_merged_index;
		inner_map->merged[inner_index] = true;
		outer_map->merged[outer_index] = true;
		return outer_merged_index;
	}
	if (inner_merged_index >= 0 && !inner_map->merged[inner_index])
	{
		Assert(outer_merged_index == -1);
		Assert(!outer_merged);
		outer_map->merged_indexes[outer_index] = inner_merged_index;
		outer_map->merged[outer_index] = true;
		inner_map->merged[inner_index] = true;
		return inner_merged_index;
	}
	return -1;
}

/*
 * process_outer_partition
 *		Try to assign given outer partition a merged partition, and return the
 *		index of the merged partition if successful, -1 otherwise
 *
 * If the partition is newly created, *next_index is incremented.  Also, if it
 * is the default partition of the join relation, *default_index is set to the
 * index if not already done.
 */
static int
process_outer_partition(PartitionMap *outer_map,
						PartitionMap *inner_map,
						bool outer_has_default,
						bool inner_has_default,
						int outer_index,
						int inner_default,
						JoinType jointype,
						int *next_index,
						int *default_index)
{
	int			merged_index = -1;

	Assert(outer_index >= 0);

	/*
	 * If the inner side has the default partition, a row from the outer
	 * partition might find its join partner in the default partition; try
	 * merging the outer partition with the default partition.  Otherwise,
	 * this should be an outer join, in which case the outer partition has to
	 * be scanned all the way anyway; merge the outer partition with a dummy
	 * partition on the other side.
	 */
	if (inner_has_default)
	{
		Assert(inner_default >= 0);

		/*
		 * If the outer side has the default partition as well, the default
		 * partition on the inner side will have two matching partitions on
		 * the other side: the outer partition and the default partition on
		 * the outer side.  Partitionwise join doesn't handle this scenario
		 * yet.
		 */
		if (outer_has_default)
			return -1;

		merged_index = merge_matching_partitions(outer_map, inner_map,
												 outer_index, inner_default,
												 next_index);
		if (merged_index == -1)
			return -1;

		/*
		 * If this is a FULL join, the default partition on the inner side has
		 * to be scanned all the way anyway, so the resulting partition will
		 * contain all key values from the default partition, which any other
		 * partition of the join relation will not contain.  Thus the
		 * resulting partition will act as the default partition of the join
		 * relation; record the index in *default_index if not already done.
		 */
		if (jointype == JOIN_FULL)
		{
			if (*default_index == -1)
				*default_index = merged_index;
			else
				Assert(*default_index == merged_index);
		}
	}
	else
	{
		Assert(IS_OUTER_JOIN(jointype));
		Assert(jointype != JOIN_RIGHT);

		/* If we have already assigned a partition, no need to do anything. */
		merged_index = outer_map->merged_indexes[outer_index];
		if (merged_index == -1)
			merged_index = merge_partition_with_dummy(outer_map, outer_index,
													  next_index);
	}
	return merged_index;
}

/*
 * process_inner_partition
 *		Try to assign given inner partition a merged partition, and return the
 *		index of the merged partition if successful, -1 otherwise
 *
 * If the partition is newly created, *next_index is incremented.  Also, if it
 * is the default partition of the join relation, *default_index is set to the
 * index if not already done.
 */
static int
process_inner_partition(PartitionMap *outer_map,
						PartitionMap *inner_map,
						bool outer_has_default,
						bool inner_has_default,
						int inner_index,
						int outer_default,
						JoinType jointype,
						int *next_index,
						int *default_index)
{
	int			merged_index = -1;

	Assert(inner_index >= 0);

	/*
	 * If the outer side has the default partition, a row from the inner
	 * partition might find its join partner in the default partition; try
	 * merging the inner partition with the default partition.  Otherwise,
	 * this should be a FULL join, in which case the inner partition has to be
	 * scanned all the way anyway; merge the inner partition with a dummy
	 * partition on the other side.
	 */
	if (outer_has_default)
	{
		Assert(outer_default >= 0);

		/*
		 * If the inner side has the default partition as well, the default
		 * partition on the outer side will have two matching partitions on
		 * the other side: the inner partition and the default partition on
		 * the inner side.  Partitionwise join doesn't handle this scenario
		 * yet.
		 */
		if (inner_has_default)
			return -1;

		merged_index = merge_matching_partitions(outer_map, inner_map,
												 outer_default, inner_index,
												 next_index);
		if (merged_index == -1)
			return -1;

		/*
		 * If this is an outer join, the default partition on the outer side
		 * has to be scanned all the way anyway, so the resulting partition
		 * will contain all key values from the default partition, which any
		 * other partition of the join relation will not contain.  Thus the
		 * resulting partition will act as the default partition of the join
		 * relation; record the index in *default_index if not already done.
		 */
		if (IS_OUTER_JOIN(jointype))
		{
			Assert(jointype != JOIN_RIGHT);
			if (*default_index == -1)
				*default_index = merged_index;
			else
				Assert(*default_index == merged_index);
		}
	}
	else
	{
		Assert(jointype == JOIN_FULL);

		/* If we have already assigned a partition, no need to do anything. */
		merged_index = inner_map->merged_indexes[inner_index];
		if (merged_index == -1)
			merged_index = merge_partition_with_dummy(inner_map, inner_index,
													  next_index);
	}
	return merged_index;
}

/*
 * merge_null_partitions
 *		Merge the NULL partitions from a join's outer and inner sides.
 *
 * If the merged partition produced from them is the NULL partition of the join
 * relation, *null_index is set to the index of the merged partition.
 *
 * Note: We assume here that the join clause for a partitioned join is strict
 * because have_partkey_equi_join() requires that the corresponding operator
 * be mergejoinable, and we currently assume that mergejoinable operators are
 * strict (see MJEvalOuterValues()/MJEvalInnerValues()).
 */
static void
merge_null_partitions(PartitionMap *outer_map,
					  PartitionMap *inner_map,
					  bool outer_has_null,
					  bool inner_has_null,
					  int outer_null,
					  int inner_null,
					  JoinType jointype,
					  int *next_index,
					  int *null_index)
{
	bool		consider_outer_null = false;
	bool		consider_inner_null = false;

	Assert(outer_has_null || inner_has_null);
	Assert(*null_index == -1);

	/*
	 * Check whether the NULL partitions have already been merged and if so,
	 * set the consider_outer_null/consider_inner_null flags.
	 */
	if (outer_has_null)
	{
		Assert(outer_null >= 0 && outer_null < outer_map->nparts);
		if (outer_map->merged_indexes[outer_null] == -1)
			consider_outer_null = true;
	}
	if (inner_has_null)
	{
		Assert(inner_null >= 0 && inner_null < inner_map->nparts);
		if (inner_map->merged_indexes[inner_null] == -1)
			consider_inner_null = true;
	}

	/* If both flags are set false, we don't need to do anything. */
	if (!consider_outer_null && !consider_inner_null)
		return;

	if (consider_outer_null && !consider_inner_null)
	{
		Assert(outer_has_null);

		/*
		 * If this is an outer join, the NULL partition on the outer side has
		 * to be scanned all the way anyway; merge the NULL partition with a
		 * dummy partition on the other side.  In that case
		 * consider_outer_null means that the NULL partition only contains
		 * NULL values as the key values, so the merged partition will do so;
		 * treat it as the NULL partition of the join relation.
		 */
		if (IS_OUTER_JOIN(jointype))
		{
			Assert(jointype != JOIN_RIGHT);
			*null_index = merge_partition_with_dummy(outer_map, outer_null,
													 next_index);
		}
	}
	else if (!consider_outer_null && consider_inner_null)
	{
		Assert(inner_has_null);

		/*
		 * If this is a FULL join, the NULL partition on the inner side has to
		 * be scanned all the way anyway; merge the NULL partition with a
		 * dummy partition on the other side.  In that case
		 * consider_inner_null means that the NULL partition only contains
		 * NULL values as the key values, so the merged partition will do so;
		 * treat it as the NULL partition of the join relation.
		 */
		if (jointype == JOIN_FULL)
			*null_index = merge_partition_with_dummy(inner_map, inner_null,
													 next_index);
	}
	else
	{
		Assert(consider_outer_null && consider_inner_null);
		Assert(outer_has_null);
		Assert(inner_has_null);

		/*
		 * If this is an outer join, the NULL partition on the outer side (and
		 * that on the inner side if this is a FULL join) have to be scanned
		 * all the way anyway, so merge them.  Note that each of the NULL
		 * partitions isn't merged yet, so they should be merged successfully.
		 * Like the above, each of the NULL partitions only contains NULL
		 * values as the key values, so the merged partition will do so; treat
		 * it as the NULL partition of the join relation.
		 *
		 * Note: if this an INNER/SEMI join, the join clause will never be
		 * satisfied by two NULL values (see comments above), so both the NULL
		 * partitions can be eliminated.
		 */
		if (IS_OUTER_JOIN(jointype))
		{
			Assert(jointype != JOIN_RIGHT);
			*null_index = merge_matching_partitions(outer_map, inner_map,
													outer_null, inner_null,
													next_index);
			Assert(*null_index >= 0);
		}
	}
}

/*
 * merge_default_partitions
 *		Merge the default partitions from a join's outer and inner sides.
 *
 * If the merged partition produced from them is the default partition of the
 * join relation, *default_index is set to the index of the merged partition.
 */
static void
merge_default_partitions(PartitionMap *outer_map,
						 PartitionMap *inner_map,
						 bool outer_has_default,
						 bool inner_has_default,
						 int outer_default,
						 int inner_default,
						 JoinType jointype,
						 int *next_index,
						 int *default_index)
{
	int			outer_merged_index = -1;
	int			inner_merged_index = -1;

	Assert(outer_has_default || inner_has_default);

	/* Get the merged partition indexes for the default partitions. */
	if (outer_has_default)
	{
		Assert(outer_default >= 0 && outer_default < outer_map->nparts);
		outer_merged_index = outer_map->merged_indexes[outer_default];
	}
	if (inner_has_default)
	{
		Assert(inner_default >= 0 && inner_default < inner_map->nparts);
		inner_merged_index = inner_map->merged_indexes[inner_default];
	}

	if (outer_has_default && !inner_has_default)
	{
		/*
		 * If this is an outer join, the default partition on the outer side
		 * has to be scanned all the way anyway; if we have not yet assigned a
		 * partition, merge the default partition with a dummy partition on
		 * the other side.  The merged partition will act as the default
		 * partition of the join relation (see comments in
		 * process_inner_partition()).
		 */
		if (IS_OUTER_JOIN(jointype))
		{
			Assert(jointype != JOIN_RIGHT);
			if (outer_merged_index == -1)
			{
				Assert(*default_index == -1);
				*default_index = merge_partition_with_dummy(outer_map,
															outer_default,
															next_index);
			}
			else
				Assert(*default_index == outer_merged_index);
		}
		else
			Assert(*default_index == -1);
	}
	else if (!outer_has_default && inner_has_default)
	{
		/*
		 * If this is a FULL join, the default partition on the inner side has
		 * to be scanned all the way anyway; if we have not yet assigned a
		 * partition, merge the default partition with a dummy partition on
		 * the other side.  The merged partition will act as the default
		 * partition of the join relation (see comments in
		 * process_outer_partition()).
		 */
		if (jointype == JOIN_FULL)
		{
			if (inner_merged_index == -1)
			{
				Assert(*default_index == -1);
				*default_index = merge_partition_with_dummy(inner_map,
															inner_default,
															next_index);
			}
			else
				Assert(*default_index == inner_merged_index);
		}
		else
			Assert(*default_index == -1);
	}
	else
	{
		Assert(outer_has_default && inner_has_default);

		/*
		 * The default partitions have to be joined with each other, so merge
		 * them.  Note that each of the default partitions isn't merged yet
		 * (see, process_outer_partition()/process_innerer_partition()), so
		 * they should be merged successfully.  The merged partition will act
		 * as the default partition of the join relation.
		 */
		Assert(outer_merged_index == -1);
		Assert(inner_merged_index == -1);
		Assert(*default_index == -1);
		*default_index = merge_matching_partitions(outer_map,
												   inner_map,
												   outer_default,
												   inner_default,
												   next_index);
		Assert(*default_index >= 0);
	}
}

/*
 * merge_partition_with_dummy
 *		Assign given partition a new partition of a join relation
 *
 * Note: The caller assumes that the given partition doesn't have a non-dummy
 * matching partition on the other side, but if the given partition finds the
 * matching partition later, we will adjust the assignment.
 */
static int
merge_partition_with_dummy(PartitionMap *map, int index, int *next_index)
{
	int			merged_index = *next_index;

	Assert(index >= 0 && index < map->nparts);
	Assert(map->merged_indexes[index] == -1);
	Assert(!map->merged[index]);
	map->merged_indexes[index] = merged_index;
	/* Leave the merged flag alone! */
	*next_index = *next_index + 1;
	return merged_index;
}

/*
 * fix_merged_indexes
 *		Adjust merged indexes of re-merged partitions
 */
static void
fix_merged_indexes(PartitionMap *outer_map, PartitionMap *inner_map,
				   int nmerged, List *merged_indexes)
{
	int		   *new_indexes;
	int			merged_index;
	int			i;
	ListCell   *lc;

	Assert(nmerged > 0);

	new_indexes = (int *) palloc(sizeof(int) * nmerged);
	for (i = 0; i < nmerged; i++)
		new_indexes[i] = -1;

	/* Build the mapping of old merged indexes to new merged indexes. */
	if (outer_map->did_remapping)
	{
		for (i = 0; i < outer_map->nparts; i++)
		{
			merged_index = outer_map->old_indexes[i];
			if (merged_index >= 0)
				new_indexes[merged_index] = outer_map->merged_indexes[i];
		}
	}
	if (inner_map->did_remapping)
	{
		for (i = 0; i < inner_map->nparts; i++)
		{
			merged_index = inner_map->old_indexes[i];
			if (merged_index >= 0)
				new_indexes[merged_index] = inner_map->merged_indexes[i];
		}
	}

	/* Fix the merged_indexes list using the mapping. */
	foreach(lc, merged_indexes)
	{
		merged_index = lfirst_int(lc);
		Assert(merged_index >= 0);
		if (new_indexes[merged_index] >= 0)
			lfirst_int(lc) = new_indexes[merged_index];
	}

	pfree(new_indexes);
}

/*
 * generate_matching_part_pairs
 *		Generate a pair of lists of partitions that produce merged partitions
 *
 * The lists of partitions are built in the order of merged partition indexes,
 * and returned in *outer_parts and *inner_parts.
 */
static void
generate_matching_part_pairs(RelOptInfo *outer_rel, RelOptInfo *inner_rel,
							 PartitionMap *outer_map, PartitionMap *inner_map,
							 int nmerged,
							 List **outer_parts, List **inner_parts)
{
	int			outer_nparts = outer_map->nparts;
	int			inner_nparts = inner_map->nparts;
	int		   *outer_indexes;
	int		   *inner_indexes;
	int			max_nparts;
	int			i;

	Assert(nmerged > 0);
	Assert(*outer_parts == NIL);
	Assert(*inner_parts == NIL);

	outer_indexes = (int *) palloc(sizeof(int) * nmerged);
	inner_indexes = (int *) palloc(sizeof(int) * nmerged);
	for (i = 0; i < nmerged; i++)
		outer_indexes[i] = inner_indexes[i] = -1;

	/* Set pairs of matching partitions. */
	Assert(outer_nparts == outer_rel->nparts);
	Assert(inner_nparts == inner_rel->nparts);
	max_nparts = Max(outer_nparts, inner_nparts);
	for (i = 0; i < max_nparts; i++)
	{
		if (i < outer_nparts)
		{
			int			merged_index = outer_map->merged_indexes[i];

			if (merged_index >= 0)
			{
				Assert(merged_index < nmerged);
				outer_indexes[merged_index] = i;
			}
		}
		if (i < inner_nparts)
		{
			int			merged_index = inner_map->merged_indexes[i];

			if (merged_index >= 0)
			{
				Assert(merged_index < nmerged);
				inner_indexes[merged_index] = i;
			}
		}
	}

	/* Build the list pairs. */
	for (i = 0; i < nmerged; i++)
	{
		int			outer_index = outer_indexes[i];
		int			inner_index = inner_indexes[i];

		/*
		 * If both partitions are dummy, it means the merged partition that
		 * had been assigned to the outer/inner partition was removed when
		 * re-merging the outer/inner partition in
		 * merge_matching_partitions(); ignore the merged partition.
		 */
		if (outer_index == -1 && inner_index == -1)
			continue;

		*outer_parts = lappend(*outer_parts, outer_index >= 0 ?
							   outer_rel->part_rels[outer_index] : NULL);
		*inner_parts = lappend(*inner_parts, inner_index >= 0 ?
							   inner_rel->part_rels[inner_index] : NULL);
	}

	pfree(outer_indexes);
	pfree(inner_indexes);
}

/*
 * build_merged_partition_bounds
 *		Create a PartitionBoundInfo struct from merged partition bounds
 */
static PartitionBoundInfo
build_merged_partition_bounds(char strategy, List *merged_datums,
							  List *merged_kinds, List *merged_indexes,
							  int null_index, int default_index)
{
	PartitionBoundInfo merged_bounds;
	int			ndatums = list_length(merged_datums);
	int			pos;
	ListCell   *lc;

	merged_bounds = (PartitionBoundInfo) palloc(sizeof(PartitionBoundInfoData));
	merged_bounds->strategy = strategy;
	merged_bounds->ndatums = ndatums;

	merged_bounds->datums = (Datum **) palloc(sizeof(Datum *) * ndatums);
	pos = 0;
	foreach(lc, merged_datums)
		merged_bounds->datums[pos++] = (Datum *) lfirst(lc);

	if (strategy == PARTITION_STRATEGY_RANGE)
	{
		Assert(list_length(merged_kinds) == ndatums);
		merged_bounds->kind = (PartitionRangeDatumKind **)
			palloc(sizeof(PartitionRangeDatumKind *) * ndatums);
		pos = 0;
		foreach(lc, merged_kinds)
			merged_bounds->kind[pos++] = (PartitionRangeDatumKind *) lfirst(lc);

		/* There are ndatums+1 indexes in the case of range partitioning. */
		merged_indexes = lappend_int(merged_indexes, -1);
		ndatums++;
	}
	else
	{
		Assert(strategy == PARTITION_STRATEGY_LIST);
		Assert(merged_kinds == NIL);
		merged_bounds->kind = NULL;
	}

	Assert(list_length(merged_indexes) == ndatums);
	merged_bounds->indexes = (int *) palloc(sizeof(int) * ndatums);
	pos = 0;
	foreach(lc, merged_indexes)
		merged_bounds->indexes[pos++] = lfirst_int(lc);

	merged_bounds->null_index = null_index;
	merged_bounds->default_index = default_index;

	return merged_bounds;
}

/*
 * get_range_partition
 *		Get the next non-dummy partition of a range-partitioned relation,
 *		returning the index of that partition
 *
 * *lb and *ub are set to the lower and upper bounds of that partition
 * respectively, and *lb_pos is advanced to the next lower bound, if any.
 */
static int
get_range_partition(RelOptInfo *rel,
					PartitionBoundInfo bi,
					int *lb_pos,
					PartitionRangeBound *lb,
					PartitionRangeBound *ub)
{
	int			part_index;

	Assert(bi->strategy == PARTITION_STRATEGY_RANGE);

	do
	{
		part_index = get_range_partition_internal(bi, lb_pos, lb, ub);
		if (part_index == -1)
			return -1;
	} while (is_dummy_partition(rel, part_index));

	return part_index;
}

static int
get_range_partition_internal(PartitionBoundInfo bi,
							 int *lb_pos,
							 PartitionRangeBound *lb,
							 PartitionRangeBound *ub)
{
	/* Return the index as -1 if we've exhausted all lower bounds. */
	if (*lb_pos >= bi->ndatums)
		return -1;

	/* A lower bound should have at least one more bound after it. */
	Assert(*lb_pos + 1 < bi->ndatums);

	/* Set the lower bound. */
	lb->index = bi->indexes[*lb_pos];
	lb->datums = bi->datums[*lb_pos];
	lb->kind = bi->kind[*lb_pos];
	lb->lower = true;
	/* Set the upper bound. */
	ub->index = bi->indexes[*lb_pos + 1];
	ub->datums = bi->datums[*lb_pos + 1];
	ub->kind = bi->kind[*lb_pos + 1];
	ub->lower = false;

	/* The index assigned to an upper bound should be valid. */
	Assert(ub->index >= 0);

	/*
	 * Advance the position to the next lower bound.  If there are no bounds
	 * left beyond the upper bound, we have reached the last lower bound.
	 */
	if (*lb_pos + 2 >= bi->ndatums)
		*lb_pos = bi->ndatums;
	else
	{
		/*
		 * If the index assigned to the bound next to the upper bound isn't
		 * valid, that is the next lower bound; else, the upper bound is also
		 * the lower bound of the next range partition.
		 */
		if (bi->indexes[*lb_pos + 2] < 0)
			*lb_pos = *lb_pos + 2;
		else
			*lb_pos = *lb_pos + 1;
	}

	return ub->index;
}

/*
 * compare_range_partitions
 *		Compare the bounds of two range partitions, and return true if the
 *		two partitions overlap, false otherwise
 *
 * *lb_cmpval is set to -1, 0, or 1 if the outer partition's lower bound is
 * lower than, equal to, or higher than the inner partition's lower bound
 * respectively.  Likewise, *ub_cmpval is set to -1, 0, or 1 if the outer
 * partition's upper bound is lower than, equal to, or higher than the inner
 * partition's upper bound respectively.
 */
static bool
compare_range_partitions(int partnatts, FmgrInfo *partsupfuncs,
						 Oid *partcollations,
						 PartitionRangeBound *outer_lb,
						 PartitionRangeBound *outer_ub,
						 PartitionRangeBound *inner_lb,
						 PartitionRangeBound *inner_ub,
						 int *lb_cmpval, int *ub_cmpval)
{
	/*
	 * Check if the outer partition's upper bound is lower than the inner
	 * partition's lower bound; if so the partitions aren't overlapping.
	 */
	if (compare_range_bounds(partnatts, partsupfuncs, partcollations,
							 outer_ub, inner_lb) < 0)
	{
		*lb_cmpval = -1;
		*ub_cmpval = -1;
		return false;
	}

	/*
	 * Check if the outer partition's lower bound is higher than the inner
	 * partition's upper bound; if so the partitions aren't overlapping.
	 */
	if (compare_range_bounds(partnatts, partsupfuncs, partcollations,
							 outer_lb, inner_ub) > 0)
	{
		*lb_cmpval = 1;
		*ub_cmpval = 1;
		return false;
	}

	/* All other cases indicate overlapping partitions. */
	*lb_cmpval = compare_range_bounds(partnatts, partsupfuncs, partcollations,
									  outer_lb, inner_lb);
	*ub_cmpval = compare_range_bounds(partnatts, partsupfuncs, partcollations,
									  outer_ub, inner_ub);
	return true;
}

/*
 * get_merged_range_bounds
 *		Given the bounds of range partitions to be joined, determine the bounds
 *		of a merged partition produced from the range partitions
 *
 * *merged_lb and *merged_ub are set to the lower and upper bounds of the
 * merged partition.
 */
static void
get_merged_range_bounds(int partnatts, FmgrInfo *partsupfuncs,
						Oid *partcollations, JoinType jointype,
						PartitionRangeBound *outer_lb,
						PartitionRangeBound *outer_ub,
						PartitionRangeBound *inner_lb,
						PartitionRangeBound *inner_ub,
						int lb_cmpval, int ub_cmpval,
						PartitionRangeBound *merged_lb,
						PartitionRangeBound *merged_ub)
{
	Assert(compare_range_bounds(partnatts, partsupfuncs, partcollations,
								outer_lb, inner_lb) == lb_cmpval);
	Assert(compare_range_bounds(partnatts, partsupfuncs, partcollations,
								outer_ub, inner_ub) == ub_cmpval);

	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_SEMI:

			/*
			 * An INNER/SEMI join will have the rows that fit both sides, so
			 * the lower bound of the merged partition will be the higher of
			 * the two lower bounds, and the upper bound of the merged
			 * partition will be the lower of the two upper bounds.
			 */
			*merged_lb = (lb_cmpval > 0) ? *outer_lb : *inner_lb;
			*merged_ub = (ub_cmpval < 0) ? *outer_ub : *inner_ub;
			break;

		case JOIN_LEFT:
		case JOIN_ANTI:

			/*
			 * A LEFT/ANTI join will have all the rows from the outer side, so
			 * the bounds of the merged partition will be the same as the
			 * outer bounds.
			 */
			*merged_lb = *outer_lb;
			*merged_ub = *outer_ub;
			break;

		case JOIN_FULL:

			/*
			 * A FULL join will have all the rows from both sides, so the
			 * lower bound of the merged partition will be the lower of the
			 * two lower bounds, and the upper bound of the merged partition
			 * will be the higher of the two upper bounds.
			 */
			*merged_lb = (lb_cmpval < 0) ? *outer_lb : *inner_lb;
			*merged_ub = (ub_cmpval > 0) ? *outer_ub : *inner_ub;
			break;

		default:
			elog(ERROR, "unrecognized join type: %d", (int) jointype);
	}
}

/*
 * add_merged_range_bounds
 *		Add the bounds of a merged partition to the lists of range bounds
 */
static void
add_merged_range_bounds(int partnatts, FmgrInfo *partsupfuncs,
						Oid *partcollations,
						PartitionRangeBound *merged_lb,
						PartitionRangeBound *merged_ub,
						int merged_index,
						List **merged_datums,
						List **merged_kinds,
						List **merged_indexes)
{
	int			cmpval;

	if (!*merged_datums)
	{
		/* First merged partition */
		Assert(!*merged_kinds);
		Assert(!*merged_indexes);
		cmpval = 1;
	}
	else
	{
		PartitionRangeBound prev_ub;

		Assert(*merged_datums);
		Assert(*merged_kinds);
		Assert(*merged_indexes);

		/* Get the last upper bound. */
		prev_ub.index = llast_int(*merged_indexes);
		prev_ub.datums = (Datum *) llast(*merged_datums);
		prev_ub.kind = (PartitionRangeDatumKind *) llast(*merged_kinds);
		prev_ub.lower = false;

		/*
		 * We pass lower1 = false to partition_rbound_cmp() to prevent it from
		 * considering the last upper bound to be smaller than the lower bound
		 * of the merged partition when the values of the two range bounds
		 * compare equal.
		 */
		cmpval = partition_rbound_cmp(partnatts, partsupfuncs, partcollations,
									  merged_lb->datums, merged_lb->kind,
									  false, &prev_ub);
		Assert(cmpval >= 0);
	}

	/*
	 * If the lower bound is higher than the last upper bound, add the lower
	 * bound with the index as -1 indicating that that is a lower bound; else,
	 * the last upper bound will be reused as the lower bound of the merged
	 * partition, so skip this.
	 */
	if (cmpval > 0)
	{
		*merged_datums = lappend(*merged_datums, merged_lb->datums);
		*merged_kinds = lappend(*merged_kinds, merged_lb->kind);
		*merged_indexes = lappend_int(*merged_indexes, -1);
	}

	/* Add the upper bound and index of the merged partition. */
	*merged_datums = lappend(*merged_datums, merged_ub->datums);
	*merged_kinds = lappend(*merged_kinds, merged_ub->kind);
	*merged_indexes = lappend_int(*merged_indexes, merged_index);
}

/*
 * partitions_are_ordered
 *		Determine whether the partitions described by 'boundinfo' are ordered,
 *		that is partitions appearing earlier in the PartitionDesc sequence
 *		contain partition keys strictly less than those appearing later.
 *		Also, if NULL values are possible, they must come in the last
 *		partition defined in the PartitionDesc.
 *
 * If out of order, or there is insufficient info to know the order,
 * then we return false.
 */
bool
partitions_are_ordered(PartitionBoundInfo boundinfo, int nparts)
{
	Assert(boundinfo != NULL);

	switch (boundinfo->strategy)
	{
		case PARTITION_STRATEGY_RANGE:

			/*
			 * RANGE-type partitioning guarantees that the partitions can be
			 * scanned in the order that they're defined in the PartitionDesc
			 * to provide sequential, non-overlapping ranges of tuples.
			 * However, if a DEFAULT partition exists then it doesn't work, as
			 * that could contain tuples from either below or above the
			 * defined range, or tuples belonging to gaps between partitions.
			 */
			if (!partition_bound_has_default(boundinfo))
				return true;
			break;

		case PARTITION_STRATEGY_LIST:

			/*
			 * LIST partitioning can also guarantee ordering, but only if the
			 * partitions don't accept interleaved values.  We could likely
			 * check for this by looping over the PartitionBound's indexes
			 * array to check that the indexes are in order.  For now, let's
			 * just keep it simple and just accept LIST partitioning when
			 * there's no DEFAULT partition, exactly one value per partition,
			 * and optionally a NULL partition that does not accept any other
			 * values.  Such a NULL partition will come last in the
			 * PartitionDesc, and the other partitions will be properly
			 * ordered.  This is a cheap test to make as it does not require
			 * any per-partition processing.  Maybe we'd like to handle more
			 * complex cases in the future.
			 */
			if (partition_bound_has_default(boundinfo))
				return false;

			if (boundinfo->ndatums + partition_bound_accepts_nulls(boundinfo)
				== nparts)
				return true;
			break;

		default:
			/* HASH, or some other strategy */
			break;
	}

	return false;
}

/*
 * check_new_partition_bound
 *
 * Checks if the new partition's bound overlaps any of the existing partitions
 * of parent.  Also performs additional checks as necessary per strategy.
 */
void
check_new_partition_bound(char *relname, Relation parent,
						  PartitionBoundSpec *spec, ParseState *pstate)
{
	PartitionKey key = RelationGetPartitionKey(parent);
	PartitionDesc partdesc = RelationGetPartitionDesc(parent);
	PartitionBoundInfo boundinfo = partdesc->boundinfo;
	int			with = -1;
	bool		overlap = false;
	int			overlap_location = -1;

	if (spec->is_default)
	{
		/*
		 * The default partition bound never conflicts with any other
		 * partition's; if that's what we're attaching, the only possible
		 * problem is that one already exists, so check for that and we're
		 * done.
		 */
		if (boundinfo == NULL || !partition_bound_has_default(boundinfo))
			return;

		/* Default partition already exists, error out. */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("partition \"%s\" conflicts with existing default partition \"%s\"",
						relname, get_rel_name(partdesc->oids[boundinfo->default_index])),
				 parser_errposition(pstate, spec->location)));
	}

	switch (key->strategy)
	{
		case PARTITION_STRATEGY_HASH:
			{
				Assert(spec->strategy == PARTITION_STRATEGY_HASH);
				Assert(spec->remainder >= 0 && spec->remainder < spec->modulus);

				if (partdesc->nparts > 0)
				{
					Datum	  **datums = boundinfo->datums;
					int			ndatums = boundinfo->ndatums;
					int			greatest_modulus;
					int			remainder;
					int			offset;
					bool		valid_modulus = true;
					int			prev_modulus,	/* Previous largest modulus */
								next_modulus;	/* Next largest modulus */

					/*
					 * Check rule that every modulus must be a factor of the
					 * next larger modulus.  For example, if you have a bunch
					 * of partitions that all have modulus 5, you can add a
					 * new partition with modulus 10 or a new partition with
					 * modulus 15, but you cannot add both a partition with
					 * modulus 10 and a partition with modulus 15, because 10
					 * is not a factor of 15.
					 *
					 * Get the greatest (modulus, remainder) pair contained in
					 * boundinfo->datums that is less than or equal to the
					 * (spec->modulus, spec->remainder) pair.
					 */
					offset = partition_hash_bsearch(boundinfo,
													spec->modulus,
													spec->remainder);
					if (offset < 0)
					{
						next_modulus = DatumGetInt32(datums[0][0]);
						valid_modulus = (next_modulus % spec->modulus) == 0;
					}
					else
					{
						prev_modulus = DatumGetInt32(datums[offset][0]);
						valid_modulus = (spec->modulus % prev_modulus) == 0;

						if (valid_modulus && (offset + 1) < ndatums)
						{
							next_modulus = DatumGetInt32(datums[offset + 1][0]);
							valid_modulus = (next_modulus % spec->modulus) == 0;
						}
					}

					if (!valid_modulus)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
								 errmsg("every hash partition modulus must be a factor of the next larger modulus")));

					greatest_modulus = get_hash_partition_greatest_modulus(boundinfo);
					remainder = spec->remainder;

					/*
					 * Normally, the lowest remainder that could conflict with
					 * the new partition is equal to the remainder specified
					 * for the new partition, but when the new partition has a
					 * modulus higher than any used so far, we need to adjust.
					 */
					if (remainder >= greatest_modulus)
						remainder = remainder % greatest_modulus;

					/* Check every potentially-conflicting remainder. */
					do
					{
						if (boundinfo->indexes[remainder] != -1)
						{
							overlap = true;
							overlap_location = spec->location;
							with = boundinfo->indexes[remainder];
							break;
						}
						remainder += spec->modulus;
					} while (remainder < greatest_modulus);
				}

				break;
			}

		case PARTITION_STRATEGY_LIST:
			{
				Assert(spec->strategy == PARTITION_STRATEGY_LIST);

				if (partdesc->nparts > 0)
				{
					ListCell   *cell;

					Assert(boundinfo &&
						   boundinfo->strategy == PARTITION_STRATEGY_LIST &&
						   (boundinfo->ndatums > 0 ||
							partition_bound_accepts_nulls(boundinfo) ||
							partition_bound_has_default(boundinfo)));

					foreach(cell, spec->listdatums)
					{
						Const	   *val = castNode(Const, lfirst(cell));

						overlap_location = val->location;
						if (!val->constisnull)
						{
							int			offset;
							bool		equal;

							offset = partition_list_bsearch(&key->partsupfunc[0],
															key->partcollation,
															boundinfo,
															val->constvalue,
															&equal);
							if (offset >= 0 && equal)
							{
								overlap = true;
								with = boundinfo->indexes[offset];
								break;
							}
						}
						else if (partition_bound_accepts_nulls(boundinfo))
						{
							overlap = true;
							with = boundinfo->null_index;
							break;
						}
					}
				}

				break;
			}

		case PARTITION_STRATEGY_RANGE:
			{
				PartitionRangeBound *lower,
						   *upper;
				int			cmpval;

				Assert(spec->strategy == PARTITION_STRATEGY_RANGE);
				lower = make_one_partition_rbound(key, -1, spec->lowerdatums, true);
				upper = make_one_partition_rbound(key, -1, spec->upperdatums, false);

				/*
				 * First check if the resulting range would be empty with
				 * specified lower and upper bounds.  partition_rbound_cmp
				 * cannot return zero here, since the lower-bound flags are
				 * different.
				 */
				cmpval = partition_rbound_cmp(key->partnatts,
											  key->partsupfunc,
											  key->partcollation,
											  lower->datums, lower->kind,
											  true, upper);
				Assert(cmpval != 0);
				if (cmpval > 0)
				{
					/* Point to problematic key in the lower datums list. */
					PartitionRangeDatum *datum = list_nth(spec->lowerdatums,
														  cmpval - 1);

					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("empty range bound specified for partition \"%s\"",
									relname),
							 errdetail("Specified lower bound %s is greater than or equal to upper bound %s.",
									   get_range_partbound_string(spec->lowerdatums),
									   get_range_partbound_string(spec->upperdatums)),
							 parser_errposition(pstate, datum->location)));
				}

				if (partdesc->nparts > 0)
				{
					int			offset;

					Assert(boundinfo &&
						   boundinfo->strategy == PARTITION_STRATEGY_RANGE &&
						   (boundinfo->ndatums > 0 ||
							partition_bound_has_default(boundinfo)));

					/*
					 * Test whether the new lower bound (which is treated
					 * inclusively as part of the new partition) lies inside
					 * an existing partition, or in a gap.
					 *
					 * If it's inside an existing partition, the bound at
					 * offset + 1 will be the upper bound of that partition,
					 * and its index will be >= 0.
					 *
					 * If it's in a gap, the bound at offset + 1 will be the
					 * lower bound of the next partition, and its index will
					 * be -1. This is also true if there is no next partition,
					 * since the index array is initialised with an extra -1
					 * at the end.
					 */
					offset = partition_range_bsearch(key->partnatts,
													 key->partsupfunc,
													 key->partcollation,
													 boundinfo, lower,
													 &cmpval);

					if (boundinfo->indexes[offset + 1] < 0)
					{
						/*
						 * Check that the new partition will fit in the gap.
						 * For it to fit, the new upper bound must be less
						 * than or equal to the lower bound of the next
						 * partition, if there is one.
						 */
						if (offset + 1 < boundinfo->ndatums)
						{
							Datum	   *datums;
							PartitionRangeDatumKind *kind;
							bool		is_lower;

							datums = boundinfo->datums[offset + 1];
							kind = boundinfo->kind[offset + 1];
							is_lower = (boundinfo->indexes[offset + 1] == -1);

							cmpval = partition_rbound_cmp(key->partnatts,
														  key->partsupfunc,
														  key->partcollation,
														  datums, kind,
														  is_lower, upper);
							if (cmpval < 0)
							{
								/*
								 * Point to problematic key in the upper
								 * datums list.
								 */
								PartitionRangeDatum *datum =
								list_nth(spec->upperdatums, Abs(cmpval) - 1);

								/*
								 * The new partition overlaps with the
								 * existing partition between offset + 1 and
								 * offset + 2.
								 */
								overlap = true;
								overlap_location = datum->location;
								with = boundinfo->indexes[offset + 2];
							}
						}
					}
					else
					{
						/*
						 * The new partition overlaps with the existing
						 * partition between offset and offset + 1.
						 */
						PartitionRangeDatum *datum;

						/*
						 * Point to problematic key in the lower datums list;
						 * if we have equality, point to the first one.
						 */
						datum = cmpval == 0 ? linitial(spec->lowerdatums) :
							list_nth(spec->lowerdatums, Abs(cmpval) - 1);
						overlap = true;
						overlap_location = datum->location;
						with = boundinfo->indexes[offset + 1];
					}
				}

				break;
			}

		default:
			elog(ERROR, "unexpected partition strategy: %d",
				 (int) key->strategy);
	}

	if (overlap)
	{
		Assert(with >= 0);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("partition \"%s\" would overlap partition \"%s\"",
						relname, get_rel_name(partdesc->oids[with])),
				 parser_errposition(pstate, overlap_location)));
	}
}

/*
 * check_default_partition_contents
 *
 * This function checks if there exists a row in the default partition that
 * would properly belong to the new partition being added.  If it finds one,
 * it throws an error.
 */
void
check_default_partition_contents(Relation parent, Relation default_rel,
								 PartitionBoundSpec *new_spec)
{
	List	   *new_part_constraints;
	List	   *def_part_constraints;
	List	   *all_parts;
	ListCell   *lc;

	new_part_constraints = (new_spec->strategy == PARTITION_STRATEGY_LIST)
		? get_qual_for_list(parent, new_spec)
		: get_qual_for_range(parent, new_spec, false);
	def_part_constraints =
		get_proposed_default_constraint(new_part_constraints);

	/*
	 * Map the Vars in the constraint expression from parent's attnos to
	 * default_rel's.
	 */
	def_part_constraints =
		map_partition_varattnos(def_part_constraints, 1, default_rel,
								parent);

	/*
	 * If the existing constraints on the default partition imply that it will
	 * not contain any row that would belong to the new partition, we can
	 * avoid scanning the default partition.
	 */
	if (PartConstraintImpliedByRelConstraint(default_rel, def_part_constraints))
	{
		ereport(DEBUG1,
				(errmsg("updated partition constraint for default partition \"%s\" is implied by existing constraints",
						RelationGetRelationName(default_rel))));
		return;
	}

	/*
	 * Scan the default partition and its subpartitions, and check for rows
	 * that do not satisfy the revised partition constraints.
	 */
	if (default_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		all_parts = find_all_inheritors(RelationGetRelid(default_rel),
										AccessExclusiveLock, NULL);
	else
		all_parts = list_make1_oid(RelationGetRelid(default_rel));

	foreach(lc, all_parts)
	{
		Oid			part_relid = lfirst_oid(lc);
		Relation	part_rel;
		Expr	   *partition_constraint;
		EState	   *estate;
		ExprState  *partqualstate = NULL;
		Snapshot	snapshot;
		ExprContext *econtext;
		TableScanDesc scan;
		MemoryContext oldCxt;
		TupleTableSlot *tupslot;

		/* Lock already taken above. */
		if (part_relid != RelationGetRelid(default_rel))
		{
			part_rel = table_open(part_relid, NoLock);

			/*
			 * Map the Vars in the constraint expression from default_rel's
			 * the sub-partition's.
			 */
			partition_constraint = make_ands_explicit(def_part_constraints);
			partition_constraint = (Expr *)
				map_partition_varattnos((List *) partition_constraint, 1,
										part_rel, default_rel);

			/*
			 * If the partition constraints on default partition child imply
			 * that it will not contain any row that would belong to the new
			 * partition, we can avoid scanning the child table.
			 */
			if (PartConstraintImpliedByRelConstraint(part_rel,
													 def_part_constraints))
			{
				ereport(DEBUG1,
						(errmsg("updated partition constraint for default partition \"%s\" is implied by existing constraints",
								RelationGetRelationName(part_rel))));

				table_close(part_rel, NoLock);
				continue;
			}
		}
		else
		{
			part_rel = default_rel;
			partition_constraint = make_ands_explicit(def_part_constraints);
		}

		/*
		 * Only RELKIND_RELATION relations (i.e. leaf partitions) need to be
		 * scanned.
		 */
		if (part_rel->rd_rel->relkind != RELKIND_RELATION)
		{
			if (part_rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
				ereport(WARNING,
						(errcode(ERRCODE_CHECK_VIOLATION),
						 errmsg("skipped scanning foreign table \"%s\" which is a partition of default partition \"%s\"",
								RelationGetRelationName(part_rel),
								RelationGetRelationName(default_rel))));

			if (RelationGetRelid(default_rel) != RelationGetRelid(part_rel))
				table_close(part_rel, NoLock);

			continue;
		}

		estate = CreateExecutorState();

		/* Build expression execution states for partition check quals */
		partqualstate = ExecPrepareExpr(partition_constraint, estate);

		econtext = GetPerTupleExprContext(estate);
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		tupslot = table_slot_create(part_rel, &estate->es_tupleTable);
		scan = table_beginscan(part_rel, snapshot, 0, NULL);

		/*
		 * Switch to per-tuple memory context and reset it for each tuple
		 * produced, so we don't leak memory.
		 */
		oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		while (table_scan_getnextslot(scan, ForwardScanDirection, tupslot))
		{
			econtext->ecxt_scantuple = tupslot;

			if (!ExecCheck(partqualstate, econtext))
				ereport(ERROR,
						(errcode(ERRCODE_CHECK_VIOLATION),
						 errmsg("updated partition constraint for default partition \"%s\" would be violated by some row",
								RelationGetRelationName(default_rel)),
						 errtable(default_rel)));

			ResetExprContext(econtext);
			CHECK_FOR_INTERRUPTS();
		}

		MemoryContextSwitchTo(oldCxt);
		table_endscan(scan);
		UnregisterSnapshot(snapshot);
		ExecDropSingleTupleTableSlot(tupslot);
		FreeExecutorState(estate);

		if (RelationGetRelid(default_rel) != RelationGetRelid(part_rel))
			table_close(part_rel, NoLock);	/* keep the lock until commit */
	}
}

/*
 * get_hash_partition_greatest_modulus
 *
 * Returns the greatest modulus of the hash partition bound. The greatest
 * modulus will be at the end of the datums array because hash partitions are
 * arranged in the ascending order of their moduli and remainders.
 */
int
get_hash_partition_greatest_modulus(PartitionBoundInfo bound)
{
	Assert(bound && bound->strategy == PARTITION_STRATEGY_HASH);
	Assert(bound->datums && bound->ndatums > 0);
	Assert(DatumGetInt32(bound->datums[bound->ndatums - 1][0]) > 0);

	return DatumGetInt32(bound->datums[bound->ndatums - 1][0]);
}

/*
 * make_one_partition_rbound
 *
 * Return a PartitionRangeBound given a list of PartitionRangeDatum elements
 * and a flag telling whether the bound is lower or not.  Made into a function
 * because there are multiple sites that want to use this facility.
 */
static PartitionRangeBound *
make_one_partition_rbound(PartitionKey key, int index, List *datums, bool lower)
{
	PartitionRangeBound *bound;
	ListCell   *lc;
	int			i;

	Assert(datums != NIL);

	bound = (PartitionRangeBound *) palloc0(sizeof(PartitionRangeBound));
	bound->index = index;
	bound->datums = (Datum *) palloc0(key->partnatts * sizeof(Datum));
	bound->kind = (PartitionRangeDatumKind *) palloc0(key->partnatts *
													  sizeof(PartitionRangeDatumKind));
	bound->lower = lower;

	i = 0;
	foreach(lc, datums)
	{
		PartitionRangeDatum *datum = castNode(PartitionRangeDatum, lfirst(lc));

		/* What's contained in this range datum? */
		bound->kind[i] = datum->kind;

		if (datum->kind == PARTITION_RANGE_DATUM_VALUE)
		{
			Const	   *val = castNode(Const, datum->value);

			if (val->constisnull)
				elog(ERROR, "invalid range bound datum");
			bound->datums[i] = val->constvalue;
		}

		i++;
	}

	return bound;
}

/*
 * partition_rbound_cmp
 *
 * For two range bounds this decides whether the 1st one (specified by
 * datums1, kind1, and lower1) is <, =, or > the bound specified in *b2.
 *
 * 0 is returned if they are equal, otherwise a non-zero integer whose sign
 * indicates the ordering, and whose absolute value gives the 1-based
 * partition key number of the first mismatching column.
 *
 * partnatts, partsupfunc and partcollation give the number of attributes in the
 * bounds to be compared, comparison function to be used and the collations of
 * attributes, respectively.
 *
 * Note that if the values of the two range bounds compare equal, then we take
 * into account whether they are upper or lower bounds, and an upper bound is
 * considered to be smaller than a lower bound. This is important to the way
 * that RelationBuildPartitionDesc() builds the PartitionBoundInfoData
 * structure, which only stores the upper bound of a common boundary between
 * two contiguous partitions.
 */
static int32
partition_rbound_cmp(int partnatts, FmgrInfo *partsupfunc,
					 Oid *partcollation,
					 Datum *datums1, PartitionRangeDatumKind *kind1,
					 bool lower1, PartitionRangeBound *b2)
{
	int32		colnum = 0;
	int32		cmpval = 0;		/* placate compiler */
	int			i;
	Datum	   *datums2 = b2->datums;
	PartitionRangeDatumKind *kind2 = b2->kind;
	bool		lower2 = b2->lower;

	for (i = 0; i < partnatts; i++)
	{
		/* Track column number in case we need it for result */
		colnum++;

		/*
		 * First, handle cases where the column is unbounded, which should not
		 * invoke the comparison procedure, and should not consider any later
		 * columns. Note that the PartitionRangeDatumKind enum elements
		 * compare the same way as the values they represent.
		 */
		if (kind1[i] < kind2[i])
			return -colnum;
		else if (kind1[i] > kind2[i])
			return colnum;
		else if (kind1[i] != PARTITION_RANGE_DATUM_VALUE)
		{
			/*
			 * The column bounds are both MINVALUE or both MAXVALUE. No later
			 * columns should be considered, but we still need to compare
			 * whether they are upper or lower bounds.
			 */
			break;
		}

		cmpval = DatumGetInt32(FunctionCall2Coll(&partsupfunc[i],
												 partcollation[i],
												 datums1[i],
												 datums2[i]));
		if (cmpval != 0)
			break;
	}

	/*
	 * If the comparison is anything other than equal, we're done. If they
	 * compare equal though, we still have to consider whether the boundaries
	 * are inclusive or exclusive.  Exclusive one is considered smaller of the
	 * two.
	 */
	if (cmpval == 0 && lower1 != lower2)
		cmpval = lower1 ? 1 : -1;

	return cmpval == 0 ? 0 : (cmpval < 0 ? -colnum : colnum);
}

/*
 * partition_rbound_datum_cmp
 *
 * Return whether range bound (specified in rb_datums and rb_kind)
 * is <, =, or > partition key of tuple (tuple_datums)
 *
 * n_tuple_datums, partsupfunc and partcollation give number of attributes in
 * the bounds to be compared, comparison function to be used and the collations
 * of attributes resp.
 */
int32
partition_rbound_datum_cmp(FmgrInfo *partsupfunc, Oid *partcollation,
						   Datum *rb_datums, PartitionRangeDatumKind *rb_kind,
						   Datum *tuple_datums, int n_tuple_datums)
{
	int			i;
	int32		cmpval = -1;

	for (i = 0; i < n_tuple_datums; i++)
	{
		if (rb_kind[i] == PARTITION_RANGE_DATUM_MINVALUE)
			return -1;
		else if (rb_kind[i] == PARTITION_RANGE_DATUM_MAXVALUE)
			return 1;

		cmpval = DatumGetInt32(FunctionCall2Coll(&partsupfunc[i],
												 partcollation[i],
												 rb_datums[i],
												 tuple_datums[i]));
		if (cmpval != 0)
			break;
	}

	return cmpval;
}

/*
 * partition_hbound_cmp
 *
 * Compares modulus first, then remainder if modulus is equal.
 */
static int32
partition_hbound_cmp(int modulus1, int remainder1, int modulus2, int remainder2)
{
	if (modulus1 < modulus2)
		return -1;
	if (modulus1 > modulus2)
		return 1;
	if (modulus1 == modulus2 && remainder1 != remainder2)
		return (remainder1 > remainder2) ? 1 : -1;
	return 0;
}

/*
 * partition_list_bsearch
 *		Returns the index of the greatest bound datum that is less than equal
 * 		to the given value or -1 if all of the bound datums are greater
 *
 * *is_equal is set to true if the bound datum at the returned index is equal
 * to the input value.
 */
int
partition_list_bsearch(FmgrInfo *partsupfunc, Oid *partcollation,
					   PartitionBoundInfo boundinfo,
					   Datum value, bool *is_equal)
{
	int			lo,
				hi,
				mid;

	lo = -1;
	hi = boundinfo->ndatums - 1;
	while (lo < hi)
	{
		int32		cmpval;

		mid = (lo + hi + 1) / 2;
		cmpval = DatumGetInt32(FunctionCall2Coll(&partsupfunc[0],
												 partcollation[0],
												 boundinfo->datums[mid][0],
												 value));
		if (cmpval <= 0)
		{
			lo = mid;
			*is_equal = (cmpval == 0);
			if (*is_equal)
				break;
		}
		else
			hi = mid - 1;
	}

	return lo;
}

/*
 * partition_range_bsearch
 *		Returns the index of the greatest range bound that is less than or
 *		equal to the given range bound or -1 if all of the range bounds are
 *		greater
 *
 * Upon return from this function, *cmpval is set to 0 if the bound at the
 * returned index matches the input range bound exactly, otherwise a
 * non-zero integer whose sign indicates the ordering, and whose absolute
 * value gives the 1-based partition key number of the first mismatching
 * column.
 */
static int
partition_range_bsearch(int partnatts, FmgrInfo *partsupfunc,
						Oid *partcollation,
						PartitionBoundInfo boundinfo,
						PartitionRangeBound *probe, int32 *cmpval)
{
	int			lo,
				hi,
				mid;

	lo = -1;
	hi = boundinfo->ndatums - 1;
	while (lo < hi)
	{
		mid = (lo + hi + 1) / 2;
		*cmpval = partition_rbound_cmp(partnatts, partsupfunc,
									   partcollation,
									   boundinfo->datums[mid],
									   boundinfo->kind[mid],
									   (boundinfo->indexes[mid] == -1),
									   probe);
		if (*cmpval <= 0)
		{
			lo = mid;
			if (*cmpval == 0)
				break;
		}
		else
			hi = mid - 1;
	}

	return lo;
}

/*
 * partition_range_datum_bsearch
 *		Returns the index of the greatest range bound that is less than or
 *		equal to the given tuple or -1 if all of the range bounds are greater
 *
 * *is_equal is set to true if the range bound at the returned index is equal
 * to the input tuple.
 */
int
partition_range_datum_bsearch(FmgrInfo *partsupfunc, Oid *partcollation,
							  PartitionBoundInfo boundinfo,
							  int nvalues, Datum *values, bool *is_equal)
{
	int			lo,
				hi,
				mid;

	lo = -1;
	hi = boundinfo->ndatums - 1;
	while (lo < hi)
	{
		int32		cmpval;

		mid = (lo + hi + 1) / 2;
		cmpval = partition_rbound_datum_cmp(partsupfunc,
											partcollation,
											boundinfo->datums[mid],
											boundinfo->kind[mid],
											values,
											nvalues);
		if (cmpval <= 0)
		{
			lo = mid;
			*is_equal = (cmpval == 0);

			if (*is_equal)
				break;
		}
		else
			hi = mid - 1;
	}

	return lo;
}

/*
 * partition_hash_bsearch
 *		Returns the index of the greatest (modulus, remainder) pair that is
 *		less than or equal to the given (modulus, remainder) pair or -1 if
 *		all of them are greater
 */
int
partition_hash_bsearch(PartitionBoundInfo boundinfo,
					   int modulus, int remainder)
{
	int			lo,
				hi,
				mid;

	lo = -1;
	hi = boundinfo->ndatums - 1;
	while (lo < hi)
	{
		int32		cmpval,
					bound_modulus,
					bound_remainder;

		mid = (lo + hi + 1) / 2;
		bound_modulus = DatumGetInt32(boundinfo->datums[mid][0]);
		bound_remainder = DatumGetInt32(boundinfo->datums[mid][1]);
		cmpval = partition_hbound_cmp(bound_modulus, bound_remainder,
									  modulus, remainder);
		if (cmpval <= 0)
		{
			lo = mid;

			if (cmpval == 0)
				break;
		}
		else
			hi = mid - 1;
	}

	return lo;
}

/*
 * qsort_partition_hbound_cmp
 *
 * Hash bounds are sorted by modulus, then by remainder.
 */
static int32
qsort_partition_hbound_cmp(const void *a, const void *b)
{
	PartitionHashBound *h1 = (*(PartitionHashBound *const *) a);
	PartitionHashBound *h2 = (*(PartitionHashBound *const *) b);

	return partition_hbound_cmp(h1->modulus, h1->remainder,
								h2->modulus, h2->remainder);
}

/*
 * qsort_partition_list_value_cmp
 *
 * Compare two list partition bound datums.
 */
static int32
qsort_partition_list_value_cmp(const void *a, const void *b, void *arg)
{
	Datum		val1 = (*(PartitionListValue *const *) a)->value,
				val2 = (*(PartitionListValue *const *) b)->value;
	PartitionKey key = (PartitionKey) arg;

	return DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[0],
										   key->partcollation[0],
										   val1, val2));
}

/*
 * qsort_partition_rbound_cmp
 *
 * Used when sorting range bounds across all range partitions.
 */
static int32
qsort_partition_rbound_cmp(const void *a, const void *b, void *arg)
{
	PartitionRangeBound *b1 = (*(PartitionRangeBound *const *) a);
	PartitionRangeBound *b2 = (*(PartitionRangeBound *const *) b);
	PartitionKey key = (PartitionKey) arg;

	return compare_range_bounds(key->partnatts, key->partsupfunc,
								key->partcollation,
								b1, b2);
}

/*
 * get_partition_bound_num_indexes
 *
 * Returns the number of the entries in the partition bound indexes array.
 */
static int
get_partition_bound_num_indexes(PartitionBoundInfo bound)
{
	int			num_indexes;

	Assert(bound);

	switch (bound->strategy)
	{
		case PARTITION_STRATEGY_HASH:

			/*
			 * The number of the entries in the indexes array is same as the
			 * greatest modulus.
			 */
			num_indexes = get_hash_partition_greatest_modulus(bound);
			break;

		case PARTITION_STRATEGY_LIST:
			num_indexes = bound->ndatums;
			break;

		case PARTITION_STRATEGY_RANGE:
			/* Range partitioned table has an extra index. */
			num_indexes = bound->ndatums + 1;
			break;

		default:
			elog(ERROR, "unexpected partition strategy: %d",
				 (int) bound->strategy);
	}

	return num_indexes;
}

/*
 * get_partition_operator
 *
 * Return oid of the operator of the given strategy for the given partition
 * key column.  It is assumed that the partitioning key is of the same type as
 * the chosen partitioning opclass, or at least binary-compatible.  In the
 * latter case, *need_relabel is set to true if the opclass is not of a
 * polymorphic type (indicating a RelabelType node needed on top), otherwise
 * false.
 */
static Oid
get_partition_operator(PartitionKey key, int col, StrategyNumber strategy,
					   bool *need_relabel)
{
	Oid			operoid;

	/*
	 * Get the operator in the partitioning opfamily using the opclass'
	 * declared input type as both left- and righttype.
	 */
	operoid = get_opfamily_member(key->partopfamily[col],
								  key->partopcintype[col],
								  key->partopcintype[col],
								  strategy);
	if (!OidIsValid(operoid))
		elog(ERROR, "missing operator %d(%u,%u) in partition opfamily %u",
			 strategy, key->partopcintype[col], key->partopcintype[col],
			 key->partopfamily[col]);

	/*
	 * If the partition key column is not of the same type as the operator
	 * class and not polymorphic, tell caller to wrap the non-Const expression
	 * in a RelabelType.  This matches what parse_coerce.c does.
	 */
	*need_relabel = (key->parttypid[col] != key->partopcintype[col] &&
					 key->partopcintype[col] != RECORDOID &&
					 !IsPolymorphicType(key->partopcintype[col]));

	return operoid;
}

/*
 * make_partition_op_expr
 *		Returns an Expr for the given partition key column with arg1 and
 *		arg2 as its leftop and rightop, respectively
 */
static Expr *
make_partition_op_expr(PartitionKey key, int keynum,
					   uint16 strategy, Expr *arg1, Expr *arg2)
{
	Oid			operoid;
	bool		need_relabel = false;
	Expr	   *result = NULL;

	/* Get the correct btree operator for this partitioning column */
	operoid = get_partition_operator(key, keynum, strategy, &need_relabel);

	/*
	 * Chosen operator may be such that the non-Const operand needs to be
	 * coerced, so apply the same; see the comment in
	 * get_partition_operator().
	 */
	if (!IsA(arg1, Const) &&
		(need_relabel ||
		 key->partcollation[keynum] != key->parttypcoll[keynum]))
		arg1 = (Expr *) makeRelabelType(arg1,
										key->partopcintype[keynum],
										-1,
										key->partcollation[keynum],
										COERCE_EXPLICIT_CAST);

	/* Generate the actual expression */
	switch (key->strategy)
	{
		case PARTITION_STRATEGY_LIST:
			{
				List	   *elems = (List *) arg2;
				int			nelems = list_length(elems);

				Assert(nelems >= 1);
				Assert(keynum == 0);

				if (nelems > 1 &&
					!type_is_array(key->parttypid[keynum]))
				{
					ArrayExpr  *arrexpr;
					ScalarArrayOpExpr *saopexpr;

					/* Construct an ArrayExpr for the right-hand inputs */
					arrexpr = makeNode(ArrayExpr);
					arrexpr->array_typeid =
						get_array_type(key->parttypid[keynum]);
					arrexpr->array_collid = key->parttypcoll[keynum];
					arrexpr->element_typeid = key->parttypid[keynum];
					arrexpr->elements = elems;
					arrexpr->multidims = false;
					arrexpr->location = -1;

					/* Build leftop = ANY (rightop) */
					saopexpr = makeNode(ScalarArrayOpExpr);
					saopexpr->opno = operoid;
					saopexpr->opfuncid = get_opcode(operoid);
					saopexpr->useOr = true;
					saopexpr->inputcollid = key->partcollation[keynum];
					saopexpr->args = list_make2(arg1, arrexpr);
					saopexpr->location = -1;

					result = (Expr *) saopexpr;
				}
				else
				{
					List	   *elemops = NIL;
					ListCell   *lc;

					foreach(lc, elems)
					{
						Expr	   *elem = lfirst(lc),
								   *elemop;

						elemop = make_opclause(operoid,
											   BOOLOID,
											   false,
											   arg1, elem,
											   InvalidOid,
											   key->partcollation[keynum]);
						elemops = lappend(elemops, elemop);
					}

					result = nelems > 1 ? makeBoolExpr(OR_EXPR, elemops, -1) : linitial(elemops);
				}
				break;
			}

		case PARTITION_STRATEGY_RANGE:
			result = make_opclause(operoid,
								   BOOLOID,
								   false,
								   arg1, arg2,
								   InvalidOid,
								   key->partcollation[keynum]);
			break;

		default:
			elog(ERROR, "invalid partitioning strategy");
			break;
	}

	return result;
}

/*
 * get_qual_for_hash
 *
 * Returns a CHECK constraint expression to use as a hash partition's
 * constraint, given the parent relation and partition bound structure.
 *
 * The partition constraint for a hash partition is always a call to the
 * built-in function satisfies_hash_partition().
 */
static List *
get_qual_for_hash(Relation parent, PartitionBoundSpec *spec)
{
	PartitionKey key = RelationGetPartitionKey(parent);
	FuncExpr   *fexpr;
	Node	   *relidConst;
	Node	   *modulusConst;
	Node	   *remainderConst;
	List	   *args;
	ListCell   *partexprs_item;
	int			i;

	/* Fixed arguments. */
	relidConst = (Node *) makeConst(OIDOID,
									-1,
									InvalidOid,
									sizeof(Oid),
									ObjectIdGetDatum(RelationGetRelid(parent)),
									false,
									true);

	modulusConst = (Node *) makeConst(INT4OID,
									  -1,
									  InvalidOid,
									  sizeof(int32),
									  Int32GetDatum(spec->modulus),
									  false,
									  true);

	remainderConst = (Node *) makeConst(INT4OID,
										-1,
										InvalidOid,
										sizeof(int32),
										Int32GetDatum(spec->remainder),
										false,
										true);

	args = list_make3(relidConst, modulusConst, remainderConst);
	partexprs_item = list_head(key->partexprs);

	/* Add an argument for each key column. */
	for (i = 0; i < key->partnatts; i++)
	{
		Node	   *keyCol;

		/* Left operand */
		if (key->partattrs[i] != 0)
		{
			keyCol = (Node *) makeVar(1,
									  key->partattrs[i],
									  key->parttypid[i],
									  key->parttypmod[i],
									  key->parttypcoll[i],
									  0);
		}
		else
		{
			keyCol = (Node *) copyObject(lfirst(partexprs_item));
			partexprs_item = lnext(key->partexprs, partexprs_item);
		}

		args = lappend(args, keyCol);
	}

	fexpr = makeFuncExpr(F_SATISFIES_HASH_PARTITION,
						 BOOLOID,
						 args,
						 InvalidOid,
						 InvalidOid,
						 COERCE_EXPLICIT_CALL);

	return list_make1(fexpr);
}

/*
 * get_qual_for_list
 *
 * Returns an implicit-AND list of expressions to use as a list partition's
 * constraint, given the parent relation and partition bound structure.
 *
 * The function returns NIL for a default partition when it's the only
 * partition since in that case there is no constraint.
 */
static List *
get_qual_for_list(Relation parent, PartitionBoundSpec *spec)
{
	PartitionKey key = RelationGetPartitionKey(parent);
	List	   *result;
	Expr	   *keyCol;
	Expr	   *opexpr;
	NullTest   *nulltest;
	ListCell   *cell;
	List	   *elems = NIL;
	bool		list_has_null = false;

	/*
	 * Only single-column list partitioning is supported, so we are worried
	 * only about the partition key with index 0.
	 */
	Assert(key->partnatts == 1);

	/* Construct Var or expression representing the partition column */
	if (key->partattrs[0] != 0)
		keyCol = (Expr *) makeVar(1,
								  key->partattrs[0],
								  key->parttypid[0],
								  key->parttypmod[0],
								  key->parttypcoll[0],
								  0);
	else
		keyCol = (Expr *) copyObject(linitial(key->partexprs));

	/*
	 * For default list partition, collect datums for all the partitions. The
	 * default partition constraint should check that the partition key is
	 * equal to none of those.
	 */
	if (spec->is_default)
	{
		int			i;
		int			ndatums = 0;
		PartitionDesc pdesc = RelationGetPartitionDesc(parent);
		PartitionBoundInfo boundinfo = pdesc->boundinfo;

		if (boundinfo)
		{
			ndatums = boundinfo->ndatums;

			if (partition_bound_accepts_nulls(boundinfo))
				list_has_null = true;
		}

		/*
		 * If default is the only partition, there need not be any partition
		 * constraint on it.
		 */
		if (ndatums == 0 && !list_has_null)
			return NIL;

		for (i = 0; i < ndatums; i++)
		{
			Const	   *val;

			/*
			 * Construct Const from known-not-null datum.  We must be careful
			 * to copy the value, because our result has to be able to outlive
			 * the relcache entry we're copying from.
			 */
			val = makeConst(key->parttypid[0],
							key->parttypmod[0],
							key->parttypcoll[0],
							key->parttyplen[0],
							datumCopy(*boundinfo->datums[i],
									  key->parttypbyval[0],
									  key->parttyplen[0]),
							false,	/* isnull */
							key->parttypbyval[0]);

			elems = lappend(elems, val);
		}
	}
	else
	{
		/*
		 * Create list of Consts for the allowed values, excluding any nulls.
		 */
		foreach(cell, spec->listdatums)
		{
			Const	   *val = castNode(Const, lfirst(cell));

			if (val->constisnull)
				list_has_null = true;
			else
				elems = lappend(elems, copyObject(val));
		}
	}

	if (elems)
	{
		/*
		 * Generate the operator expression from the non-null partition
		 * values.
		 */
		opexpr = make_partition_op_expr(key, 0, BTEqualStrategyNumber,
										keyCol, (Expr *) elems);
	}
	else
	{
		/*
		 * If there are no partition values, we don't need an operator
		 * expression.
		 */
		opexpr = NULL;
	}

	if (!list_has_null)
	{
		/*
		 * Gin up a "col IS NOT NULL" test that will be AND'd with the main
		 * expression.  This might seem redundant, but the partition routing
		 * machinery needs it.
		 */
		nulltest = makeNode(NullTest);
		nulltest->arg = keyCol;
		nulltest->nulltesttype = IS_NOT_NULL;
		nulltest->argisrow = false;
		nulltest->location = -1;

		result = opexpr ? list_make2(nulltest, opexpr) : list_make1(nulltest);
	}
	else
	{
		/*
		 * Gin up a "col IS NULL" test that will be OR'd with the main
		 * expression.
		 */
		nulltest = makeNode(NullTest);
		nulltest->arg = keyCol;
		nulltest->nulltesttype = IS_NULL;
		nulltest->argisrow = false;
		nulltest->location = -1;

		if (opexpr)
		{
			Expr	   *or;

			or = makeBoolExpr(OR_EXPR, list_make2(nulltest, opexpr), -1);
			result = list_make1(or);
		}
		else
			result = list_make1(nulltest);
	}

	/*
	 * Note that, in general, applying NOT to a constraint expression doesn't
	 * necessarily invert the set of rows it accepts, because NOT (NULL) is
	 * NULL.  However, the partition constraints we construct here never
	 * evaluate to NULL, so applying NOT works as intended.
	 */
	if (spec->is_default)
	{
		result = list_make1(make_ands_explicit(result));
		result = list_make1(makeBoolExpr(NOT_EXPR, result, -1));
	}

	return result;
}

/*
 * get_qual_for_range
 *
 * Returns an implicit-AND list of expressions to use as a range partition's
 * constraint, given the parent relation and partition bound structure.
 *
 * For a multi-column range partition key, say (a, b, c), with (al, bl, cl)
 * as the lower bound tuple and (au, bu, cu) as the upper bound tuple, we
 * generate an expression tree of the following form:
 *
 *	(a IS NOT NULL) and (b IS NOT NULL) and (c IS NOT NULL)
 *		AND
 *	(a > al OR (a = al AND b > bl) OR (a = al AND b = bl AND c >= cl))
 *		AND
 *	(a < au OR (a = au AND b < bu) OR (a = au AND b = bu AND c < cu))
 *
 * It is often the case that a prefix of lower and upper bound tuples contains
 * the same values, for example, (al = au), in which case, we will emit an
 * expression tree of the following form:
 *
 *	(a IS NOT NULL) and (b IS NOT NULL) and (c IS NOT NULL)
 *		AND
 *	(a = al)
 *		AND
 *	(b > bl OR (b = bl AND c >= cl))
 *		AND
 *	(b < bu OR (b = bu AND c < cu))
 *
 * If a bound datum is either MINVALUE or MAXVALUE, these expressions are
 * simplified using the fact that any value is greater than MINVALUE and less
 * than MAXVALUE. So, for example, if cu = MAXVALUE, c < cu is automatically
 * true, and we need not emit any expression for it, and the last line becomes
 *
 *	(b < bu) OR (b = bu), which is simplified to (b <= bu)
 *
 * In most common cases with only one partition column, say a, the following
 * expression tree will be generated: a IS NOT NULL AND a >= al AND a < au
 *
 * For default partition, it returns the negation of the constraints of all
 * the other partitions.
 *
 * External callers should pass for_default as false; we set it to true only
 * when recursing.
 */
static List *
get_qual_for_range(Relation parent, PartitionBoundSpec *spec,
				   bool for_default)
{
	List	   *result = NIL;
	ListCell   *cell1,
			   *cell2,
			   *partexprs_item,
			   *partexprs_item_saved;
	int			i,
				j;
	PartitionRangeDatum *ldatum,
			   *udatum;
	PartitionKey key = RelationGetPartitionKey(parent);
	Expr	   *keyCol;
	Const	   *lower_val,
			   *upper_val;
	List	   *lower_or_arms,
			   *upper_or_arms;
	int			num_or_arms,
				current_or_arm;
	ListCell   *lower_or_start_datum,
			   *upper_or_start_datum;
	bool		need_next_lower_arm,
				need_next_upper_arm;

	if (spec->is_default)
	{
		List	   *or_expr_args = NIL;
		PartitionDesc pdesc = RelationGetPartitionDesc(parent);
		Oid		   *inhoids = pdesc->oids;
		int			nparts = pdesc->nparts,
					i;

		for (i = 0; i < nparts; i++)
		{
			Oid			inhrelid = inhoids[i];
			HeapTuple	tuple;
			Datum		datum;
			bool		isnull;
			PartitionBoundSpec *bspec;

			tuple = SearchSysCache1(RELOID, inhrelid);
			if (!HeapTupleIsValid(tuple))
				elog(ERROR, "cache lookup failed for relation %u", inhrelid);

			datum = SysCacheGetAttr(RELOID, tuple,
									Anum_pg_class_relpartbound,
									&isnull);
			if (isnull)
				elog(ERROR, "null relpartbound for relation %u", inhrelid);

			bspec = (PartitionBoundSpec *)
				stringToNode(TextDatumGetCString(datum));
			if (!IsA(bspec, PartitionBoundSpec))
				elog(ERROR, "expected PartitionBoundSpec");

			if (!bspec->is_default)
			{
				List	   *part_qual;

				part_qual = get_qual_for_range(parent, bspec, true);

				/*
				 * AND the constraints of the partition and add to
				 * or_expr_args
				 */
				or_expr_args = lappend(or_expr_args, list_length(part_qual) > 1
									   ? makeBoolExpr(AND_EXPR, part_qual, -1)
									   : linitial(part_qual));
			}
			ReleaseSysCache(tuple);
		}

		if (or_expr_args != NIL)
		{
			Expr	   *other_parts_constr;

			/*
			 * Combine the constraints obtained for non-default partitions
			 * using OR.  As requested, each of the OR's args doesn't include
			 * the NOT NULL test for partition keys (which is to avoid its
			 * useless repetition).  Add the same now.
			 */
			other_parts_constr =
				makeBoolExpr(AND_EXPR,
							 lappend(get_range_nulltest(key),
									 list_length(or_expr_args) > 1
									 ? makeBoolExpr(OR_EXPR, or_expr_args,
													-1)
									 : linitial(or_expr_args)),
							 -1);

			/*
			 * Finally, the default partition contains everything *NOT*
			 * contained in the non-default partitions.
			 */
			result = list_make1(makeBoolExpr(NOT_EXPR,
											 list_make1(other_parts_constr), -1));
		}

		return result;
	}

	/*
	 * If it is the recursive call for default, we skip the get_range_nulltest
	 * to avoid accumulating the NullTest on the same keys for each partition.
	 */
	if (!for_default)
		result = get_range_nulltest(key);

	/*
	 * Iterate over the key columns and check if the corresponding lower and
	 * upper datums are equal using the btree equality operator for the
	 * column's type.  If equal, we emit single keyCol = common_value
	 * expression.  Starting from the first column for which the corresponding
	 * lower and upper bound datums are not equal, we generate OR expressions
	 * as shown in the function's header comment.
	 */
	i = 0;
	partexprs_item = list_head(key->partexprs);
	partexprs_item_saved = partexprs_item;	/* placate compiler */
	forboth(cell1, spec->lowerdatums, cell2, spec->upperdatums)
	{
		EState	   *estate;
		MemoryContext oldcxt;
		Expr	   *test_expr;
		ExprState  *test_exprstate;
		Datum		test_result;
		bool		isNull;

		ldatum = castNode(PartitionRangeDatum, lfirst(cell1));
		udatum = castNode(PartitionRangeDatum, lfirst(cell2));

		/*
		 * Since get_range_key_properties() modifies partexprs_item, and we
		 * might need to start over from the previous expression in the later
		 * part of this function, save away the current value.
		 */
		partexprs_item_saved = partexprs_item;

		get_range_key_properties(key, i, ldatum, udatum,
								 &partexprs_item,
								 &keyCol,
								 &lower_val, &upper_val);

		/*
		 * If either value is NULL, the corresponding partition bound is
		 * either MINVALUE or MAXVALUE, and we treat them as unequal, because
		 * even if they're the same, there is no common value to equate the
		 * key column with.
		 */
		if (!lower_val || !upper_val)
			break;

		/* Create the test expression */
		estate = CreateExecutorState();
		oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
		test_expr = make_partition_op_expr(key, i, BTEqualStrategyNumber,
										   (Expr *) lower_val,
										   (Expr *) upper_val);
		fix_opfuncids((Node *) test_expr);
		test_exprstate = ExecInitExpr(test_expr, NULL);
		test_result = ExecEvalExprSwitchContext(test_exprstate,
												GetPerTupleExprContext(estate),
												&isNull);
		MemoryContextSwitchTo(oldcxt);
		FreeExecutorState(estate);

		/* If not equal, go generate the OR expressions */
		if (!DatumGetBool(test_result))
			break;

		/*
		 * The bounds for the last key column can't be equal, because such a
		 * range partition would never be allowed to be defined (it would have
		 * an empty range otherwise).
		 */
		if (i == key->partnatts - 1)
			elog(ERROR, "invalid range bound specification");

		/* Equal, so generate keyCol = lower_val expression */
		result = lappend(result,
						 make_partition_op_expr(key, i, BTEqualStrategyNumber,
												keyCol, (Expr *) lower_val));

		i++;
	}

	/* First pair of lower_val and upper_val that are not equal. */
	lower_or_start_datum = cell1;
	upper_or_start_datum = cell2;

	/* OR will have as many arms as there are key columns left. */
	num_or_arms = key->partnatts - i;
	current_or_arm = 0;
	lower_or_arms = upper_or_arms = NIL;
	need_next_lower_arm = need_next_upper_arm = true;
	while (current_or_arm < num_or_arms)
	{
		List	   *lower_or_arm_args = NIL,
				   *upper_or_arm_args = NIL;

		/* Restart scan of columns from the i'th one */
		j = i;
		partexprs_item = partexprs_item_saved;

		for_both_cell(cell1, spec->lowerdatums, lower_or_start_datum,
					  cell2, spec->upperdatums, upper_or_start_datum)
		{
			PartitionRangeDatum *ldatum_next = NULL,
					   *udatum_next = NULL;

			ldatum = castNode(PartitionRangeDatum, lfirst(cell1));
			if (lnext(spec->lowerdatums, cell1))
				ldatum_next = castNode(PartitionRangeDatum,
									   lfirst(lnext(spec->lowerdatums, cell1)));
			udatum = castNode(PartitionRangeDatum, lfirst(cell2));
			if (lnext(spec->upperdatums, cell2))
				udatum_next = castNode(PartitionRangeDatum,
									   lfirst(lnext(spec->upperdatums, cell2)));
			get_range_key_properties(key, j, ldatum, udatum,
									 &partexprs_item,
									 &keyCol,
									 &lower_val, &upper_val);

			if (need_next_lower_arm && lower_val)
			{
				uint16		strategy;

				/*
				 * For the non-last columns of this arm, use the EQ operator.
				 * For the last column of this arm, use GT, unless this is the
				 * last column of the whole bound check, or the next bound
				 * datum is MINVALUE, in which case use GE.
				 */
				if (j - i < current_or_arm)
					strategy = BTEqualStrategyNumber;
				else if (j == key->partnatts - 1 ||
						 (ldatum_next &&
						  ldatum_next->kind == PARTITION_RANGE_DATUM_MINVALUE))
					strategy = BTGreaterEqualStrategyNumber;
				else
					strategy = BTGreaterStrategyNumber;

				lower_or_arm_args = lappend(lower_or_arm_args,
											make_partition_op_expr(key, j,
																   strategy,
																   keyCol,
																   (Expr *) lower_val));
			}

			if (need_next_upper_arm && upper_val)
			{
				uint16		strategy;

				/*
				 * For the non-last columns of this arm, use the EQ operator.
				 * For the last column of this arm, use LT, unless the next
				 * bound datum is MAXVALUE, in which case use LE.
				 */
				if (j - i < current_or_arm)
					strategy = BTEqualStrategyNumber;
				else if (udatum_next &&
						 udatum_next->kind == PARTITION_RANGE_DATUM_MAXVALUE)
					strategy = BTLessEqualStrategyNumber;
				else
					strategy = BTLessStrategyNumber;

				upper_or_arm_args = lappend(upper_or_arm_args,
											make_partition_op_expr(key, j,
																   strategy,
																   keyCol,
																   (Expr *) upper_val));
			}

			/*
			 * Did we generate enough of OR's arguments?  First arm considers
			 * the first of the remaining columns, second arm considers first
			 * two of the remaining columns, and so on.
			 */
			++j;
			if (j - i > current_or_arm)
			{
				/*
				 * We must not emit any more arms if the new column that will
				 * be considered is unbounded, or this one was.
				 */
				if (!lower_val || !ldatum_next ||
					ldatum_next->kind != PARTITION_RANGE_DATUM_VALUE)
					need_next_lower_arm = false;
				if (!upper_val || !udatum_next ||
					udatum_next->kind != PARTITION_RANGE_DATUM_VALUE)
					need_next_upper_arm = false;
				break;
			}
		}

		if (lower_or_arm_args != NIL)
			lower_or_arms = lappend(lower_or_arms,
									list_length(lower_or_arm_args) > 1
									? makeBoolExpr(AND_EXPR, lower_or_arm_args, -1)
									: linitial(lower_or_arm_args));

		if (upper_or_arm_args != NIL)
			upper_or_arms = lappend(upper_or_arms,
									list_length(upper_or_arm_args) > 1
									? makeBoolExpr(AND_EXPR, upper_or_arm_args, -1)
									: linitial(upper_or_arm_args));

		/* If no work to do in the next iteration, break away. */
		if (!need_next_lower_arm && !need_next_upper_arm)
			break;

		++current_or_arm;
	}

	/*
	 * Generate the OR expressions for each of lower and upper bounds (if
	 * required), and append to the list of implicitly ANDed list of
	 * expressions.
	 */
	if (lower_or_arms != NIL)
		result = lappend(result,
						 list_length(lower_or_arms) > 1
						 ? makeBoolExpr(OR_EXPR, lower_or_arms, -1)
						 : linitial(lower_or_arms));
	if (upper_or_arms != NIL)
		result = lappend(result,
						 list_length(upper_or_arms) > 1
						 ? makeBoolExpr(OR_EXPR, upper_or_arms, -1)
						 : linitial(upper_or_arms));

	/*
	 * As noted above, for non-default, we return list with constant TRUE. If
	 * the result is NIL during the recursive call for default, it implies
	 * this is the only other partition which can hold every value of the key
	 * except NULL. Hence we return the NullTest result skipped earlier.
	 */
	if (result == NIL)
		result = for_default
			? get_range_nulltest(key)
			: list_make1(makeBoolConst(true, false));

	return result;
}

/*
 * get_range_key_properties
 *		Returns range partition key information for a given column
 *
 * This is a subroutine for get_qual_for_range, and its API is pretty
 * specialized to that caller.
 *
 * Constructs an Expr for the key column (returned in *keyCol) and Consts
 * for the lower and upper range limits (returned in *lower_val and
 * *upper_val).  For MINVALUE/MAXVALUE limits, NULL is returned instead of
 * a Const.  All of these structures are freshly palloc'd.
 *
 * *partexprs_item points to the cell containing the next expression in
 * the key->partexprs list, or NULL.  It may be advanced upon return.
 */
static void
get_range_key_properties(PartitionKey key, int keynum,
						 PartitionRangeDatum *ldatum,
						 PartitionRangeDatum *udatum,
						 ListCell **partexprs_item,
						 Expr **keyCol,
						 Const **lower_val, Const **upper_val)
{
	/* Get partition key expression for this column */
	if (key->partattrs[keynum] != 0)
	{
		*keyCol = (Expr *) makeVar(1,
								   key->partattrs[keynum],
								   key->parttypid[keynum],
								   key->parttypmod[keynum],
								   key->parttypcoll[keynum],
								   0);
	}
	else
	{
		if (*partexprs_item == NULL)
			elog(ERROR, "wrong number of partition key expressions");
		*keyCol = copyObject(lfirst(*partexprs_item));
		*partexprs_item = lnext(key->partexprs, *partexprs_item);
	}

	/* Get appropriate Const nodes for the bounds */
	if (ldatum->kind == PARTITION_RANGE_DATUM_VALUE)
		*lower_val = castNode(Const, copyObject(ldatum->value));
	else
		*lower_val = NULL;

	if (udatum->kind == PARTITION_RANGE_DATUM_VALUE)
		*upper_val = castNode(Const, copyObject(udatum->value));
	else
		*upper_val = NULL;
}

/*
 * get_range_nulltest
 *
 * A non-default range partition table does not currently allow partition
 * keys to be null, so emit an IS NOT NULL expression for each key column.
 */
static List *
get_range_nulltest(PartitionKey key)
{
	List	   *result = NIL;
	NullTest   *nulltest;
	ListCell   *partexprs_item;
	int			i;

	partexprs_item = list_head(key->partexprs);
	for (i = 0; i < key->partnatts; i++)
	{
		Expr	   *keyCol;

		if (key->partattrs[i] != 0)
		{
			keyCol = (Expr *) makeVar(1,
									  key->partattrs[i],
									  key->parttypid[i],
									  key->parttypmod[i],
									  key->parttypcoll[i],
									  0);
		}
		else
		{
			if (partexprs_item == NULL)
				elog(ERROR, "wrong number of partition key expressions");
			keyCol = copyObject(lfirst(partexprs_item));
			partexprs_item = lnext(key->partexprs, partexprs_item);
		}

		nulltest = makeNode(NullTest);
		nulltest->arg = keyCol;
		nulltest->nulltesttype = IS_NOT_NULL;
		nulltest->argisrow = false;
		nulltest->location = -1;
		result = lappend(result, nulltest);
	}

	return result;
}

/*
 * compute_partition_hash_value
 *
 * Compute the hash value for given partition key values.
 */
uint64
compute_partition_hash_value(int partnatts, FmgrInfo *partsupfunc, Oid *partcollation,
							 Datum *values, bool *isnull)
{
	int			i;
	uint64		rowHash = 0;
	Datum		seed = UInt64GetDatum(HASH_PARTITION_SEED);

	for (i = 0; i < partnatts; i++)
	{
		/* Nulls are just ignored */
		if (!isnull[i])
		{
			Datum		hash;

			Assert(OidIsValid(partsupfunc[i].fn_oid));

			/*
			 * Compute hash for each datum value by calling respective
			 * datatype-specific hash functions of each partition key
			 * attribute.
			 */
			hash = FunctionCall2Coll(&partsupfunc[i], partcollation[i],
									 values[i], seed);

			/* Form a single 64-bit hash value */
			rowHash = hash_combine64(rowHash, DatumGetUInt64(hash));
		}
	}

	return rowHash;
}

/*
 * satisfies_hash_partition
 *
 * This is an SQL-callable function for use in hash partition constraints.
 * The first three arguments are the parent table OID, modulus, and remainder.
 * The remaining arguments are the value of the partitioning columns (or
 * expressions); these are hashed and the results are combined into a single
 * hash value by calling hash_combine64.
 *
 * Returns true if remainder produced when this computed single hash value is
 * divided by the given modulus is equal to given remainder, otherwise false.
 * NB: it's important that this never return null, as the constraint machinery
 * would consider that to be a "pass".
 *
 * See get_qual_for_hash() for usage.
 */
Datum
satisfies_hash_partition(PG_FUNCTION_ARGS)
{
	typedef struct ColumnsHashData
	{
		Oid			relid;
		int			nkeys;
		Oid			variadic_type;
		int16		variadic_typlen;
		bool		variadic_typbyval;
		char		variadic_typalign;
		Oid			partcollid[PARTITION_MAX_KEYS];
		FmgrInfo	partsupfunc[FLEXIBLE_ARRAY_MEMBER];
	} ColumnsHashData;
	Oid			parentId;
	int			modulus;
	int			remainder;
	Datum		seed = UInt64GetDatum(HASH_PARTITION_SEED);
	ColumnsHashData *my_extra;
	uint64		rowHash = 0;

	/* Return false if the parent OID, modulus, or remainder is NULL. */
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
		PG_RETURN_BOOL(false);
	parentId = PG_GETARG_OID(0);
	modulus = PG_GETARG_INT32(1);
	remainder = PG_GETARG_INT32(2);

	/* Sanity check modulus and remainder. */
	if (modulus <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("modulus for hash partition must be a positive integer")));
	if (remainder < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("remainder for hash partition must be a non-negative integer")));
	if (remainder >= modulus)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("remainder for hash partition must be less than modulus")));

	/*
	 * Cache hash function information.
	 */
	my_extra = (ColumnsHashData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL || my_extra->relid != parentId)
	{
		Relation	parent;
		PartitionKey key;
		int			j;

		/* Open parent relation and fetch partition key info */
		parent = relation_open(parentId, AccessShareLock);
		key = RelationGetPartitionKey(parent);

		/* Reject parent table that is not hash-partitioned. */
		if (key == NULL || key->strategy != PARTITION_STRATEGY_HASH)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("\"%s\" is not a hash partitioned table",
							get_rel_name(parentId))));

		if (!get_fn_expr_variadic(fcinfo->flinfo))
		{
			int			nargs = PG_NARGS() - 3;

			/* complain if wrong number of column values */
			if (key->partnatts != nargs)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("number of partitioning columns (%d) does not match number of partition keys provided (%d)",
								key->partnatts, nargs)));

			/* allocate space for our cache */
			fcinfo->flinfo->fn_extra =
				MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
									   offsetof(ColumnsHashData, partsupfunc) +
									   sizeof(FmgrInfo) * nargs);
			my_extra = (ColumnsHashData *) fcinfo->flinfo->fn_extra;
			my_extra->relid = parentId;
			my_extra->nkeys = key->partnatts;
			memcpy(my_extra->partcollid, key->partcollation,
				   key->partnatts * sizeof(Oid));

			/* check argument types and save fmgr_infos */
			for (j = 0; j < key->partnatts; ++j)
			{
				Oid			argtype = get_fn_expr_argtype(fcinfo->flinfo, j + 3);

				if (argtype != key->parttypid[j] && !IsBinaryCoercible(argtype, key->parttypid[j]))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("column %d of the partition key has type \"%s\", but supplied value is of type \"%s\"",
									j + 1, format_type_be(key->parttypid[j]), format_type_be(argtype))));

				fmgr_info_copy(&my_extra->partsupfunc[j],
							   &key->partsupfunc[j],
							   fcinfo->flinfo->fn_mcxt);
			}
		}
		else
		{
			ArrayType  *variadic_array = PG_GETARG_ARRAYTYPE_P(3);

			/* allocate space for our cache -- just one FmgrInfo in this case */
			fcinfo->flinfo->fn_extra =
				MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
									   offsetof(ColumnsHashData, partsupfunc) +
									   sizeof(FmgrInfo));
			my_extra = (ColumnsHashData *) fcinfo->flinfo->fn_extra;
			my_extra->relid = parentId;
			my_extra->nkeys = key->partnatts;
			my_extra->variadic_type = ARR_ELEMTYPE(variadic_array);
			get_typlenbyvalalign(my_extra->variadic_type,
								 &my_extra->variadic_typlen,
								 &my_extra->variadic_typbyval,
								 &my_extra->variadic_typalign);
			my_extra->partcollid[0] = key->partcollation[0];

			/* check argument types */
			for (j = 0; j < key->partnatts; ++j)
				if (key->parttypid[j] != my_extra->variadic_type)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("column %d of the partition key has type \"%s\", but supplied value is of type \"%s\"",
									j + 1,
									format_type_be(key->parttypid[j]),
									format_type_be(my_extra->variadic_type))));

			fmgr_info_copy(&my_extra->partsupfunc[0],
						   &key->partsupfunc[0],
						   fcinfo->flinfo->fn_mcxt);
		}

		/* Hold lock until commit */
		relation_close(parent, NoLock);
	}

	if (!OidIsValid(my_extra->variadic_type))
	{
		int			nkeys = my_extra->nkeys;
		int			i;

		/*
		 * For a non-variadic call, neither the number of arguments nor their
		 * types can change across calls, so avoid the expense of rechecking
		 * here.
		 */

		for (i = 0; i < nkeys; i++)
		{
			Datum		hash;

			/* keys start from fourth argument of function. */
			int			argno = i + 3;

			if (PG_ARGISNULL(argno))
				continue;

			hash = FunctionCall2Coll(&my_extra->partsupfunc[i],
									 my_extra->partcollid[i],
									 PG_GETARG_DATUM(argno),
									 seed);

			/* Form a single 64-bit hash value */
			rowHash = hash_combine64(rowHash, DatumGetUInt64(hash));
		}
	}
	else
	{
		ArrayType  *variadic_array = PG_GETARG_ARRAYTYPE_P(3);
		int			i;
		int			nelems;
		Datum	   *datum;
		bool	   *isnull;

		deconstruct_array(variadic_array,
						  my_extra->variadic_type,
						  my_extra->variadic_typlen,
						  my_extra->variadic_typbyval,
						  my_extra->variadic_typalign,
						  &datum, &isnull, &nelems);

		/* complain if wrong number of column values */
		if (nelems != my_extra->nkeys)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("number of partitioning columns (%d) does not match number of partition keys provided (%d)",
							my_extra->nkeys, nelems)));

		for (i = 0; i < nelems; i++)
		{
			Datum		hash;

			if (isnull[i])
				continue;

			hash = FunctionCall2Coll(&my_extra->partsupfunc[0],
									 my_extra->partcollid[0],
									 datum[i],
									 seed);

			/* Form a single 64-bit hash value */
			rowHash = hash_combine64(rowHash, DatumGetUInt64(hash));
		}
	}

	PG_RETURN_BOOL(rowHash % modulus == remainder);
}
