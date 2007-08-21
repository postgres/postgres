/*-------------------------------------------------------------------------
 *
 * dict_ispell.c
 *		Ispell dictionary interface
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/tsearch/dict_ispell.c,v 1.1 2007/08/21 01:11:18 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "tsearch/dicts/spell.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_public.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "utils/memutils.h"


typedef struct
{
	StopList	stoplist;
	IspellDict	obj;
} DictISpell;

Datum
dispell_init(PG_FUNCTION_ARGS)
{
	DictISpell *d;
	Map		   *cfg,
			   *pcfg;
	bool		affloaded = false,
				dictloaded = false,
				stoploaded = false;
	text	   *in;

	/* init functions must defend against NULLs for themselves */
	if (PG_ARGISNULL(0) || PG_GETARG_POINTER(0) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("NULL config not allowed for ISpell")));
	in = PG_GETARG_TEXT_P(0);

	parse_keyvalpairs(in, &cfg);
	PG_FREE_IF_COPY(in, 0);

	d = (DictISpell *) palloc0(sizeof(DictISpell));
	d->stoplist.wordop = recode_and_lowerstr;

	pcfg = cfg;
	while (pcfg->key)
	{
		if (pg_strcasecmp("DictFile", pcfg->key) == 0)
		{
			if (dictloaded)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple DictFile parameters")));
			NIImportDictionary(&(d->obj),
							   get_tsearch_config_filename(pcfg->value,
														   "dict"));
			dictloaded = true;
		}
		else if (pg_strcasecmp("AffFile", pcfg->key) == 0)
		{
			if (affloaded)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple AffFile parameters")));
			NIImportAffixes(&(d->obj),
							get_tsearch_config_filename(pcfg->value,
														"affix"));
			affloaded = true;
		}
		else if (pg_strcasecmp("StopWords", pcfg->key) == 0)
		{
			if (stoploaded)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple StopWords parameters")));
			readstoplist(pcfg->value, &(d->stoplist));
			sortstoplist(&(d->stoplist));
			stoploaded = true;
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized ISpell parameter: \"%s\"",
							pcfg->key)));
		}
		pfree(pcfg->key);
		pfree(pcfg->value);
		pcfg++;
	}
	pfree(cfg);

	if (affloaded && dictloaded)
	{
		NISortDictionary(&(d->obj));
		NISortAffixes(&(d->obj));
	}
	else if (!affloaded)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("missing AffFile parameter")));
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("missing DictFile parameter")));
	}

	MemoryContextDeleteChildren(CurrentMemoryContext);

	PG_RETURN_POINTER(d);
}

Datum
dispell_lexize(PG_FUNCTION_ARGS)
{
	DictISpell *d = (DictISpell *) PG_GETARG_POINTER(0);
	char	   *in = (char *) PG_GETARG_POINTER(1);
	int32	   len = PG_GETARG_INT32(2);
	char	   *txt;
	TSLexeme   *res;
	TSLexeme   *ptr,
			   *cptr;

	if (len <= 0)
		PG_RETURN_POINTER(NULL);

	txt = lowerstr_with_len(in, len);
	res = NINormalizeWord(&(d->obj), txt);

	if (res == NULL)
		PG_RETURN_POINTER(NULL);

	ptr = cptr = res;
	while (ptr->lexeme)
	{
		if (searchstoplist(&(d->stoplist), ptr->lexeme))
		{
			pfree(ptr->lexeme);
			ptr->lexeme = NULL;
			ptr++;
		}
		else
		{
			memcpy(cptr, ptr, sizeof(TSLexeme));
			cptr++;
			ptr++;
		}
	}
	cptr->lexeme = NULL;

	PG_RETURN_POINTER(res);
}
