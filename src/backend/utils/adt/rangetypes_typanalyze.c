/*-------------------------------------------------------------------------
 *
 * rangetypes_typanalyze.c
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
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
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
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/rangetypes.h"

#define MINIMUM(a,b) (((a)<(b))?(a):(b))  // PROJECT_ULB

static int	float8_qsort_cmp(const void *a1, const void *a2);
static int	range_bound_qsort_cmp(const void *a1, const void *a2, void *arg);
static int  double_range_bound_qsort_cmp(const void *a1, const void *a2, void *arg);  // PROJECT_ULB
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

	/* Get information about range type; note column might be a domain */
	typcache = range_get_typcache(fcinfo, getBaseType(stats->attrtypid));

	if (attr->attstattarget < 0)
		attr->attstattarget = default_statistics_target;

	stats->compute_stats = compute_range_stats;
	stats->extra_data = typcache;
	/* same as in std_typanalyze */
	stats->minrows = 300 * attr->attstattarget;

	PG_RETURN_BOOL(true);
}

/*
 * Comparison function for sorting float8s, used for range lengths.
 */
static int
float8_qsort_cmp(const void *a1, const void *a2)
{
	const float8 *f1 = (const float8 *) a1;
	const float8 *f2 = (const float8 *) a2;

	if (*f1 < *f2)
		return -1;
	else if (*f1 == *f2)
		return 0;
	else
		return 1;
}

/*
 * Comparison function for sorting RangeBounds.
 */
static int
range_bound_qsort_cmp(const void *a1, const void *a2, void *arg)
{
	RangeBound *b1 = (RangeBound *) a1;
	RangeBound *b2 = (RangeBound *) a2;
	TypeCacheEntry *typcache = (TypeCacheEntry *) arg;

	return range_cmp_bounds(typcache, b1, b2);
}

/*
 * PROJECT_ULB
 * Comparison function to sort.
 */
static int
double_range_bound_qsort_cmp(const void *a1, const void *a2, void *arg)
{
	DoubleRangeBound *b1 = (DoubleRangeBound *) a1;
	DoubleRangeBound *b2 = (DoubleRangeBound *) a2;
	TypeCacheEntry *typcache = (TypeCacheEntry *) arg;

	return range_bound_qsort_cmp(&(b1->lb), &(b2->lb), arg);
}
static int
range_bound_idx_qsort_cmp(const void *a1, const void *a2, void *arg)
{
	RangeBoundIdx *b1 = (RangeBoundIdx *) a1;
	RangeBoundIdx *b2 = (RangeBoundIdx *) a2;
	TypeCacheEntry *typcache = (TypeCacheEntry *) arg;

	return range_bound_qsort_cmp(&(b1->bound), &(b2->bound), arg);
}

/*
 * compute_range_stats() -- compute statistics for a range column
 */
static void
compute_range_stats(VacAttrStats *stats, AnalyzeAttrFetchFunc fetchfunc,
					int samplerows, double totalrows)
{
	TypeCacheEntry *typcache = (TypeCacheEntry *) stats->extra_data;
	bool		has_subdiff = OidIsValid(typcache->rng_subdiff_finfo.fn_oid);
	int			null_cnt = 0;
	int			non_null_cnt = 0;
	int			non_empty_cnt = 0;
	int			empty_cnt = 0;
	int			range_no;
	int			slot_idx;
	int			num_bins = stats->attr->attstattarget;
	int			num_hist;
	float8	   *lengths;
	double		total_width = 0;
	/* PROJECT_ULB changed the representation of lowers and uppers */
	DoubleRangeBound *lowers;
	RangeBoundIdx *uppers;

	/* Allocate memory to hold range bounds and lengths of the sample ranges. */
	/* PROJECT_ULB changed allocation too for lowers and uppers */
	lowers = (DoubleRangeBound *) palloc(sizeof(DoubleRangeBound) * samplerows);
	lengths = (float8 *) palloc(sizeof(float8) * samplerows);

	/* Loop over the sample ranges. */
	for (range_no = 0; range_no < samplerows; range_no++)
	{
		Datum		value;
		bool		isnull,
					empty;
		RangeType  *range;
		RangeBound	lower,
					upper;
		float8		length;

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
		range = DatumGetRangeTypeP(value);
		range_deserialize(typcache, range, &lower, &upper, &empty);

		if (!empty)
		{
			/* Remember bounds and length for further usage in histograms */
			/* PROJECT_ULB changed too */
			lowers[non_empty_cnt].lb = lower;
			lowers[non_empty_cnt].ub = upper;

			if (lower.infinite || upper.infinite)
			{
				/* Length of any kind of an infinite range is infinite */
				length = get_float8_infinity();
			}
			else if (has_subdiff)
			{
				/*
				 * For an ordinary range, use subdiff function between upper
				 * and lower bound values.
				 */
				length = DatumGetFloat8(FunctionCall2Coll(&typcache->rng_subdiff_finfo,
														  typcache->rng_collation,
														  upper.val, lower.val));
			}
			else
			{
				/* Use default value of 1.0 if no subdiff is available. */
				length = 1.0;
			}
			lengths[non_empty_cnt] = length;

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
		Datum	   *length_hist_values;
		Datum	   *special_info_datum;  // PROJECT_ULB
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

		/* Estimate that non-null values are unique */
		stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);

		/* Must copy the target values into anl_context */
		old_cxt = MemoryContextSwitchTo(stats->anl_context);

		/*
		 * Generate a bounds histogram slot entry if there are at least two
		 * values.
		 */
		if (non_empty_cnt >= 2)
		{
			/* PROJECT_ULB create uppers based on ub and index in lowers after sorting */
			uppers = (RangeBoundIdx *) palloc(sizeof(RangeBoundIdx) * samplerows);
			/* Sort bound values */
			qsort_arg(lowers, non_empty_cnt, sizeof(DoubleRangeBound),
					  double_range_bound_qsort_cmp, typcache);
			for (i = 0; i < non_empty_cnt; i++)
			{
				uppers[i].bound = lowers[i].ub;
				uppers[i].idx = i;
			}
			qsort_arg(uppers, non_empty_cnt, sizeof(RangeBoundIdx),
					  range_bound_idx_qsort_cmp, typcache);

			num_hist = non_empty_cnt;
			if (num_hist > num_bins)
				num_hist = num_bins + 1;

			bound_hist_values = (Datum *) palloc(num_hist * sizeof(Datum));

			/*
			 * The object of this loop is to construct ranges from first and
			 * last entries in lowers[] and uppers[] along with evenly-spaced
			 * values in between. So the i'th value is a range of lowers[(i *
			 * (nvals - 1)) / (num_hist - 1)] and uppers[(i * (nvals - 1)) /
			 * (num_hist - 1)]. But computing that subscript directly risks
			 * integer overflow when the stats target is more than a couple
			 * thousand.  Instead we add (nvals - 1) / (num_hist - 1) to pos
			 * at each step, tracking the integral and fractional parts of the
			 * sum separately.
			 */
			delta = (non_empty_cnt - 1) / (num_hist - 1);
			deltafrac = (non_empty_cnt - 1) % (num_hist - 1);
			pos = posfrac = 0;

			for (i = 0; i < num_hist; i++)
			{
				bound_hist_values[i] = PointerGetDatum(range_serialize(typcache,
																	   &(lowers[pos].lb),
																	   &(uppers[pos].bound),
																	   false));
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

		/*
		 * Generate a length histogram slot entry if there are at least two
		 * values.
		 */
		if (non_empty_cnt >= 2)
		{
			/*
			 * Ascending sort of range lengths for further filling of
			 * histogram
			 */
			qsort(lengths, non_empty_cnt, sizeof(float8), float8_qsort_cmp);

			num_hist = non_empty_cnt;
			if (num_hist > num_bins)
				num_hist = num_bins + 1;

			length_hist_values = (Datum *) palloc(num_hist * sizeof(Datum));

			/*
			 * The object of this loop is to copy the first and last lengths[]
			 * entries along with evenly-spaced values in between. So the i'th
			 * value is lengths[(i * (nvals - 1)) / (num_hist - 1)]. But
			 * computing that subscript directly risks integer overflow when
			 * the stats target is more than a couple thousand.  Instead we
			 * add (nvals - 1) / (num_hist - 1) to pos at each step, tracking
			 * the integral and fractional parts of the sum separately.
			 */
			delta = (non_empty_cnt - 1) / (num_hist - 1);
			deltafrac = (non_empty_cnt - 1) % (num_hist - 1);
			pos = posfrac = 0;

			for (i = 0; i < num_hist; i++)
			{
				length_hist_values[i] = Float8GetDatum(lengths[pos]);
				pos += delta;
				posfrac += deltafrac;
				if (posfrac >= (num_hist - 1))
				{
					/* fractional part exceeds 1, carry to integer part */
					pos++;
					posfrac -= (num_hist - 1);
				}
			}
		}
		else
		{
			/*
			 * Even when we don't create the histogram, store an empty array
			 * to mean "no histogram". We can't just leave stavalues NULL,
			 * because get_attstatsslot() errors if you ask for stavalues, and
			 * it's NULL. We'll still store the empty fraction in stanumbers.
			 */
			length_hist_values = palloc(0);
			num_hist = 0;
		}
		stats->staop[slot_idx] = Float8LessOperator;
		stats->stacoll[slot_idx] = InvalidOid;
		stats->stavalues[slot_idx] = length_hist_values;
		stats->numvalues[slot_idx] = num_hist;
		stats->statypid[slot_idx] = FLOAT8OID;
		stats->statyplen[slot_idx] = sizeof(float8);
		stats->statypbyval[slot_idx] = FLOAT8PASSBYVAL;
		stats->statypalign[slot_idx] = 'd';

		/* Store the fraction of empty ranges */
		emptyfrac = (float4 *) palloc(sizeof(float4));
		*emptyfrac = ((double) empty_cnt) / ((double) non_null_cnt);
		stats->stanumbers[slot_idx] = emptyfrac;
		stats->numnumbers[slot_idx] = 1;

		stats->stakind[slot_idx] = STATISTIC_KIND_RANGE_LENGTH_HISTOGRAM;
		slot_idx++;

		/*
		 * PROJECT_ULB
		 * Generate a range_bound_type_histogram slot entry if there are at least two
		 * values.
		 */
		if (non_empty_cnt >= 2)
		{
			int bin;
			int special_size;
			RangeBound bin_lb, bin_ub;
			bool empty;
			float8 *special_info_range;

			num_hist = stats->numvalues[0];
			special_size = 4 * (num_hist-1);  // 4 values for each bin
			special_info_range = (float8 *) palloc(special_size * sizeof(float8));
			special_info_datum = (Datum *) palloc(special_size * sizeof(Datum));

			/* 
			 * Get the upper bound of the bin, note that the upper bound of the bin is the lower bound of the next bin,
			 * and not the  upper bound of the current bin !
			 */
			bin = 0;
			range_deserialize(typcache, DatumGetRangeTypeP(bound_hist_values[bin+1]), &bin_lb, &bin_ub, &empty);
			special_info_range[0] = 0;  // Begin values
			special_info_range[3] = 0;  // Type4 values
			/* Loop over all rows to count the begin and type4 values */
			for (i = 0; i < samplerows; i++)
			{
				if (range_cmp_bounds(typcache, &(lowers[i].lb), &bin_lb) > 0)
				{
					/* Only take new bin if it exists, o/w we continue with the last bin*/
					if (bin < num_hist-2)
					{
						bin++;
						range_deserialize(typcache, DatumGetRangeTypeP(bound_hist_values[bin+1]), &bin_lb, &bin_ub, &empty);
						special_info_range[4*bin] = 0;  // Begin values
						special_info_range[4*bin+3] = 0;  // Type4 values
					}
				}
				/* Count the values for begin/type4 */
				special_info_range[4*bin]++;
				if (range_cmp_bounds(typcache, &(lowers[i].ub), &bin_lb) <= 0)
				{
					special_info_range[4*bin+3]++;
				}
			}

			bin = 0;
			range_deserialize(typcache, DatumGetRangeTypeP(bound_hist_values[bin+1]), &bin_lb, &bin_ub, &empty);
			special_info_range[1] = 0;  // End values
			/* Loop over all rows to count the End values */
			for (i = 0; i < samplerows; i++)
			{
				/* Compare bound of the current row to see in which bin it goes */
				if (range_cmp_bounds(typcache, &(uppers[i].bound), &bin_lb) > 0)
				{
					/* Only take new bin if it exists, o/w we continue with the last bin*/
					if (bin < num_hist-2)
					{
						bin++;
						range_deserialize(typcache, DatumGetRangeTypeP(bound_hist_values[bin+1]), &bin_lb, &bin_ub, &empty);
						special_info_range[4*bin+1] = 0;  // End values
					}
				}
				/* Count the values for end */
				special_info_range[4*bin+1]++;
			}

			/* Loop over all bins to count the Continue values */
			special_info_range[2] = 0;
			for (i = 1; i < num_hist; i++)
			{
				/* Continue[i] = Begin[i-1]+Continue[i-1]-End[i-1] */
				special_info_range[4*i+2] = special_info_range[4*(i-1)] +
											special_info_range[4*(i-1)+2] -
											special_info_range[4*(i-1)+1];
			}

			/*
			 * Convert all int to Datum representing float because I did not manage to send anything
			 * other than floats or ranges
			 */
			for (i = 0; i < special_size; i++)
			{
				special_info_datum[i] = Float8GetDatum(special_info_range[i]);
			}

			stats->stakind[slot_idx] = STATISTIC_KIND_RANGE_TYPE_HISTOGRAM;
			stats->staop[slot_idx] = Float8LessOperator;
			stats->stacoll[slot_idx] = InvalidOid;
			stats->stavalues[slot_idx] = special_info_range;
			stats->numvalues[slot_idx] = special_size;
			stats->statypid[slot_idx] = FLOAT8OID;
			stats->statyplen[slot_idx] = sizeof(float8);
			stats->statypbyval[slot_idx] = FLOAT8PASSBYVAL;
			stats->statypalign[slot_idx] = 'd';
		}

		MemoryContextSwitchTo(old_cxt);
	}
	else if (null_cnt > 0)
	{
		/* We found only nulls; assume the column is entirely null */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		stats->stawidth = 0;	/* "unknown" */
		stats->stadistinct = 0.0;	/* "unknown" */
	}

	/*
	 * We don't need to bother cleaning up any of our temporary palloc's. The
	 * hashtable should also go away, as it used a child memory context.
	 */
}
