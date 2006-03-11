/* $PostgreSQL: pgsql/contrib/tsearch2/dict_ispell.c,v 1.10 2006/03/11 04:38:30 momjian Exp $ */

/*
 * ISpell interface
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include "postgres.h"

#include <ctype.h>

#include "dict.h"
#include "common.h"
#include "ispell/spell.h"
#include "ts_locale.h"

typedef struct
{
	StopList	stoplist;
	IspellDict	obj;
}	DictISpell;

PG_FUNCTION_INFO_V1(spell_init);
Datum		spell_init(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(spell_lexize);
Datum		spell_lexize(PG_FUNCTION_ARGS);

static void
freeDictISpell(DictISpell * d)
{
	NIFree(&(d->obj));
	freestoplist(&(d->stoplist));
	free(d);
}

Datum
spell_init(PG_FUNCTION_ARGS)
{
	DictISpell *d;
	Map		   *cfg,
			   *pcfg;
	text	   *in;
	bool		affloaded = false,
				dictloaded = false,
				stoploaded = false;

	if (PG_ARGISNULL(0) || PG_GETARG_POINTER(0) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("ISpell confguration error")));

	d = (DictISpell *) malloc(sizeof(DictISpell));
	if (!d)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	memset(d, 0, sizeof(DictISpell));
	d->stoplist.wordop = lowerstr;

	in = PG_GETARG_TEXT_P(0);
	parse_cfgdict(in, &cfg);
	PG_FREE_IF_COPY(in, 0);
	pcfg = cfg;
	while (pcfg->key)
	{
		if (pg_strcasecmp("DictFile", pcfg->key) == 0)
		{
			if (dictloaded)
			{
				freeDictISpell(d);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("dictionary already loaded")));
			}
			if (NIImportDictionary(&(d->obj), pcfg->value))
			{
				freeDictISpell(d);
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("could not load dictionary file \"%s\"",
								pcfg->value)));
			}
			dictloaded = true;
		}
		else if (pg_strcasecmp("AffFile", pcfg->key) == 0)
		{
			if (affloaded)
			{
				freeDictISpell(d);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("affixes already loaded")));
			}
			if (NIImportAffixes(&(d->obj), pcfg->value))
			{
				freeDictISpell(d);
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("could not load affix file \"%s\"",
								pcfg->value)));
			}
			affloaded = true;
		}
		else if (pg_strcasecmp("StopFile", pcfg->key) == 0)
		{
			text	   *tmp = char2text(pcfg->value);

			if (stoploaded)
			{
				freeDictISpell(d);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("stop words already loaded")));
			}
			readstoplist(tmp, &(d->stoplist));
			sortstoplist(&(d->stoplist));
			pfree(tmp);
			stoploaded = true;
		}
		else
		{
			freeDictISpell(d);
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized option: %s => %s",
							pcfg->key, pcfg->value)));
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
		freeDictISpell(d);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("no affixes")));
	}
	else
	{
		freeDictISpell(d);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("no dictionary")));
	}

	PG_RETURN_POINTER(d);
}

Datum
spell_lexize(PG_FUNCTION_ARGS)
{
	DictISpell *d = (DictISpell *) PG_GETARG_POINTER(0);
	char	   *in = (char *) PG_GETARG_POINTER(1);
	char	   *txt;
	TSLexeme   *res;
	TSLexeme   *ptr,
			   *cptr;

	if (!PG_GETARG_INT32(2))
		PG_RETURN_POINTER(NULL);

	txt = pnstrdup(in, PG_GETARG_INT32(2));
	res = NINormalizeWord(&(d->obj), txt);
	pfree(txt);

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
