/*-------------------------------------------------------------------------
 *
 * ragetypes_typanalyze.c
 *	  Functions for gathering statistics from range columns
 *
 * For a range type column, histograms of lower and upper bounds, and
 * the fraction of NULL and empty ranges are collected.
 *
 * Both histograms have the same length, and they are combined into a
 * single array of ranges. This has the same shape as the histogram that
 * std_typanalyze would collect, but the values are different. Each range
 * in the array is a valid range, even though the lower and upper bounds
 * come from different tuples. In theory, the standard scalar selectivity
 * functions could be used with the combined histogram.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/rangetypes_typanalyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "commands/vacuum.h"
#include "utils/builtins.h"
#include "utils/rangetypes.h"

static void compute_range_stats(VacAttrStats *stats,
		   AnalyzeAttrFetchFunc fetchfunc, int samplerows, double totalrows);

/*
 * range_typanalyze -- typanalyze function for range columns
 */
Datum
range_typanalyze(PG_FUNCTION_ARGS)
{
	VacAttrStats *stats = (VacAttrStats *) PG_GETARG_POINTER(0);
	TypeCacheEntry *typcache;
	Form_pg_attribute attr = stats->attr;

	/* Get information about range type */
	typcache = range_get_typcache(fcinfo, stats->attrtypid);

	if (attr->attstattarget < 0)
        attr->attstattarget = default_statistics_target;

	stats->compute_stats = compute_range_stats;
	stats->extra_data = typcache;
	/* same as in std_typanalyze */
	stats->minrows = 300 * attr->attstattarget;

	PG_RETURN_BOOL(true);
}

/*
 * Comparison function for sorting RangeBounds.
 */
static int
range_bound_qsort_cmp(const void *a1, const void *a2, void *arg)
{
	RangeBound *b1 = (RangeBound *)a1;
	RangeBound *b2 = (RangeBound *)a2;
	TypeCacheEntry *typcache = (TypeCacheEntry *)arg;

	return range_cmp_bounds(typcache, b1, b2);
}

/*
 * compute_range_stats() -- compute statistics for a range column
 */
static void
compute_range_stats(VacAttrStats *stats, AnalyzeAttrFetchFunc fetchfunc,
					int samplerows, double totalrows)
{
	TypeCacheEntry *typcache = (TypeCacheEntry *) stats->extra_data;
	int			null_cnt = 0;
	int			non_null_cnt = 0;
	int			non_empty_cnt = 0;
	int			empty_cnt = 0;
	int			range_no;
	int			slot_idx;
	int			num_bins = stats->attr->attstattarget;
	int			num_hist;
	RangeBound *lowers, *uppers;
	double		total_width = 0;

	/* Allocate memory for arrays of range bounds. */
	lowers = (RangeBound *) palloc(sizeof(RangeBound) * samplerows);
	uppers = (RangeBound *) palloc(sizeof(RangeBound) * samplerows);

	/* Loop over the sample ranges. */
	for (range_no = 0; range_no < samplerows; range_no++)
	{
		Datum		value;
		bool		isnull,
					empty;
		RangeType  *range;
		RangeBound	lower,
					upper;

		vacuum_delay_point();

		value = fetchfunc(stats, range_no, &isnull);
		if (isnull)
		{
			/* range is null, just count that */
			null_cnt++;
			continue;
		}

		/*
		 * XXX: should we ignore wide values, like std_typanalyze does, to
		 * avoid bloating the statistics table?
		 */
		total_width += VARSIZE_ANY(DatumGetPointer(value));

		/* Get range and deserialize it for further analysis. */
		range = DatumGetRangeType(value);
		range_deserialize(typcache, range, &lower, &upper, &empty);

		if (!empty)
		{
			/* Fill bound values for further usage in histograms */
			lowers[non_empty_cnt] = lower;
			uppers[non_empty_cnt] = upper;
			non_empty_cnt++;
		}
		else
			empty_cnt++;

		non_null_cnt++;
	}

	slot_idx = 0;

	/* We can only compute real stats if we found some non-null values. */
	if (non_null_cnt > 0)
	{
		Datum	   *bound_hist_values;
		int			pos,
					posfrac,
					delta,
					deltafrac,
					i;
		MemoryContext old_cxt;
		float4	   *emptyfrac;

		stats->stats_valid = true;
		/* Do the simple null-frac and width stats */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		stats->stawidth = total_width / (double) non_null_cnt;
		stats->stadistinct = -1.0;

		/* Must copy the target values into anl_context */
		old_cxt = MemoryContextSwitchTo(stats->anl_context);

		/*
		 * Generate a histogram slot entry if there are at least two values.
		 */
		if (non_empty_cnt >= 2)
		{
			/* Sort bound values */
			qsort_arg(lowers, non_empty_cnt, sizeof(RangeBound),
					  range_bound_qsort_cmp, typcache);
			qsort_arg(uppers, non_empty_cnt, sizeof(RangeBound),
					  range_bound_qsort_cmp, typcache);

			num_hist = non_empty_cnt;
			if (num_hist > num_bins)
				num_hist = num_bins + 1;

			bound_hist_values = (Datum *) palloc(num_hist * sizeof(Datum));

			/*
			 * The object of this loop is to construct ranges from first and
			 * last entries in lowers[] and uppers[] along with evenly-spaced
			 * values in between. So the i'th value is a range of
			 * lowers[(i * (nvals - 1)) / (num_hist - 1)] and
			 * uppers[(i * (nvals - 1)) / (num_hist - 1)]. But computing that
			 * subscript directly risks integer overflow when the stats target
			 * is more than a couple thousand.  Instead we add
			 * (nvals - 1) / (num_hist - 1) to pos at each step, tracking the
			 * integral and fractional parts of the sum separately.
			 */
			delta = (non_empty_cnt - 1) / (num_hist - 1);
			deltafrac = (non_empty_cnt - 1) % (num_hist - 1);
			pos = posfrac = 0;

			for (i = 0; i < num_hist; i++)
			{
				bound_hist_values[i] = PointerGetDatum(range_serialize(
								typcache, &lowers[pos], &uppers[pos], false));
				pos += delta;
				posfrac += deltafrac;
				if (posfrac >= (num_hist - 1))
				{
					/* fractional part exceeds 1, carry to integer part */
					pos++;
					posfrac -= (num_hist - 1);
				}
			}

			stats->stakind[slot_idx] = STATISTIC_KIND_BOUNDS_HISTOGRAM;
			stats->stavalues[slot_idx] = bound_hist_values;
			stats->numvalues[slot_idx] = num_hist;
			slot_idx++;
		}

		/* Store the fraction of empty ranges */
		emptyfrac = (float4 *) palloc(sizeof(float4));
		*emptyfrac = ((double) empty_cnt) / ((double) non_null_cnt);
		stats->stakind[slot_idx] = STATISTIC_KIND_RANGE_EMPTY_FRAC;
		stats->stanumbers[slot_idx] = emptyfrac;
		stats->numnumbers[slot_idx] = 1;
		slot_idx++;

		MemoryContextSwitchTo(old_cxt);
	}
	else if (null_cnt > 0)
	{
		/* We found only nulls; assume the column is entirely null */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		stats->stawidth = 0;		/* "unknown" */
		stats->stadistinct = 0.0;	/* "unknown" */
	}
	/*
	 * We don't need to bother cleaning up any of our temporary palloc's. The
	 * hashtable should also go away, as it used a child memory context.
	 */
}
