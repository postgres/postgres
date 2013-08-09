/*-------------------------------------------------------------------------
 *
 * jsonfuncs.c
 *		Functions to process JSON data type.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/json.h"
#include "utils/jsonapi.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"

/* semantic action functions for json_object_keys */
static void okeys_object_field_start(void *state, char *fname, bool isnull);
static void okeys_array_start(void *state);
static void okeys_scalar(void *state, char *token, JsonTokenType tokentype);

/* semantic action functions for json_get* functions */
static void get_object_start(void *state);
static void get_object_field_start(void *state, char *fname, bool isnull);
static void get_object_field_end(void *state, char *fname, bool isnull);
static void get_array_start(void *state);
static void get_array_element_start(void *state, bool isnull);
static void get_array_element_end(void *state, bool isnull);
static void get_scalar(void *state, char *token, JsonTokenType tokentype);

/* common worker function for json getter functions */
static inline Datum get_path_all(PG_FUNCTION_ARGS, bool as_text);
static inline text *get_worker(text *json, char *field, int elem_index,
		   char **tpath, int *ipath, int npath,
		   bool normalize_results);

/* semantic action functions for json_array_length */
static void alen_object_start(void *state);
static void alen_scalar(void *state, char *token, JsonTokenType tokentype);
static void alen_array_element_start(void *state, bool isnull);

/* common worker for json_each* functions */
static inline Datum each_worker(PG_FUNCTION_ARGS, bool as_text);

/* semantic action functions for json_each */
static void each_object_field_start(void *state, char *fname, bool isnull);
static void each_object_field_end(void *state, char *fname, bool isnull);
static void each_array_start(void *state);
static void each_scalar(void *state, char *token, JsonTokenType tokentype);

/* semantic action functions for json_array_elements */
static void elements_object_start(void *state);
static void elements_array_element_start(void *state, bool isnull);
static void elements_array_element_end(void *state, bool isnull);
static void elements_scalar(void *state, char *token, JsonTokenType tokentype);

/* turn a json object into a hash table */
static HTAB *get_json_object_as_hash(text *json, char *funcname, bool use_json_as_text);

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

/* search type classification for json_get* functions */
typedef enum
{
	JSON_SEARCH_OBJECT = 1,
	JSON_SEARCH_ARRAY,
	JSON_SEARCH_PATH
} JsonSearch;

/* state for json_object_keys */
typedef struct OkeysState
{
	JsonLexContext *lex;
	char	  **result;
	int			result_size;
	int			result_count;
	int			sent_count;
}	OkeysState;

/* state for json_get* functions */
typedef struct GetState
{
	JsonLexContext *lex;
	JsonSearch	search_type;
	int			search_index;
	int			array_index;
	char	   *search_term;
	char	   *result_start;
	text	   *tresult;
	bool		result_is_null;
	bool		normalize_results;
	bool		next_scalar;
	char	  **path;
	int			npath;
	char	  **current_path;
	bool	   *pathok;
	int		   *array_level_index;
	int		   *path_level_index;
}	GetState;

/* state for json_array_length */
typedef struct AlenState
{
	JsonLexContext *lex;
	int			count;
}	AlenState;

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
}	EachState;

/* state for json_array_elements */
typedef struct ElementsState
{
	JsonLexContext *lex;
	Tuplestorestate *tuple_store;
	TupleDesc	ret_tdesc;
	MemoryContext tmp_cxt;
	char	   *result_start;
}	ElementsState;

/* state for get_json_object_as_hash */
typedef struct JhashState
{
	JsonLexContext *lex;
	HTAB	   *hash;
	char	   *saved_scalar;
	char	   *save_json_start;
	bool		use_json_as_text;
	char	   *function_name;
}	JHashState;

/* used to build the hashtable */
typedef struct JsonHashEntry
{
	char		fname[NAMEDATALEN];
	char	   *val;
	char	   *json;
	bool		isnull;
}	JsonHashEntry;

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
	HTAB	   *json_hash;
	char	   *saved_scalar;
	char	   *save_json_start;
	bool		use_json_as_text;
	Tuplestorestate *tuple_store;
	TupleDesc	ret_tdesc;
	HeapTupleHeader rec;
	RecordIOData *my_extra;
	MemoryContext fn_mcxt;		/* used to stash IO funcs */
}	PopulateRecordsetState;

/*
 * SQL function json_object-keys
 *
 * Returns the set of keys for the object argument.
 *
 * This SRF operates in value-per-call mode. It processes the
 * object during the first call, and the keys are simply stashed
 * in an array, whise size is expanded as necessary. This is probably
 * safe enough for a list of keys of a single object, since they are
 * limited in size to NAMEDATALEN and the number of keys is unlikely to
 * be so huge that it has major memory implications.
 */


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
		_state->result =
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
				 errmsg("cannot call json_object_keys on an array")));
}

static void
okeys_scalar(void *state, char *token, JsonTokenType tokentype)
{
	OkeysState *_state = (OkeysState *) state;

	/* top level must be a json object */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call json_object_keys on a scalar")));
}

/*
 * json getter functions
 * these implement the -> ->> #> and #>> operators
 * and the json_extract_path*(json, text, ...) functions
 */


Datum
json_object_field(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_P(0);
	text	   *result;
	text	   *fname = PG_GETARG_TEXT_P(1);
	char	   *fnamestr = text_to_cstring(fname);

	result = get_worker(json, fnamestr, -1, NULL, NULL, -1, false);

	if (result != NULL)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

Datum
json_object_field_text(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_P(0);
	text	   *result;
	text	   *fname = PG_GETARG_TEXT_P(1);
	char	   *fnamestr = text_to_cstring(fname);

	result = get_worker(json, fnamestr, -1, NULL, NULL, -1, true);

	if (result != NULL)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

Datum
json_array_element(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_P(0);
	text	   *result;
	int			element = PG_GETARG_INT32(1);

	result = get_worker(json, NULL, element, NULL, NULL, -1, false);

	if (result != NULL)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

Datum
json_array_element_text(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_P(0);
	text	   *result;
	int			element = PG_GETARG_INT32(1);

	result = get_worker(json, NULL, element, NULL, NULL, -1, true);

	if (result != NULL)
		PG_RETURN_TEXT_P(result);
	else
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
static inline Datum
get_path_all(PG_FUNCTION_ARGS, bool as_text)
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
	long		ind;
	char	   *endptr;

	if (array_contains_nulls(path))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call function with null path elements")));


	deconstruct_array(path, TEXTOID, -1, false, 'i',
					  &pathtext, &pathnulls, &npath);

	tpath = palloc(npath * sizeof(char *));
	ipath = palloc(npath * sizeof(int));


	for (i = 0; i < npath; i++)
	{
		tpath[i] = TextDatumGetCString(pathtext[i]);
		if (*tpath[i] == '\0')
			ereport(
					ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				   errmsg("cannot call function with empty path elements")));

		/*
		 * we have no idea at this stage what structure the document is so
		 * just convert anything in the path that we can to an integer and set
		 * all the other integers to -1 which will never match.
		 */
		ind = strtol(tpath[i], &endptr, 10);
		if (*endptr == '\0' && ind <= INT_MAX && ind >= 0)
			ipath[i] = (int) ind;
		else
			ipath[i] = -1;
	}


	result = get_worker(json, NULL, -1, tpath, ipath, npath, as_text);

	if (result != NULL)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

/*
 * get_worker
 *
 * common worker for all the json getter functions
 */
static inline text *
get_worker(text *json,
		   char *field,
		   int elem_index,
		   char **tpath,
		   int *ipath,
		   int npath,
		   bool normalize_results)
{
	GetState   *state;
	JsonLexContext *lex = makeJsonLexContext(json, true);
	JsonSemAction *sem;

	/* only allowed to use one of these */
	Assert(elem_index < 0 || (tpath == NULL && ipath == NULL && field == NULL));
	Assert(tpath == NULL || field == NULL);

	state = palloc0(sizeof(GetState));
	sem = palloc0(sizeof(JsonSemAction));

	state->lex = lex;
	/* is it "_as_text" variant? */
	state->normalize_results = normalize_results;
	if (field != NULL)
	{
		/* single text argument */
		state->search_type = JSON_SEARCH_OBJECT;
		state->search_term = field;
	}
	else if (tpath != NULL)
	{
		/* path array argument */
		state->search_type = JSON_SEARCH_PATH;
		state->path = tpath;
		state->npath = npath;
		state->current_path = palloc(sizeof(char *) * npath);
		state->pathok = palloc0(sizeof(bool) * npath);
		state->pathok[0] = true;
		state->array_level_index = palloc(sizeof(int) * npath);
		state->path_level_index = ipath;

	}
	else
	{
		/* single integer argument */
		state->search_type = JSON_SEARCH_ARRAY;
		state->search_index = elem_index;
		state->array_index = -1;
	}

	sem->semstate = (void *) state;

	/*
	 * Not all	variants need all the semantic routines. only set the ones
	 * that are actually needed for maximum efficiency.
	 */
	sem->object_start = get_object_start;
	sem->array_start = get_array_start;
	sem->scalar = get_scalar;
	if (field != NULL || tpath != NULL)
	{
		sem->object_field_start = get_object_field_start;
		sem->object_field_end = get_object_field_end;
	}
	if (field == NULL)
	{
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

	/* json structure check */
	if (_state->lex->lex_level == 0 && _state->search_type == JSON_SEARCH_ARRAY)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot extract array element from a non-array")));
}

static void
get_object_field_start(void *state, char *fname, bool isnull)
{
	GetState   *_state = (GetState *) state;
	bool		get_next = false;
	int			lex_level = _state->lex->lex_level;

	if (lex_level == 1 && _state->search_type == JSON_SEARCH_OBJECT &&
		strcmp(fname, _state->search_term) == 0)
	{

		_state->tresult = NULL;
		_state->result_start = NULL;
		get_next = true;
	}
	else if (_state->search_type == JSON_SEARCH_PATH &&
			 lex_level <= _state->npath &&
			 _state->pathok[_state->lex->lex_level - 1] &&
			 strcmp(fname, _state->path[lex_level - 1]) == 0)
	{
		/* path search, path so far is ok,	and we have a match */

		/* this object overrides any previous matching object */

		_state->tresult = NULL;
		_state->result_start = NULL;

		/* if not at end of path just mark path ok */
		if (lex_level < _state->npath)
			_state->pathok[lex_level] = true;

		/* end of path, so we want this value */
		if (lex_level == _state->npath)
			get_next = true;
	}

	if (get_next)
	{
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


	/* same tests as in get_object_field_start, mutatis mutandis */
	if (lex_level == 1 && _state->search_type == JSON_SEARCH_OBJECT &&
		strcmp(fname, _state->search_term) == 0)
	{
		get_last = true;
	}
	else if (_state->search_type == JSON_SEARCH_PATH &&
			 lex_level <= _state->npath &&
			 _state->pathok[lex_level - 1] &&
			 strcmp(fname, _state->path[lex_level - 1]) == 0)
	{
		/* done with this field so reset pathok */
		if (lex_level < _state->npath)
			_state->pathok[lex_level] = false;

		if (lex_level == _state->npath)
			get_last = true;
	}

	/* for as_test variants our work is already done */
	if (get_last && _state->result_start != NULL)
	{
		/*
		 * make a text object from the string from the prevously noted json
		 * start up to the end of the previous token (the lexer is by now
		 * ahead of us on whatevere came after what we're interested in).
		 */
		int			len = _state->lex->prev_token_terminator - _state->result_start;

		if (isnull && _state->normalize_results)
			_state->tresult = (text *) NULL;
		else
			_state->tresult = cstring_to_text_with_len(_state->result_start, len);
	}

	/*
	 * don't need to reset _state->result_start b/c we're only returning one
	 * datum, the conditions should not occur more than once, and this lets us
	 * check cheaply that they don't (see object_field_start() )
	 */
}

static void
get_array_start(void *state)
{
	GetState   *_state = (GetState *) state;
	int			lex_level = _state->lex->lex_level;

	/* json structure check */
	if (lex_level == 0 && _state->search_type == JSON_SEARCH_OBJECT)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot extract field from a non-object")));

	/*
	 * initialize array count for this nesting level Note: the lex_level seen
	 * by array_start is one less than that seen by the elements of the array.
	 */
	if (_state->search_type == JSON_SEARCH_PATH &&
		lex_level < _state->npath)
		_state->array_level_index[lex_level] = -1;
}

static void
get_array_element_start(void *state, bool isnull)
{
	GetState   *_state = (GetState *) state;
	bool		get_next = false;
	int			lex_level = _state->lex->lex_level;

	if (lex_level == 1 && _state->search_type == JSON_SEARCH_ARRAY)
	{
		/* single integer search */
		_state->array_index++;
		if (_state->array_index == _state->search_index)
			get_next = true;
	}
	else if (_state->search_type == JSON_SEARCH_PATH &&
			 lex_level <= _state->npath &&
			 _state->pathok[lex_level - 1])
	{
		/*
		 * path search, path so far is ok
		 *
		 * increment the array counter. no point doing this if we already know
		 * the path is bad.
		 *
		 * then check if we have a match.
		 */

		if (++_state->array_level_index[lex_level - 1] ==
			_state->path_level_index[lex_level - 1])
		{
			if (lex_level == _state->npath)
			{
				/* match and at end of path, so get value */
				get_next = true;
			}
			else
			{
				/* not at end of path just mark path ok */
				_state->pathok[lex_level] = true;
			}
		}

	}

	/* same logic as for objects */
	if (get_next)
	{
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

	/* same logic as in get_object_end, modified for arrays */

	if (lex_level == 1 && _state->search_type == JSON_SEARCH_ARRAY &&
		_state->array_index == _state->search_index)
	{
		get_last = true;
	}
	else if (_state->search_type == JSON_SEARCH_PATH &&
			 lex_level <= _state->npath &&
			 _state->pathok[lex_level - 1] &&
			 _state->array_level_index[lex_level - 1] ==
			 _state->path_level_index[lex_level - 1])
	{
		/* done with this element so reset pathok */
		if (lex_level < _state->npath)
			_state->pathok[lex_level] = false;

		if (lex_level == _state->npath)
			get_last = true;
	}
	if (get_last && _state->result_start != NULL)
	{
		int			len = _state->lex->prev_token_terminator - _state->result_start;

		if (isnull && _state->normalize_results)
			_state->tresult = (text *) NULL;
		else
			_state->tresult = cstring_to_text_with_len(_state->result_start, len);
	}
}

static void
get_scalar(void *state, char *token, JsonTokenType tokentype)
{
	GetState   *_state = (GetState *) state;

	if (_state->lex->lex_level == 0 && _state->search_type != JSON_SEARCH_PATH)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot extract element from a scalar")));
	if (_state->next_scalar)
	{
		/* a de-escaped text value is wanted, so supply it */
		_state->tresult = cstring_to_text(token);
		/* make sure the next call to get_scalar doesn't overwrite it */
		_state->next_scalar = false;
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
	JsonLexContext *lex = makeJsonLexContext(json, false);
	JsonSemAction *sem;

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

/*
 * These next two check ensure that the json is an array (since it can't be
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
json_each_text(PG_FUNCTION_ARGS)
{
	return each_worker(fcinfo, true);
}

static inline Datum
each_worker(PG_FUNCTION_ARGS, bool as_text)
{
	text	   *json = PG_GETARG_TEXT_P(0);
	JsonLexContext *lex = makeJsonLexContext(json, true);
	JsonSemAction *sem;
	ReturnSetInfo *rsi;
	MemoryContext old_cxt;
	TupleDesc	tupdesc;
	EachState  *state;

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
		values[1] = (Datum) NULL;
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
 * SQL function json_array_elements
 *
 * get the elements from a json array
 *
 * a lot of this processing is similar to the json_each* functions
 */
Datum
json_array_elements(PG_FUNCTION_ARGS)
{
	text	   *json = PG_GETARG_TEXT_P(0);

	/* elements doesn't need any escaped strings, so use false here */
	JsonLexContext *lex = makeJsonLexContext(json, false);
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

	state->lex = lex;
	state->tmp_cxt = AllocSetContextCreate(CurrentMemoryContext,
										 "json_array_elements temporary cxt",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);

	pg_parse_json(lex, sem);

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
		_state->result_start = _state->lex->token_start;
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
	static bool nulls[1] = {false};

	/* skip over nested objects */
	if (_state->lex->lex_level != 1)
		return;

	/* use the tmp context so we can clean up after each tuple is done */
	old_cxt = MemoryContextSwitchTo(_state->tmp_cxt);

	len = _state->lex->prev_token_terminator - _state->result_start;
	val = cstring_to_text_with_len(_state->result_start, len);

	values[0] = PointerGetDatum(val);

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
				 errmsg("cannot call json_array_elements on a non-array")));
}

static void
elements_scalar(void *state, char *token, JsonTokenType tokentype)
{
	ElementsState *_state = (ElementsState *) state;

	/* json structure check */
	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call json_array_elements on a scalar")));

	/*
	 * json_array_elements always returns json, so there's no need to think
	 * about de-escaped values here.
	 */
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
 * field in the record is then looked up by name.
 */
Datum
json_populate_record(PG_FUNCTION_ARGS)
{
	Oid			argtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	text	   *json;
	bool		use_json_as_text;
	HTAB	   *json_hash;
	HeapTupleHeader rec;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tuple;
	HeapTuple	rettuple;
	RecordIOData *my_extra;
	int			ncolumns;
	int			i;
	Datum	   *values;
	bool	   *nulls;
	char		fname[NAMEDATALEN];
	JsonHashEntry *hashentry;

	use_json_as_text = PG_ARGISNULL(2) ? false : PG_GETARG_BOOL(2);

	if (!type_is_rowtype(argtype))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("first argument of json_populate_record must be a row type")));

	if (PG_ARGISNULL(0))
	{
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();

		rec = NULL;

		/*
		 * have no tuple to look at, so the only source of type info is the
		 * argtype. The lookup_rowtype_tupdesc call below will error out if we
		 * don't have a known composite type oid here.
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

	json = PG_GETARG_TEXT_P(1);

	json_hash = get_json_object_as_hash(json, "json_populate_record", use_json_as_text);

	/*
	 * if the input json is empty, we can only skip the rest if we were passed
	 * in a non-null record, since otherwise there may be issues with domain
	 * nulls.
	 */
	if (hash_get_num_entries(json_hash) == 0 && rec)
		PG_RETURN_POINTER(rec);


	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
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
		char	   *value;

		/* Ignore dropped columns in datatype */
		if (tupdesc->attrs[i]->attisdropped)
		{
			nulls[i] = true;
			continue;
		}

		memset(fname, 0, NAMEDATALEN);
		strncpy(fname, NameStr(tupdesc->attrs[i]->attname), NAMEDATALEN);
		hashentry = hash_search(json_hash, fname, HASH_FIND, NULL);

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
						  fcinfo->flinfo->fn_mcxt);
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

	ReleaseTupleDesc(tupdesc);

	PG_RETURN_DATUM(HeapTupleGetDatum(rettuple));
}

/*
 * get_json_object_as_hash
 *
 * decompose a json object into a hash table.
 *
 * Currently doesn't allow anything but a flat object. Should this
 * change?
 *
 * funcname argument allows caller to pass in its name for use in
 * error messages.
 */
static HTAB *
get_json_object_as_hash(text *json, char *funcname, bool use_json_as_text)
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
	state->use_json_as_text = use_json_as_text;

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
		if (!_state->use_json_as_text)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot call %s on a nested object",
							_state->function_name)));
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
	char		name[NAMEDATALEN];

	/*
	 * ignore field names >= NAMEDATALEN - they can't match a record field
	 * ignore nested fields.
	 */
	if (_state->lex->lex_level > 2 || strlen(fname) >= NAMEDATALEN)
		return;

	memset(name, 0, NAMEDATALEN);
	strncpy(name, fname, NAMEDATALEN);

	hashentry = hash_search(_state->hash, name, HASH_ENTER, &found);

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
json_populate_recordset(PG_FUNCTION_ARGS)
{
	Oid			argtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	text	   *json;
	bool		use_json_as_text;
	ReturnSetInfo *rsi;
	MemoryContext old_cxt;
	Oid			tupType;
	int32		tupTypmod;
	HeapTupleHeader rec;
	TupleDesc	tupdesc;
	RecordIOData *my_extra;
	int			ncolumns;
	JsonLexContext *lex;
	JsonSemAction *sem;
	PopulateRecordsetState *state;

	use_json_as_text = PG_ARGISNULL(2) ? false : PG_GETARG_BOOL(2);

	if (!type_is_rowtype(argtype))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("first argument of json_populate_recordset must be a row type")));

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
	 * because we already checked that arg1 is a record type.
	 */
	(void) get_call_result_type(fcinfo, NULL, &tupdesc);

	state = palloc0(sizeof(PopulateRecordsetState));
	sem = palloc0(sizeof(JsonSemAction));


	/* make these in a sufficiently long-lived memory context */
	old_cxt = MemoryContextSwitchTo(rsi->econtext->ecxt_per_query_memory);

	state->ret_tdesc = CreateTupleDescCopy(tupdesc);
	BlessTupleDesc(state->ret_tdesc);
	state->tuple_store =
		tuplestore_begin_heap(rsi->allowedModes & SFRM_Materialize_Random,
							  false, work_mem);

	MemoryContextSwitchTo(old_cxt);

	/* if the json is null send back an empty set */
	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();

	json = PG_GETARG_TEXT_P(1);

	if (PG_ARGISNULL(0))
		rec = NULL;
	else
		rec = PG_GETARG_HEAPTUPLEHEADER(0);

	tupType = tupdesc->tdtypeid;
	tupTypmod = tupdesc->tdtypmod;
	ncolumns = tupdesc->natts;

	lex = makeJsonLexContext(json, true);

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

	sem->semstate = (void *) state;
	sem->array_start = populate_recordset_array_start;
	sem->array_element_start = populate_recordset_array_element_start;
	sem->scalar = populate_recordset_scalar;
	sem->object_field_start = populate_recordset_object_field_start;
	sem->object_field_end = populate_recordset_object_field_end;
	sem->object_start = populate_recordset_object_start;
	sem->object_end = populate_recordset_object_end;

	state->lex = lex;

	state->my_extra = my_extra;
	state->rec = rec;
	state->use_json_as_text = use_json_as_text;
	state->fn_mcxt = fcinfo->flinfo->fn_mcxt;

	pg_parse_json(lex, sem);

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

	if (lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call json_populate_recordset on an object")));
	else if (lex_level > 1 && !_state->use_json_as_text)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		 errmsg("cannot call json_populate_recordset with nested objects")));

	/* set up a new hash for this entry */
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
	char		fname[NAMEDATALEN];
	int			i;
	RecordIOData *my_extra = _state->my_extra;
	int			ncolumns = my_extra->ncolumns;
	TupleDesc	tupdesc = _state->ret_tdesc;
	JsonHashEntry *hashentry;
	HeapTupleHeader rec = _state->rec;
	HeapTuple	rettuple;

	if (_state->lex->lex_level > 1)
		return;

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

		memset(fname, 0, NAMEDATALEN);
		strncpy(fname, NameStr(tupdesc->attrs[i]->attname), NAMEDATALEN);
		hashentry = hash_search(json_hash, fname, HASH_FIND, NULL);

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

	hash_destroy(json_hash);
}

static void
populate_recordset_array_element_start(void *state, bool isnull)
{
	PopulateRecordsetState *_state = (PopulateRecordsetState *) state;

	if (_state->lex->lex_level == 1 &&
		_state->lex->token_type != JSON_TOKEN_OBJECT_START)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("must call json_populate_recordset on an array of objects")));
}

static void
populate_recordset_array_start(void *state)
{
	PopulateRecordsetState *_state = (PopulateRecordsetState *) state;

	if (_state->lex->lex_level != 0 && !_state->use_json_as_text)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		  errmsg("cannot call json_populate_recordset with nested arrays")));
}

static void
populate_recordset_scalar(void *state, char *token, JsonTokenType tokentype)
{
	PopulateRecordsetState *_state = (PopulateRecordsetState *) state;

	if (_state->lex->lex_level == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot call json_populate_recordset on a scalar")));

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
		if (!_state->use_json_as_text)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot call json_populate_recordset on a nested object")));
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
	char		name[NAMEDATALEN];

	/*
	 * ignore field names >= NAMEDATALEN - they can't match a record field
	 * ignore nested fields.
	 */
	if (_state->lex->lex_level > 2 || strlen(fname) >= NAMEDATALEN)
		return;

	memset(name, 0, NAMEDATALEN);
	strncpy(name, fname, NAMEDATALEN);

	hashentry = hash_search(_state->json_hash, name, HASH_ENTER, &found);

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
