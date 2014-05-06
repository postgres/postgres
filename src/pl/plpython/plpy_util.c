/*
 * utility functions
 *
 * src/pl/plpython/plpy_util.c
 */

#include "postgres.h"

#include "mb/pg_wchar.h"
#include "utils/memutils.h"
#include "utils/palloc.h"

#include "plpython.h"

#include "plpy_util.h"

#include "plpy_elog.h"


void *
PLy_malloc(size_t bytes)
{
	/* We need our allocations to be long-lived, so use TopMemoryContext */
	return MemoryContextAlloc(TopMemoryContext, bytes);
}

void *
PLy_malloc0(size_t bytes)
{
	void	   *ptr = PLy_malloc(bytes);

	MemSet(ptr, 0, bytes);
	return ptr;
}

char *
PLy_strdup(const char *str)
{
	char	   *result;
	size_t		len;

	len = strlen(str) + 1;
	result = PLy_malloc(len);
	memcpy(result, str, len);

	return result;
}

/* define this away */
void
PLy_free(void *ptr)
{
	pfree(ptr);
}

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
 *
 * Note that this function is disguised as PyString_AsString() when
 * using Python 3.  That function retuns a pointer into the internal
 * memory of the argument, which isn't exactly the interface of this
 * function.  But in either case you get a rather short-lived
 * reference that you ought to better leave alone.
 */
char *
PLyUnicode_AsString(PyObject *unicode)
{
	PyObject   *o = PLyUnicode_Bytes(unicode);
	char	   *rv = pstrdup(PyBytes_AsString(o));

	Py_XDECREF(o);
	return rv;
}

#if PY_MAJOR_VERSION >= 3
/*
 * Convert a C string in the PostgreSQL server encoding to a Python
 * unicode object.  Reference ownership is passed to the caller.
 */
PyObject *
PLyUnicode_FromString(const char *s)
{
	char	   *utf8string;
	PyObject   *o;

	utf8string = pg_server_to_any(s, strlen(s), PG_UTF8);

	o = PyUnicode_FromString(utf8string);

	if (utf8string != s)
		pfree(utf8string);

	return o;
}

#endif   /* PY_MAJOR_VERSION >= 3 */
