/*-------------------------------------------------------------------------
 *
 * dict_synonym.c
 *		Synonym dictionary: replace word by its synonym
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/tsearch/dict_synonym.c,v 1.2 2007/08/22 04:13:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/defrem.h"
#include "storage/fd.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_public.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"


#define SYNBUFLEN	4096

typedef struct
{
	char	   *in;
	char	   *out;
} Syn;

typedef struct
{
	int			len;
	Syn		   *syn;
} DictSyn;

static char *
findwrd(char *in, char **end)
{
	char	   *start;

	*end = NULL;
	while (*in && t_isspace(in))
		in += pg_mblen(in);

	if (*in == '\0')
		return NULL;
	start = in;

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
	FILE	   *fin;
	char		buf[SYNBUFLEN];
	char	   *starti,
			   *starto,
			   *end = NULL;
	int			cur = 0;
	int			slen;

	foreach(l, dictoptions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);

		if (pg_strcasecmp("Synonyms", defel->defname) == 0)
			filename = defGetString(defel);
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

	if ((fin = AllocateFile(filename, "r")) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("could not open synonym file \"%s\": %m",
						filename)));

	d = (DictSyn *) palloc0(sizeof(DictSyn));

	while (fgets(buf, SYNBUFLEN, fin))
	{
		slen = strlen(buf);
		pg_verifymbstr(buf, slen, false);
		if (cur == d->len)
		{
			if (d->len == 0)
			{
				d->len = 16;
				d->syn = (Syn *) palloc(sizeof(Syn) * d->len);
			}
			else
			{
				d->len *= 2;
				d->syn = (Syn *) repalloc(d->syn, sizeof(Syn) * d->len);
			}
		}

		starti = findwrd(buf, &end);
		if (!starti)
			continue;
		*end = '\0';
		if (end >= buf + slen)
			continue;

		starto = findwrd(end + 1, &end);
		if (!starto)
			continue;
		*end = '\0';

		d->syn[cur].in = recode_and_lowerstr(starti);
		d->syn[cur].out = recode_and_lowerstr(starto);
		if (!(d->syn[cur].in && d->syn[cur].out))
		{
			FreeFile(fin);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}

		cur++;
	}

	FreeFile(fin);

	d->len = cur;
	if (cur > 1)
		qsort(d->syn, d->len, sizeof(Syn), compareSyn);

	PG_RETURN_POINTER(d);
}

Datum
dsynonym_lexize(PG_FUNCTION_ARGS)
{
	DictSyn    *d = (DictSyn *) PG_GETARG_POINTER(0);
	char	   *in = (char *) PG_GETARG_POINTER(1);
	int32	   len = PG_GETARG_INT32(2);
	Syn			key,
			   *found;
	TSLexeme   *res;

	if (len <= 0)
		PG_RETURN_POINTER(NULL);

	key.in = lowerstr_with_len(in, len);
	key.out = NULL;

	found = (Syn *) bsearch(&key, d->syn, d->len, sizeof(Syn), compareSyn);
	pfree(key.in);

	if (!found)
		PG_RETURN_POINTER(NULL);

	res = palloc(sizeof(TSLexeme) * 2);
	memset(res, 0, sizeof(TSLexeme) * 2);
	res[0].lexeme = pstrdup(found->out);

	PG_RETURN_POINTER(res);
}
