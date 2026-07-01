/*
 * contrib/btree_gist/btree_utils_num.h
 */
#ifndef __BTREE_UTILS_NUM_H__
#define __BTREE_UTILS_NUM_H__

#include <math.h>
#include <float.h>

#include "access/gist.h"
#include "btree_gist.h"
#include "utils/float.h"
#include "utils/rel.h"

typedef char GBT_NUMKEY;

/* Better readable key */
typedef struct
{
	const GBT_NUMKEY *lower,
			   *upper;
} GBT_NUMKEY_R;


/* for sorting */
typedef struct
{
	int			i;
	GBT_NUMKEY *t;
} Nsrt;


/* type description */

typedef struct
{

	/* Attribs */

	enum gbtree_type t;			/* data type */
	int32		size;			/* size of type, 0 means variable */
	int32		indexsize;		/* size of datums stored in index */

	/* Methods */

	bool		(*f_gt) (const void *, const void *, FmgrInfo *);	/* greater than */
	bool		(*f_ge) (const void *, const void *, FmgrInfo *);	/* greater or equal */
	bool		(*f_eq) (const void *, const void *, FmgrInfo *);	/* equal */
	bool		(*f_le) (const void *, const void *, FmgrInfo *);	/* less or equal */
	bool		(*f_lt) (const void *, const void *, FmgrInfo *);	/* less than */
	int			(*f_cmp) (const void *, const void *, FmgrInfo *);	/* key compare function */
	float8		(*f_dist) (const void *, const void *, FmgrInfo *); /* key distance function */
} gbtree_ninfo;


/*
 *	Numeric btree functions
 */



/*
 * Compute penalty for expanding a range olower..oupper to nlower..nupper.
 *
 * Although the arguments are declared double, they must not be NaN nor
 * large enough to risk overflows in the calculations herein.  We only
 * actually use this for integral data types, so there's no hazard.
 */
static inline float
penalty_num_impl(double olower, double oupper,
				 double nlower, double nupper,
				 int natts)
{
	float		result = 0.0F;
	double		tmp = 0.0;

	/* Add penalty for expanding upper bound */
	if (nupper > oupper)
		tmp += nupper - oupper;
	/* Add penalty for expanding lower bound */
	if (olower > nlower)
		tmp += olower - nlower;
	if (tmp > 0.0)
	{
		/* Ensure result is non-zero, even if next step underflows to zero */
		result += FLT_MIN;
		/* Scale penalty to 0 .. 1 */
		result += (float) (tmp / (tmp + (oupper - olower)));
		/* Scale to 0 .. FLT_MAX / (natts + 1) */
		result *= FLT_MAX / (natts + 1);
	}
	return result;
}

/*
 * As above, but the input values are float4 or float8, so we must cope
 * with NaNs, infinities, and overflows.
 */
static inline float
float_penalty_num_impl(double olower, double oupper,
					   double nlower, double nupper,
					   int natts)
{
	float		result = 0.0F;
	double		tmp = 0.0;

	/* Add penalty for expanding upper bound */
	if (float8_gt(nupper, oupper))
	{
		double		delta = nupper - oupper;

		if (unlikely(isnan(delta)))
		{
			/* oupper couldn't be NaN here, see float8_gt */
			if (isnan(nupper))
				delta = FLT_MAX;	/* max penalty for NaN vs non-NaN */
			else
				delta = 0.0;	/* must be Inf - Inf case */
		}
		else if (delta > FLT_MAX)
			delta = FLT_MAX;	/* clamp to FLT_MAX, esp for infinity */
		tmp += delta;
	}
	/* Add penalty for expanding lower bound */
	if (float8_gt(olower, nlower))
	{
		double		delta = olower - nlower;

		if (unlikely(isnan(delta)))
		{
			/* nlower couldn't be NaN here, see float8_gt */
			if (isnan(olower))
				delta = FLT_MAX;	/* max penalty for NaN vs non-NaN */
			else
				delta = 0.0;	/* must be Inf - Inf case */
		}
		else if (delta > FLT_MAX)
			delta = FLT_MAX;	/* clamp to FLT_MAX, esp for infinity */
		tmp += delta;
	}
	if (tmp > 0.0)
	{
		double		delta = oupper - olower;

		/* Clamp delta (the original range size) to 0 .. FLT_MAX */
		if (unlikely(isnan(delta)))
		{
			/* here, we must deal with olower possibly being NaN */
			if (isnan(oupper) && isnan(olower))
				delta = 0.0;	/* treat NaNs as equal */
			else if (isnan(oupper) || isnan(olower))
				delta = FLT_MAX;	/* max penalty for NaN vs non-NaN */
			else
				delta = 0.0;	/* must be Inf - Inf case */
		}
		else if (delta > FLT_MAX)
			delta = FLT_MAX;	/* clamp to FLT_MAX, esp for infinity */
		/* Ensure result is non-zero, even if next step underflows to zero */
		result += FLT_MIN;
		/* Scale penalty to 0 .. 1 */
		result += (float) (tmp / (tmp + delta));
		/* Scale to 0 .. FLT_MAX / (natts + 1) */
		result *= FLT_MAX / (natts + 1);
	}
	return result;
}

/*
 * These macros provide backwards-compatible notation for callers.
 */
#define penalty_num(result,olower,oupper,nlower,nupper) do { \
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0); \
	*(result) = penalty_num_impl(olower, oupper, nlower, nupper, \
								 entry->rel->rd_att->natts); \
} while (0)

#define float_penalty_num(result,olower,oupper,nlower,nupper) do { \
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0); \
	*(result) = float_penalty_num_impl(olower, oupper, nlower, nupper, \
									   entry->rel->rd_att->natts); \
} while (0)


/*
 * Convert an Interval to an approximate equivalent number of seconds
 * (as a double).  Here because we need it for time/timetz as well as
 * interval.  See interval_cmp_internal for comparison.
 */
#define INTERVAL_TO_SEC(ivp) \
	(((double) (ivp)->time) / ((double) USECS_PER_SEC) + \
	 (ivp)->day * (24.0 * SECS_PER_HOUR) + \
	 (ivp)->month * (30.0 * SECS_PER_DAY))

/* This macro is not safe to use with actual float inputs, only integers */
#define GET_FLOAT_DISTANCE(t, arg1, arg2)	Abs( ((float8) *((const t *) (arg1))) - ((float8) *((const t *) (arg2))) )


extern Interval *abs_interval(Interval *a);

extern bool gbt_num_consistent(const GBT_NUMKEY_R *key, const void *query,
							   const StrategyNumber *strategy, bool is_leaf,
							   const gbtree_ninfo *tinfo, FmgrInfo *flinfo);

extern float8 gbt_num_distance(const GBT_NUMKEY_R *key, const void *query,
							   bool is_leaf, const gbtree_ninfo *tinfo, FmgrInfo *flinfo);

extern GIST_SPLITVEC *gbt_num_picksplit(const GistEntryVector *entryvec, GIST_SPLITVEC *v,
										const gbtree_ninfo *tinfo, FmgrInfo *flinfo);

extern GISTENTRY *gbt_num_compress(GISTENTRY *entry, const gbtree_ninfo *tinfo);

extern GISTENTRY *gbt_num_fetch(GISTENTRY *entry, const gbtree_ninfo *tinfo);

extern void *gbt_num_union(GBT_NUMKEY *out, const GistEntryVector *entryvec,
						   const gbtree_ninfo *tinfo, FmgrInfo *flinfo);

extern bool gbt_num_same(const GBT_NUMKEY *a, const GBT_NUMKEY *b,
						 const gbtree_ninfo *tinfo, FmgrInfo *flinfo);

extern void gbt_num_bin_union(Datum *u, GBT_NUMKEY *e,
							  const gbtree_ninfo *tinfo, FmgrInfo *flinfo);

#endif
