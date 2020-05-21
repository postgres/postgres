/*
 * the PLyResult class
 *
 * src/pl/plpython/plpy_resultobject.c
 */

#include "postgres.h"

#include "plpy_elog.h"
#include "plpy_resultobject.h"
#include "plpython.h"

static void PLy_result_dealloc(PyObject *arg);
static PyObject *PLy_result_colnames(PyObject *self, PyObject *unused);
static PyObject *PLy_result_coltypes(PyObject *self, PyObject *unused);
static PyObject *PLy_result_coltypmods(PyObject *self, PyObject *unused);
static PyObject *PLy_result_nrows(PyObject *self, PyObject *args);
static PyObject *PLy_result_status(PyObject *self, PyObject *args);
static Py_ssize_t PLy_result_length(PyObject *arg);
static PyObject *PLy_result_item(PyObject *arg, Py_ssize_t idx);
static PyObject *PLy_result_str(PyObject *arg);
static PyObject *PLy_result_subscript(PyObject *arg, PyObject *item);
static int	PLy_result_ass_subscript(PyObject *self, PyObject *item, PyObject *value);

static char PLy_result_doc[] = "Results of a PostgreSQL query";

static PySequenceMethods PLy_result_as_sequence = {
	.sq_length = PLy_result_length,
	.sq_item = PLy_result_item,
};

static PyMappingMethods PLy_result_as_mapping = {
	.mp_length = PLy_result_length,
	.mp_subscript = PLy_result_subscript,
	.mp_ass_subscript = PLy_result_ass_subscript,
};

static PyMethodDef PLy_result_methods[] = {
	{"colnames", PLy_result_colnames, METH_NOARGS, NULL},
	{"coltypes", PLy_result_coltypes, METH_NOARGS, NULL},
	{"coltypmods", PLy_result_coltypmods, METH_NOARGS, NULL},
	{"nrows", PLy_result_nrows, METH_VARARGS, NULL},
	{"status", PLy_result_status, METH_VARARGS, NULL},
	{NULL, NULL, 0, NULL}
};

static PyTypeObject PLy_ResultType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "PLyResult",
	.tp_basicsize = sizeof(PLyResultObject),
	.tp_dealloc = PLy_result_dealloc,
	.tp_as_sequence = &PLy_result_as_sequence,
	.tp_as_mapping = &PLy_result_as_mapping,
	.tp_str = &PLy_result_str,
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_doc = PLy_result_doc,
	.tp_methods = PLy_result_methods,
};

void
PLy_result_init_type(void)
{
	if (PyType_Ready(&PLy_ResultType) < 0)
		elog(ERROR, "could not initialize PLy_ResultType");
}

PyObject *
PLy_result_new(void)
{
	PLyResultObject *ob;

	if ((ob = PyObject_New(PLyResultObject, &PLy_ResultType)) == NULL)
		return NULL;

	/* ob->tuples = NULL; */

	Py_INCREF(Py_None);
	ob->status = Py_None;
	ob->nrows = PyInt_FromLong(-1);
	ob->rows = PyList_New(0);
	ob->tupdesc = NULL;
	if (!ob->rows)
	{
		Py_DECREF(ob);
		return NULL;
	}

	return (PyObject *) ob;
}

static void
PLy_result_dealloc(PyObject *arg)
{
	PLyResultObject *ob = (PLyResultObject *) arg;

	Py_XDECREF(ob->nrows);
	Py_XDECREF(ob->rows);
	Py_XDECREF(ob->status);
	if (ob->tupdesc)
	{
		FreeTupleDesc(ob->tupdesc);
		ob->tupdesc = NULL;
	}

	arg->ob_type->tp_free(arg);
}

static PyObject *
PLy_result_colnames(PyObject *self, PyObject *unused)
{
	PLyResultObject *ob = (PLyResultObject *) self;
	PyObject   *list;
	int			i;

	if (!ob->tupdesc)
	{
		PLy_exception_set(PLy_exc_error, "command did not produce a result set");
		return NULL;
	}

	list = PyList_New(ob->tupdesc->natts);
	if (!list)
		return NULL;
	for (i = 0; i < ob->tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(ob->tupdesc, i);

		PyList_SET_ITEM(list, i, PyString_FromString(NameStr(attr->attname)));
	}

	return list;
}

static PyObject *
PLy_result_coltypes(PyObject *self, PyObject *unused)
{
	PLyResultObject *ob = (PLyResultObject *) self;
	PyObject   *list;
	int			i;

	if (!ob->tupdesc)
	{
		PLy_exception_set(PLy_exc_error, "command did not produce a result set");
		return NULL;
	}

	list = PyList_New(ob->tupdesc->natts);
	if (!list)
		return NULL;
	for (i = 0; i < ob->tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(ob->tupdesc, i);

		PyList_SET_ITEM(list, i, PyInt_FromLong(attr->atttypid));
	}

	return list;
}

static PyObject *
PLy_result_coltypmods(PyObject *self, PyObject *unused)
{
	PLyResultObject *ob = (PLyResultObject *) self;
	PyObject   *list;
	int			i;

	if (!ob->tupdesc)
	{
		PLy_exception_set(PLy_exc_error, "command did not produce a result set");
		return NULL;
	}

	list = PyList_New(ob->tupdesc->natts);
	if (!list)
		return NULL;
	for (i = 0; i < ob->tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(ob->tupdesc, i);

		PyList_SET_ITEM(list, i, PyInt_FromLong(attr->atttypmod));
	}

	return list;
}

static PyObject *
PLy_result_nrows(PyObject *self, PyObject *args)
{
	PLyResultObject *ob = (PLyResultObject *) self;

	Py_INCREF(ob->nrows);
	return ob->nrows;
}

static PyObject *
PLy_result_status(PyObject *self, PyObject *args)
{
	PLyResultObject *ob = (PLyResultObject *) self;

	Py_INCREF(ob->status);
	return ob->status;
}

static Py_ssize_t
PLy_result_length(PyObject *arg)
{
	PLyResultObject *ob = (PLyResultObject *) arg;

	return PyList_Size(ob->rows);
}

static PyObject *
PLy_result_item(PyObject *arg, Py_ssize_t idx)
{
	PyObject   *rv;
	PLyResultObject *ob = (PLyResultObject *) arg;

	rv = PyList_GetItem(ob->rows, idx);
	if (rv != NULL)
		Py_INCREF(rv);
	return rv;
}

static PyObject *
PLy_result_str(PyObject *arg)
{
	PLyResultObject *ob = (PLyResultObject *) arg;

#if PY_MAJOR_VERSION >= 3
	return PyUnicode_FromFormat("<%s status=%S nrows=%S rows=%S>",
								Py_TYPE(ob)->tp_name,
								ob->status,
								ob->nrows,
								ob->rows);
#else
	return PyString_FromFormat("<%s status=%ld nrows=%ld rows=%s>",
							   ob->ob_type->tp_name,
							   PyInt_AsLong(ob->status),
							   PyInt_AsLong(ob->nrows),
							   PyString_AsString(PyObject_Str(ob->rows)));
#endif
}

static PyObject *
PLy_result_subscript(PyObject *arg, PyObject *item)
{
	PLyResultObject *ob = (PLyResultObject *) arg;

	return PyObject_GetItem(ob->rows, item);
}

static int
PLy_result_ass_subscript(PyObject *arg, PyObject *item, PyObject *value)
{
	PLyResultObject *ob = (PLyResultObject *) arg;

	return PyObject_SetItem(ob->rows, item, value);
}
