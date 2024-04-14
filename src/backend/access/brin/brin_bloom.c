/*
 * brin_bloom.c
 *		Implementation of Bloom opclass for BRIN
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * A BRIN opclass summarizing page range into a bloom filter.
 *
 * Bloom filters allow efficient testing whether a given page range contains
 * a particular value. Therefore, if we summarize each page range into a small
 * bloom filter, we can easily (and cheaply) test whether it contains values
 * we get later.
 *
 * The index only supports equality operators, similarly to hash indexes.
 * Bloom indexes are however much smaller, and support only bitmap scans.
 *
 * Note: Don't confuse this with bloom indexes, implemented in a contrib
 * module. That extension implements an entirely new AM, building a bloom
 * filter on multiple columns in a single row. This opclass works with an
 * existing AM (BRIN) and builds bloom filter on a column.
 *
 *
 * values vs. hashes
 * -----------------
 *
 * The original column values are not used directly, but are first hashed
 * using the regular type-specific hash function, producing a uint32 hash.
 * And this hash value is then added to the summary - i.e. it's hashed
 * again and added to the bloom filter.
 *
 * This allows the code to treat all data types (byval/byref/...) the same
 * way, with only minimal space requirements, because we're working with
 * hashes and not the original values. Everything is uint32.
 *
 * Of course, this assumes the built-in hash function is reasonably good,
 * without too many collisions etc. But that does seem to be the case, at
 * least based on past experience. After all, the same hash functions are
 * used for hash indexes, hash partitioning and so on.
 *
 *
 * hashing scheme
 * --------------
 *
 * Bloom filters require a number of independent hash functions. There are
 * different schemes how to construct them - for example we might use
 * hash_uint32_extended with random seeds, but that seems fairly expensive.
 * We use a scheme requiring only two functions described in this paper:
 *
 * Less Hashing, Same Performance:Building a Better Bloom Filter
 * Adam Kirsch, Michael Mitzenmacher, Harvard School of Engineering and
 * Applied Sciences, Cambridge, Massachusetts [DOI 10.1002/rsa.20208]
 *
 * The two hash functions h1 and h2 are calculated using hard-coded seeds,
 * and then combined using (h1 + i * h2) to generate the hash functions.
 *
 *
 * sizing the bloom filter
 * -----------------------
 *
 * Size of a bloom filter depends on the number of distinct values we will
 * store in it, and the desired false positive rate. The higher the number
 * of distinct values and/or the lower the false positive rate, the larger
 * the bloom filter. On the other hand, we want to keep the index as small
 * as possible - that's one of the basic advantages of BRIN indexes.
 *
 * Although the number of distinct elements (in a page range) depends on
 * the data, we can consider it fixed. This simplifies the trade-off to
 * just false positive rate vs. size.
 *
 * At the page range level, false positive rate is a probability the bloom
 * filter matches a random value. For the whole index (with sufficiently
 * many page ranges) it represents the fraction of the index ranges (and
 * thus fraction of the table to be scanned) matching the random value.
 *
 * Furthermore, the size of the bloom filter is subject to implementation
 * limits - it has to fit onto a single index page (8kB by default). As
 * the bitmap is inherently random (when "full" about half the bits is set
 * to 1, randomly), compression can't help very much.
 *
 * To reduce the size of a filter (to fit to a page), we have to either
 * accept higher false positive rate (undesirable), or reduce the number
 * of distinct items to be stored in the filter. We can't alter the input
 * data, of course, but we may make the BRIN page ranges smaller - instead
 * of the default 128 pages (1MB) we may build index with 16-page ranges,
 * or something like that. This should reduce the number of distinct values
 * in the page range, making the filter smaller (with fixed false positive
 * rate). Even for random data sets this should help, as the number of rows
 * per heap page is limited (to ~290 with very narrow tables, likely ~20
 * in practice).
 *
 * Of course, good sizing decisions depend on having the necessary data,
 * i.e. number of distinct values in a page range (of a given size) and
 * table size (to estimate cost change due to change in false positive
 * rate due to having larger index vs. scanning larger indexes). We may
 * not have that data - for example when building an index on empty table
 * it's not really possible. And for some data we only have estimates for
 * the whole table and we can only estimate per-range values (ndistinct).
 *
 * Another challenge is that while the bloom filter is per-column, it's
 * the whole index tuple that has to fit into a page. And for multi-column
 * indexes that may include pieces we have no control over (not necessarily
 * bloom filters, the other columns may use other BRIN opclasses). So it's
 * not entirely clear how to distribute the space between those columns.
 *
 * The current logic, implemented in brin_bloom_get_ndistinct, attempts to
 * make some basic sizing decisions, based on the size of BRIN ranges, and
 * the maximum number of rows per range.
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_bloom.c
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/brin.h"
#include "access/brin_internal.h"
#include "access/brin_page.h"
#include "access/brin_tuple.h"
#include "access/hash.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "catalog/pg_amop.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include <math.h>

#define BloomEqualStrategyNumber	1

/*
 * Additional SQL level support functions. We only have one, which is
 * used to calculate hash of the input value.
 *
 * Procedure numbers must not use values reserved for BRIN itself; see
 * brin_internal.h.
 */
#define		BLOOM_MAX_PROCNUMS		1	/* maximum support procs we need */
#define		PROCNUM_HASH			11	/* required */

/*
 * Subtract this from procnum to obtain index in BloomOpaque arrays
 * (Must be equal to minimum of private procnums).
 */
#define		PROCNUM_BASE			11

/*
 * Storage type for BRIN's reloptions.
 */
typedef struct BloomOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	double		nDistinctPerRange;	/* number of distinct values per range */
	double		falsePositiveRate;	/* false positive for bloom filter */
} BloomOptions;

/*
 * The current min value (16) is somewhat arbitrary, but it's based
 * on the fact that the filter header is ~20B alone, which is about
 * the same as the filter bitmap for 16 distinct items with 1% false
 * positive rate. So by allowing lower values we'd not gain much. In
 * any case, the min should not be larger than MaxHeapTuplesPerPage
 * (~290), which is the theoretical maximum for single-page ranges.
 */
#define		BLOOM_MIN_NDISTINCT_PER_RANGE		16

/*
 * Used to determine number of distinct items, based on the number of rows
 * in a page range. The 10% is somewhat similar to what estimate_num_groups
 * does, so we use the same factor here.
 */
#define		BLOOM_DEFAULT_NDISTINCT_PER_RANGE	-0.1	/* 10% of values */

/*
 * Allowed range and default value for the false positive range. The exact
 * values are somewhat arbitrary, but were chosen considering the various
 * parameters (size of filter vs. page size, etc.).
 *
 * The lower the false-positive rate, the more accurate the filter is, but
 * it also gets larger - at some point this eliminates the main advantage
 * of BRIN indexes, which is the tiny size. At 0.01% the index is about
 * 10% of the table (assuming 290 distinct values per 8kB page).
 *
 * On the other hand, as the false-positive rate increases, larger part of
 * the table has to be scanned due to mismatches - at 25% we're probably
 * close to sequential scan being cheaper.
 */
#define		BLOOM_MIN_FALSE_POSITIVE_RATE	0.0001	/* 0.01% fp rate */
#define		BLOOM_MAX_FALSE_POSITIVE_RATE	0.25	/* 25% fp rate */
#define		BLOOM_DEFAULT_FALSE_POSITIVE_RATE	0.01	/* 1% fp rate */

#define BloomGetNDistinctPerRange(opts) \
	((opts) && (((BloomOptions *) (opts))->nDistinctPerRange != 0) ? \
	 (((BloomOptions *) (opts))->nDistinctPerRange) : \
	 BLOOM_DEFAULT_NDISTINCT_PER_RANGE)

#define BloomGetFalsePositiveRate(opts) \
	((opts) && (((BloomOptions *) (opts))->falsePositiveRate != 0.0) ? \
	 (((BloomOptions *) (opts))->falsePositiveRate) : \
	 BLOOM_DEFAULT_FALSE_POSITIVE_RATE)

/*
 * And estimate of the largest bloom we can fit onto a page. This is not
 * a perfect guarantee, for a couple of reasons. For example, the row may
 * be larger because the index has multiple columns.
 */
#define BloomMaxFilterSize \
	MAXALIGN_DOWN(BLCKSZ - \
				  (MAXALIGN(SizeOfPageHeaderData + \
							sizeof(ItemIdData)) + \
				   MAXALIGN(sizeof(BrinSpecialSpace)) + \
				   SizeOfBrinTuple))

/*
 * Seeds used to calculate two hash functions h1 and h2, which are then used
 * to generate k hashes using the (h1 + i * h2) scheme.
 */
#define BLOOM_SEED_1	0x71d924af
#define BLOOM_SEED_2	0xba48b314

/*
 * Bloom Filter
 *
 * Represents a bloom filter, built on hashes of the indexed values. That is,
 * we compute a uint32 hash of the value, and then store this hash into the
 * bloom filter (and compute additional hashes on it).
 *
 * XXX We could implement "sparse" bloom filters, keeping only the bytes that
 * are not entirely 0. But while indexes don't support TOAST, the varlena can
 * still be compressed. So this seems unnecessary, because the compression
 * should do the same job.
 *
 * XXX We can also watch the number of bits set in the bloom filter, and then
 * stop using it (and not store the bitmap, to save space) when the false
 * positive rate gets too high. But even if the false positive rate exceeds the
 * desired value, it still can eliminate some page ranges.
 */
typedef struct BloomFilter
{
	/* varlena header (do not touch directly!) */
	int32		vl_len_;

	/* space for various flags (unused for now) */
	uint16		flags;

	/* fields for the HASHED phase */
	uint8		nhashes;		/* number of hash functions */
	uint32		nbits;			/* number of bits in the bitmap (size) */
	uint32		nbits_set;		/* number of bits set to 1 */

	/* data of the bloom filter */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} BloomFilter;


/*
 * bloom_init
 * 		Initialize the Bloom Filter, allocate all the memory.
 *
 * The filter is initialized with optimal size for ndistinct expected values
 * and the requested false positive rate. The filter is stored as varlena.
 */
static BloomFilter *
bloom_init(int ndistinct, double false_positive_rate)
{
	Size		len;
	BloomFilter *filter;

	int			nbits;			/* size of filter / number of bits */
	int			nbytes;			/* size of filter / number of bytes */

	double		k;				/* number of hash functions */

	Assert(ndistinct > 0);
	Assert(false_positive_rate > 0 && false_positive_rate < 1);

	/* sizing bloom filter: -(n * ln(p)) / (ln(2))^2 */
	nbits = ceil(-(ndistinct * log(false_positive_rate)) / pow(log(2.0), 2));

	/* round m to whole bytes */
	nbytes = ((nbits + 7) / 8);
	nbits = nbytes * 8;

	/*
	 * Reject filters that are obviously too large to store on a page.
	 *
	 * Initially the bloom filter is just zeroes and so very compressible, but
	 * as we add values it gets more and more random, and so less and less
	 * compressible. So initially everything fits on the page, but we might
	 * get surprising failures later - we want to prevent that, so we reject
	 * bloom filter that are obviously too large.
	 *
	 * XXX It's not uncommon to oversize the bloom filter a bit, to defend
	 * against unexpected data anomalies (parts of table with more distinct
	 * values per range etc.). But we still need to make sure even the
	 * oversized filter fits on page, if such need arises.
	 *
	 * XXX This check is not perfect, because the index may have multiple
	 * filters that are small individually, but too large when combined.
	 */
	if (nbytes > BloomMaxFilterSize)
		elog(ERROR, "the bloom filter is too large (%d > %zu)", nbytes,
			 BloomMaxFilterSize);

	/*
	 * round(log(2.0) * m / ndistinct), but assume round() may not be
	 * available on Windows
	 */
	k = log(2.0) * nbits / ndistinct;
	k = (k - floor(k) >= 0.5) ? ceil(k) : floor(k);

	/*
	 * We allocate the whole filter. Most of it is going to be 0 bits, so the
	 * varlena is easy to compress.
	 */
	len = offsetof(BloomFilter, data) + nbytes;

	filter = (BloomFilter *) palloc0(len);

	filter->flags = 0;
	filter->nhashes = (int) k;
	filter->nbits = nbits;

	SET_VARSIZE(filter, len);

	return filter;
}


/*
 * bloom_add_value
 * 		Add value to the bloom filter.
 */
static BloomFilter *
bloom_add_value(BloomFilter *filter, uint32 value, bool *updated)
{
	int			i;
	uint64		h1,
				h2;

	/* compute the hashes, used for the bloom filter */
	h1 = hash_bytes_uint32_extended(value, BLOOM_SEED_1) % filter->nbits;
	h2 = hash_bytes_uint32_extended(value, BLOOM_SEED_2) % filter->nbits;

	/* compute the requested number of hashes */
	for (i = 0; i < filter->nhashes; i++)
	{
		/* h1 + h2 + f(i) */
		uint32		h = (h1 + i * h2) % filter->nbits;
		uint32		byte = (h / 8);
		uint32		bit = (h % 8);

		/* if the bit is not set, set it and remember we did that */
		if (!(filter->data[byte] & (0x01 << bit)))
		{
			filter->data[byte] |= (0x01 << bit);
			filter->nbits_set++;
			if (updated)
				*updated = true;
		}
	}

	return filter;
}


/*
 * bloom_contains_value
 * 		Check if the bloom filter contains a particular value.
 */
static bool
bloom_contains_value(BloomFilter *filter, uint32 value)
{
	int			i;
	uint64		h1,
				h2;

	/* calculate the two hashes */
	h1 = hash_bytes_uint32_extended(value, BLOOM_SEED_1) % filter->nbits;
	h2 = hash_bytes_uint32_extended(value, BLOOM_SEED_2) % filter->nbits;

	/* compute the requested number of hashes */
	for (i = 0; i < filter->nhashes; i++)
	{
		/* h1 + h2 + f(i) */
		uint32		h = (h1 + i * h2) % filter->nbits;
		uint32		byte = (h / 8);
		uint32		bit = (h % 8);

		/* if the bit is not set, the value is not there */
		if (!(filter->data[byte] & (0x01 << bit)))
			return false;
	}

	/* all hashes found in bloom filter */
	return true;
}

typedef struct BloomOpaque
{
	/*
	 * XXX At this point we only need a single proc (to compute the hash), but
	 * let's keep the array just like inclusion and minmax opclasses, for
	 * consistency. We may need additional procs in the future.
	 */
	FmgrInfo	extra_procinfos[BLOOM_MAX_PROCNUMS];
	bool		extra_proc_missing[BLOOM_MAX_PROCNUMS];
} BloomOpaque;

static FmgrInfo *bloom_get_procinfo(BrinDesc *bdesc, uint16 attno,
									uint16 procnum);


Datum
brin_bloom_opcinfo(PG_FUNCTION_ARGS)
{
	BrinOpcInfo *result;

	/*
	 * opaque->strategy_procinfos is initialized lazily; here it is set to
	 * all-uninitialized by palloc0 which sets fn_oid to InvalidOid.
	 *
	 * bloom indexes only store the filter as a single BYTEA column
	 */

	result = palloc0(MAXALIGN(SizeofBrinOpcInfo(1)) +
					 sizeof(BloomOpaque));
	result->oi_nstored = 1;
	result->oi_regular_nulls = true;
	result->oi_opaque = (BloomOpaque *)
		MAXALIGN((char *) result + SizeofBrinOpcInfo(1));
	result->oi_typcache[0] = lookup_type_cache(PG_BRIN_BLOOM_SUMMARYOID, 0);

	PG_RETURN_POINTER(result);
}

/*
 * brin_bloom_get_ndistinct
 *		Determine the ndistinct value used to size bloom filter.
 *
 * Adjust the ndistinct value based on the pagesPerRange value. First,
 * if it's negative, it's assumed to be relative to maximum number of
 * tuples in the range (assuming each page gets MaxHeapTuplesPerPage
 * tuples, which is likely a significant over-estimate). We also clamp
 * the value, not to over-size the bloom filter unnecessarily.
 *
 * XXX We can only do this when the pagesPerRange value was supplied.
 * If it wasn't, it has to be a read-only access to the index, in which
 * case we don't really care. But perhaps we should fall-back to the
 * default pagesPerRange value?
 *
 * XXX We might also fetch info about ndistinct estimate for the column,
 * and compute the expected number of distinct values in a range. But
 * that may be tricky due to data being sorted in various ways, so it
 * seems better to rely on the upper estimate.
 *
 * XXX We might also calculate a better estimate of rows per BRIN range,
 * instead of using MaxHeapTuplesPerPage (which probably produces values
 * much higher than reality).
 */
static int
brin_bloom_get_ndistinct(BrinDesc *bdesc, BloomOptions *opts)
{
	double		ndistinct;
	double		maxtuples;
	BlockNumber pagesPerRange;

	pagesPerRange = BrinGetPagesPerRange(bdesc->bd_index);
	ndistinct = BloomGetNDistinctPerRange(opts);

	Assert(BlockNumberIsValid(pagesPerRange));

	maxtuples = MaxHeapTuplesPerPage * pagesPerRange;

	/*
	 * Similarly to n_distinct, negative values are relative - in this case to
	 * maximum number of tuples in the page range (maxtuples).
	 */
	if (ndistinct < 0)
		ndistinct = (-ndistinct) * maxtuples;

	/*
	 * Positive values are to be used directly, but we still apply a couple of
	 * safeties to avoid using unreasonably small bloom filters.
	 */
	ndistinct = Max(ndistinct, BLOOM_MIN_NDISTINCT_PER_RANGE);

	/*
	 * And don't use more than the maximum possible number of tuples, in the
	 * range, which would be entirely wasteful.
	 */
	ndistinct = Min(ndistinct, maxtuples);

	return (int) ndistinct;
}

/*
 * Examine the given index tuple (which contains partial status of a certain
 * page range) by comparing it to the given value that comes from another heap
 * tuple.  If the new value is outside the bloom filter specified by the
 * existing tuple values, update the index tuple and return true.  Otherwise,
 * return false and do not modify in this case.
 */
Datum
brin_bloom_add_value(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	Datum		newval = PG_GETARG_DATUM(2);
	bool		isnull PG_USED_FOR_ASSERTS_ONLY = PG_GETARG_DATUM(3);
	BloomOptions *opts = (BloomOptions *) PG_GET_OPCLASS_OPTIONS();
	Oid			colloid = PG_GET_COLLATION();
	FmgrInfo   *hashFn;
	uint32		hashValue;
	bool		updated = false;
	AttrNumber	attno;
	BloomFilter *filter;

	Assert(!isnull);

	attno = column->bv_attno;

	/*
	 * If this is the first non-null value, we need to initialize the bloom
	 * filter. Otherwise just extract the existing bloom filter from
	 * BrinValues.
	 */
	if (column->bv_allnulls)
	{
		filter = bloom_init(brin_bloom_get_ndistinct(bdesc, opts),
							BloomGetFalsePositiveRate(opts));
		column->bv_values[0] = PointerGetDatum(filter);
		column->bv_allnulls = false;
		updated = true;
	}
	else
		filter = (BloomFilter *) PG_DETOAST_DATUM(column->bv_values[0]);

	/*
	 * Compute the hash of the new value, using the supplied hash function,
	 * and then add the hash value to the bloom filter.
	 */
	hashFn = bloom_get_procinfo(bdesc, attno, PROCNUM_HASH);

	hashValue = DatumGetUInt32(FunctionCall1Coll(hashFn, colloid, newval));

	filter = bloom_add_value(filter, hashValue, &updated);

	column->bv_values[0] = PointerGetDatum(filter);

	PG_RETURN_BOOL(updated);
}

/*
 * Given an index tuple corresponding to a certain page range and a scan key,
 * return whether the scan key is consistent with the index tuple's bloom
 * filter.  Return true if so, false otherwise.
 */
Datum
brin_bloom_consistent(PG_FUNCTION_ARGS)
{
	BrinDesc   *bdesc = (BrinDesc *) PG_GETARG_POINTER(0);
	BrinValues *column = (BrinValues *) PG_GETARG_POINTER(1);
	ScanKey    *keys = (ScanKey *) PG_GETARG_POINTER(2);
	int			nkeys = PG_GETARG_INT32(3);
	Oid			colloid = PG_GET_COLLATION();
	AttrNumber	attno;
	Datum		value;
	Datum		matches;
	FmgrInfo   *finfo;
	uint32		hashValue;
	BloomFilter *filter;
	int			keyno;

	filter = (BloomFilter *) PG_DETOAST_DATUM(column->bv_values[0]);

	Assert(filter);

	matches = true;

	for (keyno = 0; keyno < nkeys; keyno++)
	{
		ScanKey		key = keys[keyno];

		/* NULL keys are handled and filtered-out in bringetbitmap */
		Assert(!(key->sk_flags & SK_ISNULL));

		attno = key->sk_attno;
		value = key->sk_argument;

		switch (key->sk_strategy)
		{
			case BloomEqualStrategyNumber:

				/*
				 * In the equality case (WHERE col = someval), we want to
				 * return the current page range if the minimum value in the
				 * range <= scan key, and the maximum value >= scan key.
				 */
				finfo = bloom_get_procinfo(bdesc, attno, PROCNUM_HASH);

				hashValue = DatumGetUInt32(FunctionCall1Coll(finfo, colloid, value));
				matches &= bloom_contains_value(filter, hashValue);

				break;
			default:
				/* shouldn't happen */
				elog(ERROR, "invalid strategy number %d", key->sk_strategy);
				matches = 0;
				break;
		}

		if (!matches)
			break;
	}

	PG_RETURN_DATUM(matches);
}

/*
 * Given two BrinValues, update the first of them as a union of the summary
 * values contained in both.  The second one is untouched.
 *
 * XXX We assume the bloom filters have the same parameters for now. In the
 * future we should have 'can union' function, to decide if we can combine
 * two particular bloom filters.
 */
Datum
brin_bloom_union(PG_FUNCTION_ARGS)
{
	int			i;
	int			nbytes;
	BrinValues *col_a = (BrinValues *) PG_GETARG_POINTER(1);
	BrinValues *col_b = (BrinValues *) PG_GETARG_POINTER(2);
	BloomFilter *filter_a;
	BloomFilter *filter_b;

	Assert(col_a->bv_attno == col_b->bv_attno);
	Assert(!col_a->bv_allnulls && !col_b->bv_allnulls);

	filter_a = (BloomFilter *) PG_DETOAST_DATUM(col_a->bv_values[0]);
	filter_b = (BloomFilter *) PG_DETOAST_DATUM(col_b->bv_values[0]);

	/* make sure the filters use the same parameters */
	Assert(filter_a && filter_b);
	Assert(filter_a->nbits == filter_b->nbits);
	Assert(filter_a->nhashes == filter_b->nhashes);
	Assert((filter_a->nbits > 0) && (filter_a->nbits % 8 == 0));

	nbytes = (filter_a->nbits) / 8;

	/* simply OR the bitmaps */
	for (i = 0; i < nbytes; i++)
		filter_a->data[i] |= filter_b->data[i];

	/* update the number of bits set in the filter */
	filter_a->nbits_set = pg_popcount((const char *) filter_a->data, nbytes);

	PG_RETURN_VOID();
}

/*
 * Cache and return inclusion opclass support procedure
 *
 * Return the procedure corresponding to the given function support number
 * or null if it does not exist.
 */
static FmgrInfo *
bloom_get_procinfo(BrinDesc *bdesc, uint16 attno, uint16 procnum)
{
	BloomOpaque *opaque;
	uint16		basenum = procnum - PROCNUM_BASE;

	/*
	 * We cache these in the opaque struct, to avoid repetitive syscache
	 * lookups.
	 */
	opaque = (BloomOpaque *) bdesc->bd_info[attno - 1]->oi_opaque;

	/*
	 * If we already searched for this proc and didn't find it, don't bother
	 * searching again.
	 */
	if (opaque->extra_proc_missing[basenum])
		return NULL;

	if (opaque->extra_procinfos[basenum].fn_oid == InvalidOid)
	{
		if (RegProcedureIsValid(index_getprocid(bdesc->bd_index, attno,
												procnum)))
		{
			fmgr_info_copy(&opaque->extra_procinfos[basenum],
						   index_getprocinfo(bdesc->bd_index, attno, procnum),
						   bdesc->bd_context);
		}
		else
		{
			opaque->extra_proc_missing[basenum] = true;
			return NULL;
		}
	}

	return &opaque->extra_procinfos[basenum];
}

Datum
brin_bloom_options(PG_FUNCTION_ARGS)
{
	local_relopts *relopts = (local_relopts *) PG_GETARG_POINTER(0);

	init_local_reloptions(relopts, sizeof(BloomOptions));

	add_local_real_reloption(relopts, "n_distinct_per_range",
							 "number of distinct items expected in a BRIN page range",
							 BLOOM_DEFAULT_NDISTINCT_PER_RANGE,
							 -1.0, INT_MAX, offsetof(BloomOptions, nDistinctPerRange));

	add_local_real_reloption(relopts, "false_positive_rate",
							 "desired false-positive rate for the bloom filters",
							 BLOOM_DEFAULT_FALSE_POSITIVE_RATE,
							 BLOOM_MIN_FALSE_POSITIVE_RATE,
							 BLOOM_MAX_FALSE_POSITIVE_RATE,
							 offsetof(BloomOptions, falsePositiveRate));

	PG_RETURN_VOID();
}

/*
 * brin_bloom_summary_in
 *		- input routine for type brin_bloom_summary.
 *
 * brin_bloom_summary is only used internally to represent summaries
 * in BRIN bloom indexes, so it has no operations of its own, and we
 * disallow input too.
 */
Datum
brin_bloom_summary_in(PG_FUNCTION_ARGS)
{
	/*
	 * brin_bloom_summary stores the data in binary form and parsing text
	 * input is not needed, so disallow this.
	 */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_brin_bloom_summary")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}


/*
 * brin_bloom_summary_out
 *		- output routine for type brin_bloom_summary.
 *
 * BRIN bloom summaries are serialized into a bytea value, but we want
 * to output something nicer humans can understand.
 */
Datum
brin_bloom_summary_out(PG_FUNCTION_ARGS)
{
	BloomFilter *filter;
	StringInfoData str;

	/* detoast the data to get value with a full 4B header */
	filter = (BloomFilter *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));

	initStringInfo(&str);
	appendStringInfoChar(&str, '{');

	appendStringInfo(&str, "mode: hashed  nhashes: %u  nbits: %u  nbits_set: %u",
					 filter->nhashes, filter->nbits, filter->nbits_set);

	appendStringInfoChar(&str, '}');

	PG_RETURN_CSTRING(str.data);
}

/*
 * brin_bloom_summary_recv
 *		- binary input routine for type brin_bloom_summary.
 */
Datum
brin_bloom_summary_recv(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_brin_bloom_summary")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * brin_bloom_summary_send
 *		- binary output routine for type brin_bloom_summary.
 *
 * BRIN bloom summaries are serialized in a bytea value (although the
 * type is named differently), so let's just send that.
 */
Datum
brin_bloom_summary_send(PG_FUNCTION_ARGS)
{
	return byteasend(fcinfo);
}
