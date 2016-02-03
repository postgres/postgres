/*-------------------------------------------------------------------------
 *
 * array_typanalyze.c
 *	  Functions for gathering statistics from array columns
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/array_typanalyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tuptoaster.h"
#include "catalog/pg_collation.h"
#include "commands/vacuum.h"
#include "utils/array.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"


/*
 * To avoid consuming too much memory, IO and CPU load during analysis, and/or
 * too much space in the resulting pg_statistic rows, we ignore arrays that
 * are wider than ARRAY_WIDTH_THRESHOLD (after detoasting!).  Note that this
 * number is considerably more than the similar WIDTH_THRESHOLD limit used
 * in analyze.c's standard typanalyze code.
 */
#define ARRAY_WIDTH_THRESHOLD 0x10000

/* Extra data for compute_array_stats function */
typedef struct
{
	/* Information about array element type */
	Oid			type_id;		/* element type's OID */
	Oid			eq_opr;			/* default equality operator's OID */
	bool		typbyval;		/* physical properties of element type */
	int16		typlen;
	char		typalign;

	/*
	 * Lookup data for element type's comparison and hash functions (these are
	 * in the type's typcache entry, which we expect to remain valid over the
	 * lifespan of the ANALYZE run)
	 */
	FmgrInfo   *cmp;
	FmgrInfo   *hash;

	/* Saved state from std_typanalyze() */
	AnalyzeAttrComputeStatsFunc std_compute_stats;
	void	   *std_extra_data;
} ArrayAnalyzeExtraData;

/*
 * While compute_array_stats is running, we keep a pointer to the extra data
 * here for use by assorted subroutines.  compute_array_stats doesn't
 * currently need to be re-entrant, so avoiding this is not worth the extra
 * notational cruft that would be needed.
 */
static ArrayAnalyzeExtraData *array_extra_data;

/* A hash table entry for the Lossy Counting algorithm */
typedef struct
{
	Datum		key;			/* This is 'e' from the LC algorithm. */
	int			frequency;		/* This is 'f'. */
	int			delta;			/* And this is 'delta'. */
	int			last_container; /* For de-duplication of array elements. */
} TrackItem;

/* A hash table entry for distinct-elements counts */
typedef struct
{
	int			count;			/* Count of distinct elements in an array */
	int			frequency;		/* Number of arrays seen with this count */
} DECountItem;

static void compute_array_stats(VacAttrStats *stats,
		   AnalyzeAttrFetchFunc fetchfunc, int samplerows, double totalrows);
static void prune_element_hashtable(HTAB *elements_tab, int b_current);
static uint32 element_hash(const void *key, Size keysize);
static int	element_match(const void *key1, const void *key2, Size keysize);
static int	element_compare(const void *key1, const void *key2);
static int	trackitem_compare_frequencies_desc(const void *e1, const void *e2);
static int	trackitem_compare_element(const void *e1, const void *e2);
static int	countitem_compare_count(const void *e1, const void *e2);


/*
 * array_typanalyze -- typanalyze function for array columns
 */
Datum
array_typanalyze(PG_FUNCTION_ARGS)
{
	VacAttrStats *stats = (VacAttrStats *) PG_GETARG_POINTER(0);
	Oid			element_typeid;
	TypeCacheEntry *typentry;
	ArrayAnalyzeExtraData *extra_data;

	/*
	 * Call the standard typanalyze function.  It may fail to find needed
	 * operators, in which case we also can't do anything, so just fail.
	 */
	if (!std_typanalyze(stats))
		PG_RETURN_BOOL(false);

	/*
	 * Check attribute data type is a varlena array (or a domain over one).
	 */
	element_typeid = get_base_element_type(stats->attrtypid);
	if (!OidIsValid(element_typeid))
		elog(ERROR, "array_typanalyze was invoked for non-array type %u",
			 stats->attrtypid);

	/*
	 * Gather information about the element type.  If we fail to find
	 * something, return leaving the state from std_typanalyze() in place.
	 */
	typentry = lookup_type_cache(element_typeid,
								 TYPECACHE_EQ_OPR |
								 TYPECACHE_CMP_PROC_FINFO |
								 TYPECACHE_HASH_PROC_FINFO);

	if (!OidIsValid(typentry->eq_opr) ||
		!OidIsValid(typentry->cmp_proc_finfo.fn_oid) ||
		!OidIsValid(typentry->hash_proc_finfo.fn_oid))
		PG_RETURN_BOOL(true);

	/* Store our findings for use by compute_array_stats() */
	extra_data = (ArrayAnalyzeExtraData *) palloc(sizeof(ArrayAnalyzeExtraData));
	extra_data->type_id = typentry->type_id;
	extra_data->eq_opr = typentry->eq_opr;
	extra_data->typbyval = typentry->typbyval;
	extra_data->typlen = typentry->typlen;
	extra_data->typalign = typentry->typalign;
	extra_data->cmp = &typentry->cmp_proc_finfo;
	extra_data->hash = &typentry->hash_proc_finfo;

	/* Save old compute_stats and extra_data for scalar statistics ... */
	extra_data->std_compute_stats = stats->compute_stats;
	extra_data->std_extra_data = stats->extra_data;

	/* ... and replace with our info */
	stats->compute_stats = compute_array_stats;
	stats->extra_data = extra_data;

	/*
	 * Note we leave stats->minrows set as std_typanalyze set it.  Should it
	 * be increased for array analysis purposes?
	 */

	PG_RETURN_BOOL(true);
}

/*
 * compute_array_stats() -- compute statistics for an array column
 *
 * This function computes statistics useful for determining selectivity of
 * the array operators <@, &&, and @>.  It is invoked by ANALYZE via the
 * compute_stats hook after sample rows have been collected.
 *
 * We also invoke the standard compute_stats function, which will compute
 * "scalar" statistics relevant to the btree-style array comparison operators.
 * However, exact duplicates of an entire array may be rare despite many
 * arrays sharing individual elements.  This especially afflicts long arrays,
 * which are also liable to lack all scalar statistics due to the low
 * WIDTH_THRESHOLD used in analyze.c.  So, in addition to the standard stats,
 * we find the most common array elements and compute a histogram of distinct
 * element counts.
 *
 * The algorithm used is Lossy Counting, as proposed in the paper "Approximate
 * frequency counts over data streams" by G. S. Manku and R. Motwani, in
 * Proceedings of the 28th International Conference on Very Large Data Bases,
 * Hong Kong, China, August 2002, section 4.2. The paper is available at
 * http://www.vldb.org/conf/2002/S10P03.pdf
 *
 * The Lossy Counting (aka LC) algorithm goes like this:
 * Let s be the threshold frequency for an item (the minimum frequency we
 * are interested in) and epsilon the error margin for the frequency. Let D
 * be a set of triples (e, f, delta), where e is an element value, f is that
 * element's frequency (actually, its current occurrence count) and delta is
 * the maximum error in f. We start with D empty and process the elements in
 * batches of size w. (The batch size is also known as "bucket size" and is
 * equal to 1/epsilon.) Let the current batch number be b_current, starting
 * with 1. For each element e we either increment its f count, if it's
 * already in D, or insert a new triple into D with values (e, 1, b_current
 * - 1). After processing each batch we prune D, by removing from it all
 * elements with f + delta <= b_current.  After the algorithm finishes we
 * suppress all elements from D that do not satisfy f >= (s - epsilon) * N,
 * where N is the total number of elements in the input.  We emit the
 * remaining elements with estimated frequency f/N.  The LC paper proves
 * that this algorithm finds all elements with true frequency at least s,
 * and that no frequency is overestimated or is underestimated by more than
 * epsilon.  Furthermore, given reasonable assumptions about the input
 * distribution, the required table size is no more than about 7 times w.
 *
 * In the absence of a principled basis for other particular values, we
 * follow ts_typanalyze() and use parameters s = 0.07/K, epsilon = s/10.
 * But we leave out the correction for stopwords, which do not apply to
 * arrays.  These parameters give bucket width w = K/0.007 and maximum
 * expected hashtable size of about 1000 * K.
 *
 * Elements may repeat within an array.  Since duplicates do not change the
 * behavior of <@, && or @>, we want to count each element only once per
 * array.  Therefore, we store in the finished pg_statistic entry each
 * element's frequency as the fraction of all non-null rows that contain it.
 * We divide the raw counts by nonnull_cnt to get those figures.
 */
static void
compute_array_stats(VacAttrStats *stats, AnalyzeAttrFetchFunc fetchfunc,
					int samplerows, double totalrows)
{
	ArrayAnalyzeExtraData *extra_data;
	int			num_mcelem;
	int			null_cnt = 0;
	int			null_elem_cnt = 0;
	int			analyzed_rows = 0;

	/* This is D from the LC algorithm. */
	HTAB	   *elements_tab;
	HASHCTL		elem_hash_ctl;
	HASH_SEQ_STATUS scan_status;

	/* This is the current bucket number from the LC algorithm */
	int			b_current;

	/* This is 'w' from the LC algorithm */
	int			bucket_width;
	int			array_no;
	int64		element_no;
	TrackItem  *item;
	int			slot_idx;
	HTAB	   *count_tab;
	HASHCTL		count_hash_ctl;
	DECountItem *count_item;

	extra_data = (ArrayAnalyzeExtraData *) stats->extra_data;

	/*
	 * Invoke analyze.c's standard analysis function to create scalar-style
	 * stats for the column.  It will expect its own extra_data pointer, so
	 * temporarily install that.
	 */
	stats->extra_data = extra_data->std_extra_data;
	(*extra_data->std_compute_stats) (stats, fetchfunc, samplerows, totalrows);
	stats->extra_data = extra_data;

	/*
	 * Set up static pointer for use by subroutines.  We wait till here in
	 * case std_compute_stats somehow recursively invokes us (probably not
	 * possible, but ...)
	 */
	array_extra_data = extra_data;

	/*
	 * We want statistics_target * 10 elements in the MCELEM array. This
	 * multiplier is pretty arbitrary, but is meant to reflect the fact that
	 * the number of individual elements tracked in pg_statistic ought to be
	 * more than the number of values for a simple scalar column.
	 */
	num_mcelem = stats->attr->attstattarget * 10;

	/*
	 * We set bucket width equal to num_mcelem / 0.007 as per the comment
	 * above.
	 */
	bucket_width = num_mcelem * 1000 / 7;

	/*
	 * Create the hashtable. It will be in local memory, so we don't need to
	 * worry about overflowing the initial size. Also we don't need to pay any
	 * attention to locking and memory management.
	 */
	MemSet(&elem_hash_ctl, 0, sizeof(elem_hash_ctl));
	elem_hash_ctl.keysize = sizeof(Datum);
	elem_hash_ctl.entrysize = sizeof(TrackItem);
	elem_hash_ctl.hash = element_hash;
	elem_hash_ctl.match = element_match;
	elem_hash_ctl.hcxt = CurrentMemoryContext;
	elements_tab = hash_create("Analyzed elements table",
							   num_mcelem,
							   &elem_hash_ctl,
					HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

	/* hashtable for array distinct elements counts */
	MemSet(&count_hash_ctl, 0, sizeof(count_hash_ctl));
	count_hash_ctl.keysize = sizeof(int);
	count_hash_ctl.entrysize = sizeof(DECountItem);
	count_hash_ctl.hcxt = CurrentMemoryContext;
	count_tab = hash_create("Array distinct element count table",
							64,
							&count_hash_ctl,
							HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* Initialize counters. */
	b_current = 1;
	element_no = 0;

	/* Loop over the arrays. */
	for (array_no = 0; array_no < samplerows; array_no++)
	{
		Datum		value;
		bool		isnull;
		ArrayType  *array;
		int			num_elems;
		Datum	   *elem_values;
		bool	   *elem_nulls;
		bool		null_present;
		int			j;
		int64		prev_element_no = element_no;
		int			distinct_count;
		bool		count_item_found;

		vacuum_delay_point();

		value = fetchfunc(stats, array_no, &isnull);
		if (isnull)
		{
			/* array is null, just count that */
			null_cnt++;
			continue;
		}

		/* Skip too-large values. */
		if (toast_raw_datum_size(value) > ARRAY_WIDTH_THRESHOLD)
			continue;
		else
			analyzed_rows++;

		/*
		 * Now detoast the array if needed, and deconstruct into datums.
		 */
		array = DatumGetArrayTypeP(value);

		Assert(ARR_ELEMTYPE(array) == extra_data->type_id);
		deconstruct_array(array,
						  extra_data->type_id,
						  extra_data->typlen,
						  extra_data->typbyval,
						  extra_data->typalign,
						  &elem_values, &elem_nulls, &num_elems);

		/*
		 * We loop through the elements in the array and add them to our
		 * tracking hashtable.
		 */
		null_present = false;
		for (j = 0; j < num_elems; j++)
		{
			Datum		elem_value;
			bool		found;

			/* No null element processing other than flag setting here */
			if (elem_nulls[j])
			{
				null_present = true;
				continue;
			}

			/* Lookup current element in hashtable, adding it if new */
			elem_value = elem_values[j];
			item = (TrackItem *) hash_search(elements_tab,
											 (const void *) &elem_value,
											 HASH_ENTER, &found);

			if (found)
			{
				/* The element value is already on the tracking list */

				/*
				 * The operators we assist ignore duplicate array elements, so
				 * count a given distinct element only once per array.
				 */
				if (item->last_container == array_no)
					continue;

				item->frequency++;
				item->last_container = array_no;
			}
			else
			{
				/* Initialize new tracking list element */

				/*
				 * If element type is pass-by-reference, we must copy it into
				 * palloc'd space, so that we can release the array below. (We
				 * do this so that the space needed for element values is
				 * limited by the size of the hashtable; if we kept all the
				 * array values around, it could be much more.)
				 */
				item->key = datumCopy(elem_value,
									  extra_data->typbyval,
									  extra_data->typlen);

				item->frequency = 1;
				item->delta = b_current - 1;
				item->last_container = array_no;
			}

			/* element_no is the number of elements processed (ie N) */
			element_no++;

			/* We prune the D structure after processing each bucket */
			if (element_no % bucket_width == 0)
			{
				prune_element_hashtable(elements_tab, b_current);
				b_current++;
			}
		}

		/* Count null element presence once per array. */
		if (null_present)
			null_elem_cnt++;

		/* Update frequency of the particular array distinct element count. */
		distinct_count = (int) (element_no - prev_element_no);
		count_item = (DECountItem *) hash_search(count_tab, &distinct_count,
												 HASH_ENTER,
												 &count_item_found);

		if (count_item_found)
			count_item->frequency++;
		else
			count_item->frequency = 1;

		/* Free memory allocated while detoasting. */
		if (PointerGetDatum(array) != value)
			pfree(array);
		pfree(elem_values);
		pfree(elem_nulls);
	}

	/* Skip pg_statistic slots occupied by standard statistics */
	slot_idx = 0;
	while (slot_idx < STATISTIC_NUM_SLOTS && stats->stakind[slot_idx] != 0)
		slot_idx++;
	if (slot_idx > STATISTIC_NUM_SLOTS - 2)
		elog(ERROR, "insufficient pg_statistic slots for array stats");

	/* We can only compute real stats if we found some non-null values. */
	if (analyzed_rows > 0)
	{
		int			nonnull_cnt = analyzed_rows;
		int			count_items_count;
		int			i;
		TrackItem **sort_table;
		int			track_len;
		int64		cutoff_freq;
		int64		minfreq,
					maxfreq;

		/*
		 * We assume the standard stats code already took care of setting
		 * stats_valid, stanullfrac, stawidth, stadistinct.  We'd have to
		 * re-compute those values if we wanted to not store the standard
		 * stats.
		 */

		/*
		 * Construct an array of the interesting hashtable items, that is,
		 * those meeting the cutoff frequency (s - epsilon)*N.  Also identify
		 * the minimum and maximum frequencies among these items.
		 *
		 * Since epsilon = s/10 and bucket_width = 1/epsilon, the cutoff
		 * frequency is 9*N / bucket_width.
		 */
		cutoff_freq = 9 * element_no / bucket_width;

		i = hash_get_num_entries(elements_tab); /* surely enough space */
		sort_table = (TrackItem **) palloc(sizeof(TrackItem *) * i);

		hash_seq_init(&scan_status, elements_tab);
		track_len = 0;
		minfreq = element_no;
		maxfreq = 0;
		while ((item = (TrackItem *) hash_seq_search(&scan_status)) != NULL)
		{
			if (item->frequency > cutoff_freq)
			{
				sort_table[track_len++] = item;
				minfreq = Min(minfreq, item->frequency);
				maxfreq = Max(maxfreq, item->frequency);
			}
		}
		Assert(track_len <= i);

		/* emit some statistics for debug purposes */
		elog(DEBUG3, "compute_array_stats: target # mces = %d, "
			 "bucket width = %d, "
			 "# elements = " INT64_FORMAT ", hashtable size = %d, "
			 "usable entries = %d",
			 num_mcelem, bucket_width, element_no, i, track_len);

		/*
		 * If we obtained more elements than we really want, get rid of those
		 * with least frequencies.  The easiest way is to qsort the array into
		 * descending frequency order and truncate the array.
		 */
		if (num_mcelem < track_len)
		{
			qsort(sort_table, track_len, sizeof(TrackItem *),
				  trackitem_compare_frequencies_desc);
			/* reset minfreq to the smallest frequency we're keeping */
			minfreq = sort_table[num_mcelem - 1]->frequency;
		}
		else
			num_mcelem = track_len;

		/* Generate MCELEM slot entry */
		if (num_mcelem > 0)
		{
			MemoryContext old_context;
			Datum	   *mcelem_values;
			float4	   *mcelem_freqs;

			/*
			 * We want to store statistics sorted on the element value using
			 * the element type's default comparison function.  This permits
			 * fast binary searches in selectivity estimation functions.
			 */
			qsort(sort_table, num_mcelem, sizeof(TrackItem *),
				  trackitem_compare_element);

			/* Must copy the target values into anl_context */
			old_context = MemoryContextSwitchTo(stats->anl_context);

			/*
			 * We sorted statistics on the element value, but we want to be
			 * able to find the minimal and maximal frequencies without going
			 * through all the values.  We also want the frequency of null
			 * elements.  Store these three values at the end of mcelem_freqs.
			 */
			mcelem_values = (Datum *) palloc(num_mcelem * sizeof(Datum));
			mcelem_freqs = (float4 *) palloc((num_mcelem + 3) * sizeof(float4));

			/*
			 * See comments above about use of nonnull_cnt as the divisor for
			 * the final frequency estimates.
			 */
			for (i = 0; i < num_mcelem; i++)
			{
				TrackItem  *item = sort_table[i];

				mcelem_values[i] = datumCopy(item->key,
											 extra_data->typbyval,
											 extra_data->typlen);
				mcelem_freqs[i] = (double) item->frequency /
					(double) nonnull_cnt;
			}
			mcelem_freqs[i++] = (double) minfreq / (double) nonnull_cnt;
			mcelem_freqs[i++] = (double) maxfreq / (double) nonnull_cnt;
			mcelem_freqs[i++] = (double) null_elem_cnt / (double) nonnull_cnt;

			MemoryContextSwitchTo(old_context);

			stats->stakind[slot_idx] = STATISTIC_KIND_MCELEM;
			stats->staop[slot_idx] = extra_data->eq_opr;
			stats->stanumbers[slot_idx] = mcelem_freqs;
			/* See above comment about extra stanumber entries */
			stats->numnumbers[slot_idx] = num_mcelem + 3;
			stats->stavalues[slot_idx] = mcelem_values;
			stats->numvalues[slot_idx] = num_mcelem;
			/* We are storing values of element type */
			stats->statypid[slot_idx] = extra_data->type_id;
			stats->statyplen[slot_idx] = extra_data->typlen;
			stats->statypbyval[slot_idx] = extra_data->typbyval;
			stats->statypalign[slot_idx] = extra_data->typalign;
			slot_idx++;
		}

		/* Generate DECHIST slot entry */
		count_items_count = hash_get_num_entries(count_tab);
		if (count_items_count > 0)
		{
			int			num_hist = stats->attr->attstattarget;
			DECountItem **sorted_count_items;
			int			j;
			int			delta;
			int64		frac;
			float4	   *hist;

			/* num_hist must be at least 2 for the loop below to work */
			num_hist = Max(num_hist, 2);

			/*
			 * Create an array of DECountItem pointers, and sort them into
			 * increasing count order.
			 */
			sorted_count_items = (DECountItem **)
				palloc(sizeof(DECountItem *) * count_items_count);
			hash_seq_init(&scan_status, count_tab);
			j = 0;
			while ((count_item = (DECountItem *) hash_seq_search(&scan_status)) != NULL)
			{
				sorted_count_items[j++] = count_item;
			}
			qsort(sorted_count_items, count_items_count,
				  sizeof(DECountItem *), countitem_compare_count);

			/*
			 * Prepare to fill stanumbers with the histogram, followed by the
			 * average count.  This array must be stored in anl_context.
			 */
			hist = (float4 *)
				MemoryContextAlloc(stats->anl_context,
								   sizeof(float4) * (num_hist + 1));
			hist[num_hist] = (double) element_no / (double) nonnull_cnt;

			/*----------
			 * Construct the histogram of distinct-element counts (DECs).
			 *
			 * The object of this loop is to copy the min and max DECs to
			 * hist[0] and hist[num_hist - 1], along with evenly-spaced DECs
			 * in between (where "evenly-spaced" is with reference to the
			 * whole input population of arrays).  If we had a complete sorted
			 * array of DECs, one per analyzed row, the i'th hist value would
			 * come from DECs[i * (analyzed_rows - 1) / (num_hist - 1)]
			 * (compare the histogram-making loop in compute_scalar_stats()).
			 * But instead of that we have the sorted_count_items[] array,
			 * which holds unique DEC values with their frequencies (that is,
			 * a run-length-compressed version of the full array).  So we
			 * control advancing through sorted_count_items[] with the
			 * variable "frac", which is defined as (x - y) * (num_hist - 1),
			 * where x is the index in the notional DECs array corresponding
			 * to the start of the next sorted_count_items[] element's run,
			 * and y is the index in DECs from which we should take the next
			 * histogram value.  We have to advance whenever x <= y, that is
			 * frac <= 0.  The x component is the sum of the frequencies seen
			 * so far (up through the current sorted_count_items[] element),
			 * and of course y * (num_hist - 1) = i * (analyzed_rows - 1),
			 * per the subscript calculation above.  (The subscript calculation
			 * implies dropping any fractional part of y; in this formulation
			 * that's handled by not advancing until frac reaches 1.)
			 *
			 * Even though frac has a bounded range, it could overflow int32
			 * when working with very large statistics targets, so we do that
			 * math in int64.
			 *----------
			 */
			delta = analyzed_rows - 1;
			j = 0;				/* current index in sorted_count_items */
			/* Initialize frac for sorted_count_items[0]; y is initially 0 */
			frac = (int64) sorted_count_items[0]->frequency * (num_hist - 1);
			for (i = 0; i < num_hist; i++)
			{
				while (frac <= 0)
				{
					/* Advance, and update x component of frac */
					j++;
					frac += (int64) sorted_count_items[j]->frequency * (num_hist - 1);
				}
				hist[i] = sorted_count_items[j]->count;
				frac -= delta;	/* update y for upcoming i increment */
			}
			Assert(j == count_items_count - 1);

			stats->stakind[slot_idx] = STATISTIC_KIND_DECHIST;
			stats->staop[slot_idx] = extra_data->eq_opr;
			stats->stanumbers[slot_idx] = hist;
			stats->numnumbers[slot_idx] = num_hist + 1;
			slot_idx++;
		}
	}

	/*
	 * We don't need to bother cleaning up any of our temporary palloc's. The
	 * hashtable should also go away, as it used a child memory context.
	 */
}

/*
 * A function to prune the D structure from the Lossy Counting algorithm.
 * Consult compute_tsvector_stats() for wider explanation.
 */
static void
prune_element_hashtable(HTAB *elements_tab, int b_current)
{
	HASH_SEQ_STATUS scan_status;
	TrackItem  *item;

	hash_seq_init(&scan_status, elements_tab);
	while ((item = (TrackItem *) hash_seq_search(&scan_status)) != NULL)
	{
		if (item->frequency + item->delta <= b_current)
		{
			Datum		value = item->key;

			if (hash_search(elements_tab, (const void *) &item->key,
							HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "hash table corrupted");
			/* We should free memory if element is not passed by value */
			if (!array_extra_data->typbyval)
				pfree(DatumGetPointer(value));
		}
	}
}

/*
 * Hash function for elements.
 *
 * We use the element type's default hash opclass, and the default collation
 * if the type is collation-sensitive.
 */
static uint32
element_hash(const void *key, Size keysize)
{
	Datum		d = *((const Datum *) key);
	Datum		h;

	h = FunctionCall1Coll(array_extra_data->hash, DEFAULT_COLLATION_OID, d);
	return DatumGetUInt32(h);
}

/*
 * Matching function for elements, to be used in hashtable lookups.
 */
static int
element_match(const void *key1, const void *key2, Size keysize)
{
	/* The keysize parameter is superfluous here */
	return element_compare(key1, key2);
}

/*
 * Comparison function for elements.
 *
 * We use the element type's default btree opclass, and the default collation
 * if the type is collation-sensitive.
 *
 * XXX consider using SortSupport infrastructure
 */
static int
element_compare(const void *key1, const void *key2)
{
	Datum		d1 = *((const Datum *) key1);
	Datum		d2 = *((const Datum *) key2);
	Datum		c;

	c = FunctionCall2Coll(array_extra_data->cmp, DEFAULT_COLLATION_OID, d1, d2);
	return DatumGetInt32(c);
}

/*
 * qsort() comparator for sorting TrackItems by frequencies (descending sort)
 */
static int
trackitem_compare_frequencies_desc(const void *e1, const void *e2)
{
	const TrackItem *const * t1 = (const TrackItem *const *) e1;
	const TrackItem *const * t2 = (const TrackItem *const *) e2;

	return (*t2)->frequency - (*t1)->frequency;
}

/*
 * qsort() comparator for sorting TrackItems by element values
 */
static int
trackitem_compare_element(const void *e1, const void *e2)
{
	const TrackItem *const * t1 = (const TrackItem *const *) e1;
	const TrackItem *const * t2 = (const TrackItem *const *) e2;

	return element_compare(&(*t1)->key, &(*t2)->key);
}

/*
 * qsort() comparator for sorting DECountItems by count
 */
static int
countitem_compare_count(const void *e1, const void *e2)
{
	const DECountItem *const * t1 = (const DECountItem *const *) e1;
	const DECountItem *const * t2 = (const DECountItem *const *) e2;

	if ((*t1)->count < (*t2)->count)
		return -1;
	else if ((*t1)->count == (*t2)->count)
		return 0;
	else
		return 1;
}
