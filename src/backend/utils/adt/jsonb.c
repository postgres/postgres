/*-------------------------------------------------------------------------
 *
 * jsonb.c
 *		I/O routines for jsonb type
 *
 * Copyright (c) 2014-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonb.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/transam.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/jsonfuncs.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

typedef struct JsonbInState
{
	JsonbParseState *parseState;
	JsonbValue *res;
} JsonbInState;

/* unlike with json categories, we need to treat json and jsonb differently */
typedef enum					/* type categories for datum_to_jsonb */
{
	JSONBTYPE_NULL,				/* null, so we didn't bother to identify */
	JSONBTYPE_BOOL,				/* boolean (built-in types only) */
	JSONBTYPE_NUMERIC,			/* numeric (ditto) */
	JSONBTYPE_DATE,				/* we use special formatting for datetimes */
	JSONBTYPE_TIMESTAMP,		/* we use special formatting for timestamp */
	JSONBTYPE_TIMESTAMPTZ,		/* ... and timestamptz */
	JSONBTYPE_JSON,				/* JSON */
	JSONBTYPE_JSONB,			/* JSONB */
	JSONBTYPE_ARRAY,			/* array */
	JSONBTYPE_COMPOSITE,		/* composite */
	JSONBTYPE_JSONCAST,			/* something with an explicit cast to JSON */
	JSONBTYPE_OTHER				/* all else */
} JsonbTypeCategory;

typedef struct JsonbAggState
{
	JsonbInState *res;
	JsonbTypeCategory key_category;
	Oid			key_output_func;
	JsonbTypeCategory val_category;
	Oid			val_output_func;
} JsonbAggState;

static inline Datum jsonb_from_cstring(char *json, int len);
static size_t checkStringLen(size_t len);
static void jsonb_in_object_start(void *pstate);
static void jsonb_in_object_end(void *pstate);
static void jsonb_in_array_start(void *pstate);
static void jsonb_in_array_end(void *pstate);
static void jsonb_in_object_field_start(void *pstate, char *fname, bool isnull);
static void jsonb_put_escaped_value(StringInfo out, JsonbValue *scalarVal);
static void jsonb_in_scalar(void *pstate, char *token, JsonTokenType tokentype);
static void jsonb_categorize_type(Oid typoid,
								  JsonbTypeCategory *tcategory,
								  Oid *outfuncoid);
static void composite_to_jsonb(Datum composite, JsonbInState *result);
static void array_dim_to_jsonb(JsonbInState *result, int dim, int ndims, int *dims,
							   Datum *vals, bool *nulls, int *valcount,
							   JsonbTypeCategory tcategory, Oid outfuncoid);
static void array_to_jsonb_internal(Datum array, JsonbInState *result);
static void jsonb_categorize_type(Oid typoid,
								  JsonbTypeCategory *tcategory,
								  Oid *outfuncoid);
static void datum_to_jsonb(Datum val, bool is_null, JsonbInState *result,
						   JsonbTypeCategory tcategory, Oid outfuncoid,
						   bool key_scalar);
static void add_jsonb(Datum val, bool is_null, JsonbInState *result,
					  Oid val_type, bool key_scalar);
static JsonbParseState *clone_parse_state(JsonbParseState *state);
static char *JsonbToCStringWorker(StringInfo out, JsonbContainer *in, int estimated_len, bool indent);
static void add_indent(StringInfo out, bool indent, int level);

/*
 * jsonb type input function
 */
Datum
jsonb_in(PG_FUNCTION_ARGS)
{
	char	   *json = PG_GETARG_CSTRING(0);

	return jsonb_from_cstring(json, strlen(json));
}

/*
 * jsonb type recv function
 *
 * The type is sent as text in binary mode, so this is almost the same
 * as the input function, but it's prefixed with a version number so we
 * can change the binary format sent in future if necessary. For now,
 * only version 1 is supported.
 */
Datum
jsonb_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int			version = pq_getmsgint(buf, 1);
	char	   *str;
	int			nbytes;

	if (version == 1)
		str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	else
		elog(ERROR, "unsupported jsonb version number %d", version);

	return jsonb_from_cstring(str, nbytes);
}

/*
 * jsonb type output function
 */
Datum
jsonb_out(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	char	   *out;

	out = JsonbToCString(NULL, &jb->root, VARSIZE(jb));

	PG_RETURN_CSTRING(out);
}

/*
 * jsonb type send function
 *
 * Just send jsonb as a version number, then a string of text
 */
Datum
jsonb_send(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	StringInfoData buf;
	StringInfo	jtext = makeStringInfo();
	int			version = 1;

	(void) JsonbToCString(jtext, &jb->root, VARSIZE(jb));

	pq_begintypsend(&buf);
	pq_sendint8(&buf, version);
	pq_sendtext(&buf, jtext->data, jtext->len);
	pfree(jtext->data);
	pfree(jtext);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Get the type name of a jsonb container.
 */
static const char *
JsonbContainerTypeName(JsonbContainer *jbc)
{
	JsonbValue	scalar;

	if (JsonbExtractScalar(jbc, &scalar))
		return JsonbTypeName(&scalar);
	else if (JsonContainerIsArray(jbc))
		return "array";
	else if (JsonContainerIsObject(jbc))
		return "object";
	else
	{
		elog(ERROR, "invalid jsonb container type: 0x%08x", jbc->header);
		return "unknown";
	}
}

/*
 * Get the type name of a jsonb value.
 */
const char *
JsonbTypeName(JsonbValue *jbv)
{
	switch (jbv->type)
	{
		case jbvBinary:
			return JsonbContainerTypeName(jbv->val.binary.data);
		case jbvObject:
			return "object";
		case jbvArray:
			return "array";
		case jbvNumeric:
			return "number";
		case jbvString:
			return "string";
		case jbvBool:
			return "boolean";
		case jbvNull:
			return "null";
		case jbvDatetime:
			switch (jbv->val.datetime.typid)
			{
				case DATEOID:
					return "date";
				case TIMEOID:
					return "time without time zone";
				case TIMETZOID:
					return "time with time zone";
				case TIMESTAMPOID:
					return "timestamp without time zone";
				case TIMESTAMPTZOID:
					return "timestamp with time zone";
				default:
					elog(ERROR, "unrecognized jsonb value datetime type: %d",
						 jbv->val.datetime.typid);
			}
			return "unknown";
		default:
			elog(ERROR, "unrecognized jsonb value type: %d", jbv->type);
			return "unknown";
	}
}

/*
 * SQL function jsonb_typeof(jsonb) -> text
 *
 * This function is here because the analog json function is in json.c, since
 * it uses the json parser internals not exposed elsewhere.
 */
Datum
jsonb_typeof(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	const char *result = JsonbContainerTypeName(&in->root);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * jsonb_from_cstring
 *
 * Turns json string into a jsonb Datum.
 *
 * Uses the json parser (with hooks) to construct a jsonb.
 */
static inline Datum
jsonb_from_cstring(char *json, int len)
{
	JsonLexContext *lex;
	JsonbInState state;
	JsonSemAction sem;

	memset(&state, 0, sizeof(state));
	memset(&sem, 0, sizeof(sem));
	lex = makeJsonLexContextCstringLen(json, len, GetDatabaseEncoding(), true);

	sem.semstate = (void *) &state;

	sem.object_start = jsonb_in_object_start;
	sem.array_start = jsonb_in_array_start;
	sem.object_end = jsonb_in_object_end;
	sem.array_end = jsonb_in_array_end;
	sem.scalar = jsonb_in_scalar;
	sem.object_field_start = jsonb_in_object_field_start;

	pg_parse_json_or_ereport(lex, &sem);

	/* after parsing, the item member has the composed jsonb structure */
	PG_RETURN_POINTER(JsonbValueToJsonb(state.res));
}

static size_t
checkStringLen(size_t len)
{
	if (len > JENTRY_OFFLENMASK)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("string too long to represent as jsonb string"),
				 errdetail("Due to an implementation restriction, jsonb strings cannot exceed %d bytes.",
						   JENTRY_OFFLENMASK)));

	return len;
}

static void
jsonb_in_object_start(void *pstate)
{
	JsonbInState *_state = (JsonbInState *) pstate;

	_state->res = pushJsonbValue(&_state->parseState, WJB_BEGIN_OBJECT, NULL);
}

static void
jsonb_in_object_end(void *pstate)
{
	JsonbInState *_state = (JsonbInState *) pstate;

	_state->res = pushJsonbValue(&_state->parseState, WJB_END_OBJECT, NULL);
}

static void
jsonb_in_array_start(void *pstate)
{
	JsonbInState *_state = (JsonbInState *) pstate;

	_state->res = pushJsonbValue(&_state->parseState, WJB_BEGIN_ARRAY, NULL);
}

static void
jsonb_in_array_end(void *pstate)
{
	JsonbInState *_state = (JsonbInState *) pstate;

	_state->res = pushJsonbValue(&_state->parseState, WJB_END_ARRAY, NULL);
}

static void
jsonb_in_object_field_start(void *pstate, char *fname, bool isnull)
{
	JsonbInState *_state = (JsonbInState *) pstate;
	JsonbValue	v;

	Assert(fname != NULL);
	v.type = jbvString;
	v.val.string.len = checkStringLen(strlen(fname));
	v.val.string.val = fname;

	_state->res = pushJsonbValue(&_state->parseState, WJB_KEY, &v);
}

static void
jsonb_put_escaped_value(StringInfo out, JsonbValue *scalarVal)
{
	switch (scalarVal->type)
	{
		case jbvNull:
			appendBinaryStringInfo(out, "null", 4);
			break;
		case jbvString:
			escape_json(out, pnstrdup(scalarVal->val.string.val, scalarVal->val.string.len));
			break;
		case jbvNumeric:
			appendStringInfoString(out,
								   DatumGetCString(DirectFunctionCall1(numeric_out,
																	   PointerGetDatum(scalarVal->val.numeric))));
			break;
		case jbvBool:
			if (scalarVal->val.boolean)
				appendBinaryStringInfo(out, "true", 4);
			else
				appendBinaryStringInfo(out, "false", 5);
			break;
		default:
			elog(ERROR, "unknown jsonb scalar type");
	}
}

/*
 * For jsonb we always want the de-escaped value - that's what's in token
 */
static void
jsonb_in_scalar(void *pstate, char *token, JsonTokenType tokentype)
{
	JsonbInState *_state = (JsonbInState *) pstate;
	JsonbValue	v;
	Datum		numd;

	switch (tokentype)
	{

		case JSON_TOKEN_STRING:
			Assert(token != NULL);
			v.type = jbvString;
			v.val.string.len = checkStringLen(strlen(token));
			v.val.string.val = token;
			break;
		case JSON_TOKEN_NUMBER:

			/*
			 * No need to check size of numeric values, because maximum
			 * numeric size is well below the JsonbValue restriction
			 */
			Assert(token != NULL);
			v.type = jbvNumeric;
			numd = DirectFunctionCall3(numeric_in,
									   CStringGetDatum(token),
									   ObjectIdGetDatum(InvalidOid),
									   Int32GetDatum(-1));
			v.val.numeric = DatumGetNumeric(numd);
			break;
		case JSON_TOKEN_TRUE:
			v.type = jbvBool;
			v.val.boolean = true;
			break;
		case JSON_TOKEN_FALSE:
			v.type = jbvBool;
			v.val.boolean = false;
			break;
		case JSON_TOKEN_NULL:
			v.type = jbvNull;
			break;
		default:
			/* should not be possible */
			elog(ERROR, "invalid json token type");
			break;
	}

	if (_state->parseState == NULL)
	{
		/* single scalar */
		JsonbValue	va;

		va.type = jbvArray;
		va.val.array.rawScalar = true;
		va.val.array.nElems = 1;

		_state->res = pushJsonbValue(&_state->parseState, WJB_BEGIN_ARRAY, &va);
		_state->res = pushJsonbValue(&_state->parseState, WJB_ELEM, &v);
		_state->res = pushJsonbValue(&_state->parseState, WJB_END_ARRAY, NULL);
	}
	else
	{
		JsonbValue *o = &_state->parseState->contVal;

		switch (o->type)
		{
			case jbvArray:
				_state->res = pushJsonbValue(&_state->parseState, WJB_ELEM, &v);
				break;
			case jbvObject:
				_state->res = pushJsonbValue(&_state->parseState, WJB_VALUE, &v);
				break;
			default:
				elog(ERROR, "unexpected parent of nested structure");
		}
	}
}

/*
 * JsonbToCString
 *	   Converts jsonb value to a C-string.
 *
 * If 'out' argument is non-null, the resulting C-string is stored inside the
 * StringBuffer.  The resulting string is always returned.
 *
 * A typical case for passing the StringInfo in rather than NULL is where the
 * caller wants access to the len attribute without having to call strlen, e.g.
 * if they are converting it to a text* object.
 */
char *
JsonbToCString(StringInfo out, JsonbContainer *in, int estimated_len)
{
	return JsonbToCStringWorker(out, in, estimated_len, false);
}

/*
 * same thing but with indentation turned on
 */
char *
JsonbToCStringIndent(StringInfo out, JsonbContainer *in, int estimated_len)
{
	return JsonbToCStringWorker(out, in, estimated_len, true);
}

/*
 * common worker for above two functions
 */
static char *
JsonbToCStringWorker(StringInfo out, JsonbContainer *in, int estimated_len, bool indent)
{
	bool		first = true;
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken type = WJB_DONE;
	int			level = 0;
	bool		redo_switch = false;

	/* If we are indenting, don't add a space after a comma */
	int			ispaces = indent ? 1 : 2;

	/*
	 * Don't indent the very first item. This gets set to the indent flag at
	 * the bottom of the loop.
	 */
	bool		use_indent = false;
	bool		raw_scalar = false;
	bool		last_was_key = false;

	if (out == NULL)
		out = makeStringInfo();

	enlargeStringInfo(out, (estimated_len >= 0) ? estimated_len : 64);

	it = JsonbIteratorInit(in);

	while (redo_switch ||
		   ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE))
	{
		redo_switch = false;
		switch (type)
		{
			case WJB_BEGIN_ARRAY:
				if (!first)
					appendBinaryStringInfo(out, ", ", ispaces);

				if (!v.val.array.rawScalar)
				{
					add_indent(out, use_indent && !last_was_key, level);
					appendStringInfoCharMacro(out, '[');
				}
				else
					raw_scalar = true;

				first = true;
				level++;
				break;
			case WJB_BEGIN_OBJECT:
				if (!first)
					appendBinaryStringInfo(out, ", ", ispaces);

				add_indent(out, use_indent && !last_was_key, level);
				appendStringInfoCharMacro(out, '{');

				first = true;
				level++;
				break;
			case WJB_KEY:
				if (!first)
					appendBinaryStringInfo(out, ", ", ispaces);
				first = true;

				add_indent(out, use_indent, level);

				/* json rules guarantee this is a string */
				jsonb_put_escaped_value(out, &v);
				appendBinaryStringInfo(out, ": ", 2);

				type = JsonbIteratorNext(&it, &v, false);
				if (type == WJB_VALUE)
				{
					first = false;
					jsonb_put_escaped_value(out, &v);
				}
				else
				{
					Assert(type == WJB_BEGIN_OBJECT || type == WJB_BEGIN_ARRAY);

					/*
					 * We need to rerun the current switch() since we need to
					 * output the object which we just got from the iterator
					 * before calling the iterator again.
					 */
					redo_switch = true;
				}
				break;
			case WJB_ELEM:
				if (!first)
					appendBinaryStringInfo(out, ", ", ispaces);
				first = false;

				if (!raw_scalar)
					add_indent(out, use_indent, level);
				jsonb_put_escaped_value(out, &v);
				break;
			case WJB_END_ARRAY:
				level--;
				if (!raw_scalar)
				{
					add_indent(out, use_indent, level);
					appendStringInfoCharMacro(out, ']');
				}
				first = false;
				break;
			case WJB_END_OBJECT:
				level--;
				add_indent(out, use_indent, level);
				appendStringInfoCharMacro(out, '}');
				first = false;
				break;
			default:
				elog(ERROR, "unknown jsonb iterator token type");
		}
		use_indent = indent;
		last_was_key = redo_switch;
	}

	Assert(level == 0);

	return out->data;
}

static void
add_indent(StringInfo out, bool indent, int level)
{
	if (indent)
	{
		int			i;

		appendStringInfoCharMacro(out, '\n');
		for (i = 0; i < level; i++)
			appendBinaryStringInfo(out, "    ", 4);
	}
}


/*
 * Determine how we want to render values of a given type in datum_to_jsonb.
 *
 * Given the datatype OID, return its JsonbTypeCategory, as well as the type's
 * output function OID.  If the returned category is JSONBTYPE_JSONCAST,
 * we return the OID of the relevant cast function instead.
 */
static void
jsonb_categorize_type(Oid typoid,
					  JsonbTypeCategory *tcategory,
					  Oid *outfuncoid)
{
	bool		typisvarlena;

	/* Look through any domain */
	typoid = getBaseType(typoid);

	*outfuncoid = InvalidOid;

	/*
	 * We need to get the output function for everything except date and
	 * timestamp types, booleans, array and composite types, json and jsonb,
	 * and non-builtin types where there's a cast to json. In this last case
	 * we return the oid of the cast function instead.
	 */

	switch (typoid)
	{
		case BOOLOID:
			*tcategory = JSONBTYPE_BOOL;
			break;

		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
			*tcategory = JSONBTYPE_NUMERIC;
			break;

		case DATEOID:
			*tcategory = JSONBTYPE_DATE;
			break;

		case TIMESTAMPOID:
			*tcategory = JSONBTYPE_TIMESTAMP;
			break;

		case TIMESTAMPTZOID:
			*tcategory = JSONBTYPE_TIMESTAMPTZ;
			break;

		case JSONBOID:
			*tcategory = JSONBTYPE_JSONB;
			break;

		case JSONOID:
			*tcategory = JSONBTYPE_JSON;
			break;

		default:
			/* Check for arrays and composites */
			if (OidIsValid(get_element_type(typoid)) || typoid == ANYARRAYOID
				|| typoid == ANYCOMPATIBLEARRAYOID || typoid == RECORDARRAYOID)
				*tcategory = JSONBTYPE_ARRAY;
			else if (type_is_rowtype(typoid))	/* includes RECORDOID */
				*tcategory = JSONBTYPE_COMPOSITE;
			else
			{
				/* It's probably the general case ... */
				*tcategory = JSONBTYPE_OTHER;

				/*
				 * but first let's look for a cast to json (note: not to
				 * jsonb) if it's not built-in.
				 */
				if (typoid >= FirstNormalObjectId)
				{
					Oid			castfunc;
					CoercionPathType ctype;

					ctype = find_coercion_pathway(JSONOID, typoid,
												  COERCION_EXPLICIT, &castfunc);
					if (ctype == COERCION_PATH_FUNC && OidIsValid(castfunc))
					{
						*tcategory = JSONBTYPE_JSONCAST;
						*outfuncoid = castfunc;
					}
					else
					{
						/* not a cast type, so just get the usual output func */
						getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
					}
				}
				else
				{
					/* any other builtin type */
					getTypeOutputInfo(typoid, outfuncoid, &typisvarlena);
				}
				break;
			}
	}
}

/*
 * Turn a Datum into jsonb, adding it to the result JsonbInState.
 *
 * tcategory and outfuncoid are from a previous call to json_categorize_type,
 * except that if is_null is true then they can be invalid.
 *
 * If key_scalar is true, the value is stored as a key, so insist
 * it's of an acceptable type, and force it to be a jbvString.
 */
static void
datum_to_jsonb(Datum val, bool is_null, JsonbInState *result,
			   JsonbTypeCategory tcategory, Oid outfuncoid,
			   bool key_scalar)
{
	char	   *outputstr;
	bool		numeric_error;
	JsonbValue	jb;
	bool		scalar_jsonb = false;

	check_stack_depth();

	/* Convert val to a JsonbValue in jb (in most cases) */
	if (is_null)
	{
		Assert(!key_scalar);
		jb.type = jbvNull;
	}
	else if (key_scalar &&
			 (tcategory == JSONBTYPE_ARRAY ||
			  tcategory == JSONBTYPE_COMPOSITE ||
			  tcategory == JSONBTYPE_JSON ||
			  tcategory == JSONBTYPE_JSONB ||
			  tcategory == JSONBTYPE_JSONCAST))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("key value must be scalar, not array, composite, or json")));
	}
	else
	{
		if (tcategory == JSONBTYPE_JSONCAST)
			val = OidFunctionCall1(outfuncoid, val);

		switch (tcategory)
		{
			case JSONBTYPE_ARRAY:
				array_to_jsonb_internal(val, result);
				break;
			case JSONBTYPE_COMPOSITE:
				composite_to_jsonb(val, result);
				break;
			case JSONBTYPE_BOOL:
				if (key_scalar)
				{
					outputstr = DatumGetBool(val) ? "true" : "false";
					jb.type = jbvString;
					jb.val.string.len = strlen(outputstr);
					jb.val.string.val = outputstr;
				}
				else
				{
					jb.type = jbvBool;
					jb.val.boolean = DatumGetBool(val);
				}
				break;
			case JSONBTYPE_NUMERIC:
				outputstr = OidOutputFunctionCall(outfuncoid, val);
				if (key_scalar)
				{
					/* always quote keys */
					jb.type = jbvString;
					jb.val.string.len = strlen(outputstr);
					jb.val.string.val = outputstr;
				}
				else
				{
					/*
					 * Make it numeric if it's a valid JSON number, otherwise
					 * a string. Invalid numeric output will always have an
					 * 'N' or 'n' in it (I think).
					 */
					numeric_error = (strchr(outputstr, 'N') != NULL ||
									 strchr(outputstr, 'n') != NULL);
					if (!numeric_error)
					{
						Datum		numd;

						jb.type = jbvNumeric;
						numd = DirectFunctionCall3(numeric_in,
												   CStringGetDatum(outputstr),
												   ObjectIdGetDatum(InvalidOid),
												   Int32GetDatum(-1));
						jb.val.numeric = DatumGetNumeric(numd);
						pfree(outputstr);
					}
					else
					{
						jb.type = jbvString;
						jb.val.string.len = strlen(outputstr);
						jb.val.string.val = outputstr;
					}
				}
				break;
			case JSONBTYPE_DATE:
				jb.type = jbvString;
				jb.val.string.val = JsonEncodeDateTime(NULL, val,
													   DATEOID, NULL);
				jb.val.string.len = strlen(jb.val.string.val);
				break;
			case JSONBTYPE_TIMESTAMP:
				jb.type = jbvString;
				jb.val.string.val = JsonEncodeDateTime(NULL, val,
													   TIMESTAMPOID, NULL);
				jb.val.string.len = strlen(jb.val.string.val);
				break;
			case JSONBTYPE_TIMESTAMPTZ:
				jb.type = jbvString;
				jb.val.string.val = JsonEncodeDateTime(NULL, val,
													   TIMESTAMPTZOID, NULL);
				jb.val.string.len = strlen(jb.val.string.val);
				break;
			case JSONBTYPE_JSONCAST:
			case JSONBTYPE_JSON:
				{
					/* parse the json right into the existing result object */
					JsonLexContext *lex;
					JsonSemAction sem;
					text	   *json = DatumGetTextPP(val);

					lex = makeJsonLexContext(json, true);

					memset(&sem, 0, sizeof(sem));

					sem.semstate = (void *) result;

					sem.object_start = jsonb_in_object_start;
					sem.array_start = jsonb_in_array_start;
					sem.object_end = jsonb_in_object_end;
					sem.array_end = jsonb_in_array_end;
					sem.scalar = jsonb_in_scalar;
					sem.object_field_start = jsonb_in_object_field_start;

					pg_parse_json_or_ereport(lex, &sem);

				}
				break;
			case JSONBTYPE_JSONB:
				{
					Jsonb	   *jsonb = DatumGetJsonbP(val);
					JsonbIterator *it;

					it = JsonbIteratorInit(&jsonb->root);

					if (JB_ROOT_IS_SCALAR(jsonb))
					{
						(void) JsonbIteratorNext(&it, &jb, true);
						Assert(jb.type == jbvArray);
						(void) JsonbIteratorNext(&it, &jb, true);
						scalar_jsonb = true;
					}
					else
					{
						JsonbIteratorToken type;

						while ((type = JsonbIteratorNext(&it, &jb, false))
							   != WJB_DONE)
						{
							if (type == WJB_END_ARRAY || type == WJB_END_OBJECT ||
								type == WJB_BEGIN_ARRAY || type == WJB_BEGIN_OBJECT)
								result->res = pushJsonbValue(&result->parseState,
															 type, NULL);
							else
								result->res = pushJsonbValue(&result->parseState,
															 type, &jb);
						}
					}
				}
				break;
			default:
				outputstr = OidOutputFunctionCall(outfuncoid, val);
				jb.type = jbvString;
				jb.val.string.len = checkStringLen(strlen(outputstr));
				jb.val.string.val = outputstr;
				break;
		}
	}

	/* Now insert jb into result, unless we did it recursively */
	if (!is_null && !scalar_jsonb &&
		tcategory >= JSONBTYPE_JSON && tcategory <= JSONBTYPE_JSONCAST)
	{
		/* work has been done recursively */
		return;
	}
	else if (result->parseState == NULL)
	{
		/* single root scalar */
		JsonbValue	va;

		va.type = jbvArray;
		va.val.array.rawScalar = true;
		va.val.array.nElems = 1;

		result->res = pushJsonbValue(&result->parseState, WJB_BEGIN_ARRAY, &va);
		result->res = pushJsonbValue(&result->parseState, WJB_ELEM, &jb);
		result->res = pushJsonbValue(&result->parseState, WJB_END_ARRAY, NULL);
	}
	else
	{
		JsonbValue *o = &result->parseState->contVal;

		switch (o->type)
		{
			case jbvArray:
				result->res = pushJsonbValue(&result->parseState, WJB_ELEM, &jb);
				break;
			case jbvObject:
				result->res = pushJsonbValue(&result->parseState,
											 key_scalar ? WJB_KEY : WJB_VALUE,
											 &jb);
				break;
			default:
				elog(ERROR, "unexpected parent of nested structure");
		}
	}
}

/*
 * Process a single dimension of an array.
 * If it's the innermost dimension, output the values, otherwise call
 * ourselves recursively to process the next dimension.
 */
static void
array_dim_to_jsonb(JsonbInState *result, int dim, int ndims, int *dims, Datum *vals,
				   bool *nulls, int *valcount, JsonbTypeCategory tcategory,
				   Oid outfuncoid)
{
	int			i;

	Assert(dim < ndims);

	result->res = pushJsonbValue(&result->parseState, WJB_BEGIN_ARRAY, NULL);

	for (i = 1; i <= dims[dim]; i++)
	{
		if (dim + 1 == ndims)
		{
			datum_to_jsonb(vals[*valcount], nulls[*valcount], result, tcategory,
						   outfuncoid, false);
			(*valcount)++;
		}
		else
		{
			array_dim_to_jsonb(result, dim + 1, ndims, dims, vals, nulls,
							   valcount, tcategory, outfuncoid);
		}
	}

	result->res = pushJsonbValue(&result->parseState, WJB_END_ARRAY, NULL);
}

/*
 * Turn an array into JSON.
 */
static void
array_to_jsonb_internal(Datum array, JsonbInState *result)
{
	ArrayType  *v = DatumGetArrayTypeP(array);
	Oid			element_type = ARR_ELEMTYPE(v);
	int		   *dim;
	int			ndim;
	int			nitems;
	int			count = 0;
	Datum	   *elements;
	bool	   *nulls;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	JsonbTypeCategory tcategory;
	Oid			outfuncoid;

	ndim = ARR_NDIM(v);
	dim = ARR_DIMS(v);
	nitems = ArrayGetNItems(ndim, dim);

	if (nitems <= 0)
	{
		result->res = pushJsonbValue(&result->parseState, WJB_BEGIN_ARRAY, NULL);
		result->res = pushJsonbValue(&result->parseState, WJB_END_ARRAY, NULL);
		return;
	}

	get_typlenbyvalalign(element_type,
						 &typlen, &typbyval, &typalign);

	jsonb_categorize_type(element_type,
						  &tcategory, &outfuncoid);

	deconstruct_array(v, element_type, typlen, typbyval,
					  typalign, &elements, &nulls,
					  &nitems);

	array_dim_to_jsonb(result, 0, ndim, dim, elements, nulls, &count, tcategory,
					   outfuncoid);

	pfree(elements);
	pfree(nulls);
}

/*
 * Turn a composite / record into JSON.
 */
static void
composite_to_jsonb(Datum composite, JsonbInState *result)
{
	HeapTupleHeader td;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tmptup,
			   *tuple;
	int			i;

	td = DatumGetHeapTupleHeader(composite);

	/* Extract rowtype info and find a tupdesc */
	tupType = HeapTupleHeaderGetTypeId(td);
	tupTypmod = HeapTupleHeaderGetTypMod(td);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);

	/* Build a temporary HeapTuple control structure */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
	tmptup.t_data = td;
	tuple = &tmptup;

	result->res = pushJsonbValue(&result->parseState, WJB_BEGIN_OBJECT, NULL);

	for (i = 0; i < tupdesc->natts; i++)
	{
		Datum		val;
		bool		isnull;
		char	   *attname;
		JsonbTypeCategory tcategory;
		Oid			outfuncoid;
		JsonbValue	v;
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;

		attname = NameStr(att->attname);

		v.type = jbvString;
		/* don't need checkStringLen here - can't exceed maximum name length */
		v.val.string.len = strlen(attname);
		v.val.string.val = attname;

		result->res = pushJsonbValue(&result->parseState, WJB_KEY, &v);

		val = heap_getattr(tuple, i + 1, tupdesc, &isnull);

		if (isnull)
		{
			tcategory = JSONBTYPE_NULL;
			outfuncoid = InvalidOid;
		}
		else
			jsonb_categorize_type(att->atttypid, &tcategory, &outfuncoid);

		datum_to_jsonb(val, isnull, result, tcategory, outfuncoid, false);
	}

	result->res = pushJsonbValue(&result->parseState, WJB_END_OBJECT, NULL);
	ReleaseTupleDesc(tupdesc);
}

/*
 * Append JSON text for "val" to "result".
 *
 * This is just a thin wrapper around datum_to_jsonb.  If the same type will be
 * printed many times, avoid using this; better to do the jsonb_categorize_type
 * lookups only once.
 */

static void
add_jsonb(Datum val, bool is_null, JsonbInState *result,
		  Oid val_type, bool key_scalar)
{
	JsonbTypeCategory tcategory;
	Oid			outfuncoid;

	if (val_type == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine input data type")));

	if (is_null)
	{
		tcategory = JSONBTYPE_NULL;
		outfuncoid = InvalidOid;
	}
	else
		jsonb_categorize_type(val_type,
							  &tcategory, &outfuncoid);

	datum_to_jsonb(val, is_null, result, tcategory, outfuncoid, key_scalar);
}

/*
 * SQL function to_jsonb(anyvalue)
 */
Datum
to_jsonb(PG_FUNCTION_ARGS)
{
	Datum		val = PG_GETARG_DATUM(0);
	Oid			val_type = get_fn_expr_argtype(fcinfo->flinfo, 0);
	JsonbInState result;
	JsonbTypeCategory tcategory;
	Oid			outfuncoid;

	if (val_type == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not determine input data type")));

	jsonb_categorize_type(val_type,
						  &tcategory, &outfuncoid);

	memset(&result, 0, sizeof(JsonbInState));

	datum_to_jsonb(val, false, &result, tcategory, outfuncoid, false);

	PG_RETURN_POINTER(JsonbValueToJsonb(result.res));
}

/*
 * SQL function jsonb_build_object(variadic "any")
 */
Datum
jsonb_build_object(PG_FUNCTION_ARGS)
{
	int			nargs;
	int			i;
	JsonbInState result;
	Datum	   *args;
	bool	   *nulls;
	Oid		   *types;

	/* build argument values to build the object */
	nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);

	if (nargs < 0)
		PG_RETURN_NULL();

	if (nargs % 2 != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("argument list must have even number of elements"),
		/* translator: %s is a SQL function name */
				 errhint("The arguments of %s must consist of alternating keys and values.",
						 "jsonb_build_object()")));

	memset(&result, 0, sizeof(JsonbInState));

	result.res = pushJsonbValue(&result.parseState, WJB_BEGIN_OBJECT, NULL);

	for (i = 0; i < nargs; i += 2)
	{
		/* process key */
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("argument %d: key must not be null", i + 1)));

		add_jsonb(args[i], false, &result, types[i], true);

		/* process value */
		add_jsonb(args[i + 1], nulls[i + 1], &result, types[i + 1], false);
	}

	result.res = pushJsonbValue(&result.parseState, WJB_END_OBJECT, NULL);

	PG_RETURN_POINTER(JsonbValueToJsonb(result.res));
}

/*
 * degenerate case of jsonb_build_object where it gets 0 arguments.
 */
Datum
jsonb_build_object_noargs(PG_FUNCTION_ARGS)
{
	JsonbInState result;

	memset(&result, 0, sizeof(JsonbInState));

	(void) pushJsonbValue(&result.parseState, WJB_BEGIN_OBJECT, NULL);
	result.res = pushJsonbValue(&result.parseState, WJB_END_OBJECT, NULL);

	PG_RETURN_POINTER(JsonbValueToJsonb(result.res));
}

/*
 * SQL function jsonb_build_array(variadic "any")
 */
Datum
jsonb_build_array(PG_FUNCTION_ARGS)
{
	int			nargs;
	int			i;
	JsonbInState result;
	Datum	   *args;
	bool	   *nulls;
	Oid		   *types;

	/* build argument values to build the array */
	nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);

	if (nargs < 0)
		PG_RETURN_NULL();

	memset(&result, 0, sizeof(JsonbInState));

	result.res = pushJsonbValue(&result.parseState, WJB_BEGIN_ARRAY, NULL);

	for (i = 0; i < nargs; i++)
		add_jsonb(args[i], nulls[i], &result, types[i], false);

	result.res = pushJsonbValue(&result.parseState, WJB_END_ARRAY, NULL);

	PG_RETURN_POINTER(JsonbValueToJsonb(result.res));
}

/*
 * degenerate case of jsonb_build_array where it gets 0 arguments.
 */
Datum
jsonb_build_array_noargs(PG_FUNCTION_ARGS)
{
	JsonbInState result;

	memset(&result, 0, sizeof(JsonbInState));

	(void) pushJsonbValue(&result.parseState, WJB_BEGIN_ARRAY, NULL);
	result.res = pushJsonbValue(&result.parseState, WJB_END_ARRAY, NULL);

	PG_RETURN_POINTER(JsonbValueToJsonb(result.res));
}


/*
 * SQL function jsonb_object(text[])
 *
 * take a one or two dimensional array of text as name value pairs
 * for a jsonb object.
 *
 */
Datum
jsonb_object(PG_FUNCTION_ARGS)
{
	ArrayType  *in_array = PG_GETARG_ARRAYTYPE_P(0);
	int			ndims = ARR_NDIM(in_array);
	Datum	   *in_datums;
	bool	   *in_nulls;
	int			in_count,
				count,
				i;
	JsonbInState result;

	memset(&result, 0, sizeof(JsonbInState));

	(void) pushJsonbValue(&result.parseState, WJB_BEGIN_OBJECT, NULL);

	switch (ndims)
	{
		case 0:
			goto close_object;
			break;

		case 1:
			if ((ARR_DIMS(in_array)[0]) % 2)
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("array must have even number of elements")));
			break;

		case 2:
			if ((ARR_DIMS(in_array)[1]) != 2)
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("array must have two columns")));
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("wrong number of array subscripts")));
	}

	deconstruct_array(in_array,
					  TEXTOID, -1, false, TYPALIGN_INT,
					  &in_datums, &in_nulls, &in_count);

	count = in_count / 2;

	for (i = 0; i < count; ++i)
	{
		JsonbValue	v;
		char	   *str;
		int			len;

		if (in_nulls[i * 2])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null value not allowed for object key")));

		str = TextDatumGetCString(in_datums[i * 2]);
		len = strlen(str);

		v.type = jbvString;

		v.val.string.len = len;
		v.val.string.val = str;

		(void) pushJsonbValue(&result.parseState, WJB_KEY, &v);

		if (in_nulls[i * 2 + 1])
		{
			v.type = jbvNull;
		}
		else
		{
			str = TextDatumGetCString(in_datums[i * 2 + 1]);
			len = strlen(str);

			v.type = jbvString;

			v.val.string.len = len;
			v.val.string.val = str;
		}

		(void) pushJsonbValue(&result.parseState, WJB_VALUE, &v);
	}

	pfree(in_datums);
	pfree(in_nulls);

close_object:
	result.res = pushJsonbValue(&result.parseState, WJB_END_OBJECT, NULL);

	PG_RETURN_POINTER(JsonbValueToJsonb(result.res));
}

/*
 * SQL function jsonb_object(text[], text[])
 *
 * take separate name and value arrays of text to construct a jsonb object
 * pairwise.
 */
Datum
jsonb_object_two_arg(PG_FUNCTION_ARGS)
{
	ArrayType  *key_array = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *val_array = PG_GETARG_ARRAYTYPE_P(1);
	int			nkdims = ARR_NDIM(key_array);
	int			nvdims = ARR_NDIM(val_array);
	Datum	   *key_datums,
			   *val_datums;
	bool	   *key_nulls,
			   *val_nulls;
	int			key_count,
				val_count,
				i;
	JsonbInState result;

	memset(&result, 0, sizeof(JsonbInState));

	(void) pushJsonbValue(&result.parseState, WJB_BEGIN_OBJECT, NULL);

	if (nkdims > 1 || nkdims != nvdims)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	if (nkdims == 0)
		goto close_object;

	deconstruct_array(key_array,
					  TEXTOID, -1, false, TYPALIGN_INT,
					  &key_datums, &key_nulls, &key_count);

	deconstruct_array(val_array,
					  TEXTOID, -1, false, TYPALIGN_INT,
					  &val_datums, &val_nulls, &val_count);

	if (key_count != val_count)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("mismatched array dimensions")));

	for (i = 0; i < key_count; ++i)
	{
		JsonbValue	v;
		char	   *str;
		int			len;

		if (key_nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null value not allowed for object key")));

		str = TextDatumGetCString(key_datums[i]);
		len = strlen(str);

		v.type = jbvString;

		v.val.string.len = len;
		v.val.string.val = str;

		(void) pushJsonbValue(&result.parseState, WJB_KEY, &v);

		if (val_nulls[i])
		{
			v.type = jbvNull;
		}
		else
		{
			str = TextDatumGetCString(val_datums[i]);
			len = strlen(str);

			v.type = jbvString;

			v.val.string.len = len;
			v.val.string.val = str;
		}

		(void) pushJsonbValue(&result.parseState, WJB_VALUE, &v);
	}

	pfree(key_datums);
	pfree(key_nulls);
	pfree(val_datums);
	pfree(val_nulls);

close_object:
	result.res = pushJsonbValue(&result.parseState, WJB_END_OBJECT, NULL);

	PG_RETURN_POINTER(JsonbValueToJsonb(result.res));
}


/*
 * shallow clone of a parse state, suitable for use in aggregate
 * final functions that will only append to the values rather than
 * change them.
 */
static JsonbParseState *
clone_parse_state(JsonbParseState *state)
{
	JsonbParseState *result,
			   *icursor,
			   *ocursor;

	if (state == NULL)
		return NULL;

	result = palloc(sizeof(JsonbParseState));
	icursor = state;
	ocursor = result;
	for (;;)
	{
		ocursor->contVal = icursor->contVal;
		ocursor->size = icursor->size;
		icursor = icursor->next;
		if (icursor == NULL)
			break;
		ocursor->next = palloc(sizeof(JsonbParseState));
		ocursor = ocursor->next;
	}
	ocursor->next = NULL;

	return result;
}


/*
 * jsonb_agg aggregate function
 */
Datum
jsonb_agg_transfn(PG_FUNCTION_ARGS)
{
	MemoryContext oldcontext,
				aggcontext;
	JsonbAggState *state;
	JsonbInState elem;
	Datum		val;
	JsonbInState *result;
	bool		single_scalar = false;
	JsonbIterator *it;
	Jsonb	   *jbelem;
	JsonbValue	v;
	JsonbIteratorToken type;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* cannot be called directly because of internal-type argument */
		elog(ERROR, "jsonb_agg_transfn called in non-aggregate context");
	}

	/* set up the accumulator on the first go round */

	if (PG_ARGISNULL(0))
	{
		Oid			arg_type = get_fn_expr_argtype(fcinfo->flinfo, 1);

		if (arg_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine input data type")));

		oldcontext = MemoryContextSwitchTo(aggcontext);
		state = palloc(sizeof(JsonbAggState));
		result = palloc0(sizeof(JsonbInState));
		state->res = result;
		result->res = pushJsonbValue(&result->parseState,
									 WJB_BEGIN_ARRAY, NULL);
		MemoryContextSwitchTo(oldcontext);

		jsonb_categorize_type(arg_type, &state->val_category,
							  &state->val_output_func);
	}
	else
	{
		state = (JsonbAggState *) PG_GETARG_POINTER(0);
		result = state->res;
	}

	/* turn the argument into jsonb in the normal function context */

	val = PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1);

	memset(&elem, 0, sizeof(JsonbInState));

	datum_to_jsonb(val, PG_ARGISNULL(1), &elem, state->val_category,
				   state->val_output_func, false);

	jbelem = JsonbValueToJsonb(elem.res);

	/* switch to the aggregate context for accumulation operations */

	oldcontext = MemoryContextSwitchTo(aggcontext);

	it = JsonbIteratorInit(&jbelem->root);

	while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		switch (type)
		{
			case WJB_BEGIN_ARRAY:
				if (v.val.array.rawScalar)
					single_scalar = true;
				else
					result->res = pushJsonbValue(&result->parseState,
												 type, NULL);
				break;
			case WJB_END_ARRAY:
				if (!single_scalar)
					result->res = pushJsonbValue(&result->parseState,
												 type, NULL);
				break;
			case WJB_BEGIN_OBJECT:
			case WJB_END_OBJECT:
				result->res = pushJsonbValue(&result->parseState,
											 type, NULL);
				break;
			case WJB_ELEM:
			case WJB_KEY:
			case WJB_VALUE:
				if (v.type == jbvString)
				{
					/* copy string values in the aggregate context */
					char	   *buf = palloc(v.val.string.len + 1);

					snprintf(buf, v.val.string.len + 1, "%s", v.val.string.val);
					v.val.string.val = buf;
				}
				else if (v.type == jbvNumeric)
				{
					/* same for numeric */
					v.val.numeric =
						DatumGetNumeric(DirectFunctionCall1(numeric_uplus,
															NumericGetDatum(v.val.numeric)));
				}
				result->res = pushJsonbValue(&result->parseState,
											 type, &v);
				break;
			default:
				elog(ERROR, "unknown jsonb iterator token type");
		}
	}

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_POINTER(state);
}

Datum
jsonb_agg_finalfn(PG_FUNCTION_ARGS)
{
	JsonbAggState *arg;
	JsonbInState result;
	Jsonb	   *out;

	/* cannot be called directly because of internal-type argument */
	Assert(AggCheckCallContext(fcinfo, NULL));

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();		/* returns null iff no input values */

	arg = (JsonbAggState *) PG_GETARG_POINTER(0);

	/*
	 * We need to do a shallow clone of the argument in case the final
	 * function is called more than once, so we avoid changing the argument. A
	 * shallow clone is sufficient as we aren't going to change any of the
	 * values, just add the final array end marker.
	 */

	result.parseState = clone_parse_state(arg->res->parseState);

	result.res = pushJsonbValue(&result.parseState,
								WJB_END_ARRAY, NULL);

	out = JsonbValueToJsonb(result.res);

	PG_RETURN_POINTER(out);
}

/*
 * jsonb_object_agg aggregate function
 */
Datum
jsonb_object_agg_transfn(PG_FUNCTION_ARGS)
{
	MemoryContext oldcontext,
				aggcontext;
	JsonbInState elem;
	JsonbAggState *state;
	Datum		val;
	JsonbInState *result;
	bool		single_scalar;
	JsonbIterator *it;
	Jsonb	   *jbkey,
			   *jbval;
	JsonbValue	v;
	JsonbIteratorToken type;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
	{
		/* cannot be called directly because of internal-type argument */
		elog(ERROR, "jsonb_object_agg_transfn called in non-aggregate context");
	}

	/* set up the accumulator on the first go round */

	if (PG_ARGISNULL(0))
	{
		Oid			arg_type;

		oldcontext = MemoryContextSwitchTo(aggcontext);
		state = palloc(sizeof(JsonbAggState));
		result = palloc0(sizeof(JsonbInState));
		state->res = result;
		result->res = pushJsonbValue(&result->parseState,
									 WJB_BEGIN_OBJECT, NULL);
		MemoryContextSwitchTo(oldcontext);

		arg_type = get_fn_expr_argtype(fcinfo->flinfo, 1);

		if (arg_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine input data type")));

		jsonb_categorize_type(arg_type, &state->key_category,
							  &state->key_output_func);

		arg_type = get_fn_expr_argtype(fcinfo->flinfo, 2);

		if (arg_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine input data type")));

		jsonb_categorize_type(arg_type, &state->val_category,
							  &state->val_output_func);
	}
	else
	{
		state = (JsonbAggState *) PG_GETARG_POINTER(0);
		result = state->res;
	}

	/* turn the argument into jsonb in the normal function context */

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("field name must not be null")));

	val = PG_GETARG_DATUM(1);

	memset(&elem, 0, sizeof(JsonbInState));

	datum_to_jsonb(val, false, &elem, state->key_category,
				   state->key_output_func, true);

	jbkey = JsonbValueToJsonb(elem.res);

	val = PG_ARGISNULL(2) ? (Datum) 0 : PG_GETARG_DATUM(2);

	memset(&elem, 0, sizeof(JsonbInState));

	datum_to_jsonb(val, PG_ARGISNULL(2), &elem, state->val_category,
				   state->val_output_func, false);

	jbval = JsonbValueToJsonb(elem.res);

	it = JsonbIteratorInit(&jbkey->root);

	/* switch to the aggregate context for accumulation operations */

	oldcontext = MemoryContextSwitchTo(aggcontext);

	/*
	 * keys should be scalar, and we should have already checked for that
	 * above when calling datum_to_jsonb, so we only need to look for these
	 * things.
	 */

	while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		switch (type)
		{
			case WJB_BEGIN_ARRAY:
				if (!v.val.array.rawScalar)
					elog(ERROR, "unexpected structure for key");
				break;
			case WJB_ELEM:
				if (v.type == jbvString)
				{
					/* copy string values in the aggregate context */
					char	   *buf = palloc(v.val.string.len + 1);

					snprintf(buf, v.val.string.len + 1, "%s", v.val.string.val);
					v.val.string.val = buf;
				}
				else
				{
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("object keys must be strings")));
				}
				result->res = pushJsonbValue(&result->parseState,
											 WJB_KEY, &v);
				break;
			case WJB_END_ARRAY:
				break;
			default:
				elog(ERROR, "unexpected structure for key");
				break;
		}
	}

	it = JsonbIteratorInit(&jbval->root);

	single_scalar = false;

	/*
	 * values can be anything, including structured and null, so we treat them
	 * as in json_agg_transfn, except that single scalars are always pushed as
	 * WJB_VALUE items.
	 */

	while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		switch (type)
		{
			case WJB_BEGIN_ARRAY:
				if (v.val.array.rawScalar)
					single_scalar = true;
				else
					result->res = pushJsonbValue(&result->parseState,
												 type, NULL);
				break;
			case WJB_END_ARRAY:
				if (!single_scalar)
					result->res = pushJsonbValue(&result->parseState,
												 type, NULL);
				break;
			case WJB_BEGIN_OBJECT:
			case WJB_END_OBJECT:
				result->res = pushJsonbValue(&result->parseState,
											 type, NULL);
				break;
			case WJB_ELEM:
			case WJB_KEY:
			case WJB_VALUE:
				if (v.type == jbvString)
				{
					/* copy string values in the aggregate context */
					char	   *buf = palloc(v.val.string.len + 1);

					snprintf(buf, v.val.string.len + 1, "%s", v.val.string.val);
					v.val.string.val = buf;
				}
				else if (v.type == jbvNumeric)
				{
					/* same for numeric */
					v.val.numeric =
						DatumGetNumeric(DirectFunctionCall1(numeric_uplus,
															NumericGetDatum(v.val.numeric)));
				}
				result->res = pushJsonbValue(&result->parseState,
											 single_scalar ? WJB_VALUE : type,
											 &v);
				break;
			default:
				elog(ERROR, "unknown jsonb iterator token type");
		}
	}

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_POINTER(state);
}

Datum
jsonb_object_agg_finalfn(PG_FUNCTION_ARGS)
{
	JsonbAggState *arg;
	JsonbInState result;
	Jsonb	   *out;

	/* cannot be called directly because of internal-type argument */
	Assert(AggCheckCallContext(fcinfo, NULL));

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();		/* returns null iff no input values */

	arg = (JsonbAggState *) PG_GETARG_POINTER(0);

	/*
	 * We need to do a shallow clone of the argument's res field in case the
	 * final function is called more than once, so we avoid changing the
	 * aggregate state value.  A shallow clone is sufficient as we aren't
	 * going to change any of the values, just add the final object end
	 * marker.
	 */

	result.parseState = clone_parse_state(arg->res->parseState);

	result.res = pushJsonbValue(&result.parseState,
								WJB_END_OBJECT, NULL);

	out = JsonbValueToJsonb(result.res);

	PG_RETURN_POINTER(out);
}


/*
 * Extract scalar value from raw-scalar pseudo-array jsonb.
 */
bool
JsonbExtractScalar(JsonbContainer *jbc, JsonbValue *res)
{
	JsonbIterator *it;
	JsonbIteratorToken tok PG_USED_FOR_ASSERTS_ONLY;
	JsonbValue	tmp;

	if (!JsonContainerIsArray(jbc) || !JsonContainerIsScalar(jbc))
	{
		/* inform caller about actual type of container */
		res->type = (JsonContainerIsArray(jbc)) ? jbvArray : jbvObject;
		return false;
	}

	/*
	 * A root scalar is stored as an array of one element, so we get the array
	 * and then its first (and only) member.
	 */
	it = JsonbIteratorInit(jbc);

	tok = JsonbIteratorNext(&it, &tmp, true);
	Assert(tok == WJB_BEGIN_ARRAY);
	Assert(tmp.val.array.nElems == 1 && tmp.val.array.rawScalar);

	tok = JsonbIteratorNext(&it, res, true);
	Assert(tok == WJB_ELEM);
	Assert(IsAJsonbScalar(res));

	tok = JsonbIteratorNext(&it, &tmp, true);
	Assert(tok == WJB_END_ARRAY);

	tok = JsonbIteratorNext(&it, &tmp, true);
	Assert(tok == WJB_DONE);

	return true;
}

/*
 * Emit correct, translatable cast error message
 */
static void
cannotCastJsonbValue(enum jbvType type, const char *sqltype)
{
	static const struct
	{
		enum jbvType type;
		const char *msg;
	}
				messages[] =
	{
		{jbvNull, gettext_noop("cannot cast jsonb null to type %s")},
		{jbvString, gettext_noop("cannot cast jsonb string to type %s")},
		{jbvNumeric, gettext_noop("cannot cast jsonb numeric to type %s")},
		{jbvBool, gettext_noop("cannot cast jsonb boolean to type %s")},
		{jbvArray, gettext_noop("cannot cast jsonb array to type %s")},
		{jbvObject, gettext_noop("cannot cast jsonb object to type %s")},
		{jbvBinary, gettext_noop("cannot cast jsonb array or object to type %s")}
	};
	int			i;

	for (i = 0; i < lengthof(messages); i++)
		if (messages[i].type == type)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg(messages[i].msg, sqltype)));

	/* should be unreachable */
	elog(ERROR, "unknown jsonb type: %d", (int) type);
}

Datum
jsonb_bool(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	JsonbValue	v;

	if (!JsonbExtractScalar(&in->root, &v) || v.type != jbvBool)
		cannotCastJsonbValue(v.type, "boolean");

	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_BOOL(v.val.boolean);
}

Datum
jsonb_numeric(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	JsonbValue	v;
	Numeric		retValue;

	if (!JsonbExtractScalar(&in->root, &v) || v.type != jbvNumeric)
		cannotCastJsonbValue(v.type, "numeric");

	/*
	 * v.val.numeric points into jsonb body, so we need to make a copy to
	 * return
	 */
	retValue = DatumGetNumericCopy(NumericGetDatum(v.val.numeric));

	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_NUMERIC(retValue);
}

Datum
jsonb_int2(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	JsonbValue	v;
	Datum		retValue;

	if (!JsonbExtractScalar(&in->root, &v) || v.type != jbvNumeric)
		cannotCastJsonbValue(v.type, "smallint");

	retValue = DirectFunctionCall1(numeric_int2,
								   NumericGetDatum(v.val.numeric));

	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_DATUM(retValue);
}

Datum
jsonb_int4(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	JsonbValue	v;
	Datum		retValue;

	if (!JsonbExtractScalar(&in->root, &v) || v.type != jbvNumeric)
		cannotCastJsonbValue(v.type, "integer");

	retValue = DirectFunctionCall1(numeric_int4,
								   NumericGetDatum(v.val.numeric));

	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_DATUM(retValue);
}

Datum
jsonb_int8(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	JsonbValue	v;
	Datum		retValue;

	if (!JsonbExtractScalar(&in->root, &v) || v.type != jbvNumeric)
		cannotCastJsonbValue(v.type, "bigint");

	retValue = DirectFunctionCall1(numeric_int8,
								   NumericGetDatum(v.val.numeric));

	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_DATUM(retValue);
}

Datum
jsonb_float4(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	JsonbValue	v;
	Datum		retValue;

	if (!JsonbExtractScalar(&in->root, &v) || v.type != jbvNumeric)
		cannotCastJsonbValue(v.type, "real");

	retValue = DirectFunctionCall1(numeric_float4,
								   NumericGetDatum(v.val.numeric));

	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_DATUM(retValue);
}

Datum
jsonb_float8(PG_FUNCTION_ARGS)
{
	Jsonb	   *in = PG_GETARG_JSONB_P(0);
	JsonbValue	v;
	Datum		retValue;

	if (!JsonbExtractScalar(&in->root, &v) || v.type != jbvNumeric)
		cannotCastJsonbValue(v.type, "double precision");

	retValue = DirectFunctionCall1(numeric_float8,
								   NumericGetDatum(v.val.numeric));

	PG_FREE_IF_COPY(in, 0);

	PG_RETURN_DATUM(retValue);
}
