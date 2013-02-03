/*
 * the PLyResult class
 *
 * src/pl/plpython/plpy_resultobject.c
 */

#include "postgres.h"

#include "plpython.h"

#include "plpy_resultobject.h"
#include "plpy_elog.h"


static void PLy_result_dealloc(PyObject *arg);
static PyObject *PLy_result_colnames(PyObject *self, PyObject *unused);
static PyObject *PLy_result_coltypes(PyObject *self, PyObject *unused);
static PyObject *PLy_result_coltypmods(PyObject *self, PyObject *unused);
static PyObject *PLy_result_nrows(PyObject *self, PyObject *args);
static PyObject *PLy_result_status(PyObject *self, PyObject *args);
static Py_ssize_t PLy_result_length(PyObject *arg);
static PyObject *PLy_result_item(PyObject *arg, Py_ssize_t idx);
static PyObject *PLy_result_slice(PyObject *arg, Py_ssize_t lidx, Py_ssize_t hidx);
static int	PLy_result_ass_slice(PyObject *arg, Py_ssize_t lidx, Py_ssize_t hidx, PyObject *slice);
static PyObject *PLy_result_str(PyObject *arg);
static PyObject *PLy_result_subscript(PyObject *arg, PyObject *item);
static int	PLy_result_ass_subscript(PyObject *self, PyObject *item, PyObject *value);

static char PLy_result_doc[] = {
	"Results of a PostgreSQL query"
};

static PySequenceMethods PLy_result_as_sequence = {
	PLy_result_length,			/* sq_length */
	NULL,						/* sq_concat */
	NULL,						/* sq_repeat */
	PLy_result_item,			/* sq_item */
	PLy_result_slice,			/* sq_slice */
	NULL,						/* sq_ass_item */
	PLy_result_ass_slice,		/* sq_ass_slice */
};

static PyMappingMethods PLy_result_as_mapping = {
	PLy_result_length,			/* mp_length */
	PLy_result_subscript,		/* mp_subscript */
	PLy_result_ass_subscript,	/* mp_ass_subscript */
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
	"PLyResult",				/* tp_name */
	sizeof(PLyResultObject),	/* tp_size */
	0,							/* tp_itemsize */

	/*
	 * methods
	 */
	PLy_result_dealloc,			/* tp_dealloc */
	0,							/* tp_print */
	0,							/* tp_getattr */
	0,							/* tp_setattr */
	0,							/* tp_compare */
	0,							/* tp_repr */
	0,							/* tp_as_number */
	&PLy_result_as_sequence,	/* tp_as_sequence */
	&PLy_result_as_mapping,		/* tp_as_mapping */
	0,							/* tp_hash */
	0,							/* tp_call */
	&PLy_result_str,			/* tp_str */
	0,							/* tp_getattro */
	0,							/* tp_setattro */
	0,							/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,	/* tp_flags */
	PLy_result_doc,				/* tp_doc */
	0,							/* tp_traverse */
	0,							/* tp_clear */
	0,							/* tp_richcompare */
	0,							/* tp_weaklistoffset */
	0,							/* tp_iter */
	0,							/* tp_iternext */
	PLy_result_methods,			/* tp_tpmethods */
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
	for (i = 0; i < ob->tupdesc->natts; i++)
		PyList_SET_ITEM(list, i, PyString_FromString(NameStr(ob->tupdesc->attrs[i]->attname)));

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
	for (i = 0; i < ob->tupdesc->natts; i++)
		PyList_SET_ITEM(list, i, PyInt_FromLong(ob->tupdesc->attrs[i]->atttypid));

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
	for (i = 0; i < ob->tupdesc->natts; i++)
		PyList_SET_ITEM(list, i, PyInt_FromLong(ob->tupdesc->attrs[i]->atttypmod));

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
PLy_result_slice(PyObject *arg, Py_ssize_t lidx, Py_ssize_t hidx)
{
	PLyResultObject *ob = (PLyResultObject *) arg;

	return PyList_GetSlice(ob->rows, lidx, hidx);
}

static int
PLy_result_ass_slice(PyObject *arg, Py_ssize_t lidx, Py_ssize_t hidx, PyObject *slice)
{
	int			rv;
	PLyResultObject *ob = (PLyResultObject *) arg;

	rv = PyList_SetSlice(ob->rows, lidx, hidx, slice);
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
