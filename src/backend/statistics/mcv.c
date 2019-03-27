/*-------------------------------------------------------------------------
 *
 * mcv.c
 *	  POSTGRES multivariate MCV lists
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/statistics/mcv.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_statistic_ext.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/*
 * Computes size of a serialized MCV item, depending on the number of
 * dimensions (columns) the statistic is defined on. The datum values are
 * stored in a separate array (deduplicated, to minimize the size), and
 * so the serialized items only store uint16 indexes into that array.
 *
 * Each serialized item store (in this order):
 *
 * - indexes to values	  (ndim * sizeof(uint16))
 * - null flags			  (ndim * sizeof(bool))
 * - frequency			  (sizeof(double))
 * - base_frequency		  (sizeof(double))
 *
 * So in total each MCV item requires this many bytes:
 *
 *	 ndim * (sizeof(uint16) + sizeof(bool)) + 2 * sizeof(double)
 */
#define ITEM_SIZE(ndims)	\
	((ndims) * (sizeof(uint16) + sizeof(bool)) + 2 * sizeof(double))

/*
 * Macros for convenient access to parts of a serialized MCV item.
 */
#define ITEM_INDEXES(item)			((uint16 *) (item))
#define ITEM_NULLS(item,ndims)		((bool *) (ITEM_INDEXES(item) + (ndims)))
#define ITEM_FREQUENCY(item,ndims)	((double *) (ITEM_NULLS(item, ndims) + (ndims)))
#define ITEM_BASE_FREQUENCY(item,ndims)	((double *) (ITEM_FREQUENCY(item, ndims) + 1))


static MultiSortSupport build_mss(VacAttrStats **stats, int numattrs);

static SortItem *build_distinct_groups(int numrows, SortItem *items,
					  MultiSortSupport mss, int *ndistinct);

static int count_distinct_groups(int numrows, SortItem *items,
					  MultiSortSupport mss);

/*
 * get_mincount_for_mcv_list
 * 		Determine the minimum number of times a value needs to appear in
 * 		the sample for it to be included in the MCV list.
 *
 * We want to keep only values that appear sufficiently often in the
 * sample that it is reasonable to extrapolate their sample frequencies to
 * the entire table.  We do this by placing an upper bound on the relative
 * standard error of the sample frequency, so that any estimates the
 * planner generates from the MCV statistics can be expected to be
 * reasonably accurate.
 *
 * Since we are sampling without replacement, the sample frequency of a
 * particular value is described by a hypergeometric distribution.  A
 * common rule of thumb when estimating errors in this situation is to
 * require at least 10 instances of the value in the sample, in which case
 * the distribution can be approximated by a normal distribution, and
 * standard error analysis techniques can be applied.  Given a sample size
 * of n, a population size of N, and a sample frequency of p=cnt/n, the
 * standard error of the proportion p is given by
 *		SE = sqrt(p*(1-p)/n) * sqrt((N-n)/(N-1))
 * where the second term is the finite population correction.  To get
 * reasonably accurate planner estimates, we impose an upper bound on the
 * relative standard error of 20% -- i.e., SE/p < 0.2.  This 20% relative
 * error bound is fairly arbitrary, but has been found empirically to work
 * well.  Rearranging this formula gives a lower bound on the number of
 * instances of the value seen:
 *		cnt > n*(N-n) / (N-n+0.04*n*(N-1))
 * This bound is at most 25, and approaches 0 as n approaches 0 or N. The
 * case where n approaches 0 cannot happen in practice, since the sample
 * size is at least 300.  The case where n approaches N corresponds to
 * sampling the whole the table, in which case it is reasonable to keep
 * the whole MCV list (have no lower bound), so it makes sense to apply
 * this formula for all inputs, even though the above derivation is
 * technically only valid when the right hand side is at least around 10.
 *
 * An alternative way to look at this formula is as follows -- assume that
 * the number of instances of the value seen scales up to the entire
 * table, so that the population count is K=N*cnt/n. Then the distribution
 * in the sample is a hypergeometric distribution parameterised by N, n
 * and K, and the bound above is mathematically equivalent to demanding
 * that the standard deviation of that distribution is less than 20% of
 * its mean.  Thus the relative errors in any planner estimates produced
 * from the MCV statistics are likely to be not too large.
 */
static double
get_mincount_for_mcv_list(int samplerows, double totalrows)
{
	double		n = samplerows;
	double		N = totalrows;
	double		numer,
				denom;

	numer = n * (N - n);
	denom = N - n + 0.04 * n * (N - 1);

	/* Guard against division by zero (possible if n = N = 1) */
	if (denom == 0.0)
		return 0.0;

	return numer / denom;
}

/*
 * Builds MCV list from the set of sampled rows.
 *
 * The algorithm is quite simple:
 *
 *	   (1) sort the data (default collation, '<' for the data type)
 *
 *	   (2) count distinct groups, decide how many to keep
 *
 *	   (3) build the MCV list using the threshold determined in (2)
 *
 *	   (4) remove rows represented by the MCV from the sample
 *
 */
MCVList *
statext_mcv_build(int numrows, HeapTuple *rows, Bitmapset *attrs,
				  VacAttrStats **stats, double totalrows)
{
	int			i,
				numattrs,
				ngroups,
				nitems;
	AttrNumber *attnums;
	double		mincount;
	SortItem   *items;
	SortItem   *groups;
	MCVList    *mcvlist = NULL;
	MultiSortSupport mss;

	attnums = build_attnums_array(attrs, &numattrs);

	/* comparator for all the columns */
	mss = build_mss(stats, numattrs);

	/* sort the rows */
	items = build_sorted_items(numrows, &nitems, rows, stats[0]->tupDesc,
							   mss, numattrs, attnums);

	if (!items)
		return NULL;

	/* transform the sorted rows into groups (sorted by frequency) */
	groups = build_distinct_groups(nitems, items, mss, &ngroups);

	/*
	 * Maximum number of MCV items to store, based on the attribute with the
	 * largest stats target (and the number of groups we have available).
	 */
	nitems = stats[0]->attr->attstattarget;
	for (i = 1; i < numattrs; i++)
	{
		if (stats[i]->attr->attstattarget > nitems)
			nitems = stats[i]->attr->attstattarget;
	}
	if (nitems > ngroups)
		nitems = ngroups;

	/*
	 * Decide how many items to keep in the MCV list. We can't use the same
	 * algorithm as per-column MCV lists, because that only considers the
	 * actual group frequency - but we're primarily interested in how the
	 * actual frequency differs from the base frequency (product of simple
	 * per-column frequencies, as if the columns were independent).
	 *
	 * Using the same algorithm might exclude items that are close to the
	 * "average" frequency of the sample. But that does not say whether the
	 * observed frequency is close to the base frequency or not. We also
	 * need to consider unexpectedly uncommon items (again, compared to the
	 * base frequency), and the single-column algorithm does not have to.
	 *
	 * We simply decide how many items to keep by computing minimum count
	 * using get_mincount_for_mcv_list() and then keep all items that seem
	 * to be more common than that.
	 */
	mincount = get_mincount_for_mcv_list(numrows, totalrows);

	/*
	 * Walk the groups until we find the first group with a count below
	 * the mincount threshold (the index of that group is the number of
	 * groups we want to keep).
	 */
	for (i = 0; i < nitems; i++)
	{
		if (groups[i].count < mincount)
		{
			nitems = i;
			break;
		}
	}

	/*
	 * At this point we know the number of items for the MCV list. There might
	 * be none (for uniform distribution with many groups), and in that case
	 * there will be no MCV list. Otherwise construct the MCV list.
	 */
	if (nitems > 0)
	{
		int	j;

		/*
		 * Allocate the MCV list structure, set the global parameters.
		 */
		mcvlist = (MCVList *) palloc0(sizeof(MCVList));

		mcvlist->magic = STATS_MCV_MAGIC;
		mcvlist->type = STATS_MCV_TYPE_BASIC;
		mcvlist->ndimensions = numattrs;
		mcvlist->nitems = nitems;

		/* store info about data type OIDs */
		for (i = 0; i < numattrs; i++)
			mcvlist->types[i] = stats[i]->attrtypid;

		/*
		 * Preallocate Datum/isnull arrays for all items.
		 *
		 * XXX Perhaps we might allocate this in a single chunk, to reduce
		 * the palloc overhead. We're the only ones dealing with the built
		 * MCV lists anyway. Not sure it's worth it, though, as we're not
		 * re-building stats very often.
		 */
		mcvlist->items = (MCVItem **) palloc(sizeof(MCVItem *) * nitems);

		for (i = 0; i < nitems; i++)
		{
			mcvlist->items[i] = (MCVItem *) palloc(sizeof(MCVItem));
			mcvlist->items[i]->values = (Datum *) palloc(sizeof(Datum) * numattrs);
			mcvlist->items[i]->isnull = (bool *) palloc(sizeof(bool) * numattrs);
		}

		/* Copy the first chunk of groups into the result. */
		for (i = 0; i < nitems; i++)
		{
			/* just pointer to the proper place in the list */
			MCVItem    *item = mcvlist->items[i];

			/* copy values for the group */
			memcpy(item->values, groups[i].values, sizeof(Datum) * numattrs);
			memcpy(item->isnull, groups[i].isnull, sizeof(bool) * numattrs);

			/* groups should be sorted by frequency in descending order */
			Assert((i == 0) || (groups[i - 1].count >= groups[i].count));

			/* group frequency */
			item->frequency = (double) groups[i].count / numrows;

			/* base frequency, if the attributes were independent */
			item->base_frequency = 1.0;
			for (j = 0; j < numattrs; j++)
			{
				int			count = 0;
				int			k;

				for (k = 0; k < ngroups; k++)
				{
					if (multi_sort_compare_dim(j, &groups[i], &groups[k], mss) == 0)
						count += groups[k].count;
				}

				item->base_frequency *= (double) count / numrows;
			}
		}
	}

	pfree(items);
	pfree(groups);

	return mcvlist;
}

/*
 * build_mss
 *	build MultiSortSupport for the attributes passed in attrs
 */
static MultiSortSupport
build_mss(VacAttrStats **stats, int numattrs)
{
	int			i;

	/* Sort by multiple columns (using array of SortSupport) */
	MultiSortSupport mss = multi_sort_init(numattrs);

	/* prepare the sort functions for all the attributes */
	for (i = 0; i < numattrs; i++)
	{
		VacAttrStats *colstat = stats[i];
		TypeCacheEntry *type;

		type = lookup_type_cache(colstat->attrtypid, TYPECACHE_LT_OPR);
		if (type->lt_opr == InvalidOid) /* shouldn't happen */
			elog(ERROR, "cache lookup failed for ordering operator for type %u",
				 colstat->attrtypid);

		multi_sort_add_dimension(mss, i, type->lt_opr, type->typcollation);
	}

	return mss;
}

/*
 * count_distinct_groups
 *	count distinct combinations of SortItems in the array
 *
 * The array is assumed to be sorted according to the MultiSortSupport.
 */
static int
count_distinct_groups(int numrows, SortItem *items, MultiSortSupport mss)
{
	int			i;
	int			ndistinct;

	ndistinct = 1;
	for (i = 1; i < numrows; i++)
	{
		/* make sure the array really is sorted */
		Assert(multi_sort_compare(&items[i], &items[i - 1], mss) >= 0);

		if (multi_sort_compare(&items[i], &items[i - 1], mss) != 0)
			ndistinct += 1;
	}

	return ndistinct;
}

/*
 * compare_sort_item_count
 *	comparator for sorting items by count (frequencies) in descending order
 */
static int
compare_sort_item_count(const void *a, const void *b)
{
	SortItem   *ia = (SortItem *) a;
	SortItem   *ib = (SortItem *) b;

	if (ia->count == ib->count)
		return 0;
	else if (ia->count > ib->count)
		return -1;

	return 1;
}

/*
 * build_distinct_groups
 *	build an array of SortItems for distinct groups and counts matching items
 *
 * The input array is assumed to be sorted
 */
static SortItem *
build_distinct_groups(int numrows, SortItem *items, MultiSortSupport mss,
					  int *ndistinct)
{
	int			i,
				j;
	int			ngroups = count_distinct_groups(numrows, items, mss);

	SortItem   *groups = (SortItem *) palloc(ngroups * sizeof(SortItem));

	j = 0;
	groups[0] = items[0];
	groups[0].count = 1;

	for (i = 1; i < numrows; i++)
	{
		/* Assume sorted in ascending order. */
		Assert(multi_sort_compare(&items[i], &items[i - 1], mss) >= 0);

		/* New distinct group detected. */
		if (multi_sort_compare(&items[i], &items[i - 1], mss) != 0)
		{
			groups[++j] = items[i];
			groups[j].count = 0;
		}

		groups[j].count++;
	}

	/* ensure we filled the expected number of distinct groups */
	Assert(j + 1 == ngroups);

	/* Sort the distinct groups by frequency (in descending order). */
	pg_qsort((void *) groups, ngroups, sizeof(SortItem),
			 compare_sort_item_count);

	*ndistinct = ngroups;
	return groups;
}


/*
 * statext_mcv_load
 *		Load the MCV list for the indicated pg_statistic_ext tuple
 */
MCVList *
statext_mcv_load(Oid mvoid)
{
	bool		isnull;
	Datum		mcvlist;
	HeapTuple	htup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(mvoid));

	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for statistics object %u", mvoid);

	mcvlist = SysCacheGetAttr(STATEXTOID, htup,
							  Anum_pg_statistic_ext_stxmcv, &isnull);

	ReleaseSysCache(htup);

	if (isnull)
		return NULL;

	return statext_mcv_deserialize(DatumGetByteaP(mcvlist));
}


/*
 * Serialize MCV list into a bytea value.
 *
 * The basic algorithm is simple:
 *
 * (1) perform deduplication (for each attribute separately)
 *	   (a) collect all (non-NULL) attribute values from all MCV items
 *	   (b) sort the data (using 'lt' from VacAttrStats)
 *	   (c) remove duplicate values from the array
 *
 * (2) serialize the arrays into a bytea value
 *
 * (3) process all MCV list items
 *	   (a) replace values with indexes into the arrays
 *
 * Each attribute has to be processed separately, as we may be mixing different
 * datatypes, with different sort operators, etc.
 *
 * We use uint16 values for the indexes in step (3), as the number of MCV items
 * is limited by the statistics target (which is capped to 10k at the moment).
 * We might increase this to 65k and still fit into uint16, so there's a bit of
 * slack. Furthermore, this limit is on the number of distinct values per column,
 * and we usually have few of those (and various combinations of them for the
 * those MCV list). So uint16 seems fine for now.
 *
 * We don't really expect the serialization to save as much space as for
 * histograms, as we are not doing any bucket splits (which is the source
 * of high redundancy in histograms).
 *
 * TODO: Consider packing boolean flags (NULL) for each item into a single char
 * (or a longer type) instead of using an array of bool items.
 */
bytea *
statext_mcv_serialize(MCVList * mcvlist, VacAttrStats **stats)
{
	int			i;
	int			dim;
	int			ndims = mcvlist->ndimensions;
	int			itemsize = ITEM_SIZE(ndims);

	SortSupport ssup;
	DimensionInfo *info;

	Size		total_length;

	/* allocate the item just once */
	char	   *item = palloc0(itemsize);

	/* serialized items (indexes into arrays, etc.) */
	bytea	   *output;
	char	   *data = NULL;

	/* values per dimension (and number of non-NULL values) */
	Datum	  **values = (Datum **) palloc0(sizeof(Datum *) * ndims);
	int		   *counts = (int *) palloc0(sizeof(int) * ndims);

	/*
	 * We'll include some rudimentary information about the attributes (type
	 * length, etc.), so that we don't have to look them up while
	 * deserializing the MCV list.
	 *
	 * XXX Maybe this is not a great idea? Or maybe we should actually copy
	 * more fields, e.g. typeid, which would allow us to display the MCV list
	 * using only the serialized representation (currently we have to fetch
	 * this info from the relation).
	 */
	info = (DimensionInfo *) palloc0(sizeof(DimensionInfo) * ndims);

	/* sort support data for all attributes included in the MCV list */
	ssup = (SortSupport) palloc0(sizeof(SortSupportData) * ndims);

	/* collect and deduplicate values for each dimension (attribute) */
	for (dim = 0; dim < ndims; dim++)
	{
		int			ndistinct;
		TypeCacheEntry *typentry;

		/*
		 * Lookup the LT operator (can't get it from stats extra_data, as we
		 * don't know how to interpret that - scalar vs. array etc.).
		 */
		typentry = lookup_type_cache(stats[dim]->attrtypid, TYPECACHE_LT_OPR);

		/* copy important info about the data type (length, by-value) */
		info[dim].typlen = stats[dim]->attrtype->typlen;
		info[dim].typbyval = stats[dim]->attrtype->typbyval;

		/* allocate space for values in the attribute and collect them */
		values[dim] = (Datum *) palloc0(sizeof(Datum) * mcvlist->nitems);

		for (i = 0; i < mcvlist->nitems; i++)
		{
			/* skip NULL values - we don't need to deduplicate those */
			if (mcvlist->items[i]->isnull[dim])
				continue;

			/* append the value at the end */
			values[dim][counts[dim]] = mcvlist->items[i]->values[dim];
			counts[dim] += 1;
		}

		/* if there are just NULL values in this dimension, we're done */
		if (counts[dim] == 0)
			continue;

		/* sort and deduplicate the data */
		ssup[dim].ssup_cxt = CurrentMemoryContext;
		ssup[dim].ssup_collation = DEFAULT_COLLATION_OID;
		ssup[dim].ssup_nulls_first = false;

		PrepareSortSupportFromOrderingOp(typentry->lt_opr, &ssup[dim]);

		qsort_arg(values[dim], counts[dim], sizeof(Datum),
				  compare_scalars_simple, &ssup[dim]);

		/*
		 * Walk through the array and eliminate duplicate values, but keep the
		 * ordering (so that we can do bsearch later). We know there's at
		 * least one item as (counts[dim] != 0), so we can skip the first
		 * element.
		 */
		ndistinct = 1;			/* number of distinct values */
		for (i = 1; i < counts[dim]; i++)
		{
			/* expect sorted array */
			Assert(compare_datums_simple(values[dim][i - 1], values[dim][i], &ssup[dim]) <= 0);

			/* if the value is the same as the previous one, we can skip it */
			if (!compare_datums_simple(values[dim][i - 1], values[dim][i], &ssup[dim]))
				continue;

			values[dim][ndistinct] = values[dim][i];
			ndistinct += 1;
		}

		/* we must not exceed PG_UINT16_MAX, as we use uint16 indexes */
		Assert(ndistinct <= PG_UINT16_MAX);

		/*
		 * Store additional info about the attribute - number of deduplicated
		 * values, and also size of the serialized data. For fixed-length data
		 * types this is trivial to compute, for varwidth types we need to
		 * actually walk the array and sum the sizes.
		 */
		info[dim].nvalues = ndistinct;

		if (info[dim].typlen > 0)	/* fixed-length data types */
			info[dim].nbytes = info[dim].nvalues * info[dim].typlen;
		else if (info[dim].typlen == -1)	/* varlena */
		{
			info[dim].nbytes = 0;
			for (i = 0; i < info[dim].nvalues; i++)
			{
				values[dim][i] = PointerGetDatum(PG_DETOAST_DATUM(values[dim][i]));
				info[dim].nbytes += VARSIZE_ANY(values[dim][i]);
			}
		}
		else if (info[dim].typlen == -2)	/* cstring */
		{
			info[dim].nbytes = 0;
			for (i = 0; i < info[dim].nvalues; i++)
			{
				/* c-strings include terminator, so +1 byte */
				values[dim][i] = PointerGetDatum(PG_DETOAST_DATUM(values[dim][i]));
				info[dim].nbytes += strlen(DatumGetCString(values[dim][i])) + 1;
			}
		}

		/* we know (count>0) so there must be some data */
		Assert(info[dim].nbytes > 0);
	}

	/*
	 * Now we can finally compute how much space we'll actually need for the
	 * whole serialized MCV list, as it contains these fields:
	 *
	 * - length (4B) for varlena - magic (4B) - type (4B) - ndimensions (4B) -
	 * nitems (4B) - info (ndim * sizeof(DimensionInfo) - arrays of values for
	 * each dimension - serialized items (nitems * itemsize)
	 *
	 * So the 'header' size is 20B + ndim * sizeof(DimensionInfo) and then we
	 * will place all the data (values + indexes). We'll however use offsetof
	 * and sizeof to compute sizes of the structs.
	 */
	total_length = (sizeof(int32) + offsetof(MCVList, items)
					+ (ndims * sizeof(DimensionInfo))
					+ mcvlist->nitems * itemsize);

	/* add space for the arrays of deduplicated values */
	for (i = 0; i < ndims; i++)
		total_length += info[i].nbytes;

	/* allocate space for the serialized MCV list, set header fields */
	output = (bytea *) palloc0(total_length);
	SET_VARSIZE(output, total_length);

	/* 'data' points to the current position in the output buffer */
	data = VARDATA(output);

	/* MCV list header (number of items, ...) */
	memcpy(data, mcvlist, offsetof(MCVList, items));
	data += offsetof(MCVList, items);

	/* information about the attributes */
	memcpy(data, info, sizeof(DimensionInfo) * ndims);
	data += sizeof(DimensionInfo) * ndims;

	/* Copy the deduplicated values for all attributes to the output. */
	for (dim = 0; dim < ndims; dim++)
	{
#ifdef USE_ASSERT_CHECKING
		/* remember the starting point for Asserts later */
		char	   *tmp = data;
#endif
		for (i = 0; i < info[dim].nvalues; i++)
		{
			Datum		v = values[dim][i];

			if (info[dim].typbyval) /* passed by value */
			{
				memcpy(data, &v, info[dim].typlen);
				data += info[dim].typlen;
			}
			else if (info[dim].typlen > 0)	/* pased by reference */
			{
				memcpy(data, DatumGetPointer(v), info[dim].typlen);
				data += info[dim].typlen;
			}
			else if (info[dim].typlen == -1)	/* varlena */
			{
				int	len = VARSIZE_ANY(v);

				memcpy(data, DatumGetPointer(v), len);
				data += len;
			}
			else if (info[dim].typlen == -2)	/* cstring */
			{
				Size	len = strlen(DatumGetCString(v)) + 1;  /* terminator */

				memcpy(data, DatumGetCString(v), len );
				data += len;
			}

			/* no underflows or overflows */
			Assert((data > tmp) && ((data - tmp) <= info[dim].nbytes));
		}

		/*
		 * check we got exactly the amount of data we expected for this
		 * dimension
		 */
		Assert((data - tmp) == info[dim].nbytes);
	}

	/* Serialize the items, with uint16 indexes instead of the values. */
	for (i = 0; i < mcvlist->nitems; i++)
	{
		MCVItem    *mcvitem = mcvlist->items[i];

		/* don't write beyond the allocated space */
		Assert(data <= (char *) output + total_length - itemsize);

		/* reset the item (we only allocate it once and reuse it) */
		memset(item, 0, itemsize);

		for (dim = 0; dim < ndims; dim++)
		{
			Datum	   *value;

			/* do the lookup only for non-NULL values */
			if (mcvlist->items[i]->isnull[dim])
				continue;

			value = (Datum *) bsearch_arg(&mcvitem->values[dim], values[dim],
									  info[dim].nvalues, sizeof(Datum),
									  compare_scalars_simple, &ssup[dim]);

			Assert(value != NULL);	/* serialization or deduplication error */

			/* compute index within the array */
			ITEM_INDEXES(item)[dim] = (uint16) (value - values[dim]);

			/* check the index is within expected bounds */
			Assert(ITEM_INDEXES(item)[dim] >= 0);
			Assert(ITEM_INDEXES(item)[dim] < info[dim].nvalues);
		}

		/* copy NULL and frequency flags into the item */
		memcpy(ITEM_NULLS(item, ndims), mcvitem->isnull, sizeof(bool) * ndims);
		memcpy(ITEM_FREQUENCY(item, ndims), &mcvitem->frequency, sizeof(double));
		memcpy(ITEM_BASE_FREQUENCY(item, ndims), &mcvitem->base_frequency, sizeof(double));

		/* copy the serialized item into the array */
		memcpy(data, item, itemsize);

		data += itemsize;
	}

	/* at this point we expect to match the total_length exactly */
	Assert((data - (char *) output) == total_length);

	pfree(item);
	pfree(values);
	pfree(counts);

	return output;
}

/*
 * Reads serialized MCV list into MCVList structure.
 *
 * Unlike with histograms, we deserialize the MCV list fully (i.e. we don't
 * keep the deduplicated arrays and pointers into them), as we don't expect
 * there to be a lot of duplicate values. But perhaps that's not true and we
 * should keep the MCV in serialized form too.
 *
 * XXX See how much memory we could save by keeping the deduplicated version
 * (both for typical and corner cases with few distinct values but many items).
 */
MCVList *
statext_mcv_deserialize(bytea *data)
{
	int			dim,
				i;
	Size		expected_size;
	MCVList    *mcvlist;
	char	   *tmp;

	int			ndims,
				nitems,
				itemsize;
	DimensionInfo *info = NULL;
	Datum	  **values = NULL;

	/* local allocation buffer (used only for deserialization) */
	int			bufflen;
	char	   *buff;
	char	   *ptr;

	/* buffer used for the result */
	int			rbufflen;
	char	   *rbuff;
	char	   *rptr;

	if (data == NULL)
		return NULL;

	/*
	 * We can't possibly deserialize a MCV list if there's not even a complete
	 * header.
	 */
	if (VARSIZE_ANY_EXHDR(data) < offsetof(MCVList, items))
		elog(ERROR, "invalid MCV size %ld (expected at least %zu)",
			 VARSIZE_ANY_EXHDR(data), offsetof(MCVList, items));

	/* read the MCV list header */
	mcvlist = (MCVList *) palloc0(sizeof(MCVList));

	/* initialize pointer to the data part (skip the varlena header) */
	tmp = VARDATA_ANY(data);

	/* get the header and perform further sanity checks */
	memcpy(mcvlist, tmp, offsetof(MCVList, items));
	tmp += offsetof(MCVList, items);

	if (mcvlist->magic != STATS_MCV_MAGIC)
		elog(ERROR, "invalid MCV magic %u (expected %u)",
			 mcvlist->magic, STATS_MCV_MAGIC);

	if (mcvlist->type != STATS_MCV_TYPE_BASIC)
		elog(ERROR, "invalid MCV type %u (expected %u)",
			 mcvlist->type, STATS_MCV_TYPE_BASIC);

	if (mcvlist->ndimensions == 0)
		elog(ERROR, "invalid zero-length dimension array in MCVList");
	else if ((mcvlist->ndimensions > STATS_MAX_DIMENSIONS) ||
			 (mcvlist->ndimensions < 0))
		elog(ERROR, "invalid length (%d) dimension array in MCVList",
			 mcvlist->ndimensions);

	if (mcvlist->nitems == 0)
		elog(ERROR, "invalid zero-length item array in MCVList");
	else if (mcvlist->nitems > STATS_MCVLIST_MAX_ITEMS)
		elog(ERROR, "invalid length (%u) item array in MCVList",
			 mcvlist->nitems);

	nitems = mcvlist->nitems;
	ndims = mcvlist->ndimensions;
	itemsize = ITEM_SIZE(ndims);

	/*
	 * Check amount of data including DimensionInfo for all dimensions and
	 * also the serialized items (including uint16 indexes). Also, walk
	 * through the dimension information and add it to the sum.
	 */
	expected_size = offsetof(MCVList, items) +
		ndims * sizeof(DimensionInfo) +
		(nitems * itemsize);

	/*
	 * Check that we have at least the dimension and info records, along with
	 * the items. We don't know the size of the serialized values yet. We need
	 * to do this check first, before accessing the dimension info.
	 */
	if (VARSIZE_ANY_EXHDR(data) < expected_size)
		elog(ERROR, "invalid MCV size %ld (expected %zu)",
			 VARSIZE_ANY_EXHDR(data), expected_size);

	/* Now it's safe to access the dimension info. */
	info = (DimensionInfo *) (tmp);
	tmp += ndims * sizeof(DimensionInfo);

	/* account for the value arrays */
	for (dim = 0; dim < ndims; dim++)
	{
		/*
		 * XXX I wonder if we can/should rely on asserts here. Maybe those
		 * checks should be done every time?
		 */
		Assert(info[dim].nvalues >= 0);
		Assert(info[dim].nbytes >= 0);

		expected_size += info[dim].nbytes;
	}

	/*
	 * Now we know the total expected MCV size, including all the pieces
	 * (header, dimension info. items and deduplicated data). So do the final
	 * check on size.
	 */
	if (VARSIZE_ANY_EXHDR(data) != expected_size)
		elog(ERROR, "invalid MCV size %ld (expected %zu)",
			 VARSIZE_ANY_EXHDR(data), expected_size);

	/*
	 * Allocate one large chunk of memory for the intermediate data, needed
	 * only for deserializing the MCV list (and allocate densely to minimize
	 * the palloc overhead).
	 *
	 * Let's see how much space we'll actually need, and also include space
	 * for the array with pointers.
	 *
	 * We need an array of Datum pointers values for each dimension, so that
	 * we can easily translate the uint16 indexes. We also need a top-level
	 * array of pointers to those per-dimension arrays.
	 *
	 * For byval types with size matching sizeof(Datum) we can reuse the
	 * serialized array directly.
	 */
	bufflen = sizeof(Datum **) * ndims; /* space for top-level pointers */

	for (dim = 0; dim < ndims; dim++)
	{
		/* for full-size byval types, we reuse the serialized value */
		if (!(info[dim].typbyval && info[dim].typlen == sizeof(Datum)))
			bufflen += (sizeof(Datum) * info[dim].nvalues);
	}

	buff = palloc0(bufflen);
	ptr = buff;

	values = (Datum **) buff;
	ptr += (sizeof(Datum *) * ndims);

	/*
	 * XXX This uses pointers to the original data array (the types not passed
	 * by value), so when someone frees the memory, e.g. by doing something
	 * like this:
	 *
	 *	  bytea * data = ... fetch the data from catalog ...
	 *
	 *	  MCVList mcvlist = deserialize_mcv_list(data);
	 *
	 *	  pfree(data);
	 *
	 * then 'mcvlist' references the freed memory. Should copy the pieces.
	 */
	for (dim = 0; dim < ndims; dim++)
	{
#ifdef USE_ASSERT_CHECKING
		/* remember where data for this dimension starts */
		char	   *start = tmp;
#endif
		if (info[dim].typbyval)
		{
			/* passed by value / size matches Datum - just reuse the array */
			if (info[dim].typlen == sizeof(Datum))
			{
				values[dim] = (Datum *) tmp;
				tmp += info[dim].nbytes;

				/* no overflow of input array */
				Assert(tmp <= start + info[dim].nbytes);
			}
			else
			{
				values[dim] = (Datum *) ptr;
				ptr += (sizeof(Datum) * info[dim].nvalues);

				for (i = 0; i < info[dim].nvalues; i++)
				{
					/* just point into the array */
					memcpy(&values[dim][i], tmp, info[dim].typlen);
					tmp += info[dim].typlen;

					/* no overflow of input array */
					Assert(tmp <= start + info[dim].nbytes);
				}
			}
		}
		else
		{
			/* all the other types need a chunk of the buffer */
			values[dim] = (Datum *) ptr;
			ptr += (sizeof(Datum) * info[dim].nvalues);

			/* passed by reference, but fixed length (name, tid, ...) */
			if (info[dim].typlen > 0)
			{
				for (i = 0; i < info[dim].nvalues; i++)
				{
					/* just point into the array */
					values[dim][i] = PointerGetDatum(tmp);
					tmp += info[dim].typlen;

					/* no overflow of input array */
					Assert(tmp <= start + info[dim].nbytes);
				}
			}
			else if (info[dim].typlen == -1)
			{
				/* varlena */
				for (i = 0; i < info[dim].nvalues; i++)
				{
					/* just point into the array */
					values[dim][i] = PointerGetDatum(tmp);
					tmp += VARSIZE_ANY(tmp);

					/* no overflow of input array */
					Assert(tmp <= start + info[dim].nbytes);
				}
			}
			else if (info[dim].typlen == -2)
			{
				/* cstring */
				for (i = 0; i < info[dim].nvalues; i++)
				{
					/* just point into the array */
					values[dim][i] = PointerGetDatum(tmp);
					tmp += (strlen(tmp) + 1);	/* don't forget the \0 */

					/* no overflow of input array */
					Assert(tmp <= start + info[dim].nbytes);
				}
			}
		}

		/* check we consumed the serialized data for this dimension exactly */
		Assert((tmp - start) == info[dim].nbytes);
	}

	/* we should have exhausted the buffer exactly */
	Assert((ptr - buff) == bufflen);

	/* allocate space for all the MCV items in a single piece */
	rbufflen = (sizeof(MCVItem *) + sizeof(MCVItem) +
				sizeof(Datum) * ndims + sizeof(bool) * ndims) * nitems;

	rbuff = palloc0(rbufflen);
	rptr = rbuff;

	mcvlist->items = (MCVItem * *) rbuff;
	rptr += (sizeof(MCVItem *) * nitems);

	/* deserialize the MCV items and translate the indexes to Datums */
	for (i = 0; i < nitems; i++)
	{
		uint16	   *indexes = NULL;
		MCVItem    *item = (MCVItem *) rptr;

		rptr += (sizeof(MCVItem));

		item->values = (Datum *) rptr;
		rptr += (sizeof(Datum) * ndims);

		item->isnull = (bool *) rptr;
		rptr += (sizeof(bool) * ndims);

		/* just point to the right place */
		indexes = ITEM_INDEXES(tmp);

		memcpy(item->isnull, ITEM_NULLS(tmp, ndims), sizeof(bool) * ndims);
		memcpy(&item->frequency, ITEM_FREQUENCY(tmp, ndims), sizeof(double));
		memcpy(&item->base_frequency, ITEM_BASE_FREQUENCY(tmp, ndims), sizeof(double));

		/* translate the values */
		for (dim = 0; dim < ndims; dim++)
			if (!item->isnull[dim])
				item->values[dim] = values[dim][indexes[dim]];

		mcvlist->items[i] = item;

		tmp += ITEM_SIZE(ndims);

		/* check we're not overflowing the input */
		Assert(tmp <= (char *) data + VARSIZE_ANY(data));
	}

	/* check that we processed all the data */
	Assert(tmp == (char *) data + VARSIZE_ANY(data));

	/* release the temporary buffer */
	pfree(buff);

	return mcvlist;
}

/*
 * SRF with details about buckets of a histogram:
 *
 * - item ID (0...nitems)
 * - values (string array)
 * - nulls only (boolean array)
 * - frequency (double precision)
 * - base_frequency (double precision)
 *
 * The input is the OID of the statistics, and there are no rows returned if
 * the statistics contains no histogram.
 */
Datum
pg_stats_ext_mcvlist_items(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int			call_cntr;
	int			max_calls;
	AttInMetadata *attinmeta;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		MCVList    *mcvlist;
		TupleDesc	tupdesc;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		mcvlist = statext_mcv_deserialize(PG_GETARG_BYTEA_P(0));

		funcctx->user_fctx = mcvlist;

		/* total number of tuples to be returned */
		funcctx->max_calls = 0;
		if (funcctx->user_fctx != NULL)
			funcctx->max_calls = mcvlist->nitems;

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));

		/*
		 * generate attribute metadata needed later to produce tuples from raw
		 * C strings
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	attinmeta = funcctx->attinmeta;

	if (call_cntr < max_calls)	/* do when there is more left to send */
	{
		char	  **values;
		HeapTuple	tuple;
		Datum		result;

		StringInfoData	itemValues;
		StringInfoData	itemNulls;

		int			i;

		Oid		   *outfuncs;
		FmgrInfo   *fmgrinfo;

		MCVList    *mcvlist;
		MCVItem    *item;

		mcvlist = (MCVList *) funcctx->user_fctx;

		Assert(call_cntr < mcvlist->nitems);

		item = mcvlist->items[call_cntr];

		/*
		 * Prepare a values array for building the returned tuple. This should
		 * be an array of C strings which will be processed later by the type
		 * input functions.
		 */
		values = (char **) palloc0(5 * sizeof(char *));

		values[0] = (char *) palloc(64 * sizeof(char));	/* item index */
		values[3] = (char *) palloc(64 * sizeof(char));	/* frequency */
		values[4] = (char *) palloc(64 * sizeof(char));	/* base frequency */

		outfuncs = (Oid *) palloc0(sizeof(Oid) * mcvlist->ndimensions);
		fmgrinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * mcvlist->ndimensions);

		for (i = 0; i < mcvlist->ndimensions; i++)
		{
			bool		isvarlena;

			getTypeOutputInfo(mcvlist->types[i], &outfuncs[i], &isvarlena);

			fmgr_info(outfuncs[i], &fmgrinfo[i]);
		}

		/* build the arrays of values / nulls */
		initStringInfo(&itemValues);
		initStringInfo(&itemNulls);

		appendStringInfoChar(&itemValues, '{');
		appendStringInfoChar(&itemNulls, '{');

		for (i = 0; i < mcvlist->ndimensions; i++)
		{
			Datum		val,
						valout;

			if (i > 0)
			{
				appendStringInfoString(&itemValues, ", ");
				appendStringInfoString(&itemNulls, ", ");
			}

			if (item->isnull[i])
				valout = CStringGetDatum("NULL");
			else
			{
				val = item->values[i];
				valout = FunctionCall1(&fmgrinfo[i], val);
			}

			appendStringInfoString(&itemValues, DatumGetCString(valout));
			appendStringInfoString(&itemNulls, item->isnull[i] ? "t" : "f");
		}

		appendStringInfoChar(&itemValues, '}');
		appendStringInfoChar(&itemNulls, '}');

		snprintf(values[0], 64, "%d", call_cntr);
		snprintf(values[3], 64, "%f", item->frequency);
		snprintf(values[4], 64, "%f", item->base_frequency);

		values[1] = itemValues.data;
		values[2] = itemNulls.data;

		/* build a tuple */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* make the tuple into a datum */
		result = HeapTupleGetDatum(tuple);

		/* clean up (this is not really necessary) */
		pfree(itemValues.data);
		pfree(itemNulls.data);

		pfree(values[0]);
		pfree(values[3]);
		pfree(values[4]);
		pfree(values);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else						/* do when there is no more left */
	{
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * pg_mcv_list_in		- input routine for type pg_mcv_list.
 *
 * pg_mcv_list is real enough to be a table column, but it has no operations
 * of its own, and disallows input too
 */
Datum
pg_mcv_list_in(PG_FUNCTION_ARGS)
{
	/*
	 * pg_mcv_list stores the data in binary form and parsing text input is
	 * not needed, so disallow this.
	 */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_mcv_list")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}


/*
 * pg_mcv_list_out		- output routine for type PG_MCV_LIST.
 *
 * MCV lists are serialized into a bytea value, so we simply call byteaout()
 * to serialize the value into text. But it'd be nice to serialize that into
 * a meaningful representation (e.g. for inspection by people).
 *
 * XXX This should probably return something meaningful, similar to what
 * pg_dependencies_out does. Not sure how to deal with the deduplicated
 * values, though - do we want to expand that or not?
 */
Datum
pg_mcv_list_out(PG_FUNCTION_ARGS)
{
	return byteaout(fcinfo);
}

/*
 * pg_mcv_list_recv		- binary input routine for type pg_mcv_list.
 */
Datum
pg_mcv_list_recv(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_mcv_list")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * pg_mcv_list_send		- binary output routine for type pg_mcv_list.
 *
 * MCV lists are serialized in a bytea value (although the type is named
 * differently), so let's just send that.
 */
Datum
pg_mcv_list_send(PG_FUNCTION_ARGS)
{
	return byteasend(fcinfo);
}

/*
 * mcv_get_match_bitmap
 *	Evaluate clauses using the MCV list, and update the match bitmap.
 *
 * A match bitmap keeps match/mismatch status for each MCV item, and we
 * update it based on additional clauses. We also use it to skip items
 * that can't possibly match (e.g. item marked as "mismatch" can't change
 * to "match" when evaluating AND clause list).
 *
 * The function also returns a flag indicating whether there was an
 * equality condition for all attributes, the minimum frequency in the MCV
 * list, and a total MCV frequency (sum of frequencies for all items).
 *
 * XXX Currently the match bitmap uses a bool for each MCV item, which is
 * somewhat wasteful as we could do with just a single bit, thus reducing
 * the size to ~1/8. It would also allow us to combine bitmaps simply using
 * & and |, which should be faster than min/max. The bitmaps are fairly
 * small, though (thanks to the cap on the MCV list size).
 */
static bool *
mcv_get_match_bitmap(PlannerInfo *root, List *clauses,
					  Bitmapset *keys, MCVList * mcvlist, bool is_or)
{
	int			i;
	ListCell   *l;
	bool	   *matches;

	/* The bitmap may be partially built. */
	Assert(clauses != NIL);
	Assert(list_length(clauses) >= 1);
	Assert(mcvlist != NULL);
	Assert(mcvlist->nitems > 0);
	Assert(mcvlist->nitems <= STATS_MCVLIST_MAX_ITEMS);

	matches = palloc(sizeof(bool) * mcvlist->nitems);
	memset(matches, (is_or) ? false : true,
		   sizeof(bool) * mcvlist->nitems);

	/*
	 * Loop through the list of clauses, and for each of them evaluate all the
	 * MCV items not yet eliminated by the preceding clauses.
	 */
	foreach(l, clauses)
	{
		Node	   *clause = (Node *) lfirst(l);

		/* if it's a RestrictInfo, then extract the clause */
		if (IsA(clause, RestrictInfo))
			clause = (Node *) ((RestrictInfo *) clause)->clause;

		/*
		 * Handle the various types of clauses - OpClause, NullTest and
		 * AND/OR/NOT
		 */
		if (is_opclause(clause))
		{
			OpExpr	   *expr = (OpExpr *) clause;
			bool		varonleft = true;
			bool		ok;
			FmgrInfo	opproc;

			/* get procedure computing operator selectivity */
			RegProcedure oprrest = get_oprrest(expr->opno);

			fmgr_info(get_opcode(expr->opno), &opproc);

			ok = (NumRelids(clause) == 1) &&
				(is_pseudo_constant_clause(lsecond(expr->args)) ||
				 (varonleft = false,
				  is_pseudo_constant_clause(linitial(expr->args))));

			if (ok)
			{
				TypeCacheEntry *typecache;
				FmgrInfo	gtproc;
				Var		   *var;
				Const	   *cst;
				bool		isgt;
				int			idx;

				/* extract the var and const from the expression */
				var = (varonleft) ? linitial(expr->args) : lsecond(expr->args);
				cst = (varonleft) ? lsecond(expr->args) : linitial(expr->args);
				isgt = (!varonleft);

				/* strip binary-compatible relabeling */
				if (IsA(var, RelabelType))
					var = (Var *) ((RelabelType *) var)->arg;

				/* match the attribute to a dimension of the statistic */
				idx = bms_member_index(keys, var->varattno);

				/* get information about the >= procedure */
				typecache = lookup_type_cache(var->vartype, TYPECACHE_GT_OPR);
				fmgr_info(get_opcode(typecache->gt_opr), &gtproc);

				/*
				 * Walk through the MCV items and evaluate the current clause.
				 * We can skip items that were already ruled out, and
				 * terminate if there are no remaining MCV items that might
				 * possibly match.
				 */
				for (i = 0; i < mcvlist->nitems; i++)
				{
					bool		mismatch = false;
					MCVItem    *item = mcvlist->items[i];

					/*
					 * For AND-lists, we can also mark NULL items as 'no
					 * match' (and then skip them). For OR-lists this is not
					 * possible.
					 */
					if ((!is_or) && item->isnull[idx])
						matches[i] = false;

					/* skip MCV items that were already ruled out */
					if ((!is_or) && (matches[i] == false))
						continue;
					else if (is_or && (matches[i] == true))
						continue;

					switch (oprrest)
					{
						case F_EQSEL:
						case F_NEQSEL:

							/*
							 * We don't care about isgt in equality, because
							 * it does not matter whether it's (var op const)
							 * or (const op var).
							 */
							mismatch = !DatumGetBool(FunctionCall2Coll(&opproc,
																	   DEFAULT_COLLATION_OID,
																	   cst->constvalue,
																	   item->values[idx]));

							break;

						case F_SCALARLTSEL: /* column < constant */
						case F_SCALARLESEL: /* column <= constant */
						case F_SCALARGTSEL: /* column > constant */
						case F_SCALARGESEL: /* column >= constant */

							/*
							 * First check whether the constant is below the
							 * lower boundary (in that case we can skip the
							 * bucket, because there's no overlap).
							 */
							if (isgt)
								mismatch = !DatumGetBool(FunctionCall2Coll(&opproc,
																		   DEFAULT_COLLATION_OID,
																		   cst->constvalue,
																		   item->values[idx]));
							else
								mismatch = !DatumGetBool(FunctionCall2Coll(&opproc,
																		   DEFAULT_COLLATION_OID,
																		   item->values[idx],
																		   cst->constvalue));

							break;
					}

					/*
					 * XXX The conditions on matches[i] are not needed, as we
					 * skip MCV items that can't become true/false, depending
					 * on the current flag. See beginning of the loop over MCV
					 * items.
					 */

					if ((is_or) && (!mismatch))
					{
						/* OR - was not a match before, matches now */
						matches[i] = true;
						continue;
					}
					else if ((!is_or) && mismatch)
					{
						/* AND - was a match before, does not match anymore */
						matches[i] = false;
						continue;
					}

				}
			}
		}
		else if (IsA(clause, NullTest))
		{
			NullTest   *expr = (NullTest *) clause;
			Var		   *var = (Var *) (expr->arg);

			/* match the attribute to a dimension of the statistic */
			int			idx = bms_member_index(keys, var->varattno);

			/*
			 * Walk through the MCV items and evaluate the current clause. We
			 * can skip items that were already ruled out, and terminate if
			 * there are no remaining MCV items that might possibly match.
			 */
			for (i = 0; i < mcvlist->nitems; i++)
			{
				bool		match = false;	/* assume mismatch */
				MCVItem    *item = mcvlist->items[i];

				/* if the clause mismatches the MCV item, update the bitmap */
				switch (expr->nulltesttype)
				{
					case IS_NULL:
						match = (item->isnull[idx]) ? true : match;
						break;

					case IS_NOT_NULL:
						match = (!item->isnull[idx]) ? true : match;
						break;
				}

				/* now, update the match bitmap, depending on OR/AND type */
				if (is_or)
					matches[i] = Max(matches[i], match);
				else
					matches[i] = Min(matches[i], match);
			}
		}
		else if (is_orclause(clause) || is_andclause(clause))
		{
			/* AND/OR clause, with all subclauses being compatible */

			int			i;
			BoolExpr   *bool_clause = ((BoolExpr *) clause);
			List	   *bool_clauses = bool_clause->args;

			/* match/mismatch bitmap for each MCV item */
			bool	   *bool_matches = NULL;

			Assert(bool_clauses != NIL);
			Assert(list_length(bool_clauses) >= 2);

			/* build the match bitmap for the OR-clauses */
			bool_matches = mcv_get_match_bitmap(root, bool_clauses, keys,
												mcvlist, is_orclause(clause));

			/*
			 * Merge the bitmap produced by mcv_get_match_bitmap into the
			 * current one. We need to consider if we're evaluating AND or OR
			 * condition when merging the results.
			 */
			for (i = 0; i < mcvlist->nitems; i++)
			{
				/* Is this OR or AND clause? */
				if (is_or)
					matches[i] = Max(matches[i], bool_matches[i]);
				else
					matches[i] = Min(matches[i], bool_matches[i]);
			}

			pfree(bool_matches);
		}
		else if (is_notclause(clause))
		{
			/* NOT clause, with all subclauses compatible */

			int			i;
			BoolExpr   *not_clause = ((BoolExpr *) clause);
			List	   *not_args = not_clause->args;

			/* match/mismatch bitmap for each MCV item */
			bool	   *not_matches = NULL;

			Assert(not_args != NIL);
			Assert(list_length(not_args) == 1);

			/* build the match bitmap for the NOT-clause */
			not_matches = mcv_get_match_bitmap(root, not_args, keys,
											   mcvlist, false);

			/*
			 * Merge the bitmap produced by mcv_get_match_bitmap into the
			 * current one.
			 */
			for (i = 0; i < mcvlist->nitems; i++)
			{
				/*
				 * When handling a NOT clause, we need to invert the result
				 * before merging it into the global result.
				 */
				if (not_matches[i] == false)
					not_matches[i] = true;
				else
					not_matches[i] = false;

				/* Is this OR or AND clause? */
				if (is_or)
					matches[i] = Max(matches[i], not_matches[i]);
				else
					matches[i] = Min(matches[i], not_matches[i]);
			}

			pfree(not_matches);
		}
		else if (IsA(clause, Var))
		{
			/* Var (has to be a boolean Var, possibly from below NOT) */

			Var		   *var = (Var *) (clause);

			/* match the attribute to a dimension of the statistic */
			int			idx = bms_member_index(keys, var->varattno);

			Assert(var->vartype == BOOLOID);

			/*
			 * Walk through the MCV items and evaluate the current clause. We
			 * can skip items that were already ruled out, and terminate if
			 * there are no remaining MCV items that might possibly match.
			 */
			for (i = 0; i < mcvlist->nitems; i++)
			{
				MCVItem    *item = mcvlist->items[i];
				bool		match = false;

				/* if the item is NULL, it's a mismatch */
				if (!item->isnull[idx] && DatumGetBool(item->values[idx]))
					match = true;

				/* now, update the match bitmap, depending on OR/AND type */
				if (is_or)
					matches[i] = Max(matches[i], match);
				else
					matches[i] = Min(matches[i], match);
			}
		}
		else
		{
			elog(ERROR, "unknown clause type: %d", clause->type);
		}
	}

	return matches;
}


/*
 * mcv_clauselist_selectivity
 *		Return the selectivity estimate computed using an MCV list.
 *
 * First builds a bitmap of MCV items matching the clauses, and then sums
 * the frequencies of matching items.
 *
 * It also produces two additional interesting selectivities - total
 * selectivity of all the MCV items (not just the matching ones), and the
 * base frequency computed on the assumption of independence.
 */
Selectivity
mcv_clauselist_selectivity(PlannerInfo *root, StatisticExtInfo *stat,
						   List *clauses, int varRelid,
						   JoinType jointype, SpecialJoinInfo *sjinfo,
						   RelOptInfo *rel,
						   Selectivity *basesel, Selectivity *totalsel)
{
	int			i;
	MCVList    *mcv;
	Selectivity s = 0.0;

	/* match/mismatch bitmap for each MCV item */
	bool	   *matches = NULL;

	/* load the MCV list stored in the statistics object */
	mcv = statext_mcv_load(stat->statOid);

	/* build a match bitmap for the clauses */
	matches = mcv_get_match_bitmap(root, clauses, stat->keys, mcv, false);

	/* sum frequencies for all the matching MCV items */
	*basesel = 0.0;
	*totalsel = 0.0;
	for (i = 0; i < mcv->nitems; i++)
	{
		*totalsel += mcv->items[i]->frequency;

		if (matches[i] != false)
		{
			/* XXX Shouldn't the basesel be outside the if condition? */
			*basesel += mcv->items[i]->base_frequency;
			s += mcv->items[i]->frequency;
		}
	}

	return s;
}
