/*
 * the plpy module
 *
 * src/pl/plpython/plpy_plpymodule.c
 */

#include "postgres.h"

#include "mb/pg_wchar.h"
#include "utils/builtins.h"

#include "plpython.h"

#include "plpy_plpymodule.h"

#include "plpy_cursorobject.h"
#include "plpy_elog.h"
#include "plpy_planobject.h"
#include "plpy_resultobject.h"
#include "plpy_spi.h"
#include "plpy_subxactobject.h"


HTAB	   *PLy_spi_exceptions = NULL;


static void PLy_add_exceptions(PyObject *plpy);
static PyObject *PLy_create_exception(char *name,
					 PyObject *base, PyObject *dict,
					 const char *modname, PyObject *mod);
static void PLy_generate_spi_exceptions(PyObject *mod, PyObject *base);

/* module functions */
static PyObject *PLy_debug(PyObject *self, PyObject *args);
static PyObject *PLy_log(PyObject *self, PyObject *args);
static PyObject *PLy_info(PyObject *self, PyObject *args);
static PyObject *PLy_notice(PyObject *self, PyObject *args);
static PyObject *PLy_warning(PyObject *self, PyObject *args);
static PyObject *PLy_error(PyObject *self, PyObject *args);
static PyObject *PLy_fatal(PyObject *self, PyObject *args);
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
	{"debug", PLy_debug, METH_VARARGS, NULL},
	{"log", PLy_log, METH_VARARGS, NULL},
	{"info", PLy_info, METH_VARARGS, NULL},
	{"notice", PLy_notice, METH_VARARGS, NULL},
	{"warning", PLy_warning, METH_VARARGS, NULL},
	{"error", PLy_error, METH_VARARGS, NULL},
	{"fatal", PLy_fatal, METH_VARARGS, NULL},

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

	{NULL, NULL, 0, NULL}
};

static PyMethodDef PLy_exc_methods[] = {
	{NULL, NULL, 0, NULL}
};

#if PY_MAJOR_VERSION >= 3
static PyModuleDef PLy_module = {
	PyModuleDef_HEAD_INIT,		/* m_base */
	"plpy",						/* m_name */
	NULL,						/* m_doc */
	-1,							/* m_size */
	PLy_methods,				/* m_methods */
};

static PyModuleDef PLy_exc_module = {
	PyModuleDef_HEAD_INIT,		/* m_base */
	"spiexceptions",			/* m_name */
	NULL,						/* m_doc */
	-1,							/* m_size */
	PLy_exc_methods,			/* m_methods */
	NULL,						/* m_reload */
	NULL,						/* m_traverse */
	NULL,						/* m_clear */
	NULL						/* m_free */
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
#endif   /* PY_MAJOR_VERSION >= 3 */

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
	hash_ctl.hash = tag_hash;
	PLy_spi_exceptions = hash_create("PL/Python SPI exceptions", 256,
									 &hash_ctl, HASH_ELEM | HASH_FUNCTION);

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
		PLy_elog(ERROR, "could not create exception \"%s\"", name);

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
			PLy_elog(ERROR, "could not generate SPI exceptions");

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
static PyObject *PLy_output(volatile int, PyObject *, PyObject *);

static PyObject *
PLy_debug(PyObject *self, PyObject *args)
{
	return PLy_output(DEBUG2, self, args);
}

static PyObject *
PLy_log(PyObject *self, PyObject *args)
{
	return PLy_output(LOG, self, args);
}

static PyObject *
PLy_info(PyObject *self, PyObject *args)
{
	return PLy_output(INFO, self, args);
}

static PyObject *
PLy_notice(PyObject *self, PyObject *args)
{
	return PLy_output(NOTICE, self, args);
}

static PyObject *
PLy_warning(PyObject *self, PyObject *args)
{
	return PLy_output(WARNING, self, args);
}

static PyObject *
PLy_error(PyObject *self, PyObject *args)
{
	return PLy_output(ERROR, self, args);
}

static PyObject *
PLy_fatal(PyObject *self, PyObject *args)
{
	return PLy_output(FATAL, self, args);
}

static PyObject *
PLy_quote_literal(PyObject *self, PyObject *args)
{
	const char *str;
	char	   *quoted;
	PyObject   *ret;

	if (!PyArg_ParseTuple(args, "s", &str))
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

	if (!PyArg_ParseTuple(args, "z", &str))
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

	if (!PyArg_ParseTuple(args, "s", &str))
		return NULL;

	quoted = quote_identifier(str);
	ret = PyString_FromString(quoted);

	return ret;
}

static PyObject *
PLy_output(volatile int level, PyObject *self, PyObject *args)
{
	PyObject   *volatile so;
	char	   *volatile sv;
	volatile MemoryContext oldcontext;

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
	if (so == NULL || ((sv = PyString_AsString(so)) == NULL))
	{
		level = ERROR;
		sv = dgettext(TEXTDOMAIN, "could not parse error message in plpy.elog");
	}

	oldcontext = CurrentMemoryContext;
	PG_TRY();
	{
		pg_verifymbstr(sv, strlen(sv), false);
		elog(level, "%s", sv);
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();

		/*
		 * Note: If sv came from PyString_AsString(), it points into storage
		 * owned by so.  So free so after using sv.
		 */
		Py_XDECREF(so);

		/* Make Python raise the exception */
		PLy_exception_set(PLy_exc_error, "%s", edata->message);
		return NULL;
	}
	PG_END_TRY();

	Py_XDECREF(so);

	/*
	 * return a legal object so the interpreter will continue on its merry way
	 */
	Py_INCREF(Py_None);
	return Py_None;
}
