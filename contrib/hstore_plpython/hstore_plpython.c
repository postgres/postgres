#include "postgres.h"
#include "fmgr.h"
#include "plpython.h"
#include "plpy_typeio.h"
#include "hstore.h"

PG_MODULE_MAGIC;


PG_FUNCTION_INFO_V1(hstore_to_plpython);

Datum
hstore_to_plpython(PG_FUNCTION_ARGS)
{
	HStore	   *in = PG_GETARG_HS(0);
	int			i;
	int			count = HS_COUNT(in);
	char	   *base = STRPTR(in);
	HEntry	   *entries = ARRPTR(in);
	PyObject   *dict;

	dict = PyDict_New();

	for (i = 0; i < count; i++)
	{
		PyObject   *key;

		key = PyString_FromStringAndSize(HSTORE_KEY(entries, base, i),
										 HSTORE_KEYLEN(entries, i));
		if (HSTORE_VALISNULL(entries, i))
			PyDict_SetItem(dict, key, Py_None);
		else
		{
			PyObject   *value;

			value = PyString_FromStringAndSize(HSTORE_VAL(entries, base, i),
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
	volatile PyObject *items_v = NULL;
	int32		pcount;
	HStore	   *out;

	dict = (PyObject *) PG_GETARG_POINTER(0);
	if (!PyMapping_Check(dict))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("not a Python mapping")));

	pcount = PyMapping_Size(dict);
	items_v = PyMapping_Items(dict);

	PG_TRY();
	{
		int32		buflen;
		int32		i;
		Pairs	   *pairs;
		PyObject   *items = (PyObject *) items_v;

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
		Py_DECREF(items_v);

		pcount = hstoreUniquePairs(pairs, pcount, &buflen);
		out = hstorePairs(pairs, pcount, buflen);
	}
	PG_CATCH();
	{
		Py_DECREF(items_v);
		PG_RE_THROW();
	}
	PG_END_TRY();

	PG_RETURN_POINTER(out);
}
