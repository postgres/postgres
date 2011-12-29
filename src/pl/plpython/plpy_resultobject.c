/*
 * the PLyResult class
 *
 * src/pl/plpython/plpy_resultobject.c
 */

#include "postgres.h"

#include "plpython.h"

#include "plpy_resultobject.h"


static void PLy_result_dealloc(PyObject *arg);
static PyObject *PLy_result_nrows(PyObject *self, PyObject *args);
static PyObject *PLy_result_status(PyObject *self, PyObject *args);
static Py_ssize_t PLy_result_length(PyObject *arg);
static PyObject *PLy_result_item(PyObject *arg, Py_ssize_t idx);
static PyObject *PLy_result_slice(PyObject *arg, Py_ssize_t lidx, Py_ssize_t hidx);
static int	PLy_result_ass_item(PyObject *arg, Py_ssize_t idx, PyObject *item);
static int	PLy_result_ass_slice(PyObject *rg, Py_ssize_t lidx, Py_ssize_t hidx, PyObject *slice);

static char PLy_result_doc[] = {
	"Results of a PostgreSQL query"
};

static PySequenceMethods PLy_result_as_sequence = {
	PLy_result_length,			/* sq_length */
	NULL,						/* sq_concat */
	NULL,						/* sq_repeat */
	PLy_result_item,			/* sq_item */
	PLy_result_slice,			/* sq_slice */
	PLy_result_ass_item,		/* sq_ass_item */
	PLy_result_ass_slice,		/* sq_ass_slice */
};

static PyMethodDef PLy_result_methods[] = {
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
	0,							/* tp_as_mapping */
	0,							/* tp_hash */
	0,							/* tp_call */
	0,							/* tp_str */
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

	return (PyObject *) ob;
}

static void
PLy_result_dealloc(PyObject *arg)
{
	PLyResultObject *ob = (PLyResultObject *) arg;

	Py_XDECREF(ob->nrows);
	Py_XDECREF(ob->rows);
	Py_XDECREF(ob->status);

	arg->ob_type->tp_free(arg);
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

static int
PLy_result_ass_item(PyObject *arg, Py_ssize_t idx, PyObject *item)
{
	int			rv;
	PLyResultObject *ob = (PLyResultObject *) arg;

	Py_INCREF(item);
	rv = PyList_SetItem(ob->rows, idx, item);
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
