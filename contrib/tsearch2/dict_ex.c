/* $PostgreSQL: pgsql/contrib/tsearch2/dict_ex.c,v 1.9 2006/11/20 14:03:30 teodor Exp $ */

/*
 * example of dictionary
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include "postgres.h"

#include "dict.h"
#include "common.h"
#include "ts_locale.h"

typedef struct
{
	StopList	stoplist;
}	DictExample;


PG_FUNCTION_INFO_V1(dex_init);
Datum		dex_init(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(dex_lexize);
Datum		dex_lexize(PG_FUNCTION_ARGS);

Datum
dex_init(PG_FUNCTION_ARGS)
{
	DictExample *d = (DictExample *) malloc(sizeof(DictExample));

	if (!d)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	memset(d, 0, sizeof(DictExample));

	d->stoplist.wordop = lowerstr;

	if (!PG_ARGISNULL(0) && PG_GETARG_POINTER(0) != NULL)
	{
		text	   *in = PG_GETARG_TEXT_P(0);

		readstoplist(in, &(d->stoplist));
		sortstoplist(&(d->stoplist));
		PG_FREE_IF_COPY(in, 0);
	}

	PG_RETURN_POINTER(d);
}

Datum
dex_lexize(PG_FUNCTION_ARGS)
{
	DictExample *d = (DictExample *) PG_GETARG_POINTER(0);
	char	   *in = (char *) PG_GETARG_POINTER(1);
	char	   *utxt = pnstrdup(in, PG_GETARG_INT32(2));
	TSLexeme   *res = palloc(sizeof(TSLexeme) * 2);
	char	   *txt = lowerstr(utxt);

	pfree(utxt);
	memset(res, 0, sizeof(TSLexeme) * 2);

	if (*txt == '\0' || searchstoplist(&(d->stoplist), txt))
	{
		pfree(txt);
	}
	else
		res[0].lexeme = txt;

	PG_RETURN_POINTER(res);
}
