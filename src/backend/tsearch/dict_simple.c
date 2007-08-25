/*-------------------------------------------------------------------------
 *
 * dict_simple.c
 *		Simple dictionary: just lowercase and check for stopword
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/tsearch/dict_simple.c,v 1.3 2007/08/25 00:03:59 tgl Exp $
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
	StopList	stoplist;
} DictSimple;


Datum
dsimple_init(PG_FUNCTION_ARGS)
{
	List	   *dictoptions = (List *) PG_GETARG_POINTER(0);
	DictSimple *d = (DictSimple *) palloc0(sizeof(DictSimple));
	bool		stoploaded = false;
	ListCell   *l;

	foreach(l, dictoptions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);

		if (pg_strcasecmp("StopWords", defel->defname) == 0)
		{
			if (stoploaded)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple StopWords parameters")));
			readstoplist(defGetString(defel), &d->stoplist, lowerstr);
			stoploaded = true;
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized simple dictionary parameter: \"%s\"",
							defel->defname)));
		}
	}

	PG_RETURN_POINTER(d);
}

Datum
dsimple_lexize(PG_FUNCTION_ARGS)
{
	DictSimple *d = (DictSimple *) PG_GETARG_POINTER(0);
	char	   *in = (char *) PG_GETARG_POINTER(1);
	int32	   len = PG_GETARG_INT32(2);
	char	   *txt;
	TSLexeme   *res = palloc0(sizeof(TSLexeme) * 2);

	txt = lowerstr_with_len(in, len);

	if (*txt == '\0' || searchstoplist(&(d->stoplist), txt))
		pfree(txt);
	else
		res[0].lexeme = txt;

	PG_RETURN_POINTER(res);
}
