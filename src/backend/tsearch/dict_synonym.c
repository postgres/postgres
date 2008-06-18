/*-------------------------------------------------------------------------
 *
 * dict_synonym.c
 *		Synonym dictionary: replace word by its synonym
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/tsearch/dict_synonym.c,v 1.9 2008/06/18 20:55:42 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/defrem.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_public.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"

typedef struct
{
	char	   *in;
	char	   *out;
} Syn;

typedef struct
{
	int			len;			/* length of syn array */
	Syn		   *syn;
	bool		case_sensitive;
} DictSyn;

/*
 * Finds the next whitespace-delimited word within the 'in' string.
 * Returns a pointer to the first character of the word, and a pointer
 * to the next byte after the last character in the word (in *end).
 */
static char *
findwrd(char *in, char **end)
{
	char	   *start;

	/* Skip leading spaces */
	while (*in && t_isspace(in))
		in += pg_mblen(in);

	/* Return NULL on empty lines */
	if (*in == '\0')
	{
		*end = NULL;
		return NULL;
	}

	start = in;

	/* Find end of word */
	while (*in && !t_isspace(in))
		in += pg_mblen(in);

	*end = in;
	return start;
}

static int
compareSyn(const void *a, const void *b)
{
	return strcmp(((Syn *) a)->in, ((Syn *) b)->in);
}


Datum
dsynonym_init(PG_FUNCTION_ARGS)
{
	List	   *dictoptions = (List *) PG_GETARG_POINTER(0);
	DictSyn    *d;
	ListCell   *l;
	char	   *filename = NULL;
	bool		case_sensitive = false;
	tsearch_readline_state trst;
	char	   *starti,
			   *starto,
			   *end = NULL;
	int			cur = 0;
	char	   *line = NULL;

	foreach(l, dictoptions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);

		if (pg_strcasecmp("Synonyms", defel->defname) == 0)
			filename = defGetString(defel);
		else if (pg_strcasecmp("CaseSensitive", defel->defname) == 0)
			case_sensitive = defGetBoolean(defel);
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized synonym parameter: \"%s\"",
							defel->defname)));
	}

	if (!filename)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("missing Synonyms parameter")));

	filename = get_tsearch_config_filename(filename, "syn");

	if (!tsearch_readline_begin(&trst, filename))
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open synonym file \"%s\": %m",
						filename)));

	d = (DictSyn *) palloc0(sizeof(DictSyn));

	while ((line = tsearch_readline(&trst)) != NULL)
	{
		starti = findwrd(line, &end);
		if (!starti)
		{
			/* Empty line */
			goto skipline;
		}
		if (*end == '\0')
		{
			/* A line with only one word. Ignore silently. */
			goto skipline;
		}
		*end = '\0';

		starto = findwrd(end + 1, &end);
		if (!starto)
		{
			/* A line with only one word (+whitespace). Ignore silently. */
			goto skipline;
		}
		*end = '\0';

		/*
		 * starti now points to the first word, and starto to the second word
		 * on the line, with a \0 terminator at the end of both words.
		 */

		if (cur >= d->len)
		{
			if (d->len == 0)
			{
				d->len = 64;
				d->syn = (Syn *) palloc(sizeof(Syn) * d->len);
			}
			else
			{
				d->len *= 2;
				d->syn = (Syn *) repalloc(d->syn, sizeof(Syn) * d->len);
			}
		}

		if (case_sensitive)
		{
			d->syn[cur].in = pstrdup(starti);
			d->syn[cur].out = pstrdup(starto);
		}
		else
		{
			d->syn[cur].in = lowerstr(starti);
			d->syn[cur].out = lowerstr(starto);
		}

		cur++;

skipline:
		pfree(line);
	}

	tsearch_readline_end(&trst);

	d->len = cur;
	qsort(d->syn, d->len, sizeof(Syn), compareSyn);

	d->case_sensitive = case_sensitive;

	PG_RETURN_POINTER(d);
}

Datum
dsynonym_lexize(PG_FUNCTION_ARGS)
{
	DictSyn    *d = (DictSyn *) PG_GETARG_POINTER(0);
	char	   *in = (char *) PG_GETARG_POINTER(1);
	int32		len = PG_GETARG_INT32(2);
	Syn			key,
			   *found;
	TSLexeme   *res;

	/* note: d->len test protects against Solaris bsearch-of-no-items bug */
	if (len <= 0 || d->len <= 0)
		PG_RETURN_POINTER(NULL);

	if (d->case_sensitive)
		key.in = pnstrdup(in, len);
	else
		key.in = lowerstr_with_len(in, len);

	key.out = NULL;

	found = (Syn *) bsearch(&key, d->syn, d->len, sizeof(Syn), compareSyn);
	pfree(key.in);

	if (!found)
		PG_RETURN_POINTER(NULL);

	res = palloc0(sizeof(TSLexeme) * 2);
	res[0].lexeme = pstrdup(found->out);

	PG_RETURN_POINTER(res);
}
