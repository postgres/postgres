/*-------------------------------------------------------------------------
 *
 * partition.c
 *		  Partitioning related data structures and functions.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *		  src/backend/catalog/partition.c
 *
 *-------------------------------------------------------------------------
*/

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/sysattr.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaddress.h"
#include "catalog/partition.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_inherits_fn.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_type.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/var.h"
#include "rewrite/rewriteManip.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/syscache.h"

/*
 * Information about bounds of a partitioned relation
 *
 * A list partition datum that is known to be NULL is never put into the
 * datums array. Instead, it is tracked using the null_index field.
 *
 * In the case of range partitioning, ndatums will typically be far less than
 * 2 * nparts, because a partition's upper bound and the next partition's lower
 * bound are the same in most common cases, and we only store one of them (the
 * upper bound).
 *
 * In the case of list partitioning, the indexes array stores one entry for
 * every datum, which is the index of the partition that accepts a given datum.
 * In case of range partitioning, it stores one entry per distinct range
 * datum, which is the index of the partition for which a given datum
 * is an upper bound.
 */

typedef struct PartitionBoundInfoData
{
	char		strategy;		/* list or range bounds? */
	int			ndatums;		/* Length of the datums following array */
	Datum	  **datums;			/* Array of datum-tuples with key->partnatts
								 * datums each */
	PartitionRangeDatumKind **kind; /* The kind of each range bound datum;
									 * NULL for list partitioned tables */
	int		   *indexes;		/* Partition indexes; one entry per member of
								 * the datums array (plus one if range
								 * partitioned table) */
	int			null_index;		/* Index of the null-accepting partition; -1
								 * if there isn't one */
	int			default_index;	/* Index of the default partition; -1 if there
								 * isn't one */
} PartitionBoundInfoData;

#define partition_bound_accepts_nulls(bi) ((bi)->null_index != -1)
#define partition_bound_has_default(bi) ((bi)->default_index != -1)

/*
 * When qsort'ing partition bounds after reading from the catalog, each bound
 * is represented with one of the following structs.
 */

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

static int32 qsort_partition_list_value_cmp(const void *a, const void *b,
							   void *arg);
static int32 qsort_partition_rbound_cmp(const void *a, const void *b,
						   void *arg);

static Oid get_partition_operator(PartitionKey key, int col,
					   StrategyNumber strategy, bool *need_relabel);
static Expr *make_partition_op_expr(PartitionKey key, int keynum,
					   uint16 strategy, Expr *arg1, Expr *arg2);
static void get_range_key_properties(PartitionKey key, int keynum,
						 PartitionRangeDatum *ldatum,
						 PartitionRangeDatum *udatum,
						 ListCell **partexprs_item,
						 Expr **keyCol,
						 Const **lower_val, Const **upper_val);
static List *get_qual_for_list(Relation parent, PartitionBoundSpec *spec);
static List *get_qual_for_range(Relation parent, PartitionBoundSpec *spec,
				   bool for_default);
static List *get_range_nulltest(PartitionKey key);
static List *generate_partition_qual(Relation rel);

static PartitionRangeBound *make_one_range_bound(PartitionKey key, int index,
					 List *datums, bool lower);
static int32 partition_rbound_cmp(PartitionKey key,
					 Datum *datums1, PartitionRangeDatumKind *kind1,
					 bool lower1, PartitionRangeBound *b2);
static int32 partition_rbound_datum_cmp(PartitionKey key,
						   Datum *rb_datums, PartitionRangeDatumKind *rb_kind,
						   Datum *tuple_datums);

static int32 partition_bound_cmp(PartitionKey key,
					PartitionBoundInfo boundinfo,
					int offset, void *probe, bool probe_is_bound);
static int partition_bound_bsearch(PartitionKey key,
						PartitionBoundInfo boundinfo,
						void *probe, bool probe_is_bound, bool *is_equal);
static void get_partition_dispatch_recurse(Relation rel, Relation parent,
							   List **pds, List **leaf_part_oids);

/*
 * RelationBuildPartitionDesc
 *		Form rel's partition descriptor
 *
 * Not flushed from the cache by RelationClearRelation() unless changed because
 * of addition or removal of partition.
 */
void
RelationBuildPartitionDesc(Relation rel)
{
	List	   *inhoids,
			   *partoids;
	Oid		   *oids = NULL;
	List	   *boundspecs = NIL;
	ListCell   *cell;
	int			i,
				nparts;
	PartitionKey key = RelationGetPartitionKey(rel);
	PartitionDesc result;
	MemoryContext oldcxt;

	int			ndatums = 0;
	int			default_index = -1;

	/* List partitioning specific */
	PartitionListValue **all_values = NULL;
	int			null_index = -1;

	/* Range partitioning specific */
	PartitionRangeBound **rbounds = NULL;

	/*
	 * The following could happen in situations where rel has a pg_class entry
	 * but not the pg_partitioned_table entry yet.
	 */
	if (key == NULL)
		return;

	/* Get partition oids from pg_inherits */
	inhoids = find_inheritance_children(RelationGetRelid(rel), NoLock);

	/* Collect bound spec nodes in a list */
	i = 0;
	partoids = NIL;
	foreach(cell, inhoids)
	{
		Oid			inhrelid = lfirst_oid(cell);
		HeapTuple	tuple;
		Datum		datum;
		bool		isnull;
		Node	   *boundspec;

		tuple = SearchSysCache1(RELOID, inhrelid);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", inhrelid);

		/*
		 * It is possible that the pg_class tuple of a partition has not been
		 * updated yet to set its relpartbound field.  The only case where
		 * this happens is when we open the parent relation to check using its
		 * partition descriptor that a new partition's bound does not overlap
		 * some existing partition.
		 */
		if (!((Form_pg_class) GETSTRUCT(tuple))->relispartition)
		{
			ReleaseSysCache(tuple);
			continue;
		}

		datum = SysCacheGetAttr(RELOID, tuple,
								Anum_pg_class_relpartbound,
								&isnull);
		Assert(!isnull);
		boundspec = (Node *) stringToNode(TextDatumGetCString(datum));

		/*
		 * Sanity check: If the PartitionBoundSpec says this is the default
		 * partition, its OID should correspond to whatever's stored in
		 * pg_partitioned_table.partdefid; if not, the catalog is corrupt.
		 */
		if (castNode(PartitionBoundSpec, boundspec)->is_default)
		{
			Oid			partdefid;

			partdefid = get_default_partition_oid(RelationGetRelid(rel));
			if (partdefid != inhrelid)
				elog(ERROR, "expected partdefid %u, but got %u",
					 inhrelid, partdefid);
		}

		boundspecs = lappend(boundspecs, boundspec);
		partoids = lappend_oid(partoids, inhrelid);
		ReleaseSysCache(tuple);
	}

	nparts = list_length(partoids);

	if (nparts > 0)
	{
		oids = (Oid *) palloc(nparts * sizeof(Oid));
		i = 0;
		foreach(cell, partoids)
			oids[i++] = lfirst_oid(cell);

		/* Convert from node to the internal representation */
		if (key->strategy == PARTITION_STRATEGY_LIST)
		{
			List	   *non_null_values = NIL;

			/*
			 * Create a unified list of non-null values across all partitions.
			 */
			i = 0;
			null_index = -1;
			foreach(cell, boundspecs)
			{
				PartitionBoundSpec *spec = castNode(PartitionBoundSpec,
													lfirst(cell));
				ListCell   *c;

				if (spec->strategy != PARTITION_STRATEGY_LIST)
					elog(ERROR, "invalid strategy in partition bound spec");

				/*
				 * Note the index of the partition bound spec for the default
				 * partition. There's no datum to add to the list of non-null
				 * datums for this partition.
				 */
				if (spec->is_default)
				{
					default_index = i;
					i++;
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
						 * Never put a null into the values array, flag
						 * instead for the code further down below where we
						 * construct the actual relcache struct.
						 */
						if (null_index != -1)
							elog(ERROR, "found null more than once");
						null_index = i;
					}

					if (list_value)
						non_null_values = lappend(non_null_values,
												  list_value);
				}

				i++;
			}

			ndatums = list_length(non_null_values);

			/*
			 * Collect all list values in one array. Alongside the value, we
			 * also save the index of partition the value comes from.
			 */
			all_values = (PartitionListValue **) palloc(ndatums *
														sizeof(PartitionListValue *));
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
		}
		else if (key->strategy == PARTITION_STRATEGY_RANGE)
		{
			int			k;
			PartitionRangeBound **all_bounds,
					   *prev;

			all_bounds = (PartitionRangeBound **) palloc0(2 * nparts *
														  sizeof(PartitionRangeBound *));

			/*
			 * Create a unified list of range bounds across all the
			 * partitions.
			 */
			i = ndatums = 0;
			foreach(cell, boundspecs)
			{
				PartitionBoundSpec *spec = castNode(PartitionBoundSpec,
													lfirst(cell));
				PartitionRangeBound *lower,
						   *upper;

				if (spec->strategy != PARTITION_STRATEGY_RANGE)
					elog(ERROR, "invalid strategy in partition bound spec");

				/*
				 * Note the index of the partition bound spec for the default
				 * partition. There's no datum to add to the allbounds array
				 * for this partition.
				 */
				if (spec->is_default)
				{
					default_index = i++;
					continue;
				}

				lower = make_one_range_bound(key, i, spec->lowerdatums,
											 true);
				upper = make_one_range_bound(key, i, spec->upperdatums,
											 false);
				all_bounds[ndatums++] = lower;
				all_bounds[ndatums++] = upper;
				i++;
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
					 * If the bounds are both MINVALUE or MAXVALUE, stop now
					 * and treat them as equal, since any values after this
					 * point must be ignored.
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
				 * Only if the bound is distinct save it into a temporary
				 * array i.e. rbounds which is later copied into boundinfo
				 * datums array.
				 */
				if (is_distinct)
					rbounds[k++] = all_bounds[i];

				prev = cur;
			}

			/* Update ndatums to hold the count of distinct datums. */
			ndatums = k;
		}
		else
			elog(ERROR, "unexpected partition strategy: %d",
				 (int) key->strategy);
	}

	/* Now build the actual relcache partition descriptor */
	rel->rd_pdcxt = AllocSetContextCreate(CacheMemoryContext,
										  RelationGetRelationName(rel),
										  ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(rel->rd_pdcxt);

	result = (PartitionDescData *) palloc0(sizeof(PartitionDescData));
	result->nparts = nparts;
	if (nparts > 0)
	{
		PartitionBoundInfo boundinfo;
		int		   *mapping;
		int			next_index = 0;

		result->oids = (Oid *) palloc0(nparts * sizeof(Oid));

		boundinfo = (PartitionBoundInfoData *)
			palloc0(sizeof(PartitionBoundInfoData));
		boundinfo->strategy = key->strategy;
		boundinfo->default_index = -1;
		boundinfo->ndatums = ndatums;
		boundinfo->null_index = -1;
		boundinfo->datums = (Datum **) palloc0(ndatums * sizeof(Datum *));

		/* Initialize mapping array with invalid values */
		mapping = (int *) palloc(sizeof(int) * nparts);
		for (i = 0; i < nparts; i++)
			mapping[i] = -1;

		switch (key->strategy)
		{
			case PARTITION_STRATEGY_LIST:
				{
					boundinfo->indexes = (int *) palloc(ndatums * sizeof(int));

					/*
					 * Copy values.  Indexes of individual values are mapped
					 * to canonical values so that they match for any two list
					 * partitioned tables with same number of partitions and
					 * same lists per partition.  One way to canonicalize is
					 * to assign the index in all_values[] of the smallest
					 * value of each partition, as the index of all of the
					 * partition's values.
					 */
					for (i = 0; i < ndatums; i++)
					{
						boundinfo->datums[i] = (Datum *) palloc(sizeof(Datum));
						boundinfo->datums[i][0] = datumCopy(all_values[i]->value,
															key->parttypbyval[0],
															key->parttyplen[0]);

						/* If the old index has no mapping, assign one */
						if (mapping[all_values[i]->index] == -1)
							mapping[all_values[i]->index] = next_index++;

						boundinfo->indexes[i] = mapping[all_values[i]->index];
					}

					/*
					 * If null-accepting partition has no mapped index yet,
					 * assign one.  This could happen if such partition
					 * accepts only null and hence not covered in the above
					 * loop which only handled non-null values.
					 */
					if (null_index != -1)
					{
						Assert(null_index >= 0);
						if (mapping[null_index] == -1)
							mapping[null_index] = next_index++;
						boundinfo->null_index = mapping[null_index];
					}

					/* Assign mapped index for the default partition. */
					if (default_index != -1)
					{
						/*
						 * The default partition accepts any value not
						 * specified in the lists of other partitions, hence
						 * it should not get mapped index while assigning
						 * those for non-null datums.
						 */
						Assert(default_index >= 0 &&
							   mapping[default_index] == -1);
						mapping[default_index] = next_index++;
						boundinfo->default_index = mapping[default_index];
					}

					/* All partition must now have a valid mapping */
					Assert(next_index == nparts);
					break;
				}

			case PARTITION_STRATEGY_RANGE:
				{
					boundinfo->kind = (PartitionRangeDatumKind **)
						palloc(ndatums *
							   sizeof(PartitionRangeDatumKind *));
					boundinfo->indexes = (int *) palloc((ndatums + 1) *
														sizeof(int));

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
						 * Any lower bounds in the rbounds array have invalid
						 * indexes assigned, because the values between the
						 * previous bound (if there is one) and this (lower)
						 * bound are not part of the range of any existing
						 * partition.
						 */
						if (rbounds[i]->lower)
							boundinfo->indexes[i] = -1;
						else
						{
							int			orig_index = rbounds[i]->index;

							/* If the old index has no mapping, assign one */
							if (mapping[orig_index] == -1)
								mapping[orig_index] = next_index++;

							boundinfo->indexes[i] = mapping[orig_index];
						}
					}

					/* Assign mapped index for the default partition. */
					if (default_index != -1)
					{
						Assert(default_index >= 0 && mapping[default_index] == -1);
						mapping[default_index] = next_index++;
						boundinfo->default_index = mapping[default_index];
					}
					boundinfo->indexes[i] = -1;
					break;
				}

			default:
				elog(ERROR, "unexpected partition strategy: %d",
					 (int) key->strategy);
		}

		result->boundinfo = boundinfo;

		/*
		 * Now assign OIDs from the original array into mapped indexes of the
		 * result array.  Order of OIDs in the former is defined by the
		 * catalog scan that retrieved them, whereas that in the latter is
		 * defined by canonicalized representation of the list values or the
		 * range bounds.
		 */
		for (i = 0; i < nparts; i++)
			result->oids[mapping[i]] = oids[i];
		pfree(mapping);
	}

	MemoryContextSwitchTo(oldcxt);
	rel->rd_partdesc = result;
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

				/* Non-finite bounds are equal without further examination. */
				if (b1->kind[i][j] != PARTITION_RANGE_DATUM_VALUE)
					continue;
			}

			/*
			 * Compare the actual values. Note that it would be both incorrect
			 * and unsafe to invoke the comparison operator derived from the
			 * partitioning specification here.  It would be incorrect because
			 * we want the relcache entry to be updated for ANY change to the
			 * partition bounds, not just those that the partitioning operator
			 * thinks are significant.  It would be unsafe because we might
			 * reach this code in the context of an aborted transaction, and
			 * an arbitrary partitioning operator might not be safe in that
			 * context.  datumIsEqual() should be simple enough to be safe.
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

	return true;
}

/*
 * Return a copy of given PartitionBoundInfo structure. The data types of bounds
 * are described by given partition key specification.
 */
extern PartitionBoundInfo
partition_bounds_copy(PartitionBoundInfo src,
					  PartitionKey key)
{
	PartitionBoundInfo	dest;
	int		i;
	int		ndatums;
	int		partnatts;
	int		num_indexes;

	dest = (PartitionBoundInfo) palloc(sizeof(PartitionBoundInfoData));

	dest->strategy = src->strategy;
	ndatums = dest->ndatums = src->ndatums;
	partnatts = key->partnatts;

	/* Range partitioned table has an extra index. */
	num_indexes = key->strategy == PARTITION_STRATEGY_RANGE ? ndatums + 1 : ndatums;

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

	for (i = 0; i < ndatums; i++)
	{
		int		j;
		dest->datums[i] = (Datum *) palloc(sizeof(Datum) * partnatts);

		for (j = 0; j < partnatts; j++)
		{
			if (dest->kind == NULL ||
				dest->kind[i][j] == PARTITION_RANGE_DATUM_VALUE)
				dest->datums[i][j] = datumCopy(src->datums[i][j],
											   key->parttypbyval[j],
											   key->parttyplen[j]);
		}
	}

	dest->indexes = (int *) palloc(sizeof(int) * num_indexes);
	memcpy(dest->indexes, src->indexes, sizeof(int) * num_indexes);

	dest->null_index = src->null_index;
	dest->default_index = src->default_index;

	return dest;
}

/*
 * check_new_partition_bound
 *
 * Checks if the new partition's bound overlaps any of the existing partitions
 * of parent.  Also performs additional checks as necessary per strategy.
 */
void
check_new_partition_bound(char *relname, Relation parent,
						  PartitionBoundSpec *spec)
{
	PartitionKey key = RelationGetPartitionKey(parent);
	PartitionDesc partdesc = RelationGetPartitionDesc(parent);
	PartitionBoundInfo boundinfo = partdesc->boundinfo;
	ParseState *pstate = make_parsestate(NULL);
	int			with = -1;
	bool		overlap = false;

	if (spec->is_default)
	{
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

						if (!val->constisnull)
						{
							int			offset;
							bool		equal;

							offset = partition_bound_bsearch(key, boundinfo,
															 &val->constvalue,
															 true, &equal);
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

				Assert(spec->strategy == PARTITION_STRATEGY_RANGE);
				lower = make_one_range_bound(key, -1, spec->lowerdatums, true);
				upper = make_one_range_bound(key, -1, spec->upperdatums, false);

				/*
				 * First check if the resulting range would be empty with
				 * specified lower and upper bounds
				 */
				if (partition_rbound_cmp(key, lower->datums, lower->kind, true,
										 upper) >= 0)
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("empty range bound specified for partition \"%s\"",
									relname),
							 errdetail("Specified lower bound %s is greater than or equal to upper bound %s.",
									   get_range_partbound_string(spec->lowerdatums),
									   get_range_partbound_string(spec->upperdatums)),
							 parser_errposition(pstate, spec->location)));
				}

				if (partdesc->nparts > 0)
				{
					PartitionBoundInfo boundinfo = partdesc->boundinfo;
					int			offset;
					bool		equal;

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
					offset = partition_bound_bsearch(key, boundinfo, lower,
													 true, &equal);

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
							int32		cmpval;

							cmpval = partition_bound_cmp(key, boundinfo,
														 offset + 1, upper,
														 true);
							if (cmpval < 0)
							{
								/*
								 * The new partition overlaps with the
								 * existing partition between offset + 1 and
								 * offset + 2.
								 */
								overlap = true;
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
						overlap = true;
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
				 parser_errposition(pstate, spec->location)));
	}
}

/*
 * check_default_allows_bound
 *
 * This function checks if there exists a row in the default partition that
 * would properly belong to the new partition being added.  If it finds one,
 * it throws an error.
 */
void
check_default_allows_bound(Relation parent, Relation default_rel,
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
	 * If the existing constraints on the default partition imply that it will
	 * not contain any row that would belong to the new partition, we can
	 * avoid scanning the default partition.
	 */
	if (PartConstraintImpliedByRelConstraint(default_rel, def_part_constraints))
	{
		ereport(INFO,
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
		Expr	   *constr;
		Expr	   *partition_constraint;
		EState	   *estate;
		HeapTuple	tuple;
		ExprState  *partqualstate = NULL;
		Snapshot	snapshot;
		TupleDesc	tupdesc;
		ExprContext *econtext;
		HeapScanDesc scan;
		MemoryContext oldCxt;
		TupleTableSlot *tupslot;

		/* Lock already taken above. */
		if (part_relid != RelationGetRelid(default_rel))
		{
			part_rel = heap_open(part_relid, NoLock);

			/*
			 * If the partition constraints on default partition child imply
			 * that it will not contain any row that would belong to the new
			 * partition, we can avoid scanning the child table.
			 */
			if (PartConstraintImpliedByRelConstraint(part_rel,
													 def_part_constraints))
			{
				ereport(INFO,
						(errmsg("updated partition constraint for default partition \"%s\" is implied by existing constraints",
								RelationGetRelationName(part_rel))));

				heap_close(part_rel, NoLock);
				continue;
			}
		}
		else
			part_rel = default_rel;

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
				heap_close(part_rel, NoLock);

			continue;
		}

		tupdesc = CreateTupleDescCopy(RelationGetDescr(part_rel));
		constr = linitial(def_part_constraints);
		partition_constraint = (Expr *)
			map_partition_varattnos((List *) constr,
									1, part_rel, parent, NULL);
		estate = CreateExecutorState();

		/* Build expression execution states for partition check quals */
		partqualstate = ExecPrepareExpr(partition_constraint, estate);

		econtext = GetPerTupleExprContext(estate);
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		scan = heap_beginscan(part_rel, snapshot, 0, NULL);
		tupslot = MakeSingleTupleTableSlot(tupdesc);

		/*
		 * Switch to per-tuple memory context and reset it for each tuple
		 * produced, so we don't leak memory.
		 */
		oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			ExecStoreTuple(tuple, tupslot, InvalidBuffer, false);
			econtext->ecxt_scantuple = tupslot;

			if (!ExecCheck(partqualstate, econtext))
				ereport(ERROR,
						(errcode(ERRCODE_CHECK_VIOLATION),
						 errmsg("updated partition constraint for default partition \"%s\" would be violated by some row",
								RelationGetRelationName(default_rel))));

			ResetExprContext(econtext);
			CHECK_FOR_INTERRUPTS();
		}

		MemoryContextSwitchTo(oldCxt);
		heap_endscan(scan);
		UnregisterSnapshot(snapshot);
		ExecDropSingleTupleTableSlot(tupslot);
		FreeExecutorState(estate);

		if (RelationGetRelid(default_rel) != RelationGetRelid(part_rel))
			heap_close(part_rel, NoLock);	/* keep the lock until commit */
	}
}

/*
 * get_partition_parent
 *
 * Returns inheritance parent of a partition by scanning pg_inherits
 *
 * Note: Because this function assumes that the relation whose OID is passed
 * as an argument will have precisely one parent, it should only be called
 * when it is known that the relation is a partition.
 */
Oid
get_partition_parent(Oid relid)
{
	Form_pg_inherits form;
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tuple;
	Oid			result;

	catalogRelation = heap_open(InheritsRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1],
				Anum_pg_inherits_inhseqno,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(1));

	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId, true,
							  NULL, 2, key);

	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for parent of relation %u", relid);

	form = (Form_pg_inherits) GETSTRUCT(tuple);
	result = form->inhparent;

	systable_endscan(scan);
	heap_close(catalogRelation, AccessShareLock);

	return result;
}

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
 * map_partition_varattnos - maps varattno of any Vars in expr from the
 * parent attno to partition attno.
 *
 * We must allow for cases where physical attnos of a partition can be
 * different from the parent's.
 *
 * If found_whole_row is not NULL, *found_whole_row returns whether a
 * whole-row variable was found in the input expression.
 *
 * Note: this will work on any node tree, so really the argument and result
 * should be declared "Node *".  But a substantial majority of the callers
 * are working on Lists, so it's less messy to do the casts internally.
 */
List *
map_partition_varattnos(List *expr, int target_varno,
						Relation partrel, Relation parent,
						bool *found_whole_row)
{
	bool		my_found_whole_row = false;

	if (expr != NIL)
	{
		AttrNumber *part_attnos;

		part_attnos = convert_tuples_by_name_map(RelationGetDescr(partrel),
												 RelationGetDescr(parent),
												 gettext_noop("could not convert row type"));
		expr = (List *) map_variable_attnos((Node *) expr,
											target_varno, 0,
											part_attnos,
											RelationGetDescr(parent)->natts,
											RelationGetForm(partrel)->reltype,
											&my_found_whole_row);
	}

	if (found_whole_row)
		*found_whole_row = my_found_whole_row;

	return expr;
}

/*
 * RelationGetPartitionQual
 *
 * Returns a list of partition quals
 */
List *
RelationGetPartitionQual(Relation rel)
{
	/* Quick exit */
	if (!rel->rd_rel->relispartition)
		return NIL;

	return generate_partition_qual(rel);
}

/*
 * get_partition_qual_relid
 *
 * Returns an expression tree describing the passed-in relation's partition
 * constraint. If there is no partition constraint returns NULL; this can
 * happen if the default partition is the only partition.
 */
Expr *
get_partition_qual_relid(Oid relid)
{
	Relation	rel = heap_open(relid, AccessShareLock);
	Expr	   *result = NULL;
	List	   *and_args;

	/* Do the work only if this relation is a partition. */
	if (rel->rd_rel->relispartition)
	{
		and_args = generate_partition_qual(rel);

		if (and_args == NIL)
			result = NULL;
		else if (list_length(and_args) > 1)
			result = makeBoolExpr(AND_EXPR, and_args, -1);
		else
			result = linitial(and_args);
	}

	/* Keep the lock. */
	heap_close(rel, NoLock);

	return result;
}

/*
 * RelationGetPartitionDispatchInfo
 *		Returns information necessary to route tuples down a partition tree
 *
 * The number of elements in the returned array (that is, the number of
 * PartitionDispatch objects for the partitioned tables in the partition tree)
 * is returned in *num_parted and a list of the OIDs of all the leaf
 * partitions of rel is returned in *leaf_part_oids.
 *
 * All the relations in the partition tree (including 'rel') must have been
 * locked (using at least the AccessShareLock) by the caller.
 */
PartitionDispatch *
RelationGetPartitionDispatchInfo(Relation rel,
								 int *num_parted, List **leaf_part_oids)
{
	List	   *pdlist = NIL;
	PartitionDispatchData **pd;
	ListCell   *lc;
	int			i;

	Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	*num_parted = 0;
	*leaf_part_oids = NIL;

	get_partition_dispatch_recurse(rel, NULL, &pdlist, leaf_part_oids);
	*num_parted = list_length(pdlist);
	pd = (PartitionDispatchData **) palloc(*num_parted *
										   sizeof(PartitionDispatchData *));
	i = 0;
	foreach(lc, pdlist)
	{
		pd[i++] = lfirst(lc);
	}

	return pd;
}

/*
 * get_partition_dispatch_recurse
 *		Recursively expand partition tree rooted at rel
 *
 * As the partition tree is expanded in a depth-first manner, we maintain two
 * global lists: of PartitionDispatch objects corresponding to partitioned
 * tables in *pds and of the leaf partition OIDs in *leaf_part_oids.
 *
 * Note that the order of OIDs of leaf partitions in leaf_part_oids matches
 * the order in which the planner's expand_partitioned_rtentry() processes
 * them.  It's not necessarily the case that the offsets match up exactly,
 * because constraint exclusion might prune away some partitions on the
 * planner side, whereas we'll always have the complete list; but unpruned
 * partitions will appear in the same order in the plan as they are returned
 * here.
 */
static void
get_partition_dispatch_recurse(Relation rel, Relation parent,
							   List **pds, List **leaf_part_oids)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	PartitionDesc partdesc = RelationGetPartitionDesc(rel);
	PartitionKey partkey = RelationGetPartitionKey(rel);
	PartitionDispatch pd;
	int			i;

	check_stack_depth();

	/* Build a PartitionDispatch for this table and add it to *pds. */
	pd = (PartitionDispatch) palloc(sizeof(PartitionDispatchData));
	*pds = lappend(*pds, pd);
	pd->reldesc = rel;
	pd->key = partkey;
	pd->keystate = NIL;
	pd->partdesc = partdesc;
	if (parent != NULL)
	{
		/*
		 * For every partitioned table other than the root, we must store a
		 * tuple table slot initialized with its tuple descriptor and a tuple
		 * conversion map to convert a tuple from its parent's rowtype to its
		 * own. That is to make sure that we are looking at the correct row
		 * using the correct tuple descriptor when computing its partition key
		 * for tuple routing.
		 */
		pd->tupslot = MakeSingleTupleTableSlot(tupdesc);
		pd->tupmap = convert_tuples_by_name(RelationGetDescr(parent),
											tupdesc,
											gettext_noop("could not convert row type"));
	}
	else
	{
		/* Not required for the root partitioned table */
		pd->tupslot = NULL;
		pd->tupmap = NULL;
	}

	/*
	 * Go look at each partition of this table.  If it's a leaf partition,
	 * simply add its OID to *leaf_part_oids.  If it's a partitioned table,
	 * recursively call get_partition_dispatch_recurse(), so that its
	 * partitions are processed as well and a corresponding PartitionDispatch
	 * object gets added to *pds.
	 *
	 * About the values in pd->indexes: for a leaf partition, it contains the
	 * leaf partition's position in the global list *leaf_part_oids minus 1,
	 * whereas for a partitioned table partition, it contains the partition's
	 * position in the global list *pds multiplied by -1.  The latter is
	 * multiplied by -1 to distinguish partitioned tables from leaf partitions
	 * when going through the values in pd->indexes.  So, for example, when
	 * using it during tuple-routing, encountering a value >= 0 means we found
	 * a leaf partition.  It is immediately returned as the index in the array
	 * of ResultRelInfos of all the leaf partitions, using which we insert the
	 * tuple into that leaf partition.  A negative value means we found a
	 * partitioned table.  The value multiplied by -1 is returned as the index
	 * in the array of PartitionDispatch objects of all partitioned tables in
	 * the tree.  This value is used to continue the search in the next level
	 * of the partition tree.
	 */
	pd->indexes = (int *) palloc(partdesc->nparts * sizeof(int));
	for (i = 0; i < partdesc->nparts; i++)
	{
		Oid			partrelid = partdesc->oids[i];

		if (get_rel_relkind(partrelid) != RELKIND_PARTITIONED_TABLE)
		{
			*leaf_part_oids = lappend_oid(*leaf_part_oids, partrelid);
			pd->indexes[i] = list_length(*leaf_part_oids) - 1;
		}
		else
		{
			/*
			 * We assume all tables in the partition tree were already locked
			 * by the caller.
			 */
			Relation	partrel = heap_open(partrelid, NoLock);

			pd->indexes[i] = -list_length(*pds);
			get_partition_dispatch_recurse(partrel, rel, pds, leaf_part_oids);
		}
	}
}

/* Module-local functions */

/*
 * get_partition_operator
 *
 * Return oid of the operator of given strategy for a given partition key
 * column.
 */
static Oid
get_partition_operator(PartitionKey key, int col, StrategyNumber strategy,
					   bool *need_relabel)
{
	Oid			operoid;

	/*
	 * First check if there exists an operator of the given strategy, with
	 * this column's type as both its lefttype and righttype, in the
	 * partitioning operator family specified for the column.
	 */
	operoid = get_opfamily_member(key->partopfamily[col],
								  key->parttypid[col],
								  key->parttypid[col],
								  strategy);

	/*
	 * If one doesn't exist, we must resort to using an operator in the same
	 * operator family but with the operator class declared input type.  It is
	 * OK to do so, because the column's type is known to be binary-coercible
	 * with the operator class input type (otherwise, the operator class in
	 * question would not have been accepted as the partitioning operator
	 * class).  We must however inform the caller to wrap the non-Const
	 * expression with a RelabelType node to denote the implicit coercion. It
	 * ensures that the resulting expression structurally matches similarly
	 * processed expressions within the optimizer.
	 */
	if (!OidIsValid(operoid))
	{
		operoid = get_opfamily_member(key->partopfamily[col],
									  key->partopcintype[col],
									  key->partopcintype[col],
									  strategy);
		if (!OidIsValid(operoid))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 strategy, key->partopcintype[col], key->partopcintype[col],
				 key->partopfamily[col]);
		*need_relabel = true;
	}
	else
		*need_relabel = false;

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
				ScalarArrayOpExpr *saopexpr;

				/* Build leftop = ANY (rightop) */
				saopexpr = makeNode(ScalarArrayOpExpr);
				saopexpr->opno = operoid;
				saopexpr->opfuncid = get_opcode(operoid);
				saopexpr->useOr = true;
				saopexpr->inputcollid = key->partcollation[keynum];
				saopexpr->args = list_make2(arg1, arg2);
				saopexpr->location = -1;

				result = (Expr *) saopexpr;
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
 * get_qual_for_list
 *
 * Returns an implicit-AND list of expressions to use as a list partition's
 * constraint, given the partition key and bound structures.
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
	ArrayExpr  *arr;
	Expr	   *opexpr;
	NullTest   *nulltest;
	ListCell   *cell;
	List	   *arrelems = NIL;
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

			arrelems = lappend(arrelems, val);
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
				arrelems = lappend(arrelems, copyObject(val));
		}
	}

	if (arrelems)
	{
		/* Construct an ArrayExpr for the non-null partition values */
		arr = makeNode(ArrayExpr);
		arr->array_typeid = !type_is_array(key->parttypid[0])
			? get_array_type(key->parttypid[0])
			: key->parttypid[0];
		arr->array_collid = key->parttypcoll[0];
		arr->element_typeid = key->parttypid[0];
		arr->elements = arrelems;
		arr->multidims = false;
		arr->location = -1;

		/* Generate the main expression, i.e., keyCol = ANY (arr) */
		opexpr = make_partition_op_expr(key, 0, BTEqualStrategyNumber,
										keyCol, (Expr *) arr);
	}
	else
	{
		/* If there are no partition values, we don't need an = ANY expr */
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
		*partexprs_item = lnext(*partexprs_item);
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
			partexprs_item = lnext(partexprs_item);
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
 * get_qual_for_range
 *
 * Returns an implicit-AND list of expressions to use as a range partition's
 * constraint, given the partition key and bound structures.
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
 *	(b < bu) OR (b = bu AND c < cu))
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

			Assert(!isnull);
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
			/* OR all the non-default partition constraints; then negate it */
			result = lappend(result,
							 list_length(or_expr_args) > 1
							 ? makeBoolExpr(OR_EXPR, or_expr_args, -1)
							 : linitial(or_expr_args));
			result = list_make1(makeBoolExpr(NOT_EXPR, result, -1));
		}

		return result;
	}

	lower_or_start_datum = list_head(spec->lowerdatums);
	upper_or_start_datum = list_head(spec->upperdatums);
	num_or_arms = key->partnatts;

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

		for_both_cell(cell1, lower_or_start_datum, cell2, upper_or_start_datum)
		{
			PartitionRangeDatum *ldatum_next = NULL,
					   *udatum_next = NULL;

			ldatum = castNode(PartitionRangeDatum, lfirst(cell1));
			if (lnext(cell1))
				ldatum_next = castNode(PartitionRangeDatum,
									   lfirst(lnext(cell1)));
			udatum = castNode(PartitionRangeDatum, lfirst(cell2));
			if (lnext(cell2))
				udatum_next = castNode(PartitionRangeDatum,
									   lfirst(lnext(cell2)));
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
 * generate_partition_qual
 *
 * Generate partition predicate from rel's partition bound expression. The
 * function returns a NIL list if there is no predicate.
 *
 * Result expression tree is stored CacheMemoryContext to ensure it survives
 * as long as the relcache entry. But we should be running in a less long-lived
 * working context. To avoid leaking cache memory if this routine fails partway
 * through, we build in working memory and then copy the completed structure
 * into cache memory.
 */
static List *
generate_partition_qual(Relation rel)
{
	HeapTuple	tuple;
	MemoryContext oldcxt;
	Datum		boundDatum;
	bool		isnull;
	PartitionBoundSpec *bound;
	List	   *my_qual = NIL,
			   *result = NIL;
	Relation	parent;
	bool		found_whole_row;

	/* Guard against stack overflow due to overly deep partition tree */
	check_stack_depth();

	/* Quick copy */
	if (rel->rd_partcheck != NIL)
		return copyObject(rel->rd_partcheck);

	/* Grab at least an AccessShareLock on the parent table */
	parent = heap_open(get_partition_parent(RelationGetRelid(rel)),
					   AccessShareLock);

	/* Get pg_class.relpartbound */
	tuple = SearchSysCache1(RELOID, RelationGetRelid(rel));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(rel));

	boundDatum = SysCacheGetAttr(RELOID, tuple,
								 Anum_pg_class_relpartbound,
								 &isnull);
	if (isnull)					/* should not happen */
		elog(ERROR, "relation \"%s\" has relpartbound = null",
			 RelationGetRelationName(rel));
	bound = castNode(PartitionBoundSpec,
					 stringToNode(TextDatumGetCString(boundDatum)));
	ReleaseSysCache(tuple);

	my_qual = get_qual_from_partbound(rel, parent, bound);

	/* Add the parent's quals to the list (if any) */
	if (parent->rd_rel->relispartition)
		result = list_concat(generate_partition_qual(parent), my_qual);
	else
		result = my_qual;

	/*
	 * Change Vars to have partition's attnos instead of the parent's. We do
	 * this after we concatenate the parent's quals, because we want every Var
	 * in it to bear this relation's attnos. It's safe to assume varno = 1
	 * here.
	 */
	result = map_partition_varattnos(result, 1, rel, parent,
									 &found_whole_row);
	/* There can never be a whole-row reference here */
	if (found_whole_row)
		elog(ERROR, "unexpected whole-row reference found in partition key");

	/* Save a copy in the relcache */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	rel->rd_partcheck = copyObject(result);
	MemoryContextSwitchTo(oldcxt);

	/* Keep the parent locked until commit */
	heap_close(parent, NoLock);

	return result;
}

/* ----------------
 *		FormPartitionKeyDatum
 *			Construct values[] and isnull[] arrays for the partition key
 *			of a tuple.
 *
 *	pd				Partition dispatch object of the partitioned table
 *	slot			Heap tuple from which to extract partition key
 *	estate			executor state for evaluating any partition key
 *					expressions (must be non-NULL)
 *	values			Array of partition key Datums (output area)
 *	isnull			Array of is-null indicators (output area)
 *
 * the ecxt_scantuple slot of estate's per-tuple expr context must point to
 * the heap tuple passed in.
 * ----------------
 */
void
FormPartitionKeyDatum(PartitionDispatch pd,
					  TupleTableSlot *slot,
					  EState *estate,
					  Datum *values,
					  bool *isnull)
{
	ListCell   *partexpr_item;
	int			i;

	if (pd->key->partexprs != NIL && pd->keystate == NIL)
	{
		/* Check caller has set up context correctly */
		Assert(estate != NULL &&
			   GetPerTupleExprContext(estate)->ecxt_scantuple == slot);

		/* First time through, set up expression evaluation state */
		pd->keystate = ExecPrepareExprList(pd->key->partexprs, estate);
	}

	partexpr_item = list_head(pd->keystate);
	for (i = 0; i < pd->key->partnatts; i++)
	{
		AttrNumber	keycol = pd->key->partattrs[i];
		Datum		datum;
		bool		isNull;

		if (keycol != 0)
		{
			/* Plain column; get the value directly from the heap tuple */
			datum = slot_getattr(slot, keycol, &isNull);
		}
		else
		{
			/* Expression; need to evaluate it */
			if (partexpr_item == NULL)
				elog(ERROR, "wrong number of partition key expressions");
			datum = ExecEvalExprSwitchContext((ExprState *) lfirst(partexpr_item),
											  GetPerTupleExprContext(estate),
											  &isNull);
			partexpr_item = lnext(partexpr_item);
		}
		values[i] = datum;
		isnull[i] = isNull;
	}

	if (partexpr_item != NULL)
		elog(ERROR, "wrong number of partition key expressions");
}

/*
 * get_partition_for_tuple
 *		Finds a leaf partition for tuple contained in *slot
 *
 * Returned value is the sequence number of the leaf partition thus found,
 * or -1 if no leaf partition is found for the tuple.  *failed_at is set
 * to the OID of the partitioned table whose partition was not found in
 * the latter case.
 */
int
get_partition_for_tuple(PartitionDispatch *pd,
						TupleTableSlot *slot,
						EState *estate,
						PartitionDispatchData **failed_at,
						TupleTableSlot **failed_slot)
{
	PartitionDispatch parent;
	Datum		values[PARTITION_MAX_KEYS];
	bool		isnull[PARTITION_MAX_KEYS];
	int			result;
	ExprContext *ecxt = GetPerTupleExprContext(estate);
	TupleTableSlot *ecxt_scantuple_old = ecxt->ecxt_scantuple;

	/* start with the root partitioned table */
	parent = pd[0];
	while (true)
	{
		PartitionKey key = parent->key;
		PartitionDesc partdesc = parent->partdesc;
		TupleTableSlot *myslot = parent->tupslot;
		TupleConversionMap *map = parent->tupmap;
		int			cur_index = -1;

		if (myslot != NULL && map != NULL)
		{
			HeapTuple	tuple = ExecFetchSlotTuple(slot);

			ExecClearTuple(myslot);
			tuple = do_convert_tuple(tuple, map);
			ExecStoreTuple(tuple, myslot, InvalidBuffer, true);
			slot = myslot;
		}

		/* Quick exit */
		if (partdesc->nparts == 0)
		{
			*failed_at = parent;
			*failed_slot = slot;
			result = -1;
			goto error_exit;
		}

		/*
		 * Extract partition key from tuple. Expression evaluation machinery
		 * that FormPartitionKeyDatum() invokes expects ecxt_scantuple to
		 * point to the correct tuple slot.  The slot might have changed from
		 * what was used for the parent table if the table of the current
		 * partitioning level has different tuple descriptor from the parent.
		 * So update ecxt_scantuple accordingly.
		 */
		ecxt->ecxt_scantuple = slot;
		FormPartitionKeyDatum(parent, slot, estate, values, isnull);

		/* Route as appropriate based on partitioning strategy. */
		switch (key->strategy)
		{
			case PARTITION_STRATEGY_LIST:

				if (isnull[0])
				{
					if (partition_bound_accepts_nulls(partdesc->boundinfo))
						cur_index = partdesc->boundinfo->null_index;
				}
				else
				{
					bool		equal = false;
					int			cur_offset;

					cur_offset = partition_bound_bsearch(key,
														 partdesc->boundinfo,
														 values,
														 false,
														 &equal);
					if (cur_offset >= 0 && equal)
						cur_index = partdesc->boundinfo->indexes[cur_offset];
				}
				break;

			case PARTITION_STRATEGY_RANGE:
				{
					bool		equal = false,
								range_partkey_has_null = false;
					int			cur_offset;
					int			i;

					/*
					 * No range includes NULL, so this will be accepted by the
					 * default partition if there is one, and otherwise
					 * rejected.
					 */
					for (i = 0; i < key->partnatts; i++)
					{
						if (isnull[i] &&
							partition_bound_has_default(partdesc->boundinfo))
						{
							range_partkey_has_null = true;
							break;
						}
						else if (isnull[i])
						{
							*failed_at = parent;
							*failed_slot = slot;
							result = -1;
							goto error_exit;
						}
					}

					/*
					 * No need to search for partition, as the null key will
					 * be routed to the default partition.
					 */
					if (range_partkey_has_null)
						break;

					cur_offset = partition_bound_bsearch(key,
														 partdesc->boundinfo,
														 values,
														 false,
														 &equal);

					/*
					 * The offset returned is such that the bound at
					 * cur_offset is less than or equal to the tuple value, so
					 * the bound at offset+1 is the upper bound.
					 */
					cur_index = partdesc->boundinfo->indexes[cur_offset + 1];
				}
				break;

			default:
				elog(ERROR, "unexpected partition strategy: %d",
					 (int) key->strategy);
		}

		/*
		 * cur_index < 0 means we failed to find a partition of this parent.
		 * Use the default partition, if there is one.
		 */
		if (cur_index < 0)
			cur_index = partdesc->boundinfo->default_index;

		/*
		 * If cur_index is still less than 0 at this point, there's no
		 * partition for this tuple.  Otherwise, we either found the leaf
		 * partition, or a child partitioned table through which we have to
		 * route the tuple.
		 */
		if (cur_index < 0)
		{
			result = -1;
			*failed_at = parent;
			*failed_slot = slot;
			break;
		}
		else if (parent->indexes[cur_index] >= 0)
		{
			result = parent->indexes[cur_index];
			break;
		}
		else
			parent = pd[-parent->indexes[cur_index]];
	}

error_exit:
	ecxt->ecxt_scantuple = ecxt_scantuple_old;
	return result;
}

/*
 * qsort_partition_list_value_cmp
 *
 * Compare two list partition bound datums
 */
static int32
qsort_partition_list_value_cmp(const void *a, const void *b, void *arg)
{
	Datum		val1 = (*(const PartitionListValue **) a)->value,
				val2 = (*(const PartitionListValue **) b)->value;
	PartitionKey key = (PartitionKey) arg;

	return DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[0],
										   key->partcollation[0],
										   val1, val2));
}

/*
 * make_one_range_bound
 *
 * Return a PartitionRangeBound given a list of PartitionRangeDatum elements
 * and a flag telling whether the bound is lower or not.  Made into a function
 * because there are multiple sites that want to use this facility.
 */
static PartitionRangeBound *
make_one_range_bound(PartitionKey key, int index, List *datums, bool lower)
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

/* Used when sorting range bounds across all range partitions */
static int32
qsort_partition_rbound_cmp(const void *a, const void *b, void *arg)
{
	PartitionRangeBound *b1 = (*(PartitionRangeBound *const *) a);
	PartitionRangeBound *b2 = (*(PartitionRangeBound *const *) b);
	PartitionKey key = (PartitionKey) arg;

	return partition_rbound_cmp(key, b1->datums, b1->kind, b1->lower, b2);
}

/*
 * partition_rbound_cmp
 *
 * Return for two range bounds whether the 1st one (specified in datum1,
 * kind1, and lower1) is <, =, or > the bound specified in *b2.
 *
 * Note that if the values of the two range bounds compare equal, then we take
 * into account whether they are upper or lower bounds, and an upper bound is
 * considered to be smaller than a lower bound. This is important to the way
 * that RelationBuildPartitionDesc() builds the PartitionBoundInfoData
 * structure, which only stores the upper bound of a common boundary between
 * two contiguous partitions.
 */
static int32
partition_rbound_cmp(PartitionKey key,
					 Datum *datums1, PartitionRangeDatumKind *kind1,
					 bool lower1, PartitionRangeBound *b2)
{
	int32		cmpval = 0;		/* placate compiler */
	int			i;
	Datum	   *datums2 = b2->datums;
	PartitionRangeDatumKind *kind2 = b2->kind;
	bool		lower2 = b2->lower;

	for (i = 0; i < key->partnatts; i++)
	{
		/*
		 * First, handle cases where the column is unbounded, which should not
		 * invoke the comparison procedure, and should not consider any later
		 * columns. Note that the PartitionRangeDatumKind enum elements
		 * compare the same way as the values they represent.
		 */
		if (kind1[i] < kind2[i])
			return -1;
		else if (kind1[i] > kind2[i])
			return 1;
		else if (kind1[i] != PARTITION_RANGE_DATUM_VALUE)

			/*
			 * The column bounds are both MINVALUE or both MAXVALUE. No later
			 * columns should be considered, but we still need to compare
			 * whether they are upper or lower bounds.
			 */
			break;

		cmpval = DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[i],
												 key->partcollation[i],
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

	return cmpval;
}

/*
 * partition_rbound_datum_cmp
 *
 * Return whether range bound (specified in rb_datums, rb_kind, and rb_lower)
 * is <, =, or > partition key of tuple (tuple_datums)
 */
static int32
partition_rbound_datum_cmp(PartitionKey key,
						   Datum *rb_datums, PartitionRangeDatumKind *rb_kind,
						   Datum *tuple_datums)
{
	int			i;
	int32		cmpval = -1;

	for (i = 0; i < key->partnatts; i++)
	{
		if (rb_kind[i] == PARTITION_RANGE_DATUM_MINVALUE)
			return -1;
		else if (rb_kind[i] == PARTITION_RANGE_DATUM_MAXVALUE)
			return 1;

		cmpval = DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[i],
												 key->partcollation[i],
												 rb_datums[i],
												 tuple_datums[i]));
		if (cmpval != 0)
			break;
	}

	return cmpval;
}

/*
 * partition_bound_cmp
 *
 * Return whether the bound at offset in boundinfo is <, =, or > the argument
 * specified in *probe.
 */
static int32
partition_bound_cmp(PartitionKey key, PartitionBoundInfo boundinfo,
					int offset, void *probe, bool probe_is_bound)
{
	Datum	   *bound_datums = boundinfo->datums[offset];
	int32		cmpval = -1;

	switch (key->strategy)
	{
		case PARTITION_STRATEGY_LIST:
			cmpval = DatumGetInt32(FunctionCall2Coll(&key->partsupfunc[0],
													 key->partcollation[0],
													 bound_datums[0],
													 *(Datum *) probe));
			break;

		case PARTITION_STRATEGY_RANGE:
			{
				PartitionRangeDatumKind *kind = boundinfo->kind[offset];

				if (probe_is_bound)
				{
					/*
					 * We need to pass whether the existing bound is a lower
					 * bound, so that two equal-valued lower and upper bounds
					 * are not regarded equal.
					 */
					bool		lower = boundinfo->indexes[offset] < 0;

					cmpval = partition_rbound_cmp(key,
												  bound_datums, kind, lower,
												  (PartitionRangeBound *) probe);
				}
				else
					cmpval = partition_rbound_datum_cmp(key,
														bound_datums, kind,
														(Datum *) probe);
				break;
			}

		default:
			elog(ERROR, "unexpected partition strategy: %d",
				 (int) key->strategy);
	}

	return cmpval;
}

/*
 * Binary search on a collection of partition bounds. Returns greatest
 * bound in array boundinfo->datums which is less than or equal to *probe.
 * If all bounds in the array are greater than *probe, -1 is returned.
 *
 * *probe could either be a partition bound or a Datum array representing
 * the partition key of a tuple being routed; probe_is_bound tells which.
 * We pass that down to the comparison function so that it can interpret the
 * contents of *probe accordingly.
 *
 * *is_equal is set to whether the bound at the returned index is equal with
 * *probe.
 */
static int
partition_bound_bsearch(PartitionKey key, PartitionBoundInfo boundinfo,
						void *probe, bool probe_is_bound, bool *is_equal)
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
		cmpval = partition_bound_cmp(key, boundinfo, mid, probe,
									 probe_is_bound);
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
 * get_default_oid_from_partdesc
 *
 * Given a partition descriptor, return the OID of the default partition, if
 * one exists; else, return InvalidOid.
 */
Oid
get_default_oid_from_partdesc(PartitionDesc partdesc)
{
	if (partdesc && partdesc->boundinfo &&
		partition_bound_has_default(partdesc->boundinfo))
		return partdesc->oids[partdesc->boundinfo->default_index];

	return InvalidOid;
}

/*
 * get_default_partition_oid
 *
 * Given a relation OID, return the OID of the default partition, if one
 * exists.  Use get_default_oid_from_partdesc where possible, for
 * efficiency.
 */
Oid
get_default_partition_oid(Oid parentId)
{
	HeapTuple	tuple;
	Oid			defaultPartId = InvalidOid;

	tuple = SearchSysCache1(PARTRELID, ObjectIdGetDatum(parentId));

	if (HeapTupleIsValid(tuple))
	{
		Form_pg_partitioned_table part_table_form;

		part_table_form = (Form_pg_partitioned_table) GETSTRUCT(tuple);
		defaultPartId = part_table_form->partdefid;
		ReleaseSysCache(tuple);
	}

	return defaultPartId;
}

/*
 * update_default_partition_oid
 *
 * Update pg_partition_table.partdefid with a new default partition OID.
 */
void
update_default_partition_oid(Oid parentId, Oid defaultPartId)
{
	HeapTuple	tuple;
	Relation	pg_partitioned_table;
	Form_pg_partitioned_table part_table_form;

	pg_partitioned_table = heap_open(PartitionedRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(PARTRELID, ObjectIdGetDatum(parentId));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for partition key of relation %u",
			 parentId);

	part_table_form = (Form_pg_partitioned_table) GETSTRUCT(tuple);
	part_table_form->partdefid = defaultPartId;
	CatalogTupleUpdate(pg_partitioned_table, &tuple->t_self, tuple);

	heap_freetuple(tuple);
	heap_close(pg_partitioned_table, RowExclusiveLock);
}

/*
 * get_proposed_default_constraint
 *
 * This function returns the negation of new_part_constraints, which
 * would be an integral part of the default partition constraints after
 * addition of the partition to which the new_part_constraints belongs.
 */
List *
get_proposed_default_constraint(List *new_part_constraints)
{
	Expr	   *defPartConstraint;

	defPartConstraint = make_ands_explicit(new_part_constraints);

	/*
	 * Derive the partition constraints of default partition by negating the
	 * given partition constraints. The partition constraint never evaluates
	 * to NULL, so negating it like this is safe.
	 */
	defPartConstraint = makeBoolExpr(NOT_EXPR,
									 list_make1(defPartConstraint),
									 -1);
	defPartConstraint =
		(Expr *) eval_const_expressions(NULL,
										(Node *) defPartConstraint);
	defPartConstraint = canonicalize_qual(defPartConstraint);

	return list_make1(defPartConstraint);
}
