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
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "optimizer/clauses.h"
#include "optimizer/planmain.h"
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
 * datums array. Instead, it is tracked using has_null and null_index fields.
 *
 * In the case of range partitioning, ndatums will typically be far less than
 * 2 * nparts, because a partition's upper bound and the next partition's lower
 * bound are the same in most common cases, and we only store one of them.
 *
 * In the case of list partitioning, the indexes array stores one entry for
 * every datum, which is the index of the partition that accepts a given datum.
 * In case of range partitioning, it stores one entry per distinct range
 * datum, which is the index of the partition for which a given datum
 * is an upper bound.
 */

/* Ternary value to represent what's contained in a range bound datum */
typedef enum RangeDatumContent
{
	RANGE_DATUM_FINITE = 0,		/* actual datum stored elsewhere */
	RANGE_DATUM_NEG_INF,		/* negative infinity */
	RANGE_DATUM_POS_INF			/* positive infinity */
} RangeDatumContent;

typedef struct PartitionBoundInfoData
{
	char		strategy;		/* list or range bounds? */
	int			ndatums;		/* Length of the datums following array */
	Datum	  **datums;			/* Array of datum-tuples with key->partnatts
								 * datums each */
	RangeDatumContent **content;/* what's contained in each range bound datum?
								 * (see the above enum); NULL for list
								 * partitioned tables */
	int		   *indexes;		/* Partition indexes; one entry per member of
								 * the datums array (plus one if range
								 * partitioned table) */
	bool		has_null;		/* Is there a null-accepting partition? false
								 * for range partitioned tables */
	int			null_index;		/* Index of the null-accepting partition; -1
								 * for range partitioned tables */
} PartitionBoundInfoData;

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
	RangeDatumContent *content; /* what's contained in each datum? */
	bool		lower;			/* this is the lower (vs upper) bound */
} PartitionRangeBound;

static int32 qsort_partition_list_value_cmp(const void *a, const void *b,
							   void *arg);
static int32 qsort_partition_rbound_cmp(const void *a, const void *b,
						   void *arg);

static List *get_qual_for_list(PartitionKey key, PartitionBoundSpec *spec);
static List *get_qual_for_range(PartitionKey key, PartitionBoundSpec *spec);
static Oid get_partition_operator(PartitionKey key, int col,
					   StrategyNumber strategy, bool *need_relabel);
static List *generate_partition_qual(Relation rel);

static PartitionRangeBound *make_one_range_bound(PartitionKey key, int index,
					 List *datums, bool lower);
static int32 partition_rbound_cmp(PartitionKey key,
					 Datum *datums1, RangeDatumContent *content1, bool lower1,
					 PartitionRangeBound *b2);
static int32 partition_rbound_datum_cmp(PartitionKey key,
						   Datum *rb_datums, RangeDatumContent *rb_content,
						   Datum *tuple_datums);

static int32 partition_bound_cmp(PartitionKey key,
					PartitionBoundInfo boundinfo,
					int offset, void *probe, bool probe_is_bound);
static int partition_bound_bsearch(PartitionKey key,
						PartitionBoundInfo boundinfo,
						void *probe, bool probe_is_bound, bool *is_equal);

/* Support get_partition_for_tuple() */
static void FormPartitionKeyDatum(PartitionDispatch pd,
					  TupleTableSlot *slot,
					  EState *estate,
					  Datum *values,
					  bool *isnull);

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

	/* List partitioning specific */
	PartitionListValue **all_values = NULL;
	bool		found_null = false;
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
			found_null = false;
			null_index = -1;
			foreach(cell, boundspecs)
			{
				ListCell   *c;
				PartitionBoundSpec *spec = lfirst(cell);

				if (spec->strategy != PARTITION_STRATEGY_LIST)
					elog(ERROR, "invalid strategy in partition bound spec");

				foreach(c, spec->listdatums)
				{
					Const	   *val = lfirst(c);
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
						if (found_null)
							elog(ERROR, "found null more than once");
						found_null = true;
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
			int			j,
						k;
			PartitionRangeBound **all_bounds,
					   *prev;
			bool	   *distinct_indexes;

			all_bounds = (PartitionRangeBound **) palloc0(2 * nparts *
											  sizeof(PartitionRangeBound *));
			distinct_indexes = (bool *) palloc(2 * nparts * sizeof(bool));

			/*
			 * Create a unified list of range bounds across all the
			 * partitions.
			 */
			i = j = 0;
			foreach(cell, boundspecs)
			{
				PartitionBoundSpec *spec = lfirst(cell);
				PartitionRangeBound *lower,
						   *upper;

				if (spec->strategy != PARTITION_STRATEGY_RANGE)
					elog(ERROR, "invalid strategy in partition bound spec");

				lower = make_one_range_bound(key, i, spec->lowerdatums,
											 true);
				upper = make_one_range_bound(key, i, spec->upperdatums,
											 false);
				all_bounds[j] = lower;
				all_bounds[j + 1] = upper;
				j += 2;
				i++;
			}
			Assert(j == 2 * nparts);

			/* Sort all the bounds in ascending order */
			qsort_arg(all_bounds, 2 * nparts,
					  sizeof(PartitionRangeBound *),
					  qsort_partition_rbound_cmp,
					  (void *) key);

			/*
			 * Count the number of distinct bounds to allocate an array of
			 * that size.
			 */
			ndatums = 0;
			prev = NULL;
			for (i = 0; i < 2 * nparts; i++)
			{
				PartitionRangeBound *cur = all_bounds[i];
				bool		is_distinct = false;
				int			j;

				/* Is current bound is distinct from the previous? */
				for (j = 0; j < key->partnatts; j++)
				{
					Datum		cmpval;

					if (prev == NULL)
					{
						is_distinct = true;
						break;
					}

					/*
					 * If either of them has infinite element, we can't equate
					 * them.  Even when both are infinite, they'd have
					 * opposite signs, because only one of cur and prev is a
					 * lower bound).
					 */
					if (cur->content[j] != RANGE_DATUM_FINITE ||
						prev->content[j] != RANGE_DATUM_FINITE)
					{
						is_distinct = true;
						break;
					}
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
				 * Count the current bound if it is distinct from the previous
				 * one.  Also, store if the index i contains a distinct bound
				 * that we'd like put in the relcache array.
				 */
				if (is_distinct)
				{
					distinct_indexes[i] = true;
					ndatums++;
				}
				else
					distinct_indexes[i] = false;

				prev = cur;
			}

			/*
			 * Finally save them in an array from where they will be copied
			 * into the relcache.
			 */
			rbounds = (PartitionRangeBound **) palloc(ndatums *
											  sizeof(PartitionRangeBound *));
			k = 0;
			for (i = 0; i < 2 * nparts; i++)
			{
				if (distinct_indexes[i])
					rbounds[k++] = all_bounds[i];
			}
			Assert(k == ndatums);
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
		boundinfo->ndatums = ndatums;
		boundinfo->datums = (Datum **) palloc0(ndatums * sizeof(Datum *));

		/* Initialize mapping array with invalid values */
		mapping = (int *) palloc(sizeof(int) * nparts);
		for (i = 0; i < nparts; i++)
			mapping[i] = -1;

		switch (key->strategy)
		{
			case PARTITION_STRATEGY_LIST:
				{
					boundinfo->has_null = found_null;
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
					if (found_null)
					{
						Assert(null_index >= 0);
						if (mapping[null_index] == -1)
							mapping[null_index] = next_index++;
					}

					/* All partition must now have a valid mapping */
					Assert(next_index == nparts);

					if (found_null)
						boundinfo->null_index = mapping[null_index];
					else
						boundinfo->null_index = -1;
					break;
				}

			case PARTITION_STRATEGY_RANGE:
				{
					boundinfo->content = (RangeDatumContent **) palloc(ndatums *
												sizeof(RangeDatumContent *));
					boundinfo->indexes = (int *) palloc((ndatums + 1) *
														sizeof(int));

					for (i = 0; i < ndatums; i++)
					{
						int			j;

						boundinfo->datums[i] = (Datum *) palloc(key->partnatts *
															  sizeof(Datum));
						boundinfo->content[i] = (RangeDatumContent *)
							palloc(key->partnatts *
								   sizeof(RangeDatumContent));
						for (j = 0; j < key->partnatts; j++)
						{
							if (rbounds[i]->content[j] == RANGE_DATUM_FINITE)
								boundinfo->datums[i][j] =
									datumCopy(rbounds[i]->datums[j],
											  key->parttypbyval[j],
											  key->parttyplen[j]);
							/* Remember, we are storing the tri-state value. */
							boundinfo->content[i][j] = rbounds[i]->content[j];
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

							/* If the old index is has no mapping, assign one */
							if (mapping[orig_index] == -1)
								mapping[orig_index] = next_index++;

							boundinfo->indexes[i] = mapping[orig_index];
						}
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
		 * catalog scan that retrived them, whereas that in the latter is
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
partition_bounds_equal(PartitionKey key,
					   PartitionBoundInfo b1, PartitionBoundInfo b2)
{
	int			i;

	if (b1->strategy != b2->strategy)
		return false;

	if (b1->ndatums != b2->ndatums)
		return false;

	if (b1->has_null != b2->has_null)
		return false;

	if (b1->null_index != b2->null_index)
		return false;

	for (i = 0; i < b1->ndatums; i++)
	{
		int			j;

		for (j = 0; j < key->partnatts; j++)
		{
			/* For range partitions, the bounds might not be finite. */
			if (b1->content != NULL)
			{
				/*
				 * A finite bound always differs from an infinite bound, and
				 * different kinds of infinities differ from each other.
				 */
				if (b1->content[i][j] != b2->content[i][j])
					return false;

				/* Non-finite bounds are equal without further examination. */
				if (b1->content[i][j] != RANGE_DATUM_FINITE)
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
							  key->parttypbyval[j],
							  key->parttyplen[j]))
				return false;
		}

		if (b1->indexes[i] != b2->indexes[i])
			return false;
	}

	/* There are ndatums+1 indexes in case of range partitions */
	if (key->strategy == PARTITION_STRATEGY_RANGE &&
		b1->indexes[i] != b2->indexes[i])
		return false;

	return true;
}

/*
 * check_new_partition_bound
 *
 * Checks if the new partition's bound overlaps any of the existing partitions
 * of parent.  Also performs additional checks as necessary per strategy.
 */
void
check_new_partition_bound(char *relname, Relation parent, Node *bound)
{
	PartitionBoundSpec *spec = (PartitionBoundSpec *) bound;
	PartitionKey key = RelationGetPartitionKey(parent);
	PartitionDesc partdesc = RelationGetPartitionDesc(parent);
	ParseState *pstate = make_parsestate(NULL);
	int			with = -1;
	bool		overlap = false;

	switch (key->strategy)
	{
		case PARTITION_STRATEGY_LIST:
			{
				Assert(spec->strategy == PARTITION_STRATEGY_LIST);

				if (partdesc->nparts > 0)
				{
					PartitionBoundInfo boundinfo = partdesc->boundinfo;
					ListCell   *cell;

					Assert(boundinfo &&
						   boundinfo->strategy == PARTITION_STRATEGY_LIST &&
						   (boundinfo->ndatums > 0 || boundinfo->has_null));

					foreach(cell, spec->listdatums)
					{
						Const	   *val = lfirst(cell);

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
						else if (boundinfo->has_null)
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
				if (partition_rbound_cmp(key, lower->datums, lower->content, true,
										 upper) >= 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					errmsg("cannot create range partition with empty range"),
							 parser_errposition(pstate, spec->location)));

				if (partdesc->nparts > 0)
				{
					PartitionBoundInfo boundinfo = partdesc->boundinfo;
					int			off1,
								off2;
					bool		equal = false;

					Assert(boundinfo && boundinfo->ndatums > 0 &&
						   boundinfo->strategy == PARTITION_STRATEGY_RANGE);

					/*
					 * Firstly, find the greatest range bound that is less
					 * than or equal to the new lower bound.
					 */
					off1 = partition_bound_bsearch(key, boundinfo, lower, true,
												   &equal);

					/*
					 * off1 == -1 means that all existing bounds are greater
					 * than the new lower bound.  In that case and the case
					 * where no partition is defined between the bounds at
					 * off1 and off1 + 1, we have a "gap" in the range that
					 * could be occupied by the new partition.  We confirm if
					 * so by checking whether the new upper bound is confined
					 * within the gap.
					 */
					if (!equal && boundinfo->indexes[off1 + 1] < 0)
					{
						off2 = partition_bound_bsearch(key, boundinfo, upper,
													   true, &equal);

						/*
						 * If the new upper bound is returned to be equal to
						 * the bound at off2, the latter must be the upper
						 * bound of some partition with which the new
						 * partition clearly overlaps.
						 *
						 * Also, if bound at off2 is not same as the one
						 * returned for the new lower bound (IOW, off1 !=
						 * off2), then the new partition overlaps at least one
						 * partition.
						 */
						if (equal || off1 != off2)
						{
							overlap = true;

							/*
							 * The bound at off2 could be the lower bound of
							 * the partition with which the new partition
							 * overlaps.  In that case, use the upper bound
							 * (that is, the bound at off2 + 1) to get the
							 * index of that partition.
							 */
							if (boundinfo->indexes[off2] < 0)
								with = boundinfo->indexes[off2 + 1];
							else
								with = boundinfo->indexes[off2];
						}
					}
					else
					{
						/*
						 * Equal has been set to true and there is no "gap"
						 * between the bound at off1 and that at off1 + 1, so
						 * the new partition will overlap some partition. In
						 * the former case, the new lower bound is found to be
						 * equal to the bound at off1, which could only ever
						 * be true if the latter is the lower bound of some
						 * partition.  It's clear in such a case that the new
						 * partition overlaps that partition, whose index we
						 * get using its upper bound (that is, using the bound
						 * at off1 + 1).
						 */
						overlap = true;
						with = boundinfo->indexes[off1 + 1];
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
	Assert(HeapTupleIsValid(tuple));

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
get_qual_from_partbound(Relation rel, Relation parent, Node *bound)
{
	PartitionBoundSpec *spec = (PartitionBoundSpec *) bound;
	PartitionKey key = RelationGetPartitionKey(parent);
	List	   *my_qual = NIL;

	Assert(key != NULL);

	switch (key->strategy)
	{
		case PARTITION_STRATEGY_LIST:
			Assert(spec->strategy == PARTITION_STRATEGY_LIST);
			my_qual = get_qual_for_list(key, spec);
			break;

		case PARTITION_STRATEGY_RANGE:
			Assert(spec->strategy == PARTITION_STRATEGY_RANGE);
			my_qual = get_qual_for_range(key, spec);
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
 * We must allow for a case where physical attnos of a partition can be
 * different from the parent's.
 */
List *
map_partition_varattnos(List *expr, int target_varno,
						Relation partrel, Relation parent)
{
	AttrNumber *part_attnos;
	bool		found_whole_row;

	if (expr == NIL)
		return NIL;

	part_attnos = convert_tuples_by_name_map(RelationGetDescr(partrel),
											 RelationGetDescr(parent),
								 gettext_noop("could not convert row type"));
	expr = (List *) map_variable_attnos((Node *) expr,
										target_varno, 0,
										part_attnos,
										RelationGetDescr(parent)->natts,
										&found_whole_row);
	/* There can never be a whole-row reference here */
	if (found_whole_row)
		elog(ERROR, "unexpected whole-row reference found in partition key");

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
 * Append OIDs of rel's partitions to the list 'partoids' and for each OID,
 * append pointer rel to the list 'parents'.
 */
#define APPEND_REL_PARTITION_OIDS(rel, partoids, parents) \
	do\
	{\
		int		i;\
		for (i = 0; i < (rel)->rd_partdesc->nparts; i++)\
		{\
			(partoids) = lappend_oid((partoids), (rel)->rd_partdesc->oids[i]);\
			(parents) = lappend((parents), (rel));\
		}\
	} while(0)

/*
 * RelationGetPartitionDispatchInfo
 *		Returns information necessary to route tuples down a partition tree
 *
 * All the partitions will be locked with lockmode, unless it is NoLock.
 * A list of the OIDs of all the leaf partition of rel is returned in
 * *leaf_part_oids.
 */
PartitionDispatch *
RelationGetPartitionDispatchInfo(Relation rel, int lockmode,
								 int *num_parted, List **leaf_part_oids)
{
	PartitionDispatchData **pd;
	List	   *all_parts = NIL,
			   *all_parents = NIL,
			   *parted_rels,
			   *parted_rel_parents;
	ListCell   *lc1,
			   *lc2;
	int			i,
				k,
				offset;

	/*
	 * Lock partitions and make a list of the partitioned ones to prepare
	 * their PartitionDispatch objects below.
	 *
	 * Cannot use find_all_inheritors() here, because then the order of OIDs
	 * in parted_rels list would be unknown, which does not help, because we
	 * we assign indexes within individual PartitionDispatch in an order that
	 * is predetermined (determined by the order of OIDs in individual
	 * partition descriptors).
	 */
	*num_parted = 1;
	parted_rels = list_make1(rel);
	/* Root partitioned table has no parent, so NULL for parent */
	parted_rel_parents = list_make1(NULL);
	APPEND_REL_PARTITION_OIDS(rel, all_parts, all_parents);
	forboth(lc1, all_parts, lc2, all_parents)
	{
		Relation	partrel = heap_open(lfirst_oid(lc1), lockmode);
		Relation	parent = lfirst(lc2);
		PartitionDesc partdesc = RelationGetPartitionDesc(partrel);

		/*
		 * If this partition is a partitioned table, add its children to the
		 * end of the list, so that they are processed as well.
		 */
		if (partdesc)
		{
			(*num_parted)++;
			parted_rels = lappend(parted_rels, partrel);
			parted_rel_parents = lappend(parted_rel_parents, parent);
			APPEND_REL_PARTITION_OIDS(partrel, all_parts, all_parents);
		}
		else
			heap_close(partrel, NoLock);

		/*
		 * We keep the partitioned ones open until we're done using the
		 * information being collected here (for example, see
		 * ExecEndModifyTable).
		 */
	}

	/*
	 * We want to create two arrays - one for leaf partitions and another for
	 * partitioned tables (including the root table and internal partitions).
	 * While we only create the latter here, leaf partition array of suitable
	 * objects (such as, ResultRelInfo) is created by the caller using the
	 * list of OIDs we return.  Indexes into these arrays get assigned in a
	 * breadth-first manner, whereby partitions of any given level are placed
	 * consecutively in the respective arrays.
	 */
	pd = (PartitionDispatchData **) palloc(*num_parted *
										   sizeof(PartitionDispatchData *));
	*leaf_part_oids = NIL;
	i = k = offset = 0;
	forboth(lc1, parted_rels, lc2, parted_rel_parents)
	{
		Relation	partrel = lfirst(lc1);
		Relation	parent = lfirst(lc2);
		PartitionKey partkey = RelationGetPartitionKey(partrel);
		TupleDesc	tupdesc = RelationGetDescr(partrel);
		PartitionDesc partdesc = RelationGetPartitionDesc(partrel);
		int			j,
					m;

		pd[i] = (PartitionDispatch) palloc(sizeof(PartitionDispatchData));
		pd[i]->reldesc = partrel;
		pd[i]->key = partkey;
		pd[i]->keystate = NIL;
		pd[i]->partdesc = partdesc;
		if (parent != NULL)
		{
			/*
			 * For every partitioned table other than root, we must store a
			 * tuple table slot initialized with its tuple descriptor and a
			 * tuple conversion map to convert a tuple from its parent's
			 * rowtype to its own. That is to make sure that we are looking at
			 * the correct row using the correct tuple descriptor when
			 * computing its partition key for tuple routing.
			 */
			pd[i]->tupslot = MakeSingleTupleTableSlot(tupdesc);
			pd[i]->tupmap = convert_tuples_by_name(RelationGetDescr(parent),
												   tupdesc,
								 gettext_noop("could not convert row type"));
		}
		else
		{
			/* Not required for the root partitioned table */
			pd[i]->tupslot = NULL;
			pd[i]->tupmap = NULL;
		}
		pd[i]->indexes = (int *) palloc(partdesc->nparts * sizeof(int));

		/*
		 * Indexes corresponding to the internal partitions are multiplied by
		 * -1 to distinguish them from those of leaf partitions.  Encountering
		 * an index >= 0 means we found a leaf partition, which is immediately
		 * returned as the partition we are looking for.  A negative index
		 * means we found a partitioned table, whose PartitionDispatch object
		 * is located at the above index multiplied back by -1.  Using the
		 * PartitionDispatch object, search is continued further down the
		 * partition tree.
		 */
		m = 0;
		for (j = 0; j < partdesc->nparts; j++)
		{
			Oid			partrelid = partdesc->oids[j];

			if (get_rel_relkind(partrelid) != RELKIND_PARTITIONED_TABLE)
			{
				*leaf_part_oids = lappend_oid(*leaf_part_oids, partrelid);
				pd[i]->indexes[j] = k++;
			}
			else
			{
				/*
				 * offset denotes the number of partitioned tables of upper
				 * levels including those of the current level.  Any partition
				 * of this table must belong to the next level and hence will
				 * be placed after the last partitioned table of this level.
				 */
				pd[i]->indexes[j] = -(1 + offset + m);
				m++;
			}
		}
		i++;

		/*
		 * This counts the number of partitioned tables at upper levels
		 * including those of the current level.
		 */
		offset += m;
	}

	return pd;
}

/* Module-local functions */

/*
 * get_qual_for_list
 *
 * Returns a list of expressions to use as a list partition's constraint.
 */
static List *
get_qual_for_list(PartitionKey key, PartitionBoundSpec *spec)
{
	List	   *result;
	ArrayExpr  *arr;
	ScalarArrayOpExpr *opexpr;
	ListCell   *cell,
			   *prev,
			   *next;
	Node	   *keyCol;
	Oid			operoid;
	bool		need_relabel,
				list_has_null = false;
	NullTest   *nulltest1 = NULL,
			   *nulltest2 = NULL;

	/* Left operand is either a simple Var or arbitrary expression */
	if (key->partattrs[0] != 0)
		keyCol = (Node *) makeVar(1,
								  key->partattrs[0],
								  key->parttypid[0],
								  key->parttypmod[0],
								  key->parttypcoll[0],
								  0);
	else
		keyCol = (Node *) copyObject(linitial(key->partexprs));

	/*
	 * We must remove any NULL value in the list; we handle it separately
	 * below.
	 */
	prev = NULL;
	for (cell = list_head(spec->listdatums); cell; cell = next)
	{
		Const	   *val = (Const *) lfirst(cell);

		next = lnext(cell);

		if (val->constisnull)
		{
			list_has_null = true;
			spec->listdatums = list_delete_cell(spec->listdatums,
												cell, prev);
		}
		else
			prev = cell;
	}

	if (!list_has_null)
	{
		/*
		 * Gin up a col IS NOT NULL test that will be AND'd with other
		 * expressions
		 */
		nulltest1 = makeNode(NullTest);
		nulltest1->arg = (Expr *) keyCol;
		nulltest1->nulltesttype = IS_NOT_NULL;
		nulltest1->argisrow = false;
		nulltest1->location = -1;
	}
	else
	{
		/*
		 * Gin up a col IS NULL test that will be OR'd with other expressions
		 */
		nulltest2 = makeNode(NullTest);
		nulltest2->arg = (Expr *) keyCol;
		nulltest2->nulltesttype = IS_NULL;
		nulltest2->argisrow = false;
		nulltest2->location = -1;
	}

	/* Right operand is an ArrayExpr containing this partition's values */
	arr = makeNode(ArrayExpr);
	arr->array_typeid = !type_is_array(key->parttypid[0])
		? get_array_type(key->parttypid[0])
		: key->parttypid[0];
	arr->array_collid = key->parttypcoll[0];
	arr->element_typeid = key->parttypid[0];
	arr->elements = spec->listdatums;
	arr->multidims = false;
	arr->location = -1;

	/* Get the correct btree equality operator */
	operoid = get_partition_operator(key, 0, BTEqualStrategyNumber,
									 &need_relabel);
	if (need_relabel || key->partcollation[0] != key->parttypcoll[0])
		keyCol = (Node *) makeRelabelType((Expr *) keyCol,
										  key->partopcintype[0],
										  -1,
										  key->partcollation[0],
										  COERCE_EXPLICIT_CAST);

	/* Build leftop = ANY (rightop) */
	opexpr = makeNode(ScalarArrayOpExpr);
	opexpr->opno = operoid;
	opexpr->opfuncid = get_opcode(operoid);
	opexpr->useOr = true;
	opexpr->inputcollid = key->partcollation[0];
	opexpr->args = list_make2(keyCol, arr);
	opexpr->location = -1;

	if (nulltest1)
		result = list_make2(nulltest1, opexpr);
	else if (nulltest2)
	{
		Expr	   *or;

		or = makeBoolExpr(OR_EXPR, list_make2(nulltest2, opexpr), -1);
		result = list_make1(or);
	}
	else
		result = list_make1(opexpr);

	return result;
}

/*
 * get_qual_for_range
 *
 * Get a list of OpExpr's to use as a range partition's constraint.
 */
static List *
get_qual_for_range(PartitionKey key, PartitionBoundSpec *spec)
{
	List	   *result = NIL;
	ListCell   *cell1,
			   *cell2,
			   *partexprs_item;
	int			i;

	/*
	 * Iterate over columns of the key, emitting an OpExpr for each using the
	 * corresponding lower and upper datums as constant operands.
	 */
	i = 0;
	partexprs_item = list_head(key->partexprs);
	forboth(cell1, spec->lowerdatums, cell2, spec->upperdatums)
	{
		PartitionRangeDatum *ldatum = lfirst(cell1),
				   *udatum = lfirst(cell2);
		Node	   *keyCol;
		Const	   *lower_val = NULL,
				   *upper_val = NULL;
		EState	   *estate;
		MemoryContext oldcxt;
		Expr	   *test_expr;
		ExprState  *test_exprstate;
		Datum		test_result;
		bool		isNull;
		bool		need_relabel = false;
		Oid			operoid;
		NullTest   *nulltest;

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
			partexprs_item = lnext(partexprs_item);
		}

		/*
		 * Emit a IS NOT NULL expression for non-Var keys, because whereas
		 * simple attributes are covered by NOT NULL constraints, expression
		 * keys are still nullable which is not acceptable in case of range
		 * partitioning.
		 */
		if (!IsA(keyCol, Var))
		{
			nulltest = makeNode(NullTest);
			nulltest->arg = (Expr *) keyCol;
			nulltest->nulltesttype = IS_NOT_NULL;
			nulltest->argisrow = false;
			nulltest->location = -1;
			result = lappend(result, nulltest);
		}

		/*
		 * Stop at this column if either of lower or upper datum is infinite,
		 * but do emit an OpExpr for the non-infinite datum.
		 */
		if (!ldatum->infinite)
			lower_val = (Const *) ldatum->value;
		if (!udatum->infinite)
			upper_val = (Const *) udatum->value;

		/*
		 * If lower_val and upper_val are both finite and happen to be equal,
		 * emit only (keyCol = lower_val) for this column, because all rows in
		 * this partition could only ever contain this value (ie, lower_val)
		 * in the current partitioning column.  We must consider further
		 * columns because the above condition does not fully constrain the
		 * rows of this partition.
		 */
		if (lower_val && upper_val)
		{
			/* Get the correct btree equality operator for the test */
			operoid = get_partition_operator(key, i, BTEqualStrategyNumber,
											 &need_relabel);

			/* Create the test expression */
			estate = CreateExecutorState();
			oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
			test_expr = make_opclause(operoid,
									  BOOLOID,
									  false,
									  (Expr *) lower_val,
									  (Expr *) upper_val,
									  InvalidOid,
									  key->partcollation[i]);
			fix_opfuncids((Node *) test_expr);
			test_exprstate = ExecInitExpr(test_expr, NULL);
			test_result = ExecEvalExprSwitchContext(test_exprstate,
											  GetPerTupleExprContext(estate),
													&isNull);
			MemoryContextSwitchTo(oldcxt);
			FreeExecutorState(estate);

			if (DatumGetBool(test_result))
			{
				/* This can never be, but it's better to make sure */
				if (i == key->partnatts - 1)
					elog(ERROR, "invalid range bound specification");

				if (need_relabel || key->partcollation[i] != key->parttypcoll[i])
					keyCol = (Node *) makeRelabelType((Expr *) keyCol,
													  key->partopcintype[i],
													  -1,
													  key->partcollation[i],
													  COERCE_EXPLICIT_CAST);
				result = lappend(result,
								 make_opclause(operoid,
											   BOOLOID,
											   false,
											   (Expr *) keyCol,
											   (Expr *) lower_val,
											   InvalidOid,
											   key->partcollation[i]));

				/* Go over to consider the next column. */
				i++;
				continue;
			}
		}

		/*
		 * We can say here that lower_val != upper_val.  Emit expressions
		 * (keyCol >= lower_val) and (keyCol < upper_val), then stop.
		 */
		if (lower_val)
		{
			operoid = get_partition_operator(key, i,
											 BTGreaterEqualStrategyNumber,
											 &need_relabel);

			if (need_relabel || key->partcollation[i] != key->parttypcoll[i])
				keyCol = (Node *) makeRelabelType((Expr *) keyCol,
												  key->partopcintype[i],
												  -1,
												  key->partcollation[i],
												  COERCE_EXPLICIT_CAST);
			result = lappend(result,
							 make_opclause(operoid,
										   BOOLOID,
										   false,
										   (Expr *) keyCol,
										   (Expr *) lower_val,
										   InvalidOid,
										   key->partcollation[i]));
		}

		if (upper_val)
		{
			operoid = get_partition_operator(key, i,
											 BTLessStrategyNumber,
											 &need_relabel);

			if (need_relabel || key->partcollation[i] != key->parttypcoll[i])
				keyCol = (Node *) makeRelabelType((Expr *) keyCol,
												  key->partopcintype[i],
												  -1,
												  key->partcollation[i],
												  COERCE_EXPLICIT_CAST);

			result = lappend(result,
							 make_opclause(operoid,
										   BOOLOID,
										   false,
										   (Expr *) keyCol,
										   (Expr *) upper_val,
										   InvalidOid,
										   key->partcollation[i]));
		}

		/*
		 * We can stop at this column, because we would not have checked the
		 * next column when routing a given row into this partition.
		 */
		break;
	}

	return result;
}

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
	 * opreator family but with the operator class declared input type.  It is
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
		*need_relabel = true;
	}
	else
		*need_relabel = false;

	if (!OidIsValid(operoid))
		elog(ERROR, "could not find operator for partitioning");

	return operoid;
}

/*
 * generate_partition_qual
 *
 * Generate partition predicate from rel's partition bound expression
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
	Node	   *bound;
	List	   *my_qual = NIL,
			   *result = NIL;
	Relation	parent;

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
	bound = stringToNode(TextDatumGetCString(boundDatum));
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
	result = map_partition_varattnos(result, 1, rel, parent);

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
static void
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
		pd->keystate = (List *) ExecPrepareExpr((Expr *) pd->key->partexprs,
												estate);
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
						Oid *failed_at)
{
	PartitionDispatch parent;
	Datum		values[PARTITION_MAX_KEYS];
	bool		isnull[PARTITION_MAX_KEYS];
	int			cur_offset,
				cur_index;
	int			i,
				result;
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

		/* Quick exit */
		if (partdesc->nparts == 0)
		{
			*failed_at = RelationGetRelid(parent->reldesc);
			return -1;
		}

		if (myslot != NULL && map != NULL)
		{
			HeapTuple	tuple = ExecFetchSlotTuple(slot);

			ExecClearTuple(myslot);
			tuple = do_convert_tuple(tuple, map);
			ExecStoreTuple(tuple, myslot, InvalidBuffer, true);
			slot = myslot;
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

		if (key->strategy == PARTITION_STRATEGY_RANGE)
		{
			/* Disallow nulls in the range partition key of the tuple */
			for (i = 0; i < key->partnatts; i++)
				if (isnull[i])
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						errmsg("range partition key of row contains null")));
		}

		if (partdesc->boundinfo->has_null && isnull[0])
			/* Tuple maps to the null-accepting list partition */
			cur_index = partdesc->boundinfo->null_index;
		else
		{
			/* Else bsearch in partdesc->boundinfo */
			bool		equal = false;

			cur_offset = partition_bound_bsearch(key, partdesc->boundinfo,
												 values, false, &equal);
			switch (key->strategy)
			{
				case PARTITION_STRATEGY_LIST:
					if (cur_offset >= 0 && equal)
						cur_index = partdesc->boundinfo->indexes[cur_offset];
					else
						cur_index = -1;
					break;

				case PARTITION_STRATEGY_RANGE:

					/*
					 * Offset returned is such that the bound at offset is
					 * found to be less or equal with the tuple. So, the bound
					 * at offset+1 would be the upper bound.
					 */
					cur_index = partdesc->boundinfo->indexes[cur_offset + 1];
					break;

				default:
					elog(ERROR, "unexpected partition strategy: %d",
						 (int) key->strategy);
			}
		}

		/*
		 * cur_index < 0 means we failed to find a partition of this parent.
		 * cur_index >= 0 means we either found the leaf partition, or the
		 * next parent to find a partition of.
		 */
		if (cur_index < 0)
		{
			result = -1;
			*failed_at = RelationGetRelid(parent->reldesc);
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
	ListCell   *cell;
	int			i;

	bound = (PartitionRangeBound *) palloc0(sizeof(PartitionRangeBound));
	bound->index = index;
	bound->datums = (Datum *) palloc0(key->partnatts * sizeof(Datum));
	bound->content = (RangeDatumContent *) palloc0(key->partnatts *
												   sizeof(RangeDatumContent));
	bound->lower = lower;

	i = 0;
	foreach(cell, datums)
	{
		PartitionRangeDatum *datum = lfirst(cell);

		/* What's contained in this range datum? */
		bound->content[i] = !datum->infinite
			? RANGE_DATUM_FINITE
			: (lower ? RANGE_DATUM_NEG_INF
			   : RANGE_DATUM_POS_INF);

		if (bound->content[i] == RANGE_DATUM_FINITE)
		{
			Const	   *val = (Const *) datum->value;

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

	return partition_rbound_cmp(key, b1->datums, b1->content, b1->lower, b2);
}

/*
 * partition_rbound_cmp
 *
 * Return for two range bounds whether the 1st one (specified in datum1,
 * content1, and lower1) is <=, =, >= the bound specified in *b2
 */
static int32
partition_rbound_cmp(PartitionKey key,
					 Datum *datums1, RangeDatumContent *content1, bool lower1,
					 PartitionRangeBound *b2)
{
	int32		cmpval = 0;		/* placate compiler */
	int			i;
	Datum	   *datums2 = b2->datums;
	RangeDatumContent *content2 = b2->content;
	bool		lower2 = b2->lower;

	for (i = 0; i < key->partnatts; i++)
	{
		/*
		 * First, handle cases involving infinity, which don't require
		 * invoking the comparison proc.
		 */
		if (content1[i] != RANGE_DATUM_FINITE &&
			content2[i] != RANGE_DATUM_FINITE)

			/*
			 * Both are infinity, so they are equal unless one is negative
			 * infinity and other positive (or vice versa)
			 */
			return content1[i] == content2[i] ? 0
				: (content1[i] < content2[i] ? -1 : 1);
		else if (content1[i] != RANGE_DATUM_FINITE)
			return content1[i] == RANGE_DATUM_NEG_INF ? -1 : 1;
		else if (content2[i] != RANGE_DATUM_FINITE)
			return content2[i] == RANGE_DATUM_NEG_INF ? 1 : -1;

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
 * Return whether range bound (specified in rb_datums, rb_content, and
 * rb_lower) <=, =, >= partition key of tuple (tuple_datums)
 */
static int32
partition_rbound_datum_cmp(PartitionKey key,
						   Datum *rb_datums, RangeDatumContent *rb_content,
						   Datum *tuple_datums)
{
	int			i;
	int32		cmpval = -1;

	for (i = 0; i < key->partnatts; i++)
	{
		if (rb_content[i] != RANGE_DATUM_FINITE)
			return rb_content[i] == RANGE_DATUM_NEG_INF ? -1 : 1;

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
 * Return whether the bound at offset in boundinfo is <=, =, >= the argument
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
				RangeDatumContent *content = boundinfo->content[offset];

				if (probe_is_bound)
				{
					/*
					 * We need to pass whether the existing bound is a lower
					 * bound, so that two equal-valued lower and upper bounds
					 * are not regarded equal.
					 */
					bool		lower = boundinfo->indexes[offset] < 0;

					cmpval = partition_rbound_cmp(key,
												bound_datums, content, lower,
											  (PartitionRangeBound *) probe);
				}
				else
					cmpval = partition_rbound_datum_cmp(key,
														bound_datums, content,
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
 * bound in array boundinfo->datums which is less than or equal to *probe
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
