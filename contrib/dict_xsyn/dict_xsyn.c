/*-------------------------------------------------------------------------
 *
 * dict_xsyn.c
 *	  Extended synonym dictionary
 *
 * Copyright (c) 2007-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/dict_xsyn/dict_xsyn.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "commands/defrem.h"
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

	bool		matchorig;
	bool		keeporig;
	bool		matchsynonyms;
	bool		keepsynonyms;
} DictSyn;


PG_FUNCTION_INFO_V1(dxsyn_init);
PG_FUNCTION_INFO_V1(dxsyn_lexize);

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
	return strcmp(((const Syn *) a)->key, ((const Syn *) b)->key);
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
		char	   *pos;
		char	   *end;

		if (*line == '\0')
			continue;

		value = lowerstr(line);
		pfree(line);

		pos = value;
		while ((key = find_word(pos, &end)) != NULL)
		{
			/* Enlarge syn structure if full */
			if (cur == d->len)
			{
				d->len = (d->len > 0) ? 2 * d->len : 16;
				if (d->syn)
					d->syn = (Syn *) repalloc(d->syn, sizeof(Syn) * d->len);
				else
					d->syn = (Syn *) palloc(sizeof(Syn) * d->len);
			}

			/* Save first word only if we will match it */
			if (pos != value || d->matchorig)
			{
				d->syn[cur].key = pnstrdup(key, end - key);
				d->syn[cur].value = pstrdup(value);

				cur++;
			}

			pos = end;

			/* Don't bother scanning synonyms if we will not match them */
			if (!d->matchsynonyms)
				break;
		}

		pfree(value);
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
	char	   *filename = NULL;

	d = (DictSyn *) palloc0(sizeof(DictSyn));
	d->len = 0;
	d->syn = NULL;
	d->matchorig = true;
	d->keeporig = true;
	d->matchsynonyms = false;
	d->keepsynonyms = true;

	foreach(l, dictoptions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);

		if (pg_strcasecmp(defel->defname, "MATCHORIG") == 0)
		{
			d->matchorig = defGetBoolean(defel);
		}
		else if (pg_strcasecmp(defel->defname, "KEEPORIG") == 0)
		{
			d->keeporig = defGetBoolean(defel);
		}
		else if (pg_strcasecmp(defel->defname, "MATCHSYNONYMS") == 0)
		{
			d->matchsynonyms = defGetBoolean(defel);
		}
		else if (pg_strcasecmp(defel->defname, "KEEPSYNONYMS") == 0)
		{
			d->keepsynonyms = defGetBoolean(defel);
		}
		else if (pg_strcasecmp(defel->defname, "RULES") == 0)
		{
			/* we can't read the rules before parsing all options! */
			filename = defGetString(defel);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized xsyn parameter: \"%s\"",
							defel->defname)));
		}
	}

	if (filename)
		read_dictionary(d, filename);

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
		char	   *value = found->value;
		char	   *syn;
		char	   *pos;
		char	   *end;
		int			nsyns = 0;

		res = palloc(sizeof(TSLexeme));

		pos = value;
		while ((syn = find_word(pos, &end)) != NULL)
		{
			res = repalloc(res, sizeof(TSLexeme) * (nsyns + 2));

			/* The first word is output only if keeporig=true */
			if (pos != value || d->keeporig)
			{
				res[nsyns].lexeme = pnstrdup(syn, end - syn);
				res[nsyns].nvariant = 0;
				res[nsyns].flags = 0;
				nsyns++;
			}

			pos = end;

			/* Stop if we are not to output the synonyms */
			if (!d->keepsynonyms)
				break;
		}
		res[nsyns].lexeme = NULL;
	}

	PG_RETURN_POINTER(res);
}
