/*-------------------------------------------------------------------------
 *
 * tsearch2.c
 *		Backwards-compatibility package for old contrib/tsearch2 API
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  contrib/tsearch2/tsearch2.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

static Oid	current_dictionary_oid = InvalidOid;
static Oid	current_parser_oid = InvalidOid;

/* insert given value at argument position 0 */
#define INSERT_ARGUMENT0(argument, isnull)				\
	do {												\
		int i;											\
		for (i = fcinfo->nargs; i > 0; i--)				\
		{												\
			fcinfo->arg[i] = fcinfo->arg[i-1];			\
			fcinfo->argnull[i] = fcinfo->argnull[i-1];	\
		}												\
		fcinfo->arg[0] = (argument);					\
		fcinfo->argnull[0] = (isnull);					\
		fcinfo->nargs++;								\
	} while (0)

#define TextGetObjectId(infunction, text) \
	DatumGetObjectId(DirectFunctionCall1(infunction, \
					 CStringGetDatum(text_to_cstring(text))))

#define UNSUPPORTED_FUNCTION(name)						\
	PG_FUNCTION_INFO_V1(name);							\
	Datum												\
	name(PG_FUNCTION_ARGS)								\
	{													\
		ereport(ERROR,									\
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),\
				 errmsg("function %s is no longer supported", \
						format_procedure(fcinfo->flinfo->fn_oid)), \
				 errhint("Switch to new tsearch functionality."))); \
		/* keep compiler quiet */						\
		PG_RETURN_NULL();								\
	}													\
	extern int no_such_variable

static Oid	GetCurrentDict(void);
static Oid	GetCurrentParser(void);

PG_FUNCTION_INFO_V1(tsa_lexize_byname);
PG_FUNCTION_INFO_V1(tsa_lexize_bycurrent);
PG_FUNCTION_INFO_V1(tsa_set_curdict);
PG_FUNCTION_INFO_V1(tsa_set_curdict_byname);
PG_FUNCTION_INFO_V1(tsa_token_type_current);
PG_FUNCTION_INFO_V1(tsa_set_curprs);
PG_FUNCTION_INFO_V1(tsa_set_curprs_byname);
PG_FUNCTION_INFO_V1(tsa_parse_current);
PG_FUNCTION_INFO_V1(tsa_set_curcfg);
PG_FUNCTION_INFO_V1(tsa_set_curcfg_byname);
PG_FUNCTION_INFO_V1(tsa_to_tsvector_name);
PG_FUNCTION_INFO_V1(tsa_to_tsquery_name);
PG_FUNCTION_INFO_V1(tsa_plainto_tsquery_name);
PG_FUNCTION_INFO_V1(tsa_headline_byname);
PG_FUNCTION_INFO_V1(tsa_ts_stat);
PG_FUNCTION_INFO_V1(tsa_tsearch2);
PG_FUNCTION_INFO_V1(tsa_rewrite_accum);
PG_FUNCTION_INFO_V1(tsa_rewrite_finish);


/*
 * List of unsupported functions
 *
 * The parser and dictionary functions are defined only so that the former
 * contents of pg_ts_parser and pg_ts_dict can be loaded into the system,
 * for ease of reference while creating the new tsearch configuration.
 */

UNSUPPORTED_FUNCTION(tsa_dex_init);
UNSUPPORTED_FUNCTION(tsa_dex_lexize);

UNSUPPORTED_FUNCTION(tsa_snb_en_init);
UNSUPPORTED_FUNCTION(tsa_snb_lexize);
UNSUPPORTED_FUNCTION(tsa_snb_ru_init_koi8);
UNSUPPORTED_FUNCTION(tsa_snb_ru_init_utf8);
UNSUPPORTED_FUNCTION(tsa_snb_ru_init);

UNSUPPORTED_FUNCTION(tsa_spell_init);
UNSUPPORTED_FUNCTION(tsa_spell_lexize);

UNSUPPORTED_FUNCTION(tsa_syn_init);
UNSUPPORTED_FUNCTION(tsa_syn_lexize);

UNSUPPORTED_FUNCTION(tsa_thesaurus_init);
UNSUPPORTED_FUNCTION(tsa_thesaurus_lexize);

UNSUPPORTED_FUNCTION(tsa_prsd_start);
UNSUPPORTED_FUNCTION(tsa_prsd_getlexeme);
UNSUPPORTED_FUNCTION(tsa_prsd_end);
UNSUPPORTED_FUNCTION(tsa_prsd_lextype);
UNSUPPORTED_FUNCTION(tsa_prsd_headline);

UNSUPPORTED_FUNCTION(tsa_reset_tsearch);
UNSUPPORTED_FUNCTION(tsa_get_covers);


/*
 * list of redefined functions
 */

/* lexize(text, text) */
Datum
tsa_lexize_byname(PG_FUNCTION_ARGS)
{
	text	   *dictname = PG_GETARG_TEXT_PP(0);
	Datum		arg1 = PG_GETARG_DATUM(1);

	return DirectFunctionCall2(ts_lexize,
				ObjectIdGetDatum(TextGetObjectId(regdictionaryin, dictname)),
							   arg1);
}

/* lexize(text) */
Datum
tsa_lexize_bycurrent(PG_FUNCTION_ARGS)
{
	Datum		arg0 = PG_GETARG_DATUM(0);
	Oid			id = GetCurrentDict();

	return DirectFunctionCall2(ts_lexize,
							   ObjectIdGetDatum(id),
							   arg0);
}

/* set_curdict(int) */
Datum
tsa_set_curdict(PG_FUNCTION_ARGS)
{
	Oid			dict_oid = PG_GETARG_OID(0);

	if (!SearchSysCacheExists(TSDICTOID,
							  ObjectIdGetDatum(dict_oid),
							  0, 0, 0))
		elog(ERROR, "cache lookup failed for text search dictionary %u",
			 dict_oid);

	current_dictionary_oid = dict_oid;

	PG_RETURN_VOID();
}

/* set_curdict(text) */
Datum
tsa_set_curdict_byname(PG_FUNCTION_ARGS)
{
	text	   *name = PG_GETARG_TEXT_PP(0);
	Oid			dict_oid;

	dict_oid = get_ts_dict_oid(stringToQualifiedNameList(text_to_cstring(name)), false);

	current_dictionary_oid = dict_oid;

	PG_RETURN_VOID();
}

/* token_type() */
Datum
tsa_token_type_current(PG_FUNCTION_ARGS)
{
	INSERT_ARGUMENT0(ObjectIdGetDatum(GetCurrentParser()), false);
	return ts_token_type_byid(fcinfo);
}

/* set_curprs(int) */
Datum
tsa_set_curprs(PG_FUNCTION_ARGS)
{
	Oid			parser_oid = PG_GETARG_OID(0);

	if (!SearchSysCacheExists(TSPARSEROID,
							  ObjectIdGetDatum(parser_oid),
							  0, 0, 0))
		elog(ERROR, "cache lookup failed for text search parser %u",
			 parser_oid);

	current_parser_oid = parser_oid;

	PG_RETURN_VOID();
}

/* set_curprs(text) */
Datum
tsa_set_curprs_byname(PG_FUNCTION_ARGS)
{
	text	   *name = PG_GETARG_TEXT_PP(0);
	Oid			parser_oid;

	parser_oid = get_ts_parser_oid(stringToQualifiedNameList(text_to_cstring(name)), false);

	current_parser_oid = parser_oid;

	PG_RETURN_VOID();
}

/* parse(text) */
Datum
tsa_parse_current(PG_FUNCTION_ARGS)
{
	INSERT_ARGUMENT0(ObjectIdGetDatum(GetCurrentParser()), false);
	return ts_parse_byid(fcinfo);
}

/* set_curcfg(int) */
Datum
tsa_set_curcfg(PG_FUNCTION_ARGS)
{
	Oid			arg0 = PG_GETARG_OID(0);
	char	   *name;

	name = DatumGetCString(DirectFunctionCall1(regconfigout,
											   ObjectIdGetDatum(arg0)));

	SetConfigOption("default_text_search_config", name,
					PGC_USERSET, PGC_S_SESSION);

	PG_RETURN_VOID();
}

/* set_curcfg(text) */
Datum
tsa_set_curcfg_byname(PG_FUNCTION_ARGS)
{
	text	   *arg0 = PG_GETARG_TEXT_PP(0);
	char	   *name;

	name = text_to_cstring(arg0);

	SetConfigOption("default_text_search_config", name,
					PGC_USERSET, PGC_S_SESSION);

	PG_RETURN_VOID();
}

/* to_tsvector(text, text) */
Datum
tsa_to_tsvector_name(PG_FUNCTION_ARGS)
{
	text	   *cfgname = PG_GETARG_TEXT_PP(0);
	Datum		arg1 = PG_GETARG_DATUM(1);
	Oid			config_oid;

	config_oid = TextGetObjectId(regconfigin, cfgname);

	return DirectFunctionCall2(to_tsvector_byid,
							   ObjectIdGetDatum(config_oid), arg1);
}

/* to_tsquery(text, text) */
Datum
tsa_to_tsquery_name(PG_FUNCTION_ARGS)
{
	text	   *cfgname = PG_GETARG_TEXT_PP(0);
	Datum		arg1 = PG_GETARG_DATUM(1);
	Oid			config_oid;

	config_oid = TextGetObjectId(regconfigin, cfgname);

	return DirectFunctionCall2(to_tsquery_byid,
							   ObjectIdGetDatum(config_oid), arg1);
}


/* plainto_tsquery(text, text) */
Datum
tsa_plainto_tsquery_name(PG_FUNCTION_ARGS)
{
	text	   *cfgname = PG_GETARG_TEXT_PP(0);
	Datum		arg1 = PG_GETARG_DATUM(1);
	Oid			config_oid;

	config_oid = TextGetObjectId(regconfigin, cfgname);

	return DirectFunctionCall2(plainto_tsquery_byid,
							   ObjectIdGetDatum(config_oid), arg1);
}

/* headline(text, text, tsquery [,text]) */
Datum
tsa_headline_byname(PG_FUNCTION_ARGS)
{
	Datum		arg0 = PG_GETARG_DATUM(0);
	Datum		arg1 = PG_GETARG_DATUM(1);
	Datum		arg2 = PG_GETARG_DATUM(2);
	Datum		result;
	Oid			config_oid;

	/* first parameter has to be converted to oid */
	config_oid = DatumGetObjectId(DirectFunctionCall1(regconfigin,
								CStringGetDatum(TextDatumGetCString(arg0))));

	if (PG_NARGS() == 3)
		result = DirectFunctionCall3(ts_headline_byid,
								   ObjectIdGetDatum(config_oid), arg1, arg2);
	else
	{
		Datum		arg3 = PG_GETARG_DATUM(3);

		result = DirectFunctionCall4(ts_headline_byid_opt,
									 ObjectIdGetDatum(config_oid),
									 arg1, arg2, arg3);
	}

	return result;
}

/*
 * tsearch2 version of update trigger
 *
 * We pass this on to the core trigger after inserting the default text
 * search configuration name as the second argument.  Note that this isn't
 * a complete implementation of the original functionality; tsearch2 allowed
 * transformation function names to be included in the list.  However, that
 * is deliberately removed as being a security risk.
 */
Datum
tsa_tsearch2(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata;
	Trigger    *trigger;
	char	  **tgargs,
			  **tgargs_old;
	int			i;
	Datum		res;

	/* Check call context */
	if (!CALLED_AS_TRIGGER(fcinfo))		/* internal error */
		elog(ERROR, "tsvector_update_trigger: not fired by trigger manager");

	trigdata = (TriggerData *) fcinfo->context;
	trigger = trigdata->tg_trigger;

	if (trigger->tgnargs < 2)
		elog(ERROR, "TSearch: format tsearch2(tsvector_field, text_field1,...)");

	/* create space for configuration name */
	tgargs = (char **) palloc((trigger->tgnargs + 1) * sizeof(char *));
	tgargs[0] = trigger->tgargs[0];
	for (i = 1; i < trigger->tgnargs; i++)
		tgargs[i + 1] = trigger->tgargs[i];

	tgargs[1] = pstrdup(GetConfigOptionByName("default_text_search_config",
											  NULL));
	tgargs_old = trigger->tgargs;
	trigger->tgargs = tgargs;
	trigger->tgnargs++;

	res = tsvector_update_trigger_byid(fcinfo);

	/* restore old trigger data */
	trigger->tgargs = tgargs_old;
	trigger->tgnargs--;

	pfree(tgargs[1]);
	pfree(tgargs);

	return res;
}


Datum
tsa_rewrite_accum(PG_FUNCTION_ARGS)
{
	TSQuery		acc;
	ArrayType  *qa;
	TSQuery		q;
	QTNode	   *qex = NULL,
			   *subs = NULL,
			   *acctree = NULL;
	bool		isfind = false;
	Datum	   *elemsp;
	int			nelemsp;
	MemoryContext aggcontext;
	MemoryContext oldcontext;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
		elog(ERROR, "tsa_rewrite_accum called in non-aggregate context");

	if (PG_ARGISNULL(0) || PG_GETARG_POINTER(0) == NULL)
	{
		acc = (TSQuery) MemoryContextAlloc(aggcontext, HDRSIZETQ);
		SET_VARSIZE(acc, HDRSIZETQ);
		acc->size = 0;
	}
	else
		acc = PG_GETARG_TSQUERY(0);

	if (PG_ARGISNULL(1) || PG_GETARG_POINTER(1) == NULL)
		PG_RETURN_TSQUERY(acc);
	else
		qa = PG_GETARG_ARRAYTYPE_P_COPY(1);

	if (ARR_NDIM(qa) != 1)
		elog(ERROR, "array must be one-dimensional, not %d dimensions",
			 ARR_NDIM(qa));
	if (ArrayGetNItems(ARR_NDIM(qa), ARR_DIMS(qa)) != 3)
		elog(ERROR, "array must have three elements");
	if (ARR_ELEMTYPE(qa) != TSQUERYOID)
		elog(ERROR, "array must contain tsquery elements");

	deconstruct_array(qa, TSQUERYOID, -1, false, 'i', &elemsp, NULL, &nelemsp);

	q = DatumGetTSQuery(elemsp[0]);
	if (q->size == 0)
	{
		pfree(elemsp);
		PG_RETURN_POINTER(acc);
	}

	if (!acc->size)
	{
		if (VARSIZE(acc) > HDRSIZETQ)
		{
			pfree(elemsp);
			PG_RETURN_POINTER(acc);
		}
		else
			acctree = QT2QTN(GETQUERY(q), GETOPERAND(q));
	}
	else
		acctree = QT2QTN(GETQUERY(acc), GETOPERAND(acc));

	QTNTernary(acctree);
	QTNSort(acctree);

	q = DatumGetTSQuery(elemsp[1]);
	if (q->size == 0)
	{
		pfree(elemsp);
		PG_RETURN_POINTER(acc);
	}
	qex = QT2QTN(GETQUERY(q), GETOPERAND(q));
	QTNTernary(qex);
	QTNSort(qex);

	q = DatumGetTSQuery(elemsp[2]);
	if (q->size)
		subs = QT2QTN(GETQUERY(q), GETOPERAND(q));

	acctree = findsubquery(acctree, qex, subs, &isfind);

	if (isfind || !acc->size)
	{
		/* pfree( acc ); do not pfree(p), because nodeAgg.c will */
		if (acctree)
		{
			QTNBinary(acctree);
			oldcontext = MemoryContextSwitchTo(aggcontext);
			acc = QTN2QT(acctree);
			MemoryContextSwitchTo(oldcontext);
		}
		else
		{
			acc = (TSQuery) MemoryContextAlloc(aggcontext, HDRSIZETQ);
			SET_VARSIZE(acc, HDRSIZETQ);
			acc->size = 0;
		}
	}

	pfree(elemsp);
	QTNFree(qex);
	QTNFree(subs);
	QTNFree(acctree);

	PG_RETURN_TSQUERY(acc);
}

Datum
tsa_rewrite_finish(PG_FUNCTION_ARGS)
{
	TSQuery		acc = PG_GETARG_TSQUERY(0);
	TSQuery		rewrited;

	if (acc == NULL || PG_ARGISNULL(0) || acc->size == 0)
	{
		rewrited = (TSQuery) palloc(HDRSIZETQ);
		SET_VARSIZE(rewrited, HDRSIZETQ);
		rewrited->size = 0;
	}
	else
	{
		rewrited = (TSQuery) palloc(VARSIZE(acc));
		memcpy(rewrited, acc, VARSIZE(acc));
		pfree(acc);
	}

	PG_RETURN_POINTER(rewrited);
}


/*
 * Get Oid of current dictionary
 */
static Oid
GetCurrentDict(void)
{
	if (current_dictionary_oid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("no current dictionary"),
				 errhint("Execute SELECT set_curdict(...).")));

	return current_dictionary_oid;
}

/*
 * Get Oid of current parser
 *
 * Here, it seems reasonable to select the "default" parser if none has been
 * set.
 */
static Oid
GetCurrentParser(void)
{
	if (current_parser_oid == InvalidOid)
		current_parser_oid = get_ts_parser_oid(stringToQualifiedNameList("pg_catalog.default"), false);
	return current_parser_oid;
}
