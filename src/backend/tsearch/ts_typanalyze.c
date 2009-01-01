/*-------------------------------------------------------------------------
 *
 * ts_typanalyze.c
 *	  functions for gathering statistics from tsvector columns
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/tsearch/ts_typanalyze.c,v 1.6 2009/01/01 17:23:48 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "catalog/pg_operator.h"
#include "commands/vacuum.h"
#include "tsearch/ts_type.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"


/* A hash key for lexemes */
typedef struct
{
	char	   *lexeme;			/* lexeme (not NULL terminated!) */
	int			length;			/* its length in bytes */
} LexemeHashKey;

/* A hash table entry for the Lossy Counting algorithm */
typedef struct
{
	LexemeHashKey	key;		/* This is 'e' from the LC algorithm. */
	int				frequency;	/* This is 'f'. */
	int				delta;		/* And this is 'delta'. */
} TrackItem;

static void compute_tsvector_stats(VacAttrStats *stats,
								   AnalyzeAttrFetchFunc fetchfunc,
								   int samplerows,
								   double totalrows);
static void prune_lexemes_hashtable(HTAB *lexemes_tab, int b_current);
static uint32 lexeme_hash(const void *key, Size keysize);
static int lexeme_match(const void *key1, const void *key2, Size keysize);
static int lexeme_compare(const void *key1, const void *key2);
static int trackitem_compare_frequencies_desc(const void *e1, const void *e2);
static int trackitem_compare_lexemes(const void *e1, const void *e2);


/*
 *	ts_typanalyze -- a custom typanalyze function for tsvector columns
 */
Datum
ts_typanalyze(PG_FUNCTION_ARGS)
{
	VacAttrStats *stats = (VacAttrStats *) PG_GETARG_POINTER(0);
	Form_pg_attribute attr = stats->attr;

	/* If the attstattarget column is negative, use the default value */
	/* NB: it is okay to scribble on stats->attr since it's a copy */
	if (attr->attstattarget < 0)
		attr->attstattarget = default_statistics_target;

	stats->compute_stats = compute_tsvector_stats;
	/* see comment about the choice of minrows in commands/analyze.c */
	stats->minrows = 300 * attr->attstattarget;

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
 *	Let D be a set of triples (e, f, d), where e is an element value, f is
 *	that element's frequency (occurrence count) and d is the maximum error in
 *	f.  We start with D empty and process the elements in batches of size
 *	w. (The batch size is also known as "bucket size".) Let the current batch
 *	number be b_current, starting with 1. For each element e we either
 *	increment its f count, if it's already in D, or insert a new triple into D
 *	with values (e, 1, b_current - 1). After processing each batch we prune D,
 *	by removing from it all elements with f + d <= b_current. Finally, we
 *	gather elements with largest f.  The LC paper proves error bounds on f
 *	dependent on the batch size w, and shows that the required table size
 *	is no more than a few times w.
 *
 *	We use a hashtable for the D structure and a bucket width of
 *	statistics_target * 10, where 10 is an arbitrarily chosen constant,
 *	meant to approximate the number of lexemes in a single tsvector.
 */
static void
compute_tsvector_stats(VacAttrStats *stats,
					   AnalyzeAttrFetchFunc fetchfunc,
					   int samplerows,
					   double totalrows)
{
	int				num_mcelem;
	int				null_cnt = 0;
	double			total_width = 0;
	/* This is D from the LC algorithm. */
	HTAB			*lexemes_tab;
	HASHCTL			hash_ctl;
	HASH_SEQ_STATUS	scan_status;
	/* This is the current bucket number from the LC algorithm */
	int				b_current;
	/* This is 'w' from the LC algorithm */
	int				bucket_width;
	int vector_no,
		lexeme_no;
	LexemeHashKey 	hash_key;
	TrackItem		*item;

	/* We want statistics_target * 10 lexemes in the MCELEM array */
	num_mcelem = stats->attr->attstattarget * 10;

	/*
	 * We set bucket width equal to the target number of result lexemes.
	 * This is probably about right but perhaps might need to be scaled
	 * up or down a bit?
	 */
	bucket_width = num_mcelem;

	/*
	 * Create the hashtable. It will be in local memory, so we don't need to
	 * worry about initial size too much. Also we don't need to pay any
	 * attention to locking and memory management.
	 */
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(LexemeHashKey);
	hash_ctl.entrysize = sizeof(TrackItem);
	hash_ctl.hash = lexeme_hash;
	hash_ctl.match = lexeme_match;
	hash_ctl.hcxt = CurrentMemoryContext;
	lexemes_tab = hash_create("Analyzed lexemes table",
							  bucket_width * 4,
							  &hash_ctl,
							  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

	/* Initialize counters. */
	b_current = 1;
	lexeme_no = 1;

	/* Loop over the tsvectors. */
	for (vector_no = 0; vector_no < samplerows; vector_no++)
	{
		Datum		value;
		bool		isnull;
		TSVector 	vector;
		WordEntry	*curentryptr;
		char		*lexemesptr;
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
		 * tracking hashtable.  Note: the hashtable entries will point into
		 * the (detoasted) tsvector value, therefore we cannot free that
		 * storage until we're done.
		 */
		lexemesptr = STRPTR(vector);
		curentryptr = ARRPTR(vector);
		for (j = 0; j < vector->size; j++)
		{
			bool			found;

			/* Construct a hash key */
			hash_key.lexeme = lexemesptr + curentryptr->pos;
			hash_key.length = curentryptr->len;

			/* Lookup current lexeme in hashtable, adding it if new */
			item = (TrackItem *) hash_search(lexemes_tab,
											 (const void *) &hash_key,
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
			}

			/* We prune the D structure after processing each bucket */
			if (lexeme_no % bucket_width == 0)
			{
				prune_lexemes_hashtable(lexemes_tab, b_current);
				b_current++;
			}

			/* Advance to the next WordEntry in the tsvector */
			lexeme_no++;
			curentryptr++;
		}
	}

	/* We can only compute real stats if we found some non-null values. */
	if (null_cnt < samplerows)
	{
		int			nonnull_cnt = samplerows - null_cnt;
		int			i;
		TrackItem	**sort_table;
		int			track_len;
		int			minfreq, maxfreq;

		stats->stats_valid = true;
		/* Do the simple null-frac and average width stats */
		stats->stanullfrac = (double) null_cnt / (double) samplerows;
		stats->stawidth = total_width / (double) nonnull_cnt;

		/* Assume it's a unique column (see notes above) */
		stats->stadistinct = -1.0;

		/*
		 * Determine the top-N lexemes by simply copying pointers from the
		 * hashtable into an array and applying qsort()
		 */
		track_len = hash_get_num_entries(lexemes_tab);

		sort_table = (TrackItem **) palloc(sizeof(TrackItem *) * track_len);

		hash_seq_init(&scan_status, lexemes_tab);
		i = 0;
		while ((item = (TrackItem *) hash_seq_search(&scan_status)) != NULL)
		{
			sort_table[i++] = item;
		}
		Assert(i == track_len);

		qsort(sort_table, track_len, sizeof(TrackItem *),
			  trackitem_compare_frequencies_desc);

		/* Suppress any single-occurrence items */
		while (track_len > 0)
		{
			if (sort_table[track_len-1]->frequency > 1)
				break;
			track_len--;
		}

		/* Determine the number of most common lexemes to be stored */
		if (num_mcelem > track_len)
			num_mcelem = track_len;

		/* Generate MCELEM slot entry */
		if (num_mcelem > 0)
		{
			MemoryContext	old_context;
			Datum			*mcelem_values;
			float4			*mcelem_freqs;

			/* Grab the minimal and maximal frequencies that will get stored */
			minfreq = sort_table[num_mcelem - 1]->frequency;
			maxfreq = sort_table[0]->frequency;

			/*
			 * We want to store statistics sorted on the lexeme value using
			 * first length, then byte-for-byte comparison. The reason for
			 * doing length comparison first is that we don't care about the
			 * ordering so long as it's consistent, and comparing lengths first
			 * gives us a chance to avoid a strncmp() call.
			 *
			 * This is different from what we do with scalar statistics -- they
			 * get sorted on frequencies. The rationale is that we usually
			 * search through most common elements looking for a specific
			 * value, so we can grab its frequency.  When values are presorted
			 * we can employ binary search for that.  See ts_selfuncs.c for a
			 * real usage scenario.
			 */
			qsort(sort_table, num_mcelem, sizeof(TrackItem *),
				  trackitem_compare_lexemes);

			/* Must copy the target values into anl_context */
			old_context = MemoryContextSwitchTo(stats->anl_context);

			/*
			 * We sorted statistics on the lexeme value, but we want to be
			 * able to find out the minimal and maximal frequency without
			 * going through all the values.  We keep those two extra
			 * frequencies in two extra cells in mcelem_freqs.
			 */
			mcelem_values = (Datum *) palloc(num_mcelem * sizeof(Datum));
			mcelem_freqs = (float4 *) palloc((num_mcelem + 2) * sizeof(float4));

			for (i = 0; i < num_mcelem; i++)
			{
				TrackItem *item = sort_table[i];

				mcelem_values[i] =
					PointerGetDatum(cstring_to_text_with_len(item->key.lexeme,
															 item->key.length));
				mcelem_freqs[i] = (double) item->frequency / (double) nonnull_cnt;
			}
			mcelem_freqs[i++] = (double) minfreq / (double) nonnull_cnt;
			mcelem_freqs[i] = (double) maxfreq / (double) nonnull_cnt;
			MemoryContextSwitchTo(old_context);

			stats->stakind[0] = STATISTIC_KIND_MCELEM;
			stats->staop[0] = TextEqualOperator;
			stats->stanumbers[0] = mcelem_freqs;
			/* See above comment about two extra frequency fields */
			stats->numnumbers[0] = num_mcelem + 2;
			stats->stavalues[0] = mcelem_values;
			stats->numvalues[0] = num_mcelem;
			/* We are storing text values */
			stats->statypid[0] = TEXTOID;
			stats->statyplen[0] = -1; /* typlen, -1 for varlena */
			stats->statypbyval[0] = false;
			stats->statypalign[0] = 'i';
		}
	}
	else
	{
		/* We found only nulls; assume the column is entirely null */
		stats->stats_valid = true;
		stats->stanullfrac = 1.0;
		stats->stawidth = 0;		/* "unknown" */
		stats->stadistinct = 0.0;	/* "unknown" */
	}

	/*
	 * We don't need to bother cleaning up any of our temporary palloc's.
	 * The hashtable should also go away, as it used a child memory context.
	 */
}

/*
 *	A function to prune the D structure from the Lossy Counting algorithm.
 *	Consult compute_tsvector_stats() for wider explanation.
 */
static void
prune_lexemes_hashtable(HTAB *lexemes_tab, int b_current)
{
	HASH_SEQ_STATUS	scan_status;
	TrackItem		*item;

	hash_seq_init(&scan_status, lexemes_tab);
	while ((item = (TrackItem *) hash_seq_search(&scan_status)) != NULL)
	{
		if (item->frequency + item->delta <= b_current)
		{
			if (hash_search(lexemes_tab, (const void *) &item->key,
							HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "hash table corrupted");
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
	const LexemeHashKey	*d1 = (const LexemeHashKey *) key1;
	const LexemeHashKey	*d2 = (const LexemeHashKey *) key2;

	/* First, compare by length */
	if (d1->length > d2->length)
		return 1;
	else if (d1->length < d2->length)
		return -1;
	/* Lengths are equal, do a byte-by-byte comparison */
	return strncmp(d1->lexeme, d2->lexeme, d1->length);
}

/*
 *	qsort() comparator for sorting TrackItems on frequencies (descending sort)
 */
static int
trackitem_compare_frequencies_desc(const void *e1, const void *e2)
{
	const TrackItem * const *t1 = (const TrackItem * const *) e1;
	const TrackItem * const *t2 = (const TrackItem * const *) e2;

	return (*t2)->frequency - (*t1)->frequency;
}

/*
 *	qsort() comparator for sorting TrackItems on lexemes
 */
static int
trackitem_compare_lexemes(const void *e1, const void *e2)
{
	const TrackItem * const *t1 = (const TrackItem * const *) e1;
	const TrackItem * const *t2 = (const TrackItem * const *) e2;

	return lexeme_compare(&(*t1)->key, &(*t2)->key);
}
