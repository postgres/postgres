/*
 * interface functions to dictionary
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "postgres.h"
#include "fmgr.h"
#include "utils/array.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"

#include "dict.h"
#include "common.h"
#include "snmap.h"

/*********top interface**********/

static void *plan_getdict = NULL;

void
init_dict(Oid id, DictInfo * dict)
{
	Oid			arg[1] = {OIDOID};
	bool		isnull;
	Datum		pars[1] = {ObjectIdGetDatum(id)};
	int			stat;

	memset(dict, 0, sizeof(DictInfo));
	SPI_connect();
	if (!plan_getdict)
	{
		plan_getdict = SPI_saveplan(SPI_prepare("select dict_init, dict_initoption, dict_lexize from pg_ts_dict where oid = $1", 1, arg));
		if (!plan_getdict)
			ts_error(ERROR, "SPI_prepare() failed");
	}

	stat = SPI_execp(plan_getdict, pars, " ", 1);
	if (stat < 0)
		ts_error(ERROR, "SPI_execp return %d", stat);
	if (SPI_processed > 0)
	{
		Datum		opt;
		Oid			oid = InvalidOid;

		oid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
		if (!(isnull || oid == InvalidOid))
		{
			opt = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
			dict->dictionary = (void *) DatumGetPointer(OidFunctionCall1(oid, opt));
		}
		oid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 3, &isnull));
		if (isnull || oid == InvalidOid)
			ts_error(ERROR, "Null dict_lexize for dictonary %d", id);
		fmgr_info_cxt(oid, &(dict->lexize_info), TopMemoryContext);
		dict->dict_id = id;
	}
	else
		ts_error(ERROR, "No dictionary with id %d", id);
	SPI_finish();
}

typedef struct
{
	DictInfo   *last_dict;
	int			len;
	int			reallen;
	DictInfo   *list;
	SNMap		name2id_map;
}	DictList;

static DictList DList = {NULL, 0, 0, NULL, {0, 0, NULL}};

void
reset_dict(void)
{
	freeSNMap(&(DList.name2id_map));
	/* XXX need to free DList.list[*].dictionary */
	if (DList.list)
		free(DList.list);
	memset(&DList, 0, sizeof(DictList));
}


static int
comparedict(const void *a, const void *b)
{
	if ( ((DictInfo *) a)->dict_id == ((DictInfo *) b)->dict_id )
		return 0;
	return ( ((DictInfo *) a)->dict_id < ((DictInfo *) b)->dict_id ) ? -1 : 1;
}

DictInfo *
finddict(Oid id)
{
	/* last used dict */
	if (DList.last_dict && DList.last_dict->dict_id == id)
		return DList.last_dict;


	/* already used dict */
	if (DList.len != 0)
	{
		DictInfo	key;

		key.dict_id = id;
		DList.last_dict = bsearch(&key, DList.list, DList.len, sizeof(DictInfo), comparedict);
		if (DList.last_dict != NULL)
			return DList.last_dict;
	}

	/* last chance */
	if (DList.len == DList.reallen)
	{
		DictInfo   *tmp;
		int			reallen = (DList.reallen) ? 2 * DList.reallen : 16;

		tmp = (DictInfo *) realloc(DList.list, sizeof(DictInfo) * reallen);
		if (!tmp)
			ts_error(ERROR, "No memory");
		DList.reallen = reallen;
		DList.list = tmp;
	}
	DList.last_dict = &(DList.list[DList.len]);
	init_dict(id, DList.last_dict);

	DList.len++;
	qsort(DList.list, DList.len, sizeof(DictInfo), comparedict);
	return finddict(id); /* qsort changed order!! */ ;
}

static void *plan_name2id = NULL;

Oid
name2id_dict(text *name)
{
	Oid			arg[1] = {TEXTOID};
	bool		isnull;
	Datum		pars[1] = {PointerGetDatum(name)};
	int			stat;
	Oid			id = findSNMap_t(&(DList.name2id_map), name);

	if (id)
		return id;

	SPI_connect();
	if (!plan_name2id)
	{
		plan_name2id = SPI_saveplan(SPI_prepare("select oid from pg_ts_dict where dict_name = $1", 1, arg));
		if (!plan_name2id)
			ts_error(ERROR, "SPI_prepare() failed");
	}

	stat = SPI_execp(plan_name2id, pars, " ", 1);
	if (stat < 0)
		ts_error(ERROR, "SPI_execp return %d", stat);
	if (SPI_processed > 0)
		id = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
	else
		ts_error(ERROR, "No dictionary with name '%s'", text2char(name));
	SPI_finish();
	addSNMap_t(&(DList.name2id_map), name, id);
	return id;
}


/******sql-level interface******/
PG_FUNCTION_INFO_V1(lexize);
Datum		lexize(PG_FUNCTION_ARGS);

Datum
lexize(PG_FUNCTION_ARGS)
{
	text	   *in = PG_GETARG_TEXT_P(1);
	DictInfo   *dict = finddict(PG_GETARG_OID(0));
	char	  **res,
			  **ptr;
	Datum	   *da;
	ArrayType  *a;


	ptr = res = (char **) DatumGetPointer(
									  FunctionCall3(&(dict->lexize_info),
									   PointerGetDatum(dict->dictionary),
											PointerGetDatum(VARDATA(in)),
									Int32GetDatum(VARSIZE(in) - VARHDRSZ)
													)
		);
	PG_FREE_IF_COPY(in, 1);
	if (!res)
	{
		if (PG_NARGS() > 2)
			PG_RETURN_POINTER(NULL);
		else
			PG_RETURN_NULL();
	}

	while (*ptr)
		ptr++;
	da = (Datum *) palloc(sizeof(Datum) * (ptr - res + 1));
	ptr = res;
	while (*ptr)
	{
		da[ptr - res] = PointerGetDatum(char2text(*ptr));
		ptr++;
	}

	a = construct_array(
						da,
						ptr - res,
						TEXTOID,
						-1,
						false,
						'i'
		);

	ptr = res;
	while (*ptr)
	{
		pfree(DatumGetPointer(da[ptr - res]));
		pfree(*ptr);
		ptr++;
	}
	pfree(res);
	pfree(da);

	PG_RETURN_POINTER(a);
}

PG_FUNCTION_INFO_V1(lexize_byname);
Datum		lexize_byname(PG_FUNCTION_ARGS);
Datum
lexize_byname(PG_FUNCTION_ARGS)
{
	text	   *dictname = PG_GETARG_TEXT_P(0);
	Datum		res;

	strdup("simple");
	res = DirectFunctionCall3(
							  lexize,
							  ObjectIdGetDatum(name2id_dict(dictname)),
							  PG_GETARG_DATUM(1),
							  (Datum) 0
		);
	PG_FREE_IF_COPY(dictname, 0);
	if (res)
		PG_RETURN_DATUM(res);
	else
		PG_RETURN_NULL();
}

static Oid	currect_dictionary_id = 0;

PG_FUNCTION_INFO_V1(set_curdict);
Datum		set_curdict(PG_FUNCTION_ARGS);
Datum
set_curdict(PG_FUNCTION_ARGS)
{
	finddict(PG_GETARG_OID(0));
	currect_dictionary_id = PG_GETARG_OID(0);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(set_curdict_byname);
Datum		set_curdict_byname(PG_FUNCTION_ARGS);
Datum
set_curdict_byname(PG_FUNCTION_ARGS)
{
	text	   *dictname = PG_GETARG_TEXT_P(0);

	DirectFunctionCall1(
						set_curdict,
						ObjectIdGetDatum(name2id_dict(dictname))
		);
	PG_FREE_IF_COPY(dictname, 0);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(lexize_bycurrent);
Datum		lexize_bycurrent(PG_FUNCTION_ARGS);
Datum
lexize_bycurrent(PG_FUNCTION_ARGS)
{
	Datum		res;

	if (currect_dictionary_id == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("no currect dictionary"),
				 errhint("Execute select set_curdict().")));

	res = DirectFunctionCall3(
							  lexize,
							  ObjectIdGetDatum(currect_dictionary_id),
							  PG_GETARG_DATUM(0),
							  (Datum) 0
		);
	if (res)
		PG_RETURN_DATUM(res);
	else
		PG_RETURN_NULL();
}
