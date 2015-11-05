/*
 * the PLyPlan class
 *
 * src/pl/plpython/plpy_planobject.c
 */

#include "postgres.h"

#include "plpython.h"

#include "plpy_planobject.h"

#include "plpy_elog.h"
#include "utils/memutils.h"


static void PLy_plan_dealloc(PyObject *arg);
static PyObject *PLy_plan_status(PyObject *self, PyObject *args);

static char PLy_plan_doc[] = {
	"Store a PostgreSQL plan"
};

static PyMethodDef PLy_plan_methods[] = {
	{"status", PLy_plan_status, METH_VARARGS, NULL},
	{NULL, NULL, 0, NULL}
};

static PyTypeObject PLy_PlanType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	"PLyPlan",					/* tp_name */
	sizeof(PLyPlanObject),		/* tp_size */
	0,							/* tp_itemsize */

	/*
	 * methods
	 */
	PLy_plan_dealloc,			/* tp_dealloc */
	0,							/* tp_print */
	0,							/* tp_getattr */
	0,							/* tp_setattr */
	0,							/* tp_compare */
	0,							/* tp_repr */
	0,							/* tp_as_number */
	0,							/* tp_as_sequence */
	0,							/* tp_as_mapping */
	0,							/* tp_hash */
	0,							/* tp_call */
	0,							/* tp_str */
	0,							/* tp_getattro */
	0,							/* tp_setattro */
	0,							/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,	/* tp_flags */
	PLy_plan_doc,				/* tp_doc */
	0,							/* tp_traverse */
	0,							/* tp_clear */
	0,							/* tp_richcompare */
	0,							/* tp_weaklistoffset */
	0,							/* tp_iter */
	0,							/* tp_iternext */
	PLy_plan_methods,			/* tp_tpmethods */
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
PLy_plan_status(PyObject *self, PyObject *args)
{
	if (PyArg_ParseTuple(args, ""))
	{
		Py_INCREF(Py_True);
		return Py_True;
		/* return PyInt_FromLong(self->status); */
	}
	PLy_exception_set(PLy_exc_error, "plan.status takes no arguments");
	return NULL;
}
