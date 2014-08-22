/*-------------------------------------------------------------------------
 *
 * jsonfuncs.c
 *		Functions to process JSON data types.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/json.h"
#include "utils/jsonapi.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"

/* semantic action functions for json_object_keys */
static void okeys_object_field_start(void *state, char *fname, bool isnull);
static void okeys_array_start(void *state);
static void okeys_scalar(void *state, char *token, JsonTokenType tokentype);

/* semantic action functions for json_get* functions */
static void get_object_start(void *state);
static void get_object_end(void *state);
static void get_object_field_start(void *state, char *fname, bool isnull);
static void get_object_field_end(void *state, char *fname, bool isnull);
static void get_array_start(void *state);
static void get_array_end(void *state);
static void get_array_element_start(void *state, bool isnull);
static void get_array_element_end(void *state, bool isnull);
static void get_scalar(void *state, char *token, JsonTokenType tokentype);

/* common worker function for json getter functions */
static Datum get_path_all(FunctionCallInfo fcinfo, bool as_text);
static text *get_worker(text *json, char **tpath, int *ipath, int npath,
		   bool normalize_results);
static Datum get_jsonb_path_all(FunctionCallInfo fcinfo, bool as_text);

/* semantic action functions for json_array_length */
static void alen_object_start(void *state);
static void alen_scalar(void *state, char *token, JsonTokenType tokentype);
static void alen_array_element_start(void *state, bool isnull);

/* common workers for json{b}_each* functions */
static Datum each_worker(FunctionCallInfo fcinfo, bool as_text);
static Datum each_worker_jsonb(FunctionCallInfo fcinfo, const char *funcname,
				  bool as_text);

/* semantic action functions for json_each */
static void each_object_field_start(void *state, char *fname, bool isnull);
static void each_object_field_end(void *state, char *fname, bool isnull);
static void each_array_start(void *state);
static void each_scalar(void *state, char *token, JsonTokenType tokentype);

/* common workers for json{b}_array_elements_* functions */
static Datum elements_worker(FunctionCallInfo fcinfo, const char *funcname,
				bool as_text);
static Datum elements_worker_jsonb(FunctionCallInfo fcinfo, const char *funcname,
					  bool as_text);

/* semantic action functions for json_array_elements */
static void elements_object_start(void *state);
static void elements_array_element_start(void *state, bool isnull);
static void elements_array_element_end(void *state, bool isnull);
static void elements_scalar(void *state, char *token, JsonTokenType tokentype);

/* turn a json object into a hash table */
static HTAB *get_json_object_as_hash(text *json, const char *funcname);

/* common worker for populate_record and to_record */
static Datum populate_record_worker(FunctionCallInfo fcinfo, const char *funcname,
					   bool have_record_arg);

/* semantic action functions for get_json_object_as_hash */
static void hash_object_field_start(void *state, char *fname, bool isnull);
static void hash_object_field_end(void *state, char *fname, bool isnull);
static void hash_array_start(void *state);
static void hash_scalar(void *state, char *token, JsonTokenType tokentype);

/* semantic action functions for populate_recordset */
static void populate_recordset_object_field_start(void *state, char *fname, bool isnull);
static void populate_recordset_object_field_end(void *state, char *fname, bool isnull);
static void populate_recordset_scalar(void *state, char *token, JsonTokenType tokentype);
static void populate_recordset_object_start(void *state);
static void populate_recordset_object_end(void *state);
static void populate_recordset_array_start(void *state);
static void populate_recordset_array_element_start(void *state, bool isnull);

/* worker function for populate_recordset and to_recordset */
static Datum populate_recordset_worker(FunctionCallInfo fcinfo, const char *funcname,
						  bool have_record_arg);

/* Worker that takes care of common setup for us */
static JsonbValue *findJsonbValueFromContainerLen(JsonbContainer *container,
							   uint32 flags,
							   char *key,
							   uint32 keylen);

/* state for json_object_keys */
typedef struct OkeysState
{
	JsonLexContext *lex;
	char	  **result;
	int			result_size;
	int			result_count;
	int			sent_count;
} OkeysState;

/* state for json_get* functions */
typedef struct GetState
{
	JsonLexContext *lex;
	text	   *tresult;
	char	   *result_start;
	bool		normalize_results;
	bool		next_scalar;
	int			npath;			/* length of each path-related array */
	char	  **path_names;		/* field name(s) being sought */
	int		   *path_indexes;	/* array index(es) being sought */
	bool	   *pathok;			/* is path matched to current depth? */
	int		   *array_cur_index;	/* current element index at each path level */
} GetState;

/* state for json_array_length */
typedef struct AlenState
{
	JsonLexContext *lex;
	int			count;
} AlenState;

/* state for json_each */
typedef struct EachState
{
	JsonLexContext *lex;
	Tuplestorestate *tuple_store;
	TupleDesc	ret_tdesc;
	MemoryContext tmp_cxt;
	char	   *result_start;
	bool		normalize_results;
	bool		next_scalar;
	char	   *normalized_scalar;
} EachState;

/* state for json_array_elements */
typedef struct ElementsState
{
	JsonLexContext *lex;
	const char *function_name;
	Tuplestorestate *tuple_store;
	TupleDesc	ret_tdesc;
	MemoryContext tmp_cxt;
	char	   *result_start;
	bool		normalize_results;
	bool		next_scalar;
	char	   *normalized_scalar;
} ElementsState;

/* state for get_json_object_as_hash */
typedef struct JhashState
{
	JsonLexContext *lex;
	const char *function_name;
	HTAB	   *hash;
	char	   *saved_scalar;
	char	   *save_json_start;
} JHashState;

/* hashtable element */
typedef struct JsonHashEntry
{
	char		fname[NAMEDATALEN];		/* hash key (MUST BE FIRST) */
	char	   *val;
	char	   *json;
	bool		isnull;
} JsonHashEntry;

/* these two are stolen from hstore / record_out, used in populate_record* */
typedef struct ColumnIOData
{
	Oid			column_type;
	Oid			typiofunc;
	Oid			typioparam;
	FmgrInfo	proc;
} ColumnIOData;

typedef struct RecordIOData
{
	Oid			record_type;
	int32		record_typmod;
	int			ncolumns;
	ColumnIOData columns[1];	/* VARIABLE LENGTH ARRAY */
} RecordIOData;

/* state for populate_recordset */
typedef struct PopulateRecordsetState
{
	JsonLexContext *lex;
	const char *function_name;
	HTAB	   *json_hash;
	char	   *saved_scalar;
	char	   *save_json_start;
	Tuplestorestate *tuple_store;
	TupleDesc	ret_tdesc;
	HeapTupleHeader rec;
	RecordIOData *my_extra;
	MemoryContext fn_mcxt;		/* used to stash IO funcs */
} PopulateRecordsetState;

/* Turn a jsonb object into a record */
static void make_row_from_rec_and_jsonb(Jsonb *element,
							PopulateRecordsetState *state);

/*
 * SQL function json_object_keys
 *
 * Returns the set of keys for the object argument.
 *
 * This SRF operates in value-per-call mode. It processes the
 * object during the first call, and the keys are simply stashed
 * in an array, whose size is expanded as necessary. This is probably
 * safe enough for a list of keys of a single object, since they are
 * limited in size to NAMEDATALEN and the number of keys is unlikely to
 * be so huge that it has major memory implications.
 */
Datum
jsonb_object_keys(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	OkeysState *state;
	int			i;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		Jsonb	   *jb = PG_GETARG_JSONB(0);
		bool		skipNested = false;
		JsonbIterator *it;
		JsonbValue	v;
		int			r;

		if (JB_ROOT_IS_SCALAR(jb))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot call %s on a scalar",
							"jsonb_object_keys")));
		else if (JB_ROOT_IS_ARRAY(jb))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot call %s on an array",
							"jsonb_object_keys")));

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		state = palloc(sizeof(OkeysState));

		state->result_size = JB_ROOT_COUNT(jb);
		state->result_count = 0;
		state->sent_count = 0;
		state->result = palloc(state->result_size * sizeof(char *));

		it = JsonbIteratorInit(&jb->root);

		while ((r = JsonbIteratorNext(&it, &v, skipNested)) != WJB_DONE)
		{
			skipNested = true;

			if (r == WJB_KEY)
			{
				char	   *cstr;

				cstr = palloc(v.val.string.len + 1 * sizeof(char));
				memcpy(cstr, v.val.string.val, v.val.string.len);
				cstr[v.val.string.len] = '\0';
				state->result[state->result_count++] = cstr;
			}
		}

		MemoryContextSwitchTo(oldcontext);
		funcctx->user_fctx = (void *) state;
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (OkeysState *) funcctx->user_fctx;

	if (state->sent_count < state->result_count)
	{
		char	   *nxt = state->result[state->sent_count++];

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(nxt));
	}

	/* cleanup to reduce or eliminate memory leaks */
	for (i = 0; i < state->result_count; i++)
		pfree(state->result[i]);
	pfree(state->result);
	pfree(state);

	SRF_RETURN_DONE(funcctx);
}


Datum
json_object_keys(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	OkeysState *state;
	int			i;

	if (SRF_IS_FIRSTCALL())
	{
		text	   *json = PG_GETARG_TEXT_P(0);
		JsonLexContext *lex = makeJsonLexContext(json, true);
		JsonSemAction *sem;
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		state = palloc(sizeof(OkeysState));
		sem = palloc0(sizeof(JsonSemAction));

		state->lex = lex;
		state->result_size = 256;
		state->result_count = 0;
		state->sent_count = 0;
		state->result = palloc(256 * sizeof(char *));

		sem->semstate = (void *) state;
		sem->array_start = okeys_array_start;
		sem->scalar = okeys_scalar;
		sem->object_field_start = okeys_object_field_start;
		/* remainder are all NULL, courtesy of palloc0 above */

		pg_parse_json(lex, sem);
		/* keys are now in state->result */

		pfree(lex->strval->data);
		pfree(lex->strval);
		pfree(lex);
		pfree(sem);

		MemoryContextSwitchTo(oldcontext);
		funcctx->user_fctx = (void *) state;
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (OkeysState *) funcctx->user_fctx;

	if (state->sent_count < state->result_count)
	{
		char	   *nxt = state->result[state->sent_count++];

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(nxt));
	}

	/* cleanup to reduce or eliminate memory leaks */
	for (i = 0; i < state->result_count; i++)
		pfree(state->result[i]);
	pfree(state->result);
	pfree(state);

	SRF_RETURN_DONE(funcctx);
}

static void
okeys_object_field_start(void *state, char *fname, bool isnull)
{
	OkeysState *_state = (OkeysState *) state;

	/* only collecting keys for the top level object */
	if (_state->lex->lex_level != 1)
		return;

	/* enlarge result array if necessary */
	if (_state->result_count >= _state->result_size)
	{
		_state->result_size *= 2;
		_state->result = (char **)
			repalloc(_state->result, sizeof(char *) * _state->result_size);
	}

	/* save a copy of the field name */
	_state->result[_state->result_count++] = pstrdup(fname);
}

static void
okeys_array_start(void *state)
{
	OkeysState *_state = (OkeysState *) state;

	/* top level must be a json object */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on an array",
						"json_object_keys")));
}

static void
okeys_scalar(void *state, char *token, JsonTokenType tokentype)
{
	OkeysState *_state = (OkeysState *) state;

	/* top level must be a json object */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on a scalar",
						"json_object_keys")));
}

/*
 * json and jsonb getter functions
 * these implement the -> ->> #> and #>> operators
 * and the json{b?}_extract_path*(json, text, ...) functions
 */


Datum
json_object_field(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_P(0);
	text	   *fname = PG_GETARG_TEXT_PP(1);
	char	   *fnamestr = text_to_cstring(fname);
	text	   *result;

	result = get_worker(json, &fnamestr, NULL, 1, false);

	if (result != NULL)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

Datum
jsonb_object_field(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB(0);
	text	   *key = PG_GETARG_TEXT_PP(1);
	JsonbValue *v;

	if (!JB_ROOT_IS_OBJECT(jb))
		PG_RETURN_NULL();

	v = findJsonbValueFromContainerLen(&jb->root, JB_FOBJECT,
									   VARDATA_ANY(key),
									   VARSIZE_ANY_EXHDR(key));

	if (v != NULL)
		PG_RETURN_JSONB(JsonbValueToJsonb(v));

	PG_RETURN_NULL();
}

Datum
json_object_field_text(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_P(0);
	text	   *fname = PG_GETARG_TEXT_PP(1);
	char	   *fnamestr = text_to_cstring(fname);
	text	   *result;

	result = get_worker(json, &fnamestr, NULL, 1, true);

	if (result != NULL)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

Datum
jsonb_object_field_text(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB(0);
	text	   *key = PG_GETARG_TEXT_PP(1);
	JsonbValue *v;

	if (!JB_ROOT_IS_OBJECT(jb))
		PG_RETURN_NULL();

	v = findJsonbValueFromContainerLen(&jb->root, JB_FOBJECT,
									   VARDATA_ANY(key),
									   VARSIZE_ANY_EXHDR(key));

	if (v != NULL)
	{
		text	   *result = NULL;

		switch (v->type)
		{
			case jbvNull:
				break;
			case jbvBool:
				result = cstring_to_text(v->val.boolean ? "true" : "false");
				break;
			case jbvString:
				result = cstring_to_text_with_len(v->val.string.val, v->val.string.len);
				break;
			case jbvNumeric:
				result = cstring_to_text(DatumGetCString(DirectFunctionCall1(numeric_out,
										  PointerGetDatum(v->val.numeric))));
				break;
			case jbvBinary:
				{
					StringInfo	jtext = makeStringInfo();

					(void) JsonbToCString(jtext, v->val.binary.data, -1);
					result = cstring_to_text_with_len(jtext->data, jtext->len);
				}
				break;
			default:
				elog(ERROR, "unrecognized jsonb type: %d", (int) v->type);
		}

		if (result)
			PG_RETURN_TEXT_P(result);
	}

	PG_RETURN_NULL();
}

Datum
json_array_element(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_P(0);
	int			element = PG_GETARG_INT32(1);
	text	   *result;

	result = get_worker(json, NULL, &element, 1, false);

	if (result != NULL)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

Datum
jsonb_array_element(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB(0);
	int			element = PG_GETARG_INT32(1);
	JsonbValue *v;

	if (!JB_ROOT_IS_ARRAY(jb))
		PG_RETURN_NULL();

	v = getIthJsonbValueFromContainer(&jb->root, element);
	if (v != NULL)
		PG_RETURN_JSONB(JsonbValueToJsonb(v));

	PG_RETURN_NULL();
}

Datum
json_array_element_text(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_P(0);
	int			element = PG_GETARG_INT32(1);
	text	   *result;

	result = get_worker(json, NULL, &element, 1, true);

	if (result != NULL)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

Datum
jsonb_array_element_text(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB(0);
	int			element = PG_GETARG_INT32(1);
	JsonbValue *v;

	if (!JB_ROOT_IS_ARRAY(jb))
		PG_RETURN_NULL();

	v = getIthJsonbValueFromContainer(&jb->root, element);
	if (v != NULL)
	{
		text	   *result = NULL;

		switch (v->type)
		{
			case jbvNull:
				break;
			case jbvBool:
				result = cstring_to_text(v->val.boolean ? "true" : "false");
				break;
			case jbvString:
				result = cstring_to_text_with_len(v->val.string.val, v->val.string.len);
				break;
			case jbvNumeric:
				result = cstring_to_text(DatumGetCString(DirectFunctionCall1(numeric_out,
										  PointerGetDatum(v->val.numeric))));
				break;
			case jbvBinary:
				{
					StringInfo	jtext = makeStringInfo();

					(void) JsonbToCString(jtext, v->val.binary.data, -1);
					result = cstring_to_text_with_len(jtext->data, jtext->len);
				}
				break;
			default:
				elog(ERROR, "unrecognized jsonb type: %d", (int) v->type);
		}

		if (result)
			PG_RETURN_TEXT_P(result);
	}

	PG_RETURN_NULL();
}

Datum
json_extract_path(PG_FUNCTION_ARGS)
{
	return get_path_all(fcinfo, false);
}

Datum
json_extract_path_text(PG_FUNCTION_ARGS)
{
	return get_path_all(fcinfo, true);
}

/*
 * common routine for extract_path functions
 */
static Datum
get_path_all(FunctionCallInfo fcinfo, bool as_text)
{
	text	   *json = PG_GETARG_TEXT_P(0);
	ArrayType  *path = PG_GETARG_ARRAYTYPE_P(1);
	text	   *result;
	Datum	   *pathtext;
	bool	   *pathnulls;
	int			npath;
	char	  **tpath;
	int		   *ipath;
	int			i;

	/*
	 * If the array contains any null elements, return NULL, on the grounds
	 * that you'd have gotten NULL if any RHS value were NULL in a nested
	 * series of applications of the -> operator.  (Note: because we also
	 * return NULL for error cases such as no-such-field, this is true
	 * regardless of the contents of the rest of the array.)
	 */
	if (array_contains_nulls(path))
		PG_RETURN_NULL();

	deconstruct_array(path, TEXTOID, -1, false, 'i',
					  &pathtext, &pathnulls, &npath);

	tpath = palloc(npath * sizeof(char *));
	ipath = palloc(npath * sizeof(int));

	for (i = 0; i < npath; i++)
	{
		Assert(!pathnulls[i]);
		tpath[i] = TextDatumGetCString(pathtext[i]);

		/*
		 * we have no idea at this stage what structure the document is so
		 * just convert anything in the path that we can to an integer and set
		 * all the other integers to -1 which will never match.
		 */
		if (*tpath[i] != '\0')
		{
			long		ind;
			char	   *endptr;

			errno = 0;
			ind = strtol(tpath[i], &endptr, 10);
			if (*endptr == '\0' && errno == 0 && ind <= INT_MAX && ind >= 0)
				ipath[i] = (int) ind;
			else
				ipath[i] = -1;
		}
		else
			ipath[i] = -1;
	}

	result = get_worker(json, tpath, ipath, npath, as_text);

	if (result != NULL)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

/*
 * get_worker
 *
 * common worker for all the json getter functions
 *
 * json: JSON object (in text form)
 * tpath[]: field name(s) to extract
 * ipath[]: array index(es) (zero-based) to extract
 * npath: length of tpath[] and/or ipath[]
 * normalize_results: true to de-escape string and null scalars
 *
 * tpath can be NULL, or any one tpath[] entry can be NULL, if an object
 * field is not to be matched at that nesting level.  Similarly, ipath can
 * be NULL, or any one ipath[] entry can be -1, if an array element is not
 * to be matched at that nesting level.
 */
static text *
get_worker(text *json,
		   char **tpath,
		   int *ipath,
		   int npath,
		   bool normalize_results)
{
	JsonLexContext *lex = makeJsonLexContext(json, true);
	JsonSemAction *sem = palloc0(sizeof(JsonSemAction));
	GetState   *state = palloc0(sizeof(GetState));

	Assert(npath >= 0);

	state->lex = lex;
	/* is it "_as_text" variant? */
	state->normalize_results = normalize_results;
	state->npath = npath;
	state->path_names = tpath;
	state->path_indexes = ipath;
	state->pathok = palloc0(sizeof(bool) * npath);
	state->array_cur_index = palloc(sizeof(int) * npath);

	if (npath > 0)
		state->pathok[0] = true;

	sem->semstate = (void *) state;

	/*
	 * Not all variants need all the semantic routines. Only set the ones that
	 * are actually needed for maximum efficiency.
	 */
	sem->scalar = get_scalar;
	if (npath == 0)
	{
		sem->object_start = get_object_start;
		sem->object_end = get_object_end;
		sem->array_start = get_array_start;
		sem->array_end = get_array_end;
	}
	if (tpath != NULL)
	{
		sem->object_field_start = get_object_field_start;
		sem->object_field_end = get_object_field_end;
	}
	if (ipath != NULL)
	{
		sem->array_start = get_array_start;
		sem->array_element_start = get_array_element_start;
		sem->array_element_end = get_array_element_end;
	}

	pg_parse_json(lex, sem);

	return state->tresult;
}

static void
get_object_start(void *state)
{
	GetState   *_state = (GetState *) state;
	int			lex_level = _state->lex->lex_level;

	if (lex_level == 0 && _state->npath == 0)
	{
		/*
		 * Special case: we should match the entire object.  We only need this
		 * at outermost level because at nested levels the match will have
		 * been started by the outer field or array element callback.
		 */
		_state->result_start = _state->lex->token_start;
	}
}

static void
get_object_end(void *state)
{
	GetState   *_state = (GetState *) state;
	int			lex_level = _state->lex->lex_level;

	if (lex_level == 0 && _state->npath == 0)
	{
		/* Special case: return the entire object */
		char	   *start = _state->result_start;
		int			len = _state->lex->prev_token_terminator - start;

		_state->tresult = cstring_to_text_with_len(start, len);
	}
}

static void
get_object_field_start(void *state, char *fname, bool isnull)
{
	GetState   *_state = (GetState *) state;
	bool		get_next = false;
	int			lex_level = _state->lex->lex_level;

	if (lex_level <= _state->npath &&
		_state->pathok[lex_level - 1] &&
		_state->path_names != NULL &&
		_state->path_names[lex_level - 1] != NULL &&
		strcmp(fname, _state->path_names[lex_level - 1]) == 0)
	{
		if (lex_level < _state->npath)
		{
			/* if not at end of path just mark path ok */
			_state->pathok[lex_level] = true;
		}
		else
		{
			/* end of path, so we want this value */
			get_next = true;
		}
	}

	if (get_next)
	{
		/* this object overrides any previous matching object */
		_state->tresult = NULL;
		_state->result_start = NULL;

		if (_state->normalize_results &&
			_state->lex->token_type == JSON_TOKEN_STRING)
		{
			/* for as_text variants, tell get_scalar to set it for us */
			_state->next_scalar = true;
		}
		else
		{
			/* for non-as_text variants, just note the json starting point */
			_state->result_start = _state->lex->token_start;
		}
	}
}

static void
get_object_field_end(void *state, char *fname, bool isnull)
{
	GetState   *_state = (GetState *) state;
	bool		get_last = false;
	int			lex_level = _state->lex->lex_level;

	/* same tests as in get_object_field_start */
	if (lex_level <= _state->npath &&
		_state->pathok[lex_level - 1] &&
		_state->path_names != NULL &&
		_state->path_names[lex_level - 1] != NULL &&
		strcmp(fname, _state->path_names[lex_level - 1]) == 0)
	{
		if (lex_level < _state->npath)
		{
			/* done with this field so reset pathok */
			_state->pathok[lex_level] = false;
		}
		else
		{
			/* end of path, so we want this value */
			get_last = true;
		}
	}

	/* for as_text scalar case, our work is already done */
	if (get_last && _state->result_start != NULL)
	{
		/*
		 * make a text object from the string from the prevously noted json
		 * start up to the end of the previous token (the lexer is by now
		 * ahead of us on whatever came after what we're interested in).
		 */
		if (isnull && _state->normalize_results)
			_state->tresult = (text *) NULL;
		else
		{
			char	   *start = _state->result_start;
			int			len = _state->lex->prev_token_terminator - start;

			_state->tresult = cstring_to_text_with_len(start, len);
		}

		/* this should be unnecessary but let's do it for cleanliness: */
		_state->result_start = NULL;
	}
}

static void
get_array_start(void *state)
{
	GetState   *_state = (GetState *) state;
	int			lex_level = _state->lex->lex_level;

	if (lex_level < _state->npath)
	{
		/* Initialize counting of elements in this array */
		_state->array_cur_index[lex_level] = -1;
	}
	else if (lex_level == 0 && _state->npath == 0)
	{
		/*
		 * Special case: we should match the entire array.  We only need this
		 * at outermost level because at nested levels the match will have
		 * been started by the outer field or array element callback.
		 */
		_state->result_start = _state->lex->token_start;
	}
}

static void
get_array_end(void *state)
{
	GetState   *_state = (GetState *) state;
	int			lex_level = _state->lex->lex_level;

	if (lex_level == 0 && _state->npath == 0)
	{
		/* Special case: return the entire array */
		char	   *start = _state->result_start;
		int			len = _state->lex->prev_token_terminator - start;

		_state->tresult = cstring_to_text_with_len(start, len);
	}
}

static void
get_array_element_start(void *state, bool isnull)
{
	GetState   *_state = (GetState *) state;
	bool		get_next = false;
	int			lex_level = _state->lex->lex_level;

	/* Update array element counter */
	if (lex_level <= _state->npath)
		_state->array_cur_index[lex_level - 1]++;

	if (lex_level <= _state->npath &&
		_state->pathok[lex_level - 1] &&
		_state->path_indexes != NULL &&
		_state->array_cur_index[lex_level - 1] == _state->path_indexes[lex_level - 1])
	{
		if (lex_level < _state->npath)
		{
			/* if not at end of path just mark path ok */
			_state->pathok[lex_level] = true;
		}
		else
		{
			/* end of path, so we want this value */
			get_next = true;
		}
	}

	/* same logic as for objects */
	if (get_next)
	{
		_state->tresult = NULL;
		_state->result_start = NULL;

		if (_state->normalize_results &&
			_state->lex->token_type == JSON_TOKEN_STRING)
		{
			_state->next_scalar = true;
		}
		else
		{
			_state->result_start = _state->lex->token_start;
		}
	}
}

static void
get_array_element_end(void *state, bool isnull)
{
	GetState   *_state = (GetState *) state;
	bool		get_last = false;
	int			lex_level = _state->lex->lex_level;

	/* same tests as in get_array_element_start */
	if (lex_level <= _state->npath &&
		_state->pathok[lex_level - 1] &&
		_state->path_indexes != NULL &&
		_state->array_cur_index[lex_level - 1] == _state->path_indexes[lex_level - 1])
	{
		if (lex_level < _state->npath)
		{
			/* done with this element so reset pathok */
			_state->pathok[lex_level] = false;
		}
		else
		{
			/* end of path, so we want this value */
			get_last = true;
		}
	}

	/* same logic as for objects */
	if (get_last && _state->result_start != NULL)
	{
		if (isnull && _state->normalize_results)
			_state->tresult = (text *) NULL;
		else
		{
			char	   *start = _state->result_start;
			int			len = _state->lex->prev_token_terminator - start;

			_state->tresult = cstring_to_text_with_len(start, len);
		}

		_state->result_start = NULL;
	}
}

static void
get_scalar(void *state, char *token, JsonTokenType tokentype)
{
	GetState   *_state = (GetState *) state;
	int			lex_level = _state->lex->lex_level;

	/* Check for whole-object match */
	if (lex_level == 0 && _state->npath == 0)
	{
		if (_state->normalize_results && tokentype == JSON_TOKEN_STRING)
		{
			/* we want the de-escaped string */
			_state->next_scalar = true;
		}
		else if (_state->normalize_results && tokentype == JSON_TOKEN_NULL)
		{
			_state->tresult = (text *) NULL;
		}
		else
		{
			/*
			 * This is a bit hokey: we will suppress whitespace after the
			 * scalar token, but not whitespace before it.  Probably not worth
			 * doing our own space-skipping to avoid that.
			 */
			char	   *start = _state->lex->input;
			int			len = _state->lex->prev_token_terminator - start;

			_state->tresult = cstring_to_text_with_len(start, len);
		}
	}

	if (_state->next_scalar)
	{
		/* a de-escaped text value is wanted, so supply it */
		_state->tresult = cstring_to_text(token);
		/* make sure the next call to get_scalar doesn't overwrite it */
		_state->next_scalar = false;
	}
}

Datum
jsonb_extract_path(PG_FUNCTION_ARGS)
{
	return get_jsonb_path_all(fcinfo, false);
}

Datum
jsonb_extract_path_text(PG_FUNCTION_ARGS)
{
	return get_jsonb_path_all(fcinfo, true);
}

static Datum
get_jsonb_path_all(FunctionCallInfo fcinfo, bool as_text)
{
	Jsonb	   *jb = PG_GETARG_JSONB(0);
	ArrayType  *path = PG_GETARG_ARRAYTYPE_P(1);
	Jsonb	   *res;
	Datum	   *pathtext;
	bool	   *pathnulls;
	int			npath;
	int			i;
	bool		have_object = false,
				have_array = false;
	JsonbValue *jbvp = NULL;
	JsonbValue	tv;
	JsonbContainer *container;

	/*
	 * If the array contains any null elements, return NULL, on the grounds
	 * that you'd have gotten NULL if any RHS value were NULL in a nested
	 * series of applications of the -> operator.  (Note: because we also
	 * return NULL for error cases such as no-such-field, this is true
	 * regardless of the contents of the rest of the array.)
	 */
	if (array_contains_nulls(path))
		PG_RETURN_NULL();

	deconstruct_array(path, TEXTOID, -1, false, 'i',
					  &pathtext, &pathnulls, &npath);

	/* Identify whether we have object, array, or scalar at top-level */
	container = &jb->root;

	if (JB_ROOT_IS_OBJECT(jb))
		have_object = true;
	else if (JB_ROOT_IS_ARRAY(jb) && !JB_ROOT_IS_SCALAR(jb))
		have_array = true;
	else
	{
		Assert(JB_ROOT_IS_ARRAY(jb) && JB_ROOT_IS_SCALAR(jb));
		/* Extract the scalar value, if it is what we'll return */
		if (npath <= 0)
			jbvp = getIthJsonbValueFromContainer(container, 0);
	}

	/*
	 * If the array is empty, return the entire LHS object, on the grounds
	 * that we should do zero field or element extractions.  For the
	 * non-scalar case we can just hand back the object without much work. For
	 * the scalar case, fall through and deal with the value below the loop.
	 * (This inconsistency arises because there's no easy way to generate a
	 * JsonbValue directly for root-level containers.)
	 */
	if (npath <= 0 && jbvp == NULL)
	{
		if (as_text)
		{
			PG_RETURN_TEXT_P(cstring_to_text(JsonbToCString(NULL,
															container,
															VARSIZE(jb))));
		}
		else
		{
			/* not text mode - just hand back the jsonb */
			PG_RETURN_JSONB(jb);
		}
	}

	for (i = 0; i < npath; i++)
	{
		if (have_object)
		{
			jbvp = findJsonbValueFromContainerLen(container,
												  JB_FOBJECT,
												  VARDATA_ANY(pathtext[i]),
											 VARSIZE_ANY_EXHDR(pathtext[i]));
		}
		else if (have_array)
		{
			long		lindex;
			uint32		index;
			char	   *indextext = TextDatumGetCString(pathtext[i]);
			char	   *endptr;

			errno = 0;
			lindex = strtol(indextext, &endptr, 10);
			if (endptr == indextext || *endptr != '\0' || errno != 0 ||
				lindex > INT_MAX || lindex < 0)
				PG_RETURN_NULL();
			index = (uint32) lindex;
			jbvp = getIthJsonbValueFromContainer(container, index);
		}
		else
		{
			/* scalar, extraction yields a null */
			PG_RETURN_NULL();
		}

		if (jbvp == NULL)
			PG_RETURN_NULL();
		else if (i == npath - 1)
			break;

		if (jbvp->type == jbvBinary)
		{
			JsonbIterator *it = JsonbIteratorInit((JsonbContainer *) jbvp->val.binary.data);
			int			r;

			r = JsonbIteratorNext(&it, &tv, true);
			container = (JsonbContainer *) jbvp->val.binary.data;
			have_object = r == WJB_BEGIN_OBJECT;
			have_array = r == WJB_BEGIN_ARRAY;
		}
		else
		{
			have_object = jbvp->type == jbvObject;
			have_array = jbvp->type == jbvArray;
		}
	}

	if (as_text)
	{
		/* special-case outputs for string and null values */
		if (jbvp->type == jbvString)
			PG_RETURN_TEXT_P(cstring_to_text_with_len(jbvp->val.string.val,
													  jbvp->val.string.len));
		if (jbvp->type == jbvNull)
			PG_RETURN_NULL();
	}

	res = JsonbValueToJsonb(jbvp);

	if (as_text)
	{
		PG_RETURN_TEXT_P(cstring_to_text(JsonbToCString(NULL,
														&res->root,
														VARSIZE(res))));
	}
	else
	{
		/* not text mode - just hand back the jsonb */
		PG_RETURN_JSONB(res);
	}
}

/*
 * SQL function json_array_length(json) -> int
 */
Datum
json_array_length(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_P(0);
	AlenState  *state;
	JsonLexContext *lex;
	JsonSemAction *sem;

	lex = makeJsonLexContext(json, false);
	state = palloc0(sizeof(AlenState));
	sem = palloc0(sizeof(JsonSemAction));

	/* palloc0 does this for us */
#if 0
	state->count = 0;
#endif
	state->lex = lex;

	sem->semstate = (void *) state;
	sem->object_start = alen_object_start;
	sem->scalar = alen_scalar;
	sem->array_element_start = alen_array_element_start;

	pg_parse_json(lex, sem);

	PG_RETURN_INT32(state->count);
}

Datum
jsonb_array_length(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB(0);

	if (JB_ROOT_IS_SCALAR(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot get array length of a scalar")));
	else if (!JB_ROOT_IS_ARRAY(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot get array length of a non-array")));

	PG_RETURN_INT32(JB_ROOT_COUNT(jb));
}

/*
 * These next two checks ensure that the json is an array (since it can't be
 * a scalar or an object).
 */

static void
alen_object_start(void *state)
{
	AlenState  *_state = (AlenState *) state;

	/* json structure check */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot get array length of a non-array")));
}

static void
alen_scalar(void *state, char *token, JsonTokenType tokentype)
{
	AlenState  *_state = (AlenState *) state;

	/* json structure check */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot get array length of a scalar")));
}

static void
alen_array_element_start(void *state, bool isnull)
{
	AlenState  *_state = (AlenState *) state;

	/* just count up all the level 1 elements */
	if (_state->lex->lex_level == 1)
		_state->count++;
}

/*
 * SQL function json_each and json_each_text
 *
 * decompose a json object into key value pairs.
 *
 * Unlike json_object_keys() these SRFs operate in materialize mode,
 * stashing results into a Tuplestore object as they go.
 * The construction of tuples is done using a temporary memory context
 * that is cleared out after each tuple is built.
 */
Datum
json_each(PG_FUNCTION_ARGS)
{
	return each_worker(fcinfo, false);
}

Datum
jsonb_each(PG_FUNCTION_ARGS)
{
	return each_worker_jsonb(fcinfo, "jsonb_each", false);
}

Datum
json_each_text(PG_FUNCTION_ARGS)
{
	return each_worker(fcinfo, true);
}

Datum
jsonb_each_text(PG_FUNCTION_ARGS)
{
	return each_worker_jsonb(fcinfo, "jsonb_each_text", true);
}

static Datum
each_worker_jsonb(FunctionCallInfo fcinfo, const char *funcname, bool as_text)
{
	Jsonb	   *jb = PG_GETARG_JSONB(0);
	ReturnSetInfo *rsi;
	Tuplestorestate *tuple_store;
	TupleDesc	tupdesc;
	TupleDesc	ret_tdesc;
	MemoryContext old_cxt,
				tmp_cxt;
	bool		skipNested = false;
	JsonbIterator *it;
	JsonbValue	v;
	int			r;

	if (!JB_ROOT_IS_OBJECT(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on a non-object",
						funcname)));

	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	if (!rsi || !IsA(rsi, ReturnSetInfo) ||
		(rsi->allowedModes & SFRM_Materialize) == 0 ||
		rsi->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that "
						"cannot accept a set")));

	rsi->returnMode = SFRM_Materialize;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record")));

	old_cxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

	ret_tdesc = CreateTupleDescCopy(tupdesc);
	BlessTupleDesc(ret_tdesc);
	tuple_store =
		tuplestore_begin_heap(rsi->allowedModes & SFRM_Materialize_Random,
							  false, work_mem);

	MemoryContextSwitchTo(old_cxt);

	tmp_cxt = AllocSetContextCreate(CurrentMemoryContext,
									"jsonb_each temporary cxt",
									ALLOCSET_DEFAULT_MINSIZE,
									ALLOCSET_DEFAULT_INITSIZE,
									ALLOCSET_DEFAULT_MAXSIZE);

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, skipNested)) != WJB_DONE)
	{
		skipNested = true;

		if (r == WJB_KEY)
		{
			text	   *key;
			HeapTuple	tuple;
			Datum		values[2];
			bool		nulls[2] = {false, false};

			/* Use the tmp context so we can clean up after each tuple is done */
			old_cxt = MemoryContextSwitchTo(tmp_cxt);

			key = cstring_to_text_with_len(v.val.string.val, v.val.string.len);

			/*
			 * The next thing the iterator fetches should be the value, no
			 * matter what shape it is.
			 */
			r = JsonbIteratorNext(&it, &v, skipNested);

			values[0] = PointerGetDatum(key);

			if (as_text)
			{
				if (v.type == jbvNull)
				{
					/* a json null is an sql null in text mode */
					nulls[1] = true;
					values[1] = (Datum) NULL;
				}
				else
				{
					text	   *sv;

					if (v.type == jbvString)
					{
						/* In text mode, scalar strings should be dequoted */
						sv = cstring_to_text_with_len(v.val.string.val, v.val.string.len);
					}
					else
					{
						/* Turn anything else into a json string */
						StringInfo	jtext = makeStringInfo();
						Jsonb	   *jb = JsonbValueToJsonb(&v);

						(void) JsonbToCString(jtext, &jb->root, 0);
						sv = cstring_to_text_with_len(jtext->data, jtext->len);
					}

					values[1] = PointerGetDatum(sv);
				}
			}
			else
			{
				/* Not in text mode, just return the Jsonb */
				Jsonb	   *val = JsonbValueToJsonb(&v);

				values[1] = PointerGetDatum(val);
			}

			tuple = heap_form_tuple(ret_tdesc, values, nulls);

			tuplestore_puttuple(tuple_store, tuple);

			/* clean up and switch back */
			MemoryContextSwitchTo(old_cxt);
			MemoryContextReset(tmp_cxt);
		}
	}

	MemoryContextDelete(tmp_cxt);

	rsi->setResult = tuple_store;
	rsi->setDesc = ret_tdesc;

	PG_RETURN_NULL();
}


static Datum
each_worker(FunctionCallInfo fcinfo, bool as_text)
{
	text	   *json = PG_GETARG_TEXT_P(0);
	JsonLexContext *lex;
	JsonSemAction *sem;
	ReturnSetInfo *rsi;
	MemoryContext old_cxt;
	TupleDesc	tupdesc;
	EachState  *state;

	lex = makeJsonLexContext(json, true);
	state = palloc0(sizeof(EachState));
	sem = palloc0(sizeof(JsonSemAction));

	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	if (!rsi || !IsA(rsi, ReturnSetInfo) ||
		(rsi->allowedModes & SFRM_Materialize) == 0 ||
		rsi->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that "
						"cannot accept a set")));

	rsi->returnMode = SFRM_Materialize;

	(void) get_call_result_type(fcinfo, NULL, &tupdesc);

	/* make these in a sufficiently long-lived memory context */
	old_cxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

	state->ret_tdesc = CreateTupleDescCopy(tupdesc);
	BlessTupleDesc(state->ret_tdesc);
	state->tuple_store =
		tuplestore_begin_heap(rsi->allowedModes & SFRM_Materialize_Random,
							  false, work_mem);

	MemoryContextSwitchTo(old_cxt);

	sem->semstate = (void *) state;
	sem->array_start = each_array_start;
	sem->scalar = each_scalar;
	sem->object_field_start = each_object_field_start;
	sem->object_field_end = each_object_field_end;

	state->normalize_results = as_text;
	state->next_scalar = false;
	state->lex = lex;
	state->tmp_cxt = AllocSetContextCreate(CurrentMemoryContext,
										   "json_each temporary cxt",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);

	pg_parse_json(lex, sem);

	MemoryContextDelete(state->tmp_cxt);

	rsi->setResult = state->tuple_store;
	rsi->setDesc = state->ret_tdesc;

	PG_RETURN_NULL();
}


static void
each_object_field_start(void *state, char *fname, bool isnull)
{
	EachState  *_state = (EachState *) state;

	/* save a pointer to where the value starts */
	if (_state->lex->lex_level == 1)
	{
		/*
		 * next_scalar will be reset in the object_field_end handler, and
		 * since we know the value is a scalar there is no danger of it being
		 * on while recursing down the tree.
		 */
		if (_state->normalize_results && _state->lex->token_type == JSON_TOKEN_STRING)
			_state->next_scalar = true;
		else
			_state->result_start = _state->lex->token_start;
	}
}

static void
each_object_field_end(void *state, char *fname, bool isnull)
{
	EachState  *_state = (EachState *) state;
	MemoryContext old_cxt;
	int			len;
	text	   *val;
	HeapTuple	tuple;
	Datum		values[2];
	bool		nulls[2] = {false, false};

	/* skip over nested objects */
	if (_state->lex->lex_level != 1)
		return;

	/* use the tmp context so we can clean up after each tuple is done */
	old_cxt = MemoryContextSwitchTo(_state->tmp_cxt);

	values[0] = CStringGetTextDatum(fname);

	if (isnull && _state->normalize_results)
	{
		nulls[1] = true;
		values[1] = (Datum) 0;
	}
	else if (_state->next_scalar)
	{
		values[1] = CStringGetTextDatum(_state->normalized_scalar);
		_state->next_scalar = false;
	}
	else
	{
		len = _state->lex->prev_token_terminator - _state->result_start;
		val = cstring_to_text_with_len(_state->result_start, len);
		values[1] = PointerGetDatum(val);
	}

	tuple = heap_form_tuple(_state->ret_tdesc, values, nulls);

	tuplestore_puttuple(_state->tuple_store, tuple);

	/* clean up and switch back */
	MemoryContextSwitchTo(old_cxt);
	MemoryContextReset(_state->tmp_cxt);
}

static void
each_array_start(void *state)
{
	EachState  *_state = (EachState *) state;

	/* json structure check */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot deconstruct an array as an object")));
}

static void
each_scalar(void *state, char *token, JsonTokenType tokentype)
{
	EachState  *_state = (EachState *) state;

	/* json structure check */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot deconstruct a scalar")));

	/* supply de-escaped value if required */
	if (_state->next_scalar)
		_state->normalized_scalar = token;
}

/*
 * SQL functions json_array_elements and json_array_elements_text
 *
 * get the elements from a json array
 *
 * a lot of this processing is similar to the json_each* functions
 */

Datum
jsonb_array_elements(PG_FUNCTION_ARGS)
{
	return elements_worker_jsonb(fcinfo, "jsonb_array_elements", false);
}

Datum
jsonb_array_elements_text(PG_FUNCTION_ARGS)
{
	return elements_worker_jsonb(fcinfo, "jsonb_array_elements_text", true);
}

static Datum
elements_worker_jsonb(FunctionCallInfo fcinfo, const char *funcname,
					  bool as_text)
{
	Jsonb	   *jb = PG_GETARG_JSONB(0);
	ReturnSetInfo *rsi;
	Tuplestorestate *tuple_store;
	TupleDesc	tupdesc;
	TupleDesc	ret_tdesc;
	MemoryContext old_cxt,
				tmp_cxt;
	bool		skipNested = false;
	JsonbIterator *it;
	JsonbValue	v;
	int			r;

	if (JB_ROOT_IS_SCALAR(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot extract elements from a scalar")));
	else if (!JB_ROOT_IS_ARRAY(jb))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot extract elements from an object")));

	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	if (!rsi || !IsA(rsi, ReturnSetInfo) ||
		(rsi->allowedModes & SFRM_Materialize) == 0 ||
		rsi->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that "
						"cannot accept a set")));

	rsi->returnMode = SFRM_Materialize;

	/* it's a simple type, so don't use get_call_result_type() */
	tupdesc = rsi->expectedDesc;

	old_cxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

	ret_tdesc = CreateTupleDescCopy(tupdesc);
	BlessTupleDesc(ret_tdesc);
	tuple_store =
		tuplestore_begin_heap(rsi->allowedModes & SFRM_Materialize_Random,
							  false, work_mem);

	MemoryContextSwitchTo(old_cxt);

	tmp_cxt = AllocSetContextCreate(CurrentMemoryContext,
									"jsonb_array_elements temporary cxt",
									ALLOCSET_DEFAULT_MINSIZE,
									ALLOCSET_DEFAULT_INITSIZE,
									ALLOCSET_DEFAULT_MAXSIZE);

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, skipNested)) != WJB_DONE)
	{
		skipNested = true;

		if (r == WJB_ELEM)
		{
			HeapTuple	tuple;
			Datum		values[1];
			bool		nulls[1] = {false};

			/* use the tmp context so we can clean up after each tuple is done */
			old_cxt = MemoryContextSwitchTo(tmp_cxt);

			if (!as_text)
			{
				Jsonb	   *val = JsonbValueToJsonb(&v);

				values[0] = PointerGetDatum(val);
			}
			else
			{
				if (v.type == jbvNull)
				{
					/* a json null is an sql null in text mode */
					nulls[0] = true;
					values[0] = (Datum) NULL;
				}
				else
				{
					text	   *sv;

					if (v.type == jbvString)
					{
						/* in text mode scalar strings should be dequoted */
						sv = cstring_to_text_with_len(v.val.string.val, v.val.string.len);
					}
					else
					{
						/* turn anything else into a json string */
						StringInfo	jtext = makeStringInfo();
						Jsonb	   *jb = JsonbValueToJsonb(&v);

						(void) JsonbToCString(jtext, &jb->root, 0);
						sv = cstring_to_text_with_len(jtext->data, jtext->len);
					}

					values[0] = PointerGetDatum(sv);
				}
			}

			tuple = heap_form_tuple(ret_tdesc, values, nulls);

			tuplestore_puttuple(tuple_store, tuple);

			/* clean up and switch back */
			MemoryContextSwitchTo(old_cxt);
			MemoryContextReset(tmp_cxt);
		}
	}

	MemoryContextDelete(tmp_cxt);

	rsi->setResult = tuple_store;
	rsi->setDesc = ret_tdesc;

	PG_RETURN_NULL();
}

Datum
json_array_elements(PG_FUNCTION_ARGS)
{
	return elements_worker(fcinfo, "json_array_elements", false);
}

Datum
json_array_elements_text(PG_FUNCTION_ARGS)
{
	return elements_worker(fcinfo, "json_array_elements_text", true);
}

static Datum
elements_worker(FunctionCallInfo fcinfo, const char *funcname, bool as_text)
{
	text	   *json = PG_GETARG_TEXT_P(0);

	/* elements only needs escaped strings when as_text */
	JsonLexContext *lex = makeJsonLexContext(json, as_text);
	JsonSemAction *sem;
	ReturnSetInfo *rsi;
	MemoryContext old_cxt;
	TupleDesc	tupdesc;
	ElementsState *state;

	state = palloc0(sizeof(ElementsState));
	sem = palloc0(sizeof(JsonSemAction));

	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	if (!rsi || !IsA(rsi, ReturnSetInfo) ||
		(rsi->allowedModes & SFRM_Materialize) == 0 ||
		rsi->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that "
						"cannot accept a set")));

	rsi->returnMode = SFRM_Materialize;

	/* it's a simple type, so don't use get_call_result_type() */
	tupdesc = rsi->expectedDesc;

	/* make these in a sufficiently long-lived memory context */
	old_cxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

	state->ret_tdesc = CreateTupleDescCopy(tupdesc);
	BlessTupleDesc(state->ret_tdesc);
	state->tuple_store =
		tuplestore_begin_heap(rsi->allowedModes & SFRM_Materialize_Random,
							  false, work_mem);

	MemoryContextSwitchTo(old_cxt);

	sem->semstate = (void *) state;
	sem->object_start = elements_object_start;
	sem->scalar = elements_scalar;
	sem->array_element_start = elements_array_element_start;
	sem->array_element_end = elements_array_element_end;

	state->function_name = funcname;
	state->normalize_results = as_text;
	state->next_scalar = false;
	state->lex = lex;
	state->tmp_cxt = AllocSetContextCreate(CurrentMemoryContext,
										 "json_array_elements temporary cxt",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);

	pg_parse_json(lex, sem);

	MemoryContextDelete(state->tmp_cxt);

	rsi->setResult = state->tuple_store;
	rsi->setDesc = state->ret_tdesc;

	PG_RETURN_NULL();
}

static void
elements_array_element_start(void *state, bool isnull)
{
	ElementsState *_state = (ElementsState *) state;

	/* save a pointer to where the value starts */
	if (_state->lex->lex_level == 1)
	{
		/*
		 * next_scalar will be reset in the array_element_end handler, and
		 * since we know the value is a scalar there is no danger of it being
		 * on while recursing down the tree.
		 */
		if (_state->normalize_results && _state->lex->token_type == JSON_TOKEN_STRING)
			_state->next_scalar = true;
		else
			_state->result_start = _state->lex->token_start;
	}
}

static void
elements_array_element_end(void *state, bool isnull)
{
	ElementsState *_state = (ElementsState *) state;
	MemoryContext old_cxt;
	int			len;
	text	   *val;
	HeapTuple	tuple;
	Datum		values[1];
	bool		nulls[1] = {false};

	/* skip over nested objects */
	if (_state->lex->lex_level != 1)
		return;

	/* use the tmp context so we can clean up after each tuple is done */
	old_cxt = MemoryContextSwitchTo(_state->tmp_cxt);

	if (isnull && _state->normalize_results)
	{
		nulls[0] = true;
		values[0] = (Datum) NULL;
	}
	else if (_state->next_scalar)
	{
		values[0] = CStringGetTextDatum(_state->normalized_scalar);
		_state->next_scalar = false;
	}
	else
	{
		len = _state->lex->prev_token_terminator - _state->result_start;
		val = cstring_to_text_with_len(_state->result_start, len);
		values[0] = PointerGetDatum(val);
	}

	tuple = heap_form_tuple(_state->ret_tdesc, values, nulls);

	tuplestore_puttuple(_state->tuple_store, tuple);

	/* clean up and switch back */
	MemoryContextSwitchTo(old_cxt);
	MemoryContextReset(_state->tmp_cxt);
}

static void
elements_object_start(void *state)
{
	ElementsState *_state = (ElementsState *) state;

	/* json structure check */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on a non-array",
						_state->function_name)));
}

static void
elements_scalar(void *state, char *token, JsonTokenType tokentype)
{
	ElementsState *_state = (ElementsState *) state;

	/* json structure check */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on a scalar",
						_state->function_name)));

	/* supply de-escaped value if required */
	if (_state->next_scalar)
		_state->normalized_scalar = token;
}

/*
 * SQL function json_populate_record
 *
 * set fields in a record from the argument json
 *
 * Code adapted shamelessly from hstore's populate_record
 * which is in turn partly adapted from record_out.
 *
 * The json is decomposed into a hash table, in which each
 * field in the record is then looked up by name. For jsonb
 * we fetch the values direct from the object.
 */
Datum
jsonb_populate_record(PG_FUNCTION_ARGS)
{
	return populate_record_worker(fcinfo, "jsonb_populate_record", true);
}

Datum
jsonb_to_record(PG_FUNCTION_ARGS)
{
	return populate_record_worker(fcinfo, "jsonb_to_record", false);
}

Datum
json_populate_record(PG_FUNCTION_ARGS)
{
	return populate_record_worker(fcinfo, "json_populate_record", true);
}

Datum
json_to_record(PG_FUNCTION_ARGS)
{
	return populate_record_worker(fcinfo, "json_to_record", false);
}

static Datum
populate_record_worker(FunctionCallInfo fcinfo, const char *funcname,
					   bool have_record_arg)
{
	int			json_arg_num = have_record_arg ? 1 : 0;
	Oid			jtype = get_fn_expr_argtype(fcinfo->flinfo, json_arg_num);
	text	   *json;
	Jsonb	   *jb = NULL;
	HTAB	   *json_hash = NULL;
	HeapTupleHeader rec = NULL;
	Oid			tupType = InvalidOid;
	int32		tupTypmod = -1;
	TupleDesc	tupdesc;
	HeapTupleData tuple;
	HeapTuple	rettuple;
	RecordIOData *my_extra;
	int			ncolumns;
	int			i;
	Datum	   *values;
	bool	   *nulls;

	Assert(jtype == JSONOID || jtype == JSONBOID);

	if (have_record_arg)
	{
		Oid			argtype = get_fn_expr_argtype(fcinfo->flinfo, 0);

		if (!type_is_rowtype(argtype))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("first argument of %s must be a row type",
							funcname)));

		if (PG_ARGISNULL(0))
		{
			if (PG_ARGISNULL(1))
				PG_RETURN_NULL();

			/*
			 * have no tuple to look at, so the only source of type info is
			 * the argtype. The lookup_rowtype_tupdesc call below will error
			 * out if we don't have a known composite type oid here.
			 */
			tupType = argtype;
			tupTypmod = -1;
		}
		else
		{
			rec = PG_GETARG_HEAPTUPLEHEADER(0);

			if (PG_ARGISNULL(1))
				PG_RETURN_POINTER(rec);

			/* Extract type info from the tuple itself */
			tupType = HeapTupleHeaderGetTypeId(rec);
			tupTypmod = HeapTupleHeaderGetTypMod(rec);
		}

		tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	}
	else
	{
		/* json{b}_to_record case */
		if (PG_ARGISNULL(0))
			PG_RETURN_NULL();

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record"),
					 errhint("Try calling the function in the FROM clause "
							 "using a column definition list.")));
	}

	if (jtype == JSONOID)
	{
		/* just get the text */
		json = PG_GETARG_TEXT_P(json_arg_num);

		json_hash = get_json_object_as_hash(json, funcname);

		/*
		 * if the input json is empty, we can only skip the rest if we were
		 * passed in a non-null record, since otherwise there may be issues
		 * with domain nulls.
		 */
		if (hash_get_num_entries(json_hash) == 0 && rec)
		{
			hash_destroy(json_hash);
			PG_RETURN_POINTER(rec);
		}
	}
	else
	{
		jb = PG_GETARG_JSONB(json_arg_num);

		/* same logic as for json */
		if (!have_record_arg && rec)
			PG_RETURN_POINTER(rec);
	}

	ncolumns = tupdesc->natts;

	if (rec)
	{
		/* Build a temporary HeapTuple control structure */
		tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
		ItemPointerSetInvalid(&(tuple.t_self));
		tuple.t_tableOid = InvalidOid;
		tuple.t_data = rec;
	}

	/*
	 * We arrange to look up the needed I/O info just once per series of
	 * calls, assuming the record type doesn't change underneath us.
	 */
	my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns != ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   sizeof(RecordIOData) - sizeof(ColumnIOData)
							   + ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = 0;
		my_extra->ncolumns = ncolumns;
		MemSet(my_extra->columns, 0, sizeof(ColumnIOData) * ncolumns);
	}

	if (have_record_arg && (my_extra->record_type != tupType ||
							my_extra->record_typmod != tupTypmod))
	{
		MemSet(my_extra, 0,
			   sizeof(RecordIOData) - sizeof(ColumnIOData)
			   + ncolumns * sizeof(ColumnIOData));
		my_extra->record_type = tupType;
		my_extra->record_typmod = tupTypmod;
		my_extra->ncolumns = ncolumns;
	}

	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));

	if (rec)
	{
		/* Break down the tuple into fields */
		heap_deform_tuple(&tuple, tupdesc, values, nulls);
	}
	else
	{
		for (i = 0; i < ncolumns; ++i)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
		}
	}

	for (i = 0; i < ncolumns; ++i)
	{
		ColumnIOData *column_info = &my_extra->columns[i];
		Oid			column_type = tupdesc->attrs[i]->atttypid;
		JsonbValue *v = NULL;
		JsonHashEntry *hashentry = NULL;

		/* Ignore dropped columns in datatype */
		if (tupdesc->attrs[i]->attisdropped)
		{
			nulls[i] = true;
			continue;
		}

		if (jtype == JSONOID)
		{
			hashentry = hash_search(json_hash,
									NameStr(tupdesc->attrs[i]->attname),
									HASH_FIND, NULL);
		}
		else
		{
			char	   *key = NameStr(tupdesc->attrs[i]->attname);

			v = findJsonbValueFromContainerLen(&jb->root, JB_FOBJECT, key,
											   strlen(key));
		}

		/*
		 * we can't just skip here if the key wasn't found since we might have
		 * a domain to deal with. If we were passed in a non-null record
		 * datum, we assume that the existing values are valid (if they're
		 * not, then it's not our fault), but if we were passed in a null,
		 * then every field which we don't populate needs to be run through
		 * the input function just in case it's a domain type.
		 */
		if (((jtype == JSONOID && hashentry == NULL) ||
			 (jtype == JSONBOID && v == NULL)) && rec)
			continue;

		/*
		 * Prepare to convert the column value from text
		 */
		if (column_info->column_type != column_type)
		{
			getTypeInputInfo(column_type,
							 &column_info->typiofunc,
							 &column_info->typioparam);
			fmgr_info_cxt(column_info->typiofunc, &column_info->proc,
						  fcinfo->flinfo->fn_mcxt);
			column_info->column_type = column_type;
		}
		if ((jtype == JSONOID && (hashentry == NULL || hashentry->isnull)) ||
			(jtype == JSONBOID && (v == NULL || v->type == jbvNull)))
		{
			/*
			 * need InputFunctionCall to happen even for nulls, so that domain
			 * checks are done
			 */
			values[i] = InputFunctionCall(&column_info->proc, NULL,
										  column_info->typioparam,
										  tupdesc->attrs[i]->atttypmod);
			nulls[i] = true;
		}
		else
		{
			char	   *s = NULL;

			if (jtype == JSONOID)
			{
				/* already done the hard work in the json case */
				s = hashentry->val;
			}
			else
			{
				if (v->type == jbvString)
					s = pnstrdup(v->val.string.val, v->val.string.len);
				else if (v->type == jbvBool)
					s = pnstrdup((v->val.boolean) ? "t" : "f", 1);
				else if (v->type == jbvNumeric)
					s = DatumGetCString(DirectFunctionCall1(numeric_out,
										   PointerGetDatum(v->val.numeric)));
				else if (v->type == jbvBinary)
					s = JsonbToCString(NULL, (JsonbContainer *) v->val.binary.data, v->val.binary.len);
				else
					elog(ERROR, "unrecognized jsonb type: %d", (int) v->type);
			}

			values[i] = InputFunctionCall(&column_info->proc, s,
										  column_info->typioparam,
										  tupdesc->attrs[i]->atttypmod);
			nulls[i] = false;
		}
	}

	rettuple = heap_form_tuple(tupdesc, values, nulls);

	ReleaseTupleDesc(tupdesc);

	if (json_hash)
		hash_destroy(json_hash);

	PG_RETURN_DATUM(HeapTupleGetDatum(rettuple));
}

/*
 * get_json_object_as_hash
 *
 * decompose a json object into a hash table.
 */
static HTAB *
get_json_object_as_hash(text *json, const char *funcname)
{
	HASHCTL		ctl;
	HTAB	   *tab;
	JHashState *state;
	JsonLexContext *lex = makeJsonLexContext(json, true);
	JsonSemAction *sem;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = NAMEDATALEN;
	ctl.entrysize = sizeof(JsonHashEntry);
	ctl.hcxt = CurrentMemoryContext;
	tab = hash_create("json object hashtable",
					  100,
					  &ctl,
					  HASH_ELEM | HASH_CONTEXT);

	state = palloc0(sizeof(JHashState));
	sem = palloc0(sizeof(JsonSemAction));

	state->function_name = funcname;
	state->hash = tab;
	state->lex = lex;

	sem->semstate = (void *) state;
	sem->array_start = hash_array_start;
	sem->scalar = hash_scalar;
	sem->object_field_start = hash_object_field_start;
	sem->object_field_end = hash_object_field_end;

	pg_parse_json(lex, sem);

	return tab;
}

static void
hash_object_field_start(void *state, char *fname, bool isnull)
{
	JHashState *_state = (JHashState *) state;

	if (_state->lex->lex_level > 1)
		return;

	if (_state->lex->token_type == JSON_TOKEN_ARRAY_START ||
		_state->lex->token_type == JSON_TOKEN_OBJECT_START)
	{
		/* remember start position of the whole text of the subobject */
		_state->save_json_start = _state->lex->token_start;
	}
	else
	{
		/* must be a scalar */
		_state->save_json_start = NULL;
	}
}

static void
hash_object_field_end(void *state, char *fname, bool isnull)
{
	JHashState *_state = (JHashState *) state;
	JsonHashEntry *hashentry;
	bool		found;

	/*
	 * Ignore nested fields.
	 */
	if (_state->lex->lex_level > 2)
		return;

	/*
	 * Ignore field names >= NAMEDATALEN - they can't match a record field.
	 * (Note: without this test, the hash code would truncate the string at
	 * NAMEDATALEN-1, and could then match against a similarly-truncated
	 * record field name.  That would be a reasonable behavior, but this code
	 * has previously insisted on exact equality, so we keep this behavior.)
	 */
	if (strlen(fname) >= NAMEDATALEN)
		return;

	hashentry = hash_search(_state->hash, fname, HASH_ENTER, &found);

	/*
	 * found being true indicates a duplicate. We don't do anything about
	 * that, a later field with the same name overrides the earlier field.
	 */

	hashentry->isnull = isnull;
	if (_state->save_json_start != NULL)
	{
		int			len = _state->lex->prev_token_terminator - _state->save_json_start;
		char	   *val = palloc((len + 1) * sizeof(char));

		memcpy(val, _state->save_json_start, len);
		val[len] = '\0';
		hashentry->val = val;
	}
	else
	{
		/* must have had a scalar instead */
		hashentry->val = _state->saved_scalar;
	}
}

static void
hash_array_start(void *state)
{
	JHashState *_state = (JHashState *) state;

	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			   errmsg("cannot call %s on an array", _state->function_name)));
}

static void
hash_scalar(void *state, char *token, JsonTokenType tokentype)
{
	JHashState *_state = (JHashState *) state;

	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			   errmsg("cannot call %s on a scalar", _state->function_name)));

	if (_state->lex->lex_level == 1)
		_state->saved_scalar = token;
}


/*
 * SQL function json_populate_recordset
 *
 * set fields in a set of records from the argument json,
 * which must be an array of objects.
 *
 * similar to json_populate_record, but the tuple-building code
 * is pushed down into the semantic action handlers so it's done
 * per object in the array.
 */
Datum
jsonb_populate_recordset(PG_FUNCTION_ARGS)
{
	return populate_recordset_worker(fcinfo, "jsonb_populate_recordset", true);
}

Datum
jsonb_to_recordset(PG_FUNCTION_ARGS)
{
	return populate_recordset_worker(fcinfo, "jsonb_to_recordset", false);
}

Datum
json_populate_recordset(PG_FUNCTION_ARGS)
{
	return populate_recordset_worker(fcinfo, "json_populate_recordset", true);
}

Datum
json_to_recordset(PG_FUNCTION_ARGS)
{
	return populate_recordset_worker(fcinfo, "json_to_recordset", false);
}

static void
make_row_from_rec_and_jsonb(Jsonb *element, PopulateRecordsetState *state)
{
	Datum	   *values;
	bool	   *nulls;
	int			i;
	RecordIOData *my_extra = state->my_extra;
	int			ncolumns = my_extra->ncolumns;
	TupleDesc	tupdesc = state->ret_tdesc;
	HeapTupleHeader rec = state->rec;
	HeapTuple	rettuple;

	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));

	if (state->rec)
	{
		HeapTupleData tuple;

		/* Build a temporary HeapTuple control structure */
		tuple.t_len = HeapTupleHeaderGetDatumLength(state->rec);
		ItemPointerSetInvalid(&(tuple.t_self));
		tuple.t_tableOid = InvalidOid;
		tuple.t_data = state->rec;

		/* Break down the tuple into fields */
		heap_deform_tuple(&tuple, tupdesc, values, nulls);
	}
	else
	{
		for (i = 0; i < ncolumns; ++i)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
		}
	}

	for (i = 0; i < ncolumns; ++i)
	{
		ColumnIOData *column_info = &my_extra->columns[i];
		Oid			column_type = tupdesc->attrs[i]->atttypid;
		JsonbValue *v = NULL;
		char	   *key;

		/* Ignore dropped columns in datatype */
		if (tupdesc->attrs[i]->attisdropped)
		{
			nulls[i] = true;
			continue;
		}

		key = NameStr(tupdesc->attrs[i]->attname);

		v = findJsonbValueFromContainerLen(&element->root, JB_FOBJECT,
										   key, strlen(key));

		/*
		 * We can't just skip here if the key wasn't found since we might have
		 * a domain to deal with. If we were passed in a non-null record
		 * datum, we assume that the existing values are valid (if they're
		 * not, then it's not our fault), but if we were passed in a null,
		 * then every field which we don't populate needs to be run through
		 * the input function just in case it's a domain type.
		 */
		if (v == NULL && rec)
			continue;

		/*
		 * Prepare to convert the column value from text
		 */
		if (column_info->column_type != column_type)
		{
			getTypeInputInfo(column_type,
							 &column_info->typiofunc,
							 &column_info->typioparam);
			fmgr_info_cxt(column_info->typiofunc, &column_info->proc,
						  state->fn_mcxt);
			column_info->column_type = column_type;
		}
		if (v == NULL || v->type == jbvNull)
		{
			/*
			 * Need InputFunctionCall to happen even for nulls, so that domain
			 * checks are done
			 */
			values[i] = InputFunctionCall(&column_info->proc, NULL,
										  column_info->typioparam,
										  tupdesc->attrs[i]->atttypmod);
			nulls[i] = true;
		}
		else
		{
			char	   *s = NULL;

			if (v->type == jbvString)
				s = pnstrdup(v->val.string.val, v->val.string.len);
			else if (v->type == jbvBool)
				s = pnstrdup((v->val.boolean) ? "t" : "f", 1);
			else if (v->type == jbvNumeric)
				s = DatumGetCString(DirectFunctionCall1(numeric_out,
										   PointerGetDatum(v->val.numeric)));
			else if (v->type == jbvBinary)
				s = JsonbToCString(NULL, (JsonbContainer *) v->val.binary.data, v->val.binary.len);
			else
				elog(ERROR, "unrecognized jsonb type: %d", (int) v->type);

			values[i] = InputFunctionCall(&column_info->proc, s,
										  column_info->typioparam,
										  tupdesc->attrs[i]->atttypmod);
			nulls[i] = false;
		}
	}

	rettuple = heap_form_tuple(tupdesc, values, nulls);

	tuplestore_puttuple(state->tuple_store, rettuple);
}

/*
 * common worker for json_populate_recordset() and json_to_recordset()
 */
static Datum
populate_recordset_worker(FunctionCallInfo fcinfo, const char *funcname,
						  bool have_record_arg)
{
	int			json_arg_num = have_record_arg ? 1 : 0;
	Oid			jtype = get_fn_expr_argtype(fcinfo->flinfo, json_arg_num);
	ReturnSetInfo *rsi;
	MemoryContext old_cxt;
	Oid			tupType;
	int32		tupTypmod;
	HeapTupleHeader rec;
	TupleDesc	tupdesc;
	RecordIOData *my_extra;
	int			ncolumns;
	PopulateRecordsetState *state;

	if (have_record_arg)
	{
		Oid			argtype = get_fn_expr_argtype(fcinfo->flinfo, 0);

		if (!type_is_rowtype(argtype))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("first argument of %s must be a row type",
							funcname)));
	}

	rsi = (ReturnSetInfo *) fcinfo->resultinfo;

	if (!rsi || !IsA(rsi, ReturnSetInfo) ||
		(rsi->allowedModes & SFRM_Materialize) == 0 ||
		rsi->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that "
						"cannot accept a set")));

	rsi->returnMode = SFRM_Materialize;

	/*
	 * get the tupdesc from the result set info - it must be a record type
	 * because we already checked that arg1 is a record type, or we're in a
	 * to_record function which returns a setof record.
	 */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record")));

	/* if the json is null send back an empty set */
	if (PG_ARGISNULL(json_arg_num))
		PG_RETURN_NULL();

	if (!have_record_arg || PG_ARGISNULL(0))
		rec = NULL;
	else
		rec = PG_GETARG_HEAPTUPLEHEADER(0);

	tupType = tupdesc->tdtypeid;
	tupTypmod = tupdesc->tdtypmod;
	ncolumns = tupdesc->natts;

	/*
	 * We arrange to look up the needed I/O info just once per series of
	 * calls, assuming the record type doesn't change underneath us.
	 */
	my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns != ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   sizeof(RecordIOData) - sizeof(ColumnIOData)
							   + ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = 0;
	}

	if (my_extra->record_type != tupType ||
		my_extra->record_typmod != tupTypmod)
	{
		MemSet(my_extra, 0,
			   sizeof(RecordIOData) - sizeof(ColumnIOData)
			   + ncolumns * sizeof(ColumnIOData));
		my_extra->record_type = tupType;
		my_extra->record_typmod = tupTypmod;
		my_extra->ncolumns = ncolumns;
	}

	state = palloc0(sizeof(PopulateRecordsetState));

	/* make these in a sufficiently long-lived memory context */
	old_cxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);
	state->ret_tdesc = CreateTupleDescCopy(tupdesc);;
	BlessTupleDesc(state->ret_tdesc);
	state->tuple_store = tuplestore_begin_heap(rsi->allowedModes &
											   SFRM_Materialize_Random,
											   false, work_mem);
	MemoryContextSwitchTo(old_cxt);

	state->function_name = funcname;
	state->my_extra = my_extra;
	state->rec = rec;
	state->fn_mcxt = fcinfo->flinfo->fn_mcxt;

	if (jtype == JSONOID)
	{
		text	   *json = PG_GETARG_TEXT_P(json_arg_num);
		JsonLexContext *lex;
		JsonSemAction *sem;

		sem = palloc0(sizeof(JsonSemAction));

		lex = makeJsonLexContext(json, true);

		sem->semstate = (void *) state;
		sem->array_start = populate_recordset_array_start;
		sem->array_element_start = populate_recordset_array_element_start;
		sem->scalar = populate_recordset_scalar;
		sem->object_field_start = populate_recordset_object_field_start;
		sem->object_field_end = populate_recordset_object_field_end;
		sem->object_start = populate_recordset_object_start;
		sem->object_end = populate_recordset_object_end;

		state->lex = lex;

		pg_parse_json(lex, sem);
	}
	else
	{
		Jsonb	   *jb = PG_GETARG_JSONB(json_arg_num);
		JsonbIterator *it;
		JsonbValue	v;
		bool		skipNested = false;
		int			r;

		Assert(jtype == JSONBOID);

		if (JB_ROOT_IS_SCALAR(jb) || !JB_ROOT_IS_ARRAY(jb))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot call %s on a non-array",
							funcname)));

		it = JsonbIteratorInit(&jb->root);

		while ((r = JsonbIteratorNext(&it, &v, skipNested)) != WJB_DONE)
		{
			skipNested = true;

			if (r == WJB_ELEM)
			{
				Jsonb	   *element = JsonbValueToJsonb(&v);

				if (!JB_ROOT_IS_OBJECT(element))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("argument of %s must be an array of objects",
								funcname)));
				make_row_from_rec_and_jsonb(element, state);
			}
		}
	}

	rsi->setResult = state->tuple_store;
	rsi->setDesc = state->ret_tdesc;

	PG_RETURN_NULL();
}

static void
populate_recordset_object_start(void *state)
{
	PopulateRecordsetState *_state = (PopulateRecordsetState *) state;
	int			lex_level = _state->lex->lex_level;
	HASHCTL		ctl;

	/* Reject object at top level: we must have an array at level 0 */
	if (lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on an object",
						_state->function_name)));

	/* Nested objects require no special processing */
	if (lex_level > 1)
		return;

	/* Object at level 1: set up a new hash table for this object */
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = NAMEDATALEN;
	ctl.entrysize = sizeof(JsonHashEntry);
	ctl.hcxt = CurrentMemoryContext;
	_state->json_hash = hash_create("json object hashtable",
									100,
									&ctl,
									HASH_ELEM | HASH_CONTEXT);
}

static void
populate_recordset_object_end(void *state)
{
	PopulateRecordsetState *_state = (PopulateRecordsetState *) state;
	HTAB	   *json_hash = _state->json_hash;
	Datum	   *values;
	bool	   *nulls;
	int			i;
	RecordIOData *my_extra = _state->my_extra;
	int			ncolumns = my_extra->ncolumns;
	TupleDesc	tupdesc = _state->ret_tdesc;
	JsonHashEntry *hashentry;
	HeapTupleHeader rec = _state->rec;
	HeapTuple	rettuple;

	/* Nested objects require no special processing */
	if (_state->lex->lex_level > 1)
		return;

	/* Otherwise, construct and return a tuple based on this level-1 object */
	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));

	if (_state->rec)
	{
		HeapTupleData tuple;

		/* Build a temporary HeapTuple control structure */
		tuple.t_len = HeapTupleHeaderGetDatumLength(_state->rec);
		ItemPointerSetInvalid(&(tuple.t_self));
		tuple.t_tableOid = InvalidOid;
		tuple.t_data = _state->rec;

		/* Break down the tuple into fields */
		heap_deform_tuple(&tuple, tupdesc, values, nulls);
	}
	else
	{
		for (i = 0; i < ncolumns; ++i)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
		}
	}

	for (i = 0; i < ncolumns; ++i)
	{
		ColumnIOData *column_info = &my_extra->columns[i];
		Oid			column_type = tupdesc->attrs[i]->atttypid;
		char	   *value;

		/* Ignore dropped columns in datatype */
		if (tupdesc->attrs[i]->attisdropped)
		{
			nulls[i] = true;
			continue;
		}

		hashentry = hash_search(json_hash,
								NameStr(tupdesc->attrs[i]->attname),
								HASH_FIND, NULL);

		/*
		 * we can't just skip here if the key wasn't found since we might have
		 * a domain to deal with. If we were passed in a non-null record
		 * datum, we assume that the existing values are valid (if they're
		 * not, then it's not our fault), but if we were passed in a null,
		 * then every field which we don't populate needs to be run through
		 * the input function just in case it's a domain type.
		 */
		if (hashentry == NULL && rec)
			continue;

		/*
		 * Prepare to convert the column value from text
		 */
		if (column_info->column_type != column_type)
		{
			getTypeInputInfo(column_type,
							 &column_info->typiofunc,
							 &column_info->typioparam);
			fmgr_info_cxt(column_info->typiofunc, &column_info->proc,
						  _state->fn_mcxt);
			column_info->column_type = column_type;
		}
		if (hashentry == NULL || hashentry->isnull)
		{
			/*
			 * need InputFunctionCall to happen even for nulls, so that domain
			 * checks are done
			 */
			values[i] = InputFunctionCall(&column_info->proc, NULL,
										  column_info->typioparam,
										  tupdesc->attrs[i]->atttypmod);
			nulls[i] = true;
		}
		else
		{
			value = hashentry->val;

			values[i] = InputFunctionCall(&column_info->proc, value,
										  column_info->typioparam,
										  tupdesc->attrs[i]->atttypmod);
			nulls[i] = false;
		}
	}

	rettuple = heap_form_tuple(tupdesc, values, nulls);

	tuplestore_puttuple(_state->tuple_store, rettuple);

	/* Done with hash for this object */
	hash_destroy(json_hash);
	_state->json_hash = NULL;
}

static void
populate_recordset_array_element_start(void *state, bool isnull)
{
	PopulateRecordsetState *_state = (PopulateRecordsetState *) state;

	if (_state->lex->lex_level == 1 &&
		_state->lex->token_type != JSON_TOKEN_OBJECT_START)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument of %s must be an array of objects",
						_state->function_name)));
}

static void
populate_recordset_array_start(void *state)
{
	/* nothing to do */
}

static void
populate_recordset_scalar(void *state, char *token, JsonTokenType tokentype)
{
	PopulateRecordsetState *_state = (PopulateRecordsetState *) state;

	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call %s on a scalar",
						_state->function_name)));

	if (_state->lex->lex_level == 2)
		_state->saved_scalar = token;
}

static void
populate_recordset_object_field_start(void *state, char *fname, bool isnull)
{
	PopulateRecordsetState *_state = (PopulateRecordsetState *) state;

	if (_state->lex->lex_level > 2)
		return;

	if (_state->lex->token_type == JSON_TOKEN_ARRAY_START ||
		_state->lex->token_type == JSON_TOKEN_OBJECT_START)
	{
		_state->save_json_start = _state->lex->token_start;
	}
	else
	{
		_state->save_json_start = NULL;
	}
}

static void
populate_recordset_object_field_end(void *state, char *fname, bool isnull)
{
	PopulateRecordsetState *_state = (PopulateRecordsetState *) state;
	JsonHashEntry *hashentry;
	bool		found;

	/*
	 * Ignore nested fields.
	 */
	if (_state->lex->lex_level > 2)
		return;

	/*
	 * Ignore field names >= NAMEDATALEN - they can't match a record field.
	 * (Note: without this test, the hash code would truncate the string at
	 * NAMEDATALEN-1, and could then match against a similarly-truncated
	 * record field name.  That would be a reasonable behavior, but this code
	 * has previously insisted on exact equality, so we keep this behavior.)
	 */
	if (strlen(fname) >= NAMEDATALEN)
		return;

	hashentry = hash_search(_state->json_hash, fname, HASH_ENTER, &found);

	/*
	 * found being true indicates a duplicate. We don't do anything about
	 * that, a later field with the same name overrides the earlier field.
	 */

	hashentry->isnull = isnull;
	if (_state->save_json_start != NULL)
	{
		int			len = _state->lex->prev_token_terminator - _state->save_json_start;
		char	   *val = palloc((len + 1) * sizeof(char));

		memcpy(val, _state->save_json_start, len);
		val[len] = '\0';
		hashentry->val = val;
	}
	else
	{
		/* must have had a scalar instead */
		hashentry->val = _state->saved_scalar;
	}
}

/*
 * findJsonbValueFromContainer() wrapper that sets up JsonbValue key string.
 */
static JsonbValue *
findJsonbValueFromContainerLen(JsonbContainer *container, uint32 flags,
							   char *key, uint32 keylen)
{
	JsonbValue	k;

	k.type = jbvString;
	k.val.string.val = key;
	k.val.string.len = keylen;

	return findJsonbValueFromContainer(container, flags, &k);
}
