/*
 * example of dictionary
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "postgres.h"

#include "dict.h"
#include "common.h"

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
	char	   *txt = pnstrdup(in, PG_GETARG_INT32(2));
	char	  **res = palloc(sizeof(char *) * 2);

	if (*txt == '\0' || searchstoplist(&(d->stoplist), txt))
	{
		pfree(txt);
		res[0] = NULL;
	}
	else
		res[0] = txt;
	res[1] = NULL;

	PG_RETURN_POINTER(res);
}
