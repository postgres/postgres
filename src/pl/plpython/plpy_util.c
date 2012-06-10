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
 * PostgreSQL server encoding.	Reference ownership is passed to the
 * caller.
 */
PyObject *
PLyUnicode_Bytes(PyObject *unicode)
{
	PyObject   *rv;
	const char *serverenc;

	/*
	 * Python understands almost all PostgreSQL encoding names, but it doesn't
	 * know SQL_ASCII.
	 */
	if (GetDatabaseEncoding() == PG_SQL_ASCII)
		serverenc = "ascii";
	else
		serverenc = GetDatabaseEncodingName();
	rv = PyUnicode_AsEncodedString(unicode, serverenc, "strict");
	if (rv == NULL)
		PLy_elog(ERROR, "could not convert Python Unicode object to PostgreSQL server encoding");
	return rv;
}

/*
 * Convert a Python unicode object to a C string in PostgreSQL server
 * encoding.  No Python object reference is passed out of this
 * function.  The result is palloc'ed.
 *
 * Note that this function is disguised as PyString_AsString() when
 * using Python 3.	That function retuns a pointer into the internal
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
 * unicode object.	Reference ownership is passed to the caller.
 */
PyObject *
PLyUnicode_FromString(const char *s)
{
	char	   *utf8string;
	PyObject   *o;

	utf8string = (char *) pg_do_encoding_conversion((unsigned char *) s,
													strlen(s),
													GetDatabaseEncoding(),
													PG_UTF8);

	o = PyUnicode_FromString(utf8string);

	if (utf8string != s)
		pfree(utf8string);

	return o;
}

#endif   /* PY_MAJOR_VERSION >= 3 */
