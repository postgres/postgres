/*
 * utility functions
 *
 * src/pl/plpython/plpy_util.c
 */

#include "postgres.h"

#include "mb/pg_wchar.h"
#include "plpy_elog.h"
#include "plpy_util.h"
#include "plpython.h"
#include "utils/memutils.h"

/*
 * Convert a Python unicode object to a Python string/bytes object in
 * PostgreSQL server encoding.  Reference ownership is passed to the
 * caller.
 */
PyObject *
PLyUnicode_Bytes(PyObject *unicode)
{
	PyObject   *bytes,
			   *rv;
	char	   *utf8string,
			   *encoded;

	/* First encode the Python unicode object with UTF-8. */
	bytes = PyUnicode_AsUTF8String(unicode);
	if (bytes == NULL)
		PLy_elog(ERROR, "could not convert Python Unicode object to bytes");

	utf8string = PyBytes_AsString(bytes);
	if (utf8string == NULL)
	{
		Py_DECREF(bytes);
		PLy_elog(ERROR, "could not extract bytes from encoded string");
	}

	/*
	 * Then convert to server encoding if necessary.
	 *
	 * PyUnicode_AsEncodedString could be used to encode the object directly
	 * in the server encoding, but Python doesn't support all the encodings
	 * that PostgreSQL does (EUC_TW and MULE_INTERNAL). UTF-8 is used as an
	 * intermediary in PLyUnicode_FromString as well.
	 */
	if (GetDatabaseEncoding() != PG_UTF8)
	{
		PG_TRY();
		{
			encoded = pg_any_to_server(utf8string,
									   strlen(utf8string),
									   PG_UTF8);
		}
		PG_CATCH();
		{
			Py_DECREF(bytes);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	else
		encoded = utf8string;

	/* finally, build a bytes object in the server encoding */
	rv = PyBytes_FromStringAndSize(encoded, strlen(encoded));

	/* if pg_any_to_server allocated memory, free it now */
	if (utf8string != encoded)
		pfree(encoded);

	Py_DECREF(bytes);
	return rv;
}

/*
 * Convert a Python unicode object to a C string in PostgreSQL server
 * encoding.  No Python object reference is passed out of this
 * function.  The result is palloc'ed.
 */
char *
PLyUnicode_AsString(PyObject *unicode)
{
	PyObject   *o = PLyUnicode_Bytes(unicode);
	char	   *rv = pstrdup(PyBytes_AsString(o));

	Py_XDECREF(o);
	return rv;
}

/*
 * Convert a C string in the PostgreSQL server encoding to a Python
 * unicode object.  Reference ownership is passed to the caller.
 */
PyObject *
PLyUnicode_FromStringAndSize(const char *s, Py_ssize_t size)
{
	char	   *utf8string;
	PyObject   *o;

	utf8string = pg_server_to_any(s, size, PG_UTF8);

	if (utf8string == s)
	{
		o = PyUnicode_FromStringAndSize(s, size);
	}
	else
	{
		o = PyUnicode_FromString(utf8string);
		pfree(utf8string);
	}

	return o;
}

PyObject *
PLyUnicode_FromString(const char *s)
{
	return PLyUnicode_FromStringAndSize(s, strlen(s));
}
