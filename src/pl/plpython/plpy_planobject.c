/*
 * the PLyPlan class
 *
 * src/pl/plpython/plpy_planobject.c
 */

#include "postgres.h"

#include "plpy_cursorobject.h"
#include "plpy_planobject.h"
#include "plpy_spi.h"
#include "plpython.h"
#include "utils/memutils.h"

static void PLy_plan_dealloc(PLyPlanObject *self);
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

static PyType_Slot PLyPlan_slots[] =
{
	{
		Py_tp_dealloc, PLy_plan_dealloc
	},
	{
		Py_tp_doc, (char *) PLy_plan_doc
	},
	{
		Py_tp_methods, PLy_plan_methods
	},
	{
		0, NULL
	}
};

static PyType_Spec PLyPlan_spec =
{
	.name = "PLyPlan",
		.basicsize = sizeof(PLyPlanObject),
		.flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
		.slots = PLyPlan_slots,
};

static PyTypeObject *PLy_PlanType;

void
PLy_plan_init_type(void)
{
	PLy_PlanType = (PyTypeObject *) PyType_FromSpec(&PLyPlan_spec);
	if (!PLy_PlanType)
		elog(ERROR, "could not initialize PLy_PlanType");
}

PyObject *
PLy_plan_new(void)
{
	PLyPlanObject *ob;

	if ((ob = PyObject_New(PLyPlanObject, PLy_PlanType)) == NULL)
		return NULL;
#if PY_VERSION_HEX < 0x03080000
	/* Workaround for Python issue 35810; no longer necessary in Python 3.8 */
	Py_INCREF(PLy_PlanType);
#endif

	ob->plan = NULL;
	ob->nargs = 0;
	ob->types = NULL;
	ob->args = NULL;
	ob->mcxt = NULL;

	return (PyObject *) ob;
}

bool
is_PLyPlanObject(PyObject *ob)
{
	return ob->ob_type == PLy_PlanType;
}

static void
PLy_plan_dealloc(PLyPlanObject *self)
{
#if PY_VERSION_HEX >= 0x03080000
	PyTypeObject *tp = Py_TYPE(self);
#endif

	if (self->plan)
	{
		SPI_freeplan(self->plan);
		self->plan = NULL;
	}
	if (self->mcxt)
	{
		MemoryContextDelete(self->mcxt);
		self->mcxt = NULL;
	}

	PyObject_Free(self);
#if PY_VERSION_HEX >= 0x03080000
	/* This was not needed before Python 3.8 (Python issue 35810) */
	Py_DECREF(tp);
#endif
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
		/* return PyLong_FromLong(self->status); */
	}
	return NULL;
}
