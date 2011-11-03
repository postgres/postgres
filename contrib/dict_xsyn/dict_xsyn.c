/*-------------------------------------------------------------------------
 *
 * dict_xsyn.c
 *	  Extended synonym dictionary
 *
 * Copyright (c) 2007-2009, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/contrib/dict_xsyn/dict_xsyn.c,v 1.6 2009/01/01 17:23:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "commands/defrem.h"
#include "fmgr.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_utils.h"

PG_MODULE_MAGIC;

typedef struct
{
	char	   *key;			/* Word */
	char	   *value;			/* Unparsed list of synonyms, including the
								 * word itself */
} Syn;

typedef struct
{
	int			len;
	Syn		   *syn;

	bool		keeporig;
} DictSyn;


PG_FUNCTION_INFO_V1(dxsyn_init);
Datum		dxsyn_init(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(dxsyn_lexize);
Datum		dxsyn_lexize(PG_FUNCTION_ARGS);

static char *
find_word(char *in, char **end)
{
	char	   *start;

	*end = NULL;
	while (*in && t_isspace(in))
		in += pg_mblen(in);

	if (!*in || *in == '#')
		return NULL;
	start = in;

	while (*in && !t_isspace(in))
		in += pg_mblen(in);

	*end = in;

	return start;
}

static int
compare_syn(const void *a, const void *b)
{
	return strcmp(((Syn *) a)->key, ((Syn *) b)->key);
}

static void
read_dictionary(DictSyn *d, char *filename)
{
	char	   *real_filename = get_tsearch_config_filename(filename, "rules");
	tsearch_readline_state trst;
	char	   *line;
	int			cur = 0;

	if (!tsearch_readline_begin(&trst, real_filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open synonym file \"%s\": %m",
						real_filename)));

	while ((line = tsearch_readline(&trst)) != NULL)
	{
		char	   *value;
		char	   *key;
		char	   *end = NULL;

		if (*line == '\0')
			continue;

		value = lowerstr(line);
		pfree(line);

		key = find_word(value, &end);
		if (!key)
		{
			pfree(value);
			continue;
		}

		if (cur == d->len)
		{
			d->len = (d->len > 0) ? 2 * d->len : 16;
			if (d->syn)
				d->syn = (Syn *) repalloc(d->syn, sizeof(Syn) * d->len);
			else
				d->syn = (Syn *) palloc(sizeof(Syn) * d->len);
		}

		d->syn[cur].key = pnstrdup(key, end - key);
		d->syn[cur].value = value;

		cur++;
	}

	tsearch_readline_end(&trst);

	d->len = cur;
	if (cur > 1)
		qsort(d->syn, d->len, sizeof(Syn), compare_syn);

	pfree(real_filename);
}

Datum
dxsyn_init(PG_FUNCTION_ARGS)
{
	List	   *dictoptions = (List *) PG_GETARG_POINTER(0);
	DictSyn    *d;
	ListCell   *l;

	d = (DictSyn *) palloc0(sizeof(DictSyn));
	d->len = 0;
	d->syn = NULL;
	d->keeporig = true;

	foreach(l, dictoptions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);

		if (pg_strcasecmp(defel->defname, "KEEPORIG") == 0)
		{
			d->keeporig = defGetBoolean(defel);
		}
		else if (pg_strcasecmp(defel->defname, "RULES") == 0)
		{
			read_dictionary(d, defGetString(defel));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized xsyn parameter: \"%s\"",
							defel->defname)));
		}
	}

	PG_RETURN_POINTER(d);
}

Datum
dxsyn_lexize(PG_FUNCTION_ARGS)
{
	DictSyn    *d = (DictSyn *) PG_GETARG_POINTER(0);
	char	   *in = (char *) PG_GETARG_POINTER(1);
	int			length = PG_GETARG_INT32(2);
	Syn			word;
	Syn		   *found;
	TSLexeme   *res = NULL;

	if (!length || d->len == 0)
		PG_RETURN_POINTER(NULL);

	/* Create search pattern */
	{
		char	   *temp = pnstrdup(in, length);

		word.key = lowerstr(temp);
		pfree(temp);
		word.value = NULL;
	}

	/* Look for matching syn */
	found = (Syn *) bsearch(&word, d->syn, d->len, sizeof(Syn), compare_syn);
	pfree(word.key);

	if (!found)
		PG_RETURN_POINTER(NULL);

	/* Parse string of synonyms and return array of words */
	{
		char	   *value = pstrdup(found->value);
		int			value_length = strlen(value);
		char	   *pos = value;
		int			nsyns = 0;
		bool		is_first = true;

		res = palloc(sizeof(TSLexeme));

		while (pos < value + value_length)
		{
			char	   *end;
			char	   *syn = find_word(pos, &end);

			if (!syn)
				break;
			*end = '\0';

			res = repalloc(res, sizeof(TSLexeme) * (nsyns + 2));

			/* first word is added to result only if KEEPORIG flag is set */
			if (d->keeporig || !is_first)
			{
				res[nsyns].lexeme = pstrdup(syn);
				res[nsyns].nvariant = 0;
				res[nsyns].flags = 0;

				nsyns++;
			}

			is_first = false;

			pos = end + 1;
		}

		res[nsyns].lexeme = NULL;

		pfree(value);
	}

	PG_RETURN_POINTER(res);
}
