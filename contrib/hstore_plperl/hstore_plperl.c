#include "postgres.h"

#include "fmgr.h"
#include "hstore/hstore.h"
#include "plperl.h"

PG_MODULE_MAGIC_EXT(
					.name = "hstore_plperl",
					.version = PG_VERSION
);

/* Linkage to functions in hstore module */
typedef HStore *(*hstoreUpgrade_t) (Datum orig);
static hstoreUpgrade_t hstoreUpgrade_p;
typedef int (*hstoreUniquePairs_t) (Pairs *a, int32 l, int32 *buflen);
static hstoreUniquePairs_t hstoreUniquePairs_p;
typedef HStore *(*hstorePairs_t) (Pairs *pairs, int32 pcount, int32 buflen);
static hstorePairs_t hstorePairs_p;
typedef size_t (*hstoreCheckKeyLen_t) (size_t len);
static hstoreCheckKeyLen_t hstoreCheckKeyLen_p;
typedef size_t (*hstoreCheckValLen_t) (size_t len);
static hstoreCheckValLen_t hstoreCheckValLen_p;


/*
 * Module initialize function: fetch function pointers for cross-module calls.
 */
void
_PG_init(void)
{
	/* Asserts verify that typedefs above match original declarations */
	AssertVariableIsOfType(&hstoreUpgrade, hstoreUpgrade_t);
	hstoreUpgrade_p = (hstoreUpgrade_t)
		load_external_function("$libdir/hstore", "hstoreUpgrade",
							   true, NULL);
	AssertVariableIsOfType(&hstoreUniquePairs, hstoreUniquePairs_t);
	hstoreUniquePairs_p = (hstoreUniquePairs_t)
		load_external_function("$libdir/hstore", "hstoreUniquePairs",
							   true, NULL);
	AssertVariableIsOfType(&hstorePairs, hstorePairs_t);
	hstorePairs_p = (hstorePairs_t)
		load_external_function("$libdir/hstore", "hstorePairs",
							   true, NULL);
	AssertVariableIsOfType(&hstoreCheckKeyLen, hstoreCheckKeyLen_t);
	hstoreCheckKeyLen_p = (hstoreCheckKeyLen_t)
		load_external_function("$libdir/hstore", "hstoreCheckKeyLen",
							   true, NULL);
	AssertVariableIsOfType(&hstoreCheckValLen, hstoreCheckValLen_t);
	hstoreCheckValLen_p = (hstoreCheckValLen_t)
		load_external_function("$libdir/hstore", "hstoreCheckValLen",
							   true, NULL);
}


/* These defines must be after the module init function */
#define hstoreUpgrade hstoreUpgrade_p
#define hstoreUniquePairs hstoreUniquePairs_p
#define hstorePairs hstorePairs_p
#define hstoreCheckKeyLen hstoreCheckKeyLen_p
#define hstoreCheckValLen hstoreCheckValLen_p


PG_FUNCTION_INFO_V1(hstore_to_plperl);

Datum
hstore_to_plperl(PG_FUNCTION_ARGS)
{
	dTHX;
	HStore	   *in = PG_GETARG_HSTORE_P(0);
	int			i;
	int			count = HS_COUNT(in);
	char	   *base = STRPTR(in);
	HEntry	   *entries = ARRPTR(in);
	HV		   *hv;

	hv = newHV();

	for (i = 0; i < count; i++)
	{
		const char *key;
		SV		   *value;

		key = pnstrdup(HSTORE_KEY(entries, base, i),
					   HSTORE_KEYLEN(entries, i));
		value = HSTORE_VALISNULL(entries, i) ? newSV(0) :
			cstr2sv(pnstrdup(HSTORE_VAL(entries, base, i),
							 HSTORE_VALLEN(entries, i)));

		(void) hv_store(hv, key, strlen(key), value, 0);
	}

	return PointerGetDatum(newRV((SV *) hv));
}


PG_FUNCTION_INFO_V1(plperl_to_hstore);

Datum
plperl_to_hstore(PG_FUNCTION_ARGS)
{
	dTHX;
	SV		   *in = (SV *) PG_GETARG_POINTER(0);
	HV		   *hv;
	HE		   *he;
	int32		buflen;
	int32		i;
	int32		pcount;
	HStore	   *out;
	Pairs	   *pairs;

	/* Dereference references recursively. */
	while (SvROK(in))
		in = SvRV(in);

	/* Now we must have a hash. */
	if (SvTYPE(in) != SVt_PVHV)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot transform non-hash Perl value to hstore")));
	hv = (HV *) in;

	pcount = hv_iterinit(hv);

	pairs = palloc(pcount * sizeof(Pairs));

	i = 0;
	while ((he = hv_iternext(hv)))
	{
		char	   *key = sv2cstr(HeSVKEY_force(he));
		SV		   *value = HeVAL(he);

		pairs[i].key = pstrdup(key);
		pairs[i].keylen = hstoreCheckKeyLen(strlen(pairs[i].key));
		pairs[i].needfree = true;

		if (!SvOK(value))
		{
			pairs[i].val = NULL;
			pairs[i].vallen = 0;
			pairs[i].isnull = true;
		}
		else
		{
			pairs[i].val = pstrdup(sv2cstr(value));
			pairs[i].vallen = hstoreCheckValLen(strlen(pairs[i].val));
			pairs[i].isnull = false;
		}

		i++;
	}

	pcount = hstoreUniquePairs(pairs, pcount, &buflen);
	out = hstorePairs(pairs, pcount, buflen);
	PG_RETURN_POINTER(out);
}
