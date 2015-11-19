#include "postgres.h"
#undef _
#include "fmgr.h"
#include "plperl.h"
#include "plperl_helpers.h"
#include "hstore.h"

PG_MODULE_MAGIC;


PG_FUNCTION_INFO_V1(hstore_to_plperl);

Datum
hstore_to_plperl(PG_FUNCTION_ARGS)
{
	HStore	   *in = PG_GETARG_HS(0);
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
	HV		   *hv;
	HE		   *he;
	int32		buflen;
	int32		i;
	int32		pcount;
	HStore	   *out;
	Pairs	   *pairs;

	hv = (HV *) SvRV((SV *) PG_GETARG_POINTER(0));

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
