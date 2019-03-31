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
	MAXALIGN((ndims) * (sizeof(uint16) + sizeof(bool)) + 2 * sizeof(double))

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
		mcvlist = (MCVList *) palloc0(offsetof(MCVList, items) +
									  sizeof(MCVItem) * nitems);

		mcvlist->magic = STATS_MCV_MAGIC;
		mcvlist->type = STATS_MCV_TYPE_BASIC;
		mcvlist->ndimensions = numattrs;
		mcvlist->nitems = nitems;

		/* store info about data type OIDs */
		for (i = 0; i < numattrs; i++)
			mcvlist->types[i] = stats[i]->attrtypid;

		/* Copy the first chunk of groups into the result. */
		for (i = 0; i < nitems; i++)
		{
			/* just pointer to the proper place in the list */
			MCVItem    *item = &mcvlist->items[i];

			item->values = (Datum *) palloc(sizeof(Datum) * numattrs);
			item->isnull = (bool *) palloc(sizeof(bool) * numattrs);

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
	MCVList    *result;
	bool		isnull;
	Datum		mcvlist;
	HeapTuple	htup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(mvoid));

	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for statistics object %u", mvoid);

	mcvlist = SysCacheGetAttr(STATEXTOID, htup,
							  Anum_pg_statistic_ext_stxmcv, &isnull);

	if (isnull)
		elog(ERROR,
			 "requested statistic kind \"%c\" is not yet built for statistics object %u",
			 STATS_EXT_DEPENDENCIES, mvoid);

	result = statext_mcv_deserialize(DatumGetByteaP(mcvlist));

	ReleaseSysCache(htup);

	return result;
}


/*
 * statext_mcv_serialize
 *		Serialize MCV list into a pg_mcv_list value.
 *
 * The MCV items may include values of various data types, and it's reasonable
 * to expect redundancy (values for a given attribute, repeated for multiple
 * MCV list items). So we deduplicate the values into arrays, and then replace
 * the values by indexes into those arrays.
 *
 * The overall structure of the serialized representation looks like this:
 *
 * +---------------+----------------+---------------------+-------+
 * | header fields | dimension info | deduplicated values | items |
 * +---------------+----------------+---------------------+-------+
 *
 * Where dimension info stores information about type of K-th attribute (e.g.
 * typlen, typbyval and length of deduplicated values).  Deduplicated values
 * store deduplicated values for each attribute.  And items store the actual
 * MCV list items, with values replaced by indexes into the arrays.
 *
 * When serializing the items, we use uint16 indexes. The number of MCV items
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
	char	   *raw;
	char	   *ptr;

	/* values per dimension (and number of non-NULL values) */
	Datum	  **values = (Datum **) palloc0(sizeof(Datum *) * ndims);
	int		   *counts = (int *) palloc0(sizeof(int) * ndims);

	/*
	 * We'll include some rudimentary information about the attribute types
	 * (length, by-val flag), so that we don't have to look them up while
	 * deserializating the MCV list (we already have the type OID in the
	 * header).  This is safe, because when changing type of the attribute the
	 * statistics gets dropped automatically.  We need to store the info about
	 * the arrays of deduplicated values anyway.
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
			if (mcvlist->items[i].isnull[dim])
				continue;

			/* append the value at the end */
			values[dim][counts[dim]] = mcvlist->items[i].values[dim];
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
				Size	len;

				values[dim][i] = PointerGetDatum(PG_DETOAST_DATUM(values[dim][i]));

				len = VARSIZE_ANY(values[dim][i]);
				info[dim].nbytes += MAXALIGN(len);
			}
		}
		else if (info[dim].typlen == -2)	/* cstring */
		{
			info[dim].nbytes = 0;
			for (i = 0; i < info[dim].nvalues; i++)
			{
				Size	len;

				/* c-strings include terminator, so +1 byte */
				values[dim][i] = PointerGetDatum(PG_DETOAST_DATUM(values[dim][i]));

				len = strlen(DatumGetCString(values[dim][i])) + 1;
				info[dim].nbytes += MAXALIGN(len);
			}
		}

		/* we know (count>0) so there must be some data */
		Assert(info[dim].nbytes > 0);
	}

	/*
	 * Now we can finally compute how much space we'll actually need for the
	 * whole serialized MCV list (varlena header, MCV header, dimension info
	 * for each attribute, deduplicated values and items).
	 */
	total_length = offsetof(MCVList, items)
		+ MAXALIGN(ndims * sizeof(DimensionInfo));

	/* add space for the arrays of deduplicated values */
	for (i = 0; i < ndims; i++)
		total_length += MAXALIGN(info[i].nbytes);

	/* and finally the items (no additional alignment needed) */
	total_length += mcvlist->nitems * itemsize;

	/*
	 * Allocate space for the whole serialized MCV list (we'll skip bytes,
	 * so we set them to zero to make the result more compressible).
	 */
	raw = palloc0(total_length);
	ptr = raw;

	/* copy the MCV list header */
	memcpy(ptr, mcvlist, offsetof(MCVList, items));
	ptr += offsetof(MCVList, items);

	/* store information about the attributes */
	memcpy(ptr, info, sizeof(DimensionInfo) * ndims);
	ptr += MAXALIGN(sizeof(DimensionInfo) * ndims);

	/* Copy the deduplicated values for all attributes to the output. */
	for (dim = 0; dim < ndims; dim++)
	{
		/* remember the starting point for Asserts later */
		char	   *start PG_USED_FOR_ASSERTS_ONLY = ptr;

		for (i = 0; i < info[dim].nvalues; i++)
		{
			Datum		value = values[dim][i];

			if (info[dim].typbyval) /* passed by value */
			{
				Datum		tmp;

				/*
				 * For values passed by value, we need to copy just the
				 * significant bytes - we can't use memcpy directly, as that
				 * assumes little endian behavior.  store_att_byval does
				 * almost what we need, but it requires properly aligned
				 * buffer - the output buffer does not guarantee that. So we
				 * simply use a static Datum variable (which guarantees proper
				 * alignment), and then copy the value from it.
				 */
				store_att_byval(&tmp, value, info[dim].typlen);

				memcpy(ptr, &tmp, info[dim].typlen);
				ptr += info[dim].typlen;
			}
			else if (info[dim].typlen > 0)	/* pased by reference */
			{
				/* no special alignment needed, treated as char array */
				memcpy(ptr, DatumGetPointer(value), info[dim].typlen);
				ptr += info[dim].typlen;
			}
			else if (info[dim].typlen == -1)	/* varlena */
			{
				int			len = VARSIZE_ANY(value);

				memcpy(ptr, DatumGetPointer(value), len);
				ptr += MAXALIGN(len);
			}
			else if (info[dim].typlen == -2)	/* cstring */
			{
				Size		len = strlen(DatumGetCString(value)) + 1;	/* terminator */

				memcpy(ptr, DatumGetCString(value), len);
				ptr += MAXALIGN(len);
			}

			/* no underflows or overflows */
			Assert((ptr > start) && ((ptr - start) <= info[dim].nbytes));
		}

		/* we should get exactly nbytes of data for this dimension */
		Assert((ptr - start) == info[dim].nbytes);

		/* make sure the pointer is aligned correctly after each dimension */
		ptr = raw + MAXALIGN(ptr - raw);
	}

	/* Serialize the items, with uint16 indexes instead of the values. */
	for (i = 0; i < mcvlist->nitems; i++)
	{
		MCVItem    *mcvitem = &mcvlist->items[i];

		/* don't write beyond the allocated space */
		Assert(ptr <= raw + total_length - itemsize);

		/* reset the item (we only allocate it once and reuse it) */
		memset(item, 0, itemsize);

		for (dim = 0; dim < ndims; dim++)
		{
			Datum	   *value;

			/* do the lookup only for non-NULL values */
			if (mcvitem->isnull[dim])
				continue;

			value = (Datum *) bsearch_arg(&mcvitem->values[dim], values[dim],
										  info[dim].nvalues, sizeof(Datum),
										  compare_scalars_simple, &ssup[dim]);

			Assert(value != NULL);	/* serialization or deduplication error */

			/* compute index within the array */
			ITEM_INDEXES(item)[dim] = (uint16) (value - values[dim]);

			/* check the index is within expected bounds */
			Assert(ITEM_INDEXES(item)[dim] < info[dim].nvalues);
		}

		/* copy NULL and frequency flags into the item */
		memcpy(ITEM_NULLS(item, ndims), mcvitem->isnull, sizeof(bool) * ndims);
		memcpy(ITEM_FREQUENCY(item, ndims), &mcvitem->frequency, sizeof(double));
		memcpy(ITEM_BASE_FREQUENCY(item, ndims), &mcvitem->base_frequency, sizeof(double));

		/* copy the serialized item into the array */
		memcpy(ptr, item, itemsize);

		ptr += itemsize;
	}

	/* at this point we expect to match the total_length exactly */
	Assert((ptr - raw) == total_length);

	pfree(item);
	pfree(values);
	pfree(counts);

	output = (bytea *) palloc(VARHDRSZ + total_length);
	SET_VARSIZE(output, VARHDRSZ + total_length);

	memcpy(VARDATA_ANY(output), raw, total_length);

	pfree(raw);

	return output;
}

/*
 * statext_mcv_deserialize
 *		Reads serialized MCV list into MCVList structure.
 *
 * All the memory needed by the MCV list is allocated as a single chunk, so
 * it's possible to simply pfree() it at once.
 */
MCVList *
statext_mcv_deserialize(bytea *data)
{
	int			dim,
				i;
	Size		expected_size;
	MCVList    *mcvlist;
	char	   *raw;
	char	   *ptr;

	int			ndims,
				nitems,
				itemsize;
	DimensionInfo *info = NULL;

	/* local allocation buffer (used only for deserialization) */
	Datum	  **map = NULL;

	/* MCV list */
	Size		mcvlen;

	/* buffer used for the result */
	Size		datalen;
	char	   *dataptr;
	char	   *valuesptr;
	char	   *isnullptr;

	if (data == NULL)
		return NULL;

	/*
	 * We can't possibly deserialize a MCV list if there's not even a complete
	 * header.
	 */
	if (VARSIZE_ANY_EXHDR(data) < offsetof(MCVList, items))
		elog(ERROR, "invalid MCV size %zd (expected at least %zu)",
			 VARSIZE_ANY_EXHDR(data), offsetof(MCVList, items));

	/* read the MCV list header */
	mcvlist = (MCVList *) palloc0(offsetof(MCVList, items));

	/* initialize pointer to the data part (skip the varlena header) */
	raw = palloc(VARSIZE_ANY_EXHDR(data));
	ptr = raw;

	memcpy(raw, VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data));

	/* get the header and perform further sanity checks */
	memcpy(mcvlist, ptr, offsetof(MCVList, items));
	ptr += offsetof(MCVList, items);

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
		elog(ERROR, "invalid MCV size %zd (expected %zu)",
			 VARSIZE_ANY_EXHDR(data), expected_size);

	/* Now it's safe to access the dimension info. */
	info = (DimensionInfo *) ptr;
	ptr += MAXALIGN(ndims * sizeof(DimensionInfo));

	/* account for the value arrays */
	for (dim = 0; dim < ndims; dim++)
	{
		/*
		 * XXX I wonder if we can/should rely on asserts here. Maybe those
		 * checks should be done every time?
		 */
		Assert(info[dim].nvalues >= 0);
		Assert(info[dim].nbytes >= 0);

		expected_size += MAXALIGN(info[dim].nbytes);
	}

	/*
	 * Now we know the total expected MCV size, including all the pieces
	 * (header, dimension info. items and deduplicated data). So do the final
	 * check on size.
	 */
	if (VARSIZE_ANY_EXHDR(data) != expected_size)
		elog(ERROR, "invalid MCV size %zd (expected %zu)",
			 VARSIZE_ANY_EXHDR(data), expected_size);

	/*
	 * We need an array of Datum values for each dimension, so that we can
	 * easily translate the uint16 indexes later. We also need a top-level
	 * array of pointers to those per-dimension arrays.
	 *
	 * While allocating the arrays for dimensions, compute how much space we
	 * need for a copy of the by-ref data, as we can't simply point to the
	 * original values (it might go away).
	 */
	datalen = 0;				/* space for by-ref data */
	map = (Datum **) palloc(ndims * sizeof(Datum **));

	for (dim = 0; dim < ndims; dim++)
	{
		map[dim] = (Datum *) palloc(sizeof(Datum) * info[dim].nvalues);

		/* space needed for a copy of data for by-ref types */
		if (!info[dim].typbyval)
			datalen += MAXALIGN(info[dim].nbytes);
	}

	/*
	 * Now resize the MCV list so that the allocation includes all the data
	 * Allocate space for a copy of the data, as we can't simply reference the
	 * original data - it may disappear while we're still using the MCV list,
	 * e.g. due to catcache release. Only needed for by-ref types.
	 */
	mcvlen = MAXALIGN(offsetof(MCVList, items) + (sizeof(MCVItem) * nitems));

	/* arrays of values and isnull flags for all MCV items */
	mcvlen += nitems * MAXALIGN(sizeof(Datum) * ndims);
	mcvlen += nitems * MAXALIGN(sizeof(bool) * ndims);

	/* we don't quite need to align this, but it makes some asserts easier */
	mcvlen += MAXALIGN(datalen);

	/* now resize the deserialized MCV list, and compute pointers to parts */
	mcvlist = repalloc(mcvlist, mcvlen);

	/* pointer to the beginning of values/isnull arrays */
	valuesptr = (char *) mcvlist
		+ MAXALIGN(offsetof(MCVList, items) + (sizeof(MCVItem) * nitems));

	isnullptr = valuesptr + (nitems * MAXALIGN(sizeof(Datum) * ndims));

	dataptr = isnullptr + (nitems * MAXALIGN(sizeof(bool) * ndims));

	/*
	 * Build mapping (index => value) for translating the serialized data into
	 * the in-memory representation.
	 */
	for (dim = 0; dim < ndims; dim++)
	{
		/* remember start position in the input array */
		char	   *start PG_USED_FOR_ASSERTS_ONLY = ptr;

		if (info[dim].typbyval)
		{
			/* for by-val types we simply copy data into the mapping */
			for (i = 0; i < info[dim].nvalues; i++)
			{
				Datum		v = 0;

				memcpy(&v, ptr, info[dim].typlen);
				ptr += info[dim].typlen;

				map[dim][i] = fetch_att(&v, true, info[dim].typlen);

				/* no under/overflow of input array */
				Assert(ptr <= (start + info[dim].nbytes));
			}
		}
		else
		{
			/* for by-ref types we need to also make a copy of the data */

			/* passed by reference, but fixed length (name, tid, ...) */
			if (info[dim].typlen > 0)
			{
				for (i = 0; i < info[dim].nvalues; i++)
				{
					memcpy(dataptr, ptr, info[dim].typlen);
					ptr += info[dim].typlen;

					/* just point into the array */
					map[dim][i] = PointerGetDatum(dataptr);
					dataptr += info[dim].typlen;
				}
			}
			else if (info[dim].typlen == -1)
			{
				/* varlena */
				for (i = 0; i < info[dim].nvalues; i++)
				{
					Size		len = VARSIZE_ANY(ptr);

					memcpy(dataptr, ptr, len);
					ptr += MAXALIGN(len);

					/* just point into the array */
					map[dim][i] = PointerGetDatum(dataptr);
					dataptr += MAXALIGN(len);
				}
			}
			else if (info[dim].typlen == -2)
			{
				/* cstring */
				for (i = 0; i < info[dim].nvalues; i++)
				{
					Size		len = (strlen(ptr) + 1);	/* don't forget the \0 */

					memcpy(dataptr, ptr, len);
					ptr += MAXALIGN(len);

					/* just point into the array */
					map[dim][i] = PointerGetDatum(dataptr);
					dataptr += MAXALIGN(len);
				}
			}

			/* no under/overflow of input array */
			Assert(ptr <= (start + info[dim].nbytes));

			/* no overflow of the output mcv value */
			Assert(dataptr <= ((char *) mcvlist + mcvlen));
		}

		/* check we consumed input data for this dimension exactly */
		Assert(ptr == (start + info[dim].nbytes));

		/* ensure proper alignment of the data */
		ptr = raw + MAXALIGN(ptr - raw);
	}

	/* we should have also filled the MCV list exactly */
	Assert(dataptr == ((char *) mcvlist + mcvlen));

	/* deserialize the MCV items and translate the indexes to Datums */
	for (i = 0; i < nitems; i++)
	{
		uint16	   *indexes = NULL;
		MCVItem    *item = &mcvlist->items[i];

		item->values = (Datum *) valuesptr;
		valuesptr += MAXALIGN(sizeof(Datum) * ndims);

		item->isnull = (bool *) isnullptr;
		isnullptr += MAXALIGN(sizeof(bool) * ndims);


		/* just point to the right place */
		indexes = ITEM_INDEXES(ptr);

		memcpy(item->isnull, ITEM_NULLS(ptr, ndims), sizeof(bool) * ndims);
		memcpy(&item->frequency, ITEM_FREQUENCY(ptr, ndims), sizeof(double));
		memcpy(&item->base_frequency, ITEM_BASE_FREQUENCY(ptr, ndims), sizeof(double));

		/* translate the values */
		for (dim = 0; dim < ndims; dim++)
			if (!item->isnull[dim])
				item->values[dim] = map[dim][indexes[dim]];

		ptr += ITEM_SIZE(ndims);

		/* check we're not overflowing the input */
		Assert(ptr <= (char *) raw + VARSIZE_ANY_EXHDR(data));
	}

	/* check that we processed all the data */
	Assert(ptr == raw + VARSIZE_ANY_EXHDR(data));

	/* release the buffers used for mapping */
	for (dim = 0; dim < ndims; dim++)
		pfree(map[dim]);

	pfree(map);
	pfree(raw);

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

		item = &mcvlist->items[call_cntr];

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
					MCVItem    *item = &mcvlist->items[i];

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
				MCVItem    *item = &mcvlist->items[i];

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
				MCVItem    *item = &mcvlist->items[i];
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
		*totalsel += mcv->items[i].frequency;

		if (matches[i] != false)
		{
			/* XXX Shouldn't the basesel be outside the if condition? */
			*basesel += mcv->items[i].base_frequency;
			s += mcv->items[i].frequency;
		}
	}

	return s;
}
