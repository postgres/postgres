/*
 * the PLyPlan class
 *
 * src/pl/plpython/plpy_planobject.c
 */

#include "postgres.h"

#include "plpy_cursorobject.h"
#include "plpy_elog.h"
#include "plpy_planobject.h"
#include "plpy_spi.h"
#include "plpython.h"
#include "utils/memutils.h"

static void PLy_plan_dealloc(PyObject *arg);
static PyObject *PLy_plan_cursor(PyObject *self, PyObject *args);
static PyObject *PLy_plan_execute(PyObject *self, PyObject *args);
static PyObject *PLy_plan_status(PyObject *self, PyObject *args);

static char PLy_plan_doc[] = "Store a PostgreSQL plan";

static PyMethodDef PLy_plan_methods[] = {
	{"cursor", PLy_plan_cursor, METH_VARARGS, NULL},
	{"execute", PLy_plan_execute, METH_VARARGS, NULL},
	{"status", PLy_plan_status, METH_VARARGS, NULL},
	{NULL, NULL, 0, NULL}
};

static PyTypeObject PLy_PlanType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = "PLyPlan",
	.tp_basicsize = sizeof(PLyPlanObject),
	.tp_dealloc = PLy_plan_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_doc = PLy_plan_doc,
	.tp_methods = PLy_plan_methods,
};

void
PLy_plan_init_type(void)
{
	if (PyType_Ready(&PLy_PlanType) < 0)
		elog(ERROR, "could not initialize PLy_PlanType");
}

PyObject *
PLy_plan_new(void)
{
	PLyPlanObject *ob;

	if ((ob = PyObject_New(PLyPlanObject, &PLy_PlanType)) == NULL)
		return NULL;

	ob->plan = NULL;
	ob->nargs = 0;
	ob->types = NULL;
	ob->values = NULL;
	ob->args = NULL;
	ob->mcxt = NULL;

	return (PyObject *) ob;
}

bool
is_PLyPlanObject(PyObject *ob)
{
	return ob->ob_type == &PLy_PlanType;
}

static void
PLy_plan_dealloc(PyObject *arg)
{
	PLyPlanObject *ob = (PLyPlanObject *) arg;

	if (ob->plan)
	{
		SPI_freeplan(ob->plan);
		ob->plan = NULL;
	}
	if (ob->mcxt)
	{
		MemoryContextDelete(ob->mcxt);
		ob->mcxt = NULL;
	}
	arg->ob_type->tp_free(arg);
}


static PyObject *
PLy_plan_cursor(PyObject *self, PyObject *args)
{
	PyObject   *planargs = NULL;

	if (!PyArg_ParseTuple(args, "|O", &planargs))
		return NULL;

	return PLy_cursor_plan(self, planargs);
}


static PyObject *
PLy_plan_execute(PyObject *self, PyObject *args)
{
	PyObject   *list = NULL;
	long		limit = 0;

	if (!PyArg_ParseTuple(args, "|Ol", &list, &limit))
		return NULL;

	return PLy_spi_execute_plan(self, list, limit);
}


static PyObject *
PLy_plan_status(PyObject *self, PyObject *args)
{
	if (PyArg_ParseTuple(args, ":status"))
	{
		Py_INCREF(Py_True);
		return Py_True;
		/* return PyInt_FromLong(self->status); */
	}
	return NULL;
}
