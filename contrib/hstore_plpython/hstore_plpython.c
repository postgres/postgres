#include "postgres.h"

#include "fmgr.h"
#include "hstore/hstore.h"
#include "plpy_typeio.h"
#include "plpython.h"

PG_MODULE_MAGIC_EXT(
					.name = "hstore_plpython",
					.version = PG_VERSION
);

/* Linkage to functions in plpython module */
typedef char *(*PLyObject_AsString_t) (PyObject *plrv);
static PLyObject_AsString_t PLyObject_AsString_p;
typedef PyObject *(*PLyUnicode_FromStringAndSize_t) (const char *s, Py_ssize_t size);
static PLyUnicode_FromStringAndSize_t PLyUnicode_FromStringAndSize_p;

/* Linkage to functions in hstore module */
typedef HStore *(*hstoreUpgrade_t) (Datum orig);
static hstoreUpgrade_t hstoreUpgrade_p;
typedef int (*hstoreUniquePairs_t) (Pairs *a, int32 l, int32 *buflen);
static hstoreUniquePairs_t hstoreUniquePairs_p;
typedef HStore *(*hstorePairs_t) (Pairs *pairs, int32 pcount, int32 buflen);
static hstorePairs_t hstorePairs_p;
typedef size_t (*hstoreCheckKeyLen_t) (size_t len);
static hstoreCheckKeyLen_t hstoreCheckKeyLen_p;
typedef size_t (*hstoreCheckValLen_t) (size_t len);
static hstoreCheckValLen_t hstoreCheckValLen_p;


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
	AssertVariableIsOfType(&PLyUnicode_FromStringAndSize, PLyUnicode_FromStringAndSize_t);
	PLyUnicode_FromStringAndSize_p = (PLyUnicode_FromStringAndSize_t)
		load_external_function("$libdir/" PLPYTHON_LIBNAME, "PLyUnicode_FromStringAndSize",
							   true, NULL);
	AssertVariableIsOfType(&hstoreUpgrade, hstoreUpgrade_t);
	hstoreUpgrade_p = (hstoreUpgrade_t)
		load_external_function("$libdir/hstore", "hstoreUpgrade",
							   true, NULL);
	AssertVariableIsOfType(&hstoreUniquePairs, hstoreUniquePairs_t);
	hstoreUniquePairs_p = (hstoreUniquePairs_t)
		load_external_function("$libdir/hstore", "hstoreUniquePairs",
							   true, NULL);
	AssertVariableIsOfType(&hstorePairs, hstorePairs_t);
	hstorePairs_p = (hstorePairs_t)
		load_external_function("$libdir/hstore", "hstorePairs",
							   true, NULL);
	AssertVariableIsOfType(&hstoreCheckKeyLen, hstoreCheckKeyLen_t);
	hstoreCheckKeyLen_p = (hstoreCheckKeyLen_t)
		load_external_function("$libdir/hstore", "hstoreCheckKeyLen",
							   true, NULL);
	AssertVariableIsOfType(&hstoreCheckValLen, hstoreCheckValLen_t);
	hstoreCheckValLen_p = (hstoreCheckValLen_t)
		load_external_function("$libdir/hstore", "hstoreCheckValLen",
							   true, NULL);
}


/* These defines must be after the module init function */
#define PLyObject_AsString PLyObject_AsString_p
#define PLyUnicode_FromStringAndSize PLyUnicode_FromStringAndSize_p
#define hstoreUpgrade hstoreUpgrade_p
#define hstoreUniquePairs hstoreUniquePairs_p
#define hstorePairs hstorePairs_p
#define hstoreCheckKeyLen hstoreCheckKeyLen_p
#define hstoreCheckValLen hstoreCheckValLen_p


PG_FUNCTION_INFO_V1(hstore_to_plpython);

Datum
hstore_to_plpython(PG_FUNCTION_ARGS)
{
	HStore	   *in = PG_GETARG_HSTORE_P(0);
	int			i;
	int			count = HS_COUNT(in);
	char	   *base = STRPTR(in);
	HEntry	   *entries = ARRPTR(in);
	PyObject   *dict;

	dict = PyDict_New();
	if (!dict)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	for (i = 0; i < count; i++)
	{
		PyObject   *key;

		key = PLyUnicode_FromStringAndSize(HSTORE_KEY(entries, base, i),
										   HSTORE_KEYLEN(entries, i));
		if (HSTORE_VALISNULL(entries, i))
			PyDict_SetItem(dict, key, Py_None);
		else
		{
			PyObject   *value;

			value = PLyUnicode_FromStringAndSize(HSTORE_VAL(entries, base, i),
												 HSTORE_VALLEN(entries, i));
			PyDict_SetItem(dict, key, value);
			Py_XDECREF(value);
		}
		Py_XDECREF(key);
	}

	return PointerGetDatum(dict);
}


PG_FUNCTION_INFO_V1(plpython_to_hstore);

Datum
plpython_to_hstore(PG_FUNCTION_ARGS)
{
	PyObject   *dict;
	PyObject   *volatile items;
	Py_ssize_t	pcount;
	HStore	   *volatile out;

	dict = (PyObject *) PG_GETARG_POINTER(0);

	/*
	 * As of Python 3, PyMapping_Check() is unreliable unless one first checks
	 * that the object isn't a sequence.  (Cleaner solutions exist, but not
	 * before Python 3.10, which we're not prepared to require yet.)
	 */
	if (PySequence_Check(dict) || !PyMapping_Check(dict))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("not a Python mapping")));

	pcount = PyMapping_Size(dict);
	items = PyMapping_Items(dict);

	PG_TRY();
	{
		int32		buflen;
		Py_ssize_t	i;
		Pairs	   *pairs;

		pairs = palloc(pcount * sizeof(*pairs));

		for (i = 0; i < pcount; i++)
		{
			PyObject   *tuple;
			PyObject   *key;
			PyObject   *value;

			tuple = PyList_GetItem(items, i);
			key = PyTuple_GetItem(tuple, 0);
			value = PyTuple_GetItem(tuple, 1);

			pairs[i].key = PLyObject_AsString(key);
			pairs[i].keylen = hstoreCheckKeyLen(strlen(pairs[i].key));
			pairs[i].needfree = true;

			if (value == Py_None)
			{
				pairs[i].val = NULL;
				pairs[i].vallen = 0;
				pairs[i].isnull = true;
			}
			else
			{
				pairs[i].val = PLyObject_AsString(value);
				pairs[i].vallen = hstoreCheckValLen(strlen(pairs[i].val));
				pairs[i].isnull = false;
			}
		}

		pcount = hstoreUniquePairs(pairs, pcount, &buflen);
		out = hstorePairs(pairs, pcount, buflen);
	}
	PG_FINALLY();
	{
		Py_DECREF(items);
	}
	PG_END_TRY();

	PG_RETURN_POINTER(out);
}
