/* $PostgreSQL: pgsql/contrib/tsearch2/dict_syn.c,v 1.10.2.1 2006/12/04 09:27:44 teodor Exp $ */

/*
 * ISpell interface
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include "postgres.h"

#include <ctype.h>

#include "dict.h"
#include "common.h"
#include "ts_locale.h"

#define SYNBUFLEN	4096
typedef struct
{
	char	   *in;
	char	   *out;
}	Syn;

typedef struct
{
	int			len;
	Syn		   *syn;
}	DictSyn;

PG_FUNCTION_INFO_V1(syn_init);
Datum		syn_init(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(syn_lexize);
Datum		syn_lexize(PG_FUNCTION_ARGS);

static char *
findwrd(char *in, char **end)
{
	char	   *start;

	*end = NULL;
	while (*in && isspace((unsigned char) *in))
		in++;

	if (!in)
		return NULL;
	start = in;

	while (*in && !isspace((unsigned char) *in))
		in++;

	*end = in;
	return start;
}

static int
compareSyn(const void *a, const void *b)
{
	return strcmp(((Syn *) a)->in, ((Syn *) b)->in);
}


Datum
syn_init(PG_FUNCTION_ARGS)
{
	text	   *in;
	DictSyn    *d;
	int			cur = 0;
	FILE	   *fin;
	char	   *filename;
	char		buf[SYNBUFLEN];
	char	   *starti,
			   *starto,
			   *end = NULL;
	int			slen;

	if (PG_ARGISNULL(0) || PG_GETARG_POINTER(0) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("NULL config")));

	in = PG_GETARG_TEXT_P(0);
	if (VARSIZE(in) - VARHDRSZ == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("VOID config")));

	filename = text2char(in);
	PG_FREE_IF_COPY(in, 0);
	if ((fin = fopen(filename, "r")) == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						filename)));

	d = (DictSyn *) malloc(sizeof(DictSyn));
	if (!d)
	{
		fclose(fin);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	}
	memset(d, 0, sizeof(DictSyn));

	while (fgets(buf, SYNBUFLEN, fin))
	{
		slen = strlen(buf) - 1;
		buf[slen] = '\0';
		if (*buf == '\0')
			continue;
		if (cur == d->len)
		{
			d->len = (d->len) ? 2 * d->len : 16;
			d->syn = (Syn *) realloc(d->syn, sizeof(Syn) * d->len);
			if (!d->syn)
			{
				fclose(fin);
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory")));
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

		d->syn[cur].in = strdup(lowerstr(starti));
		d->syn[cur].out = strdup(lowerstr(starto));
		if (!(d->syn[cur].in && d->syn[cur].out))
		{
			fclose(fin);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		}

		cur++;
	}

	fclose(fin);

	d->len = cur;
	if (cur > 1)
		qsort(d->syn, d->len, sizeof(Syn), compareSyn);

	pfree(filename);
	PG_RETURN_POINTER(d);
}

Datum
syn_lexize(PG_FUNCTION_ARGS)
{
	DictSyn    *d = (DictSyn *) PG_GETARG_POINTER(0);
	char	   *in = (char *) PG_GETARG_POINTER(1);
	Syn			key,
			   *found;
	TSLexeme   *res = NULL;
	char	   *wrd;

	if (!PG_GETARG_INT32(2))
		PG_RETURN_POINTER(NULL);

	key.out = NULL;
	wrd = pnstrdup(in, PG_GETARG_INT32(2));
	key.in = lowerstr(wrd);
	pfree(wrd);

	found = (Syn *) bsearch(&key, d->syn, d->len, sizeof(Syn), compareSyn);
	pfree(key.in);

	if (!found)
		PG_RETURN_POINTER(NULL);

	res = palloc(sizeof(TSLexeme) * 2);
	memset(res, 0, sizeof(TSLexeme) * 2);
	res[0].lexeme = pstrdup(found->out);

	PG_RETURN_POINTER(res);
}
