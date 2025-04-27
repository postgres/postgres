/*
 * the PLySubtransaction class
 *
 * src/pl/plpython/plpy_subxactobject.c
 */

#include "postgres.h"

#include "access/xact.h"
#include "plpy_elog.h"
#include "plpy_subxactobject.h"
#include "plpy_util.h"
#include "utils/memutils.h"

List	   *explicit_subtransactions = NIL;


static PyObject *PLy_subtransaction_enter(PyObject *self, PyObject *unused);
static PyObject *PLy_subtransaction_exit(PyObject *self, PyObject *args);

static char PLy_subtransaction_doc[] =
"PostgreSQL subtransaction context manager";

static PyMethodDef PLy_subtransaction_methods[] = {
	{"__enter__", PLy_subtransaction_enter, METH_VARARGS, NULL},
	{"__exit__", PLy_subtransaction_exit, METH_VARARGS, NULL},
	/* user-friendly names for Python <2.6 */
	{"enter", PLy_subtransaction_enter, METH_VARARGS, NULL},
	{"exit", PLy_subtransaction_exit, METH_VARARGS, NULL},
	{NULL, NULL, 0, NULL}
};

static PyType_Slot PLySubtransaction_slots[] =
{
	{
		Py_tp_doc, (char *) PLy_subtransaction_doc
	},
	{
		Py_tp_methods, PLy_subtransaction_methods
	},
	{
		0, NULL
	}
};

static PyType_Spec PLySubtransaction_spec =
{
	.name = "PLySubtransaction",
		.basicsize = sizeof(PLySubtransactionObject),
		.flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
		.slots = PLySubtransaction_slots,
};

static PyTypeObject *PLy_SubtransactionType;


void
PLy_subtransaction_init_type(void)
{
	PLy_SubtransactionType = (PyTypeObject *) PyType_FromSpec(&PLySubtransaction_spec);
	if (!PLy_SubtransactionType)
		elog(ERROR, "could not initialize PLy_SubtransactionType");
}

/* s = plpy.subtransaction() */
PyObject *
PLy_subtransaction_new(PyObject *self, PyObject *unused)
{
	PLySubtransactionObject *ob;

	ob = PyObject_New(PLySubtransactionObject, PLy_SubtransactionType);
	if (ob == NULL)
		return NULL;
#if PY_VERSION_HEX < 0x03080000
	/* Workaround for Python issue 35810; no longer necessary in Python 3.8 */
	Py_INCREF(PLy_SubtransactionType);
#endif

	ob->started = false;
	ob->exited = false;

	return (PyObject *) ob;
}

/*
 * subxact.__enter__() or subxact.enter()
 *
 * Start an explicit subtransaction.  SPI calls within an explicit
 * subtransaction will not start another one, so you can atomically
 * execute many SPI calls and still get a controllable exception if
 * one of them fails.
 */
static PyObject *
PLy_subtransaction_enter(PyObject *self, PyObject *unused)
{
	PLySubtransactionData *subxactdata;
	MemoryContext oldcontext;
	PLySubtransactionObject *subxact = (PLySubtransactionObject *) self;

	if (subxact->started)
	{
		PLy_exception_set(PyExc_ValueError, "this subtransaction has already been entered");
		return NULL;
	}

	if (subxact->exited)
	{
		PLy_exception_set(PyExc_ValueError, "this subtransaction has already been exited");
		return NULL;
	}

	subxact->started = true;
	oldcontext = CurrentMemoryContext;

	subxactdata = (PLySubtransactionData *)
		MemoryContextAlloc(TopTransactionContext,
						   sizeof(PLySubtransactionData));

	subxactdata->oldcontext = oldcontext;
	subxactdata->oldowner = CurrentResourceOwner;

	BeginInternalSubTransaction(NULL);

	/* Be sure that cells of explicit_subtransactions list are long-lived */
	MemoryContextSwitchTo(TopTransactionContext);
	explicit_subtransactions = lcons(subxactdata, explicit_subtransactions);

	/* Caller wants to stay in original memory context */
	MemoryContextSwitchTo(oldcontext);

	Py_INCREF(self);
	return self;
}

/*
 * subxact.__exit__(exc_type, exc, tb) or subxact.exit(exc_type, exc, tb)
 *
 * Exit an explicit subtransaction. exc_type is an exception type, exc
 * is the exception object, tb is the traceback.  If exc_type is None,
 * commit the subtransaction, if not abort it.
 *
 * The method signature is chosen to allow subtransaction objects to
 * be used as context managers as described in
 * <http://www.python.org/dev/peps/pep-0343/>.
 */
static PyObject *
PLy_subtransaction_exit(PyObject *self, PyObject *args)
{
	PyObject   *type;
	PyObject   *value;
	PyObject   *traceback;
	PLySubtransactionData *subxactdata;
	PLySubtransactionObject *subxact = (PLySubtransactionObject *) self;

	if (!PyArg_ParseTuple(args, "OOO", &type, &value, &traceback))
		return NULL;

	if (!subxact->started)
	{
		PLy_exception_set(PyExc_ValueError, "this subtransaction has not been entered");
		return NULL;
	}

	if (subxact->exited)
	{
		PLy_exception_set(PyExc_ValueError, "this subtransaction has already been exited");
		return NULL;
	}

	if (explicit_subtransactions == NIL)
	{
		PLy_exception_set(PyExc_ValueError, "there is no subtransaction to exit from");
		return NULL;
	}

	subxact->exited = true;

	if (type != Py_None)
	{
		/* Abort the inner transaction */
		RollbackAndReleaseCurrentSubTransaction();
	}
	else
	{
		ReleaseCurrentSubTransaction();
	}

	subxactdata = (PLySubtransactionData *) linitial(explicit_subtransactions);
	explicit_subtransactions = list_delete_first(explicit_subtransactions);

	MemoryContextSwitchTo(subxactdata->oldcontext);
	CurrentResourceOwner = subxactdata->oldowner;
	pfree(subxactdata);

	Py_RETURN_NONE;
}
