/*-------------------------------------------------------------------------
 *
 * dict_simple.c
 *		Simple dictionary: just lowercase and check for stopword
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/tsearch/dict_simple.c,v 1.1 2007/08/21 01:11:18 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "tsearch/ts_locale.h"
#include "tsearch/ts_public.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"


typedef struct
{
	StopList	stoplist;
} DictExample;


Datum
dsimple_init(PG_FUNCTION_ARGS)
{
	DictExample *d = (DictExample *) palloc0(sizeof(DictExample));

	d->stoplist.wordop = recode_and_lowerstr;

	if (!PG_ARGISNULL(0) && PG_GETARG_POINTER(0) != NULL)
	{
		text   *in = PG_GETARG_TEXT_P(0);
		char   *filename = TextPGetCString(in);

		readstoplist(filename, &d->stoplist);
		sortstoplist(&d->stoplist);
		pfree(filename);
	}

	PG_RETURN_POINTER(d);
}

Datum
dsimple_lexize(PG_FUNCTION_ARGS)
{
	DictExample *d = (DictExample *) PG_GETARG_POINTER(0);
	char	   *in = (char *) PG_GETARG_POINTER(1);
	int32	   len = PG_GETARG_INT32(2);
	char	   *txt = lowerstr_with_len(in, len);
	TSLexeme   *res = palloc0(sizeof(TSLexeme) * 2);

	if (*txt == '\0' || searchstoplist(&(d->stoplist), txt))
	{
		pfree(txt);
	}
	else
		res[0].lexeme = txt;

	PG_RETURN_POINTER(res);
}
