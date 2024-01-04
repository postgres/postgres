/*-------------------------------------------------------------------------
 *
 * dict_int.c
 *	  Text search dictionary for integers
 *
 * Copyright (c) 2007-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/dict_int/dict_int.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/defrem.h"
#include "tsearch/ts_public.h"

PG_MODULE_MAGIC;

typedef struct
{
	int			maxlen;
	bool		rejectlong;
	bool		absval;
} DictInt;


PG_FUNCTION_INFO_V1(dintdict_init);
PG_FUNCTION_INFO_V1(dintdict_lexize);

Datum
dintdict_init(PG_FUNCTION_ARGS)
{
	List	   *dictoptions = (List *) PG_GETARG_POINTER(0);
	DictInt    *d;
	ListCell   *l;

	d = (DictInt *) palloc0(sizeof(DictInt));
	d->maxlen = 6;
	d->rejectlong = false;
	d->absval = false;

	foreach(l, dictoptions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);

		if (strcmp(defel->defname, "maxlen") == 0)
		{
			d->maxlen = atoi(defGetString(defel));

			if (d->maxlen < 1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("maxlen value has to be >= 1")));
		}
		else if (strcmp(defel->defname, "rejectlong") == 0)
		{
			d->rejectlong = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "absval") == 0)
		{
			d->absval = defGetBoolean(defel);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized intdict parameter: \"%s\"",
							defel->defname)));
		}
	}

	PG_RETURN_POINTER(d);
}

Datum
dintdict_lexize(PG_FUNCTION_ARGS)
{
	DictInt    *d = (DictInt *) PG_GETARG_POINTER(0);
	char	   *in = (char *) PG_GETARG_POINTER(1);
	int			len = PG_GETARG_INT32(2);
	char	   *txt;
	TSLexeme   *res = palloc0(sizeof(TSLexeme) * 2);

	res[1].lexeme = NULL;

	if (d->absval && (in[0] == '+' || in[0] == '-'))
	{
		len--;
		txt = pnstrdup(in + 1, len);
	}
	else
		txt = pnstrdup(in, len);

	if (len > d->maxlen)
	{
		if (d->rejectlong)
		{
			/* reject by returning void array */
			pfree(txt);
			res[0].lexeme = NULL;
		}
		else
		{
			/* trim integer */
			txt[d->maxlen] = '\0';
			res[0].lexeme = txt;
		}
	}
	else
	{
		res[0].lexeme = txt;
	}

	PG_RETURN_POINTER(res);
}
