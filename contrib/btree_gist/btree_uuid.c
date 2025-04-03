/*
 * contrib/btree_gist/btree_uuid.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_num.h"
#include "port/pg_bswap.h"
#include "utils/sortsupport.h"
#include "utils/uuid.h"

typedef struct
{
	pg_uuid_t	lower,
				upper;
} uuidKEY;


/* GiST support functions */
PG_FUNCTION_INFO_V1(gbt_uuid_compress);
PG_FUNCTION_INFO_V1(gbt_uuid_fetch);
PG_FUNCTION_INFO_V1(gbt_uuid_union);
PG_FUNCTION_INFO_V1(gbt_uuid_picksplit);
PG_FUNCTION_INFO_V1(gbt_uuid_consistent);
PG_FUNCTION_INFO_V1(gbt_uuid_penalty);
PG_FUNCTION_INFO_V1(gbt_uuid_same);
PG_FUNCTION_INFO_V1(gbt_uuid_sortsupport);


static int
uuid_internal_cmp(const pg_uuid_t *arg1, const pg_uuid_t *arg2)
{
	return memcmp(arg1->data, arg2->data, UUID_LEN);
}

static bool
gbt_uuidgt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return uuid_internal_cmp((const pg_uuid_t *) a, (const pg_uuid_t *) b) > 0;
}

static bool
gbt_uuidge(const void *a, const void *b, FmgrInfo *flinfo)
{
	return uuid_internal_cmp((const pg_uuid_t *) a, (const pg_uuid_t *) b) >= 0;
}

static bool
gbt_uuideq(const void *a, const void *b, FmgrInfo *flinfo)
{
	return uuid_internal_cmp((const pg_uuid_t *) a, (const pg_uuid_t *) b) == 0;
}

static bool
gbt_uuidle(const void *a, const void *b, FmgrInfo *flinfo)
{
	return uuid_internal_cmp((const pg_uuid_t *) a, (const pg_uuid_t *) b) <= 0;
}

static bool
gbt_uuidlt(const void *a, const void *b, FmgrInfo *flinfo)
{
	return uuid_internal_cmp((const pg_uuid_t *) a, (const pg_uuid_t *) b) < 0;
}

static int
gbt_uuidkey_cmp(const void *a, const void *b, FmgrInfo *flinfo)
{
	uuidKEY    *ia = (uuidKEY *) (((const Nsrt *) a)->t);
	uuidKEY    *ib = (uuidKEY *) (((const Nsrt *) b)->t);
	int			res;

	res = uuid_internal_cmp(&ia->lower, &ib->lower);
	if (res == 0)
		res = uuid_internal_cmp(&ia->upper, &ib->upper);
	return res;
}


static const gbtree_ninfo tinfo =
{
	gbt_t_uuid,
	UUID_LEN,
	32,							/* sizeof(gbtreekey32) */
	gbt_uuidgt,
	gbt_uuidge,
	gbt_uuideq,
	gbt_uuidle,
	gbt_uuidlt,
	gbt_uuidkey_cmp,
	NULL
};


/**************************************************
 * GiST support functions
 **************************************************/

Datum
gbt_uuid_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		char	   *r = (char *) palloc(2 * UUID_LEN);
		pg_uuid_t  *key = DatumGetUUIDP(entry->key);

		retval = palloc(sizeof(GISTENTRY));

		memcpy(r, key, UUID_LEN);
		memcpy(r + UUID_LEN, key, UUID_LEN);
		gistentryinit(*retval, PointerGetDatum(r),
					  entry->rel, entry->page,
					  entry->offset, false);
	}
	else
		retval = entry;

	PG_RETURN_POINTER(retval);
}

Datum
gbt_uuid_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_num_fetch(entry, &tinfo));
}

Datum
gbt_uuid_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	pg_uuid_t  *query = PG_GETARG_UUID_P(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	uuidKEY    *kkk = (uuidKEY *) DatumGetPointer(entry->key);
	GBT_NUMKEY_R key;

	/* All cases served by this function are exact */
	*recheck = false;

	key.lower = (GBT_NUMKEY *) &kkk->lower;
	key.upper = (GBT_NUMKEY *) &kkk->upper;

	PG_RETURN_BOOL(gbt_num_consistent(&key, query, &strategy,
									  GIST_LEAF(entry), &tinfo,
									  fcinfo->flinfo));
}

Datum
gbt_uuid_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	void	   *out = palloc(sizeof(uuidKEY));

	*(int *) PG_GETARG_POINTER(1) = sizeof(uuidKEY);
	PG_RETURN_POINTER(gbt_num_union(out, entryvec, &tinfo, fcinfo->flinfo));
}

/*
 * Convert a uuid to a "double" value for estimating sizes of ranges.
 */
static double
uuid_2_double(const pg_uuid_t *u)
{
	uint64		uu[2];
	const double two64 = 18446744073709551616.0;	/* 2^64 */

	/* Source data may not be suitably aligned, so copy */
	memcpy(uu, u->data, UUID_LEN);

	/*
	 * uuid values should be considered as big-endian numbers, since that
	 * corresponds to how memcmp will compare them.  On a little-endian
	 * machine, byte-swap each half so we can use native uint64 arithmetic.
	 */
#ifndef WORDS_BIGENDIAN
	uu[0] = pg_bswap64(uu[0]);
	uu[1] = pg_bswap64(uu[1]);
#endif

	/*
	 * 2^128 is about 3.4e38, which in theory could exceed the range of
	 * "double" (POSIX only requires 1e37).  To avoid any risk of overflow,
	 * put the decimal point between the two halves rather than treating the
	 * uuid value as a 128-bit integer.
	 */
	return (double) uu[0] + (double) uu[1] / two64;
}

Datum
gbt_uuid_penalty(PG_FUNCTION_ARGS)
{
	uuidKEY    *origentry = (uuidKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	uuidKEY    *newentry = (uuidKEY *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	double		olower,
				oupper,
				nlower,
				nupper;

	olower = uuid_2_double(&origentry->lower);
	oupper = uuid_2_double(&origentry->upper);
	nlower = uuid_2_double(&newentry->lower);
	nupper = uuid_2_double(&newentry->upper);

	penalty_num(result, olower, oupper, nlower, nupper);

	PG_RETURN_POINTER(result);
}

Datum
gbt_uuid_picksplit(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(gbt_num_picksplit((GistEntryVector *) PG_GETARG_POINTER(0),
										(GIST_SPLITVEC *) PG_GETARG_POINTER(1),
										&tinfo, fcinfo->flinfo));
}

Datum
gbt_uuid_same(PG_FUNCTION_ARGS)
{
	uuidKEY    *b1 = (uuidKEY *) PG_GETARG_POINTER(0);
	uuidKEY    *b2 = (uuidKEY *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_num_same((void *) b1, (void *) b2, &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}

static int
gbt_uuid_ssup_cmp(Datum x, Datum y, SortSupport ssup)
{
	uuidKEY    *arg1 = (uuidKEY *) DatumGetPointer(x);
	uuidKEY    *arg2 = (uuidKEY *) DatumGetPointer(y);

	/* for leaf items we expect lower == upper, so only compare lower */
	return uuid_internal_cmp(&arg1->lower, &arg2->lower);
}

Datum
gbt_uuid_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = gbt_uuid_ssup_cmp;
	ssup->ssup_extra = NULL;

	PG_RETURN_VOID();
}
