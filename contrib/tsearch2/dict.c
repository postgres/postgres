/* $PostgreSQL: pgsql/contrib/tsearch2/dict.c,v 1.13 2006/10/04 00:29:46 momjian Exp $ */

/*
 * interface functions to dictionary
 * Teodor Sigaev <teodor@sigaev.ru>
 */
#include "postgres.h"

#include <ctype.h>

#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/memutils.h"

#include "dict.h"
#include "common.h"
#include "snmap.h"

/*********top interface**********/

void
init_dict(Oid id, DictInfo * dict)
{
	Oid			arg[1];
	bool		isnull;
	Datum		pars[1];
	int			stat;
	void	   *plan;
	char		buf[1024];
	char	   *nsp = get_namespace(TSNSP_FunctionOid);

	arg[0] = OIDOID;
	pars[0] = ObjectIdGetDatum(id);

	memset(dict, 0, sizeof(DictInfo));
	SPI_connect();
	sprintf(buf, "select dict_init, dict_initoption, dict_lexize from %s.pg_ts_dict where oid = $1", nsp);
	pfree(nsp);
	plan = SPI_prepare(buf, 1, arg);
	if (!plan)
		ts_error(ERROR, "SPI_prepare() failed");

	stat = SPI_execp(plan, pars, " ", 1);
	if (stat < 0)
		ts_error(ERROR, "SPI_execp return %d", stat);
	if (SPI_processed > 0)
	{
		Datum		opt;
		Oid			oid = InvalidOid;

		/* setup dictlexize method */
		oid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 3, &isnull));
		if (isnull || oid == InvalidOid)
			ts_error(ERROR, "Null dict_lexize for dictonary %d", id);
		fmgr_info_cxt(oid, &(dict->lexize_info), TopMemoryContext);

		/* setup and call dictinit method, optinally */
		oid = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
		if (!(isnull || oid == InvalidOid))
		{
			opt = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
			dict->dictionary = (void *) DatumGetPointer(OidFunctionCall1(oid, opt));
		}
		dict->dict_id = id;
	}
	else
		ts_error(ERROR, "No dictionary with id %d", id);
	SPI_freeplan(plan);
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
	if (((DictInfo *) a)->dict_id == ((DictInfo *) b)->dict_id)
		return 0;
	return (((DictInfo *) a)->dict_id < ((DictInfo *) b)->dict_id) ? -1 : 1;
}

static void
insertdict(Oid id)
{
	DictInfo	newdict;

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
	init_dict(id, &newdict);

	DList.list[DList.len] = newdict;
	DList.len++;

	qsort(DList.list, DList.len, sizeof(DictInfo), comparedict);
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

	/* insert new dictionary */
	insertdict(id);
	return finddict(id); /* qsort changed order!! */ ;
}

Oid
name2id_dict(text *name)
{
	Oid			arg[1];
	bool		isnull;
	Datum		pars[1];
	int			stat;
	Oid			id = findSNMap_t(&(DList.name2id_map), name);
	void	   *plan;
	char		buf[1024],
			   *nsp;

	arg[0] = TEXTOID;
	pars[0] = PointerGetDatum(name);

	if (id)
		return id;

	nsp = get_namespace(TSNSP_FunctionOid);
	SPI_connect();
	sprintf(buf, "select oid from %s.pg_ts_dict where dict_name = $1", nsp);
	pfree(nsp);
	plan = SPI_prepare(buf, 1, arg);
	if (!plan)
		ts_error(ERROR, "SPI_prepare() failed");

	stat = SPI_execp(plan, pars, " ", 1);
	if (stat < 0)
		ts_error(ERROR, "SPI_execp return %d", stat);
	if (SPI_processed > 0)
		id = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
	else
		ts_error(ERROR, "No dictionary with name '%s'", text2char(name));
	SPI_freeplan(plan);
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
	DictInfo   *dict;
	TSLexeme   *res,
			   *ptr;
	Datum	   *da;
	ArrayType  *a;
	DictSubState dstate = {false, false, NULL};

	SET_FUNCOID();
	dict = finddict(PG_GETARG_OID(0));

	ptr = res = (TSLexeme *) DatumGetPointer(
										  FunctionCall4(&(dict->lexize_info),
										   PointerGetDatum(dict->dictionary),
												PointerGetDatum(VARDATA(in)),
									   Int32GetDatum(VARSIZE(in) - VARHDRSZ),
													 PointerGetDatum(&dstate)
														)
		);

	if (dstate.getnext)
	{
		dstate.isend = true;
		ptr = res = (TSLexeme *) DatumGetPointer(
										  FunctionCall4(&(dict->lexize_info),
										   PointerGetDatum(dict->dictionary),
												PointerGetDatum(VARDATA(in)),
									   Int32GetDatum(VARSIZE(in) - VARHDRSZ),
													 PointerGetDatum(&dstate)
														)
			);
	}

	PG_FREE_IF_COPY(in, 1);
	if (!res)
	{
		if (PG_NARGS() > 2)
			PG_RETURN_POINTER(NULL);
		else
			PG_RETURN_NULL();
	}

	while (ptr->lexeme)
		ptr++;
	da = (Datum *) palloc(sizeof(Datum) * (ptr - res + 1));
	ptr = res;
	while (ptr->lexeme)
	{
		da[ptr - res] = PointerGetDatum(char2text(ptr->lexeme));
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
	while (ptr->lexeme)
	{
		pfree(DatumGetPointer(da[ptr - res]));
		pfree(ptr->lexeme);
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

	SET_FUNCOID();

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
	SET_FUNCOID();
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

	SET_FUNCOID();
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

	SET_FUNCOID();
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
