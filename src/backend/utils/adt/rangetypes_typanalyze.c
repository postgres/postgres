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

// hadiii -> ...
typedef struct SimpleRange
{
    int         start;
    int         end;
    int         length;
} SimpleRange;


static int	float8_qsort_cmp(const void *a1, const void *a2);
static int	range_bound_qsort_cmp(const void *a1, const void *a2, void *arg);
static void compute_range_stats(VacAttrStats *stats,
								AnalyzeAttrFetchFunc fetchfunc, int samplerows, double totalrows);

void accumulate_range_in_slot_percentage(float8 s_min, float8 s_max, SimpleRange *range, float8 *value_to_add_to);

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


// hadiii -> ...
void accumulate_range_in_slot_percentage(float8 s_min, float8 s_max, SimpleRange *range, float8 *acc) {
    int r_min =     range->start;
    int r_max =     range->end;
    int r_length =  range->length;

    float8 cv = 0;

    if(s_max < r_min || s_min > r_max)
        cv = 0;
    else if(s_min <= r_min && s_max >= r_max)
        cv = 1;
    else if(s_min > r_min && s_max < r_max)
        cv = (float8)(s_max - s_min) / r_length;
    else if(s_min <= r_min)
        cv = (float8)(s_max - r_min) / r_length;
    else if(s_max >= r_max)
        cv = (float8)(r_max - s_min) / r_length;

    *acc += cv;
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
	RangeBound *lowers,
			   *uppers;
	double		total_width = 0;

	/* Allocate memory to hold range bounds and lengths of the sample ranges. */
	lowers = (RangeBound *) palloc(sizeof(RangeBound) * samplerows);
	uppers = (RangeBound *) palloc(sizeof(RangeBound) * samplerows);
	lengths = (float8 *) palloc(sizeof(float8) * samplerows);

    // hadiii -> ...
    int         sample_lower = 0;
    int         sample_upper = 0;
    int         SLOTS_COUNT = 20;
    int         slot_length = 0;
    // hadiii -> ...
    SimpleRange *simple_ranges = (SimpleRange *) palloc(sizeof(SimpleRange) * samplerows);
    
    float8 *hist_bins = (float8 *) palloc(sizeof(float8) * SLOTS_COUNT + 1);
    float8 *slots_values = (float8 *) palloc(sizeof(float8) * SLOTS_COUNT);
    Datum *hist_bins_values = (Datum *) palloc(sizeof(Datum) * SLOTS_COUNT + 1);
    Datum *datum_slot_values = (Datum *) palloc(sizeof(Datum) * SLOTS_COUNT);

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

        /*
            - read the hist from a selectivity function
            - make the estimator in the same order done in python
            - alter the operator to use the new estimations
            - make some tests
            - ask Hind and Luka to prepare a report
            - finalize the report
        */

		if (!empty)
		{
			/* Remember bounds and length for further usage in histograms */
			lowers[non_empty_cnt] = lower;
			uppers[non_empty_cnt] = upper;

            // hadiii -> ...
            int d_lower = DatumGetInt16(lower.val);
            int d_upper = DatumGetInt16(upper.val);

            if(d_lower < sample_lower) sample_lower = d_lower;
            if(d_upper > sample_upper) sample_upper = d_upper;

            simple_ranges[range_no].start = d_lower;
            simple_ranges[range_no].end = d_upper;
            simple_ranges[range_no].length = d_upper - d_lower;
            
            slot_length = (sample_upper - sample_lower) / SLOTS_COUNT;


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
																	   &lowers[pos],
																	   &uppers[pos],
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

    printf("slot_length: %d\n", slot_length);
    printf("sample_lower: %d\n", sample_lower);

    // hadii
    for(int i = 0; i <= SLOTS_COUNT; i++) {
        hist_bins[i] = (float8)(i * slot_length) + sample_lower;
        if(i == SLOTS_COUNT)
            hist_bins[i] = sample_upper;

        printf("hist_bins[%d]: %f\n", i, hist_bins[i]);
        hist_bins_values[i] = Float8GetDatum(hist_bins[i]);            
    }

    stats->stakind[slot_idx] = STATISTIC_KIND_BINS_HISTOGRAM;
    stats->stavalues[slot_idx] = hist_bins_values;
    stats->numvalues[slot_idx] = SLOTS_COUNT + 1;
    stats->statypid[slot_idx] = FLOAT8OID;
    stats->statyplen[slot_idx] = sizeof(float8);
    stats->statypbyval[slot_idx] = FLOAT8PASSBYVAL;

    slot_idx++;

    // hadii: fill in the histograms
    for(int i = 0; i < samplerows; i++) {
        SimpleRange curr_range = simple_ranges[i];
        for(int j = 1; j < SLOTS_COUNT; j++) {

            float8 slot_min = hist_bins[j];
            float8 slot_max = hist_bins[j + 1];

            accumulate_range_in_slot_percentage(slot_min, slot_max, &curr_range, &slots_values[j]);
        }
    }  

    // hadii
    for(int i = 0; i < SLOTS_COUNT; i++) {
        float8 curr_slot_value = slots_values[i];

        printf("curr_slot_value[%d]: %f\n", i, curr_slot_value);
        datum_slot_values[i] = Float8GetDatum(curr_slot_value);            
    }

    stats->stakind[slot_idx] = STATISTIC_KIND_BINS_VALUES_HISTOGRAM;
    stats->stavalues[slot_idx] = datum_slot_values;
    stats->numvalues[slot_idx] = SLOTS_COUNT;
    stats->statypid[slot_idx] = FLOAT8OID;
    stats->statyplen[slot_idx] = sizeof(float8);
    stats->statypbyval[slot_idx] = FLOAT8PASSBYVAL;

	/*
	 * We don't need to bother cleaning up any of our temporary palloc's. The
	 * hashtable should also go away, as it used a child memory context.
	 */

    // hadiii -> 443
    fflush(stdout);
}
