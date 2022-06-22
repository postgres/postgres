/*
 * the plpy module
 *
 * src/pl/plpython/plpy_plpymodule.c
 */

#include "postgres.h"

#include "access/xact.h"
#include "mb/pg_wchar.h"
#include "plpy_cursorobject.h"
#include "plpy_elog.h"
#include "plpy_main.h"
#include "plpy_planobject.h"
#include "plpy_plpymodule.h"
#include "plpy_resultobject.h"
#include "plpy_spi.h"
#include "plpy_subxactobject.h"
#include "plpython.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"

HTAB	   *PLy_spi_exceptions = NULL;


static void PLy_add_exceptions(PyObject *plpy);
static PyObject *PLy_create_exception(char *name,
									  PyObject *base, PyObject *dict,
									  const char *modname, PyObject *mod);
static void PLy_generate_spi_exceptions(PyObject *mod, PyObject *base);

/* module functions */
static PyObject *PLy_debug(PyObject *self, PyObject *args, PyObject *kw);
static PyObject *PLy_log(PyObject *self, PyObject *args, PyObject *kw);
static PyObject *PLy_info(PyObject *self, PyObject *args, PyObject *kw);
static PyObject *PLy_notice(PyObject *self, PyObject *args, PyObject *kw);
static PyObject *PLy_warning(PyObject *self, PyObject *args, PyObject *kw);
static PyObject *PLy_error(PyObject *self, PyObject *args, PyObject *kw);
static PyObject *PLy_fatal(PyObject *self, PyObject *args, PyObject *kw);
static PyObject *PLy_quote_literal(PyObject *self, PyObject *args);
static PyObject *PLy_quote_nullable(PyObject *self, PyObject *args);
static PyObject *PLy_quote_ident(PyObject *self, PyObject *args);


/* A list of all known exceptions, generated from backend/utils/errcodes.txt */
typedef struct ExceptionMap
{
	char	   *name;
	char	   *classname;
	int			sqlstate;
} ExceptionMap;

static const ExceptionMap exception_map[] = {
#include "spiexceptions.h"
	{NULL, NULL, 0}
};

static PyMethodDef PLy_methods[] = {
	/*
	 * logging methods
	 */
	{"debug", (PyCFunction) PLy_debug, METH_VARARGS | METH_KEYWORDS, NULL},
	{"log", (PyCFunction) PLy_log, METH_VARARGS | METH_KEYWORDS, NULL},
	{"info", (PyCFunction) PLy_info, METH_VARARGS | METH_KEYWORDS, NULL},
	{"notice", (PyCFunction) PLy_notice, METH_VARARGS | METH_KEYWORDS, NULL},
	{"warning", (PyCFunction) PLy_warning, METH_VARARGS | METH_KEYWORDS, NULL},
	{"error", (PyCFunction) PLy_error, METH_VARARGS | METH_KEYWORDS, NULL},
	{"fatal", (PyCFunction) PLy_fatal, METH_VARARGS | METH_KEYWORDS, NULL},

	/*
	 * create a stored plan
	 */
	{"prepare", PLy_spi_prepare, METH_VARARGS, NULL},

	/*
	 * execute a plan or query
	 */
	{"execute", PLy_spi_execute, METH_VARARGS, NULL},

	/*
	 * escaping strings
	 */
	{"quote_literal", PLy_quote_literal, METH_VARARGS, NULL},
	{"quote_nullable", PLy_quote_nullable, METH_VARARGS, NULL},
	{"quote_ident", PLy_quote_ident, METH_VARARGS, NULL},

	/*
	 * create the subtransaction context manager
	 */
	{"subtransaction", PLy_subtransaction_new, METH_NOARGS, NULL},

	/*
	 * create a cursor
	 */
	{"cursor", PLy_cursor, METH_VARARGS, NULL},

	/*
	 * transaction control
	 */
	{"commit", PLy_commit, METH_NOARGS, NULL},
	{"rollback", PLy_rollback, METH_NOARGS, NULL},

	{NULL, NULL, 0, NULL}
};

static PyMethodDef PLy_exc_methods[] = {
	{NULL, NULL, 0, NULL}
};

#if PY_MAJOR_VERSION >= 3
static PyModuleDef PLy_module = {
	PyModuleDef_HEAD_INIT,
	.m_name = "plpy",
	.m_size = -1,
	.m_methods = PLy_methods,
};

static PyModuleDef PLy_exc_module = {
	PyModuleDef_HEAD_INIT,
	.m_name = "spiexceptions",
	.m_size = -1,
	.m_methods = PLy_exc_methods,
};

/*
 * Must have external linkage, because PyMODINIT_FUNC does dllexport on
 * Windows-like platforms.
 */
PyMODINIT_FUNC
PyInit_plpy(void)
{
	PyObject   *m;

	m = PyModule_Create(&PLy_module);
	if (m == NULL)
		return NULL;

	PLy_add_exceptions(m);

	return m;
}
#endif							/* PY_MAJOR_VERSION >= 3 */

void
PLy_init_plpy(void)
{
	PyObject   *main_mod,
			   *main_dict,
			   *plpy_mod;

#if PY_MAJOR_VERSION < 3
	PyObject   *plpy;
#endif

	/*
	 * initialize plpy module
	 */
	PLy_plan_init_type();
	PLy_result_init_type();
	PLy_subtransaction_init_type();
	PLy_cursor_init_type();

#if PY_MAJOR_VERSION >= 3
	PyModule_Create(&PLy_module);
	/* for Python 3 we initialized the exceptions in PyInit_plpy */
#else
	plpy = Py_InitModule("plpy", PLy_methods);
	PLy_add_exceptions(plpy);
#endif

	/* PyDict_SetItemString(plpy, "PlanType", (PyObject *) &PLy_PlanType); */

	/*
	 * initialize main module, and add plpy
	 */
	main_mod = PyImport_AddModule("__main__");
	main_dict = PyModule_GetDict(main_mod);
	plpy_mod = PyImport_AddModule("plpy");
	if (plpy_mod == NULL)
		PLy_elog(ERROR, "could not import \"plpy\" module");
	PyDict_SetItemString(main_dict, "plpy", plpy_mod);
	if (PyErr_Occurred())
		PLy_elog(ERROR, "could not import \"plpy\" module");
}

static void
PLy_add_exceptions(PyObject *plpy)
{
	PyObject   *excmod;
	HASHCTL		hash_ctl;

#if PY_MAJOR_VERSION < 3
	excmod = Py_InitModule("spiexceptions", PLy_exc_methods);
#else
	excmod = PyModule_Create(&PLy_exc_module);
#endif
	if (excmod == NULL)
		PLy_elog(ERROR, "could not create the spiexceptions module");

	/*
	 * PyModule_AddObject does not add a refcount to the object, for some odd
	 * reason; we must do that.
	 */
	Py_INCREF(excmod);
	if (PyModule_AddObject(plpy, "spiexceptions", excmod) < 0)
		PLy_elog(ERROR, "could not add the spiexceptions module");

	PLy_exc_error = PLy_create_exception("plpy.Error", NULL, NULL,
										 "Error", plpy);
	PLy_exc_fatal = PLy_create_exception("plpy.Fatal", NULL, NULL,
										 "Fatal", plpy);
	PLy_exc_spi_error = PLy_create_exception("plpy.SPIError", NULL, NULL,
											 "SPIError", plpy);

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(int);
	hash_ctl.entrysize = sizeof(PLyExceptionEntry);
	PLy_spi_exceptions = hash_create("PL/Python SPI exceptions", 256,
									 &hash_ctl, HASH_ELEM | HASH_BLOBS);

	PLy_generate_spi_exceptions(excmod, PLy_exc_spi_error);
}

/*
 * Create an exception object and add it to the module
 */
static PyObject *
PLy_create_exception(char *name, PyObject *base, PyObject *dict,
					 const char *modname, PyObject *mod)
{
	PyObject   *exc;

	exc = PyErr_NewException(name, base, dict);
	if (exc == NULL)
		PLy_elog(ERROR, NULL);

	/*
	 * PyModule_AddObject does not add a refcount to the object, for some odd
	 * reason; we must do that.
	 */
	Py_INCREF(exc);
	PyModule_AddObject(mod, modname, exc);

	/*
	 * The caller will also store a pointer to the exception object in some
	 * permanent variable, so add another ref to account for that.  This is
	 * probably excessively paranoid, but let's be sure.
	 */
	Py_INCREF(exc);
	return exc;
}

/*
 * Add all the autogenerated exceptions as subclasses of SPIError
 */
static void
PLy_generate_spi_exceptions(PyObject *mod, PyObject *base)
{
	int			i;

	for (i = 0; exception_map[i].name != NULL; i++)
	{
		bool		found;
		PyObject   *exc;
		PLyExceptionEntry *entry;
		PyObject   *sqlstate;
		PyObject   *dict = PyDict_New();

		if (dict == NULL)
			PLy_elog(ERROR, NULL);

		sqlstate = PyString_FromString(unpack_sql_state(exception_map[i].sqlstate));
		if (sqlstate == NULL)
			PLy_elog(ERROR, "could not generate SPI exceptions");

		PyDict_SetItemString(dict, "sqlstate", sqlstate);
		Py_DECREF(sqlstate);

		exc = PLy_create_exception(exception_map[i].name, base, dict,
								   exception_map[i].classname, mod);

		entry = hash_search(PLy_spi_exceptions, &exception_map[i].sqlstate,
							HASH_ENTER, &found);
		Assert(!found);
		entry->exc = exc;
	}
}


/*
 * the python interface to the elog function
 * don't confuse these with PLy_elog
 */
static PyObject *PLy_output(volatile int level, PyObject *self,
							PyObject *args, PyObject *kw);

static PyObject *
PLy_debug(PyObject *self, PyObject *args, PyObject *kw)
{
	return PLy_output(DEBUG2, self, args, kw);
}

static PyObject *
PLy_log(PyObject *self, PyObject *args, PyObject *kw)
{
	return PLy_output(LOG, self, args, kw);
}

static PyObject *
PLy_info(PyObject *self, PyObject *args, PyObject *kw)
{
	return PLy_output(INFO, self, args, kw);
}

static PyObject *
PLy_notice(PyObject *self, PyObject *args, PyObject *kw)
{
	return PLy_output(NOTICE, self, args, kw);
}

static PyObject *
PLy_warning(PyObject *self, PyObject *args, PyObject *kw)
{
	return PLy_output(WARNING, self, args, kw);
}

static PyObject *
PLy_error(PyObject *self, PyObject *args, PyObject *kw)
{
	return PLy_output(ERROR, self, args, kw);
}

static PyObject *
PLy_fatal(PyObject *self, PyObject *args, PyObject *kw)
{
	return PLy_output(FATAL, self, args, kw);
}

static PyObject *
PLy_quote_literal(PyObject *self, PyObject *args)
{
	const char *str;
	char	   *quoted;
	PyObject   *ret;

	if (!PyArg_ParseTuple(args, "s:quote_literal", &str))
		return NULL;

	quoted = quote_literal_cstr(str);
	ret = PyString_FromString(quoted);
	pfree(quoted);

	return ret;
}

static PyObject *
PLy_quote_nullable(PyObject *self, PyObject *args)
{
	const char *str;
	char	   *quoted;
	PyObject   *ret;

	if (!PyArg_ParseTuple(args, "z:quote_nullable", &str))
		return NULL;

	if (str == NULL)
		return PyString_FromString("NULL");

	quoted = quote_literal_cstr(str);
	ret = PyString_FromString(quoted);
	pfree(quoted);

	return ret;
}

static PyObject *
PLy_quote_ident(PyObject *self, PyObject *args)
{
	const char *str;
	const char *quoted;
	PyObject   *ret;

	if (!PyArg_ParseTuple(args, "s:quote_ident", &str))
		return NULL;

	quoted = quote_identifier(str);
	ret = PyString_FromString(quoted);

	return ret;
}

/* enforce cast of object to string */
static char *
object_to_string(PyObject *obj)
{
	if (obj)
	{
		PyObject   *so = PyObject_Str(obj);

		if (so != NULL)
		{
			char	   *str;

			str = pstrdup(PyString_AsString(so));
			Py_DECREF(so);

			return str;
		}
	}

	return NULL;
}

static PyObject *
PLy_output(volatile int level, PyObject *self, PyObject *args, PyObject *kw)
{
	int			sqlstate = 0;
	char	   *volatile sqlstatestr = NULL;
	char	   *volatile message = NULL;
	char	   *volatile detail = NULL;
	char	   *volatile hint = NULL;
	char	   *volatile column_name = NULL;
	char	   *volatile constraint_name = NULL;
	char	   *volatile datatype_name = NULL;
	char	   *volatile table_name = NULL;
	char	   *volatile schema_name = NULL;
	volatile MemoryContext oldcontext;
	PyObject   *key,
			   *value;
	PyObject   *volatile so;
	Py_ssize_t	pos = 0;

	if (PyTuple_Size(args) == 1)
	{
		/*
		 * Treat single argument specially to avoid undesirable ('tuple',)
		 * decoration.
		 */
		PyObject   *o;

		if (!PyArg_UnpackTuple(args, "plpy.elog", 1, 1, &o))
			PLy_elog(ERROR, "could not unpack arguments in plpy.elog");
		so = PyObject_Str(o);
	}
	else
		so = PyObject_Str(args);

	if (so == NULL || ((message = PyString_AsString(so)) == NULL))
	{
		level = ERROR;
		message = dgettext(TEXTDOMAIN, "could not parse error message in plpy.elog");
	}
	message = pstrdup(message);

	Py_XDECREF(so);

	if (kw != NULL)
	{
		while (PyDict_Next(kw, &pos, &key, &value))
		{
			char	   *keyword = PyString_AsString(key);

			if (strcmp(keyword, "message") == 0)
			{
				/* the message should not be overwritten */
				if (PyTuple_Size(args) != 0)
				{
					PLy_exception_set(PyExc_TypeError, "argument 'message' given by name and position");
					return NULL;
				}

				if (message)
					pfree(message);
				message = object_to_string(value);
			}
			else if (strcmp(keyword, "detail") == 0)
				detail = object_to_string(value);
			else if (strcmp(keyword, "hint") == 0)
				hint = object_to_string(value);
			else if (strcmp(keyword, "sqlstate") == 0)
				sqlstatestr = object_to_string(value);
			else if (strcmp(keyword, "schema_name") == 0)
				schema_name = object_to_string(value);
			else if (strcmp(keyword, "table_name") == 0)
				table_name = object_to_string(value);
			else if (strcmp(keyword, "column_name") == 0)
				column_name = object_to_string(value);
			else if (strcmp(keyword, "datatype_name") == 0)
				datatype_name = object_to_string(value);
			else if (strcmp(keyword, "constraint_name") == 0)
				constraint_name = object_to_string(value);
			else
			{
				PLy_exception_set(PyExc_TypeError,
								  "'%s' is an invalid keyword argument for this function",
								  keyword);
				return NULL;
			}
		}
	}

	if (sqlstatestr != NULL)
	{
		if (strlen(sqlstatestr) != 5)
		{
			PLy_exception_set(PyExc_ValueError, "invalid SQLSTATE code");
			return NULL;
		}

		if (strspn(sqlstatestr, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") != 5)
		{
			PLy_exception_set(PyExc_ValueError, "invalid SQLSTATE code");
			return NULL;
		}

		sqlstate = MAKE_SQLSTATE(sqlstatestr[0],
								 sqlstatestr[1],
								 sqlstatestr[2],
								 sqlstatestr[3],
								 sqlstatestr[4]);
	}

	oldcontext = CurrentMemoryContext;
	PG_TRY();
	{
		if (message != NULL)
			pg_verifymbstr(message, strlen(message), false);
		if (detail != NULL)
			pg_verifymbstr(detail, strlen(detail), false);
		if (hint != NULL)
			pg_verifymbstr(hint, strlen(hint), false);
		if (schema_name != NULL)
			pg_verifymbstr(schema_name, strlen(schema_name), false);
		if (table_name != NULL)
			pg_verifymbstr(table_name, strlen(table_name), false);
		if (column_name != NULL)
			pg_verifymbstr(column_name, strlen(column_name), false);
		if (datatype_name != NULL)
			pg_verifymbstr(datatype_name, strlen(datatype_name), false);
		if (constraint_name != NULL)
			pg_verifymbstr(constraint_name, strlen(constraint_name), false);

		ereport(level,
				((sqlstate != 0) ? errcode(sqlstate) : 0,
				 (message != NULL) ? errmsg_internal("%s", message) : 0,
				 (detail != NULL) ? errdetail_internal("%s", detail) : 0,
				 (hint != NULL) ? errhint("%s", hint) : 0,
				 (column_name != NULL) ?
				 err_generic_string(PG_DIAG_COLUMN_NAME, column_name) : 0,
				 (constraint_name != NULL) ?
				 err_generic_string(PG_DIAG_CONSTRAINT_NAME, constraint_name) : 0,
				 (datatype_name != NULL) ?
				 err_generic_string(PG_DIAG_DATATYPE_NAME, datatype_name) : 0,
				 (table_name != NULL) ?
				 err_generic_string(PG_DIAG_TABLE_NAME, table_name) : 0,
				 (schema_name != NULL) ?
				 err_generic_string(PG_DIAG_SCHEMA_NAME, schema_name) : 0));
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		PLy_exception_set_with_details(PLy_exc_error, edata);
		FreeErrorData(edata);

		return NULL;
	}
	PG_END_TRY();

	/*
	 * return a legal object so the interpreter will continue on its merry way
	 */
	Py_RETURN_NONE;
}
