/*
 * the PLyResult class
 *
 * src/pl/plpython/plpy_resultobject.c
 */

#include "postgres.h"

#include "plpy_elog.h"
#include "plpy_resultobject.h"
#include "plpy_util.h"

static void PLy_result_dealloc(PLyResultObject *self);
static PyObject *PLy_result_colnames(PyObject *self, PyObject *unused);
static PyObject *PLy_result_coltypes(PyObject *self, PyObject *unused);
static PyObject *PLy_result_coltypmods(PyObject *self, PyObject *unused);
static PyObject *PLy_result_nrows(PyObject *self, PyObject *args);
static PyObject *PLy_result_status(PyObject *self, PyObject *args);
static Py_ssize_t PLy_result_length(PyObject *arg);
static PyObject *PLy_result_item(PyObject *arg, Py_ssize_t idx);
static PyObject *PLy_result_str(PyObject *arg);
static PyObject *PLy_result_subscript(PyObject *arg, PyObject *item);
static int	PLy_result_ass_subscript(PyObject *arg, PyObject *item, PyObject *value);

static char PLy_result_doc[] = "Results of a PostgreSQL query";

static PyMethodDef PLy_result_methods[] = {
	{"colnames", PLy_result_colnames, METH_NOARGS, NULL},
	{"coltypes", PLy_result_coltypes, METH_NOARGS, NULL},
	{"coltypmods", PLy_result_coltypmods, METH_NOARGS, NULL},
	{"nrows", PLy_result_nrows, METH_VARARGS, NULL},
	{"status", PLy_result_status, METH_VARARGS, NULL},
	{NULL, NULL, 0, NULL}
};

static PyType_Slot PLyResult_slots[] =
{
	{
		Py_tp_dealloc, PLy_result_dealloc
	},
	{
		Py_sq_length, PLy_result_length
	},
	{
		Py_sq_item, PLy_result_item
	},
	{
		Py_mp_length, PLy_result_length
	},
	{
		Py_mp_subscript, PLy_result_subscript
	},
	{
		Py_mp_ass_subscript, PLy_result_ass_subscript
	},
	{
		Py_tp_str, PLy_result_str
	},
	{
		Py_tp_doc, (char *) PLy_result_doc
	},
	{
		Py_tp_methods, PLy_result_methods
	},
	{
		0, NULL
	}
};

static PyType_Spec PLyResult_spec =
{
	.name = "PLyResult",
	.basicsize = sizeof(PLyResultObject),
	.flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.slots = PLyResult_slots,
};

static PyTypeObject *PLy_ResultType;

void
PLy_result_init_type(void)
{
	PLy_ResultType = (PyTypeObject *) PyType_FromSpec(&PLyResult_spec);
	if (!PLy_ResultType)
		elog(ERROR, "could not initialize PLy_ResultType");
}

PyObject *
PLy_result_new(void)
{
	PLyResultObject *ob;

	if ((ob = PyObject_New(PLyResultObject, PLy_ResultType)) == NULL)
		return NULL;
#if PY_VERSION_HEX < 0x03080000
	/* Workaround for Python issue 35810; no longer necessary in Python 3.8 */
	Py_INCREF(PLy_ResultType);
#endif

	/* ob->tuples = NULL; */

	Py_INCREF(Py_None);
	ob->status = Py_None;
	ob->nrows = PyLong_FromLong(-1);
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
PLy_result_dealloc(PLyResultObject *self)
{
#if PY_VERSION_HEX >= 0x03080000
	PyTypeObject *tp = Py_TYPE(self);
#endif

	Py_XDECREF(self->nrows);
	Py_XDECREF(self->rows);
	Py_XDECREF(self->status);
	if (self->tupdesc)
	{
		FreeTupleDesc(self->tupdesc);
		self->tupdesc = NULL;
	}

	PyObject_Free(self);
#if PY_VERSION_HEX >= 0x03080000
	/* This was not needed before Python 3.8 (Python issue 35810) */
	Py_DECREF(tp);
#endif
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

		PyList_SetItem(list, i, PLyUnicode_FromString(NameStr(attr->attname)));
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

		PyList_SetItem(list, i, PyLong_FromLong(attr->atttypid));
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

		PyList_SetItem(list, i, PyLong_FromLong(attr->atttypmod));
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

	return PyUnicode_FromFormat("<%s status=%S nrows=%S rows=%S>",
								"PLyResult",
								ob->status,
								ob->nrows,
								ob->rows);
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
