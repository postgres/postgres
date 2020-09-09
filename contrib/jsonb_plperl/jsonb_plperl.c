#include "postgres.h"

#include <math.h>

#include "fmgr.h"
#include "plperl.h"
#include "plperl_helpers.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"

PG_MODULE_MAGIC;

static SV  *Jsonb_to_SV(JsonbContainer *jsonb);
static JsonbValue *SV_to_JsonbValue(SV *obj, JsonbParseState **ps, bool is_elem);


static SV  *
JsonbValue_to_SV(JsonbValue *jbv)
{
	dTHX;

	switch (jbv->type)
	{
		case jbvBinary:
			return Jsonb_to_SV(jbv->val.binary.data);

		case jbvNumeric:
			{
				char	   *str = DatumGetCString(DirectFunctionCall1(numeric_out,
																	  NumericGetDatum(jbv->val.numeric)));
				SV		   *result = newSVnv(SvNV(cstr2sv(str)));

				pfree(str);
				return result;
			}

		case jbvString:
			{
				char	   *str = pnstrdup(jbv->val.string.val,
										   jbv->val.string.len);
				SV		   *result = cstr2sv(str);

				pfree(str);
				return result;
			}

		case jbvBool:
			return newSVnv(SvNV(jbv->val.boolean ? &PL_sv_yes : &PL_sv_no));

		case jbvNull:
			return newSV(0);

		default:
			elog(ERROR, "unexpected jsonb value type: %d", jbv->type);
			return NULL;
	}
}

static SV  *
Jsonb_to_SV(JsonbContainer *jsonb)
{
	dTHX;
	JsonbValue	v;
	JsonbIterator *it;
	JsonbIteratorToken r;

	it = JsonbIteratorInit(jsonb);
	r = JsonbIteratorNext(&it, &v, true);

	switch (r)
	{
		case WJB_BEGIN_ARRAY:
			if (v.val.array.rawScalar)
			{
				JsonbValue	tmp;

				if ((r = JsonbIteratorNext(&it, &v, true)) != WJB_ELEM ||
					(r = JsonbIteratorNext(&it, &tmp, true)) != WJB_END_ARRAY ||
					(r = JsonbIteratorNext(&it, &tmp, true)) != WJB_DONE)
					elog(ERROR, "unexpected jsonb token: %d", r);

				return JsonbValue_to_SV(&v);
			}
			else
			{
				AV		   *av = newAV();

				while ((r = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
				{
					if (r == WJB_ELEM)
						av_push(av, JsonbValue_to_SV(&v));
				}

				return newRV((SV *) av);
			}

		case WJB_BEGIN_OBJECT:
			{
				HV		   *hv = newHV();

				while ((r = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
				{
					if (r == WJB_KEY)
					{
						/* json key in v, json value in val */
						JsonbValue	val;

						if (JsonbIteratorNext(&it, &val, true) == WJB_VALUE)
						{
							SV		   *value = JsonbValue_to_SV(&val);

							(void) hv_store(hv,
											v.val.string.val, v.val.string.len,
											value, 0);
						}
					}
				}

				return newRV((SV *) hv);
			}

		default:
			elog(ERROR, "unexpected jsonb token: %d", r);
			return NULL;
	}
}

static JsonbValue *
AV_to_JsonbValue(AV *in, JsonbParseState **jsonb_state)
{
	dTHX;
	SSize_t		pcount = av_len(in) + 1;
	SSize_t		i;

	pushJsonbValue(jsonb_state, WJB_BEGIN_ARRAY, NULL);

	for (i = 0; i < pcount; i++)
	{
		SV		  **value = av_fetch(in, i, FALSE);

		if (value)
			(void) SV_to_JsonbValue(*value, jsonb_state, true);
	}

	return pushJsonbValue(jsonb_state, WJB_END_ARRAY, NULL);
}

static JsonbValue *
HV_to_JsonbValue(HV *obj, JsonbParseState **jsonb_state)
{
	dTHX;
	JsonbValue	key;
	SV		   *val;
	char	   *kstr;
	I32			klen;

	key.type = jbvString;

	pushJsonbValue(jsonb_state, WJB_BEGIN_OBJECT, NULL);

	(void) hv_iterinit(obj);

	while ((val = hv_iternextsv(obj, &kstr, &klen)))
	{
		key.val.string.val = pnstrdup(kstr, klen);
		key.val.string.len = klen;
		pushJsonbValue(jsonb_state, WJB_KEY, &key);
		(void) SV_to_JsonbValue(val, jsonb_state, false);
	}

	return pushJsonbValue(jsonb_state, WJB_END_OBJECT, NULL);
}

static JsonbValue *
SV_to_JsonbValue(SV *in, JsonbParseState **jsonb_state, bool is_elem)
{
	dTHX;
	JsonbValue	out;			/* result */

	/* Dereference references recursively. */
	while (SvROK(in))
		in = SvRV(in);

	switch (SvTYPE(in))
	{
		case SVt_PVAV:
			return AV_to_JsonbValue((AV *) in, jsonb_state);

		case SVt_PVHV:
			return HV_to_JsonbValue((HV *) in, jsonb_state);

		default:
			if (!SvOK(in))
			{
				out.type = jbvNull;
			}
			else if (SvUOK(in))
			{
				/*
				 * If UV is >=64 bits, we have no better way to make this
				 * happen than converting to text and back.  Given the low
				 * usage of UV in Perl code, it's not clear it's worth working
				 * hard to provide alternate code paths.
				 */
				const char *strval = SvPV_nolen(in);

				out.type = jbvNumeric;
				out.val.numeric =
					DatumGetNumeric(DirectFunctionCall3(numeric_in,
														CStringGetDatum(strval),
														ObjectIdGetDatum(InvalidOid),
														Int32GetDatum(-1)));
			}
			else if (SvIOK(in))
			{
				IV			ival = SvIV(in);

				out.type = jbvNumeric;
				out.val.numeric = int64_to_numeric(ival);
			}
			else if (SvNOK(in))
			{
				double		nval = SvNV(in);

				/*
				 * jsonb doesn't allow infinity or NaN (per JSON
				 * specification), but the numeric type that is used for the
				 * storage accepts those, so we have to reject them here
				 * explicitly.
				 */
				if (isinf(nval))
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("cannot convert infinity to jsonb")));
				if (isnan(nval))
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("cannot convert NaN to jsonb")));

				out.type = jbvNumeric;
				out.val.numeric =
					DatumGetNumeric(DirectFunctionCall1(float8_numeric,
														Float8GetDatum(nval)));
			}
			else if (SvPOK(in))
			{
				out.type = jbvString;
				out.val.string.val = sv2cstr(in);
				out.val.string.len = strlen(out.val.string.val);
			}
			else
			{
				/*
				 * XXX It might be nice if we could include the Perl type in
				 * the error message.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot transform this Perl type to jsonb")));
				return NULL;
			}
	}

	/* Push result into 'jsonb_state' unless it is a raw scalar. */
	return *jsonb_state
		? pushJsonbValue(jsonb_state, is_elem ? WJB_ELEM : WJB_VALUE, &out)
		: memcpy(palloc(sizeof(JsonbValue)), &out, sizeof(JsonbValue));
}


PG_FUNCTION_INFO_V1(jsonb_to_plperl);

Datum
jsonb_to_plperl(PG_FUNCTION_ARGS)
{
	dTHX;
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	SV		   *sv = Jsonb_to_SV(&in->root);

	return PointerGetDatum(sv);
}


PG_FUNCTION_INFO_V1(plperl_to_jsonb);

Datum
plperl_to_jsonb(PG_FUNCTION_ARGS)
{
	dTHX;
	JsonbParseState *jsonb_state = NULL;
	SV		   *in = (SV *) PG_GETARG_POINTER(0);
	JsonbValue *out = SV_to_JsonbValue(in, &jsonb_state, true);
	Jsonb	   *result = JsonbValueToJsonb(out);

	PG_RETURN_JSONB_P(result);
}
