#include "postgres.h"

#include "plpy_elog.h"
#include "plpy_typeio.h"
#include "plpython.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/numeric.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

/* for PLyObject_AsString in plpy_typeio.c */
typedef char *(*PLyObject_AsString_t) (PyObject *plrv);
static PLyObject_AsString_t PLyObject_AsString_p;

typedef void (*PLy_elog_impl_t) (int elevel, const char *fmt,...);
static PLy_elog_impl_t PLy_elog_impl_p;

/*
 * decimal_constructor is a function from python library and used
 * for transforming strings into python decimal type
 */
static PyObject *decimal_constructor;

static PyObject *PLyObject_FromJsonbContainer(JsonbContainer *jsonb);
static JsonbValue *PLyObject_ToJsonbValue(PyObject *obj,
										  JsonbParseState **jsonb_state, bool is_elem);

#if PY_MAJOR_VERSION >= 3
typedef PyObject *(*PLyUnicode_FromStringAndSize_t)
			(const char *s, Py_ssize_t size);
static PLyUnicode_FromStringAndSize_t PLyUnicode_FromStringAndSize_p;
#endif

/*
 * Module initialize function: fetch function pointers for cross-module calls.
 */
void
_PG_init(void)
{
	/* Asserts verify that typedefs above match original declarations */
	AssertVariableIsOfType(&PLyObject_AsString, PLyObject_AsString_t);
	PLyObject_AsString_p = (PLyObject_AsString_t)
		load_external_function("$libdir/" PLPYTHON_LIBNAME, "PLyObject_AsString",
							   true, NULL);
#if PY_MAJOR_VERSION >= 3
	AssertVariableIsOfType(&PLyUnicode_FromStringAndSize, PLyUnicode_FromStringAndSize_t);
	PLyUnicode_FromStringAndSize_p = (PLyUnicode_FromStringAndSize_t)
		load_external_function("$libdir/" PLPYTHON_LIBNAME, "PLyUnicode_FromStringAndSize",
							   true, NULL);
#endif

	AssertVariableIsOfType(&PLy_elog_impl, PLy_elog_impl_t);
	PLy_elog_impl_p = (PLy_elog_impl_t)
		load_external_function("$libdir/" PLPYTHON_LIBNAME, "PLy_elog_impl",
							   true, NULL);
}

/* These defines must be after the _PG_init */
#define PLyObject_AsString (PLyObject_AsString_p)
#define PLyUnicode_FromStringAndSize (PLyUnicode_FromStringAndSize_p)
#undef PLy_elog
#define PLy_elog (PLy_elog_impl_p)

/*
 * PLyString_FromJsonbValue
 *
 * Transform string JsonbValue to Python string.
 */
static PyObject *
PLyString_FromJsonbValue(JsonbValue *jbv)
{
	Assert(jbv->type == jbvString);

	return PyString_FromStringAndSize(jbv->val.string.val, jbv->val.string.len);
}

/*
 * PLyString_ToJsonbValue
 *
 * Transform Python string to JsonbValue.
 */
static void
PLyString_ToJsonbValue(PyObject *obj, JsonbValue *jbvElem)
{
	jbvElem->type = jbvString;
	jbvElem->val.string.val = PLyObject_AsString(obj);
	jbvElem->val.string.len = strlen(jbvElem->val.string.val);
}

/*
 * PLyObject_FromJsonbValue
 *
 * Transform JsonbValue to PyObject.
 */
static PyObject *
PLyObject_FromJsonbValue(JsonbValue *jsonbValue)
{
	switch (jsonbValue->type)
	{
		case jbvNull:
			Py_RETURN_NONE;

		case jbvBinary:
			return PLyObject_FromJsonbContainer(jsonbValue->val.binary.data);

		case jbvNumeric:
			{
				Datum		num;
				char	   *str;

				num = NumericGetDatum(jsonbValue->val.numeric);
				str = DatumGetCString(DirectFunctionCall1(numeric_out, num));

				return PyObject_CallFunction(decimal_constructor, "s", str);
			}

		case jbvString:
			return PLyString_FromJsonbValue(jsonbValue);

		case jbvBool:
			if (jsonbValue->val.boolean)
				Py_RETURN_TRUE;
			else
				Py_RETURN_FALSE;

		default:
			elog(ERROR, "unexpected jsonb value type: %d", jsonbValue->type);
			return NULL;
	}
}

/*
 * PLyObject_FromJsonbContainer
 *
 * Transform JsonbContainer to PyObject.
 */
static PyObject *
PLyObject_FromJsonbContainer(JsonbContainer *jsonb)
{
	JsonbIteratorToken r;
	JsonbValue	v;
	JsonbIterator *it;
	PyObject   *result;

	it = JsonbIteratorInit(jsonb);
	r = JsonbIteratorNext(&it, &v, true);

	switch (r)
	{
		case WJB_BEGIN_ARRAY:
			if (v.val.array.rawScalar)
			{
				JsonbValue	tmp;

				if ((r = JsonbIteratorNext(&it, &v, true)) != WJB_ELEM ||
					(r = JsonbIteratorNext(&it, &tmp, true)) != WJB_END_ARRAY ||
					(r = JsonbIteratorNext(&it, &tmp, true)) != WJB_DONE)
					elog(ERROR, "unexpected jsonb token: %d", r);

				result = PLyObject_FromJsonbValue(&v);
			}
			else
			{
				PyObject   *volatile elem = NULL;

				result = PyList_New(0);
				if (!result)
					return NULL;

				PG_TRY();
				{
					while ((r = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
					{
						if (r != WJB_ELEM)
							continue;

						elem = PLyObject_FromJsonbValue(&v);

						PyList_Append(result, elem);
						Py_XDECREF(elem);
						elem = NULL;
					}
				}
				PG_CATCH();
				{
					Py_XDECREF(elem);
					Py_XDECREF(result);
					PG_RE_THROW();
				}
				PG_END_TRY();
			}
			break;

		case WJB_BEGIN_OBJECT:
			{
				PyObject   *volatile result_v = PyDict_New();
				PyObject   *volatile key = NULL;
				PyObject   *volatile val = NULL;

				if (!result_v)
					return NULL;

				PG_TRY();
				{
					while ((r = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
					{
						if (r != WJB_KEY)
							continue;

						key = PLyString_FromJsonbValue(&v);
						if (!key)
						{
							Py_XDECREF(result_v);
							result_v = NULL;
							break;
						}

						if ((r = JsonbIteratorNext(&it, &v, true)) != WJB_VALUE)
							elog(ERROR, "unexpected jsonb token: %d", r);

						val = PLyObject_FromJsonbValue(&v);
						if (!val)
						{
							Py_XDECREF(key);
							key = NULL;
							Py_XDECREF(result_v);
							result_v = NULL;
							break;
						}

						PyDict_SetItem(result_v, key, val);

						Py_XDECREF(key);
						key = NULL;
						Py_XDECREF(val);
						val = NULL;
					}
				}
				PG_CATCH();
				{
					Py_XDECREF(result_v);
					Py_XDECREF(key);
					Py_XDECREF(val);
					PG_RE_THROW();
				}
				PG_END_TRY();

				result = result_v;
			}
			break;

		default:
			elog(ERROR, "unexpected jsonb token: %d", r);
			return NULL;
	}

	return result;
}

/*
 * PLyMapping_ToJsonbValue
 *
 * Transform Python dict to JsonbValue.
 */
static JsonbValue *
PLyMapping_ToJsonbValue(PyObject *obj, JsonbParseState **jsonb_state)
{
	Py_ssize_t	pcount;
	PyObject   *volatile items;
	JsonbValue *volatile out;

	pcount = PyMapping_Size(obj);
	items = PyMapping_Items(obj);

	PG_TRY();
	{
		Py_ssize_t	i;

		pushJsonbValue(jsonb_state, WJB_BEGIN_OBJECT, NULL);

		for (i = 0; i < pcount; i++)
		{
			JsonbValue	jbvKey;
			PyObject   *item = PyList_GetItem(items, i);
			PyObject   *key = PyTuple_GetItem(item, 0);
			PyObject   *value = PyTuple_GetItem(item, 1);

			/* Python dictionary can have None as key */
			if (key == Py_None)
			{
				jbvKey.type = jbvString;
				jbvKey.val.string.len = 0;
				jbvKey.val.string.val = "";
			}
			else
			{
				/* All others types of keys we serialize to string */
				PLyString_ToJsonbValue(key, &jbvKey);
			}

			(void) pushJsonbValue(jsonb_state, WJB_KEY, &jbvKey);
			(void) PLyObject_ToJsonbValue(value, jsonb_state, false);
		}

		out = pushJsonbValue(jsonb_state, WJB_END_OBJECT, NULL);
	}
	PG_FINALLY();
	{
		Py_DECREF(items);
	}
	PG_END_TRY();

	return out;
}

/*
 * PLySequence_ToJsonbValue
 *
 * Transform python list to JsonbValue. Expects transformed PyObject and
 * a state required for jsonb construction.
 */
static JsonbValue *
PLySequence_ToJsonbValue(PyObject *obj, JsonbParseState **jsonb_state)
{
	Py_ssize_t	i;
	Py_ssize_t	pcount;
	PyObject   *volatile value = NULL;

	pcount = PySequence_Size(obj);

	pushJsonbValue(jsonb_state, WJB_BEGIN_ARRAY, NULL);

	PG_TRY();
	{
		for (i = 0; i < pcount; i++)
		{
			value = PySequence_GetItem(obj, i);
			Assert(value);

			(void) PLyObject_ToJsonbValue(value, jsonb_state, true);
			Py_XDECREF(value);
			value = NULL;
		}
	}
	PG_CATCH();
	{
		Py_XDECREF(value);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return pushJsonbValue(jsonb_state, WJB_END_ARRAY, NULL);
}

/*
 * PLyNumber_ToJsonbValue(PyObject *obj)
 *
 * Transform python number to JsonbValue.
 */
static JsonbValue *
PLyNumber_ToJsonbValue(PyObject *obj, JsonbValue *jbvNum)
{
	Numeric		num;
	char	   *str = PLyObject_AsString(obj);

	PG_TRY();
	{
		Datum		numd;

		numd = DirectFunctionCall3(numeric_in,
								   CStringGetDatum(str),
								   ObjectIdGetDatum(InvalidOid),
								   Int32GetDatum(-1));
		num = DatumGetNumeric(numd);
	}
	PG_CATCH();
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("could not convert value \"%s\" to jsonb", str)));
	}
	PG_END_TRY();

	pfree(str);

	/*
	 * jsonb doesn't allow NaN (per JSON specification), so we have to prevent
	 * it here explicitly.  (Infinity is also not allowed in jsonb, but
	 * numeric_in above already catches that.)
	 */
	if (numeric_is_nan(num))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("cannot convert NaN to jsonb")));

	jbvNum->type = jbvNumeric;
	jbvNum->val.numeric = num;

	return jbvNum;
}

/*
 * PLyObject_ToJsonbValue(PyObject *obj)
 *
 * Transform python object to JsonbValue.
 */
static JsonbValue *
PLyObject_ToJsonbValue(PyObject *obj, JsonbParseState **jsonb_state, bool is_elem)
{
	JsonbValue *out;

	if (!(PyString_Check(obj) || PyUnicode_Check(obj)))
	{
		if (PySequence_Check(obj))
			return PLySequence_ToJsonbValue(obj, jsonb_state);
		else if (PyMapping_Check(obj))
			return PLyMapping_ToJsonbValue(obj, jsonb_state);
	}

	out = palloc(sizeof(JsonbValue));

	if (obj == Py_None)
		out->type = jbvNull;
	else if (PyString_Check(obj) || PyUnicode_Check(obj))
		PLyString_ToJsonbValue(obj, out);

	/*
	 * PyNumber_Check() returns true for booleans, so boolean check should
	 * come first.
	 */
	else if (PyBool_Check(obj))
	{
		out->type = jbvBool;
		out->val.boolean = (obj == Py_True);
	}
	else if (PyNumber_Check(obj))
		out = PLyNumber_ToJsonbValue(obj, out);
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Python type \"%s\" cannot be transformed to jsonb",
						PLyObject_AsString((PyObject *) obj->ob_type))));

	/* Push result into 'jsonb_state' unless it is raw scalar value. */
	return (*jsonb_state ?
			pushJsonbValue(jsonb_state, is_elem ? WJB_ELEM : WJB_VALUE, out) :
			out);
}

/*
 * plpython_to_jsonb
 *
 * Transform python object to Jsonb datum
 */
PG_FUNCTION_INFO_V1(plpython_to_jsonb);
Datum
plpython_to_jsonb(PG_FUNCTION_ARGS)
{
	PyObject   *obj;
	JsonbValue *out;
	JsonbParseState *jsonb_state = NULL;

	obj = (PyObject *) PG_GETARG_POINTER(0);
	out = PLyObject_ToJsonbValue(obj, &jsonb_state, true);
	PG_RETURN_POINTER(JsonbValueToJsonb(out));
}

/*
 * jsonb_to_plpython
 *
 * Transform Jsonb datum to PyObject and return it as internal.
 */
PG_FUNCTION_INFO_V1(jsonb_to_plpython);
Datum
jsonb_to_plpython(PG_FUNCTION_ARGS)
{
	PyObject   *result;
	Jsonb	   *in = PG_GETARG_JSONB_P(0);

	/*
	 * Initialize pointer to Decimal constructor. First we try "cdecimal", C
	 * version of decimal library. In case of failure we use slower "decimal"
	 * module.
	 */
	if (!decimal_constructor)
	{
		PyObject   *decimal_module = PyImport_ImportModule("cdecimal");

		if (!decimal_module)
		{
			PyErr_Clear();
			decimal_module = PyImport_ImportModule("decimal");
		}
		Assert(decimal_module);
		decimal_constructor = PyObject_GetAttrString(decimal_module, "Decimal");
	}

	result = PLyObject_FromJsonbContainer(&in->root);
	if (!result)
		PLy_elog(ERROR, "transformation from jsonb to Python failed");

	return PointerGetDatum(result);
}
