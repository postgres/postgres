/*-------------------------------------------------------------------------
 *
 * mvdistinct.c
 *	  POSTGRES multivariate ndistinct coefficients
 *
 * Estimating number of groups in a combination of columns (e.g. for GROUP BY)
 * is tricky, and the estimation error is often significant.

 * The multivariate ndistinct coefficients address this by storing ndistinct
 * estimates for combinations of the user-specified columns.  So for example
 * given a statistics object on three columns (a,b,c), this module estimates
 * and stores n-distinct for (a,b), (a,c), (b,c) and (a,b,c).  The per-column
 * estimates are already available in pg_statistic.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/statistics/mvdistinct.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "lib/stringinfo.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"
#include "utils/fmgrprotos.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "varatt.h"

static double ndistinct_for_combination(double totalrows, StatsBuildData *data,
										int k, int *combination);
static double estimate_ndistinct(double totalrows, int numrows, int d, int f1);
static int	n_choose_k(int n, int k);
static int	num_combinations(int n);

/* size of the struct header fields (magic, type, nitems) */
#define SizeOfHeader		(3 * sizeof(uint32))

/* size of a serialized ndistinct item (coefficient, natts, atts) */
#define SizeOfItem(natts) \
	(sizeof(double) + sizeof(int) + (natts) * sizeof(AttrNumber))

/* minimal size of a ndistinct item (with two attributes) */
#define MinSizeOfItem	SizeOfItem(2)

/* minimal size of mvndistinct, when all items are minimal */
#define MinSizeOfItems(nitems)	\
	(SizeOfHeader + (nitems) * MinSizeOfItem)

/* Combination generator API */

/* internal state for generator of k-combinations of n elements */
typedef struct CombinationGenerator
{
	int			k;				/* size of the combination */
	int			n;				/* total number of elements */
	int			current;		/* index of the next combination to return */
	int			ncombinations;	/* number of combinations (size of array) */
	int		   *combinations;	/* array of pre-built combinations */
} CombinationGenerator;

static CombinationGenerator *generator_init(int n, int k);
static void generator_free(CombinationGenerator *state);
static int *generator_next(CombinationGenerator *state);
static void generate_combinations(CombinationGenerator *state);


/*
 * statext_ndistinct_build
 *		Compute ndistinct coefficient for the combination of attributes.
 *
 * This computes the ndistinct estimate using the same estimator used
 * in analyze.c and then computes the coefficient.
 *
 * To handle expressions easily, we treat them as system attributes with
 * negative attnums, and offset everything by number of expressions to
 * allow using Bitmapsets.
 */
MVNDistinct *
statext_ndistinct_build(double totalrows, StatsBuildData *data)
{
	MVNDistinct *result;
	int			k;
	int			itemcnt;
	int			numattrs = data->nattnums;
	int			numcombs = num_combinations(numattrs);

	result = palloc(offsetof(MVNDistinct, items) +
					numcombs * sizeof(MVNDistinctItem));
	result->magic = STATS_NDISTINCT_MAGIC;
	result->type = STATS_NDISTINCT_TYPE_BASIC;
	result->nitems = numcombs;

	itemcnt = 0;
	for (k = 2; k <= numattrs; k++)
	{
		int		   *combination;
		CombinationGenerator *generator;

		/* generate combinations of K out of N elements */
		generator = generator_init(numattrs, k);

		while ((combination = generator_next(generator)))
		{
			MVNDistinctItem *item = &result->items[itemcnt];
			int			j;

			item->attributes = palloc(sizeof(AttrNumber) * k);
			item->nattributes = k;

			/* translate the indexes to attnums */
			for (j = 0; j < k; j++)
			{
				item->attributes[j] = data->attnums[combination[j]];

				Assert(AttributeNumberIsValid(item->attributes[j]));
			}

			item->ndistinct =
				ndistinct_for_combination(totalrows, data, k, combination);

			itemcnt++;
			Assert(itemcnt <= result->nitems);
		}

		generator_free(generator);
	}

	/* must consume exactly the whole output array */
	Assert(itemcnt == result->nitems);

	return result;
}

/*
 * statext_ndistinct_load
 *		Load the ndistinct value for the indicated pg_statistic_ext tuple
 */
MVNDistinct *
statext_ndistinct_load(Oid mvoid, bool inh)
{
	MVNDistinct *result;
	bool		isnull;
	Datum		ndist;
	HeapTuple	htup;

	htup = SearchSysCache2(STATEXTDATASTXOID,
						   ObjectIdGetDatum(mvoid), BoolGetDatum(inh));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "cache lookup failed for statistics object %u", mvoid);

	ndist = SysCacheGetAttr(STATEXTDATASTXOID, htup,
							Anum_pg_statistic_ext_data_stxdndistinct, &isnull);
	if (isnull)
		elog(ERROR,
			 "requested statistics kind \"%c\" is not yet built for statistics object %u",
			 STATS_EXT_NDISTINCT, mvoid);

	result = statext_ndistinct_deserialize(DatumGetByteaPP(ndist));

	ReleaseSysCache(htup);

	return result;
}

/*
 * statext_ndistinct_serialize
 *		serialize ndistinct to the on-disk bytea format
 */
bytea *
statext_ndistinct_serialize(MVNDistinct *ndistinct)
{
	int			i;
	bytea	   *output;
	char	   *tmp;
	Size		len;

	Assert(ndistinct->magic == STATS_NDISTINCT_MAGIC);
	Assert(ndistinct->type == STATS_NDISTINCT_TYPE_BASIC);

	/*
	 * Base size is size of scalar fields in the struct, plus one base struct
	 * for each item, including number of items for each.
	 */
	len = VARHDRSZ + SizeOfHeader;

	/* and also include space for the actual attribute numbers */
	for (i = 0; i < ndistinct->nitems; i++)
	{
		int			nmembers;

		nmembers = ndistinct->items[i].nattributes;
		Assert(nmembers >= 2);

		len += SizeOfItem(nmembers);
	}

	output = (bytea *) palloc(len);
	SET_VARSIZE(output, len);

	tmp = VARDATA(output);

	/* Store the base struct values (magic, type, nitems) */
	memcpy(tmp, &ndistinct->magic, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(tmp, &ndistinct->type, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(tmp, &ndistinct->nitems, sizeof(uint32));
	tmp += sizeof(uint32);

	/*
	 * store number of attributes and attribute numbers for each entry
	 */
	for (i = 0; i < ndistinct->nitems; i++)
	{
		MVNDistinctItem item = ndistinct->items[i];
		int			nmembers = item.nattributes;

		memcpy(tmp, &item.ndistinct, sizeof(double));
		tmp += sizeof(double);
		memcpy(tmp, &nmembers, sizeof(int));
		tmp += sizeof(int);

		memcpy(tmp, item.attributes, sizeof(AttrNumber) * nmembers);
		tmp += nmembers * sizeof(AttrNumber);

		/* protect against overflows */
		Assert(tmp <= ((char *) output + len));
	}

	/* check we used exactly the expected space */
	Assert(tmp == ((char *) output + len));

	return output;
}

/*
 * statext_ndistinct_deserialize
 *		Read an on-disk bytea format MVNDistinct to in-memory format
 */
MVNDistinct *
statext_ndistinct_deserialize(bytea *data)
{
	int			i;
	Size		minimum_size;
	MVNDistinct ndist;
	MVNDistinct *ndistinct;
	char	   *tmp;

	if (data == NULL)
		return NULL;

	/* we expect at least the basic fields of MVNDistinct struct */
	if (VARSIZE_ANY_EXHDR(data) < SizeOfHeader)
		elog(ERROR, "invalid MVNDistinct size %zu (expected at least %zu)",
			 VARSIZE_ANY_EXHDR(data), SizeOfHeader);

	/* initialize pointer to the data part (skip the varlena header) */
	tmp = VARDATA_ANY(data);

	/* read the header fields and perform basic sanity checks */
	memcpy(&ndist.magic, tmp, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(&ndist.type, tmp, sizeof(uint32));
	tmp += sizeof(uint32);
	memcpy(&ndist.nitems, tmp, sizeof(uint32));
	tmp += sizeof(uint32);

	if (ndist.magic != STATS_NDISTINCT_MAGIC)
		elog(ERROR, "invalid ndistinct magic %08x (expected %08x)",
			 ndist.magic, STATS_NDISTINCT_MAGIC);
	if (ndist.type != STATS_NDISTINCT_TYPE_BASIC)
		elog(ERROR, "invalid ndistinct type %d (expected %d)",
			 ndist.type, STATS_NDISTINCT_TYPE_BASIC);
	if (ndist.nitems == 0)
		elog(ERROR, "invalid zero-length item array in MVNDistinct");

	/* what minimum bytea size do we expect for those parameters */
	minimum_size = MinSizeOfItems(ndist.nitems);
	if (VARSIZE_ANY_EXHDR(data) < minimum_size)
		elog(ERROR, "invalid MVNDistinct size %zu (expected at least %zu)",
			 VARSIZE_ANY_EXHDR(data), minimum_size);

	/*
	 * Allocate space for the ndistinct items (no space for each item's
	 * attnos: those live in bitmapsets allocated separately)
	 */
	ndistinct = palloc0(MAXALIGN(offsetof(MVNDistinct, items)) +
						(ndist.nitems * sizeof(MVNDistinctItem)));
	ndistinct->magic = ndist.magic;
	ndistinct->type = ndist.type;
	ndistinct->nitems = ndist.nitems;

	for (i = 0; i < ndistinct->nitems; i++)
	{
		MVNDistinctItem *item = &ndistinct->items[i];

		/* ndistinct value */
		memcpy(&item->ndistinct, tmp, sizeof(double));
		tmp += sizeof(double);

		/* number of attributes */
		memcpy(&item->nattributes, tmp, sizeof(int));
		tmp += sizeof(int);
		Assert((item->nattributes >= 2) && (item->nattributes <= STATS_MAX_DIMENSIONS));

		item->attributes
			= (AttrNumber *) palloc(item->nattributes * sizeof(AttrNumber));

		memcpy(item->attributes, tmp, sizeof(AttrNumber) * item->nattributes);
		tmp += sizeof(AttrNumber) * item->nattributes;

		/* still within the bytea */
		Assert(tmp <= ((char *) data + VARSIZE_ANY(data)));
	}

	/* we should have consumed the whole bytea exactly */
	Assert(tmp == ((char *) data + VARSIZE_ANY(data)));

	return ndistinct;
}

/*
 * pg_ndistinct_in
 *		input routine for type pg_ndistinct
 *
 * pg_ndistinct is real enough to be a table column, but it has no
 * operations of its own, and disallows input (just like pg_node_tree).
 */
Datum
pg_ndistinct_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_ndistinct")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * pg_ndistinct
 *		output routine for type pg_ndistinct
 *
 * Produces a human-readable representation of the value.
 */
Datum
pg_ndistinct_out(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(0);
	MVNDistinct *ndist = statext_ndistinct_deserialize(data);
	int			i;
	StringInfoData str;

	initStringInfo(&str);
	appendStringInfoChar(&str, '{');

	for (i = 0; i < ndist->nitems; i++)
	{
		int			j;
		MVNDistinctItem item = ndist->items[i];

		if (i > 0)
			appendStringInfoString(&str, ", ");

		for (j = 0; j < item.nattributes; j++)
		{
			AttrNumber	attnum = item.attributes[j];

			appendStringInfo(&str, "%s%d", (j == 0) ? "\"" : ", ", attnum);
		}
		appendStringInfo(&str, "\": %d", (int) item.ndistinct);
	}

	appendStringInfoChar(&str, '}');

	PG_RETURN_CSTRING(str.data);
}

/*
 * pg_ndistinct_recv
 *		binary input routine for type pg_ndistinct
 */
Datum
pg_ndistinct_recv(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_ndistinct")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * pg_ndistinct_send
 *		binary output routine for type pg_ndistinct
 *
 * n-distinct is serialized into a bytea value, so let's send that.
 */
Datum
pg_ndistinct_send(PG_FUNCTION_ARGS)
{
	return byteasend(fcinfo);
}

/*
 * ndistinct_for_combination
 *		Estimates number of distinct values in a combination of columns.
 *
 * This uses the same ndistinct estimator as compute_scalar_stats() in
 * ANALYZE, i.e.,
 *		n*d / (n - f1 + f1*n/N)
 *
 * except that instead of values in a single column we are dealing with
 * combination of multiple columns.
 */
static double
ndistinct_for_combination(double totalrows, StatsBuildData *data,
						  int k, int *combination)
{
	int			i,
				j;
	int			f1,
				cnt,
				d;
	bool	   *isnull;
	Datum	   *values;
	SortItem   *items;
	MultiSortSupport mss;
	int			numrows = data->numrows;

	mss = multi_sort_init(k);

	/*
	 * In order to determine the number of distinct elements, create separate
	 * values[]/isnull[] arrays with all the data we have, then sort them
	 * using the specified column combination as dimensions.  We could try to
	 * sort in place, but it'd probably be more complex and bug-prone.
	 */
	items = (SortItem *) palloc(numrows * sizeof(SortItem));
	values = (Datum *) palloc0(sizeof(Datum) * numrows * k);
	isnull = (bool *) palloc0(sizeof(bool) * numrows * k);

	for (i = 0; i < numrows; i++)
	{
		items[i].values = &values[i * k];
		items[i].isnull = &isnull[i * k];
	}

	/*
	 * For each dimension, set up sort-support and fill in the values from the
	 * sample data.
	 *
	 * We use the column data types' default sort operators and collations;
	 * perhaps at some point it'd be worth using column-specific collations?
	 */
	for (i = 0; i < k; i++)
	{
		Oid			typid;
		TypeCacheEntry *type;
		Oid			collid = InvalidOid;
		VacAttrStats *colstat = data->stats[combination[i]];

		typid = colstat->attrtypid;
		collid = colstat->attrcollid;

		type = lookup_type_cache(typid, TYPECACHE_LT_OPR);
		if (type->lt_opr == InvalidOid) /* shouldn't happen */
			elog(ERROR, "cache lookup failed for ordering operator for type %u",
				 typid);

		/* prepare the sort function for this dimension */
		multi_sort_add_dimension(mss, i, type->lt_opr, collid);

		/* accumulate all the data for this dimension into the arrays */
		for (j = 0; j < numrows; j++)
		{
			items[j].values[i] = data->values[combination[i]][j];
			items[j].isnull[i] = data->nulls[combination[i]][j];
		}
	}

	/* We can sort the array now ... */
	qsort_interruptible(items, numrows, sizeof(SortItem),
						multi_sort_compare, mss);

	/* ... and count the number of distinct combinations */

	f1 = 0;
	cnt = 1;
	d = 1;
	for (i = 1; i < numrows; i++)
	{
		if (multi_sort_compare(&items[i], &items[i - 1], mss) != 0)
		{
			if (cnt == 1)
				f1 += 1;

			d++;
			cnt = 0;
		}

		cnt += 1;
	}

	if (cnt == 1)
		f1 += 1;

	return estimate_ndistinct(totalrows, numrows, d, f1);
}

/* The Duj1 estimator (already used in analyze.c). */
static double
estimate_ndistinct(double totalrows, int numrows, int d, int f1)
{
	double		numer,
				denom,
				ndistinct;

	numer = (double) numrows * (double) d;

	denom = (double) (numrows - f1) +
		(double) f1 * (double) numrows / totalrows;

	ndistinct = numer / denom;

	/* Clamp to sane range in case of roundoff error */
	if (ndistinct < (double) d)
		ndistinct = (double) d;

	if (ndistinct > totalrows)
		ndistinct = totalrows;

	return floor(ndistinct + 0.5);
}

/*
 * n_choose_k
 *		computes binomial coefficients using an algorithm that is both
 *		efficient and prevents overflows
 */
static int
n_choose_k(int n, int k)
{
	int			d,
				r;

	Assert((k > 0) && (n >= k));

	/* use symmetry of the binomial coefficients */
	k = Min(k, n - k);

	r = 1;
	for (d = 1; d <= k; ++d)
	{
		r *= n--;
		r /= d;
	}

	return r;
}

/*
 * num_combinations
 *		number of combinations, excluding single-value combinations
 */
static int
num_combinations(int n)
{
	return (1 << n) - (n + 1);
}

/*
 * generator_init
 *		initialize the generator of combinations
 *
 * The generator produces combinations of K elements in the interval (0..N).
 * We prebuild all the combinations in this method, which is simpler than
 * generating them on the fly.
 */
static CombinationGenerator *
generator_init(int n, int k)
{
	CombinationGenerator *state;

	Assert((n >= k) && (k > 0));

	/* allocate the generator state as a single chunk of memory */
	state = (CombinationGenerator *) palloc(sizeof(CombinationGenerator));

	state->ncombinations = n_choose_k(n, k);

	/* pre-allocate space for all combinations */
	state->combinations = (int *) palloc(sizeof(int) * k * state->ncombinations);

	state->current = 0;
	state->k = k;
	state->n = n;

	/* now actually pre-generate all the combinations of K elements */
	generate_combinations(state);

	/* make sure we got the expected number of combinations */
	Assert(state->current == state->ncombinations);

	/* reset the number, so we start with the first one */
	state->current = 0;

	return state;
}

/*
 * generator_next
 *		returns the next combination from the prebuilt list
 *
 * Returns a combination of K array indexes (0 .. N), as specified to
 * generator_init), or NULL when there are no more combination.
 */
static int *
generator_next(CombinationGenerator *state)
{
	if (state->current == state->ncombinations)
		return NULL;

	return &state->combinations[state->k * state->current++];
}

/*
 * generator_free
 *		free the internal state of the generator
 *
 * Releases the generator internal state (pre-built combinations).
 */
static void
generator_free(CombinationGenerator *state)
{
	pfree(state->combinations);
	pfree(state);
}

/*
 * generate_combinations_recurse
 *		given a prefix, generate all possible combinations
 *
 * Given a prefix (first few elements of the combination), generate following
 * elements recursively. We generate the combinations in lexicographic order,
 * which eliminates permutations of the same combination.
 */
static void
generate_combinations_recurse(CombinationGenerator *state,
							  int index, int start, int *current)
{
	/* If we haven't filled all the elements, simply recurse. */
	if (index < state->k)
	{
		int			i;

		/*
		 * The values have to be in ascending order, so make sure we start
		 * with the value passed by parameter.
		 */

		for (i = start; i < state->n; i++)
		{
			current[index] = i;
			generate_combinations_recurse(state, (index + 1), (i + 1), current);
		}

		return;
	}
	else
	{
		/* we got a valid combination, add it to the array */
		memcpy(&state->combinations[(state->k * state->current)],
			   current, state->k * sizeof(int));
		state->current++;
	}
}

/*
 * generate_combinations
 *		generate all k-combinations of N elements
 */
static void
generate_combinations(CombinationGenerator *state)
{
	int		   *current = (int *) palloc0(sizeof(int) * state->k);

	generate_combinations_recurse(state, 0, 0, current);

	pfree(current);
}
