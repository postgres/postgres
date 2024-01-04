/*-------------------------------------------------------------------------
 *
 * ts_typanalyze.c
 *	  functions for gathering statistics from tsvector columns
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/ts_typanalyze.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "commands/vacuum.h"
#include "common/hashfn.h"
#include "tsearch/ts_type.h"
#include "utils/builtins.h"
#include "varatt.h"


/* A hash key for lexemes */
typedef struct
{
	char	   *lexeme;			/* lexeme (not NULL terminated!) */
	int			length;			/* its length in bytes */
} LexemeHashKey;

/* A hash table entry for the Lossy Counting algorithm */
typedef struct
{
	LexemeHashKey key;			/* This is 'e' from the LC algorithm. */
	int			frequency;		/* This is 'f'. */
	int			delta;			/* And this is 'delta'. */
} TrackItem;

static void compute_tsvector_stats(VacAttrStats *stats,
								   AnalyzeAttrFetchFunc fetchfunc,
								   int samplerows,
								   double totalrows);
static void prune_lexemes_hashtable(HTAB *lexemes_tab, int b_current);
static uint32 lexeme_hash(const void *key, Size keysize);
static int	lexeme_match(const void *key1, const void *key2, Size keysize);
static int	lexeme_compare(const void *key1, const void *key2);
static int	trackitem_compare_frequencies_desc(const void *e1, const void *e2,
											   void *arg);
static int	trackitem_compare_lexemes(const void *e1, const void *e2,
									  void *arg);


/*
 *	ts_typanalyze -- a custom typanalyze function for tsvector columns
 */
Datum
ts_typanalyze(PG_FUNCTION_ARGS)
{
	VacAttrStats *stats = (VacAttrStats *) PG_GETARG_POINTER(0);

	/* If the attstattarget column is negative, use the default value */
	if (stats->attstattarget < 0)
		stats->attstattarget = default_statistics_target;

	stats->compute_stats = compute_tsvector_stats;
	/* see comment about the choice of minrows in commands/analyze.c */
	stats->minrows = 300 * stats->attstattarget;

	PG_RETURN_BOOL(true);
}

/*
 *	compute_tsvector_stats() -- compute statistics for a tsvector column
 *
 *	This functions computes statistics that are useful for determining @@
 *	operations' selectivity, along with the fraction of non-null rows and
 *	average width.
 *
 *	Instead of finding the most common values, as we do for most datatypes,
 *	we're looking for the most common lexemes. This is more useful, because
 *	there most probably won't be any two rows with the same tsvector and thus
 *	the notion of a MCV is a bit bogus with this datatype. With a list of the
 *	most common lexemes we can do a better job at figuring out @@ selectivity.
 *
 *	For the same reasons we assume that tsvector columns are unique when
 *	determining the number of distinct values.
 *
 *	The algorithm used is Lossy Counting, as proposed in the paper "Approximate
 *	frequency counts over data streams" by G. S. Manku and R. Motwani, in
 *	Proceedings of the 28th International Conference on Very Large Data Bases,
 *	Hong Kong, China, August 2002, section 4.2. The paper is available at
 *	http://www.vldb.org/conf/2002/S10P03.pdf
 *
 *	The Lossy Counting (aka LC) algorithm goes like this:
 *	Let s be the threshold frequency for an item (the minimum frequency we
 *	are interested in) and epsilon the error margin for the frequency. Let D
 *	be a set of triples (e, f, delta), where e is an element value, f is that
 *	element's frequency (actually, its current occurrence count) and delta is
 *	the maximum error in f. We start with D empty and process the elements in
 *	batches of size w. (The batch size is also known as "bucket size" and is
 *	equal to 1/epsilon.) Let the current batch number be b_current, starting
 *	with 1. For each element e we either increment its f count, if it's
 *	already in D, or insert a new triple into D with values (e, 1, b_current
 *	- 1). After processing each batch we prune D, by removing from it all
 *	elements with f + delta <= b_current.  After the algorithm finishes we
 *	suppress all elements from D that do not satisfy f >= (s - epsilon) * N,
 *	where N is the total number of elements in the input.  We emit the
 *	remaining elements with estimated frequency f/N.  The LC paper proves
 *	that this algorithm finds all elements with true frequency at least s,
 *	and that no frequency is overestimated or is underestimated by more than
 *	epsilon.  Furthermore, given reasonable assumptions about the input
 *	distribution, the required table size is no more than about 7 times w.
 *
 *	We set s to be the estimated frequency of the K'th word in a natural
 *	language's frequency table, where K is the target number of entries in
 *	the MCELEM array plus an arbitrary constant, meant to reflect the fact
 *	that the most common words in any language would usually be stopwords
 *	so we will not actually see them in the input.  We assume that the
 *	distribution of word frequencies (including the stopwords) follows Zipf's
 *	law with an exponent of 1.
 *
 *	Assuming Zipfian distribution, the frequency of the K'th word is equal
 *	to 1/(K * H(W)) where H(n) is 1/2 + 1/3 + ... + 1/n and W is the number of
 *	words in the language.  Putting W as one million, we get roughly 0.07/K.
 *	Assuming top 10 words are stopwords gives s = 0.07/(K + 10).  We set
 *	epsilon = s/10, which gives bucket width w = (K + 10)/0.007 and
 *	maximum expected hashtable size of about 1000 * (K + 10).
 *
 *	Note: in the above discussion, s, epsilon, and f/N are in terms of a
 *	lexeme's frequency as a fraction of all lexemes seen in the input.
 *	However, what we actually want to store in the finished pg_statistic
 *	entry is each lexeme's frequency as a fraction of all rows that it occurs
 *	in.  Assuming that the input tsvectors are correctly constructed, no
 *	lexeme occurs more than once per tsvector, so the final count f is a
 *	correct estimate of the number of input tsvectors it occurs in, and we
 *	need only change the divisor from N to nonnull_cnt to get the number we
 *	want.
 */
static void
compute_tsvector_stats(VacAttrStats *stats,
					   AnalyzeAttrFetchFunc fetchfunc,
					   int samplerows,
					   double totalrows)
{
	int			num_mcelem;
	int			null_cnt = 0;
	double		total_width = 0;

	/* This is D from the LC algorithm. */
	HTAB	   *lexemes_tab;
	HASHCTL		hash_ctl;
	HASH_SEQ_STATUS scan_status;

	/* This is the current bucket number from the LC algorithm */
	int			b_current;

	/* This is 'w' from the LC algorithm */
	int			bucket_width;
	int			vector_no,
				lexeme_no;
	LexemeHashKey hash_key;

	/*
	 * We want statistics_target * 10 lexemes in the MCELEM array.  This
	 * multiplier is pretty arbitrary, but is meant to reflect the fact that
	 * the number of individual lexeme values tracked in pg_statistic ought to
	 * be more than the number of values for a simple scalar column.
	 */
	num_mcelem = stats->attstattarget * 10;

	/*
	 * We set bucket width equal to (num_mcelem + 10) / 0.007 as per the
	 * comment above.
	 */
	bucket_width = (num_mcelem + 10) * 1000 / 7;

	/*
	 * Create the hashtable. It will be in local memory, so we don't need to
	 * worry about overflowing the initial size. Also we don't need to pay any
	 * attention to locking and memory management.
	 */
	hash_ctl.keysize = sizeof(LexemeHashKey);
	hash_ctl.entrysize = sizeof(TrackItem);
	hash_ctl.hash = lexeme_hash;
	hash_ctl.match = lexeme_match;
	hash_ctl.hcxt = CurrentMemoryContext;
	lexemes_tab = hash_create("Analyzed lexemes table",
							  num_mcelem,
							  &hash_ctl,
							  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

	/* Initialize counters. */
	b_current = 1;
	lexeme_no = 0;

	/* Loop over the tsvectors. */
	for (vector_no = 0; vector_no < samplerows; vector_no++)
	{
		Datum		value;
		bool		isnull;
		TSVector	vector;
		WordEntry  *curentryptr;
		char	   *lexemesptr;
		int			j;

		vacuum_delay_point();

		value = fetchfunc(stats, vector_no, &isnull);

		/*
		 * Check for null/nonnull.
		 */
		if (isnull)
		{
			null_cnt++;
			continue;
		}

		/*
		 * Add up widths for average-width calculation.  Since it's a
		 * tsvector, we know it's varlena.  As in the regular
		 * compute_minimal_stats function, we use the toasted width for this
		 * calculation.
		 */
		total_width += VARSIZE_ANY(DatumGetPointer(value));

		/*
		 * Now detoast the tsvector if needed.
		 */
		vector = DatumGetTSVector(value);

		/*
		 * We loop through the lexemes in the tsvector and add them to our
		 * tracking hashtable.
		 */
		lexemesptr = STRPTR(vector);
		curentryptr = ARRPTR(vector);
		for (j = 0; j < vector->size; j++)
		{
			TrackItem  *item;
			bool		found;

			/*
			 * Construct a hash key.  The key points into the (detoasted)
			 * tsvector value at this point, but if a new entry is created, we
			 * make a copy of it.  This way we can free the tsvector value
			 * once we've processed all its lexemes.
			 */
			hash_key.lexeme = lexemesptr + curentryptr->pos;
			hash_key.length = curentryptr->len;

			/* Lookup current lexeme in hashtable, adding it if new */
			item = (TrackItem *) hash_search(lexemes_tab,
											 &hash_key,
											 HASH_ENTER, &found);

			if (found)
			{
				/* The lexeme is already on the tracking list */
				item->frequency++;
			}
			else
			{
				/* Initialize new tracking list element */
				item->frequency = 1;
				item->delta = b_current - 1;

				item->key.lexeme = palloc(hash_key.length);
				memcpy(item->key.lexeme, hash_key.lexeme, hash_key.length);
			}

			/* lexeme_no is the number of elements processed (ie N) */
			lexeme_no++;

			/* We prune the D structure after processing each bucket */
			if (lexeme_no % bucket_width == 0)
			{
				prune_lexemes_hashtable(lexemes_tab, b_current);
				b_current++;
			}

			/* Advance to the next WordEntry in the tsvector */
			curentryptr++;
		}

		/* If the vector was toasted, free the detoasted copy. */
		if (TSVectorGetDatum(vector) != value)
			pfree(vector);
	}

	/* We can only compute real stats if we found some non-null values. */
	if (null_cnt < samplerows)
	{
		int			nonnull_cnt = samplerows - null_cnt;
		int			i;
		TrackItem **sort_table;
		TrackItem  *item;
		int			track_len;
		int			cutoff_freq;
		int			minfreq,
					maxfreq;

		stats->stats_valid = true;
		/* Do the simple null-frac and average width stats */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		stats->stawidth = total_width / (double) nonnull_cnt;

		/* Assume it's a unique column (see notes above) */
		stats->stadistinct = -1.0 * (1.0 - stats->stanullfrac);

		/*
		 * Construct an array of the interesting hashtable items, that is,
		 * those meeting the cutoff frequency (s - epsilon)*N.  Also identify
		 * the minimum and maximum frequencies among these items.
		 *
		 * Since epsilon = s/10 and bucket_width = 1/epsilon, the cutoff
		 * frequency is 9*N / bucket_width.
		 */
		cutoff_freq = 9 * lexeme_no / bucket_width;

		i = hash_get_num_entries(lexemes_tab);	/* surely enough space */
		sort_table = (TrackItem **) palloc(sizeof(TrackItem *) * i);

		hash_seq_init(&scan_status, lexemes_tab);
		track_len = 0;
		minfreq = lexeme_no;
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
		elog(DEBUG3, "tsvector_stats: target # mces = %d, bucket width = %d, "
			 "# lexemes = %d, hashtable size = %d, usable entries = %d",
			 num_mcelem, bucket_width, lexeme_no, i, track_len);

		/*
		 * If we obtained more lexemes than we really want, get rid of those
		 * with least frequencies.  The easiest way is to qsort the array into
		 * descending frequency order and truncate the array.
		 */
		if (num_mcelem < track_len)
		{
			qsort_interruptible(sort_table, track_len, sizeof(TrackItem *),
								trackitem_compare_frequencies_desc, NULL);
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
			 * We want to store statistics sorted on the lexeme value using
			 * first length, then byte-for-byte comparison. The reason for
			 * doing length comparison first is that we don't care about the
			 * ordering so long as it's consistent, and comparing lengths
			 * first gives us a chance to avoid a strncmp() call.
			 *
			 * This is different from what we do with scalar statistics --
			 * they get sorted on frequencies. The rationale is that we
			 * usually search through most common elements looking for a
			 * specific value, so we can grab its frequency.  When values are
			 * presorted we can employ binary search for that.  See
			 * ts_selfuncs.c for a real usage scenario.
			 */
			qsort_interruptible(sort_table, num_mcelem, sizeof(TrackItem *),
								trackitem_compare_lexemes, NULL);

			/* Must copy the target values into anl_context */
			old_context = MemoryContextSwitchTo(stats->anl_context);

			/*
			 * We sorted statistics on the lexeme value, but we want to be
			 * able to find out the minimal and maximal frequency without
			 * going through all the values.  We keep those two extra
			 * frequencies in two extra cells in mcelem_freqs.
			 *
			 * (Note: the MCELEM statistics slot definition allows for a third
			 * extra number containing the frequency of nulls, but we don't
			 * create that for a tsvector column, since null elements aren't
			 * possible.)
			 */
			mcelem_values = (Datum *) palloc(num_mcelem * sizeof(Datum));
			mcelem_freqs = (float4 *) palloc((num_mcelem + 2) * sizeof(float4));

			/*
			 * See comments above about use of nonnull_cnt as the divisor for
			 * the final frequency estimates.
			 */
			for (i = 0; i < num_mcelem; i++)
			{
				TrackItem  *titem = sort_table[i];

				mcelem_values[i] =
					PointerGetDatum(cstring_to_text_with_len(titem->key.lexeme,
															 titem->key.length));
				mcelem_freqs[i] = (double) titem->frequency / (double) nonnull_cnt;
			}
			mcelem_freqs[i++] = (double) minfreq / (double) nonnull_cnt;
			mcelem_freqs[i] = (double) maxfreq / (double) nonnull_cnt;
			MemoryContextSwitchTo(old_context);

			stats->stakind[0] = STATISTIC_KIND_MCELEM;
			stats->staop[0] = TextEqualOperator;
			stats->stacoll[0] = DEFAULT_COLLATION_OID;
			stats->stanumbers[0] = mcelem_freqs;
			/* See above comment about two extra frequency fields */
			stats->numnumbers[0] = num_mcelem + 2;
			stats->stavalues[0] = mcelem_values;
			stats->numvalues[0] = num_mcelem;
			/* We are storing text values */
			stats->statypid[0] = TEXTOID;
			stats->statyplen[0] = -1;	/* typlen, -1 for varlena */
			stats->statypbyval[0] = false;
			stats->statypalign[0] = 'i';
		}
	}
	else
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

/*
 *	A function to prune the D structure from the Lossy Counting algorithm.
 *	Consult compute_tsvector_stats() for wider explanation.
 */
static void
prune_lexemes_hashtable(HTAB *lexemes_tab, int b_current)
{
	HASH_SEQ_STATUS scan_status;
	TrackItem  *item;

	hash_seq_init(&scan_status, lexemes_tab);
	while ((item = (TrackItem *) hash_seq_search(&scan_status)) != NULL)
	{
		if (item->frequency + item->delta <= b_current)
		{
			char	   *lexeme = item->key.lexeme;

			if (hash_search(lexemes_tab, &item->key,
							HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "hash table corrupted");
			pfree(lexeme);
		}
	}
}

/*
 * Hash functions for lexemes. They are strings, but not NULL terminated,
 * so we need a special hash function.
 */
static uint32
lexeme_hash(const void *key, Size keysize)
{
	const LexemeHashKey *l = (const LexemeHashKey *) key;

	return DatumGetUInt32(hash_any((const unsigned char *) l->lexeme,
								   l->length));
}

/*
 *	Matching function for lexemes, to be used in hashtable lookups.
 */
static int
lexeme_match(const void *key1, const void *key2, Size keysize)
{
	/* The keysize parameter is superfluous, the keys store their lengths */
	return lexeme_compare(key1, key2);
}

/*
 *	Comparison function for lexemes.
 */
static int
lexeme_compare(const void *key1, const void *key2)
{
	const LexemeHashKey *d1 = (const LexemeHashKey *) key1;
	const LexemeHashKey *d2 = (const LexemeHashKey *) key2;

	/* First, compare by length */
	if (d1->length > d2->length)
		return 1;
	else if (d1->length < d2->length)
		return -1;
	/* Lengths are equal, do a byte-by-byte comparison */
	return strncmp(d1->lexeme, d2->lexeme, d1->length);
}

/*
 *	Comparator for sorting TrackItems on frequencies (descending sort)
 */
static int
trackitem_compare_frequencies_desc(const void *e1, const void *e2, void *arg)
{
	const TrackItem *const *t1 = (const TrackItem *const *) e1;
	const TrackItem *const *t2 = (const TrackItem *const *) e2;

	return (*t2)->frequency - (*t1)->frequency;
}

/*
 *	Comparator for sorting TrackItems on lexemes
 */
static int
trackitem_compare_lexemes(const void *e1, const void *e2, void *arg)
{
	const TrackItem *const *t1 = (const TrackItem *const *) e1;
	const TrackItem *const *t2 = (const TrackItem *const *) e2;

	return lexeme_compare(&(*t1)->key, &(*t2)->key);
}
